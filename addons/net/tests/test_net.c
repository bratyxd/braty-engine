/* net add-on on loopback: TCP connect / accept / send both ways (1 MB in one send), peer close,
 * refused port, unresolvable name, UDP round trip with the sender reported, oversized datagram
 * dropped, zero and stale handles, a full socket table, no allocation per send. Ports 47801..47804. */
#include "braty/braty.h"
#include "braty/br_net.h"
#include "br_test.h"

#include <SDL3/SDL.h>

#define TCP_PORT 47801
#define UDP_SERVER_PORT 47802
#define UDP_CLIENT_PORT 47803
#define CLOSED_PORT 47804
#define BIG_SIZE (1024 * 1024)
#define CLOSE_SENDS 16

static uint8_t big_out[BIG_SIZE];
static uint8_t big_in[BIG_SIZE];

/* Real-time deadline `seconds` from now: loops stop there however fast the machine is. */
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
    br_engine_step(e, 1.0f / 60.0f); /* false headless: nothing was drawn */
    SDL_Delay(1);
}

static bool step_until_state(br_engine *e, br_net_socket s, br_net_state wanted, double seconds)
{
    uint64_t deadline = deadline_in(seconds);
    while (br_net_socket_state(e, s) != wanted && before(deadline)) {
        step(e);
    }
    return br_net_socket_state(e, s) == wanted;
}

/* Reads exactly `size` bytes from s into out (stepping meanwhile); the number of reads, 0 on timeout. */
static int receive_exactly(br_engine *e, br_net_socket s, uint8_t *out, int size, int chunk)
{
    int got = 0;
    int reads = 0;
    uint64_t deadline = deadline_in(20.0);
    while (got < size && before(deadline)) {
        int capacity = size - got < chunk ? size - got : chunk;
        int n = br_net_receive(e, s, out + got, capacity);
        if (n < 0) {
            return 0;
        }
        if (n == 0) {
            step(e);
            continue;
        }
        got += n;
        reads++;
    }
    return got == size ? reads : 0;
}

/* Reads (and throws away) until br_net_receive returns -1; false on timeout. */
static bool receive_until_end(br_engine *e, br_net_socket s)
{
    uint8_t scratch[256];
    uint64_t deadline = deadline_in(10.0);
    while (before(deadline)) {
        int n = br_net_receive(e, s, scratch, (int)sizeof scratch);
        if (n < 0) {
            return true;
        }
        if (n == 0) {
            step(e);
        }
    }
    return false;
}

static void test_not_registered(void)
{
    br_engine *e = br_engine_create("braty-test-net-unregistered", NULL);
    BR_CHECK(e != NULL);
    br_net_socket s = br_net_connect(e, "127.0.0.1", TCP_PORT);
    BR_CHECK(s.generation == 0);
    BR_CHECK(br_net_socket_state(e, s) == BR_NET_CLOSED);
    BR_CHECK(br_net_listen(e, TCP_PORT).generation == 0);
    BR_CHECK(br_net_udp_open(e, 0).generation == 0);
    BR_CHECK(br_net_resolve(e, "127.0.0.1").generation == 0);
    br_engine_destroy(e);
}

/* Connects a client to the server and accepts it; both OPEN. */
static void connect_pair(br_engine *e, br_net_server server, br_net_socket *client, br_net_socket *peer)
{
    *client = br_net_connect(e, "127.0.0.1", TCP_PORT);
    BR_CHECK(client->generation != 0);
    BR_CHECK(br_net_socket_state(e, *client) == BR_NET_CONNECTING);
    br_net_socket none = { 0, 0 };
    *peer = none;
    uint64_t deadline = deadline_in(10.0);
    while ((peer->generation == 0 || br_net_socket_state(e, *client) != BR_NET_OPEN) && before(deadline)) {
        step(e);
        if (peer->generation == 0) {
            *peer = br_net_accept(e, server);
        }
    }
    BR_CHECK(br_net_socket_state(e, *client) == BR_NET_OPEN);
    BR_CHECK(peer->generation != 0);
    BR_CHECK(br_net_socket_state(e, *peer) == BR_NET_OPEN);
}

