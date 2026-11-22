/* br_http_parse: the pure parts of http (br_http_internal.h). The response parser hands the status
 * line, the headers and chunked bodies to picohttpparser and keeps for itself what the library
 * leaves to its caller: limits, body framing (RFC 9112 section 6.3), interim responses, obs-fold. */
#include "br_http_internal.h"

#include "picohttpparser.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <string.h>

/* Body bytes copied into the buffer per step: bounds how far a feed can overshoot max_body. */
#define BR_HTTP_PIECE 16384

typedef enum br_http_framing {
    BR_HTTP_FRAME_HEAD,     /* reading the header block */
    BR_HTTP_FRAME_NONE,     /* no body (HEAD, 1xx, 204, 304): DONE at the end of the headers */
    BR_HTTP_FRAME_LENGTH,   /* Content-Length */
    BR_HTTP_FRAME_CHUNKED,  /* Transfer-Encoding: chunked */
    BR_HTTP_FRAME_CLOSE     /* neither: the body ends when the connection closes */
} br_http_framing;

/* A header of the final response: offsets of its NUL-terminated name and value in data. */
typedef struct br_http_header_span {
    uint32_t name;
    uint32_t value;
} br_http_header_span;

struct br_http_parser {
    char                      *data;           /* header block, then the (decoded) body */
    size_t                     size;
    size_t                     capacity;
    size_t                     header_size;    /* bytes of the final header block once parsed */
    size_t                     parsed_size;    /* bytes phr_parse_response saw last time (its last_len) */
    size_t                     interim_bytes;  /* header bytes of 1xx responses skipped so far */
    size_t                     body_size;
    size_t                     max_body;
    size_t                     unread_size;    /* FRAME_NONE: bytes after the header block */
    uint64_t                   content_length;
    br_http_framing            framing;
    br_http_parse_result       result;
    bool                       head_request;
    int                        status;
    uint32_t                   header_count;
    br_http_header_span        headers[BR_HTTP_MAX_HEADERS];
    struct phr_chunked_decoder chunked;
    char                       error[256];
};

static void br_http_set_error(char *error, size_t capacity, const char *fmt, ...)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(error, capacity, fmt, args);
    va_end(args);
}

/* RFC 9110 section 5.6.2: tchar. */
static bool br_http_is_tchar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && SDL_strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* Printable ASCII without space: what may go into a request line or Host unescaped. */
static bool br_http_is_visible(const char *text)
{
    for (const char *c = text; *c != '\0'; c++) {
        if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f) {
            return false;
        }
    }
    return true;
}

/* ---- URLs */

typedef struct br_http_scheme {
    const char *prefix;
    bool        secure;
    bool        websocket;
} br_http_scheme;

static const br_http_scheme br_http_schemes[] = {
    { "http://", false, false },
    { "https://", true, false },
    { "ws://", false, true },
    { "wss://", true, true },
};

static bool br_http_host_char(char c, bool ipv6)
{
    if (ipv6) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
    }
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

