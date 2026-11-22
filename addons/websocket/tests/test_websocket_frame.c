/* websocket without sockets: the handshake helpers (accept key, request, every client check of
 * RFC 6455 section 4.1), UTF-8, and br_websocket_conn (our wrapper around wslay) driven through
 * in-memory bytes with a test clock: RFC 6455 section 5.7 examples byte for byte, round trips at
 * payload sizes 0 / 125 / 126 / 65535 / 65536, masking, one byte at a time, every protocol error
 * and the close code it sends, the closing handshake both ways, messages readable after CLOSED,
 * a peer that vanishes, the close timeout, the keep-alive, the unread limit. */
#include "braty/braty.h"
#include "br_test.h"
#include "br_websocket_internal.h"
#include "ws_test_frames.h"

#include <SDL3/SDL.h>

#define SECOND 1000000000ull

static const uint8_t rfc_mask[4] = { 0x37, 0xfa, 0x21, 0x3d };   /* RFC 6455 section 5.7 */
static uint8_t wire[4 * WS_TEST_MAX_PAYLOAD];
static ws_test_frame frames[8];
static uint8_t big[1100000];

/* Masks for the conn: the RFC's, repeated. */
static bool fixed_random(void *user, void *out, size_t size)
{
    const uint8_t *mask = user;
    uint8_t *p = out;
    for (size_t i = 0; i < size; i++) {
        p[i] = mask[i % 4];
    }
    return true;
}

/* Masks for the conn: a counter, so every frame gets another one. */
static bool counting_random(void *user, void *out, size_t size)
{
    uint8_t *counter = user;
    uint8_t *p = out;
    for (size_t i = 0; i < size; i++) {
        p[i] = (uint8_t)(++*counter * 37u + 11u);
    }
    return true;
}

static void open_conn(br_websocket_conn *c)
{
    BR_CHECK(br_websocket_conn_init(c, fixed_random, (void *)rfc_mask, 0));
    BR_CHECK(c->state == BR_WEBSOCKET_OPEN);
}

static void feed(br_websocket_conn *c, const void *data, size_t size)
{
    br_websocket_conn_feed(c, data, size, 0);
}

/* Reads every frame of the conn's output into frames[] and clears it: the count (-1 when the
 * output is not whole frames). */
static int output_frames(br_websocket_conn *c)
{
    size_t size = 0;
    const uint8_t *out = br_websocket_conn_output(c, &size);
    int count = 0;
    size_t at = 0;
    while (at < size && count < (int)BR_ARRAY_COUNT(frames)) {
        size_t n = ws_test_frame_read(out + at, size - at, &frames[count]);
        if (n == 0) {
            break;
        }
        at += n;
        count++;
    }
    br_websocket_conn_output_clear(c);
    return at == size ? count : -1;
}

/* The bytes of the conn's output equal expected (and the output is cleared). */
static bool output_is(br_websocket_conn *c, const uint8_t *expected, size_t size)
{
    size_t got = 0;
    const void *out = br_websocket_conn_output(c, &got);
    bool same = got == size && out != NULL && SDL_memcmp(out, expected, size) == 0;
    br_websocket_conn_output_clear(c);
    return same;
}

static bool receive_is(br_websocket_conn *c, const char *text, bool is_text)
{
    br_websocket_message m;
    SDL_zero(m);
    if (!br_websocket_conn_receive(c, &m)) {
        return false;
    }
    size_t size = SDL_strlen(text);
    return m.size == size && m.text == is_text && SDL_memcmp(m.data, text, size) == 0 &&
           ((const char *)m.data)[size] == '\0';
}

static bool contains(const char *text, const char *part)
{
    return text != NULL && SDL_strstr(text, part) != NULL;
}

/* ---- the opening handshake */

