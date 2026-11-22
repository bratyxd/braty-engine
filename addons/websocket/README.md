# websocket - RFC 6455 client (add-on)

Header: `addons/websocket/include/braty/br_websocket.h`. Sources: `addons/websocket/src/br_websocket.c`
(sockets, the handshake over `tls`, the system), `addons/websocket/src/br_websocket_frame.c` (the
handshake checks and `br_websocket_conn`, our wrapper around wslay over in-memory bytes),
`addons/websocket/src/br_websocket_internal.h` (both, for the tests). Tests:
`addons/websocket/tests/test_websocket_frame.c` (no sockets, a test clock) and
`addons/websocket/tests/test_websocket.c` (loopback, ports 47831..47833, `wss://` against the tls
test server). Example: `addons/websocket/examples/websocket_echo.c` (`websocket_echo <url> <text>`,
built, not run by CTest).

An add-on on top of `http`: built when `BRATY_ADDONS` names it. Link `braty::websocket` (`http`,
`tls`, `net`, Mbed TLS and SDL3_net come with it), call `br_websocket_register(e)` in init (it
registers the other three). The framing is **wslay 1.1.1** (vendored in
`addons/websocket/third_party/wslay/`, MIT): frames, masking, fragments, control frames, ping ->
pong, UTF-8 of text, the closing handshake. The opening handshake (on `http`'s request writer and
response parser), the limits, the keep-alive, the timeouts and the handle-and-poll API are ours.

## Functions you actually need

| Function | What for |
|---|---|
| `br_websocket_register(e)` | Once in init. Idempotent; registers `http`, `tls`, `net`. |
| `br_websocket_open(e, url, protocols, headers)` | `ws://` / `wss://`. Returns at once in CONNECTING. `protocols` = `"chat, v2"` or NULL; `headers` = extra `"Name: value\r\n"` lines (Origin, Authorization, Cookie) or NULL. |
| `br_websocket_socket_state(e, ws)` | CONNECTING -> OPEN -> (CLOSING) -> CLOSED, or FAILED. CLOSED for zero / stale handles. |
| `br_websocket_send_text(e, ws, text)` | A text message (valid UTF-8, else false + log). False unless OPEN. |
| `br_websocket_send_binary(e, ws, data, size)` | A binary message. False unless OPEN. |
| `br_websocket_receive(e, ws, &m)` | The next whole message (`m.data` NUL-terminated, `m.size`, `m.text`); valid until the next receive. Works after CLOSED / FAILED for what arrived before. |
| `br_websocket_close(e, ws, 1000, "bye")` | The closing handshake: CLOSING until the server answers (or 5 s), then CLOSED. |
| `br_websocket_close_code(e, ws)` | The server's close code (1000, 1001, 4xxx; 1005 = it sent none); 1006 = ended without a close frame. |
| `br_websocket_error(e, ws)` | Why it FAILED or ended uncleanly; "" after a clean close. |
| `br_websocket_release(e, ws)` | Always, when done: frees the slot and unread messages (an OPEN socket gets a close 1001 first). |
| `br_websocket_set_ping_interval(e, ws, s)` | Keep-alive (default 30 s, 0 = off). |
| `br_websocket_set_max_message(e, ws, bytes)` | Largest message accepted (default 16 MB); more fails with 1009. |

## Snippet: a connection that comes back (reconnect with backoff)

The server can go away at any time: a deploy (close 1001 or 1012), a network change, a laptop
lid. This system keeps one connection up: it reads every message (also the last ones after a
close), notices the end by polling, releases the handle and opens a new one after a delay that
doubles on every failed attempt, up to 30 s, in real time (a paused game still reconnects).

```c
typedef struct net_link {
    br_websocket ws;          /* zero = none */
    uint64_t     retry_at;    /* SDL_GetTicksNS of the next attempt */
    float        backoff;     /* seconds before the next attempt */
    bool         online;
} net_link;                   /* a field of the game-state struct; backoff starts at 0.5 */

static void link_system(br_engine *e, float dt, void *user)   /* BR_PHASE_UPDATE */
{
    game *g = user;
    net_link *link = &g->link;
    (void)dt;
    uint64_t now = SDL_GetTicksNS();
    br_websocket_message m;
    while (br_websocket_receive(e, link->ws, &m)) {            /* copy out what you keep */
        net_message_to_sim_input(&g->input, m.data, m.size, m.text);
    }
    switch (br_websocket_socket_state(e, link->ws)) {
    case BR_WEBSOCKET_OPEN:
        if (!link->online) {
            link->online = true;
            link->backoff = 0.5f;                              /* it worked: start small again */
            resend_unacknowledged(e, g);                       /* see "Messages in flight" */
        }
        break;
    case BR_WEBSOCKET_CONNECTING:
    case BR_WEBSOCKET_CLOSING:
        break;
    case BR_WEBSOCKET_CLOSED:                                  /* also: no handle yet */
    case BR_WEBSOCKET_FAILED:
        if (link->ws.generation != 0) {                        /* it just ended */
            uint16_t code = br_websocket_close_code(e, link->ws);
            br_log_warn("server link: code %u %s", (unsigned)code, br_websocket_error(e, link->ws));
            br_websocket_release(e, link->ws);
            link->ws = (br_websocket){ 0, 0 };
            link->online = false;
            link->retry_at = now + (uint64_t)((double)link->backoff * 1e9);
            link->backoff = SDL_min(link->backoff * 2.0f, 30.0f);
            if (code == 4001) {                                /* your server's "kicked": stay off */
                link->retry_at = UINT64_MAX;
            }
        }
        if (now >= link->retry_at) {
            link->ws = br_websocket_open(e, "wss://game.example.com/live", "game.v1", NULL);
            link->retry_at = now + (uint64_t)((double)link->backoff * 1e9);   /* if open refused */
        }
        break;
    }
}

/* init */
br_websocket_register(e);
g->link.backoff = 0.5f;
br_system_add(e, "net.link", BR_PHASE_UPDATE, 0, link_system, g);
```