bool br_http_url_split(const char *url, br_http_url *out, bool *websocket, char *error, size_t error_capacity)
{
    if (url == NULL || out == NULL) {
        br_http_set_error(error, error_capacity, "no URL");
        return false;
    }
    SDL_zerop(out);
    const br_http_scheme *scheme = NULL;
    for (size_t i = 0; i < BR_ARRAY_COUNT(br_http_schemes); i++) {
        if (SDL_strncasecmp(url, br_http_schemes[i].prefix, SDL_strlen(br_http_schemes[i].prefix)) == 0) {
            scheme = &br_http_schemes[i];
        }
    }
    if (scheme == NULL) {
        br_http_set_error(error, error_capacity, "URL '%.64s' does not start with http://, https://, ws:// or wss://", url);
        return false;
    }
    const char *authority = url + SDL_strlen(scheme->prefix);
    size_t authority_length = strcspn(authority, "/?#");
    const char *after = authority + authority_length;
    if (memchr(authority, '@', authority_length) != NULL) {
        br_http_set_error(error, error_capacity, "URL user info (user@host) is not supported");
        return false;
    }

    /* host: a name, an IPv4 text or [IPv6] */
    const char *host = authority;
    size_t host_length = 0;
    const char *rest = NULL;  /* what follows the host inside the authority */
    bool ipv6 = authority_length > 0 && authority[0] == '[';
    if (ipv6) {
        const char *bracket = memchr(authority, ']', authority_length);  /* never past the authority */
        if (bracket == NULL) {
            br_http_set_error(error, error_capacity, "URL host '[' has no ']'");
            return false;
        }
        host = authority + 1;
        host_length = (size_t)(bracket - host);
        rest = bracket + 1;
    } else {
        host_length = strcspn(authority, ":/?#");
        rest = authority + host_length;
    }
    for (size_t i = 0; i < host_length; i++) {
        if (!br_http_host_char(host[i], ipv6)) {
            br_http_set_error(error, error_capacity, "URL host has a character that is not allowed");
            return false;
        }
    }
    if (host_length == 0 || host_length >= sizeof out->host) {
        br_http_set_error(error, error_capacity, "URL host is empty or longer than %d bytes", (int)sizeof out->host - 1);
        return false;
    }
    SDL_memcpy(out->host, host, host_length);
    out->host[host_length] = '\0';

    /* port: empty means the default (RFC 3986 section 3.2.3) */
    out->secure = scheme->secure;
    out->port = scheme->secure ? 443 : 80;
    if (rest < after) {
        if (*rest != ':') {
            br_http_set_error(error, error_capacity, "URL has text after the host");
            return false;
        }
        rest++;
        if (rest < after) {
            uint32_t port = 0;
            for (const char *c = rest; c < after; c++) {
                if (*c < '0' || *c > '9' || port > 65535) {
                    br_http_set_error(error, error_capacity, "URL port is not a number from 1 to 65535");
                    return false;
                }
                port = port * 10 + (uint32_t)(*c - '0');
            }
            if (port == 0 || port > 65535) {
                br_http_set_error(error, error_capacity, "URL port is not a number from 1 to 65535");
                return false;
            }
            out->port = (uint16_t)port;
        }
    }

    /* path and query; the fragment is not sent (RFC 9110 section 4.2.4) */
    size_t path_length = strcspn(after, "#");
    const char *prefix = (*after == '/') ? "" : "/";
    if (path_length + SDL_strlen(prefix) >= sizeof out->path) {
        br_http_set_error(error, error_capacity, "URL path is longer than %d bytes", (int)sizeof out->path - 1);
        return false;
    }
    SDL_strlcpy(out->path, prefix, sizeof out->path);
    size_t start = SDL_strlen(out->path);
    SDL_memcpy(out->path + start, after, path_length);
    out->path[start + path_length] = '\0';
    if (!br_http_is_visible(out->path)) {
        br_http_set_error(error, error_capacity, "URL path has a space or a control character (percent-encode it)");
        return false;
    }
    if (websocket != NULL) {
        *websocket = scheme->websocket;
    }
    return true;
}

bool br_http_url_parse(const char *url, br_http_url *out)
{
    char error[256];
    if (!br_http_url_split(url, out, NULL, error, sizeof error)) {
        br_log_error("br_http_url_parse: %s", error);
        return false;
    }
    return true;
}

