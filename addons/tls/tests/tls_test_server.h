/* tls_test_server.h - a loopback TLS server for tests: Mbed TLS used directly over a br_net
 * server socket, pumped in the test's own loop (no threads). Used by test_tls and by the http
 * and websocket tests (link the CMake target braty_tls_test_server; certificates in
 * tests/data/tls/, the client trusts tests/data/tls/ca.pem through br_tls_set_ca_pem and opens
 * host "localhost").
 *
 *   static tls_test_server server;                   // big: static or in a struct, never moved
 *   static tls_test_conn conn;                       // the same: Mbed TLS keeps its address
 *   tls_test_server_start(&server, e, 47821, "server.pem");
 *   tls_test_trust_ca(e);                            // br_tls_set_ca_pem(ca.pem)
 *   ...client opens https://localhost:47821...
 *   while (looping) {
 *       br_engine_step(e, dt);
 *       if (!conn.used) { tls_test_server_accept(&server, &conn); }
 *       int n = tls_test_conn_receive(&conn, buf, sizeof buf);    // drives the handshake too
 *       if (n > 0) { tls_test_conn_send(&conn, reply, reply_size); }
 *   }
 *   tls_test_conn_close(&conn);
 *   tls_test_server_stop(&server);
 */
#ifndef TLS_TEST_SERVER_H
#define TLS_TEST_SERVER_H

#include "braty/braty.h"
#include "braty/br_tls.h"

#include <SDL3/SDL.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define TLS_TEST_DATA_DIR BRATY_SOURCE_DIR "/tests/data/tls/"

typedef struct tls_test_server {
    br_engine               *e;
    br_net_server            listener;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cert;
    mbedtls_pk_context       key;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
} tls_test_server;

/* One accepted client. state: 0 handshaking, 1 open, -1 closed or failed. */
typedef struct tls_test_conn {
    br_engine          *e;
    br_net_socket       socket;
    mbedtls_ssl_context ssl;
    int                 state;
    bool                used;
} tls_test_conn;

/* A file of tests/data/tls/ as NUL-terminated text (SDL_free it); *size counts the NUL. */
static inline unsigned char *tls_test_load(const char *name, size_t *size)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", TLS_TEST_DATA_DIR, name);
    size_t length = 0;
    unsigned char *data = SDL_LoadFile(path, &length);  /* SDL adds a NUL after the data */
    if (data == NULL) {
        br_log_error("tls_test_server: cannot read %s: %s", path, SDL_GetError());
        return NULL;
    }
    *size = length + 1;
    return data;
}

/* br_tls_set_ca_pem with tests/data/tls/ca.pem (or another file of that folder). */
static inline bool tls_test_trust(br_engine *e, const char *ca_file)
{
    size_t size = 0;
    unsigned char *pem = tls_test_load(ca_file, &size);
    bool ok = pem != NULL && br_tls_set_ca_pem(e, (const char *)pem, size);
    SDL_free(pem);
    return ok;
}

/* The client side trusts the test CA. */
static inline bool tls_test_trust_ca(br_engine *e)
{
    return tls_test_trust(e, "ca.pem");
}

/* Listens on port with cert_file ("server.pem": signed by the test CA; "untrusted_server.pem":
 * signed by a CA nobody trusts) and server.key. Needs br_tls_register (or br_net_register). */
static inline bool tls_test_server_start(tls_test_server *s, br_engine *e, uint16_t port, const char *cert_file)
{
    SDL_zerop(s);
    s->e = e;
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_pk_init(&s->key);
    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->drbg);
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->entropy, NULL, 0) != 0) {
        br_log_error("tls_test_server: no random generator");
        return false;
    }
    size_t cert_size = 0, key_size = 0;
    unsigned char *cert = tls_test_load(cert_file, &cert_size);
    unsigned char *key = tls_test_load("server.key", &key_size);
    bool ok = cert != NULL && key != NULL && mbedtls_x509_crt_parse(&s->cert, cert, cert_size) == 0 &&
              mbedtls_pk_parse_key(&s->key, key, key_size, NULL, 0, mbedtls_ctr_drbg_random, &s->drbg) == 0;
    SDL_free(cert);
    SDL_free(key);
    if (!ok) {
        br_log_error("tls_test_server: cannot load %s / server.key", cert_file);
        return false;
    }
    if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&s->conf, &s->cert, &s->key) != 0) {
        br_log_error("tls_test_server: bad configuration");
        return false;
    }
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->drbg);
    s->listener = br_net_listen(e, port);
    return s->listener.generation != 0;
}