static void test_tcp(br_engine *e)
{
    br_net_server server = br_net_listen(e, TCP_PORT);
    BR_CHECK(server.generation != 0);
    BR_CHECK(br_net_accept(e, server).generation == 0); /* nobody waits yet */

    br_net_socket client, peer;
    connect_pair(e, server, &client, &peer);

    /* both ways */
    uint8_t small[16];
    BR_CHECK(br_net_send(e, client, "hello", 5));
    BR_CHECK(receive_exactly(e, peer, small, 5, (int)sizeof small) > 0);
    BR_CHECK(SDL_memcmp(small, "hello", 5) == 0);
    BR_CHECK(br_net_send(e, peer, "world!", 6));
    BR_CHECK(receive_exactly(e, client, small, 6, (int)sizeof small) > 0);
    BR_CHECK(SDL_memcmp(small, "world!", 6) == 0);
    BR_CHECK(br_net_send(e, client, "", 0)); /* nothing to send is fine */

    /* 1 MB in one send, received in pieces */
    for (int i = 0; i < BIG_SIZE; i++) {
        big_out[i] = (uint8_t)(i * 7 + (i >> 11));
    }
    BR_CHECK(br_net_send(e, client, big_out, BIG_SIZE));
    int reads = receive_exactly(e, peer, big_in, BIG_SIZE, 64 * 1024);
    BR_CHECK(reads >= 16);
    BR_CHECK(SDL_memcmp(big_in, big_out, BIG_SIZE) == 0);
    BR_CHECK(br_net_send_pending(e, client) == 0);

    /* steady small sends allocate nothing */
    br_memory_stats mem_before, mem_after;
    br_memory_get_stats(&mem_before);
    int small_got = 0;
    for (int i = 0; i < 200; i++) {
        BR_CHECK(br_net_send(e, client, "0123456789", 10));
        int n = br_net_receive(e, peer, big_in + small_got, 64);
        small_got += n > 0 ? n : 0;
    }
    br_memory_get_stats(&mem_after);
    BR_CHECK(mem_after.total_allocations == mem_before.total_allocations);
    if (small_got < 2000) {
        BR_CHECK(receive_exactly(e, peer, big_in + small_got, 2000 - small_got, 4096) > 0);
    }
    BR_CHECK(SDL_memcmp(big_in + 1990, "0123456789", 10) == 0);

    /* the peer closes: the client reads -1 and turns CLOSED */
    br_net_close(e, peer);
    BR_CHECK(br_net_socket_state(e, peer) == BR_NET_CLOSED);
    BR_CHECK(receive_until_end(e, client));
    BR_CHECK(br_net_socket_state(e, client) == BR_NET_CLOSED);
    BR_CHECK(!br_net_send(e, client, "x", 1));
    br_net_close(e, client);

    /* stale handles: the closed sockets, even after their slots are reused */
    br_net_socket again, again_peer;
    connect_pair(e, server, &again, &again_peer);
    BR_CHECK(br_net_socket_state(e, client) == BR_NET_CLOSED);
    BR_CHECK(br_net_socket_state(e, peer) == BR_NET_CLOSED);
    BR_CHECK(!br_net_send(e, client, "x", 1));
    BR_CHECK(br_net_receive(e, client, small, (int)sizeof small) == -1);
    BR_CHECK(br_net_send_pending(e, peer) == 0);
    br_net_close(e, client); /* no-op */
    BR_CHECK(br_net_socket_state(e, again) == BR_NET_OPEN);

    /* close right after big sends (more than the kernel buffers, so some stays queued in
     * SDL3_net): the queued bytes still arrive, then the end of the stream */
    for (int i = 0; i < CLOSE_SENDS; i++) {
        BR_CHECK(br_net_send(e, again, big_out, BIG_SIZE));
    }
    br_net_close(e, again);
    BR_CHECK(br_net_socket_state(e, again) == BR_NET_CLOSED);
    for (int i = 0; i < CLOSE_SENDS; i++) {
        SDL_memset(big_in, 0, BIG_SIZE);
        BR_CHECK(receive_exactly(e, again_peer, big_in, BIG_SIZE, 64 * 1024) > 0);
        BR_CHECK(SDL_memcmp(big_in, big_out, BIG_SIZE) == 0);
    }
    BR_CHECK(receive_until_end(e, again_peer));
    BR_CHECK(br_net_socket_state(e, again_peer) == BR_NET_CLOSED);
    br_net_close(e, again_peer);

    br_net_server_close(e, server);
    BR_CHECK(br_net_accept(e, server).generation == 0);
    br_net_server_close(e, server); /* no-op */
}

