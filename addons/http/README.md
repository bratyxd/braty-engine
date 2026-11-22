# http - HTTP/1.1 client (add-on)

Header: `addons/http/include/braty/br_http.h`. Sources: `addons/http/src/br_http.c` (requests,
redirects, timeouts), `addons/http/src/br_http_parse.c` (URLs, the request writer, the response
parser), `addons/http/src/br_http_internal.h` (the pure parts, for the parse test and
`websocket`). Tests: `addons/http/tests/test_http_parse.c` (no sockets) and
`addons/http/tests/test_http.c` (loopback, ports 47821..47823). Example:
`addons/http/examples/http_get.c` (`http_get <url> [max_body]`, built, not run by CTest).

An add-on on top of `tls`: built when `BRATY_ADDONS` names it (or `websocket`). Link
`braty::http` (`braty::tls`, `braty::net`, Mbed TLS and SDL3_net come with it), call
`br_http_register(e)` in init (it registers `tls` and `net`). The status line, the headers and
chunked bodies are parsed by picohttpparser (vendored in `addons/http/third_party/picohttpparser/`,
MIT); URLs, the request, body framing, limits, redirects and timeouts are ours.

## Functions you actually need

| Function | What for |
|---|---|
| `br_http_register(e)` | Once in init. Idempotent; registers `tls` and `net`. |
| `br_http_get(e, url)` | A GET with the defaults. Returns at once in PENDING. |
| `br_http_send(e, &desc)` | Any method, headers, body, `timeout`, `max_body`. Copies everything. |
| `br_http_request_state(e, r)` | PENDING -> DONE (a complete response, any status) or FAILED. NONE for zero / stale / released handles. |
| `br_http_status(e, r)` | 200, 404...; 0 until DONE. |
| `br_http_header(e, r, name)` | Case-insensitive; the first when repeated; NULL when absent. |
| `br_http_header_count(e, r)`, `br_http_header_get_at(e, r, i, &name, &value)` | Every header line in order, repeated names (each `Set-Cookie`) included. |
| `br_http_body(e, r, &size)` | NUL-terminated body; valid until release. |
| `br_http_progress(e, r)` | Body bytes / Content-Length (0 when unknown, 1 when DONE): a download bar. |
| `br_http_error(e, r)` | Why it FAILED ("timed out after 30.0 s", "bad response: body of 100 bytes is over the limit of 10", the tls reason...). |
| `br_http_release(e, r)` | Always, when done with it: frees the response; cancels a pending request. |
| `br_http_url_parse(url, &out)` | Splits `http://`, `https://`, `ws://`, `wss://` URLs. |

## Snippet

```c
static void scores_system(br_engine *e, float dt, void *user)   /* BR_PHASE_UPDATE */
{
    game *g = user;
    (void)dt;
    switch (br_http_request_state(e, g->scores)) {
    case BR_HTTP_DONE:
        if (br_http_status(e, g->scores) == 200) {
            size_t size = 0;
            const char *json = br_http_body(e, g->scores, &size);
            load_scores(g, json, size);                 /* copy what you keep: the body goes at release */
        }
        br_http_release(e, g->scores);
        g->scores = (br_http_request){ 0, 0 };
        break;
    case BR_HTTP_FAILED:
        br_log_warn("scores: %s", br_http_error(e, g->scores));
        br_http_release(e, g->scores);
        g->scores = (br_http_request){ 0, 0 };
        break;
    default:                                            /* PENDING, or NONE: nothing asked */
        break;
    }
}

/* init */
br_http_register(e);
g->scores = br_http_get(e, "https://example.com/scores.json");

/* a POST */
br_http_desc desc = { 0 };
desc.method = "POST";
desc.url = "https://example.com/submit";
desc.headers = "Content-Type: application/json\r\n";
desc.body = json_text;
desc.body_size = json_size;
desc.timeout = 10.0f;
g->submit = br_http_send(e, &desc);
```

## Rules

- **Handle and poll.** Nothing blocks and nothing calls you back: `http.update` (INPUT phase,
  `BR_HTTP_UPDATE_PRIORITY`, right after `tls.update`) connects, sends, reads and parses; your
  system reads the state. Release every request you started, DONE or FAILED (the table has 16
  slots and a finished request keeps its slot and its body until release).
- **DONE means a complete response, whatever the status.** Check `br_http_status`. FAILED means
  there is no trustworthy response: timeout, connection error, certificate error, malformed or
  truncated response, a limit.
- **Added headers:** `Host`, `User-Agent: braty`, `Accept-Encoding: identity`,
  `Connection: close` and (with a body, or for POST / PUT / PATCH) `Content-Length`, each unless
  your `headers` name it. One connection per request.
- **Redirects** 301 / 302 / 303 / 307 / 308 with a `Location` are followed, up to
  `BR_HTTP_MAX_REDIRECTS` (5; the 6th fails). 303, and 301 / 302 after a POST, become GET
  without the body (and without your `Content-Length` / `Content-Type` / `Transfer-Encoding`);
  307 / 308 keep method and body. Never from https to http. On a redirect to another host, port
  or scheme your `Authorization`, `Cookie`, `Proxy-Authorization` and `Host` lines are dropped
  (as curl does; they stay dropped for the rest of the chain). A 3xx without `Location` is the response.
- **Timeout** (`desc.timeout`, default 30 s) is real time (`SDL_GetTicksNS`) and covers the whole
  request: DNS, connect, TLS handshake, every redirect, the body. A paused game still times out.

## What is checked (the response is untrusted)

