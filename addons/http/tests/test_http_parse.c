/* http's pure parts (br_http_internal.h), no sockets: URLs and redirects, the request writer and
 * its injection checks, and the response parser around picohttpparser - pieces vs whole, chunked
 * with extensions and trailers, limits (header block, body, chunk size), Content-Length rules,
 * Transfer-Encoding with Content-Length, bodiless responses, interim 1xx, 101 with bytes after it,
 * obs-fold, control characters, truncation at every byte. */
#include "braty/braty.h"
#include "braty/br_http.h"
#include "br_http_internal.h"
#include "br_test.h"

#include <SDL3/SDL.h>

static bool equal(const char *a, const char *b)
{
    return a != NULL && b != NULL && SDL_strcmp(a, b) == 0;
}

/* ---- URLs */

static void test_urls(void)
{
    br_http_url u;
    bool ws = true;
    char error[256];
    BR_CHECK(br_http_url_split("http://example.com", &u, &ws, error, sizeof error));
    BR_CHECK(!u.secure && u.port == 80 && equal(u.host, "example.com") && equal(u.path, "/") && !ws);
    BR_CHECK(br_http_url_split("https://example.com:8443/a/b?c=1#frag", &u, &ws, error, sizeof error));
    BR_CHECK(u.secure && u.port == 8443 && equal(u.path, "/a/b?c=1") && !ws);
    BR_CHECK(br_http_url_split("ws://chat.example/socket", &u, &ws, error, sizeof error));
    BR_CHECK(!u.secure && u.port == 80 && equal(u.path, "/socket") && ws);
    BR_CHECK(br_http_url_split("wss://chat.example", &u, &ws, error, sizeof error));
    BR_CHECK(u.secure && u.port == 443 && equal(u.path, "/") && ws);
    BR_CHECK(br_http_url_parse("HTTPS://Example.com", &u) && u.secure);  /* the scheme ignores case */
    BR_CHECK(br_http_url_parse("http://[::1]:8080/p", &u));
    BR_CHECK(equal(u.host, "::1") && u.port == 8080 && equal(u.path, "/p"));
    BR_CHECK(br_http_url_parse("http://[::1]", &u) && equal(u.host, "::1") && u.port == 80);
    BR_CHECK(br_http_url_parse("http://h?q=1", &u) && equal(u.path, "/?q=1"));
    BR_CHECK(br_http_url_parse("http://h:/x", &u) && u.port == 80);  /* empty port = default */
    BR_CHECK(br_http_url_parse("http://h:65535", &u) && u.port == 65535);

    BR_CHECK(!br_http_url_parse("example.com/x", &u));           /* no scheme */
    BR_CHECK(!br_http_url_parse("ftp://example.com", &u));
    BR_CHECK(!br_http_url_parse("http://", &u));                 /* empty host */
    BR_CHECK(!br_http_url_parse("http:///path", &u));
    BR_CHECK(!br_http_url_parse("http://h:70000", &u));
    BR_CHECK(!br_http_url_parse("http://h:0", &u));
    BR_CHECK(!br_http_url_parse("http://h:8x", &u));
    BR_CHECK(!br_http_url_parse("http://h:99999999999", &u));
    BR_CHECK(!br_http_url_parse("http://[::1", &u));
    BR_CHECK(!br_http_url_parse("http://[::1]x/", &u));
    BR_CHECK(!br_http_url_parse("http://user:pw@h/", &u));
    BR_CHECK(!br_http_url_parse("http://h/a b", &u));
    BR_CHECK(!br_http_url_parse("http://h/a\r\nX-Injected: 1", &u));
    BR_CHECK(!br_http_url_parse("http://h\r\nX: 1/", &u));
    BR_CHECK(!br_http_url_parse(NULL, &u));

    char long_host[300];
    SDL_strlcpy(long_host, "http://", sizeof long_host);
    for (int i = 0; i < 256; i++) {
        SDL_strlcat(long_host, "a", sizeof long_host);
    }
    BR_CHECK(!br_http_url_parse(long_host, &u));
    long_host[7 + 255] = '\0';  /* 255 bytes fit */
    BR_CHECK(br_http_url_parse(long_host, &u));

    static char long_path[2100];
    SDL_strlcpy(long_path, "http://h/", sizeof long_path);
    while (SDL_strlen(long_path) < 2060) {
        SDL_strlcat(long_path, "p", sizeof long_path);
    }
    BR_CHECK(!br_http_url_parse(long_path, &u));
}

