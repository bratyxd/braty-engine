# Protocol audit of the `http` and `websocket` add-ons (Task 5b, 2026-11-22)

What was compared, rule by rule, against battle-tested C libraries; what was fixed (each fix has a
test that fails without it); what is left out on purpose and what remains a limit.

## Sources compared

| Source | Version read | What was read |
|---|---|---|
| libcurl | master 01798a0 (2026-11-22) | `lib/http.c` (custom headers on redirects, `Curl_auth_allowed_to_host`, Transfer-Encoding, status codes), `lib/http.h` (header limits), `lib/vtls/openssl.c`, `mbedtls.c`, `schannel.c` (end of stream without close_notify), `lib/ws.c` (handshake, auto-pong, control frames), `docs/libcurl/opts/CURLOPT_MAXREDIRS.md`, `CURLOPT_REDIR_PROTOCOLS_STR.md`; the vulnerability table at curl.se/docs/vulnerabilities.html |
| llhttp | main 85c5d42 (2026-11-21) | `test/response/*.md` fixtures (what a strict parser rejects); the cases were run through our parser |
| picohttpparser | dd573d8 (2026-10-27) = the vendored commit (no later commits) | README usage, its commit history (bare LF in chunk lines, chunk overhead, strict chunk sizes, status range: all in our copy) |
| libwebsockets | main 4af168b (2026-11-22) | `lib/sansio/ws/client-ws.c` (handshake checks), `client-parser-ws.c` (close codes, ping / pong handling) |
| wslay | release-1.1.1 (vendored) and master 0e7d106 (2022-08-25, last commit) | all 28 commits after 1.1.1, the open issues and pull requests on GitHub, `lib/wslay_event.c` |
| Autobahn testsuite | main 39431b0 (2026-11-22) | `autobahntestsuite/case/` case list (1 framing ... 7 close handling, 9 limits, 10 misc, 12 compression) |
| RFCs | 9110, 9112, 3986, 6455 | the client rules cited below |

