/* br_websocket: a fixed table of sockets. Each runs one tls stream: `websocket.update` sends the
 * upgrade request once the stream is OPEN, parses the 101 with http's parser and checks it
 * (br_websocket_frame.c), then moves bytes between the stream and the slot's br_websocket_conn
 * (wslay) and enforces the real-time handshake deadline. */
#include "braty/br_websocket.h"
#include "br_websocket_internal.h"

#include "braty/br_app.h"

#include <SDL3/SDL.h>
#include <stdarg.h>

/* Bytes read per br_tls_receive, and reads per socket per frame (bounds a frame's work). */
#define BR_WEBSOCKET_READ_SIZE 16384
#define BR_WEBSOCKET_READS_PER_FRAME 64

typedef struct br_websocket_slot {
    br_tls_stream      stream;
    br_http_parser    *parser;        /* while CONNECTING */
    char              *request;       /* the upgrade request until sent */
    size_t             request_size;
    char              *protocols;     /* the offered protocols (copy), NULL = none */
    char               accept[BR_WEBSOCKET_ACCEPT_LENGTH + 1];
    br_websocket_conn  conn;          /* after the 101 (conn.ctx != NULL) */
    br_websocket_state state;         /* CONNECTING, or CLOSED / FAILED before a conn existed */
    uint64_t           start;         /* SDL_GetTicksNS at open */
    float              handshake_timeout;
    float              close_timeout;
    float              ping_interval;
    size_t             max_message;
    uint32_t           generation;
    bool               used;
    bool               sent;
    br_http_url        url;
    char               error[256];
} br_websocket_slot;

typedef struct br_websocket_addon {
    br_websocket_slot sockets[BR_WEBSOCKET_MAX];
    uint8_t           read_buffer[BR_WEBSOCKET_READ_SIZE];
} br_websocket_addon;

static br_websocket_addon *br_websocket_get_state(br_engine *e)
{
    return e != NULL ? br_addon_get(e, "websocket") : NULL;
}

static br_websocket_slot *br_websocket_slot_get(br_websocket_addon *w, br_websocket ws)
{
    if (w == NULL || ws.index >= BR_WEBSOCKET_MAX) {
        return NULL;
    }
    br_websocket_slot *slot = &w->sockets[ws.index];
    return slot->used && slot->generation == ws.generation ? slot : NULL;
}

static br_websocket_state br_websocket_slot_state(const br_websocket_slot *slot)
{
    return slot->conn.ctx != NULL ? slot->conn.state : slot->state;
}

static bool br_websocket_random(void *user, void *out, size_t size)
{
    return br_tls_random(user, out, size);
}

static uint64_t br_websocket_ns(float seconds)
{
    return (uint64_t)((double)seconds * 1e9);
}

/* Frees what only the handshake needs. */
static void br_websocket_drop_handshake(br_websocket_slot *slot)
{
    br_http_parser_destroy(slot->parser);
    slot->parser = NULL;
    br_free(slot->request);
    slot->request = NULL;
}

static void br_websocket_drop_stream(br_engine *e, br_websocket_slot *slot)
{
    br_tls_close(e, slot->stream);
    slot->stream.index = 0;
    slot->stream.generation = 0;
}

/* The handshake failed: FAILED with the reason (logged). */
static void br_websocket_fail(br_engine *e, br_websocket_slot *slot, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(slot->error, sizeof slot->error, fmt, args);
    va_end(args);
    slot->state = BR_WEBSOCKET_FAILED;
    br_log_error("websocket: %s:%u%s: %s", slot->url.host, (unsigned)slot->url.port, slot->url.path, slot->error);
    br_websocket_drop_handshake(slot);
    br_websocket_drop_stream(e, slot);
}

