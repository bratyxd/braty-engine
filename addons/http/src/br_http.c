/* br_http: a fixed table of requests. Each runs one tls stream (one connection per request):
 * `http.update` sends the request once the stream is OPEN, feeds what arrives to the response
 * parser (br_http_parse.c), follows redirects and enforces the real-time deadline. */
#include "braty/br_http.h"
#include "br_http_internal.h"

#include "braty/br_app.h"

#include <SDL3/SDL.h>
#include <stdarg.h>

/* Bytes read per br_tls_receive, and reads per request per frame (bounds a frame's work). */
#define BR_HTTP_READ_SIZE 16384
#define BR_HTTP_READS_PER_FRAME 64

typedef struct br_http_slot {
    br_tls_stream   stream;
    br_http_parser *parser;
    char           *headers;       /* the caller's headers (copy), NULL = none */
    void           *body;          /* the request body (copy), NULL = none */
    size_t          body_size;
    char           *request;       /* the request head of the current hop; freed once sent */
    size_t          request_size;
    size_t          max_body;
    uint64_t        start;         /* SDL_GetTicksNS at br_http_send */
    uint64_t        deadline;
    float           timeout;
    uint32_t        generation;
    br_http_state   state;
    int             redirects;
    bool            used;
    bool            sent;
    bool            send_length;   /* Content-Length on this hop */
    br_http_url     url;
    char            method[BR_HTTP_MAX_METHOD + 1];
    char            error[512];
} br_http_slot;

typedef struct br_http {
    br_http_slot requests[BR_HTTP_MAX_REQUESTS];
    uint8_t      read_buffer[BR_HTTP_READ_SIZE];
} br_http;

static br_http *br_http_get_state(br_engine *e)
{
    return e != NULL ? br_addon_get(e, "http") : NULL;
}

static br_http_slot *br_http_slot_get(br_http *h, br_http_request r)
{
    if (h == NULL || r.index >= BR_HTTP_MAX_REQUESTS) {
        return NULL;
    }
    br_http_slot *slot = &h->requests[r.index];
    return slot->used && slot->generation == r.generation ? slot : NULL;
}

/* Frees what only a pending request needs: the stream, the request copies. */
static void br_http_drop_connection(br_engine *e, br_http_slot *slot)
{
    br_tls_close(e, slot->stream);
    slot->stream.index = 0;
    slot->stream.generation = 0;
    br_free(slot->request);
    br_free(slot->headers);
    br_free(slot->body);
    slot->request = NULL;
    slot->headers = NULL;
    slot->body = NULL;
}

static void br_http_fail(br_engine *e, br_http_slot *slot, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(slot->error, sizeof slot->error, fmt, args);
    va_end(args);
    slot->state = BR_HTTP_FAILED;
    br_log_error("http: %s %s:%u%s: %s", slot->method, slot->url.host, (unsigned)slot->url.port, slot->url.path,
                 slot->error);
    br_http_drop_connection(e, slot);
}

/* Writes the current hop's request head and opens its stream. False when it failed (logged). */
static bool br_http_start_hop(br_engine *e, br_http_slot *slot)
{
    char reason[256] = "";
    size_t size = br_http_write_request(NULL, 0, slot->method, &slot->url, slot->headers, slot->send_length,
                                        slot->body_size, reason, sizeof reason);
    if (size == 0) {
        br_http_fail(e, slot, "%s", reason);
        return false;
    }
    br_free(slot->request);
    slot->request = br_alloc(size + 1);
    if (slot->request == NULL) {
        br_http_fail(e, slot, "out of memory");
        return false;
    }
    br_http_write_request(slot->request, size + 1, slot->method, &slot->url, slot->headers, slot->send_length,
                          slot->body_size, reason, sizeof reason);
    slot->request_size = size;
    slot->sent = false;
    br_http_parser_reset(slot->parser, slot->max_body, SDL_strcmp(slot->method, "HEAD") == 0);
    slot->stream = br_tls_open(e, slot->url.host, slot->url.port, slot->url.secure);
    if (slot->stream.generation == 0) {
        br_http_fail(e, slot, "cannot open a connection (logged above)");
        return false;
    }
    return true;
}