Checklist verdicts: **same** (as the reference, or the reference library's rule), **different-but-correct**
(another valid choice, named), **gap** (missing, a real server or attacker hits it), **bug** (wrong
behaviour). Every gap and bug below is fixed; the commit is in the last column.

## HTTP checklist

| # | Rule | Reference | Ours | Verdict | Test / commit |
|---|---|---|---|---|---|
| H1 | Schemes http(s) / ws(s), any case; default ports; empty port = default (RFC 3986 3.2.3) | curl the same | the same | same | `test_urls` |
| H2 | Port 1-65535; userinfo | curl takes userinfo | userinfo refused (send an `Authorization` header) | different-but-correct | `test_urls` |
| H3 | IPv6 literal; zone id `%25eth0` | curl takes zone ids | brackets yes, zone ids no | different-but-correct | `test_urls` |
| H4 | Non-ASCII (IDN) host | curl with libidn2 converts | refused | known limit | `test_resolve` |
| H5 | Space / CTL in the caller's URL | curl refuses (unless asked to encode) | refused | same | `test_urls` |
| H6 | Fragment not sent (RFC 9110 4.2.4) | the same | the same | same | `test_urls` |
| H7 | CR / LF / CTL in method, path, header lines (request splitting) | curl refuses CR / LF in URLs; custom header lines are the caller's | all refused before anything is sent | different-but-correct (stricter) | `test_writer`, `test_bad_arguments` |
| H8 | Host with IPv6 brackets and non-default port | the same | the same | same | `test_writer` |
| H9 | Content-Length with a body, and for POST / PUT / PATCH | the same | the same | same | `test_post` |
| H10 | Caller's `Transfer-Encoding` together with our `Content-Length` (RFC 9112 6.2: MUST NOT send both) | curl chunks the body itself | sent both | **gap** -> refused | `test_writer`; 7c397dc |
| H11 | Status line `HTTP/1.x`, 3-digit code | curl: 3 digits >= 100; llhttp 100-999; picohttpparser 100-599 | 100-599 (picohttpparser) | different-but-correct | `test_status_lines` |
| H12 | HTTP/0.9 response; leading CRLF before the status line | curl refuses 0.9; llhttp skips a CRLF | both refused | different-but-correct | probe run (llhttp fixtures) |
| H13 | Space before `:`, non-token name, NUL / CTL / lone CR in a value (RFC 9112 5.1, 5.5) | llhttp rejects | rejected (picohttpparser) | same | `test_header_bytes` |
| H14 | Bare LF as line end in the head (RFC 9112 2.2 MAY) | curl accepts, llhttp strict rejects | accepted | same as curl | `test_header_bytes` |
| H15 | obs-fold replaced by SP (RFC 9112 5.2) | curl unfolds | joined with spaces | same | `test_header_bytes` |
| H16 | Header block bound | curl 300 KB / 5000 lines (CVE-2023-38039 class) | 64 KB / 128 lines, 1xx included | same (bounded) | `test_limits` |
| H17 | Interim 1xx skipped (RFC 9110 15.2); endless 1xx | curl skips | skipped, counted against the header limit | same | `test_no_body`, `test_limits` |
| H18 | No body for HEAD, 1xx, 204, 304 (RFC 9112 6.3 item 1) | the same | the same | same | `test_no_body` |
| H19 | Transfer-Encoding and Content-Length: TE wins (6.3 item 3) | curl the same; llhttp strict errors | TE wins, connection closed after | same as curl | `test_chunked` |
| H20 | TE other than exactly `chunked` (gzip, lists, `chunkedchunked`, folded) | curl decodes or reads to close | FAILED (we asked for no coding) | different-but-correct | `test_chunked`, probe |
| H21 | chunked in an HTTP/1.0 response (RFC 9112 6.1: faulty framing) | curl decodes it | decoded; connection never reused | same as curl | probe |
| H22 | Content-Length not 1*DIGIT, negative, a list, empty, overflowing (RFC 9110 8.6) | llhttp, curl reject | rejected | same | `test_content_length` |
| H23 | Two different Content-Length values (6.3 item 5) | rejected | rejected; equal repeats accepted | same | `test_content_length` |
| H24 | Body over the caller's limit | curl `CURLOPT_MAXFILESIZE` | FAILED before the body when the length says so | same | `test_limits` |
| H25 | A huge Content-Length with `max_body = SIZE_MAX` | - | the buffer's doubling wrapped to 0: endless loop | **bug** -> refused | `test_limits`; 4844e61 |
| H26 | Chunk size overflow, `0x`, `-`, junk after the size | rejected | rejected (picohttpparser) | same | `test_chunked`, probe |
| H27 | Chunk sizes over the limit; tiny-chunk overhead flood | - | our limit before the data; picohttpparser's overhead check; real-time deadline | same | `test_chunked` |
| H28 | Bare LF in chunk lines; chunk-ext | curl accepts bare LF; llhttp validates ext | bare LF rejected (picohttpparser a875a01), ext ignored | different-but-correct | probe |
| H29 | Trailers | curl exposes them | read and dropped (RFC 9112 7.1.2 allows) | different-but-correct | `test_chunked` |
| H30 | Truncated head / length body / chunked body | error | FAILED, never DONE (every byte offset) | same | `test_truncated`, `test_responses` |
| H31 | Close-delimited body over TLS ended without close_notify | curl: error ("Connection closed abruptly" / `CURLE_RECV_ERROR`, all TLS backends) | DONE (a truncation looked complete) | **bug** -> FAILED | `test_https`, `test_tls`; 0f975e0, b737839 |
| H32 | Whole-request real-time timeout | `CURLOPT_TIMEOUT` | the same, covers redirects | same | `test_timeout` |
| H33 | Redirect codes followed; limit | curl 30 since 8.3.0 | 301/302/303/307/308, 5 | different-but-correct | `test_redirects` |
| H34 | 303 -> GET (not HEAD); 301/302 after POST -> GET; 307/308 keep method and body | the same | the same | same | `test_redirects` |
| H35 | https -> http redirect | curl follows by default | refused | different-but-correct (stricter) | `test_https` |
| H36 | Authorization / Cookie to another host, port or scheme (CVE-2018-1000007, CVE-2022-27774, CVE-2022-27776) | dropped | dropped (also Proxy-Authorization) | same | `test_redirects` |
| H37 | Caller's `Host` on a redirect to another origin | curl uses it only for the first origin | sent to the new server | **bug** -> dropped | `test_redirects`; 33b853d |
| H38 | Location with spaces / raw UTF-8 | curl and browsers percent-encode | redirect FAILED as a bad URL | **gap** -> encoded | `test_resolve`; b45f679 |
| H39 | Location to ws:, javascript:, controls; 3xx without Location | refused / is the response | the same | same | `test_resolve`, `test_redirects` |
| H40 | Relative Location with `.` / `..` segments (RFC 3986 5.2.4) | curl removes them | sent as they are (`// ponytail:`) | known limit | - |
| H41 | Read every response header line, repeated names | `curl_easy_nextheader` | only the first of a name | **gap** -> `br_http_header_count` / `_get_at` | `test_get`; c6c2151 |

## WebSocket checklist

| # | Rule | Reference | Ours | Verdict | Test / commit |
|---|---|---|---|---|---|
| W1 | Key: 16 random bytes, base64; `Sec-WebSocket-Version: 13`, `Upgrade`, `Connection: Upgrade` (RFC 6455 4.1) | the same | key from CTR-DRBG | same | `test_echo`, `test_write_handshake` |
| W2 | Status other than 101 fails | lws, curl the same | the same; the status in the error | same | `test_handshake_failures` |
| W3 | `Upgrade: websocket` (any case), `Connection` has the token `Upgrade` | lws the same | the same | same | `test_handshake_checks` |
| W4 | `Sec-WebSocket-Accept` exact; missing or repeated fails | lws checks; curl 01798a0 has the check as a comment only | checked | same as lws | `test_handshake_checks` |
| W5 | Extensions not offered / protocol not offered or several fail | lws checks; curl does not | checked | same as lws | `test_handshake_failures` |
| W6 | The chosen subprotocol can be read back | lws, curl expose it | not readable | **gap** -> `br_websocket_protocol` | `test_echo`; 7c64480 |
| W7 | Frames in the same read as the 101 | delivered | delivered | same | `test_echo` |
| W8 | Handshake bounded in size and time | the same | 64 KB / 128 lines, 30 s | same | `test_timeouts` |
| W9 | 3xx on the upgrade | lws follows | FAILED | different-but-correct (not implemented) | `test_handshake_failures` |
| W10 | Every client frame masked, fresh random mask (curl CVE-2025-10148: predictable mask) | the same | CTR-DRBG per frame | same | `test_echo`, `test_rfc_examples` |
| W11 | Masked server frame; RSV without extension; opcodes 3-7, B-F (Autobahn 3, 4) | 1002 | 1002 (wslay) | same | `test_protocol_errors` |
| W12 | Control frame > 125 bytes or fragmented (Autobahn 2.5, 5.1-5.2) | 1002 | 1002 | same | `test_protocol_errors` |
| W13 | Length not in its shortest form; 2^63 | lws tolerates non-minimal | 1002 (wslay) | different-but-correct (strict) | `test_protocol_errors` |
| W14 | Continuation without start; data frame inside a fragmented message; control frames between fragments (Autobahn 5) | the same | the same | same | `test_protocol_errors`, `test_echo` |
| W15 | Ping answered with the same payload; unsolicited pong ignored (Autobahn 2) | the same | the same | same | `test_round_trips`, `test_echo` |
| W16 | Ping flood from a server that reads nothing (curl CVE-2026-11586; lws and curl keep one pending pong) | bounded | send queue grew without end | **bug** -> `BR_WEBSOCKET_MAX_UNSENT` | `test_not_reading`; f2038bc, 52ac2c9 |
| W17 | Text UTF-8 checked as it arrives, across fragments, fail fast (Autobahn 6) | 1007 | 1007 (wslay) | same | `test_protocol_errors` |
| W18 | Message size limit; unread queue limit (Autobahn 9 class) | 1009 | 1009; 1008 for the unread queue | same | `test_violations`, `test_unread_limit` |
| W19 | Close echoed with the peer's code; CLOSED after both closes (RFC 6455 5.5.1, 7.1) | the same | the same | same | `test_closing`, `test_echo` |
| W20 | Close body of 1 byte (Autobahn 7.3.2) | 1002 | 1002 (ours: wslay reads it as no code) | same | `test_protocol_errors` |
| W21 | Close codes 0, 999, 1004-1006, 1016, 1100, 2000, 2999, 5000 (Autobahn 7.9) | 1002 | 1002 | same | `test_protocol_errors` |
| W22 | Close codes 1000-1003, 1007-1011, 3000-4999 (Autobahn 7.7) | accepted | accepted | same | `test_closing` |
| W23 | Close codes 1012-1014 (IANA, after RFC 6455; Autobahn does not test them as invalid) | lws accepts up to 1015; curl does not validate | wslay 1.1.1 failed them with 1002 | **gap** -> accepted (wslay patch) | `test_closing`; bd71db3 |
| W24 | Close code 1015 (RFC 6455 7.4.1: never in a frame) | lws accepts | 1002 | different-but-correct | `test_protocol_errors` |
| W25 | Close reason UTF-8 (Autobahn 7.5.1) | 1007 | 1007 | same | `test_protocol_errors` |
| W26 | Close frame split over two reads | checked once | wslay skipped 2 bytes of every piece: a valid multi-byte reason failed 1007, 2 bytes went unchecked | **bug** -> fixed (wslay patch) | `test_closing`; bd71db3 |
| W27 | Data received after our close, before the reply | delivered | delivered | same | `test_closing` |
| W28 | No close reply: timeout | lws times out | 5 s, then 1006 | same | `test_timeouts` |
| W29 | Who closes TCP after the closing handshake (RFC 6455 7.1.1: server SHOULD) | lws closes after the ack | the client closes at once | different-but-correct | `test_client_close` |
| W30 | Drop without a close frame -> 1006; protocol error -> close frame then TCP close (7.1.7) | the same | the same | same | `test_dropped`, `test_violations` |
| W31 | Keep-alive ping and dead-peer detection, also over TLS | lws ping-pong timer | the same; wss now tested | same | `test_keep_alive`, `test_wss`; 4cf7e37 |
| W32 | No `NULL + 0` (UB) sending an empty message (wslay master 1c97e7a) | fixed upstream after 1.1.1 | reached by our keep-alive ping | **bug** (UB) -> backported | `test_keep_alive`; bd71db3 |
| W33 | Empty messages both ways; no endless loop on any frame (curl CVE-2025-5399: a crafted frame trapped libcurl in a busy loop) | - | each frame consumes at least 2 bytes; empty messages work | same | `test_echo`, `test_round_trips` |

**Counts:** HTTP 41 items: 23 same, 10 different-but-correct, 2 known limits, 3 gaps (H10, H38,
H41), 3 bugs (H25, H31, H37). WebSocket 33 items: 24 same, 4 different-but-correct, 2 gaps (W6,
W23), 3 bugs (W16, W26, W32). In total 74: 47 same, 14 different-but-correct, 2 known limits,
5 gaps, 6 bugs, all gaps and bugs fixed.

## wslay after release-1.1.1

28 commits on master (last 0e7d106, 2022-08-25). Those that change `lib/`:

| Commit | What | Affects us |
|---|---|---|
| 1c97e7a | Fix errors with ubsan (`NULL + 0` when sending an empty message) | yes: our keep-alive pings have no payload; backported |
| 4cd83f7 | Extraneous `;` after a block (a warning) | no (compiled with warnings off) |
| 24c2a15, f22f312, c8e61b2, 865799f, 0f103ca, 1d9fd61 | fewer mallocs, intrusive queue, `wslay_stack` removed | no (refactoring, no behaviour change) |
| 30fb19f | new `wslay_event_write` API | no (feature) |
| 2fa72ec, 6dcd3af | clang-format | no |

The rest is build, CI, docs and examples. Open issue #72 (a close sent with `wslay_event_queue_msg`
and a reason is not seen by the peer): we close with `wslay_event_queue_close`, not affected.
Neither the close-code range nor the split close-reason check is fixed on master; both are our
patches (marked `braty patch` in `lib/wslay_event.c`, listed in `third_party/README.md`).

