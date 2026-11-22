/* br_websocket.h - the `websocket` add-on: an RFC 6455 client (ws:// and wss://) over `tls`.
 *
 *   br_websocket_register(e);                                  // once, in init (registers http, tls, net)
 *   g->ws = br_websocket_open(e, "wss://example.com/live", NULL, NULL);   // returns at once: CONNECTING
 *   ...each frame, in a system:
 *   if (br_websocket_socket_state(e, g->ws) == BR_WEBSOCKET_OPEN) {
 *       br_websocket_send_text(e, g->ws, "{\"move\":3}");
 *   }
 *   br_websocket_message m;
 *   while (br_websocket_receive(e, g->ws, &m)) { use(m.data, m.size, m.text); }   // also after CLOSED
 *   if (state is CLOSED or FAILED) { br_websocket_close_code(e, g->ws), br_websocket_error(e, g->ws);
 *                                    br_websocket_release(e, g->ws); reconnect later }
 *
 * The opening handshake is ours (on http's request writer and response parser); frames, masking,
 * fragments, control frames, ping -> pong and the closing handshake are wslay's (vendored in
 * addons/websocket/third_party/wslay/). The system `websocket.update` (INPUT phase,
 * BR_WEBSOCKET_UPDATE_PRIORITY: right after `http.update`) reads, writes, pings and times out.
 * Everything the server sends is untrusted. Handles are {index, generation}; zero and stale
 * handles are safe everywhere (CLOSED, false, 0, ""). README: addons/websocket/README.md.
 */
#ifndef BR_WEBSOCKET_H
#define BR_WEBSOCKET_H

#include "braty/br_http.h"

/* Sockets open at once (any state until released). One more: zero handle + log. */
#define BR_WEBSOCKET_MAX 8
/* Largest message accepted when br_websocket_set_max_message was not called (bytes). */
#define BR_WEBSOCKET_DEFAULT_MAX_MESSAGE (16 * 1024 * 1024)
/* Unread received bytes a socket keeps; more fails the connection (close code 1008). */
#define BR_WEBSOCKET_MAX_UNREAD (64 * 1024 * 1024)
/* Bytes waiting to go out to a server that does not read. A send that would pass it returns
 * false; pongs to a ping flood that pass it drop the connection (CLOSED, 1006). */
#define BR_WEBSOCKET_MAX_UNSENT (16 * 1024 * 1024)
/* Keep-alive ping interval when br_websocket_set_ping_interval was not called (seconds). */
#define BR_WEBSOCKET_DEFAULT_PING_INTERVAL 30.0f
/* Real-time seconds from br_websocket_open to the end of the opening handshake. */
#define BR_WEBSOCKET_HANDSHAKE_TIMEOUT 30.0f
/* Real-time seconds the closing handshake waits for the peer's close frame. */
#define BR_WEBSOCKET_CLOSE_TIMEOUT 5.0f
/* Priority of the `websocket.update` system in BR_PHASE_INPUT (right after `http.update`). */
#define BR_WEBSOCKET_UPDATE_PRIORITY (BR_HTTP_UPDATE_PRIORITY + 1)

/* A WebSocket connection. */
typedef struct br_websocket {
    uint32_t index;
    uint32_t generation;
} br_websocket;

typedef enum br_websocket_state {
    BR_WEBSOCKET_CLOSED,      /* ended (clean, by you, dropped, dead), or zero / stale handle */
    BR_WEBSOCKET_CONNECTING,  /* connecting, TLS, opening handshake */
    BR_WEBSOCKET_OPEN,        /* messages flow both ways */
    BR_WEBSOCKET_CLOSING,     /* br_websocket_close sent our close frame; waiting for the reply */
    BR_WEBSOCKET_FAILED       /* the handshake failed, or the server broke the protocol */
} br_websocket_state;

/* One received message. */
typedef struct br_websocket_message {
    const void *data;   /* NUL-terminated (the NUL is not in size) */
    size_t      size;
    bool        text;   /* a text message (valid UTF-8), else binary */
} br_websocket_message;

/* Registers `http` (and with it `tls`, `net`) and adds the `websocket.update` system. Idempotent;
 * false (logged) when http cannot start. */
bool br_websocket_register(br_engine *e);
/* ws:// or wss://; protocols = "chat, v2" or NULL; headers as br_http_desc.headers or NULL (not
 * Upgrade, Connection or Sec-WebSocket-*: those are ours). Returns at once in CONNECTING. Zero
 * handle (logged) when not registered, the table is full or an argument is bad. */
br_websocket br_websocket_open(br_engine *e, const char *url, const char *protocols, const char *headers);
/* CONNECTING, OPEN, CLOSING, CLOSED or FAILED; CLOSED for zero / stale handles. */
br_websocket_state br_websocket_socket_state(br_engine *e, br_websocket ws);
/* The subprotocol the server chose from the offered `protocols` ("" when it chose none, before
 * the handshake completed, and for zero / stale handles); valid until release. */
const char *br_websocket_protocol(br_engine *e, br_websocket ws);
/* Sends a text message (NUL-terminated, valid UTF-8). False (logged) unless OPEN, when the
 * text is not valid UTF-8, or when it would put more than BR_WEBSOCKET_MAX_UNSENT bytes in the
 * queue to a server that is not reading. */
bool br_websocket_send_text(br_engine *e, br_websocket ws, const char *text);
/* Sends a binary message (size 0 is fine). False as br_websocket_send_text. */
bool br_websocket_send_binary(br_engine *e, br_websocket ws, const void *data, size_t size);
/* The next whole message, false when none waits. data is NUL-terminated and valid until the next
 * br_websocket_receive on this socket. Messages that arrived before a close stay readable while
 * the socket is CLOSED or FAILED, until release. */
bool br_websocket_receive(br_engine *e, br_websocket ws, br_websocket_message *out);
/* Largest message accepted from now on (0 = BR_WEBSOCKET_DEFAULT_MAX_MESSAGE); a bigger one fails
 * the connection with close code 1009. */
void br_websocket_set_max_message(br_engine *e, br_websocket ws, size_t bytes);
/* Keep-alive: a ping after `seconds` (real time) without anything received; nothing received
 * within another `seconds` drops the socket as CLOSED with code 1006. 0 = off; default 30. */
void br_websocket_set_ping_interval(br_engine *e, br_websocket ws, float seconds);
/* Starts the closing handshake (code 1000 = normal; 1000-1003, 1007-1014 or 3000-4999, anything
 * else sends 1000; reason up to 123 bytes of UTF-8 or NULL): OPEN -> CLOSING -> CLOSED.
 * CONNECTING -> CLOSED at once. The slot is freed by br_websocket_release. */
void br_websocket_close(br_engine *e, br_websocket ws, uint16_t code, const char *reason);
/* The code of the peer's close frame (1005 when it had none); 1006 when the connection ended
 * without one (dropped, dead, timed out, failed); 0 while CONNECTING, OPEN or CLOSING. */
uint16_t br_websocket_close_code(br_engine *e, br_websocket ws);
/* Why it FAILED, or why it ended without a clean close ("" otherwise); valid until release. */
const char *br_websocket_error(br_engine *e, br_websocket ws);
/* Frees the slot and its unread messages. An OPEN connection gets a close frame 1001 (going away)
 * without waiting for the reply; call br_websocket_close first for a clean end. */
void br_websocket_release(br_engine *e, br_websocket ws);

#endif