static void test_keys(void)
{
    char accept[BR_WEBSOCKET_ACCEPT_LENGTH + 1] = "";
    br_websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept);   /* RFC 6455 section 1.3 */
    BR_CHECK(SDL_strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    uint8_t random16[16];
    for (int i = 0; i < 16; i++) {
        random16[i] = (uint8_t)(i * 17);
    }
    char key[BR_WEBSOCKET_KEY_LENGTH + 1] = "";
    br_websocket_key_text(random16, key);
    BR_CHECK(SDL_strlen(key) == BR_WEBSOCKET_KEY_LENGTH && key[22] == '=' && key[23] == '=');
}

static void test_utf8(void)
{
    BR_CHECK(br_websocket_utf8_valid("", 0));
    BR_CHECK(br_websocket_utf8_valid("hello", 5));
    BR_CHECK(br_websocket_utf8_valid("\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5", 11));   /* kosme */
    BR_CHECK(br_websocket_utf8_valid("\xf4\x8f\xbf\xbf", 4));                               /* U+10FFFF */
    BR_CHECK(br_websocket_utf8_valid("\xef\xbf\xbd", 3));                                   /* U+FFFD */
    BR_CHECK(!br_websocket_utf8_valid("\xc3\x28", 2));
    BR_CHECK(!br_websocket_utf8_valid("\xc0\xaf", 2));           /* overlong '/' */
    BR_CHECK(!br_websocket_utf8_valid("\xe0\x80\xaf", 3));       /* overlong */
    BR_CHECK(!br_websocket_utf8_valid("\xed\xa0\x80", 3));       /* surrogate */
    BR_CHECK(!br_websocket_utf8_valid("\xf4\x90\x80\x80", 4));   /* over U+10FFFF */
    BR_CHECK(!br_websocket_utf8_valid("\xe2\x82", 2));           /* truncated */
    BR_CHECK(!br_websocket_utf8_valid("\x80", 1));
    BR_CHECK(!br_websocket_utf8_valid("\xfe", 1));
}

static void test_protocols(void)
{
    char error[256] = "";
    BR_CHECK(br_websocket_protocols_valid(NULL, error, sizeof error));
    BR_CHECK(br_websocket_protocols_valid("chat", error, sizeof error));
    BR_CHECK(br_websocket_protocols_valid("chat, v2.braty", error, sizeof error));
    BR_CHECK(!br_websocket_protocols_valid("", error, sizeof error));
    BR_CHECK(!br_websocket_protocols_valid("chat,,v2", error, sizeof error));
    BR_CHECK(!br_websocket_protocols_valid("a b", error, sizeof error));
    BR_CHECK(!br_websocket_protocols_valid("x;y", error, sizeof error));
    BR_CHECK(!br_websocket_protocols_valid("chat\r\nX: 1", error, sizeof error));
}

static void test_write_handshake(void)
{
    br_http_url url;
    BR_CHECK(br_http_url_split("ws://example.com:8080/chat?x=1", &url, NULL, NULL, 0));
    char error[256] = "";
    char out[2048] = "";
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    size_t size = br_websocket_write_handshake(out, sizeof out, &url, key, "chat, v2", "Origin: https://game\r\n",
                                               error, sizeof error);
    BR_CHECK(size > 0 && size < sizeof out);
    BR_CHECK(SDL_strncmp(out, "GET /chat?x=1 HTTP/1.1\r\n", 24) == 0);
    BR_CHECK(contains(out, "\r\nHost: example.com:8080\r\n"));
    BR_CHECK(contains(out, "\r\nUpgrade: websocket\r\n"));
    BR_CHECK(contains(out, "\r\nConnection: Upgrade\r\n"));
    BR_CHECK(contains(out, "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"));
    BR_CHECK(contains(out, "\r\nSec-WebSocket-Version: 13\r\n"));
    BR_CHECK(contains(out, "\r\nSec-WebSocket-Protocol: chat, v2\r\n"));
    BR_CHECK(contains(out, "\r\nOrigin: https://game\r\n"));
    BR_CHECK(!contains(out, "Connection: close") && !contains(out, "Content-Length"));
    BR_CHECK(size >= 4 && SDL_strcmp(out + size - 4, "\r\n\r\n") == 0);
    /* the size first, then the text */
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, NULL, NULL, error, sizeof error) > 0);
    /* refused: our headers, injection, bad protocols */
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, NULL, "Connection: keep-alive\r\n", error,
                                          sizeof error) == 0);
    BR_CHECK(contains(error, "Connection"));
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, NULL, "sec-websocket-key: x\r\n", error,
                                          sizeof error) == 0);
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, NULL, "Sec-WebSocket-Protocol: chat\r\n", error,
                                          sizeof error) == 0);
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, NULL, "X: 1\r\n\r\nGET / HTTP/1.1\r\n", error,
                                          sizeof error) == 0);
    BR_CHECK(br_websocket_write_handshake(NULL, 0, &url, key, "a b", NULL, error, sizeof error) == 0);
}

