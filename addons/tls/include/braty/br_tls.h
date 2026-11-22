/* br_tls.h - the `tls` add-on: a byte stream to host:port that is plain TCP or TLS (Mbed TLS
 * 3.6 over `net`), plus the small crypto helpers http and websocket need.
 *
 *   br_tls_register(e);                                        // once, in init (registers net)
 *   g->stream = br_tls_open(e, "example.com", 443, true);      // returns at once: CONNECTING
 *   ...each frame, in a system:
 *   switch (br_tls_state(e, g->stream)) {
 *   case BR_NET_OPEN:                                          // connected and verified
 *       br_tls_send(e, g->stream, request, request_size);
 *       while ((n = br_tls_receive(e, g->stream, buf, sizeof buf)) > 0) { use n bytes of buf }
 *       break;
 *   case BR_NET_FAILED: br_log_error("%s", br_tls_error(e, g->stream)); br_tls_close(e, g->stream); break;
 *   default: break;
 *   }
 *
 * A secure stream verifies the server's certificate chain against the root certificates (the
 * built-in Mozilla bundle, or br_tls_set_ca_pem's) and the host name, sends SNI and speaks
 * TLS 1.2 or 1.3; it reads OPEN only after that handshake succeeded. The system `tls.update`
 * (INPUT phase, priority BR_TLS_UPDATE_PRIORITY: right after `net.update`) drives handshakes.
 * Handles are {index, generation}; zero and stale handles are safe everywhere (CLOSED, -1, false).
 */
#ifndef BR_TLS_H
#define BR_TLS_H

#include "braty/br_net.h"

/* Streams open at once (plain and secure). One more: zero handle + log. */
#define BR_TLS_MAX_STREAMS 32
/* Longest host name br_tls_open takes (bytes, without the NUL). */
#define BR_TLS_MAX_HOST 255
/* Priority of the `tls.update` system in BR_PHASE_INPUT (right after `net.update`). */
#define BR_TLS_UPDATE_PRIORITY (BR_NET_UPDATE_PRIORITY + 1)

/* A plain or TLS byte stream. */
typedef struct br_tls_stream {
    uint32_t index;
    uint32_t generation;
} br_tls_stream;

/* Registers `net`, starts the random generator and adds the `tls.update` system. Idempotent;
 * false (logged) when net or the crypto cannot start. */
bool br_tls_register(br_engine *e);

/* A stream to host:port; secure = TLS 1.2+ with certificate and host-name verification and SNI.
 * Returns at once in CONNECTING. Zero handle (logged) when the table is full or the arguments
 * are bad. No timeout of its own: close it after a real-time deadline of yours. */
br_tls_stream br_tls_open(br_engine *e, const char *host, uint16_t port, bool secure);
/* CONNECTING (connecting, waiting for the root certificates, handshaking), OPEN (only after the
 * handshake verified), CLOSED (the peer closed, or zero / stale handle) or FAILED. */
br_net_state br_tls_state(br_engine *e, br_tls_stream s);
/* Encrypts (when secure) and queues all size bytes; false unless OPEN. A secure send that fails
 * turns the stream FAILED. */
bool br_tls_send(br_engine *e, br_tls_stream s, const void *data, size_t size);
/* Bytes queued on the stream's socket, not yet handed to the operating system (encrypted bytes
 * for a secure stream); 0 for zero / stale handles. Grows while the peer does not read. */
size_t br_tls_send_pending(br_engine *e, br_tls_stream s);
/* As br_net_receive: bytes read (decrypted), 0 when nothing is there now (also while
 * CONNECTING), -1 when CLOSED or FAILED. The read that finds the peer gone returns -1 and sets
 * CLOSED; a TLS error sets FAILED. */
int br_tls_receive(br_engine *e, br_tls_stream s, void *buffer, int capacity);
/* True when the stream is CLOSED by an orderly end: the peer's close_notify on a secure stream,
 * the TCP close on a plain one. False when a secure stream ended without close_notify (the data
 * may have been cut short by the network or an attacker), while open, FAILED, and for zero /
 * stale handles. Check it when the end of the stream is the end of the data. */
bool br_tls_closed_cleanly(br_engine *e, br_tls_stream s);
/* Frees the handle; a secure OPEN stream sends close_notify first. Queued bytes still go out
 * (br_net_close's linger). */
void br_tls_close(br_engine *e, br_tls_stream s);
/* Why the stream FAILED ("" otherwise, and for zero / stale handles); valid until close. */
const char *br_tls_error(br_engine *e, br_tls_stream s);

/* Replaces the built-in root certificates (PEM text is copied): your own server's CA, pinning.
 * size is the text's length (a trailing NUL may be included or not). False (logged, nothing
 * changed) when no certificate parses. Streams opened later and those still handshaking use it. */
bool br_tls_set_ca_pem(br_engine *e, const char *pem, size_t size);
/* Verification on (default) or off; off logs a warning on every secure open. Development only.
 * A stream keeps the setting it was opened with. */
void br_tls_set_verify(br_engine *e, bool on);

/* Cryptographic random bytes (Mbed TLS CTR-DRBG). False (logged) when not registered. */
bool br_tls_random(br_engine *e, void *out, size_t size);
/* SHA-1 of data (WebSocket handshake; not for security). */
void br_tls_sha1(const void *data, size_t size, uint8_t out[20]);
/* Base64 text of data, NUL-terminated; returns its length, 0 when capacity is too small. */
size_t br_tls_base64_encode(const void *data, size_t size, char *out, size_t capacity);

#endif
