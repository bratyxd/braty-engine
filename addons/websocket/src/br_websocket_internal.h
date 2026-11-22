/* br_websocket_internal.h - the parts of websocket that touch no socket and no engine, for
 * test_websocket_frame and br_websocket.c:
 *
 * - the opening handshake: the request (br_websocket_write_handshake), the accept key and every
 *   check RFC 6455 section 4.1 asks of a client (br_websocket_handshake_check);
 * - br_websocket_conn: one connection after the 101, wslay's event API over two in-memory byte
 *   streams. Bytes from the peer go in with br_websocket_conn_feed / _end; bytes for the peer
 *   come out of br_websocket_conn_output. Time is passed in (SDL_GetTicksNS in the add-on, any
 *   clock in tests), so the keep-alive and the close timeout are testable without waiting.
 *
 *   br_websocket_conn c;
 *   br_websocket_conn_init(&c, random_fn, user, now);          // after a checked 101
 *   br_websocket_conn_feed(&c, bytes, n, now);                 // what the stream delivered
 *   br_websocket_conn_update(&c, now);                         // each frame: pings, timeouts
 *   size_t size; const void *out = br_websocket_conn_output(&c, &size);   // send it, then
 *   br_websocket_conn_output_clear(&c);
 *   while (br_websocket_conn_receive(&c, &m)) { ... }
 *   c.state: OPEN, CLOSING, CLOSED or FAILED; c.close_code; c.error
 *   br_websocket_conn_free(&c);
 *
 * What wslay checks and what is ours: addons/websocket/README.md "What is checked". */
#ifndef BR_WEBSOCKET_INTERNAL_H
#define BR_WEBSOCKET_INTERNAL_H

#include "braty/br_websocket.h"
#include "br_http_internal.h"

#include <stddef.h>
#include <stdint.h>

/* The GUID of RFC 6455 section 1.3, appended to the key for the accept value. */
#define BR_WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
/* Sec-WebSocket-Key: 16 random bytes as base64 (24 characters). */
#define BR_WEBSOCKET_KEY_LENGTH 24
/* Sec-WebSocket-Accept: a SHA-1 as base64 (28 characters). */
#define BR_WEBSOCKET_ACCEPT_LENGTH 28

/* ---- the opening handshake */

/* Sec-WebSocket-Key text of 16 bytes (NUL-terminated, BR_WEBSOCKET_KEY_LENGTH characters). */
void br_websocket_key_text(const uint8_t random16[16], char out[BR_WEBSOCKET_KEY_LENGTH + 1]);
/* The Sec-WebSocket-Accept a server must answer to key: base64(SHA-1(key + GUID)). */
void br_websocket_accept_key(const char *key, char out[BR_WEBSOCKET_ACCEPT_LENGTH + 1]);
/* True when protocols (NULL = none) is a comma-separated list of tokens ("chat, v2"); a reason
 * otherwise. */
bool br_websocket_protocols_valid(const char *protocols, char *error, size_t error_capacity);
/* Writes the upgrade request (GET, Host, Upgrade, Connection, Sec-WebSocket-Key / -Version /
 * -Protocol, the caller's headers) as br_http_write_request does: returns its length, the
 * capacity it needs when out is too small (out may be NULL), or 0 with a reason when the headers
 * or protocols are bad or name a header that is ours. */
size_t br_websocket_write_handshake(char *out, size_t capacity, const br_http_url *url, const char *key,
                                    const char *protocols, const char *headers, char *error,
                                    size_t error_capacity);
/* The checks of RFC 6455 section 4.1 on a complete response (parser DONE): status 101; Upgrade is
 * "websocket"; Connection has the token "Upgrade" (both case-insensitive); exactly one
 * Sec-WebSocket-Accept equal to accept; no Sec-WebSocket-Extensions (we offer none); a
 * Sec-WebSocket-Protocol only when we offered protocols, and then one of them. False with a
 * reason otherwise. */
bool br_websocket_handshake_check(const br_http_parser *p, const char *accept, const char *protocols,
                                  char *error, size_t error_capacity);
/* True when data is valid UTF-8 (no overlong forms, surrogates or code points over U+10FFFF). */
bool br_websocket_utf8_valid(const void *data, size_t size);

/* ---- a connection after the handshake */