## The seven open points

1. **Close codes 1012-1014:** accepted. libwebsockets accepts 1012-1015, curl passes any code to
   the application, Autobahn (7.9) no longer lists them as invalid. No hook in wslay's API sees the
   code before it is rejected, so `wslay_event_is_valid_status_code` is patched (one comparison);
   `br_websocket_close` may send them too. 1015 stays invalid (RFC 6455 7.4.1). Browsers were not
   checked from their sources.
2. **Close reason in pieces:** a real problem, not only two unchecked bytes: the skipped bytes leave
   the UTF-8 decoder mid-character, so a valid close reason with a multi-byte character split at a
   read boundary failed the connection with 1007. Not fixed on master; patched (skip only the code
   bytes of the frame).
3. **ubsan `NULL + 0`:** our usage reaches it (every keep-alive ping, every empty binary message);
   harmless with every compiler we use, but undefined; upstream's 4-line fix is backported.
4. **`br_websocket_protocol`:** added, with test and README row.
5. **`br_http_header_count` / `br_http_header_get_at`:** added, with tests and README row.
6. **Body without a length cut without close_notify:** curl fails it on every TLS backend; ours
   now does too, through `br_tls_closed_cleanly` (tests in `tls` and `http`). WebSocket needs no
   change: its end is the close frame, and a drop is 1006 either way.