| Case | Rule |
|---|---|
| Header block over `BR_HTTP_MAX_HEADER_BYTES` (64 KB, interim 1xx responses included) or over `BR_HTTP_MAX_HEADERS` (128) lines | FAILED; the buffer never holds more than the limit before the headers end |
| Body over `max_body` (default 16 MB) | FAILED: at once when Content-Length says so, else when the bytes (or a chunk size) cross it |
| `Content-Length` negative, not digits only (`+5`, `5, 5`, `1 2`), empty, overflowing | FAILED (RFC 9110 section 8.6) |
| Two `Content-Length` lines with different values | FAILED (RFC 9112 section 6.3 item 5); equal repeats are accepted |
| `Transfer-Encoding: chunked` and `Content-Length` together | Transfer-Encoding wins, Content-Length is ignored (RFC 9112 section 6.3 item 3); the connection is closed after it as always |
| `Transfer-Encoding` other than exactly `chunked` (gzip, lists, repeated lines) | FAILED: we asked for no coding |
| 1xx, 204, 304, any response to HEAD | No body, whatever the headers say (RFC 9112 section 6.3 item 1) |
| Interim `100 Continue`, `103 Early Hints` | Skipped; the final response follows (RFC 9110 section 15.2). An unasked `101` FAILS. |
| NUL, a lone CR or another control character in a header name or value | FAILED (picohttpparser). A lone LF ends a line (RFC 9112 section 2.2 allows it). |
| obs-fold (a header line starting with space / tab) | Joined to the line before with spaces (RFC 9112 section 5.2) |
| A CR or LF in your `url`, `method` or `headers`; a header line that is not `Name: value`; an empty line; a method that is not a token; `CONNECT` | `br_http_send` returns a zero handle and logs; nothing is sent |
| The connection closes before the response is complete (headers, Content-Length, chunked) | FAILED "connection closed ..."; never DONE |
| No length at all | The body runs until the close (RFC 9112 section 6.3 item 8); over TLS the close must be close_notify, else FAILED (truncation) |

## Gotchas

- **Body without a length vs. truncation:** a response with neither Content-Length nor chunked
  ends at the close. Over `https://` it is DONE only when the server sent TLS close_notify; a
  bare TCP end FAILS it ("... without TLS close_notify ..."), as curl does, because it may be
  cut short. Over `http://` the TCP close is all there is: such a body is only as safe as that.
- **Repeated headers:** `br_http_header` returns the first line of a name; walk
  `br_http_header_get_at` for all of them (several `Set-Cookie`). No cookies are kept.
- **Not implemented** (`// ponytail:` in the plan): keep-alive (one connection per request),
  gzip (`Accept-Encoding: identity` is sent), proxies, cookies, HTTP/2, `Expect: 100-continue`
  waits (a 100 is skipped if a server sends one), user info in URLs (`user:pw@host` fails: send an
  `Authorization` header). Relative redirects keep `.` / `..` segments as sent.
- **URLs are not escaped for you:** a space or a control character in the path fails; percent-
  encode it. The fragment (`#...`) is dropped. A redirect `Location` with spaces or raw UTF-8 is
  percent-encoded for you (as curl and browsers do).
- **Memory:** `br_http_send` copies the headers and body; the response buffer grows to the
  header block plus the body (reserved once when Content-Length is known) and is freed at
  release. A steady download allocates only when that buffer grows.

## Limits

| Constant | Value | What |
|---|---|---|
| `BR_HTTP_MAX_REQUESTS` | 16 | requests in flight (pending or done, not released); one more: zero handle + log |
| `BR_HTTP_MAX_HEADER_BYTES` | 64 KB | response header block (interim responses included) |
| `BR_HTTP_MAX_HEADERS` | 128 | response header lines |
| `BR_HTTP_DEFAULT_MAX_BODY` | 16 MB | body bytes when `desc.max_body` is 0 |
| `BR_HTTP_DEFAULT_TIMEOUT` | 30 s | whole request when `desc.timeout` is 0 |
| `BR_HTTP_MAX_REDIRECTS` | 5 | redirects followed |
| `BR_HTTP_MAX_METHOD` | 31 | method name bytes |
| `br_http_url` | host 255, path 2047 | URL parts (bytes) |

Each request also takes a `tls` stream (`BR_TLS_MAX_STREAMS` 32) and a `net` socket while it is
pending.

## For `websocket`: the handshake through `br_http_internal.h`

`br_http_url_split(url, &u, &is_ws, err, cap)` splits `ws://` / `wss://` (default ports 80 / 443).
`br_http_write_request(out, cap, "GET", &u, headers, false, 0, err, cap)` writes the request head
(call with `out` NULL first for the size; pass `Upgrade`, `Connection: Upgrade`, `Sec-WebSocket-*`
in `headers`, so its own `Connection: close` is not added). Then a parser:

```c
br_http_parser *p = br_http_parser_create(0, false);
/* for each br_tls_receive(e, s, buf, n) > 0 while the result is MORE: */
br_http_parse_result r = br_http_parser_feed(p, buf, (size_t)n);
if (r == BR_HTTP_PARSE_DONE && br_http_parser_status(p) == 101) {
    const char *accept = br_http_parser_header(p, "Sec-WebSocket-Accept");
    size_t extra = 0;
    const void *frames = br_http_parser_unread(p, &extra);   /* first frame bytes, if any came */
    /* hand frames / extra to the frame decoder, then read the stream directly */
}
/* another status: DONE with it (the error names it); ERROR: br_http_parser_error(p);
 * the peer closed: br_http_parser_finish(p) */
br_http_parser_destroy(p);
```