/* br_websocket_handshake_check of a canned response for key "dGhlIHNhbXBsZSBub25jZQ==". */
static bool check_response(const char *response, const char *protocols, char *error, size_t capacity)
{
    br_http_parser *p = br_http_parser_create(0, false);
    BR_CHECK(p != NULL);
    br_http_parse_result r = br_http_parser_feed(p, response, SDL_strlen(response));
    bool ok = false;
    if (r == BR_HTTP_PARSE_DONE) {
        ok = br_websocket_handshake_check(p, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", protocols, error, capacity);
    } else {
        SDL_snprintf(error, capacity, "parser: %d %s", (int)r, br_http_parser_error(p));
    }
    br_http_parser_destroy(p);
    return ok;
}

static void test_handshake_checks(void)
{
    char error[256] = "";
#define HEAD "HTTP/1.1 101 Switching Protocols\r\n"
#define ACCEPT "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    BR_CHECK(check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT "\r\n", NULL, error,
                            sizeof error));
    /* case-insensitive, Connection as a token list */
    BR_CHECK(check_response(HEAD "upgrade: WebSocket\r\nconnection: keep-alive, UPGRADE\r\n" ACCEPT "\r\n", NULL,
                            error, sizeof error));
    BR_CHECK(check_response(HEAD "Upgrade: websocket\r\nConnection: keep-alive\r\nConnection: Upgrade\r\n" ACCEPT
                            "\r\n", NULL, error, sizeof error));
    /* not 101 */
    BR_CHECK(!check_response("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "403"));
    BR_CHECK(!check_response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "200"));
    /* Upgrade */
    BR_CHECK(!check_response(HEAD "Connection: Upgrade\r\n" ACCEPT "\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "Upgrade"));
    BR_CHECK(!check_response(HEAD "Upgrade: h2c\r\nConnection: Upgrade\r\n" ACCEPT "\r\n", NULL, error, sizeof error));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket2\r\nConnection: Upgrade\r\n" ACCEPT "\r\n", NULL, error,
                             sizeof error));
    /* Connection */
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\n" ACCEPT "\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "Connection"));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: keep-alive\r\n" ACCEPT "\r\n", NULL, error,
                             sizeof error));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: upgraded\r\n" ACCEPT "\r\n", NULL, error,
                             sizeof error));
    /* Sec-WebSocket-Accept: exactly one, exactly the value */
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "Accept"));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                             "s3pPLMBiTxaQ9kYGzzhZRbK+xOo\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                             "S3PPLMBITXAQ9KYGZZHZRBK+XOO=\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT ACCEPT "\r\n", NULL, error,
                             sizeof error));
    /* extensions: we offer none */
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT
                             "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n", NULL, error, sizeof error));
    BR_CHECK(contains(error, "extensions"));
    /* protocols: only one we offered */
    const char *chat = HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT "Sec-WebSocket-Protocol: chat\r\n\r\n";
    BR_CHECK(!check_response(chat, NULL, error, sizeof error));
    BR_CHECK(contains(error, "chat"));
    BR_CHECK(check_response(chat, "chat, v2", error, sizeof error));
    BR_CHECK(check_response(chat, "v2,chat", error, sizeof error));
    BR_CHECK(!check_response(chat, "v2, v3", error, sizeof error));
    BR_CHECK(!check_response(chat, "Chat", error, sizeof error));
    BR_CHECK(!check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT
                             "Sec-WebSocket-Protocol: chat, v2\r\n\r\n", "chat, v2", error, sizeof error));
    /* no protocol chosen although offered: fine (RFC 6455 section 4.1) */
    BR_CHECK(check_response(HEAD "Upgrade: websocket\r\nConnection: Upgrade\r\n" ACCEPT "\r\n", "chat", error,
                            sizeof error));