/* Sends what the conn wrote; closes the stream once the conn ended (logged when not clean). */
static void br_websocket_flush(br_engine *e, br_websocket_slot *slot)
{
    size_t size = 0;
    const void *out = br_websocket_conn_output(&slot->conn, &size);
    if (size > 0) {
        if (!br_tls_send(e, slot->stream, out, size)) {
            const char *reason = br_tls_error(e, slot->stream);
            br_websocket_conn_end(&slot->conn, reason[0] != '\0' ? reason : NULL, SDL_GetTicksNS());
        }
        br_websocket_conn_output_clear(&slot->conn);
    }
    br_websocket_state state = slot->conn.state;
    if ((state == BR_WEBSOCKET_CLOSED || state == BR_WEBSOCKET_FAILED) && slot->stream.generation != 0) {
        br_websocket_drop_stream(e, slot);
        if (slot->conn.error[0] != '\0') {
            if (state == BR_WEBSOCKET_FAILED) {
                br_log_error("websocket: %s: %s", slot->url.host, slot->conn.error);
            } else {
                br_log_warn("websocket: %s: %s", slot->url.host, slot->conn.error);
            }
        }
    }
}

/* An open connection: stream -> conn, timers, conn -> stream. */
static void br_websocket_pump(br_engine *e, br_websocket_addon *w, br_websocket_slot *slot, uint64_t now)
{
    for (int i = 0; i < BR_WEBSOCKET_READS_PER_FRAME; i++) {
        br_websocket_state state = slot->conn.state;
        if (state != BR_WEBSOCKET_OPEN && state != BR_WEBSOCKET_CLOSING) {
            break;
        }
        int n = br_tls_receive(e, slot->stream, w->read_buffer, (int)sizeof w->read_buffer);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            const char *reason = br_tls_state(e, slot->stream) == BR_NET_FAILED ? br_tls_error(e, slot->stream) : NULL;
            br_websocket_conn_end(&slot->conn, reason, now);
            break;
        }
        br_websocket_conn_feed(&slot->conn, w->read_buffer, (size_t)n, now);
    }
    br_websocket_conn_update(&slot->conn, now);
    br_websocket_flush(e, slot);
}

/* The 101 passed its checks: the conn takes over, with the bytes that came after the headers. */
static void br_websocket_upgrade(br_engine *e, br_websocket_slot *slot, uint64_t now)
{
    if (!br_websocket_conn_init(&slot->conn, br_websocket_random, e, now)) {
        br_websocket_fail(e, slot, "out of memory");
        return;
    }
    br_websocket_conn_set_max_message(&slot->conn, slot->max_message);
    br_websocket_conn_set_times(&slot->conn, br_websocket_ns(slot->ping_interval), br_websocket_ns(slot->close_timeout),
                                now);
    size_t extra = 0;
    const void *first = br_http_parser_unread(slot->parser, &extra);
    if (first != NULL && extra > 0) {
        br_websocket_conn_feed(&slot->conn, first, extra, now);
    }
    br_websocket_drop_handshake(slot);
    br_websocket_flush(e, slot);
}

static void br_websocket_handshake(br_engine *e, br_websocket_addon *w, br_websocket_slot *slot, uint64_t now)
{
    if (now - slot->start >= br_websocket_ns(slot->handshake_timeout)) {
        br_websocket_fail(e, slot, "opening handshake timed out after %.1f s", (double)slot->handshake_timeout);
        return;
    }
    br_net_state net = br_tls_state(e, slot->stream);
    if (net == BR_NET_CONNECTING) {
        return;
    }
    if (net == BR_NET_FAILED) {
        br_websocket_fail(e, slot, "%s", br_tls_error(e, slot->stream));
        return;
    }
    if (net == BR_NET_OPEN && !slot->sent) {
        if (!br_tls_send(e, slot->stream, slot->request, slot->request_size)) {
            br_websocket_fail(e, slot, "sending the upgrade request failed: %s", br_tls_error(e, slot->stream));
            return;
        }
        slot->sent = true;
        br_free(slot->request);
        slot->request = NULL;
    }
    br_http_parse_result result = br_http_parser_result(slot->parser);
    for (int i = 0; i < BR_WEBSOCKET_READS_PER_FRAME && result == BR_HTTP_PARSE_MORE; i++) {
        int n = br_tls_receive(e, slot->stream, w->read_buffer, (int)sizeof w->read_buffer);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (br_tls_state(e, slot->stream) == BR_NET_FAILED) {
                br_websocket_fail(e, slot, "%s", br_tls_error(e, slot->stream));
                return;
            }
            result = br_http_parser_finish(slot->parser);
            if (result != BR_HTTP_PARSE_DONE) {
                br_websocket_fail(e, slot, "connection closed during the opening handshake");
                return;
            }
            break;
        }
        result = br_http_parser_feed(slot->parser, w->read_buffer, (size_t)n);
        int status = br_http_parser_status(slot->parser);
        if (status != 0 && status != 101) {  /* no need to wait for its body */
            br_websocket_fail(e, slot, "the server answered %d instead of 101 Switching Protocols", status);
            return;
        }
    }
    if (result == BR_HTTP_PARSE_ERROR) {
        br_websocket_fail(e, slot, "bad handshake response: %s", br_http_parser_error(slot->parser));
        return;
    }
    if (result != BR_HTTP_PARSE_DONE) {
        return;
    }
    char reason[256] = "";
    if (!br_websocket_handshake_check(slot->parser, slot->accept, slot->protocols, reason, sizeof reason)) {
        br_websocket_fail(e, slot, "%s", reason);
        return;
    }
    br_websocket_upgrade(e, slot, now);
}