static void test_failures(br_engine *e)
{
    br_net_socket refused = br_net_connect(e, "127.0.0.1", CLOSED_PORT);
    BR_CHECK(refused.generation != 0);
    BR_CHECK(step_until_state(e, refused, BR_NET_FAILED, 20.0));
    BR_CHECK(!br_net_send(e, refused, "x", 1));
    uint8_t buf[8];
    BR_CHECK(br_net_receive(e, refused, buf, (int)sizeof buf) == -1);
    br_net_close(e, refused);
    BR_CHECK(br_net_socket_state(e, refused) == BR_NET_CLOSED);

    br_net_socket nowhere = br_net_connect(e, "no-such-host.invalid", TCP_PORT);
    BR_CHECK(nowhere.generation != 0);
    BR_CHECK(step_until_state(e, nowhere, BR_NET_FAILED, 30.0));
    br_net_close(e, nowhere);

    br_net_address bad = br_net_resolve(e, "no-such-host.invalid");
    uint64_t deadline = deadline_in(30.0);
    while (br_net_address_state(e, bad) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
    }
    BR_CHECK(br_net_address_state(e, bad) == BR_NET_FAILED);
    char text[64];
    BR_CHECK(!br_net_address_get_string(e, bad, text, sizeof text));
    BR_CHECK(text[0] == '\0');
    br_net_address_release(e, bad);

    BR_CHECK(br_net_connect(e, NULL, TCP_PORT).generation == 0);
    BR_CHECK(br_net_connect(e, "127.0.0.1", 0).generation == 0);
}

/* Steps until u has a datagram; its size, 0 on timeout. */
static int udp_wait(br_engine *e, br_net_udp u, void *buf, int capacity, br_net_address *from, uint16_t *port)
{
    uint64_t deadline = deadline_in(10.0);
    while (before(deadline)) {
        int n = br_net_udp_receive(e, u, buf, capacity, from, port);
        if (n != 0) {
            return n;
        }
        step(e);
    }
    return 0;
}

static void test_udp(br_engine *e)
{
    br_net_udp server = br_net_udp_open(e, UDP_SERVER_PORT);
    br_net_udp client = br_net_udp_open(e, UDP_CLIENT_PORT);
    BR_CHECK(server.generation != 0 && client.generation != 0);

    br_net_address to = br_net_resolve(e, "127.0.0.1");
    BR_CHECK(to.generation != 0);
    uint64_t deadline = deadline_in(10.0);
    while (br_net_address_state(e, to) == BR_NET_CONNECTING && before(deadline)) {
        step(e);
    }
    BR_CHECK(br_net_address_state(e, to) == BR_NET_OPEN);
    char text[64];
    BR_CHECK(br_net_address_get_string(e, to, text, sizeof text));
    BR_CHECK(SDL_strcmp(text, "127.0.0.1") == 0);

    char buf[256];
    BR_CHECK(br_net_udp_receive(e, server, buf, (int)sizeof buf, NULL, NULL) == 0); /* none yet */
    BR_CHECK(br_net_udp_send(e, client, to, UDP_SERVER_PORT, "ping", 4));
    br_net_address sender = { 0, 0 };
    uint16_t sender_port = 0;
    BR_CHECK(udp_wait(e, server, buf, (int)sizeof buf, &sender, &sender_port) == 4);
    BR_CHECK(SDL_memcmp(buf, "ping", 4) == 0);
    BR_CHECK(sender.generation != 0);
    BR_CHECK(sender_port == UDP_CLIENT_PORT);
    BR_CHECK(br_net_address_get_string(e, sender, text, sizeof text));
    BR_CHECK(SDL_strcmp(text, "127.0.0.1") == 0);

    /* reply to the reported sender */
    BR_CHECK(br_net_udp_send(e, server, sender, sender_port, "pong", 4));
    uint16_t reply_port = 0;
    BR_CHECK(udp_wait(e, client, buf, (int)sizeof buf, NULL, &reply_port) == 4);
    BR_CHECK(SDL_memcmp(buf, "pong", 4) == 0);
    BR_CHECK(reply_port == UDP_SERVER_PORT);

    /* the same sender again: the same handle */
    BR_CHECK(br_net_udp_send(e, client, to, UDP_SERVER_PORT, "again", 5));
    br_net_address sender2 = { 0, 0 };
    BR_CHECK(udp_wait(e, server, buf, (int)sizeof buf, &sender2, NULL) == 5);
    BR_CHECK(sender2.index == sender.index && sender2.generation == sender.generation);

    /* a datagram larger than the buffer is dropped, the next one arrives */
    SDL_memset(buf, 'x', 100);
    BR_CHECK(br_net_udp_send(e, client, to, UDP_SERVER_PORT, buf, 100));
    BR_CHECK(br_net_udp_send(e, client, to, UDP_SERVER_PORT, "ok", 2));
    char tiny[10];
    BR_CHECK(udp_wait(e, server, tiny, (int)sizeof tiny, NULL, NULL) == 2);
    BR_CHECK(SDL_memcmp(tiny, "ok", 2) == 0);

    /* bad sends */
    BR_CHECK(!br_net_udp_send(e, client, to, UDP_SERVER_PORT, buf, 0));
    BR_CHECK(!br_net_udp_send(e, client, to, UDP_SERVER_PORT, big_out, BR_NET_MAX_DATAGRAM + 1));

    /* released and closed handles are stale */
    br_net_address_release(e, sender);
    BR_CHECK(br_net_address_state(e, sender) == BR_NET_CLOSED);
    BR_CHECK(!br_net_udp_send(e, server, sender, UDP_CLIENT_PORT, "x", 1));
    br_net_address_release(e, sender); /* no-op */
    br_net_address_release(e, to);
    BR_CHECK(!br_net_udp_send(e, client, to, UDP_SERVER_PORT, "x", 1));
    br_net_udp_close(e, server);
    br_net_udp_close(e, client);
    BR_CHECK(br_net_udp_receive(e, server, buf, (int)sizeof buf, NULL, NULL) == -1);
    br_net_udp_close(e, server); /* no-op */
}

