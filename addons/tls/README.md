# tls - byte streams, plain or TLS (add-on)

A byte stream to host:port that is plain TCP or TLS 1.2 / 1.3 with SNI, certificate-chain and
host-name verification, plus the small crypto helpers `http` and `websocket` need (random bytes,
SHA-1, base64). Depends on `net`. Runs on **Mbed TLS 3.6.8 LTS** (Apache-2.0, fetched from the
release tarball with a SHA-256 check, built static with its default configuration) and the
**Mozilla CA bundle** `addons/tls/certs/cacert.pem` (MPL-2.0, 118 roots), compiled in as a C array
at configure time (nothing is read from disk at run time).

Header: `addons/tls/include/braty/br_tls.h`. Source: `addons/tls/src/br_tls.c`.
Test: `addons/tls/tests/test_tls.c` (loopback, ports 47811..47816), with the test server
`addons/tls/tests/tls_test_server.h`. The add-on system: `docs/agents/addons.md`.

Games normally use `http` or `websocket` on top; `tls` is for your own protocol over TCP.

## Enable it

- **Engine as a subdirectory:** `set(BRATY_ADDONS tls)` before `add_subdirectory(<braty>)` (or
  `-DBRATY_ADDONS=tls`; `net` is built with it), then
  `target_link_libraries(game PRIVATE braty::braty braty::tls)`.
- **Prebuilt SDK:** `find_package(braty CONFIG REQUIRED)`, then
  `target_link_libraries(game PRIVATE braty::braty braty::tls)`.

`braty::net`, SDL3_net and Mbed TLS (and on Windows `ws2_32`, `bcrypt`) come with it. In init:
`br_tls_register(e)` (idempotent; it registers `net`).

## Functions

| Function | What for |
|---|---|
| `bool br_tls_register(e)` | Once in init. Idempotent; registers `net`; false (logged) when net or the crypto cannot start. |
| `br_tls_stream br_tls_open(e, host, port, secure)` | Returns at once in CONNECTING. `host` is also the name the certificate must carry (and the SNI). Zero handle (logged) when full or bad arguments. |
| `br_net_state br_tls_state(e, s)` | CONNECTING (TCP connect, root certificates loading, handshake) -> OPEN (verified), or FAILED. CLOSED after the peer closed, and for zero / stale handles. |
| `bool br_tls_send(e, s, data, size)` | Encrypts (secure) and queues everything; false unless OPEN. A failed secure send turns the stream FAILED. |
| `size_t br_tls_send_pending(e, s)` | Bytes still queued on the socket (encrypted, for a secure stream): bound your sends with it. |
| `int br_tls_receive(e, s, buf, cap)` | Decrypted bytes, 0 = nothing now (also while CONNECTING), -1 = CLOSED / FAILED. |
| `bool br_tls_closed_cleanly(e, s)` | After CLOSED: true for close_notify (secure) or a TCP close (plain); false for a secure stream cut without close_notify. |
| `void br_tls_close(e, s)` | close_notify (secure, OPEN), then `br_net_close` (queued bytes still go out). |
| `const char *br_tls_error(e, s)` | Why it FAILED: "certificate of host:443 not accepted: The certificate validity has expired", "TLS handshake with host:443 failed: ..."; "" otherwise. Valid until close. |
| `bool br_tls_set_ca_pem(e, pem, size)` | Your own roots instead of the Mozilla bundle (your server's CA, pinning). PEM is copied. |
| `void br_tls_set_verify(e, on)` | Development only: `false` = no certificate check (warns on every secure open). Streams keep the setting they were opened with. |
| `bool br_tls_random(e, out, size)` | Cryptographic random bytes (CTR-DRBG). |
| `void br_tls_sha1(data, size, out[20])` | SHA-1 (the WebSocket handshake; not for security). |
| `size_t br_tls_base64_encode(data, size, out, cap)` | Base64, NUL-terminated; its length, 0 when cap is too small. |

## Snippet

```c
typedef struct game {
    br_tls_stream stream;
    bool          sent;
    char          buffer[4096];
} game;

static void fetch_system(br_engine *e, float dt, void *user)   /* BR_PHASE_UPDATE */
{
    game *g = user;
    (void)dt;
    br_net_state state = br_tls_state(e, g->stream);
    if (state == BR_NET_OPEN) {
        if (!g->sent) {
            static const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
            g->sent = br_tls_send(e, g->stream, request, sizeof request - 1);
        }
        int n;
        while ((n = br_tls_receive(e, g->stream, g->buffer, sizeof g->buffer)) > 0) {
            take_bytes(g, g->buffer, n);
        }
    } else if (g->stream.generation != 0 && (state == BR_NET_CLOSED || state == BR_NET_FAILED)) {
        if (state == BR_NET_FAILED) {
            br_log_error("fetch: %s", br_tls_error(e, g->stream));
        } else if (!br_tls_closed_cleanly(e, g->stream)) {
            br_log_warn("fetch: cut short (no close_notify)");
        }
        br_tls_close(e, g->stream);
        g->stream = (br_tls_stream){ 0, 0 };
    }
}

/* init */
br_tls_register(e);
g->stream = br_tls_open(e, "example.com", 443, true);
br_system_add(e, "game.fetch", BR_PHASE_UPDATE, 0, fetch_system, g);
```

## Rules

- **Handle and poll.** The system `tls.update` (INPUT phase, `BR_TLS_UPDATE_PRIORITY`, right after
  `net.update`) drives handshakes. A secure stream reads OPEN only after the handshake succeeded
  and the certificate was verified; anything else ends FAILED with a reason in `br_tls_error`.
- **Never ship `br_tls_set_verify(e, false)`.** It accepts any certificate (anyone on the path
  can read and change the traffic). For your own server's CA use `br_tls_set_ca_pem`.
- **Close what you open**; zero and stale handles are safe everywhere (CLOSED, -1, false, "", no-op).
- Bytes from the network are untrusted.

## Limits

| Constant | Value | What |
|---|---|---|
| `BR_TLS_MAX_STREAMS` | 32 | streams (plain and secure); one more: zero handle + log. Each also takes a `net` socket |
| `BR_TLS_MAX_HOST` | 255 | host name bytes (without the NUL) |
| `BR_TLS_UPDATE_PRIORITY` | `BR_NET_UPDATE_PRIORITY + 1` | priority of `tls.update` in the INPUT phase |

## Gotchas

- **Root certificates load lazily.** The first secure open starts a `br_job` that parses the
  built-in bundle (118 roots, about 10 ms on a worker thread); secure streams stay CONNECTING
  until it is done. A first `https` open to example.com reaches OPEN in about 150 ms (DNS,
  connect, parse, handshake). `br_tls_set_ca_pem` before the first open skips the bundle.
- **The handshake's public-key maths runs on the main thread** inside `tls.update` (a few ms per
  connection; `// ponytail:` a job per handshake if many connections open at once).