#undef HEAD
#undef ACCEPT
}

/* ---- the connection */

static void test_rfc_examples(void)
{
    static const uint8_t hello_masked[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    static const uint8_t pong_masked[] = { 0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    static const uint8_t hello[] = { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    static const uint8_t fragments[] = { 0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f };
    static const uint8_t ping[] = { 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    br_websocket_conn c;
    open_conn(&c);
    feed(&c, hello, sizeof hello);                                 /* a single-frame unmasked text message */
    BR_CHECK(receive_is(&c, "Hello", true));
    BR_CHECK(br_websocket_conn_send(&c, true, "Hello", 5));         /* the same, masked, from the client */
    BR_CHECK(output_is(&c, hello_masked, sizeof hello_masked));
    feed(&c, fragments, sizeof fragments);                         /* a fragmented unmasked text message */
    BR_CHECK(receive_is(&c, "Hello", true));
    feed(&c, ping, sizeof ping);                                   /* unmasked ping -> masked pong */
    BR_CHECK(output_is(&c, pong_masked, sizeof pong_masked));
    BR_CHECK(!br_websocket_conn_receive(&c, &(br_websocket_message){ 0 }));   /* pings are not messages */
    /* 256 bytes and 64 KiB binary messages in a single unmasked frame */
    static uint8_t binary[65536];
    for (size_t i = 0; i < sizeof binary; i++) {
        binary[i] = (uint8_t)(i * 7);
    }
    size_t n = ws_test_frame_write(wire, 0x82, binary, 256);
    BR_CHECK(n == 260 && wire[1] == 0x7E && wire[2] == 0x01 && wire[3] == 0x00);
    feed(&c, wire, n);
    n = ws_test_frame_write(wire, 0x82, binary, 65536);
    static const uint8_t long_header[] = { 0x82, 0x7F, 0, 0, 0, 0, 0, 1, 0, 0 };
    BR_CHECK(n == 65546 && SDL_memcmp(wire, long_header, sizeof long_header) == 0);
    feed(&c, wire, n);
    br_websocket_message m;
    SDL_zero(m);
    BR_CHECK(br_websocket_conn_receive(&c, &m) && m.size == 256 && !m.text && SDL_memcmp(m.data, binary, 256) == 0);
    BR_CHECK(br_websocket_conn_receive(&c, &m) && m.size == 65536 && SDL_memcmp(m.data, binary, 65536) == 0);
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN);
    br_websocket_conn_free(&c);
}

static void test_round_trips(void)
{
    static const size_t sizes[] = { 0, 125, 126, 65535, 65536 };
    static uint8_t data[65536];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 13 + 5);
    }
    uint8_t counter = 0;
    br_websocket_conn c;
    BR_CHECK(br_websocket_conn_init(&c, counting_random, &counter, 0));
    uint8_t last_mask[4] = { 0, 0, 0, 0 };
    for (size_t k = 0; k < BR_ARRAY_COUNT(sizes); k++) {
        size_t size = sizes[k];
        /* encode: the client's frame */
        size_t header_size = size <= 125 ? 2 : size <= 65535 ? 4 : 10;
        size_t got = 0;
        BR_CHECK(br_websocket_conn_send(&c, false, data, size));
        br_websocket_conn_output(&c, &got);
        BR_CHECK(got == header_size + 4 + size);
        BR_CHECK(output_frames(&c) == 1);
        ws_test_frame *f = &frames[0];
        BR_CHECK(f->fin && f->opcode == 2 && f->rsv == 0 && f->masked && f->length == size);
        BR_CHECK(size == 0 || SDL_memcmp(f->payload, data, size) == 0);
        BR_CHECK(SDL_memcmp(f->mask, last_mask, 4) != 0);   /* a fresh mask per frame */
        SDL_memcpy(last_mask, f->mask, 4);
        /* decode: the server's frame, at once and one byte at a time */
        size_t n = ws_test_frame_write(wire, 0x82, data, size);
        BR_CHECK(n == header_size + size);
        feed(&c, wire, n);
        for (size_t i = 0; i < n; i++) {
            feed(&c, wire + i, 1);
        }
        for (int twice = 0; twice < 2; twice++) {
            br_websocket_message m;
            SDL_zero(m);
            BR_CHECK(br_websocket_conn_receive(&c, &m) && m.size == size && !m.text);
            BR_CHECK(size == 0 || SDL_memcmp(m.data, data, size) == 0);
        }
    }
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN);
    br_websocket_conn_free(&c);
}

