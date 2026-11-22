# net - TCP and UDP sockets (add-on)

TCP client and server sockets, UDP sockets and host addresses on **SDL3_net** `release-3.2.0`
(zlib; an installed `SDL3_net` package when CMake finds one, else fetched from
github.com/libsdl-org/SDL_net and built static). Depends on no other add-on. `tls`, `http` and
`websocket` are built on it.

Header: `addons/net/include/braty/br_net.h`. Source: `addons/net/src/br_net.c`.
Test: `addons/net/tests/test_net.c` (loopback, ports 47801..47804). The add-on system:
`docs/agents/addons.md`.

## Enable it

- **Engine as a subdirectory:** `set(BRATY_ADDONS net)` (or a list, or `all`) before
  `add_subdirectory(<braty>)`, or `-DBRATY_ADDONS=net` on the command line; then
  `target_link_libraries(game PRIVATE braty::braty braty::net)`.
- **Prebuilt SDK:** `find_package(braty CONFIG REQUIRED)`, then
  `target_link_libraries(game PRIVATE braty::braty braty::net)`.

SDL3_net (and on Windows `ws2_32`, `iphlpapi`) comes with `braty::net`; nobody names it. In init:
`br_net_register(e)` (idempotent; `tls`, `http` and `websocket` call it for you).

## Functions

| Function | What for |
|---|---|
| `bool br_net_register(e)` | Once in init. Idempotent; false (logged) when SDL3_net cannot start. |
| `br_net_socket br_net_connect(e, host, port)` | TCP client. Returns at once in CONNECTING; resolve + connect happen in `net.update`. |
| `br_net_state br_net_socket_state(e, s)` | CONNECTING -> OPEN, or FAILED (name not found, refused, send failed). CLOSED after the peer left, and for zero / stale handles. |
| `bool br_net_send(e, s, data, size)` | Copies all bytes into a queue (never part of them); false unless OPEN or when size is over INT_MAX. |
| `size_t br_net_send_pending(e, s)` | Bytes still queued: backpressure for big or fast senders (the queue has no limit). |
| `int br_net_receive(e, s, buf, cap)` | Bytes read, 0 = nothing now (also while CONNECTING), -1 = CLOSED / FAILED. The read that finds the peer gone returns -1 and sets CLOSED. |
| `void br_net_close(e, s)` | Frees the handle at once; queued bytes keep going out for up to `BR_NET_CLOSE_LINGER` (5 s real time), then the connection closes. |
| `br_net_server br_net_listen(e, port)` | TCP server on all interfaces (IPv4 + IPv6); zero handle (logged) on failure. |
| `br_net_socket br_net_accept(e, server)` | The next waiting connection, already OPEN; zero handle when nobody waits. Call it every frame. |
| `void br_net_server_close(e, server)` | Stops listening; accepted sockets stay open. |
| `br_net_address br_net_resolve(e, host)` | An address for UDP; poll `br_net_address_state`. |
| `br_net_state br_net_address_state(e, a)` | CONNECTING while resolving, OPEN when resolved, FAILED when not found, CLOSED for zero / stale. |
| `bool br_net_address_get_string(e, a, out, cap)` | Numeric text ("127.0.0.1", "::1"); false (out = "") unless resolved. |
| `void br_net_address_release(e, a)` | Frees an address handle (once, however often `udp_receive` returned it). |
| `br_net_udp br_net_udp_open(e, port)` | UDP socket on all interfaces, 0 = any free port. |
| `bool br_net_udp_send(e, u, to, port, data, size)` | One datagram (1..`BR_NET_MAX_DATAGRAM` bytes) to a resolved address. |
| `int br_net_udp_receive(e, u, buf, cap, &from, &from_port)` | The next datagram's size, 0 = none, -1 = zero / stale handle. `from` is one handle per sender host, the same one every time until you release it (may be NULL). |
| `void br_net_udp_close(e, u)` | Closes the UDP socket. |

## Snippet

A server for four players, in a system (handle and poll: nothing blocks, nothing calls back).

```c
typedef struct game {
    br_net_server server;
    br_net_socket players[4];      /* zero = free slot */
    char          buffer[4096];
} game;

static void net_system(br_engine *e, float dt, void *user)      /* BR_PHASE_UPDATE */
{
    game *g = user;
    (void)dt;
    br_net_socket incoming = br_net_accept(e, g->server);
    while (incoming.generation != 0) {
        int slot = -1;
        for (int i = 0; i < 4 && slot < 0; i++) {
            if (g->players[i].generation == 0) { slot = i; }
        }
        if (slot < 0) {
            br_net_close(e, incoming);                           /* full */
        } else {
            g->players[slot] = incoming;
        }
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

## Rules

- **Handle and poll.** The system `net.update` (INPUT phase, `BR_NET_UPDATE_PRIORITY`, before the
  engine's input and game systems) resolves and connects CONNECTING sockets, flushes queued sends
  and finishes closed sockets' sends. Your systems read states and bytes. No callbacks, no threads
  of its own (SDL3_net resolves names on its own threads).
- **Everything read is untrusted.** Check every length against a limit of yours before using it.
- **Close what you open** (`br_net_close`, `br_net_server_close`, `br_net_udp_close`,
  `br_net_address_release`): the tables are fixed.
- Zero and stale handles are safe everywhere: states read CLOSED, sends false, receives -1,
  closes are no-ops.

## Limits

| Constant | Value | What |
|---|---|---|
| `BR_NET_MAX_SOCKETS` | 64 | TCP sockets (connecting, open, accepted and lingering); one more: zero handle + log |
| `BR_NET_MAX_SERVERS` | 8 | listening servers |
| `BR_NET_MAX_UDP` | 8 | UDP sockets |
| `BR_NET_MAX_ADDRESSES` | 32 | address handles (resolved ones and UDP senders) |
| `BR_NET_MAX_DATAGRAM` | 64 KB | largest UDP payload sent or received |
| `BR_NET_CLOSE_LINGER` | 5 s | real time `br_net_close` keeps sending queued bytes |
| `BR_NET_UPDATE_PRIORITY` | -1500000 | priority of `net.update` in the INPUT phase |

## Gotchas

- **TCP is a byte stream.** One `br_net_send` of 100 bytes can arrive as several receives and
  two sends as one. Put a length or a delimiter in front of every message.
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
- Datagrams larger than the receive buffer are dropped (logged) and the next one is tried. A
  64 KB buffer holds any datagram.
- **Memory:** `br_net_send` copies into SDL3_net's queue (it grows once, then is reused: steady
  traffic allocates nothing). SDL3_net allocates one block per received datagram (`// ponytail:`
  in the source).

## Not implemented

TLS (use `tls`), a connect timeout, socket options (Nagle, buffer sizes), multicast and
broadcast, Emscripten (SDL3_net builds a stub there; the add-ons are tested on Windows, Linux and
macOS).