7. **Keep-alive over `wss://`:** tested (answered pings keep it OPEN; silence drops it as 1006).

## Classes of published vulnerabilities: are we exposed?

| Class | Examples | Exposed? |
|---|---|---|
| Request smuggling inputs (TE + CL, duplicate CL, bare LF / CR, chunk-ext tricks) | llhttp and proxy CVEs | No: one response per connection, then it is closed; CL / TE rules above; refused on the request side (H7, H10) |
| Header injection (CRLF in URL, method, headers) | a CR or LF in a caller's URL or header value starts a new header line (a common HTTP-client bug class) | No: refused before sending (H7) |
| Redirect credential leaks | curl CVE-2018-1000007, CVE-2022-27774, CVE-2022-27776, CVE-2025-14524 | No after H37: credentials and Host dropped on any origin change; https -> http refused |
| Integer overflow in lengths | - | Was (H25, only with `max_body = SIZE_MAX`); fixed. CL / chunk sizes checked before use; wslay rejects 2^63 lengths |
| Unbounded buffering | curl CVE-2023-38039 (headers), CVE-2026-11586 (auto-pong) | Was (W16); fixed. Header block, body, message, unread queue and unsent queue are bounded |
| Close-frame handling | wslay W26, W23 | Was; fixed by the patches |
| Predictable masks / keys | curl CVE-2025-10148 | No: CTR-DRBG |
| TLS truncation | - | Was (H31); fixed |