static void test_one_byte_at_a_time(void)
{
    /* text in 3 fragments with a ping between them, then a close: fed one byte at a time */
    size_t n = 0;
    n += ws_test_frame_write(wire + n, 0x01, "frag", 4);
    n += ws_test_frame_write(wire + n, 0x89, "p", 1);
    n += ws_test_frame_write(wire + n, 0x00, "men", 3);
    n += ws_test_frame_write(wire + n, 0x80, "ted", 3);
    n += ws_test_close_write(wire + n, 1000, "done");
    br_websocket_conn c;
    open_conn(&c);
    for (size_t i = 0; i < n; i++) {
        feed(&c, wire + i, 1);
    }
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1000);
    BR_CHECK(receive_is(&c, "fragmented", true));
    BR_CHECK(output_frames(&c) == 2);
    BR_CHECK(frames[0].opcode == 0xA && frames[0].masked && SDL_strcmp((const char *)frames[0].payload, "p") == 0);
    BR_CHECK(frames[1].opcode == 0x8 && ws_test_close_code(&frames[1]) == 1000);
    br_websocket_conn_free(&c);
}

/* Feeds bytes to a new conn (max_message when not 0): it FAILED and sent a close frame with code. */
static void expect_failure(const uint8_t *bytes, size_t size, size_t max_message, uint16_t code, const char *what)
{
    br_websocket_conn c;
    open_conn(&c);
    if (max_message != 0) {
        br_websocket_conn_set_max_message(&c, max_message);
    }
    feed(&c, bytes, size);
    bool ok = c.state == BR_WEBSOCKET_FAILED && c.close_code == 1006 && c.error[0] != '\0';
    int count = output_frames(&c);
    ok = ok && count >= 1 && frames[count - 1].opcode == 0x8 && frames[count - 1].masked &&
         ws_test_close_code(&frames[count - 1]) == code;
    if (!ok) {
        SDL_Log("test_websocket_frame: %s: state %d, close code %u, sent %u, error '%s'", what, (int)c.state,
                (unsigned)c.close_code, count >= 1 ? (unsigned)ws_test_close_code(&frames[count - 1]) : 0u, c.error);
    }
    BR_CHECK(ok);
    BR_CHECK(!br_websocket_conn_send(&c, true, "x", 1));   /* nothing more goes out */
    br_websocket_conn_free(&c);
}

