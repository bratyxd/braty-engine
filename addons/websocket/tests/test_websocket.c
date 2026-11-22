/* websocket add-on on loopback, against a server in this test (br_net, or the TLS test server of
 * the tls add-on for wss://): the handshake by hand, frames by hand (ws_test_frames.h), never
 * masked. Open -> OPEN (with the first frame in the same packet as the 101); text and binary
 * echo; every client frame masked; 3 fragments with a ping between -> one message and a pong;
 * server close 1001; br_websocket_close -> the server sees 1000, CLOSED; messages readable after
 * CLOSED; the server drops the connection -> 1006; wrong accept key, extensions, a protocol not
 * offered, HTTP 403 -> FAILED; set_max_message -> 1009; invalid UTF-8 -> 1007; keep-alive (dead
 * server dropped as 1006, answering server stays OPEN); handshake and close timeouts; wss://;
 * send before OPEN and after close; release while OPEN; the table full; zero / stale handles.
 * Ports 47831..47833. */
#include "braty/braty.h"
#include "braty/br_websocket.h"
#include "br_test.h"
#include "br_websocket_internal.h"
#include "tls_test_server.h"
#include "ws_test_frames.h"

#include <SDL3/SDL.h>

#define PORT 47831
#define TLS_PORT 47832
#define CLOSED_PORT 47833

static tls_test_server tls_server;
static tls_test_conn tls_conn;
static ws_test_frame frame;   /* the client frame server_frame read last */
static uint8_t wire[200000];

/* The server side of the current connection. */
typedef struct server_peer {
    br_engine    *e;
    bool          secure;
    bool          used;
    bool          gone;       /* the client closed */
    br_net_socket socket;
    uint8_t       in[200000];
    size_t        got;
} server_peer;

static server_peer peer;

static uint64_t deadline_in(double seconds)
{
    return SDL_GetTicksNS() + (uint64_t)(seconds * 1e9);
}

static bool before(uint64_t deadline)
{
    return SDL_GetTicksNS() < deadline;
}

static double seconds_since(uint64_t start)
{
    return (double)(SDL_GetTicksNS() - start) / 1e9;
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

/* Reads what the client sent into peer.in. */
static void peer_pump(void)
{
    if (!peer.used || peer.gone) {
        return;
    }
    int capacity = (int)(sizeof peer.in - peer.got);
    int n = peer.secure ? tls_test_conn_receive(&tls_conn, peer.in + peer.got, capacity)
                        : br_net_receive(peer.e, peer.socket, peer.in + peer.got, capacity);
    if (n > 0) {
        peer.got += (size_t)n;
    } else if (n < 0) {
        peer.gone = true;
    }
}

static bool peer_send(const void *data, size_t size)
{
    return peer.secure ? tls_test_conn_send(&tls_conn, data, size) : br_net_send(peer.e, peer.socket, data, size);
}

static void peer_close(void)
{
    if (!peer.used) {
        return;
    }
    if (peer.secure) {
        tls_test_conn_close(&tls_conn);
    } else {
        br_net_close(peer.e, peer.socket);
    }
    peer.used = false;
}

/* Steps until a client connected and its upgrade request is in peer.in. */
static bool server_accept(br_engine *e, br_net_server listener, bool secure)
{
    peer_close();
    SDL_zero(peer);
    peer.e = e;
    peer.secure = secure;
    uint64_t deadline = deadline_in(10.0);
    while (before(deadline)) {
        step(e);
        if (!peer.used) {
            if (secure) {
                peer.used = tls_test_server_accept(&tls_server, &tls_conn);
            } else {
                peer.socket = br_net_accept(e, listener);
                peer.used = peer.socket.generation != 0;
            }
            continue;
        }
        peer_pump();
        peer.in[peer.got < sizeof peer.in ? peer.got : sizeof peer.in - 1] = '\0';
        if (SDL_strstr((const char *)peer.in, "\r\n\r\n") != NULL) {
            return true;
        }
    }
    return false;
}

/* The accept value for the request in peer.in, computed here (not with the add-on's helper). */
static void request_accept(char out[64])
{
    out[0] = '\0';
    const char *key = SDL_strstr((const char *)peer.in, "\r\nSec-WebSocket-Key: ");
    if (key == NULL) {
        return;
    }
    key += 21;
    const char *end = SDL_strstr(key, "\r\n");
    char text[128];
    SDL_snprintf(text, sizeof text, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)(end - key), key);
    uint8_t digest[20];
    br_tls_sha1(text, SDL_strlen(text), digest);
    br_tls_base64_encode(digest, sizeof digest, out, 64);
}

