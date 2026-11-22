/* tls add-on on loopback, against a TLS server in this process (tls_test_server.h): plain echo,
 * secure echo with the test CA, 1 MB each way, close both ways, an untrusted CA, a host-name
 * mismatch, verification off, a server that closes mid-handshake, a plain-text server, the
 * built-in bundle, zero / stale handles, a full table, SHA-1 and base64. Ports 47811..47816. */
#include "braty/braty.h"
#include "braty/br_tls.h"
#include "br_test.h"
#include "br_tls_internal.h"
#include "tls_test_server.h"

#include <SDL3/SDL.h>

#define PLAIN_PORT 47811
#define TRUSTED_PORT 47812
#define UNTRUSTED_PORT 47813
#define HANGUP_PORT 47814
#define TEXT_PORT 47815
#define CLOSED_PORT 47816
#define BIG_SIZE (1024 * 1024)

static uint8_t big_out[BIG_SIZE];
static uint8_t big_in[BIG_SIZE];
static tls_test_server trusted;
static tls_test_server untrusted;
static tls_test_conn conn;

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

/* Steps until s leaves CONNECTING; meanwhile server (may be NULL) accepts into conn and pumps
 * its side of the handshake. */
static br_net_state handshake(br_engine *e, br_tls_stream s, tls_test_server *server)
{
    uint64_t deadline = deadline_in(20.0);
    while (br_tls_state(e, s) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
        if (server != NULL && !conn.used) {
            tls_test_server_accept(server, &conn);
        }
        tls_test_conn_pump(&conn);
    }
    return br_tls_state(e, s);
}

/* Reads exactly size bytes on the client; false on timeout or close. */
static bool client_receive(br_engine *e, br_tls_stream s, uint8_t *out, int size)
{
    int got = 0;
    uint64_t deadline = deadline_in(20.0);
    while (got < size && before(deadline)) {
        int capacity = size - got < 16384 ? size - got : 16384;
        int n = br_tls_receive(e, s, out + got, capacity);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            step(e);
        }
        got += n;
    }
    return got == size;
}

/* Reads exactly size bytes on the server side of conn; false on timeout or close. */
static bool server_receive(br_engine *e, uint8_t *out, int size)
{
    int got = 0;
    uint64_t deadline = deadline_in(20.0);
    while (got < size && before(deadline)) {
        int capacity = size - got < 16384 ? size - got : 16384;
        int n = tls_test_conn_receive(&conn, out + got, capacity);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            step(e);
        }
        got += n;
    }
    return got == size;
}

/* Steps until the client reads -1; false on timeout. */
static bool client_until_end(br_engine *e, br_tls_stream s)
{
    uint8_t scratch[256];
    uint64_t deadline = deadline_in(10.0);
    while (before(deadline)) {
        int n = br_tls_receive(e, s, scratch, (int)sizeof scratch);
        if (n < 0) {
            return true;
        }
        if (n == 0) {
            step(e);
        }
    }
    return false;
}

static bool contains(const char *text, const char *part)
{
    return text != NULL && SDL_strstr(text, part) != NULL;
}

static void test_not_registered(void)
{
    br_engine *e = br_engine_create("braty-test-tls-unregistered", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_tls_open(e, "localhost", TRUSTED_PORT, true).generation == 0);
    uint8_t bytes[4];
    BR_CHECK(!br_tls_random(e, bytes, sizeof bytes));
    BR_CHECK(!br_tls_set_ca_pem(e, "x", 1));
    br_tls_set_verify(e, false); /* logged no-op */
    br_engine_destroy(e);
}