static void test_protocol_errors(void)
{
    static uint8_t bytes[512];
    /* reserved bits (no extension was agreed) */
    expect_failure((const uint8_t *)"\xc1\x00", 2, 0, 1002, "RSV1");
    expect_failure((const uint8_t *)"\xa1\x00", 2, 0, 1002, "RSV2");
    expect_failure((const uint8_t *)"\x91\x00", 2, 0, 1002, "RSV3");
    /* unknown opcodes, data and control */
    expect_failure((const uint8_t *)"\x83\x00", 2, 0, 1002, "opcode 3");
    expect_failure((const uint8_t *)"\x8b\x00", 2, 0, 1002, "opcode 11");
    /* a control frame over 125 bytes, a fragmented control frame */
    SDL_memset(bytes, 'x', sizeof bytes);
    size_t n = ws_test_frame_write(bytes, 0x89, bytes + 300, 126);
    expect_failure(bytes, n, 0, 1002, "ping of 126 bytes");
    expect_failure((const uint8_t *)"\x09\x00", 2, 0, 1002, "fragmented ping");
    expect_failure((const uint8_t *)"\x08\x00", 2, 0, 1002, "fragmented close");
    /* lengths: top bit set, not in the shortest form */
    expect_failure((const uint8_t *)"\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x00", 10, 0, 1002, "length 2^63");
    expect_failure((const uint8_t *)"\x82\x7e\x00\x05" "abcde", 9, 0, 1002, "length 5 in 16 bits");
    expect_failure((const uint8_t *)"\x82\x7f\x00\x00\x00\x00\x00\x00\x01\x00", 10, 0, 1002, "length 256 in 64 bits");
    /* over the message limit: refused at the header, before any payload is buffered */
    n = ws_test_frame_write(bytes, 0x82, "0123456789X", 11);
    expect_failure(bytes, n, 10, 1009, "11 bytes over a limit of 10");
    expect_failure((const uint8_t *)"\x82\x7f\x00\x00\x00\x01\x00\x00\x00\x00" "x", 11, 0, 1009, "a 4 GB frame");
    n = ws_test_frame_write(bytes, 0x01, "01234", 5);   /* fragments adding up over the limit */
    n += ws_test_frame_write(bytes + n, 0x80, "56789X", 6);
    expect_failure(bytes, n, 10, 1009, "fragments over the limit");
    /* a masked frame from the server */
    n = ws_test_frame_write_ex(bytes, 0x81, "Hello", 5, rfc_mask, false, false);
    expect_failure(bytes, n, 0, 1002, "masked server frame");
    /* frames out of order */
    expect_failure((const uint8_t *)"\x80\x01" "A", 3, 0, 1002, "continuation without a start");
    expect_failure((const uint8_t *)"\x01\x01" "A" "\x81\x01" "B", 6, 0, 1002, "new message inside a fragmented one");
    /* UTF-8: invalid text, also split over fragments; invalid close reason */
    expect_failure((const uint8_t *)"\x81\x02\xc3\x28", 4, 0, 1007, "invalid UTF-8 text");
    expect_failure((const uint8_t *)"\x01\x01\xc3\x80\x01\x28", 6, 0, 1007, "invalid UTF-8 over fragments");
    expect_failure((const uint8_t *)"\x81\x01\xc3", 3, 0, 1007, "text ending inside a character");
    expect_failure((const uint8_t *)"\x88\x04\x03\xe8\xc3\x28", 6, 0, 1007, "invalid UTF-8 close reason");
    /* close codes that may not be sent, a 1-byte close body */
    expect_failure((const uint8_t *)"\x88\x02\x03\xed", 4, 0, 1002, "close code 1005");
    expect_failure((const uint8_t *)"\x88\x02\x03\xee", 4, 0, 1002, "close code 1006");
    expect_failure((const uint8_t *)"\x88\x02\x03\xe7", 4, 0, 1002, "close code 999");
    expect_failure((const uint8_t *)"\x88\x02\x13\x88", 4, 0, 1002, "close code 5000");
    expect_failure((const uint8_t *)"\x88\x01\x03", 3, 0, 1002, "close body of 1 byte");
    expect_failure((const uint8_t *)"\x88\x02\x03\xf4", 4, 0, 1002, "close code 1012 (wslay: invalid)");

    /* a character split over two fragments is fine */
    br_websocket_conn c;
    open_conn(&c);
    feed(&c, "\x01\x01\xc3\x80\x01\xa9", 6);
    BR_CHECK(receive_is(&c, "\xc3\xa9", true) && c.state == BR_WEBSOCKET_OPEN);
    br_websocket_conn_free(&c);
}