/* Answers the request with a 101 (+ extra header lines; a wrong accept value when asked) and
 * `after` bytes in the same send. */
static void server_answer(const char *extra, const void *after, size_t after_size, bool wrong_accept)
{
    char accept[64];
    request_accept(accept);
    if (wrong_accept) {
        accept[0] = accept[0] == 'A' ? 'B' : 'A';
    }
    static char response[4096];
    int length = SDL_snprintf(response, sizeof response, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                              "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n", accept,
                              extra != NULL ? extra : "");
    SDL_memcpy(wire, response, (size_t)length);
    if (after_size > 0) {
        SDL_memcpy(wire + length, after, after_size);
    }
    BR_CHECK(peer_send(wire, (size_t)length + after_size));
    peer.got = 0;
}

/* Steps until the state is no longer `from` (or seconds pass); returns the state. */
static br_websocket_state wait_while(br_engine *e, br_websocket ws, br_websocket_state from, double seconds)
{
    uint64_t deadline = deadline_in(seconds);
    while (br_websocket_socket_state(e, ws) == from && before(deadline)) {
        step(e);
        peer_pump();
    }
    return br_websocket_socket_state(e, ws);
}

/* Opens url, serves the handshake (extra headers, `after` bytes) and waits for OPEN. */
static br_websocket open_socket(br_engine *e, br_net_server listener, const char *url, bool secure)
{
    br_websocket ws = br_websocket_open(e, url, NULL, NULL);
    BR_CHECK(ws.generation != 0);
    BR_CHECK(server_accept(e, listener, secure));
    server_answer(NULL, NULL, 0, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 10.0) == BR_WEBSOCKET_OPEN);
    BR_CHECK(SDL_strcmp(br_websocket_protocol(e, ws), "") == 0);   /* none offered, none chosen */
    return ws;
}

/* Steps until a whole client frame arrived; reads it into `frame`. Every client frame is masked. */
static bool server_frame(br_engine *e, double seconds)
{
    uint64_t deadline = deadline_in(seconds);
    for (;;) {
        size_t n = ws_test_frame_read(peer.in, peer.got, &frame);
        if (n > 0) {
            SDL_memmove(peer.in, peer.in + n, peer.got - n);
            peer.got -= n;
            BR_CHECK(frame.masked);
            return true;
        }
        if (!before(deadline)) {
            return false;
        }
        step(e);
        peer_pump();
    }
}

static void server_send_frame(uint8_t first, const void *payload, size_t size)
{
    size_t n = ws_test_frame_write(wire, first, payload, size);
    BR_CHECK(peer_send(wire, n));
}

static bool receive_text(br_engine *e, br_websocket ws, const char *text, double seconds)
{
    br_websocket_message m;
    SDL_zero(m);
    uint64_t deadline = deadline_in(seconds);
    while (!br_websocket_receive(e, ws, &m)) {
        if (!before(deadline)) {
            return false;
        }
        step(e);
        peer_pump();
    }
    return m.text && m.size == SDL_strlen(text) && SDL_strcmp(m.data, text) == 0;
}

/* ---- cases */

