/* br_http.h - the `http` add-on: an HTTP/1.1 client (http:// and https://) over `tls`.
 *
 *   br_http_register(e);                                       // once, in init (registers tls, net)
 *   g->req = br_http_get(e, "https://example.com/scores.json"); // returns at once: PENDING
 *   ...each frame, in a system:
 *   switch (br_http_request_state(e, g->req)) {
 *   case BR_HTTP_DONE:                                         // a complete response, any status
 *       if (br_http_status(e, g->req) == 200) {
 *           size_t size;
 *           const char *text = br_http_body(e, g->req, &size); // NUL-terminated
 *           use(text, size);
 *       }
 *       br_http_release(e, g->req);
 *       break;
 *   case BR_HTTP_FAILED: br_log_error("%s", br_http_error(e, g->req)); br_http_release(e, g->req); break;
 *   default: break;                                            // PENDING, or NONE (released)
 *   }
 *
 * One connection per request, closed when the response is complete. The system `http.update`
 * (INPUT phase, BR_HTTP_UPDATE_PRIORITY: right after `tls.update`) sends, receives and parses.
 * Responses are untrusted: every length is checked against a limit and anything malformed or
 * over a limit FAILS the request with a reason. Handles are {index, generation}; zero and stale
 * handles are safe everywhere (NONE, 0, NULL, ""). README: addons/http/README.md.
 */
#ifndef BR_HTTP_H
#define BR_HTTP_H

#include "braty/br_tls.h"

/* Requests in flight (pending or done, not yet released). One more: zero handle + log. */
#define BR_HTTP_MAX_REQUESTS 16
/* Largest response header block (status line + headers, each interim 1xx response included). */
#define BR_HTTP_MAX_HEADER_BYTES (64 * 1024)
/* Most header lines in one response. */
#define BR_HTTP_MAX_HEADERS 128
/* Response body bytes accepted when br_http_desc.max_body is 0. */
#define BR_HTTP_DEFAULT_MAX_BODY (16 * 1024 * 1024)
/* Seconds for the whole request (connect, redirects, body) when br_http_desc.timeout is 0. */
#define BR_HTTP_DEFAULT_TIMEOUT 30.0f
/* Redirects followed; one more fails the request. */
#define BR_HTTP_MAX_REDIRECTS 5
/* Longest method name (bytes, without the NUL). */
#define BR_HTTP_MAX_METHOD 31
/* Priority of the `http.update` system in BR_PHASE_INPUT (right after `tls.update`). */
#define BR_HTTP_UPDATE_PRIORITY (BR_TLS_UPDATE_PRIORITY + 1)

/* An HTTP request and, once DONE, its response. */
typedef struct br_http_request {
    uint32_t index;
    uint32_t generation;
} br_http_request;

typedef enum br_http_state {
    BR_HTTP_NONE,     /* zero / stale handle (or released) */
    BR_HTTP_PENDING,  /* connecting, sending, receiving, following redirects */
    BR_HTTP_DONE,     /* a complete response, any status */
    BR_HTTP_FAILED    /* no complete response: br_http_error says why */
} br_http_state;

/* A split URL. host has no brackets ("::1" for "[::1]"); path starts with '/' and keeps the
 * query ("/a?b=1"); the fragment is dropped. */
typedef struct br_http_url {
    bool     secure;      /* https:// or wss:// */
    char     host[256];
    uint16_t port;
    char     path[2048];
} br_http_url;

/* Splits http:// https:// ws:// wss:// URLs (default ports 80 / 443, path "/" when missing).
 * False (logged) for anything else: no scheme, empty or over-long host, port 0 or over 65535,
 * user info ("user@host"), spaces or control characters. */
bool br_http_url_parse(const char *url, br_http_url *out);

/* What br_http_send sends. Everything is copied: the pointers need to live only for the call. */
typedef struct br_http_desc {
    const char *method;     /* NULL = "GET"; "HEAD" responses have no body */
    const char *url;
    const char *headers;    /* "Name: value\r\n" lines, NULL = none */
    const void *body;       /* NULL = none */
    size_t      body_size;
    float       timeout;    /* seconds for the whole request, 0 = BR_HTTP_DEFAULT_TIMEOUT */
    size_t      max_body;   /* response bytes accepted, 0 = BR_HTTP_DEFAULT_MAX_BODY */
} br_http_desc;

/* Registers `tls` (and `net`) and adds the `http.update` system. Idempotent; false (logged) when
 * tls cannot start. */
bool br_http_register(br_engine *e);
/* Starts a request; returns at once in PENDING. Zero handle (logged) when not registered, the
 * table is full, or the URL, method or headers are not valid (a CR or LF in any of them, a header
 * line that is not "Name: value"). */
br_http_request br_http_send(br_engine *e, const br_http_desc *desc);
/* br_http_send of a GET with the defaults. */
br_http_request br_http_get(br_engine *e, const char *url);
/* PENDING, DONE (a complete response, any status), FAILED, or NONE for zero / stale handles. */
br_http_state br_http_request_state(br_engine *e, br_http_request r);
/* The final response's status (200, 404...); 0 until DONE. */
int br_http_status(br_engine *e, br_http_request r);
/* Value of the final response's header `name` (case-insensitive; the first when repeated),
 * NUL-terminated and valid until release. NULL when absent or not DONE. */
const char *br_http_header(br_engine *e, br_http_request r, const char *name);
/* Number of header lines of the final response (repeated names count each time, so every
 * Set-Cookie is reachable); 0 until DONE. */
uint32_t br_http_header_count(br_engine *e, br_http_request r);
/* Name and value of header line `index` (0..count-1, in the order received), NUL-terminated and
 * valid until release. False (and *name / *value NULL) out of range or not DONE. name and value
 * may be NULL. */
bool br_http_header_get_at(br_engine *e, br_http_request r, uint32_t index, const char **name, const char **value);
/* The response body, NUL-terminated (the NUL is not in *size); valid until release. NULL (and
 * *size 0) until DONE. size may be NULL. */
const void *br_http_body(br_engine *e, br_http_request r, size_t *size);
/* Body bytes received / Content-Length: 0 when unknown (no length, or not started), 1 when DONE. */
float br_http_progress(br_engine *e, br_http_request r);
/* Why it FAILED ("" otherwise, and for zero / stale handles); valid until release. */
const char *br_http_error(br_engine *e, br_http_request r);
/* Frees the request and its response; cancels (closes the connection of) a pending one. */
void br_http_release(br_engine *e, br_http_request r);

#endif