static void test_closing(void)
{
    br_websocket_conn c;
    /* the server closes: we answer with its code; CLOSED with it */
    open_conn(&c);
    size_t n = ws_test_close_write(wire, 1001, "going away");
    feed(&c, wire, n);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1001 && c.error[0] == '\0');
    BR_CHECK(output_frames(&c) == 1 && frames[0].opcode == 8 && frames[0].masked && ws_test_close_code(&frames[0]) == 1001);
    BR_CHECK(!br_websocket_conn_send(&c, true, "late", 4));
    br_websocket_conn_free(&c);
    /* a close without a code: 1005 */
    open_conn(&c);
    feed(&c, "\x88\x00", 2);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1005);
    br_websocket_conn_free(&c);
    /* we close: CLOSING until the reply, which carries the code */
    open_conn(&c);
    BR_CHECK(br_websocket_conn_close(&c, 1000, "done", 0));
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSING && c.close_code == 0);
    BR_CHECK(output_frames(&c) == 1 && frames[0].opcode == 8 && frames[0].masked);
    BR_CHECK(ws_test_close_code(&frames[0]) == 1000 && SDL_strcmp((const char *)frames[0].payload + 2, "done") == 0);
    BR_CHECK(!br_websocket_conn_send(&c, true, "after", 5));
    BR_CHECK(!br_websocket_conn_close(&c, 1000, NULL, 0));
    feed(&c, "\x81\x02hi", 4);   /* data still arrives while CLOSING */
    n = ws_test_close_write(wire, 1000, NULL);
    feed(&c, wire, n);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1000 && c.error[0] == '\0');
    BR_CHECK(output_frames(&c) == 0);   /* no second close frame */
    BR_CHECK(receive_is(&c, "hi", true));
    br_websocket_conn_free(&c);
    /* messages that came before the close stay readable after CLOSED */
    open_conn(&c);
    n = ws_test_frame_write(wire, 0x81, "first", 5);
    n += ws_test_frame_write(wire + n, 0x82, "second", 6);
    n += ws_test_close_write(wire + n, 4000, "bye");
    feed(&c, wire, n);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 4000);
    BR_CHECK(receive_is(&c, "first", true));
    BR_CHECK(receive_is(&c, "second", false));
    BR_CHECK(!br_websocket_conn_receive(&c, &(br_websocket_message){ 0 }));
    br_websocket_conn_free(&c);
    /* the stream ends without a close frame: 1006; also in the middle of a frame */
    open_conn(&c);
    feed(&c, "\x81\x01" "a", 3);
    br_websocket_conn_end(&c, NULL, 0);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1006 && contains(c.error, "dropped"));
    BR_CHECK(receive_is(&c, "a", true));
    br_websocket_conn_free(&c);
    open_conn(&c);
    feed(&c, "\x81\x05He", 4);
    br_websocket_conn_end(&c, "tls: connection reset", 0);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1006 && contains(c.error, "reset"));
    BR_CHECK(!br_websocket_conn_receive(&c, &(br_websocket_message){ 0 }));
    br_websocket_conn_free(&c);
    /* we close, the stream ends before the reply: 1006 */
    open_conn(&c);
    BR_CHECK(br_websocket_conn_close(&c, 1000, NULL, 0));
    br_websocket_conn_end(&c, NULL, 0);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1006);
    br_websocket_conn_free(&c);
    /* we close, the server never answers: CLOSED 1006 at the close timeout */
    BR_CHECK(br_websocket_conn_init(&c, fixed_random, (void *)rfc_mask, 0));
    br_websocket_conn_set_times(&c, 0, 2 * SECOND, 0);
    BR_CHECK(br_websocket_conn_close(&c, 1000, NULL, SECOND));
    br_websocket_conn_update(&c, 2 * SECOND);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSING);
    br_websocket_conn_update(&c, 3 * SECOND);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1006 && contains(c.error, "no close reply"));
    br_websocket_conn_free(&c);
}