static void test_not_registered(void)
{
    br_engine *e = br_engine_create("braty-test-websocket-unregistered", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1/", NULL, NULL).generation == 0);
    br_engine_destroy(e);
}

static void test_zero_handles(br_engine *e)
{
    br_websocket zero = { 0, 0 };
    br_websocket wild = { 999, 3 };
    br_websocket_message m = { "x", 1, true };
    BR_CHECK(br_websocket_socket_state(e, zero) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_socket_state(e, wild) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(!br_websocket_send_text(e, zero, "a"));
    BR_CHECK(!br_websocket_send_binary(e, wild, "a", 1));
    BR_CHECK(!br_websocket_receive(e, zero, &m) && m.data == NULL && m.size == 0);
    BR_CHECK(!br_websocket_receive(e, zero, NULL));
    br_websocket_set_max_message(e, zero, 10);
    br_websocket_set_ping_interval(e, wild, 1.0f);
    br_websocket_close(e, zero, 1000, NULL);
    BR_CHECK(br_websocket_close_code(e, zero) == 0);
    BR_CHECK(SDL_strcmp(br_websocket_protocol(e, wild), "") == 0);
    BR_CHECK(SDL_strcmp(br_websocket_error(e, wild), "") == 0);
    br_websocket_release(e, zero);
    br_websocket_release(e, wild);
}

static void test_bad_arguments(br_engine *e)
{
    BR_CHECK(br_websocket_open(e, NULL, NULL, NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "http://127.0.0.1:47831/", NULL, NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "127.0.0.1:47831", NULL, NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/a b", NULL, NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/", "a b", NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/", "", NULL).generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, "Upgrade: h2c\r\n").generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, "Sec-WebSocket-Extensions: x\r\n").generation == 0);
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, "X: 1\r\n\r\nGET /x HTTP/1.1").generation == 0);
}

static void test_echo(br_engine *e, br_net_server listener)
{
    br_websocket ws = br_websocket_open(e, "ws://127.0.0.1:47831/echo?room=1", "chat, v2", "Origin: braty-test\r\n");
    BR_CHECK(ws.generation != 0);
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_CONNECTING);
    BR_CHECK(br_websocket_close_code(e, ws) == 0);
    BR_CHECK(server_accept(e, listener, false));
    const char *request = (const char *)peer.in;
    BR_CHECK(SDL_strncmp(request, "GET /echo?room=1 HTTP/1.1\r\n", 27) == 0);
    BR_CHECK(contains(request, "\r\nHost: 127.0.0.1:47831\r\n"));
    BR_CHECK(contains(request, "\r\nUpgrade: websocket\r\n") && contains(request, "\r\nConnection: Upgrade\r\n"));
    BR_CHECK(contains(request, "\r\nSec-WebSocket-Version: 13\r\n"));
    BR_CHECK(contains(request, "\r\nSec-WebSocket-Protocol: chat, v2\r\n"));
    BR_CHECK(contains(request, "\r\nOrigin: braty-test\r\n"));
    BR_CHECK(!br_websocket_send_text(e, ws, "too early"));   /* not OPEN yet */
    BR_CHECK(SDL_strcmp(br_websocket_protocol(e, ws), "") == 0);
    /* the 101 and the first message in one send: the bytes after the headers reach wslay */
    uint8_t first[64];
    size_t first_size = ws_test_frame_write(first, 0x81, "welcome", 7);
    server_answer("Sec-WebSocket-Protocol: chat\r\n", first, first_size, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 10.0) == BR_WEBSOCKET_OPEN);
    BR_CHECK(receive_text(e, ws, "welcome", 5.0));
    BR_CHECK(SDL_strcmp(br_websocket_protocol(e, ws), "chat") == 0);   /* the server's choice */

    /* text echo */
    BR_CHECK(br_websocket_send_text(e, ws, "hello, server"));
    BR_CHECK(server_frame(e, 5.0) && frame.fin && frame.opcode == 1 && frame.rsv == 0);
    BR_CHECK(SDL_strcmp((const char *)frame.payload, "hello, server") == 0);
    server_send_frame(0x81, frame.payload, (size_t)frame.length);
    BR_CHECK(receive_text(e, ws, "hello, server", 5.0));
    BR_CHECK(!br_websocket_send_text(e, ws, "\xc3\x28"));   /* not UTF-8 */

    /* binary echo, with a zero byte, and 64 KiB */
    static uint8_t data[65536];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 31);
    }
    BR_CHECK(br_websocket_send_binary(e, ws, data, sizeof data));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 2 && frame.length == sizeof data);
    BR_CHECK(SDL_memcmp(frame.payload, data, sizeof data) == 0);
    server_send_frame(0x82, frame.payload, (size_t)frame.length);
    br_websocket_message m;
    SDL_zero(m);
    uint64_t deadline = deadline_in(5.0);
    while (!br_websocket_receive(e, ws, &m) && before(deadline)) {
        step(e);
    }
    BR_CHECK(!m.text && m.size == sizeof data && m.data != NULL && SDL_memcmp(m.data, data, sizeof data) == 0);
    BR_CHECK(br_websocket_send_binary(e, ws, NULL, 0));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 2 && frame.length == 0);

    /* three fragments with a ping in the middle: one message, and a pong with the ping's data */
    size_t n = 0;
    n += ws_test_frame_write(wire + n, 0x01, "one ", 4);
    n += ws_test_frame_write(wire + n, 0x89, "are you there", 13);
    n += ws_test_frame_write(wire + n, 0x00, "two ", 4);
    n += ws_test_frame_write(wire + n, 0x80, "three", 5);
    BR_CHECK(peer_send(wire, n));
    BR_CHECK(receive_text(e, ws, "one two three", 5.0));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 0xA && SDL_strcmp((const char *)frame.payload, "are you there") == 0);

    /* the server closes with 1001: we answer 1001, CLOSED with it */
    n = ws_test_close_write(wire, 1001, "restart");
    BR_CHECK(peer_send(wire, n));
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_OPEN, 5.0) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1001);
    BR_CHECK(SDL_strcmp(br_websocket_error(e, ws), "") == 0);
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 8 && ws_test_close_code(&frame) == 1001);
    BR_CHECK(!br_websocket_send_text(e, ws, "after the close"));
    BR_CHECK(!br_websocket_receive(e, ws, &m));
    br_websocket_release(e, ws);
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_CLOSED && br_websocket_close_code(e, ws) == 0);
    peer_close();
}