bool br_http_url_resolve(const br_http_url *base, const char *location, br_http_url *out, char *error,
                         size_t error_capacity)
{
    if (base == NULL || location == NULL || out == NULL) {
        br_http_set_error(error, error_capacity, "no URL");
        return false;
    }
    /* Servers send spaces and raw UTF-8 in Location; like curl and browsers, percent-encode them
     * (RFC 3986 section 2.1). Other control characters stay refused by br_http_url_split. */
    char escaped[sizeof base->host + sizeof base->path];
    size_t used = 0;
    for (const unsigned char *c = (const unsigned char *)location; *c != '\0'; c++) {
        bool encode = *c == ' ' || *c >= 0x80;
        if (used + (encode ? 3 : 1) >= sizeof escaped) {
            br_http_set_error(error, error_capacity, "redirect URL is too long");
            return false;
        }
        if (encode) {
            SDL_snprintf(escaped + used, 4, "%%%02X", (unsigned)*c);
            used += 3;
        } else {
            escaped[used++] = (char)*c;
        }
    }
    escaped[used] = '\0';
    location = escaped;
    char text[sizeof base->host + sizeof base->path + 64];
    const char *scheme = base->secure ? "https" : "http";
    size_t scheme_end = strcspn(location, ":/?#");
    int length = 0;
    if (location[scheme_end] == ':') {                          /* absolute */
        length = SDL_snprintf(text, sizeof text, "%s", location);
    } else if (location[0] == '/' && location[1] == '/') {      /* scheme-relative */
        length = SDL_snprintf(text, sizeof text, "%s:%s", scheme, location);
    } else {
        bool ipv6 = SDL_strchr(base->host, ':') != NULL;
        size_t query = strcspn(base->path, "?");
        size_t directory = query;
        while (directory > 0 && base->path[directory - 1] != '/') {
            directory--;
        }
        size_t keep = 0;  /* bytes of base->path kept before location */
        if (location[0] == '\0' || location[0] == '#') {
            keep = SDL_strlen(base->path);
        } else if (location[0] == '?') {
            keep = query;
        } else if (location[0] != '/') {
            /* ponytail: "." and ".." segments are sent as they are (RFC 3986 section 5.2.4 would
             * remove them); servers resolve them, and the host cannot change this way. */
            keep = directory;
        }
        length = SDL_snprintf(text, sizeof text, "%s://%s%s%s:%u%.*s%s", scheme, ipv6 ? "[" : "", base->host,
                              ipv6 ? "]" : "", (unsigned)base->port, (int)keep, base->path, location);
    }
    if (length < 0 || (size_t)length >= sizeof text) {
        br_http_set_error(error, error_capacity, "redirect URL is too long");
        return false;
    }
    bool websocket = false;
    if (!br_http_url_split(text, out, &websocket, error, error_capacity)) {
        return false;
    }
    if (websocket) {
        br_http_set_error(error, error_capacity, "redirect to a ws:// or wss:// URL");
        return false;
    }
    return true;
}

/* ---- requests */

bool br_http_headers_valid(const char *headers, char *error, size_t error_capacity)
{
    if (headers == NULL) {
        return true;
    }
    int line = 1;
    const char *c = headers;
    while (*c != '\0') {
        const char *name = c;
        while (br_http_is_tchar(*c)) {
            c++;
        }
        if (c == name || *c != ':') {
            br_http_set_error(error, error_capacity, "header line %d is not \"Name: value\" (or is empty)", line);
            return false;
        }
        for (c++; *c != '\0' && *c != '\r' && *c != '\n'; c++) {
            unsigned char ch = (unsigned char)*c;
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                br_http_set_error(error, error_capacity, "header line %d has a control character", line);
                return false;
            }
        }
        if (*c == '\0') {
            break;  /* the last line may lack its CRLF */
        }
        if (c[0] != '\r' || c[1] != '\n') {
            /* a lone CR or LF would start a header (or a body) of the caller's text's choosing */
            br_http_set_error(error, error_capacity, "header line %d ends in a lone CR or LF, not CRLF", line);
            return false;
        }
        c += 2;
        line++;
    }
    return true;
}