static void br_websocket_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_websocket_addon *w = user;
    uint64_t now = SDL_GetTicksNS();
    for (uint32_t i = 0; i < BR_WEBSOCKET_MAX; i++) {
        br_websocket_slot *slot = &w->sockets[i];
        if (!slot->used) {
            continue;
        }
        if (slot->conn.ctx == NULL && slot->state == BR_WEBSOCKET_CONNECTING) {
            br_websocket_handshake(e, w, slot, now);
        }
        br_websocket_state state = br_websocket_slot_state(slot);
        if (slot->conn.ctx != NULL && (state == BR_WEBSOCKET_OPEN || state == BR_WEBSOCKET_CLOSING)) {
            br_websocket_pump(e, w, slot, now);
        }
    }
}

static void br_websocket_free_slot(br_engine *e, br_websocket_slot *slot)
{
    if (slot->conn.ctx != NULL && slot->conn.state == BR_WEBSOCKET_OPEN) {
        br_websocket_conn_close(&slot->conn, 1001, NULL, SDL_GetTicksNS());  /* going away */
        size_t size = 0;
        const void *out = br_websocket_conn_output(&slot->conn, &size);
        if (size > 0) {
            br_tls_send(e, slot->stream, out, size);
        }
    }
    br_websocket_drop_stream(e, slot);
    br_websocket_drop_handshake(slot);
    br_websocket_conn_free(&slot->conn);
    br_free(slot->protocols);
    slot->protocols = NULL;
    slot->used = false;
    slot->state = BR_WEBSOCKET_CLOSED;
}

static void br_websocket_shutdown(br_engine *e, void *user)
{
    br_websocket_addon *w = user;
    for (uint32_t i = 0; i < BR_WEBSOCKET_MAX; i++) {
        if (w->sockets[i].used) {
            br_websocket_free_slot(e, &w->sockets[i]);
        }
    }
    br_free(w);
}

bool br_websocket_register(br_engine *e)
{
    if (e == NULL) {
        br_log_error("br_websocket_register: no engine");
        return false;
    }
    if (br_websocket_get_state(e) != NULL) {
        return true;
    }
    if (!br_http_register(e)) {
        return false;
    }
    br_websocket_addon *w = br_alloc(sizeof(br_websocket_addon));
    if (w == NULL) {
        return false;
    }
    SDL_zerop(w);
    if (!br_addon_add(e, "websocket", w)) {
        br_free(w);
        return false;
    }
    br_system_add(e, "websocket.update", BR_PHASE_INPUT, BR_WEBSOCKET_UPDATE_PRIORITY, br_websocket_update, w);
    br_engine_on_shutdown(e, br_websocket_shutdown, w);
    return true;
}