static void test_client_close(br_engine *e, br_net_server listener)
{
    br_websocket ws = open_socket(e, listener, "ws://127.0.0.1:47831/", false);
    br_websocket_close(e, ws, 1000, "bye");
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_CLOSING);
    BR_CHECK(!br_websocket_send_text(e, ws, "x"));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 8 && ws_test_close_code(&frame) == 1000);
    BR_CHECK(SDL_strcmp((const char *)frame.payload + 2, "bye") == 0);
    size_t n = ws_test_close_write(wire, 1000, NULL);
    BR_CHECK(peer_send(wire, n));
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CLOSING, 5.0) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1000 && SDL_strcmp(br_websocket_error(e, ws), "") == 0);
    /* the client closes the TCP connection after the handshake */
    uint64_t deadline = deadline_in(5.0);
    while (!peer.gone && before(deadline)) {
        step(e);
        peer_pump();
    }
    BR_CHECK(peer.gone);
    br_websocket_release(e, ws);
    peer_close();
}

static void test_after_closed(br_engine *e, br_net_server listener)
{
    br_websocket ws = open_socket(e, listener, "ws://127.0.0.1:47831/", false);
    size_t n = ws_test_frame_write(wire, 0x81, "one", 3);
    n += ws_test_frame_write(wire + n, 0x81, "two", 3);
    n += ws_test_close_write(wire + n, 1000, "done");
    BR_CHECK(peer_send(wire, n));
    peer_close();
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_OPEN, 5.0) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1000);
    BR_CHECK(receive_text(e, ws, "one", 0.0));
    BR_CHECK(receive_text(e, ws, "two", 0.0));
    br_websocket_message m = { NULL, 0, false };
    BR_CHECK(!br_websocket_receive(e, ws, &m));
    br_websocket_release(e, ws);
}

static void test_dropped(br_engine *e, br_net_server listener)
{
    br_websocket ws = open_socket(e, listener, "ws://127.0.0.1:47831/", false);
    server_send_frame(0x81, "last words", 10);
    peer_close();   /* no close frame */
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_OPEN, 5.0) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1006);
    br_log_info("test_websocket: dropped: %s", br_websocket_error(e, ws));
    BR_CHECK(br_websocket_error(e, ws)[0] != '\0');
    BR_CHECK(receive_text(e, ws, "last words", 0.0));
    br_websocket_release(e, ws);
}

/* A handshake answered with `extra` headers (or a wrong accept) or with a whole response: FAILED
 * with an error containing `expect`. */
static void expect_handshake_failure(br_engine *e, br_net_server listener, const char *protocols, const char *extra,
                                     bool wrong_accept, const char *response, const char *expect)
{
    br_websocket ws = br_websocket_open(e, "ws://127.0.0.1:47831/", protocols, NULL);
    BR_CHECK(server_accept(e, listener, false));
    if (response != NULL) {
        BR_CHECK(peer_send(response, SDL_strlen(response)));
        peer_close();   /* that is all it says */
    } else {
        server_answer(extra, NULL, 0, wrong_accept);
    }
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_FAILED);
    br_log_info("test_websocket: handshake: %s", br_websocket_error(e, ws));
    BR_CHECK(contains(br_websocket_error(e, ws), expect));
    BR_CHECK(br_websocket_close_code(e, ws) == 1006);
    BR_CHECK(!br_websocket_send_text(e, ws, "x"));
    br_websocket_release(e, ws);
    peer_close();
}