bool br_http_headers_has(const char *headers, const char *name)
{
    if (headers == NULL || name == NULL) {
        return false;
    }
    size_t length = SDL_strlen(name);
    for (const char *line = headers; *line != '\0';) {
        if (SDL_strncasecmp(line, name, length) == 0 && line[length] == ':') {
            return true;
        }
        const char *end = SDL_strchr(line, '\n');
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return false;
}

/* Appends to out while it fits; len counts everything, so the caller learns the size needed. */
typedef struct br_http_writer {
    char  *out;
    size_t capacity;
    size_t length;
} br_http_writer;

static void br_http_put(br_http_writer *w, const char *text)
{
    size_t n = SDL_strlen(text);
    if (w->out != NULL && w->length + n < w->capacity) {
        SDL_memcpy(w->out + w->length, text, n);
    }
    w->length += n;
}

static void br_http_put_header(br_http_writer *w, const char *headers, const char *name, const char *value)
{
    if (!br_http_headers_has(headers, name)) {
        br_http_put(w, name);
        br_http_put(w, ": ");
        br_http_put(w, value);
        br_http_put(w, "\r\n");
    }
}

size_t br_http_write_request(char *out, size_t capacity, const char *method, const br_http_url *url,
                             const char *headers, bool send_length, size_t body_size, char *error,
                             size_t error_capacity)
{
    if (method == NULL) {
        method = "GET";
    }
    size_t method_length = SDL_strlen(method);
    bool token = method_length > 0 && method_length <= BR_HTTP_MAX_METHOD;
    for (size_t i = 0; token && i < method_length; i++) {
        token = br_http_is_tchar(method[i]);
    }
    if (!token) {
        br_http_set_error(error, error_capacity, "method is not a token of 1 to %d letters", BR_HTTP_MAX_METHOD);
        return 0;
    }
    if (SDL_strcmp(method, "CONNECT") == 0) {  /* a tunnel, not a request (RFC 9110 section 9.3.6) */
        br_http_set_error(error, error_capacity, "CONNECT is not supported");
        return 0;
    }
    if (url == NULL || url->host[0] == '\0' || url->path[0] != '/' || !br_http_is_visible(url->host) ||
        !br_http_is_visible(url->path)) {
        br_http_set_error(error, error_capacity, "URL host or path is empty or has a space or a control character");
        return 0;
    }
    if (!br_http_headers_valid(headers, error, error_capacity)) {
        return 0;
    }
    br_http_writer w = { out, capacity, 0 };
    br_http_put(&w, method);
    br_http_put(&w, " ");
    br_http_put(&w, url->path);
    br_http_put(&w, " HTTP/1.1\r\n");

    char host[sizeof url->host + 16];
    bool ipv6 = SDL_strchr(url->host, ':') != NULL;
    bool default_port = url->port == (url->secure ? 443 : 80);
    char port[8] = "";
    if (!default_port) {
        SDL_snprintf(port, sizeof port, ":%u", (unsigned)url->port);
    }
    SDL_snprintf(host, sizeof host, "%s%s%s%s", ipv6 ? "[" : "", url->host, ipv6 ? "]" : "", port);
    br_http_put_header(&w, headers, "Host", host);
    if (headers != NULL && headers[0] != '\0') {
        br_http_put(&w, headers);
        size_t length = SDL_strlen(headers);
        if (length < 2 || headers[length - 2] != '\r' || headers[length - 1] != '\n') {
            br_http_put(&w, "\r\n");
        }
    }
    br_http_put_header(&w, headers, "User-Agent", "braty");
    br_http_put_header(&w, headers, "Accept-Encoding", "identity");
    br_http_put_header(&w, headers, "Connection", "close");
    if (send_length) {
        char length[32];
        SDL_snprintf(length, sizeof length, "%llu", (unsigned long long)body_size);
        br_http_put_header(&w, headers, "Content-Length", length);
    }
    br_http_put(&w, "\r\n");
    if (w.out != NULL && w.length < w.capacity) {
        w.out[w.length] = '\0';
    }
    return w.length;
}

/* ---- responses */

static void br_http_fail(br_http_parser *p, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(p->error, sizeof p->error, fmt, args);
    va_end(args);
    p->result = BR_HTTP_PARSE_ERROR;
}

/* Room for `need` bytes plus a NUL. */
static bool br_http_reserve(br_http_parser *p, size_t need)
{
    if (need >= SIZE_MAX / 2) {  /* the doubling below would wrap to 0 and never end */
        br_http_fail(p, "response of %llu bytes is too large", (unsigned long long)need);
        return false;
    }
    need += 1;
    if (need <= p->capacity) {
        return true;
    }
    size_t capacity = p->capacity < 4096 ? 4096 : p->capacity;
    while (capacity < need) {
        capacity *= 2;
    }
    char *data = br_realloc(p->data, capacity);
    if (data == NULL) {
        br_http_fail(p, "out of memory for %llu response bytes", (unsigned long long)need);
        return false;
    }
    p->data = data;
    p->capacity = capacity;
    return true;
}

br_http_parser *br_http_parser_create(size_t max_body, bool head_request)
{
    br_http_parser *p = br_alloc(sizeof(br_http_parser));
    if (p == NULL) {
        return NULL;
    }
    SDL_zerop(p);
    br_http_parser_reset(p, max_body, head_request);
    return p;
}

void br_http_parser_destroy(br_http_parser *p)
{
    if (p != NULL) {
        br_free(p->data);
        br_free(p);
    }
}

void br_http_parser_reset(br_http_parser *p, size_t max_body, bool head_request)
{
    if (p == NULL) {
        return;
    }
    char *data = p->data;
    size_t capacity = p->capacity;
    SDL_zerop(p);
    p->data = data;
    p->capacity = capacity;
    p->max_body = max_body != 0 ? max_body : BR_HTTP_DEFAULT_MAX_BODY;
    p->head_request = head_request;
    p->framing = BR_HTTP_FRAME_HEAD;
    p->result = BR_HTTP_PARSE_MORE;
    p->chunked.consume_trailer = 1;  /* trailers are read and dropped (RFC 9112 section 7.1.2 allows it) */
}

static void br_http_done(br_http_parser *p)
{
    p->result = BR_HTTP_PARSE_DONE;
    if (p->framing != BR_HTTP_FRAME_NONE) {
        p->data[p->header_size + p->body_size] = '\0';  /* br_http_reserve kept room */
    }
}

static const char *br_http_find(const br_http_parser *p, const char *name, uint32_t from, uint32_t *index)
{
    for (uint32_t i = from; i < p->header_count; i++) {
        if (SDL_strcasecmp(p->data + p->headers[i].name, name) == 0) {
            *index = i;
            return p->data + p->headers[i].value;
        }
    }
    return NULL;
}

/* RFC 9110 section 8.6: Content-Length = 1*DIGIT. Every Content-Length line must carry the same
 * number (section 8.6 lets a recipient accept repeats of one value; different ones are an
 * unrecoverable framing error, RFC 9112 section 6.3 item 5). */
static bool br_http_content_length(br_http_parser *p, bool *present)
{
    *present = false;
    uint32_t index = 0;
    for (const char *value = br_http_find(p, "Content-Length", 0, &index); value != NULL;
         value = br_http_find(p, "Content-Length", index + 1, &index)) {
        uint64_t number = 0;
        if (value[0] == '\0') {
            br_http_fail(p, "empty Content-Length");
            return false;
        }
        for (const char *c = value; *c != '\0'; c++) {
            if (*c < '0' || *c > '9') {
                br_http_fail(p, "Content-Length \"%.32s\" is not a number", value);
                return false;
            }
            if (number > (UINT64_MAX - 9) / 10) {
                br_http_fail(p, "Content-Length \"%.32s\" is too large", value);
                return false;
            }
            number = number * 10 + (uint64_t)(*c - '0');
        }
        if (*present && number != p->content_length) {
            br_http_fail(p, "two different Content-Length values");
            return false;
        }
        p->content_length = number;
        *present = true;
    }
    return true;
}

/* RFC 9112 section 6.3: how the body of the final response ends. */
static void br_http_choose_framing(br_http_parser *p)
{
    /* item 1: HEAD responses, 1xx, 204 and 304 have no body whatever their headers say */
    if (p->head_request || p->status / 100 == 1 || p->status == 204 || p->status == 304) {
        p->framing = BR_HTTP_FRAME_NONE;
        p->unread_size = p->size - p->header_size;
        br_http_done(p);
        return;
    }
    uint32_t index = 0;
    const char *coding = br_http_find(p, "Transfer-Encoding", 0, &index);
    if (coding != NULL) {
        /* item 3/4: only "chunked" (we asked for no other coding); a second Transfer-Encoding
         * line would apply a second coding */
        uint32_t second = 0;
        if (SDL_strcasecmp(coding, "chunked") != 0 || br_http_find(p, "Transfer-Encoding", index + 1, &second) != NULL) {
            br_http_fail(p, "unsupported Transfer-Encoding \"%.32s\"", coding);
            return;
        }
        /* item 3: with Content-Length too, Transfer-Encoding wins and Content-Length is ignored;
         * such a message must not reuse the connection, and ours never does */
        p->framing = BR_HTTP_FRAME_CHUNKED;
        return;
    }
    bool present = false;
    if (!br_http_content_length(p, &present)) {
        return;
    }
    if (!present) {
        p->framing = BR_HTTP_FRAME_CLOSE;  /* item 8 */
        return;
    }
    if (p->content_length > p->max_body || p->content_length >= SIZE_MAX / 2) {  /* header + body must fit */
        br_http_fail(p, "body of %llu bytes is over the limit of %llu", (unsigned long long)p->content_length,
                     (unsigned long long)p->max_body);
        return;
    }
    p->framing = BR_HTTP_FRAME_LENGTH;
    br_http_reserve(p, p->header_size + (size_t)p->content_length);  /* the whole body at once */
}

/* The final response's headers, from picohttpparser's list (pointers into data): obs-fold lines
 * joined to the line before, then each name and value NUL-terminated in place. */
static void br_http_take_headers(br_http_parser *p, const struct phr_header *list, size_t count)
{
    char *value_start[BR_HTTP_MAX_HEADERS];
    size_t value_length[BR_HTTP_MAX_HEADERS];
    char *name_end[BR_HTTP_MAX_HEADERS];
    uint32_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (list[i].name == NULL) {
            /* obs-fold (RFC 9112 section 5.2): a user agent replaces it with SP. The bytes from
             * the previous value's end to this line's text (CR LF and the leading SP / HT) become
             * spaces, so the value runs on. */
            if (n == 0) {
                continue;  /* picohttpparser never starts with one */
            }
            char *end = value_start[n - 1] + value_length[n - 1];
            const char *text_end = list[i].value + list[i].value_len;
            for (char *c = end; c < text_end && (c < list[i].value || *c == ' ' || *c == '	'); c++) {
                *c = ' ';
            }
            value_length[n - 1] = (size_t)(list[i].value + list[i].value_len - value_start[n - 1]);
            while (value_length[n - 1] > 0 &&
                   (value_start[n - 1][value_length[n - 1] - 1] == ' ' || value_start[n - 1][value_length[n - 1] - 1] == '\t')) {
                value_length[n - 1]--;
            }
            continue;
        }
        name_end[n] = (char *)list[i].name + list[i].name_len;  /* the ':' */
        value_start[n] = (char *)list[i].value;
        value_length[n] = list[i].value_len;
        p->headers[n].name = (uint32_t)(list[i].name - p->data);
        p->headers[n].value = (uint32_t)(list[i].value - p->data);
        n++;
    }
    /* Each ':' and the byte after each value (space, tab, CR or LF) become the NULs. */
    for (uint32_t i = 0; i < n; i++) {
        *name_end[i] = '\0';
        value_start[i][value_length[i]] = '\0';
    }
    p->header_count = n;
}