- **Peer close:** close_notify and a TCP end without close_notify both read as CLOSED (many
  servers skip close_notify). `br_tls_closed_cleanly` tells them apart: a protocol that ends a
  message at the close (an HTTP body without a length) treats a secure stream that did not close
  cleanly as truncated (`http` does, as curl does).
- **TLS 1.2 minimum.** TLS 1.0 / 1.1 servers fail ("Handshake protocol not within min/max
  boundaries").
- **No timeout of its own**, like `net`: a server that accepts and never answers keeps the
  stream CONNECTING. Keep a real-time deadline (`SDL_GetTicksNS`) and close it.
- **Memory:** Mbed TLS allocates its record buffers (about 33 KB) per secure stream at the
  handshake with the C library's `calloc`, and frees them at close; sends and receives on an open
  stream allocate nothing.
- **The bundle ages.** Update `certs/cacert.pem` before a release (`third_party/README.md`).

## Not implemented

Session resumption (TLS 1.3 tickets are read and ignored), client certificates, a trimmed Mbed TLS
configuration (`// ponytail:` in `CMakeLists.txt`: the default builds everything), the operating
system's certificate store (the bundle is compiled in), OCSP / revocation checks, a TLS server
(the test server is test code), Emscripten.

## Testing against a local TLS server

`addons/tls/tests/tls_test_server.h` (header-only, `static inline`) is a TLS server on Mbed TLS
over a `br_net` server socket, pumped from the test's own loop. Certificates in
`tests/data/tls/` (`README.md` there has the `openssl` commands): `ca.pem` signs `server.pem`
(name `localhost` only), `untrusted_ca.pem` signs `untrusted_server.pem`; both use `server.key`.
A test target gets the header and Mbed TLS with
`target_link_libraries(test_x PRIVATE braty_tls_test_server)` (http and websocket CMakeLists,
after `braty_addon`, inside `if(TARGET test_x)`).

```c
static tls_test_server server;                 /* Mbed TLS keeps pointers: never move these */
static tls_test_conn conn;
tls_test_server_start(&server, e, 47821, "server.pem");   /* or "untrusted_server.pem" */
tls_test_trust_ca(e);                                       /* br_tls_set_ca_pem(ca.pem) */
br_tls_stream s = br_tls_open(e, "localhost", 47821, true);
while (looping) {
    br_engine_step(e, 1.0f / 60.0f);
    if (!conn.used) { tls_test_server_accept(&server, &conn); }
    int n = tls_test_conn_receive(&conn, buf, sizeof buf);  /* drives its handshake; -1 = gone */
    if (n > 0) { tls_test_conn_send(&conn, reply, reply_size); }
}
tls_test_conn_close(&conn);                    /* close_notify; conn is free for the next accept */
tls_test_server_stop(&server);
```