static void test_resolve(void)
{
    br_http_url base, out;
    char error[256];
    BR_CHECK(br_http_url_parse("https://a.example:8443/dir/page?x=1", &base));
    BR_CHECK(br_http_url_resolve(&base, "http://b.example/new", &out, error, sizeof error));
    BR_CHECK(!out.secure && equal(out.host, "b.example") && out.port == 80 && equal(out.path, "/new"));
    BR_CHECK(br_http_url_resolve(&base, "//c.example/x", &out, error, sizeof error));
    BR_CHECK(out.secure && equal(out.host, "c.example") && out.port == 443 && equal(out.path, "/x"));
    BR_CHECK(br_http_url_resolve(&base, "/top?y=2", &out, error, sizeof error));
    BR_CHECK(equal(out.host, "a.example") && out.port == 8443 && equal(out.path, "/top?y=2"));
    BR_CHECK(br_http_url_resolve(&base, "?z=3", &out, error, sizeof error) && equal(out.path, "/dir/page?z=3"));
    BR_CHECK(br_http_url_resolve(&base, "other", &out, error, sizeof error) && equal(out.path, "/dir/other"));
    BR_CHECK(br_http_url_resolve(&base, "#f", &out, error, sizeof error) && equal(out.path, "/dir/page?x=1"));
    BR_CHECK(!br_http_url_resolve(&base, "wss://a.example/", &out, error, sizeof error));
    BR_CHECK(!br_http_url_resolve(&base, "javascript:alert(1)", &out, error, sizeof error));
    BR_CHECK(!br_http_url_resolve(&base, "/a b", &out, error, sizeof error));
    BR_CHECK(br_http_url_parse("http://[::1]:8080/a", &base));
    BR_CHECK(br_http_url_resolve(&base, "/b", &out, error, sizeof error));
    BR_CHECK(equal(out.host, "::1") && out.port == 8080 && equal(out.path, "/b"));
}

/* ---- requests */