br_websocket br_websocket_open(br_engine *e, const char *url, const char *protocols, const char *headers)
{
    br_websocket none = { 0, 0 };
    br_websocket_addon *w = br_websocket_get_state(e);
    if (w == NULL) {
        br_log_error("br_websocket_open: call br_websocket_register first");
        return none;
    }
    if (url == NULL) {
        br_log_error("br_websocket_open: no URL");
        return none;
    }
    br_http_url split;
    bool websocket = false;
    char reason[256] = "";
    if (!br_http_url_split(url, &split, &websocket, reason, sizeof reason)) {
        br_log_error("br_websocket_open: %s", reason);
        return none;
    }
    if (!websocket) {
        br_log_error("br_websocket_open: %.64s is not a ws:// or wss:// URL", url);
        return none;
    }
    uint8_t random16[16];
    if (!br_tls_random(e, random16, sizeof random16)) {
        return none;
    }
    char key[BR_WEBSOCKET_KEY_LENGTH + 1];
    br_websocket_key_text(random16, key);
    /* every check of the request, before anything is taken */
    size_t size = br_websocket_write_handshake(NULL, 0, &split, key, protocols, headers, reason, sizeof reason);
    if (size == 0) {
        br_log_error("br_websocket_open: %s", reason);
        return none;
    }
    uint32_t index = 0;
    while (index < BR_WEBSOCKET_MAX && w->sockets[index].used) {
        index++;
    }
    if (index == BR_WEBSOCKET_MAX) {
        br_log_error("br_websocket_open: all %d sockets are in use (BR_WEBSOCKET_MAX; release ended ones)",
                     BR_WEBSOCKET_MAX);
        return none;
    }
    br_websocket_slot *slot = &w->sockets[index];
    uint32_t generation = slot->generation == UINT32_MAX ? 1 : slot->generation + 1;
    SDL_zerop(slot);
    slot->generation = generation;
    slot->request = br_alloc(size + 1);
    slot->parser = br_http_parser_create(0, false);
    if (protocols != NULL) {
        slot->protocols = br_alloc(SDL_strlen(protocols) + 1);
    }
    if (slot->request == NULL || slot->parser == NULL || (protocols != NULL && slot->protocols == NULL)) {
        br_log_error("br_websocket_open: out of memory");
        br_websocket_drop_handshake(slot);
        br_free(slot->protocols);
        slot->protocols = NULL;
        return none;
    }
    if (protocols != NULL) {
        SDL_strlcpy(slot->protocols, protocols, SDL_strlen(protocols) + 1);
    }
    slot->request_size = br_websocket_write_handshake(slot->request, size + 1, &split, key, protocols, headers,
                                                      reason, sizeof reason);
    br_websocket_accept_key(key, slot->accept);
    slot->used = true;
    slot->state = BR_WEBSOCKET_CONNECTING;
    slot->url = split;
    slot->start = SDL_GetTicksNS();
    slot->handshake_timeout = BR_WEBSOCKET_HANDSHAKE_TIMEOUT;
    slot->close_timeout = BR_WEBSOCKET_CLOSE_TIMEOUT;
    slot->ping_interval = BR_WEBSOCKET_DEFAULT_PING_INTERVAL;
    slot->stream = br_tls_open(e, split.host, split.port, split.secure);
    br_websocket handle = { index, generation };
    if (slot->stream.generation == 0) {
        br_websocket_fail(e, slot, "cannot open a connection (logged above)");
    }
    return handle;
}

br_websocket_state br_websocket_socket_state(br_engine *e, br_websocket ws)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    return slot != NULL ? br_websocket_slot_state(slot) : BR_WEBSOCKET_CLOSED;
}

static bool br_websocket_send(br_engine *e, br_websocket ws, bool text, const void *data, size_t size)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot == NULL || slot->conn.ctx == NULL || slot->conn.state != BR_WEBSOCKET_OPEN) {
        return false;
    }
    bool ok = br_websocket_conn_send(&slot->conn, text, data, size);
    br_websocket_flush(e, slot);
    return ok;
}

bool br_websocket_send_text(br_engine *e, br_websocket ws, const char *text)
{
    if (text == NULL) {
        return false;
    }
    size_t size = SDL_strlen(text);
    if (!br_websocket_utf8_valid(text, size)) {
        br_log_error("br_websocket_send_text: the text is not valid UTF-8 (send it as binary)");
        return false;
    }
    return br_websocket_send(e, ws, true, text, size);
}