static void br_http_body_in_place(br_http_parser *p, size_t raw);

/* Parses the header block in data; on the final response's headers, chooses the framing and
 * takes the body bytes that came with them. */
static void br_http_parse_head(br_http_parser *p)
{
    while (p->result == BR_HTTP_PARSE_MORE && p->framing == BR_HTTP_FRAME_HEAD && p->size > 0) {
        size_t limit = BR_HTTP_MAX_HEADER_BYTES - p->interim_bytes;
        size_t length = p->size < limit ? p->size : limit;
        struct phr_header list[BR_HTTP_MAX_HEADERS];
        size_t count = BR_HTTP_MAX_HEADERS;
        int minor = 0, status = 0;
        const char *message = NULL;
        size_t message_length = 0;
        size_t last = p->parsed_size < length ? p->parsed_size : 0;
        int r = phr_parse_response(p->data, length, &minor, &status, &message, &message_length, list, &count, last);
        p->parsed_size = length;
        if (r == -2) {
            if (length >= limit) {
                br_http_fail(p, "response header block is over %d bytes", BR_HTTP_MAX_HEADER_BYTES);
            }
            return;
        }
        if (r < 0) {
            br_http_fail(p, "malformed status line or header (or more than %d headers)", BR_HTTP_MAX_HEADERS);
            return;
        }
        size_t end = (size_t)r;
        if (status / 100 == 1 && status != 101) {
            /* interim (100 Continue, 103 Early Hints): skipped, the final response follows
             * (RFC 9110 section 15.2); their bytes count against the header limit */
            p->interim_bytes += end;
            if (p->interim_bytes >= BR_HTTP_MAX_HEADER_BYTES) {
                br_http_fail(p, "response header block is over %d bytes", BR_HTTP_MAX_HEADER_BYTES);
                return;
            }
            SDL_memmove(p->data, p->data + end, p->size - end);
            p->size -= end;
            p->parsed_size = 0;
            continue;
        }
        p->status = status;
        p->header_size = end;
        br_http_take_headers(p, list, count);
        br_http_choose_framing(p);
        if (p->result == BR_HTTP_PARSE_MORE) {
            size_t raw = p->size - p->header_size;
            p->size = p->header_size;
            br_http_body_in_place(p, raw);
        }
    }
}