static void test_zero_handles(br_engine *e)
{
    br_net_socket s = { 0, 0 };
    br_net_server sv = { 0, 0 };
    br_net_udp u = { 0, 0 };
    br_net_address a = { 0, 0 };
    br_net_socket wild = { 12345, 7 };
    char buf[16];
    BR_CHECK(br_net_socket_state(e, s) == BR_NET_CLOSED);
    BR_CHECK(br_net_socket_state(e, wild) == BR_NET_CLOSED);
    BR_CHECK(!br_net_send(e, s, "x", 1));
    BR_CHECK(br_net_send_pending(e, s) == 0);
    BR_CHECK(br_net_receive(e, s, buf, (int)sizeof buf) == -1);
    BR_CHECK(br_net_receive(e, wild, buf, (int)sizeof buf) == -1);
    br_net_close(e, s);
    br_net_close(e, wild);
    BR_CHECK(br_net_accept(e, sv).generation == 0);
    br_net_server_close(e, sv);
    BR_CHECK(br_net_address_state(e, a) == BR_NET_CLOSED);
    BR_CHECK(!br_net_address_get_string(e, a, buf, sizeof buf));
    br_net_address_release(e, a);
    BR_CHECK(!br_net_udp_send(e, u, a, 1, "x", 1));
    BR_CHECK(br_net_udp_receive(e, u, buf, (int)sizeof buf, NULL, NULL) == -1);
    br_net_udp_close(e, u);
}

static void test_socket_limit(br_engine *e)
{
    static br_net_socket sockets[BR_NET_MAX_SOCKETS];
    for (int i = 0; i < BR_NET_MAX_SOCKETS; i++) {
        sockets[i] = br_net_connect(e, "127.0.0.1", CLOSED_PORT);
        BR_CHECK(sockets[i].generation != 0);
    }
    BR_CHECK(br_net_connect(e, "127.0.0.1", CLOSED_PORT).generation == 0); /* one more: logged */
    for (int i = 0; i < BR_NET_MAX_SOCKETS; i++) {
        br_net_close(e, sockets[i]);
    }
    br_net_socket after = br_net_connect(e, "127.0.0.1", CLOSED_PORT); /* slots are free again */
    BR_CHECK(after.generation != 0);
    br_net_close(e, after);
}

int main(void)
{
    test_not_registered();

    br_engine *e = br_engine_create("braty-test-net", NULL);
    BR_CHECK(e != NULL);
    BR_CHECK(br_net_register(e));
    BR_CHECK(br_net_register(e)); /* idempotent */
    test_zero_handles(e);
    test_tcp(e);
    test_failures(e);
    test_udp(e);
    test_socket_limit(e);
    br_engine_destroy(e);
    return br_test_finish();
}