static inline int tls_test_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    tls_test_conn *c = ctx;
    return br_net_send(c->e, c->socket, buf, len) ? (int)len : MBEDTLS_ERR_NET_SEND_FAILED;
}

static inline int tls_test_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    tls_test_conn *c = ctx;
    int n = br_net_receive(c->e, c->socket, buf, len > 65536 ? 65536 : (int)len);
    return n > 0 ? n : n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : 0;
}

/* Takes a waiting client into c (which must stay where it is until tls_test_conn_close): true
 * when one was accepted. Its handshake then runs in tls_test_conn_pump / _receive. */
static inline bool tls_test_server_accept(tls_test_server *s, tls_test_conn *c)
{
    br_net_socket socket = br_net_accept(s->e, s->listener);
    if (socket.generation == 0) {
        return false;
    }
    SDL_zerop(c);
    c->e = s->e;
    c->socket = socket;
    c->used = true;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &s->conf) != 0) {
        c->state = -1;
        return true;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, tls_test_bio_send, tls_test_bio_recv, NULL);
    return true;
}

/* Moves the handshake on: 1 open, 0 still handshaking (or no client), -1 failed or closed. */
static inline int tls_test_conn_pump(tls_test_conn *c)
{
    if (!c->used) {
        return 0;
    }
    if (c->state == 0) {
        int ret = mbedtls_ssl_handshake(&c->ssl);
        if (ret == 0) {
            c->state = 1;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            c->state = -1;
        }
    }
    return c->state;
}

/* As br_net_receive: bytes, 0 = none now (or still handshaking), -1 = closed / failed. */
static inline int tls_test_conn_receive(tls_test_conn *c, void *buffer, int capacity)
{
    int state = tls_test_conn_pump(c);
    if (state <= 0) {
        return state;
    }
    for (;;) {
        int ret = mbedtls_ssl_read(&c->ssl, buffer, (size_t)capacity);
        if (ret > 0) {
            return ret;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        c->state = -1;  /* close_notify, end of stream or an error */
        return -1;
    }
}

/* Encrypts and queues all size bytes; false unless open. */
static inline bool tls_test_conn_send(tls_test_conn *c, const void *data, size_t size)
{
    if (!c->used || c->state != 1) {
        return false;
    }
    const unsigned char *p = data;
    while (size > 0) {
        int ret = mbedtls_ssl_write(&c->ssl, p, size);
        if (ret <= 0) {
            c->state = -1;
            return false;
        }
        p += ret;
        size -= (size_t)ret;
    }
    return true;
}

/* close_notify (when open), then closes the socket; c is free for the next accept. */
static inline void tls_test_conn_close(tls_test_conn *c)
{
    if (!c->used) {
        return;
    }
    if (c->state == 1) {
        mbedtls_ssl_close_notify(&c->ssl);
    }
    br_net_close(c->e, c->socket);
    mbedtls_ssl_free(&c->ssl);
    c->used = false;
}

/* Stops listening and frees the server's Mbed TLS state (close the conns first). */
static inline void tls_test_server_stop(tls_test_server *s)
{
    br_net_server_close(s->e, s->listener);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->cert);
    mbedtls_pk_free(&s->key);
    mbedtls_ctr_drbg_free(&s->drbg);
    mbedtls_entropy_free(&s->entropy);
}

#endif