/* raw new body bytes sit at data + header_size + body_size. */
static void br_http_body_in_place(br_http_parser *p, size_t raw)
{
    char *at = p->data + p->header_size + p->body_size;
    switch (p->framing) {
    case BR_HTTP_FRAME_LENGTH: {
        uint64_t want = p->content_length - p->body_size;
        size_t take = raw < want ? raw : (size_t)want;  /* bytes after the body are dropped */
        p->body_size += take;
        p->size = p->header_size + p->body_size;
        if (p->body_size == p->content_length) {
            br_http_done(p);
        }
        break;
    }
    case BR_HTTP_FRAME_CLOSE:
        p->body_size += raw;
        p->size = p->header_size + p->body_size;
        if (p->body_size > p->max_body) {
            br_http_fail(p, "body is over the limit of %llu bytes", (unsigned long long)p->max_body);
        }
        break;
    case BR_HTTP_FRAME_CHUNKED: {
        size_t decoded = raw;
        ptrdiff_t r = phr_decode_chunked(&p->chunked, at, &decoded);  /* in place */
        if (r == -1) {
            br_http_fail(p, "malformed chunked body");
            return;
        }
        p->body_size += decoded;
        p->size = p->header_size + p->body_size;
        /* the library takes any chunk size that fits a size_t: the limit is ours */
        if (p->body_size > p->max_body || p->chunked.bytes_left_in_chunk > p->max_body - p->body_size) {
            br_http_fail(p, "chunked body is over the limit of %llu bytes", (unsigned long long)p->max_body);
            return;
        }
        if (r >= 0) {
            br_http_done(p);  /* bytes after the last chunk and trailers are dropped */
        }
        break;
    }
    default:
        break;
    }
}