static void test_writer(void)
{
    br_http_url u;
    char out[1024];
    char error[256];
    BR_CHECK(br_http_url_parse("http://example.com/x?y=1", &u));
    size_t n = br_http_write_request(out, sizeof out, NULL, &u, NULL, false, 0, error, sizeof error);
    static const char get[] = "GET /x?y=1 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: braty\r\n"
                              "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    BR_CHECK(n == sizeof get - 1 && equal(out, get));

    /* the size it needs when out is too small, then the same text */
    BR_CHECK(br_http_write_request(NULL, 0, NULL, &u, NULL, false, 0, error, sizeof error) == n);
    BR_CHECK(br_http_write_request(out, 10, NULL, &u, NULL, false, 0, error, sizeof error) == n);

    /* the caller's names win; non-default port and IPv6 in Host; the length when asked */
    BR_CHECK(br_http_url_parse("https://[::1]:8443/p", &u));
    n = br_http_write_request(out, sizeof out, "POST", &u, "user-agent: game/1\r\nX-A: 1", true, 5, error,
                              sizeof error);
    static const char post[] = "POST /p HTTP/1.1\r\nHost: [::1]:8443\r\nuser-agent: game/1\r\nX-A: 1\r\n"
                               "Accept-Encoding: identity\r\nConnection: close\r\nContent-Length: 5\r\n\r\n";
    BR_CHECK(n == sizeof post - 1 && equal(out, post));
    BR_CHECK(br_http_headers_has("A: 1\r\nhost: x\r\n", "Host"));
    BR_CHECK(!br_http_headers_has("A: 1\r\nX-Host: x\r\n", "Host"));

    /* header injection through the caller's own method, headers or URL fails the request */
    BR_CHECK(br_http_write_request(out, sizeof out, "GET / HTTP/1.1\r\nX: y\r\n", &u, NULL, false, 0, error, sizeof error) == 0);
    BR_CHECK(br_http_write_request(out, sizeof out, "G T", &u, NULL, false, 0, error, sizeof error) == 0);
    BR_CHECK(br_http_write_request(out, sizeof out, "", &u, NULL, false, 0, error, sizeof error) == 0);
    BR_CHECK(br_http_write_request(out, sizeof out, "AVERYLONGMETHODNAMEOVERTHIRTYONEBYTES", &u, NULL, false, 0, error,
                                   sizeof error) == 0);
    BR_CHECK(br_http_write_request(out, sizeof out, "CONNECT", &u, NULL, false, 0, error, sizeof error) == 0);
    static const char *const bad_headers[] = {
        "X: a\r\nY: b\nZ: c\r\n",            /* lone LF */
        "X: a\rY: b\r\n",                    /* lone CR */
        "X: a\r\n\r\nGET /evil HTTP/1.1\r\n", /* an empty line ends the head early */
        ": v\r\n",                           /* no name */
        "X Y: v\r\n",                        /* a space in the name */
        "X: \x01\r\n",                       /* a control character */
        " folded\r\n",                       /* obs-fold */
        "X: a\r\n\n",
    };
    for (size_t i = 0; i < BR_ARRAY_COUNT(bad_headers); i++) {
        BR_CHECK(!br_http_headers_valid(bad_headers[i], error, sizeof error));
        BR_CHECK(br_http_write_request(out, sizeof out, NULL, &u, bad_headers[i], false, 0, error, sizeof error) == 0);
    }
    BR_CHECK(br_http_headers_valid("X: a\tb\r\nY:\r\n", error, sizeof error));
    br_http_url evil = u;
    SDL_strlcpy(evil.path, "/a\r\nX: 1", sizeof evil.path);
    BR_CHECK(br_http_write_request(out, sizeof out, NULL, &evil, NULL, false, 0, error, sizeof error) == 0);
    evil = u;
    SDL_strlcpy(evil.host, "h\r\nX: 1", sizeof evil.host);
    BR_CHECK(br_http_write_request(out, sizeof out, NULL, &evil, NULL, false, 0, error, sizeof error) == 0);
}

/* ---- responses */

/* Feeds text (size bytes) in pieces of `piece` bytes (0 = all at once). */
static br_http_parse_result feed(br_http_parser *p, const char *text, size_t size, size_t piece)
{
    br_http_parse_result r = br_http_parser_result(p);
    if (piece == 0) {
        return br_http_parser_feed(p, text, size);
    }
    for (size_t at = 0; at < size; at += piece) {
        r = br_http_parser_feed(p, text + at, size - at < piece ? size - at : piece);
    }
    return r;
}

/* A whole response through a new parser: its result (the parser is left in *out, or freed). */
static br_http_parse_result parse(const char *text, size_t size, size_t max_body, bool head, size_t piece,
                                  br_http_parser **out)
{
    br_http_parser *p = br_http_parser_create(max_body, head);
    br_http_parse_result r = feed(p, text, size, piece);
    if (out != NULL) {
        *out = p;
    } else {
        br_http_parser_destroy(p);
    }
    return r;
}

static br_http_parse_result parse_text(const char *text)
{
    return parse(text, SDL_strlen(text), 0, false, 0, NULL);
}

/* The response ends with the close (no length): DONE only after finish. */
static br_http_parse_result parse_and_close(const char *text, size_t max_body)
{
    br_http_parser *p = NULL;
    parse(text, SDL_strlen(text), max_body, false, 0, &p);
    br_http_parse_result r = br_http_parser_finish(p);
    br_http_parser_destroy(p);
    return r;
}