static void test_helpers(br_engine *e)
{
    uint8_t digest[20];
    static const uint8_t abc[20] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                     0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
    br_tls_sha1("abc", 3, digest);
    BR_CHECK(SDL_memcmp(digest, abc, 20) == 0);

    char text[16];
    BR_CHECK(br_tls_base64_encode("foobar", 6, text, sizeof text) == 8);
    BR_CHECK(SDL_strcmp(text, "Zm9vYmFy") == 0);
    BR_CHECK(br_tls_base64_encode("foobar", 6, text, 8) == 0); /* no room for the NUL */
    BR_CHECK(text[0] == '\0');
    BR_CHECK(br_tls_base64_encode("f", 1, text, sizeof text) == 4);
    BR_CHECK(SDL_strcmp(text, "Zg==") == 0);

    uint8_t a[32], b[32];
    BR_CHECK(br_tls_random(e, a, sizeof a));
    BR_CHECK(br_tls_random(e, b, sizeof b));
    BR_CHECK(SDL_memcmp(a, b, sizeof a) != 0);
    BR_CHECK(br_tls_random(e, big_out, 5000)); /* more than one DRBG request */
}

static void test_builtin_bundle(br_engine *e)
{
    BR_CHECK(br_tls_cacert_size > 100000);
    uint64_t start = SDL_GetTicksNS();
    int count = br_tls_builtin_root_count(e);
    uint64_t deadline = deadline_in(30.0);
    while (count < 0 && before(deadline)) {
        step(e);
        count = br_tls_builtin_root_count(e);
    }
    br_log_info("test_tls: %d built-in roots, parsed in %.1f ms", count, (double)(SDL_GetTicksNS() - start) / 1e6);
    BR_CHECK(count >= 100);
}

static void test_bad_ca(br_engine *e)
{
    static const char garbage[] = "-----BEGIN CERTIFICATE-----\nnot base64 at all\n-----END CERTIFICATE-----\n";
    BR_CHECK(!br_tls_set_ca_pem(e, garbage, sizeof garbage - 1));
    BR_CHECK(!br_tls_set_ca_pem(e, NULL, 10));
    BR_CHECK(!br_tls_set_ca_pem(e, garbage, 0));
}

static void test_plain(br_engine *e)
{
    br_net_server server = br_net_listen(e, PLAIN_PORT);
    BR_CHECK(server.generation != 0);
    br_tls_stream s = br_tls_open(e, "127.0.0.1", PLAIN_PORT, false);
    BR_CHECK(s.generation != 0);
    br_net_socket peer = { 0, 0 };
    uint64_t deadline = deadline_in(10.0);
    while ((peer.generation == 0 || br_tls_state(e, s) != BR_NET_OPEN) && before(deadline)) {
        step(e);
        if (peer.generation == 0) {
            peer = br_net_accept(e, server);
        }
    }
    BR_CHECK(br_tls_state(e, s) == BR_NET_OPEN);
    BR_CHECK(br_tls_send(e, s, "plain", 5));
    char buf[16];
    int got = 0;
    deadline = deadline_in(10.0);
    while (got < 5 && before(deadline)) {
        int n = br_net_receive(e, peer, buf + got, 5 - got);
        got += n > 0 ? n : 0;
        step(e);
    }
    BR_CHECK(got == 5 && SDL_memcmp(buf, "plain", 5) == 0);
    BR_CHECK(br_net_send(e, peer, "PLAIN", 5));
    BR_CHECK(client_receive(e, s, (uint8_t *)buf, 5));
    BR_CHECK(SDL_memcmp(buf, "PLAIN", 5) == 0);
    BR_CHECK(SDL_strcmp(br_tls_error(e, s), "") == 0);

    br_net_close(e, peer);
    BR_CHECK(client_until_end(e, s));
    BR_CHECK(br_tls_state(e, s) == BR_NET_CLOSED);
    br_tls_close(e, s);
    br_net_server_close(e, server);

    br_tls_stream refused = br_tls_open(e, "127.0.0.1", CLOSED_PORT, false);
    deadline = deadline_in(20.0);
    while (br_tls_state(e, refused) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
    }
    BR_CHECK(br_tls_state(e, refused) == BR_NET_FAILED);
    BR_CHECK(br_tls_error(e, refused)[0] != '\0');
    br_tls_close(e, refused);
}