static void test_keep_alive(void)
{
    br_websocket_conn c;
    /* silence: a ping after one interval, dropped after the second */
    BR_CHECK(br_websocket_conn_init(&c, fixed_random, (void *)rfc_mask, 0));
    br_websocket_conn_set_times(&c, SECOND, 5 * SECOND, 0);
    br_websocket_conn_update(&c, SECOND / 2);
    BR_CHECK(output_frames(&c) == 0);
    br_websocket_conn_update(&c, SECOND);
    BR_CHECK(output_frames(&c) == 1 && frames[0].opcode == 9 && frames[0].masked);
    br_websocket_conn_update(&c, SECOND + SECOND / 2);
    BR_CHECK(output_frames(&c) == 0 && c.state == BR_WEBSOCKET_OPEN);
    br_websocket_conn_update(&c, 2 * SECOND);
    BR_CHECK(c.state == BR_WEBSOCKET_CLOSED && c.close_code == 1006 && contains(c.error, "keep-alive"));
    br_websocket_conn_free(&c);
    /* a pong (or anything) keeps it open; the next ping one interval after it */
    BR_CHECK(br_websocket_conn_init(&c, fixed_random, (void *)rfc_mask, 0));
    br_websocket_conn_set_times(&c, SECOND, 5 * SECOND, 0);
    br_websocket_conn_update(&c, SECOND);
    BR_CHECK(output_frames(&c) == 1 && frames[0].opcode == 9);
    br_websocket_conn_feed(&c, "\x8a\x00", 2, SECOND + SECOND / 2);
    br_websocket_conn_update(&c, 2 * SECOND);
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN && output_frames(&c) == 0);
    br_websocket_conn_update(&c, 2 * SECOND + SECOND / 2);
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN && output_frames(&c) == 1 && frames[0].opcode == 9);
    br_websocket_conn_feed(&c, "\x81\x01x", 3, 3 * SECOND);   /* data counts as an answer too */
    br_websocket_conn_update(&c, 3 * SECOND + SECOND / 2);
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN);
    br_websocket_conn_free(&c);
    /* off: nothing ever */
    BR_CHECK(br_websocket_conn_init(&c, fixed_random, (void *)rfc_mask, 0));
    br_websocket_conn_set_times(&c, 0, 5 * SECOND, 0);
    br_websocket_conn_update(&c, 1000 * SECOND);
    BR_CHECK(c.state == BR_WEBSOCKET_OPEN && output_frames(&c) == 0);
    br_websocket_conn_free(&c);
}

static void test_unread_limit(void)
{
    /* a game that never reads: messages pile up to BR_WEBSOCKET_MAX_UNREAD, then 1008 */
    static uint8_t payload[1048576];
    size_t n = ws_test_frame_write(big, 0x82, payload, sizeof payload);
    br_websocket_conn c;
    open_conn(&c);
    int fed = 0;
    while (c.state == BR_WEBSOCKET_OPEN && fed < 100) {
        feed(&c, big, n);
        fed++;
    }
    BR_CHECK(c.state == BR_WEBSOCKET_FAILED && fed > 60 && fed <= 65);
    int count = output_frames(&c);
    BR_CHECK(count >= 1 && ws_test_close_code(&frames[count - 1]) == 1008);
    BR_CHECK(contains(c.error, "unread"));
    /* what did fit is still readable */
    int read = 0;
    br_websocket_message m;
    SDL_zero(m);
    while (br_websocket_conn_receive(&c, &m)) {
        read++;
    }
    BR_CHECK(read == fed - 1);
    br_websocket_conn_free(&c);
}

int main(void)
{
    test_keys();
    test_utf8();
    test_protocols();
    test_write_handshake();
    test_handshake_checks();
    test_rfc_examples();
    test_round_trips();
    test_one_byte_at_a_time();
    test_protocol_errors();
    test_closing();
    test_keep_alive();
    test_unread_limit();
    return br_test_finish();
}