static const char simple[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Twice: one\r\nContent-Length: 11\r\n"
                             "x-twice: two\r\nEmpty:\r\n\r\nhello world";
static const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                              "5;name=value;other\r\nhello\r\n6 ; ext\r\n world\r\n0\r\nTrailer-A: x\r\nTrailer-B: y\r\n\r\n";

static void test_pieces(void)
{
    br_http_parser *whole = NULL, *bytes = NULL;
    BR_CHECK(parse(simple, sizeof simple - 1, 0, false, 0, &whole) == BR_HTTP_PARSE_DONE);
    BR_CHECK(parse(simple, sizeof simple - 1, 0, false, 1, &bytes) == BR_HTTP_PARSE_DONE);
    BR_CHECK(br_http_parser_status(whole) == 200 && br_http_parser_status(bytes) == 200);
    size_t a = 0, b = 0;
    BR_CHECK(equal(br_http_parser_body(whole, &a), "hello world") && a == 11);
    BR_CHECK(equal(br_http_parser_body(bytes, &b), "hello world") && b == 11);
    BR_CHECK(br_http_parser_header_count(whole) == 5 && br_http_parser_header_count(bytes) == 5);
    for (uint32_t i = 0; i < br_http_parser_header_count(whole); i++) {
        const char *n1 = NULL, *v1 = NULL, *n2 = NULL, *v2 = NULL;
        BR_CHECK(br_http_parser_header_at(whole, i, &n1, &v1) && br_http_parser_header_at(bytes, i, &n2, &v2));
        BR_CHECK(equal(n1, n2) && equal(v1, v2));
    }
    /* lookup ignores case; the first of a repeated header */
    BR_CHECK(equal(br_http_parser_header(whole, "content-type"), "text/plain"));
    BR_CHECK(equal(br_http_parser_header(bytes, "CONTENT-TYPE"), "text/plain"));
    BR_CHECK(equal(br_http_parser_header(whole, "X-TWICE"), "one"));
    BR_CHECK(equal(br_http_parser_header(whole, "Empty"), ""));
    BR_CHECK(br_http_parser_header(whole, "Missing") == NULL);
    BR_CHECK(br_http_parser_progress(whole) == 1.0f);
    /* more bytes after DONE are ignored */
    BR_CHECK(br_http_parser_feed(whole, "junk", 4) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_body(whole, NULL), "hello world"));
    br_http_parser_destroy(whole);
    br_http_parser_destroy(bytes);

    /* progress while the body arrives */
    br_http_parser *p = br_http_parser_create(0, false);
    BR_CHECK(br_http_parser_progress(p) == 0.0f);
    BR_CHECK(feed(p, simple, sizeof simple - 1 - 5, 0) == BR_HTTP_PARSE_MORE);
    BR_CHECK(br_http_parser_status(p) == 200);
    BR_CHECK(br_http_parser_progress(p) > 0.5f && br_http_parser_progress(p) < 0.6f);
    br_http_parser_destroy(p);
}