/* Random bytes for the frame masks (br_tls_random in the add-on). False = no randomness. */
typedef bool (*br_websocket_random_fn)(void *user, void *out, size_t size);

typedef struct br_websocket_conn {
    struct wslay_event_context *ctx;
    br_websocket_state state;           /* OPEN, CLOSING, CLOSED or FAILED */
    uint16_t           close_code;      /* as br_websocket_close_code: 0 until CLOSED / FAILED */
    uint16_t           sent_code;       /* the code of the close frame we sent, 0 = none yet */
    char               error[256];
    br_websocket_random_fn random;
    void              *random_user;
    /* bytes from the peer, valid during one feed (wslay reads them through its recv callback) */
    const uint8_t     *in;
    size_t             in_size;
    bool               eof;             /* the stream ended */
    bool               violation;       /* we found a protocol error wslay does not check */
    bool               closed_by_us;    /* br_websocket_conn_close queued our close frame */
    /* bytes for the peer (wslay writes them through its send callback); grows, is reused */
    uint8_t           *out;
    size_t             out_size;
    size_t             out_capacity;
    /* received messages not yet read: [uint32 size][uint8 text][data][NUL], grows, is reused */
    uint8_t           *queue;
    size_t             queue_read;
    size_t             queue_write;
    size_t             queue_capacity;
    uint8_t           *current;         /* the message br_websocket_conn_receive returned last */
    size_t             current_capacity;
    size_t             max_message;
    /* keep-alive and close timeout, in the caller's nanoseconds */
    uint64_t           ping_interval;   /* 0 = off */
    uint64_t           last_received;
    uint64_t           ping_sent;
    bool               ping_waiting;
    uint64_t           close_timeout;
    uint64_t           close_deadline;
    bool               out_of_memory;
} br_websocket_conn;

/* A client connection in OPEN (wslay client context, our callbacks). False (logged) when out of
 * memory. random gives the masks. */
bool br_websocket_conn_init(br_websocket_conn *c, br_websocket_random_fn random, void *random_user,
                            uint64_t now);
/* Frees the wslay context and the buffers (a zeroed conn is fine). */
void br_websocket_conn_free(br_websocket_conn *c);
/* Bytes the peer sent (any split: one byte at a time works). Complete messages are queued for
 * _receive; pings are answered; protocol errors queue a close frame and FAIL the connection. */
void br_websocket_conn_feed(br_websocket_conn *c, const void *data, size_t size, uint64_t now);
/* The peer's stream ended: CLOSED (1006 unless the closing handshake was complete). */
void br_websocket_conn_end(br_websocket_conn *c, const char *reason, uint64_t now);
/* Keep-alive ping, dead-peer drop, close timeout. Call every frame. */
void br_websocket_conn_update(br_websocket_conn *c, uint64_t now);
/* Queues a message (text is not checked here). False unless OPEN. */
bool br_websocket_conn_send(br_websocket_conn *c, bool text, const void *data, size_t size);
/* Queues our close frame (code, reason of at most 123 bytes) and goes CLOSING. False unless OPEN. */
bool br_websocket_conn_close(br_websocket_conn *c, uint16_t code, const char *reason, uint64_t now);
/* The next queued message, as br_websocket_receive. */
bool br_websocket_conn_receive(br_websocket_conn *c, br_websocket_message *out);
/* Largest message from now on (0 = BR_WEBSOCKET_DEFAULT_MAX_MESSAGE). */
void br_websocket_conn_set_max_message(br_websocket_conn *c, size_t bytes);
/* Keep-alive interval and close timeout in nanoseconds (interval 0 = off); restarts the
 * keep-alive clock at now. */
void br_websocket_conn_set_times(br_websocket_conn *c, uint64_t ping_interval, uint64_t close_timeout, uint64_t now);
/* Bytes waiting to go to the peer (NULL / 0 when none). */
const void *br_websocket_conn_output(const br_websocket_conn *c, size_t *size);
/* The output was sent: empties it (keeps the buffer). */
void br_websocket_conn_output_clear(br_websocket_conn *c);

/* ---- for tests */

/* Handshake and close timeouts of one socket in seconds (0 keeps the current one). */
void br_websocket_set_timeouts(br_engine *e, br_websocket ws, float handshake, float close);

#endif