static void test_handshake_failures(br_engine *e, br_net_server listener)
{
    expect_handshake_failure(e, listener, NULL, NULL, true, NULL, "Accept");
    expect_handshake_failure(e, listener, NULL, "Sec-WebSocket-Extensions: permessage-deflate\r\n", false, NULL,
                             "extensions");
    expect_handshake_failure(e, listener, "chat", "Sec-WebSocket-Protocol: superchat\r\n", false, NULL, "superchat");
    expect_handshake_failure(e, listener, NULL, NULL, false,
                             "HTTP/1.1 403 Forbidden\r\nContent-Length: 1000\r\n\r\nno", "403");
    expect_handshake_failure(e, listener, NULL, NULL, false,
                             "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n", "Connection");
    expect_handshake_failure(e, listener, NULL, NULL, false, "HTTP/1.1 101 Switching", "closed");
    expect_handshake_failure(e, listener, NULL, NULL, false, "SSH-2.0-OpenSSH_9.6\r\n\r\n", "bad handshake");
}

/* The server sends `bytes`: the client fails, the server reads a close frame with code. */
static void expect_protocol_close(br_engine *e, br_net_server listener, size_t max_message, const uint8_t *bytes,
                                  size_t size, uint16_t code)
{
    br_websocket ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    if (max_message > 0) {
        br_websocket_set_max_message(e, ws, max_message);
    }
    BR_CHECK(server_accept(e, listener, false));
    server_answer(NULL, NULL, 0, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_OPEN);
    BR_CHECK(peer_send(bytes, size));
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_OPEN, 5.0) == BR_WEBSOCKET_FAILED);
    br_log_info("test_websocket: violation: %s", br_websocket_error(e, ws));
    char code_text[16];
    SDL_snprintf(code_text, sizeof code_text, "%u", (unsigned)code);
    BR_CHECK(contains(br_websocket_error(e, ws), code_text));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 8 && ws_test_close_code(&frame) == code);
    br_websocket_release(e, ws);
    peer_close();
}

static void test_violations(br_engine *e, br_net_server listener)
{
    static const char eleven[] = "0123456789X";
    size_t n = ws_test_frame_write(wire + 100000, 0x81, eleven, 11);
    expect_protocol_close(e, listener, 10, wire + 100000, n, 1009);
    expect_protocol_close(e, listener, 0, (const uint8_t *)"\x81\x02\xc3\x28", 4, 1007);
    expect_protocol_close(e, listener, 0, (const uint8_t *)"\xc1\x01x", 3, 1002);   /* RSV1 */
    expect_protocol_close(e, listener, 0, (const uint8_t *)"\x09\x00", 2, 1002);    /* fragmented ping */
}

static void test_keep_alive(br_engine *e, br_net_server listener)
{
    /* a server that stops answering: a ping after 0.2 s, dropped as 1006 after 0.4 s */
    br_websocket ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    br_websocket_set_ping_interval(e, ws, 0.2f);
    BR_CHECK(server_accept(e, listener, false));
    server_answer(NULL, NULL, 0, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_OPEN);
    uint64_t start = SDL_GetTicksNS();
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_OPEN, 5.0) == BR_WEBSOCKET_CLOSED);
    double seconds = seconds_since(start);
    br_log_info("test_websocket: dead server dropped after %.3f s: %s", seconds, br_websocket_error(e, ws));
    BR_CHECK(seconds >= 0.35 && seconds < 1.0);
    BR_CHECK(br_websocket_close_code(e, ws) == 1006 && contains(br_websocket_error(e, ws), "keep-alive"));
    BR_CHECK(server_frame(e, 1.0) && frame.opcode == 9);   /* the ping it sent */
    br_websocket_release(e, ws);
    peer_close();

    /* a server that answers every ping stays OPEN */
    ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    br_websocket_set_ping_interval(e, ws, 0.2f);
    BR_CHECK(server_accept(e, listener, false));
    server_answer(NULL, NULL, 0, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_OPEN);
    int pings = 0;
    uint64_t deadline = deadline_in(1.2);
    while (before(deadline) && br_websocket_socket_state(e, ws) == BR_WEBSOCKET_OPEN) {
        if (server_frame(e, 0.01) && frame.opcode == 9) {
            pings++;
            server_send_frame(0x8A, frame.payload, (size_t)frame.length);
        }
    }
    br_log_info("test_websocket: %d pings answered in 1.2 s", pings);
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_OPEN);
    BR_CHECK(pings >= 3);
    /* 0 switches it off */
    br_websocket_set_ping_interval(e, ws, 0.0f);
    deadline = deadline_in(0.6);
    while (before(deadline)) {
        step(e);
        peer_pump();
    }
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_OPEN);
    br_websocket_release(e, ws);
    /* release of an OPEN socket: a close frame 1001 */
    BR_CHECK(server_frame(e, 5.0));
    while (frame.opcode == 9) {   /* pings sent before it was switched off */
        BR_CHECK(server_frame(e, 5.0));
    }
    BR_CHECK(frame.opcode == 8 && ws_test_close_code(&frame) == 1001);
    peer_close();
}

