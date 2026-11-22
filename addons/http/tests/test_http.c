/* http add-on on loopback, against canned-response servers in this process (br_net) and the
 * TLS test server of the tls add-on: GET with body and headers, POST body intact, chunked, 404,
 * a redirect chain of 2 followed and one of 6 FAILED, the server closing mid-body, a server that
 * never answers (timeout, real time), max_body, release while pending, the request table full,
 * bad arguments, zero / stale handles, https://localhost. Ports 47821..47823. */
#include "braty/braty.h"
#include "braty/br_http.h"
#include "br_test.h"
#include "tls_test_server.h"

#include <SDL3/SDL.h>

#define PORT 47821
#define TLS_PORT 47822
#define CLOSED_PORT 47823

static tls_test_server tls_server;
static tls_test_conn tls_conn;
static char last_request[8192];  /* what the canned server read last (head + body) */

static uint64_t deadline_in(double seconds)
{
    return SDL_GetTicksNS() + (uint64_t)(seconds * 1e9);
}

static bool before(uint64_t deadline)
{
    return SDL_GetTicksNS() < deadline;
}

static void step(br_engine *e)
{
    br_engine_step(e, 1.0f / 60.0f);
    SDL_Delay(1);
}

static bool contains(const char *text, const char *part)
{
    return text != NULL && SDL_strstr(text, part) != NULL;
}

/* True when the request in buf (NUL-terminated, size bytes) is complete: head + Content-Length. */
static bool request_complete(const char *buf, size_t size)
{
    const char *end = SDL_strstr(buf, "\r\n\r\n");
    if (end == NULL) {
        return false;
    }
    size_t head = (size_t)(end + 4 - buf);
    const char *length = SDL_strstr(buf, "Content-Length: ");
    size_t body = length != NULL && length < end ? (size_t)SDL_strtoull(length + 16, NULL, 10) : 0;
    return size >= head + body;
}

/* Steps until r leaves PENDING. Each connection to server gets, once its request is complete,
 * responses[k] for the k-th connection (the last one repeats); then the server closes it.
 * respond false: accept and read, never answer. */
static br_http_state serve(br_engine *e, br_http_request r, br_net_server server, const char *const *responses,
                           int count, bool respond)
{
    br_net_socket peer = { 0, 0 };
    size_t got = 0;
    int served = 0;
    uint64_t deadline = deadline_in(20.0);
    while (br_http_request_state(e, r) == BR_HTTP_PENDING && before(deadline)) {
        step(e);
        if (peer.generation == 0) {
            peer = br_net_accept(e, server);
            got = 0;
        }
        if (peer.generation == 0) {
            continue;
        }
        int n = br_net_receive(e, peer, last_request + got, (int)(sizeof last_request - 1 - got));
        if (n > 0) {
            got += (size_t)n;
            last_request[got] = '\0';
        }
        if (respond && request_complete(last_request, got)) {
            const char *response = responses[served < count ? served : count - 1];
            served++;
            BR_CHECK(br_net_send(e, peer, response, SDL_strlen(response)));
            br_net_close(e, peer);  /* queued bytes still go out */
            peer.generation = 0;
        }
    }
    if (peer.generation != 0) {
        br_net_close(e, peer);
    }
    return br_http_request_state(e, r);
}

static void test_not_registered(void)
{
    br_engine *e = br_engine_create("braty-test-http-unregistered", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_http_get(e, "http://127.0.0.1/").generation == 0);
    br_engine_destroy(e);
}

static void test_zero_handles(br_engine *e)
{
    br_http_request zero = { 0, 0 };
    br_http_request wild = { 999, 3 };
    size_t size = 7;
    BR_CHECK(br_http_request_state(e, zero) == BR_HTTP_NONE);
    BR_CHECK(br_http_request_state(e, wild) == BR_HTTP_NONE);
    BR_CHECK(br_http_status(e, zero) == 0);
    BR_CHECK(br_http_header(e, zero, "Host") == NULL);
    const char *name = "x", *value = "y";
    BR_CHECK(br_http_header_count(e, wild) == 0);
    BR_CHECK(!br_http_header_get_at(e, zero, 0, &name, &value) && name == NULL && value == NULL);
    BR_CHECK(br_http_body(e, wild, &size) == NULL && size == 0);
    BR_CHECK(br_http_progress(e, zero) == 0.0f);
    BR_CHECK(SDL_strcmp(br_http_error(e, wild), "") == 0);
    br_http_release(e, zero);
    br_http_release(e, wild);
}