bool br_websocket_send_binary(br_engine *e, br_websocket ws, const void *data, size_t size)
{
    if (data == NULL && size > 0) {
        return false;
    }
    return br_websocket_send(e, ws, false, data, size);
}

bool br_websocket_receive(br_engine *e, br_websocket ws, br_websocket_message *out)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (out == NULL) {
        return false;
    }
    if (slot == NULL || !br_websocket_conn_receive(&slot->conn, out)) {
        out->data = NULL;
        out->size = 0;
        out->text = false;
        return false;
    }
    return true;
}

void br_websocket_set_max_message(br_engine *e, br_websocket ws, size_t bytes)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot != NULL) {
        slot->max_message = bytes;
        if (slot->conn.ctx != NULL) {
            br_websocket_conn_set_max_message(&slot->conn, bytes);
        }
    }
}

void br_websocket_set_ping_interval(br_engine *e, br_websocket ws, float seconds)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot != NULL) {
        slot->ping_interval = seconds > 0.0f ? seconds : 0.0f;
        if (slot->conn.ctx != NULL) {
            br_websocket_conn_set_times(&slot->conn, br_websocket_ns(slot->ping_interval),
                                        br_websocket_ns(slot->close_timeout), SDL_GetTicksNS());
        }
    }
}

void br_websocket_set_timeouts(br_engine *e, br_websocket ws, float handshake, float close)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot != NULL) {
        slot->handshake_timeout = handshake > 0.0f ? handshake : slot->handshake_timeout;
        slot->close_timeout = close > 0.0f ? close : slot->close_timeout;
        if (slot->conn.ctx != NULL) {
            slot->conn.close_timeout = br_websocket_ns(slot->close_timeout);
        }
    }
}

/* A code that may be sent (RFC 6455 section 7.4 and the IANA registry: 1012-1014 too; our
 * patched wslay accepts the same on receive). */
static bool br_websocket_code_valid(uint16_t code)
{
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

void br_websocket_close(br_engine *e, br_websocket ws, uint16_t code, const char *reason)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot == NULL) {
        return;
    }
    if (slot->conn.ctx == NULL) {
        if (slot->state == BR_WEBSOCKET_CONNECTING) {
            slot->state = BR_WEBSOCKET_CLOSED;
            br_websocket_drop_handshake(slot);
            br_websocket_drop_stream(e, slot);
        }
        return;
    }
    if (slot->conn.state != BR_WEBSOCKET_OPEN) {
        return;
    }
    if (!br_websocket_code_valid(code)) {
        br_log_error("br_websocket_close: code %u cannot be sent (1000-1003, 1007-1014, 3000-4999): sending 1000",
                     (unsigned)code);
        code = 1000;
    }
    char text[124] = "";
    if (reason != NULL) {
        size_t length = SDL_strlen(reason);
        if (!br_websocket_utf8_valid(reason, length)) {
            br_log_error("br_websocket_close: the reason is not valid UTF-8: sent without it");
            length = 0;
        }
        if (length > 123) {
            length = 123;
            while (length > 0 && ((uint8_t)reason[length] & 0xC0) == 0x80) {
                length--;  /* cut before a whole character */
            }
        }
        SDL_memcpy(text, reason, length);
        text[length] = '\0';
    }
    br_websocket_conn_close(&slot->conn, code, text[0] != '\0' ? text : NULL, SDL_GetTicksNS());
    br_websocket_flush(e, slot);
}

uint16_t br_websocket_close_code(br_engine *e, br_websocket ws)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot == NULL) {
        return 0;
    }
    if (slot->conn.ctx != NULL) {
        return slot->conn.close_code;
    }
    return slot->state == BR_WEBSOCKET_CLOSED || slot->state == BR_WEBSOCKET_FAILED ? 1006 : 0;
}

const char *br_websocket_error(br_engine *e, br_websocket ws)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot == NULL) {
        return "";
    }
    return slot->conn.ctx != NULL ? slot->conn.error : slot->error;
}

void br_websocket_release(br_engine *e, br_websocket ws)
{
    br_websocket_slot *slot = br_websocket_slot_get(br_websocket_get_state(e), ws);
    if (slot != NULL) {
        br_websocket_free_slot(e, slot);
    }
}