static void test_timeouts(br_engine *e, br_net_server listener)
{
    /* the server accepts and never answers the upgrade */
    br_websocket ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    br_websocket_set_timeouts(e, ws, 0.2f, 0.0f);
    uint64_t start = SDL_GetTicksNS();
    BR_CHECK(server_accept(e, listener, false));
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_FAILED);
    double seconds = seconds_since(start);
    br_log_info("test_websocket: handshake timeout after %.3f s: %s", seconds, br_websocket_error(e, ws));
    BR_CHECK(seconds >= 0.19 && seconds < 1.0);
    BR_CHECK(contains(br_websocket_error(e, ws), "timed out") && br_websocket_close_code(e, ws) == 1006);
    br_websocket_release(e, ws);
    peer_close();

    /* we close, the server never answers our close frame */
    ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    br_websocket_set_timeouts(e, ws, 0.0f, 0.2f);
    BR_CHECK(server_accept(e, listener, false));
    server_answer(NULL, NULL, 0, false);
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CONNECTING, 5.0) == BR_WEBSOCKET_OPEN);
    start = SDL_GetTicksNS();
    br_websocket_close(e, ws, 4001, "level over");
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CLOSING, 5.0) == BR_WEBSOCKET_CLOSED);
    seconds = seconds_since(start);
    br_log_info("test_websocket: close timeout after %.3f s: %s", seconds, br_websocket_error(e, ws));
    BR_CHECK(seconds >= 0.19 && seconds < 1.0);
    BR_CHECK(br_websocket_close_code(e, ws) == 1006 && contains(br_websocket_error(e, ws), "no close reply"));
    BR_CHECK(server_frame(e, 1.0) && frame.opcode == 8 && ws_test_close_code(&frame) == 4001);
    br_websocket_release(e, ws);
    peer_close();

    /* closed while CONNECTING: CLOSED at once */
    ws = br_websocket_open(e, "ws://127.0.0.1:47831/", NULL, NULL);
    br_websocket_close(e, ws, 1000, NULL);
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1006 && SDL_strcmp(br_websocket_error(e, ws), "") == 0);
    br_websocket_release(e, ws);
}

/* A server that reads nothing: sends stop at BR_WEBSOCKET_MAX_UNSENT, and a ping flood (each
 * answered by a pong) drops the connection instead of growing the send queue without end. */
static void test_not_reading(br_engine *e, br_net_server listener)
{
    static uint8_t chunk[1024 * 1024];
    br_websocket ws = open_socket(e, listener, "ws://127.0.0.1:47831/", false);
    int sent = 0;
    bool refused = false;
    uint64_t deadline = deadline_in(20.0);
    while (!refused && sent < 200 && before(deadline)) {
        if (br_websocket_send_binary(e, ws, chunk, sizeof chunk)) {
            sent++;
        } else {
            refused = true;
        }
        step(e);   /* no peer_pump: the server reads nothing */
    }
    br_log_info("test_websocket: %d MB accepted before the send queue was full", sent);
    BR_CHECK(refused && sent >= 16);
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_OPEN);   /* a refused send is not fatal */
    static uint8_t ping[125];
    size_t n = 0;
    while (n + 127 <= sizeof wire) {
        n += ws_test_frame_write(wire + n, 0x89, ping, sizeof ping);
    }
    deadline = deadline_in(20.0);
    while (br_websocket_socket_state(e, ws) == BR_WEBSOCKET_OPEN && before(deadline)) {
        BR_CHECK(peer_send(wire, n));
        step(e);
    }
    br_log_info("test_websocket: ping flood: %s", br_websocket_error(e, ws));
    BR_CHECK(br_websocket_socket_state(e, ws) == BR_WEBSOCKET_CLOSED && br_websocket_close_code(e, ws) == 1006);
    BR_CHECK(contains(br_websocket_error(e, ws), "not reading"));
    br_websocket_release(e, ws);
    peer_close();
}

