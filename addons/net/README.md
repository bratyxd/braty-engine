# net - TCP and UDP sockets (add-on)

Header: `addons/net/include/braty/br_net.h`. Source: `addons/net/src/br_net.c`.
Test: `addons/net/tests/test_net.c` (loopback, ports 47801..47804).

An add-on: built only when `BRATY_ADDONS` names it (or an add-on that depends on it:
`tls`, `http`, `websocket`). Configure with `-DBRATY_ADDONS=net` (or `all`), link `braty::net`
(SDL3_net comes with it; nobody names it), call `br_net_register(e)` in init. Runs on SDL3_net
`release-3.2.0` (an installed `SDL3_net` package, else fetched and built static).

Everything is non-blocking and runs on the main thread: start something, keep its handle, poll
it from a system. The system `net.update` (INPUT phase, priority `BR_NET_UPDATE_PRIORITY`, before
the engine's input and game systems) resolves and connects CONNECTING sockets, flushes queued
sends and finishes closed sockets' sends. No callbacks, no threads of its own (SDL3_net resolves
names on its own threads).

## Functions you actually need

| Function | What for |
|---|---|
| `br_net_register(e)` | Once in init. Idempotent; false when SDL3_net cannot start. |
| `br_net_connect(e, host, port)` | TCP client. Returns at once in CONNECTING; resolve + connect happen in `net.update`. |
| `br_net_socket_state(e, s)` | CONNECTING -> OPEN, or FAILED (name not found, refused, send failed). CLOSED after the peer left, and for zero / stale handles. |
| `br_net_send(e, s, data, size)` | Copies all bytes into a queue (never part of them); false unless OPEN. |
| `br_net_send_pending(e, s)` | Bytes still queued: backpressure for big or fast senders (the queue has no limit). |
| `br_net_receive(e, s, buf, cap)` | Bytes read, 0 = nothing now (also while CONNECTING), -1 = CLOSED / FAILED. The read that finds the peer gone returns -1 and sets CLOSED. |
| `br_net_close(e, s)` | Frees the handle at once; queued bytes keep going out for up to `BR_NET_CLOSE_LINGER` (5 s real time), then the connection closes. |
| `br_net_listen(e, port)` / `br_net_accept(e, srv)` / `br_net_server_close` | TCP server on all interfaces (IPv4 + IPv6). `accept` returns an OPEN socket or a zero handle when nobody waits; call it every frame. |
| `br_net_resolve(e, host)` / `br_net_address_state` / `br_net_address_get_string` / `br_net_address_release` | An address for UDP: CONNECTING while resolving, OPEN when resolved, FAILED. |
| `br_net_udp_open(e, port)` | UDP socket on all interfaces, 0 = any port. |
| `br_net_udp_send(e, u, to, port, data, size)` | One datagram (1..`BR_NET_MAX_DATAGRAM` bytes) to a resolved address. |
| `br_net_udp_receive(e, u, buf, cap, &from, &from_port)` | The next datagram's size, 0 = none, -1 = zero / stale handle. `from` is one handle per sender host, the same one every time until you release it. |

## Snippet

```c
typedef struct game {
    br_net_server server;
    br_net_socket players[4];
    char          buffer[4096];
} game;

static void net_system(br_engine *e, float dt, void *user)      /* BR_PHASE_UPDATE */
{
    game *g = user;
    (void)dt;
    br_net_socket incoming = br_net_accept(e, g->server);
    while (incoming.generation != 0) {
        put_in_free_player_slot(g, incoming);                    /* or br_net_close when full */
        incoming = br_net_accept(e, g->server);
    }
    for (int i = 0; i < 4; i++) {
        int n;
        while ((n = br_net_receive(e, g->players[i], g->buffer, sizeof g->buffer)) > 0) {
            feed_player_bytes(g, i, g->buffer, n);               /* a stream: messages need framing */
        }
        if (n < 0 && g->players[i].generation != 0) {
            br_net_close(e, g->players[i]);                      /* left, or the connection broke */
            g->players[i] = (br_net_socket){ 0, 0 };
        }
    }
}

/* init */
br_net_register(e);
g->server = br_net_listen(e, 47000);
br_system_add(e, "game.net", BR_PHASE_UPDATE, 0, net_system, g);
```

## Gotchas

- **TCP is a byte stream.** One `br_net_send` of 100 bytes can arrive as several receives and
  two sends as one. Put a length or a delimiter in front of every message.
- **Everything read is untrusted.** Check every length against a limit of yours before using it.
- **A peer that goes away is seen by the next `br_net_receive`** (it returns -1). A game that
  never reads does not notice; `net.update` sees only failed sends.
- **No connect timeout of its own.** A host that never answers stays CONNECTING until the
  operating system gives up (seconds to minutes); keep your own real-time deadline
  (`SDL_GetTicksNS`) and `br_net_close` it.
- **UDP sender handles take slots.** `br_net_udp_receive` with `from` makes one address handle per
  new sender host, `BR_NET_MAX_ADDRESSES` (32) in all with the resolved ones; release a peer's
  handle when it leaves, or pass `from = NULL` when you do not reply. When the table is full,
  `from` is zero and one warning is logged.
- **UDP errors read as 0.** Windows reports "port unreachable" for an earlier send on the next
  receive; that is logged and returns 0, and the socket keeps working.
- Datagrams larger than the receive buffer are dropped (logged). 64 KB holds any datagram.
- `br_net_send` copies into SDL3_net's queue (it grows once, then is reused: steady traffic
  allocates nothing). SDL3_net allocates one block per received datagram (`// ponytail:` in the
  source).
- Limits: `BR_NET_MAX_SOCKETS` 64 (connecting, open and lingering ones), `BR_NET_MAX_SERVERS` 8,
  `BR_NET_MAX_UDP` 8, `BR_NET_MAX_ADDRESSES` 32; one more returns a zero handle and logs.
- Zero and stale handles are safe everywhere: states read CLOSED, sends false, receives -1,
  closes are no-ops.
- Not on Emscripten (SDL3_net builds a stub there).