/* Removes the lines named `name` from a header text (in place). */
static void br_http_remove_header(char *headers, const char *name)
{
    if (headers == NULL) {
        return;
    }
    size_t length = SDL_strlen(name);
    char *line = headers;
    while (*line != '\0') {
        char *end = SDL_strchr(line, '\n');
        char *next = end != NULL ? end + 1 : line + SDL_strlen(line);
        if (SDL_strncasecmp(line, name, length) == 0 && line[length] == ':') {
            SDL_memmove(line, next, SDL_strlen(next) + 1);
        } else {
            line = next;
        }
    }
}

/* A 3xx with a Location: the next hop (true), or the response stands / the request FAILED. */
static bool br_http_follow(br_engine *e, br_http_slot *slot, int status)
{
    const char *location = br_http_parser_header(slot->parser, "Location");
    bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!redirect || location == NULL) {
        return false;  /* a 3xx without Location is the response (RFC 9110 section 15.4) */
    }
    if (slot->redirects == BR_HTTP_MAX_REDIRECTS) {
        br_http_fail(e, slot, "more than %d redirects", BR_HTTP_MAX_REDIRECTS);
        return true;
    }
    br_http_url next;
    char reason[256];
    if (!br_http_url_resolve(&slot->url, location, &next, reason, sizeof reason)) {
        br_http_fail(e, slot, "bad redirect (%d): %s", status, reason);
        return true;
    }
    if (slot->url.secure && !next.secure) {
        br_http_fail(e, slot, "redirect from https to http refused (%.200s)", location);
        return true;
    }
    /* RFC 9110 sections 15.4.2-4: 303 becomes GET; 301 and 302 after POST too (what every client
     * does). 307 and 308 keep the method and the body. */
    bool to_get = status == 303 && SDL_strcmp(slot->method, "HEAD") != 0;
    to_get = to_get || ((status == 301 || status == 302) && SDL_strcmp(slot->method, "POST") == 0);
    if (to_get) {
        SDL_strlcpy(slot->method, "GET", sizeof slot->method);
        br_free(slot->body);
        slot->body = NULL;
        slot->body_size = 0;
        slot->send_length = false;
        br_http_remove_header(slot->headers, "Content-Length");
        br_http_remove_header(slot->headers, "Content-Type");
        br_http_remove_header(slot->headers, "Transfer-Encoding");
    }
    /* Credentials go to the origin they were meant for only (curl does the same). */
    bool same_origin = next.secure == slot->url.secure && next.port == slot->url.port &&
                       SDL_strcasecmp(next.host, slot->url.host) == 0;
    if (!same_origin) {
        br_http_remove_header(slot->headers, "Authorization");
        br_http_remove_header(slot->headers, "Cookie");
        br_http_remove_header(slot->headers, "Proxy-Authorization");
    }
    br_tls_close(e, slot->stream);
    slot->url = next;
    slot->redirects++;
    br_http_start_hop(e, slot);
    return true;
}

/* The parser finished: a redirect to follow, an unexpected 101, or the response. */
static void br_http_complete(br_engine *e, br_http_slot *slot)
{
    int status = br_http_parser_status(slot->parser);
    if (status == 101) {
        br_http_fail(e, slot, "unexpected 101 Switching Protocols (use websocket for upgrades)");
        return;
    }
    if (br_http_follow(e, slot, status)) {
        return;
    }
    slot->state = BR_HTTP_DONE;
    br_http_drop_connection(e, slot);
}