static void test_secure(br_engine *e)
{
    br_tls_stream s = br_tls_open(e, "localhost", TRUSTED_PORT, true);
    BR_CHECK(s.generation != 0);
    BR_CHECK(br_tls_state(e, s) == BR_NET_CONNECTING);
    BR_CHECK(!br_tls_send(e, s, "early", 5));
    uint8_t buf[64];
    BR_CHECK(br_tls_receive(e, s, buf, (int)sizeof buf) == 0);
    BR_CHECK(handshake(e, s, &trusted) == BR_NET_OPEN);
    BR_CHECK(SDL_strcmp(br_tls_error(e, s), "") == 0);

    BR_CHECK(br_tls_send(e, s, "hello tls", 9));
    BR_CHECK(server_receive(e, buf, 9));
    BR_CHECK(SDL_memcmp(buf, "hello tls", 9) == 0);
    BR_CHECK(conn.state == 1);
    BR_CHECK(tls_test_conn_send(&conn, "HELLO TLS", 9));
    BR_CHECK(client_receive(e, s, buf, 9));
    BR_CHECK(SDL_memcmp(buf, "HELLO TLS", 9) == 0);
    BR_CHECK(br_tls_send(e, s, "", 0));

    /* 1 MB each way */
    for (int i = 0; i < BIG_SIZE; i++) {
        big_out[i] = (uint8_t)(i * 13 + (i >> 9));
    }
    BR_CHECK(br_tls_send(e, s, big_out, BIG_SIZE));
    SDL_memset(big_in, 0, BIG_SIZE);
    BR_CHECK(server_receive(e, big_in, BIG_SIZE));
    BR_CHECK(SDL_memcmp(big_in, big_out, BIG_SIZE) == 0);
    BR_CHECK(tls_test_conn_send(&conn, big_out, BIG_SIZE));
    SDL_memset(big_in, 0, BIG_SIZE);
    BR_CHECK(client_receive(e, s, big_in, BIG_SIZE));
    BR_CHECK(SDL_memcmp(big_in, big_out, BIG_SIZE) == 0);

    /* the server closes (close_notify): the client reads -1, CLOSED */
    tls_test_conn_close(&conn);
    BR_CHECK(client_until_end(e, s));
    BR_CHECK(br_tls_state(e, s) == BR_NET_CLOSED);
    BR_CHECK(!br_tls_send(e, s, "x", 1));
    br_tls_close(e, s);

    /* the client closes: the server sees the end */
    br_tls_stream c = br_tls_open(e, "localhost", TRUSTED_PORT, true);
    BR_CHECK(handshake(e, c, &trusted) == BR_NET_OPEN);
    BR_CHECK(br_tls_send(e, c, "bye", 3));
    BR_CHECK(server_receive(e, buf, 3));
    br_tls_close(e, c);
    BR_CHECK(br_tls_state(e, c) == BR_NET_CLOSED);
    uint64_t deadline = deadline_in(10.0);
    int n = 0;
    while (n >= 0 && before(deadline)) {
        n = tls_test_conn_receive(&conn, buf, (int)sizeof buf);
        step(e);
    }
    BR_CHECK(n < 0);
    tls_test_conn_close(&conn);

    /* stale handles: the closed streams, after their slots were reused */
    br_tls_stream again = br_tls_open(e, "localhost", TRUSTED_PORT, true);
    BR_CHECK(handshake(e, again, &trusted) == BR_NET_OPEN);
    BR_CHECK(br_tls_state(e, s) == BR_NET_CLOSED);
    BR_CHECK(br_tls_state(e, c) == BR_NET_CLOSED);
    BR_CHECK(!br_tls_send(e, c, "x", 1));
    BR_CHECK(br_tls_receive(e, c, buf, (int)sizeof buf) == -1);
    BR_CHECK(SDL_strcmp(br_tls_error(e, c), "") == 0);
    br_tls_close(e, c); /* no-op */
    BR_CHECK(br_tls_state(e, again) == BR_NET_OPEN);
    br_tls_close(e, again);
    tls_test_conn_close(&conn);
}