Many clients reconnecting to one server at once should add a random part to the delay (for
example `backoff * br_random_range(br_random_engine(e), 0.5f, 1.0f)`), so they do not all arrive together.

### Messages in flight

`br_websocket_send_*` returning true means the message was handed to the socket, not that the
server got it: WebSocket has no receipts. When a connection drops, what was sent in its last
seconds (up to the operating system's send buffer, and everything after a silent death) may be
lost, and the game cannot tell which. So:

- Anything that must arrive (a purchase, a move in a turn-based game, a chat line) carries an id
  (a sequence number) and stays in a small fixed table until the server acknowledges that id
  with a message of its own. After a reconnect, `resend_unacknowledged` sends the table again,
  in order.
- The server treats ids it has seen as done (idempotent), so a resend after an ack that was lost
  does no harm.
- State that changes all the time (positions) is not resent: after a reconnect the server sends a
  snapshot (or the client asks for one) and both continue from it.
- Messages from the server work the same way the other direction: if it matters, the server
  resends what the client did not acknowledge.

## Disconnects: how each one looks to a polling game

| What happened | State | `close_code` | `error` |
|---|---|---|---|
| Never connected: refused, DNS, TLS or certificate error, not 101 (e.g. 403), a handshake check failed, handshake timeout (30 s) | FAILED | 1006 | the reason ("the server answered 403 instead of 101 Switching Protocols", "handshake: a wrong Sec-WebSocket-Accept ...", "opening handshake timed out after 30.0 s", the tls error) |
| The server closed cleanly | CLOSED (straight from OPEN) | the server's code (1000, 1001, 4xxx; 1005 when its close frame had none) | "" |
| You closed (`br_websocket_close`) and the server answered | CLOSED (after CLOSING) | the code in the server's answer (normally yours) | "" |
| You closed and the server did not answer within 5 s | CLOSED (after CLOSING) | 1006 | "no close reply within 5.0 s" |
| You closed while CONNECTING | CLOSED at once | 1006 | "" |
| Dropped without a close frame (TCP reset, the server process died, Wi-Fi gone with a reset) | CLOSED | 1006 | "connection dropped without a close frame" or the tls error |
| Silently dead (nothing received for 2 x the ping interval: the first interval sends a ping) | CLOSED | 1006 | "keep-alive: nothing received for 60.0 s (a ping went unanswered): connection dropped" |
| The server broke the protocol (see below) | FAILED | 1006 | what it broke and the code we closed with ("... (closed with 1002)") |

Messages that arrived before any of these stay readable with `br_websocket_receive` until
release. CLOSED or FAILED, the handle keeps its slot until `br_websocket_release`.

## What is checked (the server is untrusted)

| Case | Checked by | We close with |
|---|---|---|
| Status other than 101 | ours | (handshake: FAILED, nothing sent) |
| `Upgrade` not `websocket`, `Connection` without the token `Upgrade` (both case-insensitive, Connection as a list) | ours | (handshake) |
| `Sec-WebSocket-Accept` missing, repeated or not exactly base64(SHA-1(key + GUID)) | ours | (handshake) |
| `Sec-WebSocket-Extensions` in the answer (we offer none) | ours | (handshake) |
| `Sec-WebSocket-Protocol` not offered, more than one, or sent when we offered none | ours | (handshake) |
| Handshake response over 64 KB / 128 headers, malformed | `http`'s parser | (handshake) |
| Control frame over 125 bytes or fragmented; a length not in its shortest form; 64-bit length with the top bit | wslay detects it (and would send an empty close); we send the code | 1002 |
| RSV1-3 set (no extension agreed) | wslay | 1002 |
| A masked frame from the server | wslay | 1002 |
| Unknown opcode (3-7, B-F) | wslay | 1002 |
| Continuation without a start; a new text / binary frame inside a fragmented message | wslay | 1002 |
| Close code not 1000-1003, 1007-1014, 3000-4999 (so 1005, 1006, 1015, 999, 5000) | wslay (1012-1014 accepted by our patch, as libwebsockets does) | 1002 |
| Close frame with a 1-byte body | ours (wslay reads it as "no code") | 1002 |
| Text message not valid UTF-8 (checked as it arrives, across fragments; also cut inside a character) | wslay | 1007 |
| Close reason not valid UTF-8 | wslay | 1007 |
| Message over `max_message` (whole message, checked at each frame header before its payload is stored) | wslay (we set the limit) | 1009 |
| Unread messages over `BR_WEBSOCKET_MAX_UNREAD` (64 MB: the game stopped reading) | ours | 1008 |
| Out of memory storing a message | ours | 1011 |
| A ping | wslay answers with a pong of the same payload, also between fragments | - |
| The server's close | wslay answers with the same code, then sends nothing more; we close the TCP connection | its code |
| Your text not UTF-8 / your close code not sendable / your reason over 123 bytes | ours: send refused / 1000 sent instead / cut at a character | - |

After FAILED from a protocol error the close frame is sent and the connection closed at once
(RFC 6455 section 7.1.7), without waiting for an answer.

## Rules

- **Handle and poll.** `websocket.update` (INPUT phase, `BR_WEBSOCKET_UPDATE_PRIORITY`, right
  after `http.update`) reads, answers pings, writes, keeps alive and times out; your system reads
  the state and the messages. Nothing calls you back. Release every socket you opened.
- **Read every frame.** Messages wait in a per-socket queue until `br_websocket_receive`; a game
  that stops reading (a paused system group) fails the socket at 64 MB of unread data.
- **Sends go out at once** (`br_websocket_send_*` hands the frame to `tls` in the call). Large
  messages are fine (one frame, any size); there is no limit on what you send.
- **Timeouts are real time** (`SDL_GetTicksNS`): handshake 30 s (`BR_WEBSOCKET_HANDSHAKE_TIMEOUT`),
  closing handshake 5 s (`BR_WEBSOCKET_CLOSE_TIMEOUT`), keep-alive 30 s
  (`BR_WEBSOCKET_DEFAULT_PING_INTERVAL`). A game that must notice a dead server quickly sets
  `br_websocket_set_ping_interval(e, ws, 5.0f)` (dead after 10 s of silence).
- **Networking is view code.** Turn received messages into the sim's input struct; the sim never
  sees a socket. Play-tests do not depend on a server.

## Gotchas

- **Close codes 1012-1014** (service restart, try again later, bad gateway; IANA, after RFC 6455)
  are a normal close: CLOSED with that code (wslay 1.1.1 refused them; our copy is patched, as
  libwebsockets accepts them). 1015 is never valid in a close frame.
- **No subprotocol getter:** when you offer several protocols, the one the server chose is checked
  but not returned. Offer one, or put the version in the URL.
- **wslay allocates** (libc `malloc`, not `br_alloc`) a block per received frame and per queued
  message; our buffers (the output, the unread queue, the current message) grow once and are
  reused. `br_memory_get_stats` does not see wslay's blocks.
- **Not implemented** (`// ponytail:` in the plan): permessage-deflate and other extensions,
  proxies, redirects of the upgrade request (a 3xx FAILS with its status), a WebSocket server,
  fragmenting what you send (each message is one frame).

## Limits

| Constant | Value | What |
|---|---|---|
| `BR_WEBSOCKET_MAX` | 8 | sockets (any state, until released); one more: zero handle + log |
| `BR_WEBSOCKET_DEFAULT_MAX_MESSAGE` | 16 MB | received message size when `set_max_message` was not called (1009 over it) |
| `BR_WEBSOCKET_MAX_UNREAD` | 64 MB | received bytes waiting for `br_websocket_receive` (1008 over it); also the largest `max_message` |
| `BR_WEBSOCKET_DEFAULT_PING_INTERVAL` | 30 s | keep-alive ping after this much silence; dropped after twice it |
| `BR_WEBSOCKET_HANDSHAKE_TIMEOUT` | 30 s | open to OPEN |
| `BR_WEBSOCKET_CLOSE_TIMEOUT` | 5 s | CLOSING to CLOSED without an answer |
| handshake response | 64 KB, 128 headers | `BR_HTTP_MAX_HEADER_BYTES`, `BR_HTTP_MAX_HEADERS` |
| close reason | 123 bytes | longer reasons are cut at a character |

Each socket also takes a `tls` stream (`BR_TLS_MAX_STREAMS` 32) and a `net` socket.

## Updating wslay

Copy `lib/*.c`, `lib/*.h`, `lib/includes/wslay/wslay.h` and `wslayver.h.in` and `COPYING` from
the new release over `addons/websocket/third_party/wslay/`, re-apply the three `braty patch`
changes in `lib/wslay_event.c` (listed in `third_party/README.md`), set `PACKAGE_VERSION` in
`addons/websocket/CMakeLists.txt` (it writes `wslayver.h` and the `config.h` wslay expects:
`htons` / `ntohs` / `ntohl` from SDL's byte swaps, `ssize_t`, the byte order), update
`third_party/README.md`, run `test_websocket_frame` and `test_websocket`.