static void br_http_step(br_engine *e, br_http *h, br_http_slot *slot, uint64_t now)
{
    if (now >= slot->deadline) {
        br_http_fail(e, slot, "timed out after %.1f s", (double)slot->timeout);
        return;
    }
    br_net_state net = br_tls_state(e, slot->stream);
    if (net == BR_NET_CONNECTING) {
        return;
    }
    if (net == BR_NET_FAILED) {
        br_http_fail(e, slot, "%s", br_tls_error(e, slot->stream));
        return;
    }
    if (net == BR_NET_OPEN && !slot->sent) {
        bool ok = br_tls_send(e, slot->stream, slot->request, slot->request_size);
        if (ok && slot->body_size > 0) {
            ok = br_tls_send(e, slot->stream, slot->body, slot->body_size);
        }
        if (!ok) {
            br_http_fail(e, slot, "sending the request failed: %s", br_tls_error(e, slot->stream));
            return;
        }
        slot->sent = true;
        br_free(slot->request);
        slot->request = NULL;
    }
    br_http_parse_result result = br_http_parser_result(slot->parser);
    for (int i = 0; i < BR_HTTP_READS_PER_FRAME && result == BR_HTTP_PARSE_MORE; i++) {
        int n = br_tls_receive(e, slot->stream, h->read_buffer, (int)sizeof h->read_buffer);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (br_tls_state(e, slot->stream) == BR_NET_FAILED) {
                br_http_fail(e, slot, "%s", br_tls_error(e, slot->stream));
                return;
            }
            /* ponytail: a TCP end without close_notify also reads as the close here (tls cannot
             * tell them apart), so a truncated body without a length is only as safe as that. */
            result = br_http_parser_finish(slot->parser);
            break;
        }
        result = br_http_parser_feed(slot->parser, h->read_buffer, (size_t)n);
    }
    if (result == BR_HTTP_PARSE_ERROR) {
        br_http_fail(e, slot, "bad response: %s", br_http_parser_error(slot->parser));
    } else if (result == BR_HTTP_PARSE_DONE) {
        br_http_complete(e, slot);
    }
}

static void br_http_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_http *h = user;
    uint64_t now = SDL_GetTicksNS();
    for (uint32_t i = 0; i < BR_HTTP_MAX_REQUESTS; i++) {
        br_http_slot *slot = &h->requests[i];
        if (slot->used && slot->state == BR_HTTP_PENDING) {
            br_http_step(e, h, slot, now);
        }
    }
}

static void br_http_free_slot(br_engine *e, br_http_slot *slot)
{
    br_http_drop_connection(e, slot);
    br_http_parser_destroy(slot->parser);
    slot->parser = NULL;
    slot->used = false;
    slot->state = BR_HTTP_NONE;
}

static void br_http_shutdown(br_engine *e, void *user)
{
    br_http *h = user;
    for (uint32_t i = 0; i < BR_HTTP_MAX_REQUESTS; i++) {
        if (h->requests[i].used) {
            br_http_free_slot(e, &h->requests[i]);
        }
    }
    br_free(h);
}

bool br_http_register(br_engine *e)
{
    if (e == NULL) {
        br_log_error("br_http_register: no engine");
        return false;
    }
    if (br_http_get_state(e) != NULL) {
        return true;
    }
    if (!br_tls_register(e)) {
        return false;
    }
    br_http *h = br_alloc(sizeof(br_http));
    if (h == NULL) {
        return false;
    }
    SDL_zerop(h);
    if (!br_addon_add(e, "http", h)) {
        br_free(h);
        return false;
    }
    br_system_add(e, "http.update", BR_PHASE_INPUT, BR_HTTP_UPDATE_PRIORITY, br_http_update, h);
    br_engine_on_shutdown(e, br_http_shutdown, h);
    return true;
}

/* A copy of size bytes (+ NUL), NULL when data is NULL or out of memory (*ok false then). */
static void *br_http_copy(const void *data, size_t size, bool *ok)
{
    if (data == NULL) {
        return NULL;
    }
    char *copy = br_alloc(size + 1);
    if (copy == NULL) {
        *ok = false;
        return NULL;
    }
    SDL_memcpy(copy, data, size);
    copy[size] = '\0';
    return copy;
}

