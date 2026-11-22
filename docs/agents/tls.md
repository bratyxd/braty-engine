# tls - byte streams, plain or TLS (add-on)

Header: `addons/tls/include/braty/br_tls.h`. Source: `addons/tls/src/br_tls.c`.
Test: `addons/tls/tests/test_tls.c` (loopback, ports 47811..47816), with the test server
`addons/tls/tests/tls_test_server.h`.

An add-on on top of `net`: built when `BRATY_ADDONS` names it (or `http` / `websocket`, which
need it). Link `braty::tls` (`braty::net` and Mbed TLS come with it), call `br_tls_register(e)`
in init (it registers `net`). Runs on Mbed TLS 3.6.8 LTS (fetched from the release tarball, built
static with its default configuration) and the Mozilla CA bundle `addons/tls/certs/cacert.pem`,
compiled in as a C array (nothing is read from disk).

A stream is plain TCP (`secure` false) or TLS 1.2 / 1.3 with SNI, certificate-chain and
host-name verification. The system `tls.update` (INPUT phase, `BR_TLS_UPDATE_PRIORITY`, right
after `net.update`) drives the handshakes. A secure stream reads OPEN only after the handshake
succeeded and the certificate was verified; anything else ends FAILED with a reason in
`br_tls_error`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_tls_register(e)` | Once in init. Idempotent; registers `net`. |
| `br_tls_open(e, host, port, secure)` | Returns at once in CONNECTING. `host` is also the name the certificate must carry (and the SNI). |
| `br_tls_state(e, s)` | CONNECTING (TCP connect, root certificates loading, handshake) -> OPEN, or FAILED. CLOSED after the peer closed, and for zero / stale handles. |
| `br_tls_send(e, s, data, size)` | Encrypts and queues everything; false unless OPEN. |
| `br_tls_send_pending(e, s)` | Bytes still queued (the peer is slow or not reading): bound your sends with it. |
| `br_tls_receive(e, s, buf, cap)` | Decrypted bytes, 0 = nothing now (also while CONNECTING), -1 = CLOSED / FAILED. |
| `br_tls_closed_cleanly(e, s)` | After CLOSED: true for close_notify (secure) or a TCP close (plain); false for a secure stream cut without close_notify. |
| `br_tls_close(e, s)` | close_notify (secure, OPEN), then `br_net_close` (queued bytes still go out). |
| `br_tls_error(e, s)` | Why it FAILED: "certificate of host:443 not accepted: The certificate validity has expired", "TLS handshake with host:443 failed: ...". |
| `br_tls_set_ca_pem(e, pem, size)` | Your own roots instead of the Mozilla bundle (your server's CA, pinning). |
| `br_tls_set_verify(e, false)` | Development only: no certificate check (warns on every secure open). Streams keep the setting they were opened with. |
| `br_tls_random` / `br_tls_sha1` / `br_tls_base64_encode` | Crypto random bytes (CTR-DRBG), SHA-1 and base64 (the WebSocket handshake). |

## Snippet

```c
static void fetch_system(br_engine *e, float dt, void *user)   /* BR_PHASE_UPDATE */
{
    game *g = user;
    (void)dt;
    switch (br_tls_state(e, g->stream)) {
    case BR_NET_OPEN:
        if (!g->sent) {
            static const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
            g->sent = br_tls_send(e, g->stream, request, sizeof request - 1);
        }
        int n;
        while ((n = br_tls_receive(e, g->stream, g->buffer, sizeof g->buffer)) > 0) {
            take_bytes(g, g->buffer, n);
        }
        break;
    case BR_NET_CLOSED:                                     /* the server finished */
    case BR_NET_FAILED:
        if (br_tls_state(e, g->stream) == BR_NET_FAILED) {
            br_log_error("fetch: %s", br_tls_error(e, g->stream));
        }
        br_tls_close(e, g->stream);
        g->stream = (br_tls_stream){ 0, 0 };
        break;
    default:
        break;
    }
}

/* init */
br_tls_register(e);
g->stream = br_tls_open(e, "example.com", 443, true);
```

Games normally use `http` or `websocket` on top; `tls` is for your own protocol over TCP.

## Behaviour

- **Root certificates load lazily.** The first secure open starts a `br_job` that parses the
  built-in bundle (118 roots, about 10 ms on a worker thread); secure streams stay
  CONNECTING until it is done. A first `https` open to example.com reaches OPEN in about 150 ms
  (DNS, connect, parse, handshake). `br_tls_set_ca_pem` before the first open skips the bundle.
- **The handshake's public-key maths runs on the main thread** inside `tls.update` (a few ms per
  connection; `// ponytail:` in the plan: a job per handshake if many connections open at once).
- **Peer close:** close_notify and a TCP end without close_notify both read as CLOSED (many
  servers skip close_notify). `br_tls_closed_cleanly` tells them apart: a protocol that ends a
  message at the close (an HTTP body without a length) treats a secure stream that did not close
  cleanly as truncated (`http` does, as curl does).
- **TLS 1.2 minimum.** TLS 1.0 / 1.1 servers fail ("Handshake protocol not within min/max
  boundaries"). No session resumption (TLS 1.3 tickets are read and ignored).
- **No timeout of its own**, like `net`: a server that accepts and never answers keeps the
  stream CONNECTING. Keep a real-time deadline (`SDL_GetTicksNS`) and close it.
- Zero and stale handles are safe everywhere: CLOSED, -1, false, "", no-op.
- Limits: `BR_TLS_MAX_STREAMS` 32 (one more: zero handle + log), host names up to
  `BR_TLS_MAX_HOST` (255) bytes. Each stream also takes a `net` socket.
- Memory: Mbed TLS allocates its record buffers (about 33 KB) per secure stream at the handshake
  with the C library's `calloc`, and frees them at close; sends and receives on an open stream
  allocate nothing.

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