/* Opens host:port securely against server and expects FAILED with `part` in the error. */
static void expect_failed(br_engine *e, const char *host, uint16_t port, tls_test_server *server, const char *part)
{
    br_tls_stream s = br_tls_open(e, host, port, true);
    BR_CHECK(s.generation != 0);
    BR_CHECK(handshake(e, s, server) == BR_NET_FAILED);
    const char *error = br_tls_error(e, s);
    br_log_info("test_tls: %s:%u failed as expected: %s", host, (unsigned)port, error);
    BR_CHECK(contains(error, part));
    BR_CHECK(!br_tls_send(e, s, "x", 1));
    uint8_t buf[8];
    BR_CHECK(br_tls_receive(e, s, buf, (int)sizeof buf) == -1);
    br_tls_close(e, s);
    tls_test_conn_close(&conn);
}

static void test_verification(br_engine *e)
{
    /* a certificate from a CA the client does not trust */
    expect_failed(e, "localhost", UNTRUSTED_PORT, &untrusted, "certificate");
    /* the right CA, the wrong name: the certificate is for localhost only */
    expect_failed(e, "127.0.0.1", TRUSTED_PORT, &trusted, "certificate");

    /* verification off: the untrusted one connects and carries data */
    br_tls_set_verify(e, false);
    br_tls_stream s = br_tls_open(e, "localhost", UNTRUSTED_PORT, true);
    br_tls_set_verify(e, true); /* the stream keeps the setting it was opened with */
    BR_CHECK(handshake(e, s, &untrusted) == BR_NET_OPEN);
    BR_CHECK(br_tls_send(e, s, "insecure", 8));
    uint8_t buf[16];
    BR_CHECK(server_receive(e, buf, 8));
    BR_CHECK(SDL_memcmp(buf, "insecure", 8) == 0);
    br_tls_close(e, s);
    tls_test_conn_close(&conn);

    /* verification is back on for new streams */
    expect_failed(e, "localhost", UNTRUSTED_PORT, &untrusted, "certificate");
}

