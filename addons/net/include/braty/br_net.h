/* br_net.h - the `net` add-on: TCP client and server sockets, UDP and addresses on SDL3_net.
 *
 *   br_net_register(e);                                   // once, in init
 *   g->sock = br_net_connect(e, "example.com", 7777);     // returns at once: CONNECTING
 *   ...each frame, in a system:
 *   if (br_net_socket_state(e, g->sock) == BR_NET_OPEN) {
 *       br_net_send(e, g->sock, "hello", 5);              // queued, sent as the socket allows
 *       char buf[4096];
 *       int n;
 *       while ((n = br_net_receive(e, g->sock, buf, sizeof buf)) > 0) { use n bytes of buf }
 *       if (n < 0) { the peer closed: br_net_close(e, g->sock) }
 *   }
 *
 * Everything is non-blocking and main-thread: start something, keep its handle, poll its state.
 * The system `net.update` (INPUT phase, priority BR_NET_UPDATE_PRIORITY) resolves and connects
 * CONNECTING sockets, flushes queued sends and finishes closed sockets' sends. Handles are
 * {index, generation}; a zeroed handle means none, and zero or stale handles are safe to pass
 * to every function (they read as CLOSED / nothing). Bytes from the network are untrusted.
 */
#ifndef BR_NET_H
#define BR_NET_H

#include "braty/br_core.h"

/* TCP sockets open at once (connecting, connected or accepted). One more: zero handle + log. */
#define BR_NET_MAX_SOCKETS 64
/* Listening TCP servers at once. */
#define BR_NET_MAX_SERVERS 8
/* UDP sockets at once. */
#define BR_NET_MAX_UDP 8
/* Address handles at once (br_net_resolve and senders reported by br_net_udp_receive). */
#define BR_NET_MAX_ADDRESSES 32
/* Largest UDP payload SDL3_net sends or receives. */
#define BR_NET_MAX_DATAGRAM (64 * 1024)
/* Real-time seconds br_net_close keeps sending a socket's queued bytes before dropping them. */
#define BR_NET_CLOSE_LINGER 5.0f
/* Priority of the `net.update` system in BR_PHASE_INPUT (early: before game input systems). */
#define BR_NET_UPDATE_PRIORITY (-1500000)

/* A TCP connection. */
typedef struct br_net_socket {
    uint32_t index;
    uint32_t generation;
} br_net_socket;

/* A listening TCP server. */
typedef struct br_net_server {
    uint32_t index;
    uint32_t generation;
} br_net_server;

/* A bound UDP socket. */
typedef struct br_net_udp {
    uint32_t index;
    uint32_t generation;
} br_net_udp;

/* A host address (IPv4 or IPv6, no port): resolved from a name or reported as a UDP sender. */
typedef struct br_net_address {
    uint32_t index;
    uint32_t generation;
} br_net_address;

/* Socket and address states. For an address CONNECTING = resolving, OPEN = resolved. */
typedef enum br_net_state {
    BR_NET_CLOSED,      /* zero / stale handle, or the peer closed (br_net_receive returned -1) */
    BR_NET_CONNECTING,  /* resolving the host name, then connecting */
    BR_NET_OPEN,        /* connected: send and receive */
    BR_NET_FAILED       /* name not found, connection refused, or a send failed */
} br_net_state;

/* Starts SDL3_net and adds the `net.update` system. Idempotent; false (logged) when SDL3_net
 * cannot start. Every other function needs it. */
bool br_net_register(br_engine *e);

/* ---- TCP */

/* Starts a connection to host:port (a name or an IP text); resolves, then connects. Zero handle
 * (logged) when the table is full or the arguments are bad. No timeout of its own (the operating
 * system's applies): a caller that wants one closes the socket after its deadline. */
br_net_socket br_net_connect(br_engine *e, const char *host, uint16_t port);
/* State of a socket; CLOSED for zero / stale handles. */
br_net_state br_net_socket_state(br_engine *e, br_net_socket s);
/* Queues all `size` bytes (copied); they go on the wire as the socket allows (also from
 * `net.update`). False unless OPEN, or when size is over INT_MAX. Never queues part. The queue
 * has no limit: a sender to a slow peer watches br_net_send_pending. */
bool br_net_send(br_engine *e, br_net_socket s, const void *data, size_t size);
/* Bytes queued by br_net_send, not yet handed to the operating system. */
size_t br_net_send_pending(br_engine *e, br_net_socket s);
/* Reads up to capacity bytes: the count, 0 when nothing is there now (also while CONNECTING),
 * -1 when the socket is CLOSED or FAILED. The read that finds the peer gone (end of stream or a
 * reset) returns -1 and sets the state to CLOSED. */
int br_net_receive(br_engine *e, br_net_socket s, void *buffer, int capacity);
/* Frees the handle at once. Bytes still queued keep being sent for up to BR_NET_CLOSE_LINGER
 * seconds (the slot stays taken meanwhile), then the connection closes. */
void br_net_close(br_engine *e, br_net_socket s);

/* Listens on port on all interfaces (IPv4 and IPv6). Zero handle (logged) on failure. */
br_net_server br_net_listen(br_engine *e, uint16_t port);
/* The next waiting connection, already OPEN; zero handle when nobody waits (or no slot: logged). */
br_net_socket br_net_accept(br_engine *e, br_net_server server);
/* Stops listening; sockets accepted from it stay open. */
void br_net_server_close(br_engine *e, br_net_server server);

/* ---- addresses */

/* Starts resolving host (a name or an IP text); poll br_net_address_state. Zero handle (logged)
 * when the table is full. Release it with br_net_address_release. */
br_net_address br_net_resolve(br_engine *e, const char *host);
/* CONNECTING while resolving, OPEN when resolved, FAILED when not found, CLOSED for zero / stale. */
br_net_state br_net_address_state(br_engine *e, br_net_address a);
/* Copies the numeric text ("127.0.0.1", "::1") into out (NUL-terminated, cut to capacity).
 * False (out = "") unless resolved. */
bool br_net_address_get_string(br_engine *e, br_net_address a, char *out, size_t capacity);
/* Frees the handle (once, however many times br_net_udp_receive returned it). */
void br_net_address_release(br_engine *e, br_net_address a);

/* ---- UDP */

/* Binds a UDP socket to port on all interfaces (0 = any free port). Zero handle (logged) on
 * failure. */
br_net_udp br_net_udp_open(br_engine *e, uint16_t port);
/* Sends one datagram (copied) to a resolved address and port. False when the address is not
 * resolved, size is 0 or over BR_NET_MAX_DATAGRAM, or the send failed (logged). */
bool br_net_udp_send(br_engine *e, br_net_udp u, br_net_address to, uint16_t port, const void *data, size_t size);
/* Takes the next waiting datagram: its size (copied into buffer), 0 when none waits (a receive
 * error is logged and reads as 0), -1 for a zero / stale handle. A datagram larger than
 * capacity is dropped and logged, and the next one is tried. `from` (may be NULL) gets the
 * sender's address: one handle per sender host (not shared with br_net_resolve handles), the
 * same handle again while it is not released; zero when the address table is full (logged
 * once). Release it when that peer is gone. `from_port` (may be NULL) gets the sender's port. */
int br_net_udp_receive(br_engine *e, br_net_udp u, void *buffer, int capacity, br_net_address *from, uint16_t *from_port);
/* Closes the socket (datagrams SDL3_net still queues are tried once more). */
void br_net_udp_close(br_engine *e, br_net_udp u);

#endif