static void test_chunked(void)
{
    for (size_t piece = 0; piece <= 3; piece++) {
        br_http_parser *p = NULL;
        BR_CHECK(parse(chunked, sizeof chunked - 1, 0, false, piece, &p) == BR_HTTP_PARSE_DONE);
        size_t size = 0;
        BR_CHECK(equal(br_http_parser_body(p, &size), "hello world") && size == 11);
        BR_CHECK(br_http_parser_progress(p) == 1.0f);
        br_http_parser_destroy(p);
    }
    /* chunk sizes over max_body or over a size_t, a size with no digits, junk after the size */
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\nabc") ==
             BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n;x\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5x\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX") == BR_HTTP_PARSE_ERROR);
    static const char over[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\n";
    BR_CHECK(parse(over, sizeof over - 1, 5, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    static const char sum[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\n";
    BR_CHECK(parse(sum, sizeof sum - 1, 5, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse(sum, sizeof sum - 1, 6, false, 0, NULL) == BR_HTTP_PARSE_MORE);

    /* Transfer-Encoding with Content-Length: Transfer-Encoding wins (RFC 9112 section 6.3) */
    br_http_parser *p = NULL;
    static const char both[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "5\r\nhello\r\n0\r\n\r\n";
    BR_CHECK(parse(both, sizeof both - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_body(p, NULL), "hello"));
    br_http_parser_destroy(p);
    /* codings we did not ask for */
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nxx") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n") ==
             BR_HTTP_PARSE_ERROR);
}

static void test_limits(void)
{
    /* a header block over 64 KB: whole, and in pieces */
    static char big[BR_HTTP_MAX_HEADER_BYTES + 200];
    SDL_strlcpy(big, "HTTP/1.1 200 OK\r\nX-Big: ", sizeof big);
    size_t at = SDL_strlen(big);
    while (at < BR_HTTP_MAX_HEADER_BYTES + 10) {
        big[at++] = 'a';
    }
    SDL_memcpy(big + at, "\r\n\r\n", 4);
    at += 4;
    BR_CHECK(parse(big, at, 0, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse(big, at, 0, false, 1000, NULL) == BR_HTTP_PARSE_ERROR);
    /* a never-ending header block stops at the limit too */
    br_http_parser *p = br_http_parser_create(0, false);
    br_http_parse_result r = feed(p, big, BR_HTTP_MAX_HEADER_BYTES + 10, 4096);
    BR_CHECK(r == BR_HTTP_PARSE_ERROR);
    BR_CHECK(SDL_strstr(br_http_parser_error(p), "header") != NULL);
    br_http_parser_destroy(p);

    /* more headers than BR_HTTP_MAX_HEADERS */
    static char many[BR_HTTP_MAX_HEADERS * 12 + 64];
    SDL_strlcpy(many, "HTTP/1.1 200 OK\r\n", sizeof many);
    for (int i = 0; i <= BR_HTTP_MAX_HEADERS; i++) {
        SDL_strlcat(many, "X-H: v\r\n", sizeof many);
    }
    SDL_strlcat(many, "\r\n", sizeof many);
    BR_CHECK(parse_text(many) == BR_HTTP_PARSE_ERROR);

    /* a body over max_body: by Content-Length (before it arrives), and without a length */
    static const char hundred[] = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
    BR_CHECK(parse(hundred, sizeof hundred - 1, 10, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse(hundred, sizeof hundred - 1, 100, false, 0, NULL) == BR_HTTP_PARSE_MORE);
    BR_CHECK(parse_and_close("HTTP/1.1 200 OK\r\n\r\n0123456789", 10) == BR_HTTP_PARSE_DONE);
    BR_CHECK(parse_and_close("HTTP/1.1 200 OK\r\n\r\n0123456789A", 10) == BR_HTTP_PARSE_ERROR);

    /* endless interim responses count against the header limit */
    p = br_http_parser_create(0, false);
    r = BR_HTTP_PARSE_MORE;
    for (int i = 0; i < 5000 && r == BR_HTTP_PARSE_MORE; i++) {
        r = br_http_parser_feed(p, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    BR_CHECK(r == BR_HTTP_PARSE_ERROR);
    br_http_parser_destroy(p);
}

static void test_content_length(void)
{
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 1 2\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc") == BR_HTTP_PARSE_ERROR);
    br_http_parser *p = NULL;
    static const char same[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab";
    BR_CHECK(parse(same, sizeof same - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_body(p, NULL), "ab"));
    br_http_parser_destroy(p);
    /* bytes after the body are not part of it */
    static const char extra[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nabEXTRA";
    BR_CHECK(parse(extra, sizeof extra - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    size_t size = 0;
    BR_CHECK(equal(br_http_parser_body(p, &size), "ab") && size == 2);
    br_http_parser_destroy(p);
    /* zero length: DONE at the end of the headers */
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_DONE);
}

static void test_status_lines(void)
{
    BR_CHECK(parse_text("HTTP/1.1 OK\r\n\r\n") == BR_HTTP_PARSE_ERROR);       /* no code */
    BR_CHECK(parse_text("HTTP/1.1 \r\n\r\n\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 20 OK\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 2000 OK\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 600 Odd\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/2 200 OK\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("ICY 200 OK\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 404\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_DONE);  /* empty reason */
    br_http_parser *p = NULL;
    static const char old[] = "HTTP/1.0 200 OK\r\n\r\nuntil close";
    BR_CHECK(parse(old, sizeof old - 1, 0, false, 0, &p) == BR_HTTP_PARSE_MORE);
    BR_CHECK(br_http_parser_progress(p) == 0.0f);  /* no length */
    BR_CHECK(br_http_parser_finish(p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_body(p, NULL), "until close"));
    br_http_parser_destroy(p);
}

static void test_header_bytes(void)
{
    static const char nul[] = "HTTP/1.1 200 OK\r\nX: a\0b\r\nContent-Length: 0\r\n\r\n";
    BR_CHECK(parse(nul, sizeof nul - 1, 0, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    static const char nul_name[] = "HTTP/1.1 200 OK\r\nX\0Y: a\r\nContent-Length: 0\r\n\r\n";
    BR_CHECK(parse(nul_name, sizeof nul_name - 1, 0, false, 0, NULL) == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nX: a\rb\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nX: a\x01" "b\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nX : a\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nX(Y): a\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_ERROR);
    BR_CHECK(parse_text("HTTP/1.1 200 OK\r\nNo colon\r\nContent-Length: 0\r\n\r\n") == BR_HTTP_PARSE_ERROR);

    /* a lone LF ends a line (RFC 9112 section 2.2 lets a recipient accept it) */
    br_http_parser *p = NULL;
    static const char lf[] = "HTTP/1.1 200 OK\nX-A: 1\nContent-Length: 2\n\nok";
    BR_CHECK(parse(lf, sizeof lf - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_header(p, "X-A"), "1") && equal(br_http_parser_body(p, NULL), "ok"));
    br_http_parser_destroy(p);

    /* obs-fold becomes one space-separated value (RFC 9112 section 5.2) */
    static const char folded[] = "HTTP/1.1 200 OK\r\nX-Fold: first\r\n  second\r\n\tthird  \r\nX-B: b\r\n"
                                 "Content-Length: 0\r\n\r\n";
    BR_CHECK(parse(folded, sizeof folded - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_header(p, "X-Fold"), "first    second   third"));
    BR_CHECK(equal(br_http_parser_header(p, "X-B"), "b"));
    BR_CHECK(br_http_parser_header_count(p) == 3);
    br_http_parser_destroy(p);

    /* bytes over 0x7F in a value are obs-text: kept */
    static const char high[] = "HTTP/1.1 200 OK\r\nX-U: caf\xc3\xa9\r\nContent-Length: 0\r\n\r\n";
    BR_CHECK(parse(high, sizeof high - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_header(p, "x-u"), "caf\xc3\xa9"));
    br_http_parser_destroy(p);
}

static void test_no_body(void)
{
    br_http_parser *p = NULL;
    size_t size = 99;
    /* 204 and 304 have no body whatever Content-Length says */
    static const char no_content[] = "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n";
    BR_CHECK(parse(no_content, sizeof no_content - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(br_http_parser_status(p) == 204 && equal(br_http_parser_body(p, &size), "") && size == 0);
    br_http_parser_destroy(p);
    static const char not_modified[] = "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n";
    BR_CHECK(parse(not_modified, sizeof not_modified - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    br_http_parser_destroy(p);
    /* a response to HEAD */
    static const char head[] = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n";
    BR_CHECK(parse(head, sizeof head - 1, 10, true, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_header(p, "Content-Length"), "1000") && equal(br_http_parser_body(p, NULL), ""));
    br_http_parser_destroy(p);

    /* interim 100 and 103 before the real response */
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                  "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    for (size_t piece = 0; piece <= 1; piece++) {
        BR_CHECK(parse(interim, sizeof interim - 1, 0, false, piece, &p) == BR_HTTP_PARSE_DONE);
        BR_CHECK(br_http_parser_status(p) == 201 && equal(br_http_parser_body(p, NULL), "ok"));
        BR_CHECK(br_http_parser_header(p, "Link") == NULL);  /* the interim headers are gone */
        br_http_parser_destroy(p);
    }

    /* 101: DONE at the end of the headers; the bytes after them are unread */
    static const char upgrade[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "\r\n\x81\x02hi";
    BR_CHECK(parse(upgrade, sizeof upgrade - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(br_http_parser_status(p) == 101 && equal(br_http_parser_header(p, "upgrade"), "websocket"));
    const uint8_t *rest = br_http_parser_unread(p, &size);
    BR_CHECK(rest != NULL && size == 4 && rest[0] == 0x81 && rest[1] == 0x02 && rest[2] == 'h' && rest[3] == 'i');
    br_http_parser_destroy(p);
    BR_CHECK(parse(upgrade, sizeof upgrade - 5, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    BR_CHECK(br_http_parser_unread(p, &size) == NULL && size == 0);
    br_http_parser_destroy(p);
}

/* Every proper prefix of a complete response: MORE (or ERROR), then finish: ERROR, never DONE. */
static void check_truncated(const char *text, size_t size)
{
    int done = 0;
    for (size_t cut = 0; cut < size; cut++) {
        br_http_parser *p = br_http_parser_create(0, false);
        br_http_parse_result r = br_http_parser_feed(p, text, cut);
        if (r == BR_HTTP_PARSE_DONE || br_http_parser_finish(p) == BR_HTTP_PARSE_DONE) {
            done++;
        }
        br_http_parser_destroy(p);
    }
    BR_CHECK(done == 0);
    BR_CHECK(parse(text, size, 0, false, 0, NULL) == BR_HTTP_PARSE_DONE);
}

static void test_truncated(void)
{
    check_truncated(simple, sizeof simple - 1);
    check_truncated(chunked, sizeof chunked - 1);
    static const char empty_chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    check_truncated(empty_chunked, sizeof empty_chunked - 1);
    br_http_parser *p = br_http_parser_create(0, false);
    BR_CHECK(br_http_parser_finish(p) == BR_HTTP_PARSE_ERROR);
    BR_CHECK(SDL_strstr(br_http_parser_error(p), "closed") != NULL);
    BR_CHECK(br_http_parser_feed(p, simple, sizeof simple - 1) == BR_HTTP_PARSE_ERROR);  /* stays ERROR */
    br_http_parser_destroy(p);
}

static void test_reset(void)
{
    br_http_parser *p = NULL;
    BR_CHECK(parse(simple, sizeof simple - 1, 0, false, 0, &p) == BR_HTTP_PARSE_DONE);
    br_http_parser_reset(p, 0, false);
    BR_CHECK(br_http_parser_result(p) == BR_HTTP_PARSE_MORE && br_http_parser_status(p) == 0);
    BR_CHECK(br_http_parser_header(p, "Content-Type") == NULL);
    BR_CHECK(br_http_parser_feed(p, chunked, sizeof chunked - 1) == BR_HTTP_PARSE_DONE);
    BR_CHECK(equal(br_http_parser_body(p, NULL), "hello world"));
    br_http_parser_destroy(p);
    br_http_parser_destroy(NULL);
    BR_CHECK(br_http_parser_feed(NULL, "x", 1) == BR_HTTP_PARSE_ERROR);
}

int main(void)
{
    test_urls();
    test_resolve();
    test_writer();
    test_pieces();
    test_chunked();
    test_limits();
    test_content_length();
    test_status_lines();
    test_header_bytes();
    test_no_body();
    test_truncated();
    test_reset();
    return br_test_finish();
}