static void test_wss(br_engine *e)
{
    BR_CHECK(tls_test_server_start(&tls_server, e, TLS_PORT, "server.pem"));
    BR_CHECK(tls_test_trust_ca(e));
    br_net_server none = { 0, 0 };
    br_websocket ws = open_socket(e, none, "wss://localhost:47832/secure", true);
    BR_CHECK(br_websocket_send_text(e, ws, "secret"));
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 1 && SDL_strcmp((const char *)frame.payload, "secret") == 0);
    server_send_frame(0x81, "secret back", 11);
    BR_CHECK(receive_text(e, ws, "secret back", 5.0));
    br_websocket_close(e, ws, 1000, NULL);
    BR_CHECK(server_frame(e, 5.0) && frame.opcode == 8 && ws_test_close_code(&frame) == 1000);
    size_t n = ws_test_close_write(wire, 1000, NULL);
    BR_CHECK(peer_send(wire, n));
    BR_CHECK(wait_while(e, ws, BR_WEBSOCKET_CLOSING, 5.0) == BR_WEBSOCKET_CLOSED);
    BR_CHECK(br_websocket_close_code(e, ws) == 1000);
    br_websocket_release(e, ws);
    peer_close();
    tls_test_server_stop(&tls_server);
}

static void test_table_full(br_engine *e)
{
    static br_websocket sockets[BR_WEBSOCKET_MAX];
    for (int i = 0; i < BR_WEBSOCKET_MAX; i++) {
        sockets[i] = br_websocket_open(e, "ws://127.0.0.1:47833/", NULL, NULL);   /* nobody listens */
        BR_CHECK(sockets[i].generation != 0);
    }
    BR_CHECK(br_websocket_open(e, "ws://127.0.0.1:47833/", NULL, NULL).generation == 0);   /* logged */
    for (int i = 0; i < BR_WEBSOCKET_MAX; i++) {
        br_websocket_release(e, sockets[i]);
    }
    br_websocket after = br_websocket_open(e, "ws://127.0.0.1:47833/", NULL, NULL);
    BR_CHECK(after.generation != 0);
    BR_CHECK(wait_while(e, after, BR_WEBSOCKET_CONNECTING, 20.0) == BR_WEBSOCKET_FAILED);   /* refused */
    br_log_info("test_websocket: refused: %s", br_websocket_error(e, after));
    BR_CHECK(br_websocket_error(e, after)[0] != '\0');
    br_websocket_release(e, after);
    BR_CHECK(br_websocket_socket_state(e, sockets[0]) == BR_WEBSOCKET_CLOSED);   /* stale */
}

int main(void)
{
    test_not_registered();

    br_engine *e = br_engine_create("braty-test-websocket", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_websocket_register(e));
    BR_CHECK(br_websocket_register(e));   /* idempotent */
    BR_CHECK(br_addon_get(e, "net") != NULL && br_addon_get(e, "tls") != NULL && br_addon_get(e, "http") != NULL);
    test_zero_handles(e);
    test_bad_arguments(e);

    br_net_server listener = br_net_listen(e, PORT);
    BR_CHECK(listener.generation != 0);
    test_echo(e, listener);
    test_client_close(e, listener);
    test_after_closed(e, listener);
    test_dropped(e, listener);
    test_handshake_failures(e, listener);
    test_violations(e, listener);
    test_keep_alive(e, listener);
    test_timeouts(e, listener);
    test_not_reading(e, listener);
    br_net_server_close(e, listener);
    test_wss(e);
    test_table_full(e);

    br_engine_destroy(e);
    return br_test_finish();
}