static void test_bad_arguments(br_engine *e)
{
    BR_CHECK(br_http_send(e, NULL).generation == 0);
    BR_CHECK(br_http_get(e, NULL).generation == 0);
    BR_CHECK(br_http_get(e, "127.0.0.1/x").generation == 0);
    BR_CHECK(br_http_get(e, "ws://127.0.0.1/").generation == 0);
    BR_CHECK(br_http_get(e, "http://127.0.0.1/a\r\nX: injected").generation == 0);
    br_http_desc desc;
    SDL_zero(desc);
    desc.url = "http://127.0.0.1:47821/";
    desc.headers = "X-Ok: 1\r\nX-Evil: 1\r\nHost: other\r\n\r\nGET /smuggled HTTP/1.1\r\n";
    BR_CHECK(br_http_send(e, &desc).generation == 0);
    desc.headers = "X-Evil: 1\nX-More: 2";
    BR_CHECK(br_http_send(e, &desc).generation == 0);
    desc.headers = NULL;
    desc.method = "GET / HTTP/1.1\r\nX:";
    BR_CHECK(br_http_send(e, &desc).generation == 0);
    desc.method = NULL;
    desc.body_size = 4;  /* no body pointer */
    BR_CHECK(br_http_send(e, &desc).generation == 0);
}