static void test_broken_servers(br_engine *e)
{
    /* a server that reads the ClientHello and hangs up */
    br_net_server hangup = br_net_listen(e, HANGUP_PORT);
    BR_CHECK(hangup.generation != 0);
    br_tls_stream s = br_tls_open(e, "localhost", HANGUP_PORT, true);
    br_net_socket peer = { 0, 0 };
    bool hung_up = false;
    uint64_t deadline = deadline_in(20.0);
    while (br_tls_state(e, s) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
        if (peer.generation == 0) {
            peer = br_net_accept(e, hangup);
        }
        uint8_t hello[512];
        if (!hung_up && br_net_receive(e, peer, hello, (int)sizeof hello) > 0) {
            br_net_close(e, peer);
            hung_up = true;
        }
    }
    BR_CHECK(hung_up);
    BR_CHECK(br_tls_state(e, s) == BR_NET_FAILED);
    br_log_info("test_tls: hang-up: %s", br_tls_error(e, s));
    BR_CHECK(br_tls_error(e, s)[0] != '\0');
    br_tls_close(e, s);
    br_net_server_close(e, hangup);

    /* a plain-text server answering a secure open */
    br_net_server text = br_net_listen(e, TEXT_PORT);
    BR_CHECK(text.generation != 0);
    s = br_tls_open(e, "localhost", TEXT_PORT, true);
    peer.index = 0;
    peer.generation = 0;
    deadline = deadline_in(20.0);
    while (br_tls_state(e, s) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
        if (peer.generation == 0) {
            peer = br_net_accept(e, text);
            if (peer.generation != 0) {
                static const char reply[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                BR_CHECK(br_net_send(e, peer, reply, sizeof reply - 1));
            }
        }
        uint8_t scratch[512];
        br_net_receive(e, peer, scratch, (int)sizeof scratch);
    }
    BR_CHECK(br_tls_state(e, s) == BR_NET_FAILED);
    br_log_info("test_tls: plain-text server: %s", br_tls_error(e, s));
    BR_CHECK(br_tls_error(e, s)[0] != '\0');
    br_tls_close(e, s);
    br_net_close(e, peer);
    br_net_server_close(e, text);
}

static void test_zero_handles(br_engine *e)
{
    br_tls_stream zero = { 0, 0 };
    br_tls_stream wild = { 12345, 9 };
    uint8_t buf[8];
    BR_CHECK(br_tls_state(e, zero) == BR_NET_CLOSED);
    BR_CHECK(br_tls_state(e, wild) == BR_NET_CLOSED);
    BR_CHECK(!br_tls_send(e, zero, "x", 1));
    BR_CHECK(br_tls_receive(e, zero, buf, (int)sizeof buf) == -1);
    BR_CHECK(br_tls_receive(e, wild, buf, (int)sizeof buf) == -1);
    BR_CHECK(SDL_strcmp(br_tls_error(e, zero), "") == 0);
    BR_CHECK(SDL_strcmp(br_tls_error(e, wild), "") == 0);
    br_tls_close(e, zero);
    br_tls_close(e, wild);
    BR_CHECK(br_tls_open(e, NULL, TRUSTED_PORT, true).generation == 0);
    BR_CHECK(br_tls_open(e, "", TRUSTED_PORT, true).generation == 0);
    BR_CHECK(br_tls_open(e, "localhost", 0, true).generation == 0);
    char long_host[BR_TLS_MAX_HOST + 2];
    SDL_memset(long_host, 'a', sizeof long_host - 1);
    long_host[sizeof long_host - 1] = '\0';
    BR_CHECK(br_tls_open(e, long_host, TRUSTED_PORT, true).generation == 0);
}

static void test_stream_limit(br_engine *e)
{
    static br_tls_stream streams[BR_TLS_MAX_STREAMS];
    for (int i = 0; i < BR_TLS_MAX_STREAMS; i++) {
        streams[i] = br_tls_open(e, "127.0.0.1", CLOSED_PORT, false);
        BR_CHECK(streams[i].generation != 0);
    }
    BR_CHECK(br_tls_open(e, "127.0.0.1", CLOSED_PORT, false).generation == 0); /* one more: logged */
    for (int i = 0; i < BR_TLS_MAX_STREAMS; i++) {
        br_tls_close(e, streams[i]);
    }
    br_tls_stream after = br_tls_open(e, "127.0.0.1", CLOSED_PORT, false);
    BR_CHECK(after.generation != 0);
    br_tls_close(e, after);
}

int main(void)
{
    test_not_registered();

    br_engine *e = br_engine_create("braty-test-tls", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_tls_register(e));
    BR_CHECK(br_tls_register(e)); /* idempotent */
    test_zero_handles(e);
    test_helpers(e);
    test_builtin_bundle(e);
    test_bad_ca(e);
    test_plain(e);

    BR_CHECK(tls_test_server_start(&trusted, e, TRUSTED_PORT, "server.pem"));
    BR_CHECK(tls_test_server_start(&untrusted, e, UNTRUSTED_PORT, "untrusted_server.pem"));
    BR_CHECK(tls_test_trust_ca(e));
    test_secure(e);
    test_verification(e);
    test_broken_servers(e);
    test_stream_limit(e);
    tls_test_server_stop(&trusted);
    tls_test_server_stop(&untrusted);

    br_engine_destroy(e);
    return br_test_finish();
}