br_http_request br_http_send(br_engine *e, const br_http_desc *desc)
{
    br_http_request none = { 0, 0 };
    br_http *h = br_http_get_state(e);
    if (h == NULL) {
        br_log_error("br_http_send: call br_http_register first");
        return none;
    }
    if (desc == NULL || desc->url == NULL) {
        br_log_error("br_http_send: no URL");
        return none;
    }
    if (desc->body == NULL && desc->body_size > 0) {
        br_log_error("br_http_send: body_size %llu without a body", (unsigned long long)desc->body_size);
        return none;
    }
    br_http_url url;
    bool websocket = false;
    char reason[256];
    if (!br_http_url_split(desc->url, &url, &websocket, reason, sizeof reason)) {
        br_log_error("br_http_send: %s", reason);
        return none;
    }
    if (websocket) {
        br_log_error("br_http_send: %.64s is a WebSocket URL (use the websocket add-on)", desc->url);
        return none;
    }
    const char *method = desc->method != NULL ? desc->method : "GET";
    /* every check the request writer makes, before anything is taken */
    if (br_http_write_request(NULL, 0, method, &url, desc->headers, false, 0, reason, sizeof reason) == 0) {
        br_log_error("br_http_send: %s", reason);
        return none;
    }
    uint32_t index = 0;
    while (index < BR_HTTP_MAX_REQUESTS && h->requests[index].used) {
        index++;
    }
    if (index == BR_HTTP_MAX_REQUESTS) {
        br_log_error("br_http_send: all %d requests are in use (BR_HTTP_MAX_REQUESTS; release finished ones)",
                     BR_HTTP_MAX_REQUESTS);
        return none;
    }
    br_http_slot *slot = &h->requests[index];
    uint32_t generation = slot->generation == UINT32_MAX ? 1 : slot->generation + 1;
    SDL_zerop(slot);
    slot->generation = generation;
    bool ok = true;
    slot->headers = br_http_copy(desc->headers, desc->headers != NULL ? SDL_strlen(desc->headers) : 0, &ok);
    slot->body = br_http_copy(desc->body, desc->body_size, &ok);
    slot->body_size = desc->body_size;
    slot->parser = br_http_parser_create(desc->max_body, false);
    if (!ok || slot->parser == NULL) {
        br_log_error("br_http_send: out of memory");
        br_free(slot->headers);
        br_free(slot->body);
        br_http_parser_destroy(slot->parser);
        slot->generation = generation;
        return none;
    }
    slot->used = true;
    slot->state = BR_HTTP_PENDING;
    slot->url = url;
    SDL_strlcpy(slot->method, method, sizeof slot->method);
    slot->max_body = desc->max_body;
    /* RFC 9110 section 8.6: a length when there is a body, or the method expects one */
    slot->send_length = desc->body != NULL || SDL_strcmp(method, "POST") == 0 || SDL_strcmp(method, "PUT") == 0 ||
                        SDL_strcmp(method, "PATCH") == 0;
    slot->timeout = desc->timeout > 0.0f ? desc->timeout : BR_HTTP_DEFAULT_TIMEOUT;
    slot->start = SDL_GetTicksNS();
    slot->deadline = slot->start + (uint64_t)((double)slot->timeout * 1e9);
    br_http_request handle = { index, generation };
    br_http_start_hop(e, slot);  /* a failure leaves the request FAILED with the reason */
    return handle;
}

br_http_request br_http_get(br_engine *e, const char *url)
{
    br_http_desc desc;
    SDL_zero(desc);
    desc.url = url;
    return br_http_send(e, &desc);
}

br_http_state br_http_request_state(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL ? slot->state : BR_HTTP_NONE;
}

int br_http_status(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL && slot->state == BR_HTTP_DONE ? br_http_parser_status(slot->parser) : 0;
}

const char *br_http_header(br_engine *e, br_http_request r, const char *name)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL && slot->state == BR_HTTP_DONE ? br_http_parser_header(slot->parser, name) : NULL;
}

uint32_t br_http_header_count(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL && slot->state == BR_HTTP_DONE ? br_http_parser_header_count(slot->parser) : 0;
}

bool br_http_header_get_at(br_engine *e, br_http_request r, uint32_t index, const char **name, const char **value)
{
    if (name != NULL) {
        *name = NULL;
    }
    if (value != NULL) {
        *value = NULL;
    }
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL && slot->state == BR_HTTP_DONE && br_http_parser_header_at(slot->parser, index, name, value);
}

const void *br_http_body(br_engine *e, br_http_request r, size_t *size)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    if (slot == NULL || slot->state != BR_HTTP_DONE) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    return br_http_parser_body(slot->parser, size);
}

float br_http_progress(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    if (slot == NULL || slot->state == BR_HTTP_FAILED) {
        return 0.0f;
    }
    return br_http_parser_progress(slot->parser);
}

const char *br_http_error(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    return slot != NULL && slot->state == BR_HTTP_FAILED ? slot->error : "";
}

void br_http_release(br_engine *e, br_http_request r)
{
    br_http_slot *slot = br_http_slot_get(br_http_get_state(e), r);
    if (slot != NULL) {
        br_http_free_slot(e, slot);
    }
}