static void test_get(br_engine *e, br_net_server server)
{
    static const char *const ok[] = {
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Game: braty\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Content-Length: 12\r\n\r\nhello, world"
    };
    br_http_request r = br_http_get(e, "http://127.0.0.1:47821/scores?top=10");
    BR_CHECK(r.generation != 0);
    BR_CHECK(br_http_request_state(e, r) == BR_HTTP_PENDING);
    BR_CHECK(br_http_status(e, r) == 0);
    BR_CHECK(serve(e, r, server, ok, 1, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 200);
    size_t size = 0;
    const char *body = br_http_body(e, r, &size);
    BR_CHECK(size == 12 && body != NULL && SDL_strcmp(body, "hello, world") == 0);
    BR_CHECK(contains(br_http_header(e, r, "content-type"), "text/plain"));
    BR_CHECK(contains(br_http_header(e, r, "X-GAME"), "braty"));
    BR_CHECK(br_http_header(e, r, "Missing") == NULL);
    /* every line, repeated names included, in order */
    BR_CHECK(br_http_header_count(e, r) == 5);
    const char *name = NULL, *value = NULL;
    BR_CHECK(br_http_header_get_at(e, r, 2, &name, &value));
    BR_CHECK(SDL_strcmp(name, "Set-Cookie") == 0 && SDL_strcmp(value, "a=1") == 0);
    BR_CHECK(br_http_header_get_at(e, r, 3, &name, &value));
    BR_CHECK(SDL_strcmp(name, "Set-Cookie") == 0 && SDL_strcmp(value, "b=2") == 0);
    BR_CHECK(br_http_header_get_at(e, r, 4, NULL, &value) && SDL_strcmp(value, "12") == 0);
    BR_CHECK(!br_http_header_get_at(e, r, 5, &name, &value) && name == NULL && value == NULL);
    BR_CHECK(br_http_progress(e, r) == 1.0f);
    BR_CHECK(SDL_strcmp(br_http_error(e, r), "") == 0);
    /* what went on the wire */
    BR_CHECK(SDL_strncmp(last_request, "GET /scores?top=10 HTTP/1.1\r\n", 29) == 0);
    BR_CHECK(contains(last_request, "\r\nHost: 127.0.0.1:47821\r\n"));
    BR_CHECK(contains(last_request, "\r\nUser-Agent: braty\r\n"));
    BR_CHECK(contains(last_request, "\r\nConnection: close\r\n"));
    BR_CHECK(contains(last_request, "\r\nAccept-Encoding: identity\r\n"));
    BR_CHECK(!contains(last_request, "Content-Length"));
    br_http_release(e, r);
    BR_CHECK(br_http_request_state(e, r) == BR_HTTP_NONE);
    BR_CHECK(br_http_body(e, r, &size) == NULL);
}

static void test_post(br_engine *e, br_net_server server)
{
    static const char *const created[] = { "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n" };
    static char body[3000];
    for (size_t i = 0; i < sizeof body; i++) {
        body[i] = (char)('a' + i % 26);
    }
    br_http_desc desc;
    SDL_zero(desc);
    desc.method = "POST";
    desc.url = "http://127.0.0.1:47821/submit";
    desc.headers = "Content-Type: application/octet-stream\r\nX-Player: 7\r\n";
    desc.body = body;
    desc.body_size = sizeof body;
    br_http_request r = br_http_send(e, &desc);
    body[0] = 'Z';  /* send copied it */
    BR_CHECK(serve(e, r, server, created, 1, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 201);
    size_t size = 99;
    BR_CHECK(br_http_body(e, r, &size) != NULL && size == 0);
    BR_CHECK(SDL_strncmp(last_request, "POST /submit HTTP/1.1\r\n", 23) == 0);
    BR_CHECK(contains(last_request, "\r\nContent-Length: 3000\r\n"));
    BR_CHECK(contains(last_request, "\r\nX-Player: 7\r\n"));
    const char *sent = SDL_strstr(last_request, "\r\n\r\n");
    body[0] = 'a';
    BR_CHECK(sent != NULL && SDL_memcmp(sent + 4, body, sizeof body) == 0);
    br_http_release(e, r);
}

static void test_responses(br_engine *e, br_net_server server)
{
    /* chunked */
    static const char *const chunked[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;x=y\r\nWiki\r\n6\r\npedia \r\nE\r\nin \r\n\r\nchunks."
        "\r\n0\r\nExpires: never\r\n\r\n"
    };
    br_http_request r = br_http_get(e, "http://127.0.0.1:47821/chunked");
    BR_CHECK(serve(e, r, server, chunked, 1, true) == BR_HTTP_DONE);
    size_t size = 0;
    const char *body = br_http_body(e, r, &size);
    BR_CHECK(body != NULL && SDL_strcmp(body, "Wikipedia in \r\n\r\nchunks.") == 0 && size == 24);
    br_http_release(e, r);

    /* 404 is DONE with its status */
    static const char *const missing[] = { "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found" };
    r = br_http_get(e, "http://127.0.0.1:47821/nothing");
    BR_CHECK(serve(e, r, server, missing, 1, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 404);
    BR_CHECK(SDL_strcmp(br_http_body(e, r, NULL), "not found") == 0);
    br_http_release(e, r);

    /* no length: the body runs to the close; an interim 100 first */
    static const char *const to_close[] = { "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.0 200 OK\r\n\r\nuntil the end" };
    r = br_http_get(e, "http://127.0.0.1:47821/close");
    BR_CHECK(serve(e, r, server, to_close, 1, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 200 && SDL_strcmp(br_http_body(e, r, NULL), "until the end") == 0);
    br_http_release(e, r);

    /* the server closes after half the body */
    static const char *const half[] = { "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly fifty bytes of the hundred promised......" };
    r = br_http_get(e, "http://127.0.0.1:47821/half");
    BR_CHECK(serve(e, r, server, half, 1, true) == BR_HTTP_FAILED);
    br_log_info("test_http: half body: %s", br_http_error(e, r));
    BR_CHECK(contains(br_http_error(e, r), "closed"));
    BR_CHECK(br_http_status(e, r) == 0 && br_http_body(e, r, NULL) == NULL);
    br_http_release(e, r);

    /* half a chunked body */
    static const char *const half_chunk[] = { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort" };
    r = br_http_get(e, "http://127.0.0.1:47821/half-chunk");
    BR_CHECK(serve(e, r, server, half_chunk, 1, true) == BR_HTTP_FAILED);
    br_http_release(e, r);

    /* max_body 10 against a 100-byte body */
    static char hundred[256];
    SDL_strlcpy(hundred, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", sizeof hundred);
    size_t head = SDL_strlen(hundred);
    SDL_memset(hundred + head, 'x', 100);
    hundred[head + 100] = '\0';
    static const char *const big[] = { hundred };
    br_http_desc desc;
    SDL_zero(desc);
    desc.url = "http://127.0.0.1:47821/big";
    desc.max_body = 10;
    r = br_http_send(e, &desc);
    BR_CHECK(serve(e, r, server, big, 1, true) == BR_HTTP_FAILED);
    br_log_info("test_http: max_body: %s", br_http_error(e, r));
    BR_CHECK(contains(br_http_error(e, r), "limit"));
    br_http_release(e, r);

    /* garbage instead of HTTP */
    static const char *const garbage[] = { "SSH-2.0-OpenSSH_9.6\r\n\r\n" };
    r = br_http_get(e, "http://127.0.0.1:47821/garbage");
    BR_CHECK(serve(e, r, server, garbage, 1, true) == BR_HTTP_FAILED);
    br_http_release(e, r);
}

static void test_redirects(br_engine *e, br_net_server server)
{
    /* 302 to a path, 301 to an absolute URL, then the answer */
    static const char *const chain[] = {
        "HTTP/1.1 302 Found\r\nLocation: /two\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 301 Moved Permanently\r\nLocation: http://localhost:47821/three\r\nContent-Length: 4\r\n\r\nmove",
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\narrived",
    };
    br_http_request r = br_http_get(e, "http://127.0.0.1:47821/one");
    BR_CHECK(serve(e, r, server, chain, 3, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 200 && SDL_strcmp(br_http_body(e, r, NULL), "arrived") == 0);
    BR_CHECK(SDL_strncmp(last_request, "GET /three HTTP/1.1\r\n", 21) == 0);
    BR_CHECK(contains(last_request, "\r\nHost: localhost:47821\r\n"));
    br_http_release(e, r);

    /* 303 after POST becomes GET without the body; credentials stay on the same origin */
    static const char *const see_other[] = {
        "HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
    };
    br_http_desc desc;
    SDL_zero(desc);
    desc.method = "POST";
    desc.url = "http://127.0.0.1:47821/form";
    desc.headers = "Authorization: Bearer x\r\nContent-Type: text/plain\r\nHost: 127.0.0.1:47821\r\n";
    desc.body = "data";
    desc.body_size = 4;
    r = br_http_send(e, &desc);
    BR_CHECK(serve(e, r, server, see_other, 2, true) == BR_HTTP_DONE);
    BR_CHECK(SDL_strncmp(last_request, "GET /result HTTP/1.1\r\n", 22) == 0);
    BR_CHECK(!contains(last_request, "Content-Length") && !contains(last_request, "data"));
    BR_CHECK(contains(last_request, "Authorization: Bearer x"));
    br_http_release(e, r);

    /* 307 keeps POST and its body; another host drops the credentials */
    static const char *const temporary[] = {
        "HTTP/1.1 307 Temporary Redirect\r\nLocation: http://localhost:47821/again\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
    };
    r = br_http_send(e, &desc);
    BR_CHECK(serve(e, r, server, temporary, 2, true) == BR_HTTP_DONE);
    BR_CHECK(SDL_strncmp(last_request, "POST /again HTTP/1.1\r\n", 22) == 0);
    BR_CHECK(contains(last_request, "\r\nContent-Length: 4\r\n") && contains(last_request, "\r\n\r\ndata"));
    BR_CHECK(!contains(last_request, "Authorization"));
    /* the caller's Host names the first origin: the new one gets its own (as curl does) */
    BR_CHECK(contains(last_request, "\r\nHost: localhost:47821\r\n") && !contains(last_request, "Host: 127.0.0.1"));
    br_http_release(e, r);

    /* six redirects: FAILED */
    static const char *const loop[] = { "HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n" };
    r = br_http_get(e, "http://127.0.0.1:47821/loop");
    BR_CHECK(serve(e, r, server, loop, 1, true) == BR_HTTP_FAILED);
    br_log_info("test_http: redirect loop: %s", br_http_error(e, r));
    BR_CHECK(contains(br_http_error(e, r), "redirects"));
    br_http_release(e, r);

    /* a 3xx without Location is the response */
    static const char *const bare[] = { "HTTP/1.1 302 Found\r\nContent-Length: 3\r\n\r\nbye" };
    r = br_http_get(e, "http://127.0.0.1:47821/bare");
    BR_CHECK(serve(e, r, server, bare, 1, true) == BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 302);
    br_http_release(e, r);
}

static void test_timeout(br_engine *e, br_net_server server)
{
    br_http_desc desc;
    SDL_zero(desc);
    desc.url = "http://127.0.0.1:47821/silent";
    desc.timeout = 0.2f;
    uint64_t start = SDL_GetTicksNS();
    br_http_request r = br_http_send(e, &desc);
    BR_CHECK(serve(e, r, server, NULL, 0, false) == BR_HTTP_FAILED);
    double seconds = (double)(SDL_GetTicksNS() - start) / 1e9;
    br_log_info("test_http: timeout after %.3f s: %s", seconds, br_http_error(e, r));
    BR_CHECK(seconds >= 0.19 && seconds < 1.0);
    BR_CHECK(contains(br_http_error(e, r), "timed out"));
    br_http_release(e, r);
}

static void test_release_pending(br_engine *e, br_net_server server)
{
    br_http_request r = br_http_get(e, "http://127.0.0.1:47821/slow");
    br_net_socket peer = { 0, 0 };
    uint64_t deadline = deadline_in(10.0);
    while (peer.generation == 0 && before(deadline)) {
        step(e);
        peer = br_net_accept(e, server);
    }
    BR_CHECK(peer.generation != 0);
    BR_CHECK(br_http_request_state(e, r) == BR_HTTP_PENDING);
    br_http_release(e, r);
    BR_CHECK(br_http_request_state(e, r) == BR_HTTP_NONE);
    /* the server sees the connection go */
    char scratch[512];
    int n = 0;
    deadline = deadline_in(10.0);
    while (n >= 0 && before(deadline)) {
        step(e);
        n = br_net_receive(e, peer, scratch, (int)sizeof scratch);
    }
    BR_CHECK(n < 0);
    br_net_close(e, peer);
    br_http_release(e, r);  /* again: a no-op */
}

static void test_table_full(br_engine *e)
{
    static br_http_request requests[BR_HTTP_MAX_REQUESTS];
    for (int i = 0; i < BR_HTTP_MAX_REQUESTS; i++) {
        requests[i] = br_http_get(e, "http://127.0.0.1:47823/");  /* nobody listens: stays PENDING a while */
        BR_CHECK(requests[i].generation != 0);
    }
    BR_CHECK(br_http_get(e, "http://127.0.0.1:47823/").generation == 0);  /* the 17th: logged */
    for (int i = 0; i < BR_HTTP_MAX_REQUESTS; i++) {
        br_http_release(e, requests[i]);
    }
    br_http_request after = br_http_get(e, "http://127.0.0.1:47823/");
    BR_CHECK(after.generation != 0);
    /* a refused connection: FAILED with tls's reason */
    uint64_t deadline = deadline_in(20.0);
    while (br_http_request_state(e, after) == BR_HTTP_PENDING && before(deadline)) {
        step(e);
    }
    BR_CHECK(br_http_request_state(e, after) == BR_HTTP_FAILED);
    br_log_info("test_http: refused: %s", br_http_error(e, after));
    BR_CHECK(br_http_error(e, after)[0] != '\0');
    br_http_release(e, after);
    /* the stale handles of the released ones */
    BR_CHECK(br_http_request_state(e, requests[0]) == BR_HTTP_NONE);
}

/* Steps until r leaves PENDING, answering its request on the TLS test server with reply; the
 * server then closes with close_notify, or (clean false) drops the TCP connection without it. */
static br_http_state serve_tls(br_engine *e, br_http_request r, const char *reply, bool clean, char *request,
                               size_t capacity)
{
    size_t got = 0;
    request[0] = '\0';
    bool answered = false;
    uint64_t deadline = deadline_in(20.0);
    while (br_http_request_state(e, r) == BR_HTTP_PENDING && before(deadline)) {
        step(e);
        if (!tls_conn.used) {
            tls_test_server_accept(&tls_server, &tls_conn);
        }
        int n = tls_test_conn_receive(&tls_conn, request + got, (int)(capacity - 1 - got));
        if (n > 0) {
            got += (size_t)n;
            request[got] = '\0';
        }
        if (!answered && request_complete(request, got)) {
            BR_CHECK(tls_test_conn_send(&tls_conn, reply, SDL_strlen(reply)));
            if (!clean) {
                tls_conn.state = -1;   /* tls_test_conn_close then skips close_notify */
            }
            tls_test_conn_close(&tls_conn);
            answered = true;
        }
    }
    return br_http_request_state(e, r);
}

static void test_https(br_engine *e)
{
    BR_CHECK(tls_test_server_start(&tls_server, e, TLS_PORT, "server.pem"));
    BR_CHECK(tls_test_trust_ca(e));
    char request[4096];
    br_http_request r = br_http_get(e, "https://localhost:47822/secure");
    BR_CHECK(serve_tls(e, r, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecret", true, request, sizeof request) ==
             BR_HTTP_DONE);
    BR_CHECK(br_http_status(e, r) == 200 && SDL_strcmp(br_http_body(e, r, NULL), "secret") == 0);
    BR_CHECK(SDL_strncmp(request, "GET /secure HTTP/1.1\r\nHost: localhost:47822\r\n", 45) == 0);
    br_http_release(e, r);

    /* a body without a length ends at the close: complete with close_notify ... */
    static const char to_close[] = "HTTP/1.1 200 OK\r\n\r\nall of it";
    r = br_http_get(e, "https://localhost:47822/to-close");
    BR_CHECK(serve_tls(e, r, to_close, true, request, sizeof request) == BR_HTTP_DONE);
    BR_CHECK(SDL_strcmp(br_http_body(e, r, NULL), "all of it") == 0);
    br_http_release(e, r);
    /* ... and maybe cut short without it (curl fails it too) */
    r = br_http_get(e, "https://localhost:47822/cut");
    BR_CHECK(serve_tls(e, r, to_close, false, request, sizeof request) == BR_HTTP_FAILED);
    br_log_info("test_http: no close_notify: %s", br_http_error(e, r));
    BR_CHECK(contains(br_http_error(e, r), "close_notify") && br_http_body(e, r, NULL) == NULL);
    br_http_release(e, r);
    /* a body with a length needs no close_notify */
    r = br_http_get(e, "https://localhost:47822/length");
    BR_CHECK(serve_tls(e, r, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", false, request, sizeof request) ==
             BR_HTTP_DONE);
    br_http_release(e, r);

    /* https never follows a redirect to http */
    r = br_http_get(e, "https://localhost:47822/downgrade");
    static const char downgrade[] = "HTTP/1.1 302 Found\r\nLocation: http://localhost:47821/\r\nContent-Length: 0\r\n\r\n";
    BR_CHECK(serve_tls(e, r, downgrade, true, request, sizeof request) == BR_HTTP_FAILED);
    br_log_info("test_http: downgrade: %s", br_http_error(e, r));
    BR_CHECK(contains(br_http_error(e, r), "https to http"));
    br_http_release(e, r);
    tls_test_conn_close(&tls_conn);
    tls_test_server_stop(&tls_server);
}

int main(void)
{
    test_not_registered();

    br_engine *e = br_engine_create("braty-test-http", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_http_register(e));
    BR_CHECK(br_http_register(e));  /* idempotent */
    test_zero_handles(e);
    test_bad_arguments(e);

    br_net_server server = br_net_listen(e, PORT);
    BR_CHECK(server.generation != 0);
    test_get(e, server);
    test_post(e, server);
    test_responses(e, server);
    test_redirects(e, server);
    test_timeout(e, server);
    test_release_pending(e, server);
    br_net_server_close(e, server);
    test_table_full(e);
    test_https(e);

    br_engine_destroy(e);
    return br_test_finish();
}