br_http_parse_result br_http_parser_feed(br_http_parser *p, const void *data, size_t size)
{
    if (p == NULL) {
        return BR_HTTP_PARSE_ERROR;
    }
    if (p->result != BR_HTTP_PARSE_MORE || size == 0) {
        return p->result;
    }
    if (data == NULL) {
        br_http_fail(p, "no data");
        return p->result;
    }
    const char *in = data;
    while (size > 0 && p->result == BR_HTTP_PARSE_MORE) {
        size_t take = 0;
        if (p->framing == BR_HTTP_FRAME_HEAD) {
            /* never more than the header limit in the buffer before the headers are complete */
            size_t limit = BR_HTTP_MAX_HEADER_BYTES - p->interim_bytes;
            size_t room = limit - p->size;
            take = size < room ? size : room;
            if (!br_http_reserve(p, p->size + take)) {
                break;
            }
            SDL_memcpy(p->data + p->size, in, take);
            p->size += take;
            br_http_parse_head(p);
        } else {
            take = size < BR_HTTP_PIECE ? size : BR_HTTP_PIECE;
            if (p->framing == BR_HTTP_FRAME_LENGTH && take > p->content_length - p->body_size) {
                take = (size_t)(p->content_length - p->body_size);
            }
            if (!br_http_reserve(p, p->header_size + p->body_size + take)) {
                break;
            }
            SDL_memcpy(p->data + p->header_size + p->body_size, in, take);
            br_http_body_in_place(p, take);
            if (p->framing == BR_HTTP_FRAME_LENGTH && p->result == BR_HTTP_PARSE_DONE) {
                take = size;  /* the rest is past the body: dropped */
            }
        }
        in += take;
        size -= take;
    }
    if (p->result == BR_HTTP_PARSE_DONE && p->framing == BR_HTTP_FRAME_NONE && size > 0 &&
        br_http_reserve(p, p->size + size)) {
        SDL_memcpy(p->data + p->size, in, size);  /* a 101's first frames */
        p->size += size;
        p->unread_size += size;
    }
    return p->result;
}

