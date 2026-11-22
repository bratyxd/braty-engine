/* br_http_internal.h - the pure parts of http, for its parse test and for `websocket`: the URL
 * parser with a reason, redirect resolution, the request writer and the response parser. None
 * of it touches a socket or the engine.
 *
 * The response parser wraps picohttpparser (status line, headers, chunked decoding) and enforces
 * what the library leaves to its caller: the header block and body limits, the body framing of
 * RFC 9112 section 6.3, interim 1xx responses, obs-fold.
 *
 *   br_http_parser *p = br_http_parser_create(max_body, false);
 *   ...for every piece of bytes read:
 *   br_http_parse_result r = br_http_parser_feed(p, bytes, n);   // MORE / DONE / ERROR
 *   ...when the peer closed:  r = br_http_parser_finish(p);       // DONE only when complete
 *   if (r == BR_HTTP_PARSE_DONE) { br_http_parser_status(p), _header(p, "Name"), _body(p, &size) }
 *   if (r == BR_HTTP_PARSE_ERROR) { br_http_parser_error(p) }
 *   br_http_parser_destroy(p);
 *
 * A 101 response (WebSocket upgrade) is DONE at the end of its headers, without a body; the
 * bytes the same feed carried after them are br_http_parser_unread. */
#ifndef BR_HTTP_INTERNAL_H
#define BR_HTTP_INTERNAL_H

#include "braty/br_http.h"

#include <stddef.h>

/* ---- URLs */

/* br_http_url_parse without logging: the reason goes to error (may be NULL). *websocket (may be
 * NULL) is true for ws:// and wss://. */
bool br_http_url_split(const char *url, br_http_url *out, bool *websocket, char *error, size_t error_capacity);
/* A redirect's Location (absolute "https://h/p", scheme-relative "//h/p", absolute path "/p",
 * query "?q" or relative "p") against the URL it came from. False with a reason when the result
 * is not a valid http(s) URL. */
bool br_http_url_resolve(const br_http_url *base, const char *location, br_http_url *out, char *error,
                         size_t error_capacity);

/* ---- requests */

/* True when `headers` ("Name: value\r\n" lines, NULL = none) is well formed: each name a token,
 * no CR, LF or NUL inside a value, no empty line; the last line may lack its CRLF. A reason
 * otherwise. */
bool br_http_headers_valid(const char *headers, char *error, size_t error_capacity);
/* True when `headers` (valid) has a line named `name` (case-insensitive). */
bool br_http_headers_has(const char *headers, const char *name);
/* Writes the request head into out (NUL-terminated): request line, Host, the caller's headers,
 * then User-Agent, Accept-Encoding: identity, Connection: close and Content-Length (when
 * send_length) unless the caller's headers name them. Returns its length without the NUL, or the
 * capacity it needs (length + 1 > capacity: call again with a bigger out; out may be NULL then).
 * 0 with a reason when method (a token, at most BR_HTTP_MAX_METHOD bytes; CONNECT refused) or
 * headers are not valid. */
size_t br_http_write_request(char *out, size_t capacity, const char *method, const br_http_url *url,
                             const char *headers, bool send_length, size_t body_size, char *error,
                             size_t error_capacity);

/* ---- responses */

typedef enum br_http_parse_result {
    BR_HTTP_PARSE_MORE,   /* need more bytes */
    BR_HTTP_PARSE_DONE,   /* a complete final response */
    BR_HTTP_PARSE_ERROR   /* malformed or over a limit: br_http_parser_error */
} br_http_parse_result;

typedef struct br_http_parser br_http_parser;

/* A parser for one response. max_body 0 = BR_HTTP_DEFAULT_MAX_BODY; head_request = the request
 * was HEAD (no body whatever the headers say). NULL when out of memory. */
br_http_parser *br_http_parser_create(size_t max_body, bool head_request);
/* Frees it (NULL is fine). */
void br_http_parser_destroy(br_http_parser *p);
/* Ready for the next response (a redirect), keeping the buffer. */
void br_http_parser_reset(br_http_parser *p, size_t max_body, bool head_request);
/* Takes the next bytes. After DONE or ERROR the bytes are ignored and the result repeats. */
br_http_parse_result br_http_parser_feed(br_http_parser *p, const void *data, size_t size);
/* The peer closed: DONE when the response was complete or its body runs until the close; else
 * ERROR (truncated). A truncated response is never DONE. */
br_http_parse_result br_http_parser_finish(br_http_parser *p);
/* MORE, DONE or ERROR, as the last feed / finish returned. */
br_http_parse_result br_http_parser_result(const br_http_parser *p);
/* The final response's status once its headers are complete (also while the body arrives), 0
 * before. */
int br_http_parser_status(const br_http_parser *p);
/* A header of the final response (case-insensitive, the first when repeated), NUL-terminated;
 * NULL when absent or before the headers are complete. Valid until reset / destroy. */
const char *br_http_parser_header(const br_http_parser *p, const char *name);
/* Number of header lines of the final response (obs-folded lines count once). */
uint32_t br_http_parser_header_count(const br_http_parser *p);
/* Name and value of header `index` (NUL-terminated); false out of range. */
bool br_http_parser_header_at(const br_http_parser *p, uint32_t index, const char **name, const char **value);
/* The body so far, NUL-terminated ("" when none); size may be NULL. Valid until the next feed. */
const char *br_http_parser_body(const br_http_parser *p, size_t *size);
/* Bytes fed after the end of a response without a body (101, 204, 304, HEAD), in the same feed
 * that completed it: for a 101, the first bytes of the new protocol. NULL / 0 otherwise. */
const void *br_http_parser_unread(const br_http_parser *p, size_t *size);
/* Body bytes / Content-Length; 0 when the length is unknown, 1 when DONE. */
float br_http_parser_progress(const br_http_parser *p);
/* Why it is ERROR ("" otherwise). */
const char *br_http_parser_error(const br_http_parser *p);

#endif