No CVE or advisory is published for picohttpparser or wslay (their GitHub security advisories are
empty; issue trackers searched). picohttpparser issue #89 (a crash on a NULL buffer) does not apply:
we call it only with a non-empty buffer of our own. The picohttpparser hardening commits (bare LF in chunk lines, chunk overhead,
strict chunk sizes, status range) are all in the vendored commit.

## Deliberately not implemented

Keep-alive connections (one connection per request), gzip / deflate (`Accept-Encoding: identity`),
HTTP/2, proxies, cookies, `Expect: 100-continue` waits, chunked request bodies, response trailers,
following redirects of the WebSocket upgrade, permessage-deflate and other extensions,
fragmenting sent messages, a WebSocket server.

## Known limits that remain

| Limit | Why it is acceptable |
|---|---|
| Relative redirects keep `.` / `..` segments (H40) | Servers resolve them; the host cannot change that way |
| No IDN, no IPv6 zone ids, no URL userinfo (H2-H4) | Games talk to their own ASCII hosts; userinfo is an `Authorization` header |
| Plain `http://` bodies without a length end at any TCP close | There is nothing better on an unauthenticated stream; servers that send a length (all real HTTP/1.1 servers) are exact |
| Credentials dropped on a cross-origin hop stay dropped if the chain returns to the first origin | Safer than curl's restore; a game that needs them calls again |
| The client closes TCP right after the closing handshake (W29) | Puts TIME_WAIT on the client's ephemeral port, which costs nothing for a game |
| wslay mallocs per received frame and queued message (libc) | Bounded by the message limit; documented in the websocket README |
| `BR_WEBSOCKET_MAX_UNSENT` counts what SDL_net still queues, not the OS socket buffer | The OS buffer is bounded by the OS; ours is what grew |
| A send refused at `BR_WEBSOCKET_MAX_UNSENT` is not retried for the game | The game sees false and decides (the server is not reading) |

## Not verified

Browser behaviour for close codes 1012-1014 (not read from Chromium / Firefox sources); the curl
commit that fixed CVE-2026-11586 (its advisory names the version, not the mechanism; curl master
keeps one pending control frame, read in `ws_enc_add_cntrl`). Nothing was run against a real
Autobahn fuzzing server; its case list was used as a checklist only.

Every CVE identifier above was checked on 2026-11-22 against curl's advisory list
(https://curl.se/docs/vuln.json, the data behind curl.se/docs/security.html): each exists and its
summary matches the row ("HTTP headers eat all memory", "HTTP authentication leak in redirects",
"Credential leak on redirect", "Auth/cookie leak on redirect", "bearer token leak on
cross-protocol redirect", "predictable WebSocket mask", "WebSocket endless loop", "WS Auto-PONG
memory exhaustion").