br_http_parse_result br_http_parser_finish(br_http_parser *p)
{
    if (p == NULL) {
        return BR_HTTP_PARSE_ERROR;
    }
    if (p->result != BR_HTTP_PARSE_MORE) {
        return p->result;
    }
    switch (p->framing) {
    case BR_HTTP_FRAME_HEAD:
        br_http_fail(p, p->size == 0 && p->interim_bytes == 0 ? "connection closed before a response"
                                                               : "connection closed inside the response headers");
        break;
    case BR_HTTP_FRAME_LENGTH:
        br_http_fail(p, "connection closed after %llu of %llu body bytes", (unsigned long long)p->body_size,
                     (unsigned long long)p->content_length);
        break;
    case BR_HTTP_FRAME_CHUNKED:
        br_http_fail(p, "connection closed inside the chunked body (after %llu bytes)", (unsigned long long)p->body_size);
        break;
    default:
        br_http_done(p);  /* FRAME_CLOSE: the close ends the body */
        break;
    }
    return p->result;
}

br_http_parse_result br_http_parser_result(const br_http_parser *p)
{
    return p != NULL ? p->result : BR_HTTP_PARSE_ERROR;
}

int br_http_parser_status(const br_http_parser *p)
{
    return p != NULL ? p->status : 0;
}

const char *br_http_parser_header(const br_http_parser *p, const char *name)
{
    uint32_t index = 0;
    return p != NULL && name != NULL ? br_http_find(p, name, 0, &index) : NULL;
}

uint32_t br_http_parser_header_count(const br_http_parser *p)
{
    return p != NULL ? p->header_count : 0;
}

bool br_http_parser_header_at(const br_http_parser *p, uint32_t index, const char **name, const char **value)
{
    if (p == NULL || index >= p->header_count) {
        return false;
    }
    if (name != NULL) {
        *name = p->data + p->headers[index].name;
    }
    if (value != NULL) {
        *value = p->data + p->headers[index].value;
    }
    return true;
}

const char *br_http_parser_body(const br_http_parser *p, size_t *size)
{
    bool has_body = p != NULL && p->framing != BR_HTTP_FRAME_HEAD && p->framing != BR_HTTP_FRAME_NONE &&
                    p->body_size > 0;
    if (size != NULL) {
        *size = has_body ? p->body_size : 0;
    }
    if (!has_body) {
        return "";
    }
    p->data[p->header_size + p->body_size] = '\0';  /* room kept by br_http_reserve */
    return p->data + p->header_size;
}

const void *br_http_parser_unread(const br_http_parser *p, size_t *size)
{
    bool any = p != NULL && p->framing == BR_HTTP_FRAME_NONE && p->unread_size > 0;
    if (size != NULL) {
        *size = any ? p->unread_size : 0;
    }
    return any ? p->data + p->header_size : NULL;
}

float br_http_parser_progress(const br_http_parser *p)
{
    if (p == NULL) {
        return 0.0f;
    }
    if (p->result == BR_HTTP_PARSE_DONE) {
        return 1.0f;
    }
    if (p->framing != BR_HTTP_FRAME_LENGTH || p->content_length == 0) {
        return 0.0f;
    }
    return (float)((double)p->body_size / (double)p->content_length);
}

const char *br_http_parser_error(const br_http_parser *p)
{
    return p != NULL && p->result == BR_HTTP_PARSE_ERROR ? p->error : "";
}
