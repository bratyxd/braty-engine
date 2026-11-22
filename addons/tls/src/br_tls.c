/* br_tls: a fixed table of streams over br_net sockets. A secure stream runs an Mbed TLS client
 * whose BIO callbacks are br_net_send / br_net_receive; `tls.update` drives the handshakes. The
 * built-in roots are parsed once, by a br_job started by the first secure open. */
#include "braty/br_tls.h"
#include "br_tls_internal.h"

#include "braty/br_app.h"
#include "braty/br_job.h"

#include <SDL3/SDL.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

typedef enum br_tls_roots_state {
    BR_TLS_ROOTS_NONE,     /* built-in bundle not parsed (no secure open yet) */
    BR_TLS_ROOTS_PARSING,  /* the job runs: only it touches `builtin` and `builtin_result` */
    BR_TLS_ROOTS_DONE
} br_tls_roots_state;

typedef struct br_tls_slot {
    mbedtls_ssl_context ssl;          /* secure streams; init when taken, freed when released */
    br_engine          *e;            /* for the BIO callbacks */
    br_net_socket       socket;
    uint32_t            generation;
    uint16_t            port;
    br_net_state        state;        /* secure streams; a plain one reads its socket's state */
    bool                used;
    bool                secure;
    bool                verify;       /* br_tls_set_verify at open time */
    bool                ssl_ready;    /* mbedtls_ssl_setup done */
    bool                clean_close;  /* secure: CLOSED by the peer's close_notify */
    char                host[BR_TLS_MAX_HOST + 1];
    char                error[512];
} br_tls_slot;

typedef struct br_tls {
    br_tls_slot              streams[BR_TLS_MAX_STREAMS];
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config       conf_verify;   /* certificate + host name required */
    mbedtls_ssl_config       conf_none;     /* br_tls_set_verify(e, false) */
    mbedtls_x509_crt        *builtin;       /* the Mozilla bundle (parsed by the job) */
    mbedtls_x509_crt        *custom;        /* br_tls_set_ca_pem; used instead of builtin */
    br_job                   builtin_job;
    br_tls_roots_state       builtin_state;
    int                      builtin_result;  /* mbedtls_x509_crt_parse's return (the job writes it) */
    int                      builtin_count;   /* roots in `builtin` once DONE */
    bool                     verify;
} br_tls;

static uint32_t br_tls_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1 : generation + 1;
}

static br_tls *br_tls_get(br_engine *e)
{
    return e != NULL ? br_addon_get(e, "tls") : NULL;
}

static br_tls_slot *br_tls_slot_get(br_tls *t, br_tls_stream s)
{
    if (t == NULL || s.index >= BR_TLS_MAX_STREAMS) {
        return NULL;
    }
    br_tls_slot *slot = &t->streams[s.index];
    return slot->used && slot->generation == s.generation ? slot : NULL;
}

/* Sets FAILED with `what` (and Mbed TLS's text for code, when not 0) and logs it. */
static void br_tls_fail(br_tls_slot *slot, const char *what, int code)
{
    char detail[200] = "";
    if (code != 0) {
        mbedtls_strerror(code, detail, sizeof detail);
    }
    SDL_snprintf(slot->error, sizeof slot->error, "%s%s%s", what, code != 0 ? ": " : "", detail);
    slot->state = BR_NET_FAILED;
    br_log_error("tls: %s", slot->error);
}

/* ---- BIO callbacks: Mbed TLS reads and writes the br_net socket */

static int br_tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    br_tls_slot *slot = ctx;
    if (!br_net_send(slot->e, slot->socket, buf, len)) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)len;  /* br_net_send queues all of it; records are far below INT_MAX */
}

static int br_tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    br_tls_slot *slot = ctx;
    int capacity = len > (size_t)SDL_MAX_SINT32 ? SDL_MAX_SINT32 : (int)len;
    int n = br_net_receive(slot->e, slot->socket, buf, capacity);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    /* 0 = end of stream (the peer closed); a broken socket is an error */
    return br_net_socket_state(slot->e, slot->socket) == BR_NET_FAILED ? MBEDTLS_ERR_NET_RECV_FAILED : 0;
}

/* ---- root certificates */

static void br_tls_parse_builtin(void *user, uint32_t begin, uint32_t end)  /* worker thread */
{
    (void)begin;
    (void)end;
    br_tls *t = user;
    t->builtin_result = mbedtls_x509_crt_parse(t->builtin, br_tls_cacert, br_tls_cacert_size + 1);
}

static int br_tls_chain_count(const mbedtls_x509_crt *chain)
{
    int count = 0;
    for (const mbedtls_x509_crt *c = chain; c != NULL && c->raw.p != NULL; c = c->next) {
        count++;
    }
    return count;
}

/* The roots secure streams verify against: the custom ones, else the built-in ones once parsed. */
static void br_tls_apply_roots(br_tls *t)
{
    mbedtls_x509_crt *chain = t->custom;
    if (chain == NULL && t->builtin_state == BR_TLS_ROOTS_DONE && t->builtin_count > 0) {
        chain = t->builtin;
    }
    mbedtls_ssl_conf_ca_chain(&t->conf_verify, chain, NULL);
    mbedtls_ssl_conf_ca_chain(&t->conf_none, chain, NULL);
}

static void br_tls_start_builtin(br_engine *e, br_tls *t)
{
    if (t->builtin_state != BR_TLS_ROOTS_NONE) {
        return;
    }
    t->builtin = br_alloc(sizeof(mbedtls_x509_crt));
    if (t->builtin == NULL) {
        t->builtin_state = BR_TLS_ROOTS_DONE;  /* count 0: secure streams fail with a reason */
        return;
    }
    mbedtls_x509_crt_init(t->builtin);
    t->builtin_state = BR_TLS_ROOTS_PARSING;
    t->builtin_job = br_job_run(e, br_tls_parse_builtin, t, 1, 0);
}

/* Takes the parse job's result once it is done. */
static void br_tls_poll_builtin(br_engine *e, br_tls *t)
{
    if (t->builtin_state != BR_TLS_ROOTS_PARSING || !br_job_is_done(e, t->builtin_job)) {
        return;
    }
    t->builtin_state = BR_TLS_ROOTS_DONE;
    t->builtin_count = t->builtin_result < 0 ? 0 : br_tls_chain_count(t->builtin);
    if (t->builtin_count == 0) {
        char detail[200];
        mbedtls_strerror(t->builtin_result, detail, sizeof detail);
        br_log_error("tls: the built-in root certificates did not parse: %s", detail);
    } else if (t->builtin_result > 0) {
        br_log_info("tls: %d built-in root certificates (%d skipped: not supported)", t->builtin_count,
                    t->builtin_result);
    }
    br_tls_apply_roots(t);
}

int br_tls_builtin_root_count(br_engine *e)
{
    br_tls *t = br_tls_get(e);
    if (t == NULL) {
        return 0;
    }
    br_tls_start_builtin(e, t);
    br_tls_poll_builtin(e, t);
    return t->builtin_state == BR_TLS_ROOTS_DONE ? t->builtin_count : -1;
}

/* ---- handshakes */

/* "certificate of host:port not accepted: <Mbed TLS's reasons>" */
static void br_tls_fail_verify(br_tls_slot *slot, uint32_t flags)
{
    char info[300];
    if (mbedtls_x509_crt_verify_info(info, sizeof info, "", flags) < 0) {
        SDL_strlcpy(info, "(reasons too long)", sizeof info);
    }
    size_t length = SDL_strlen(info);
    while (length > 0 && info[length - 1] == '\n') {
        info[--length] = '\0';
    }
    for (size_t i = 0; i < length; i++) {
        if (info[i] == '\n') {
            info[i] = ';';
        }
    }
    char what[512];
    SDL_snprintf(what, sizeof what, "certificate of %s:%u not accepted: %s", slot->host, (unsigned)slot->port, info);
    br_tls_fail(slot, what, 0);
}

static void br_tls_handshake(br_tls *t, br_tls_slot *slot)
{
    char what[320];
    br_net_state net = br_net_socket_state(slot->e, slot->socket);
    if (net == BR_NET_CONNECTING) {
        return;
    }
    if (net != BR_NET_OPEN) {
        SDL_snprintf(what, sizeof what, net == BR_NET_FAILED ? "cannot connect to %s:%u"
                                                             : "%s:%u closed the connection",
                     slot->host, (unsigned)slot->port);
        br_tls_fail(slot, what, 0);
        return;
    }
    if (slot->verify && t->custom == NULL) {
        if (t->builtin_state != BR_TLS_ROOTS_DONE) {
            return;  /* the parse job runs */
        }
        if (t->builtin_count == 0) {
            SDL_snprintf(what, sizeof what, "no root certificates to verify %s with", slot->host);
            br_tls_fail(slot, what, 0);
            return;
        }
    }
    if (!slot->ssl_ready) {
        int ret = mbedtls_ssl_setup(&slot->ssl, slot->verify ? &t->conf_verify : &t->conf_none);
        if (ret == 0) {
            ret = mbedtls_ssl_set_hostname(&slot->ssl, slot->host);  /* SNI and the name verified */
        }
        if (ret != 0) {
            SDL_snprintf(what, sizeof what, "cannot start TLS to %s", slot->host);
            br_tls_fail(slot, what, ret);
            return;
        }
        mbedtls_ssl_set_bio(&slot->ssl, slot, br_tls_bio_send, br_tls_bio_recv, NULL);
        slot->ssl_ready = true;
    }
    /* ponytail: the handshake's public-key maths runs here on the main thread (a few ms per
     * connection); a br_job per handshake if games open many connections at once. */
    int ret = mbedtls_ssl_handshake(&slot->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return;
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&slot->ssl);
    if (ret != 0) {
        if (slot->verify && flags != 0 && flags != UINT32_MAX) {
            br_tls_fail_verify(slot, flags);
        } else {
            SDL_snprintf(what, sizeof what, "TLS handshake with %s:%u failed", slot->host, (unsigned)slot->port);
            br_tls_fail(slot, what, ret);
        }
        return;
    }
    if (slot->verify && flags != 0) {  /* REQUIRED already enforced it; never report OPEN unverified */
        br_tls_fail_verify(slot, flags);
        return;
    }
    slot->state = BR_NET_OPEN;
}

static void br_tls_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_tls *t = user;
    br_tls_poll_builtin(e, t);
    for (uint32_t i = 0; i < BR_TLS_MAX_STREAMS; i++) {
        br_tls_slot *slot = &t->streams[i];
        if (!slot->used || !slot->secure) {
            continue;
        }
        if (slot->state == BR_NET_CONNECTING) {
            br_tls_handshake(t, slot);
        } else if (slot->state == BR_NET_OPEN && br_net_socket_state(e, slot->socket) == BR_NET_FAILED) {
            char what[300];
            SDL_snprintf(what, sizeof what, "connection to %s:%u failed", slot->host, (unsigned)slot->port);
            br_tls_fail(slot, what, 0);
        }
    }
}

static void br_tls_release(br_tls_slot *slot)
{
    br_net_close(slot->e, slot->socket);
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_init(&slot->ssl);  /* ready for the slot's next stream */
    slot->used = false;
}

static void br_tls_free_chain(mbedtls_x509_crt *chain)
{
    if (chain != NULL) {
        mbedtls_x509_crt_free(chain);
        br_free(chain);
    }
}

static void br_tls_shutdown(br_engine *e, void *user)
{
    br_tls *t = user;
    br_job_cancel(e, t->builtin_job);  /* the engine cancelled it already; this is cheap */
    for (uint32_t i = 0; i < BR_TLS_MAX_STREAMS; i++) {
        if (t->streams[i].used) {
            br_tls_release(&t->streams[i]);
        }
    }
    mbedtls_ssl_config_free(&t->conf_verify);
    mbedtls_ssl_config_free(&t->conf_none);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    br_tls_free_chain(t->builtin);
    br_tls_free_chain(t->custom);
    br_free(t);
    /* ponytail: PSA crypto is process-wide and stays up (another engine may use it); freeing it
     * needs a process-wide reference count. */
}

static bool br_tls_conf_init(br_tls *t, mbedtls_ssl_config *conf, int authmode)
{
    mbedtls_ssl_config_init(conf);
    int ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        char detail[200];
        mbedtls_strerror(ret, detail, sizeof detail);
        br_log_error("br_tls_register: %s", detail);
        return false;
    }
    mbedtls_ssl_conf_authmode(conf, authmode);
    mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, &t->drbg);
    return true;
}

bool br_tls_register(br_engine *e)
{
    if (e == NULL) {
        br_log_error("br_tls_register: no engine");
        return false;
    }
    if (br_tls_get(e) != NULL) {
        return true;
    }
    if (!br_net_register(e)) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {  /* TLS 1.3 runs on PSA; idempotent */
        br_log_error("br_tls_register: PSA crypto failed to start");
        return false;
    }
    br_tls *t = br_alloc(sizeof(br_tls));
    if (t == NULL) {
        return false;
    }
    SDL_zerop(t);
    t->verify = true;
    for (uint32_t i = 0; i < BR_TLS_MAX_STREAMS; i++) {
        mbedtls_ssl_init(&t->streams[i].ssl);
    }
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    static const unsigned char personal[] = "braty tls";
    int ret = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, personal, sizeof personal - 1);
    bool ok = ret == 0;
    if (!ok) {
        char detail[200];
        mbedtls_strerror(ret, detail, sizeof detail);
        br_log_error("br_tls_register: no random seed: %s", detail);
    }
    ok = ok && br_tls_conf_init(t, &t->conf_verify, MBEDTLS_SSL_VERIFY_REQUIRED);
    ok = ok && br_tls_conf_init(t, &t->conf_none, MBEDTLS_SSL_VERIFY_NONE);
    ok = ok && br_addon_add(e, "tls", t);
    if (!ok) {
        mbedtls_ssl_config_free(&t->conf_verify);
        mbedtls_ssl_config_free(&t->conf_none);
        mbedtls_ctr_drbg_free(&t->drbg);
        mbedtls_entropy_free(&t->entropy);
        br_free(t);
        return false;
    }
    br_system_add(e, "tls.update", BR_PHASE_INPUT, BR_TLS_UPDATE_PRIORITY, br_tls_update, t);
    br_engine_on_shutdown(e, br_tls_shutdown, t);
    return true;
}

/* ---- streams */

br_tls_stream br_tls_open(br_engine *e, const char *host, uint16_t port, bool secure)
{
    br_tls_stream none = { 0, 0 };
    br_tls *t = br_tls_get(e);
    if (t == NULL) {
        br_log_error("br_tls_open: call br_tls_register first");
        return none;
    }
    if (host == NULL || host[0] == '\0' || port == 0 || SDL_strlen(host) > BR_TLS_MAX_HOST) {
        br_log_error("br_tls_open: needs a host (at most %d bytes) and a port", BR_TLS_MAX_HOST);
        return none;
    }
    uint32_t index = 0;
    while (index < BR_TLS_MAX_STREAMS && t->streams[index].used) {
        index++;
    }
    if (index == BR_TLS_MAX_STREAMS) {
        br_log_error("br_tls_open: all %d streams are in use (BR_TLS_MAX_STREAMS)", BR_TLS_MAX_STREAMS);
        return none;
    }
    br_net_socket socket = br_net_connect(e, host, port);
    if (socket.generation == 0) {
        return none;  /* logged by net */
    }
    br_tls_slot *slot = &t->streams[index];
    slot->generation = br_tls_next_generation(slot->generation);
    slot->used = true;
    slot->e = e;
    slot->socket = socket;
    slot->port = port;
    slot->state = BR_NET_CONNECTING;
    slot->secure = secure;
    slot->verify = t->verify;
    slot->ssl_ready = false;
    slot->clean_close = false;
    slot->error[0] = '\0';
    SDL_strlcpy(slot->host, host, sizeof slot->host);
    if (secure) {
        if (!t->verify) {
            br_log_warn("br_tls_open: %s:%u without certificate verification (br_tls_set_verify)", host, (unsigned)port);
        }
        if (t->custom == NULL) {
            br_tls_start_builtin(e, t);
        }
    }
    br_tls_stream handle = { index, slot->generation };
    return handle;
}

br_net_state br_tls_state(br_engine *e, br_tls_stream s)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return BR_NET_CLOSED;
    }
    return slot->secure ? slot->state : br_net_socket_state(e, slot->socket);
}

bool br_tls_send(br_engine *e, br_tls_stream s, const void *data, size_t size)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return false;
    }
    if (!slot->secure) {
        return br_net_send(e, slot->socket, data, size);
    }
    if (slot->state != BR_NET_OPEN) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (data == NULL) {
        br_log_error("br_tls_send: no data");
        return false;
    }
    const unsigned char *p = data;
    while (size > 0) {
        int ret = mbedtls_ssl_write(&slot->ssl, p, size);  /* one record (at most 16 KB) per call */
        if (ret <= 0) {
            char what[300];
            SDL_snprintf(what, sizeof what, "sending to %s:%u failed", slot->host, (unsigned)slot->port);
            br_tls_fail(slot, what, ret);
            return false;
        }
        p += ret;
        size -= (size_t)ret;
    }
    return true;
}

int br_tls_receive(br_engine *e, br_tls_stream s, void *buffer, int capacity)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return -1;
    }
    if (!slot->secure) {
        return br_net_receive(e, slot->socket, buffer, capacity);
    }
    if (slot->state == BR_NET_CLOSED || slot->state == BR_NET_FAILED) {
        return -1;
    }
    if (slot->state == BR_NET_CONNECTING || buffer == NULL || capacity <= 0) {
        return 0;
    }
    for (int tries = 0; tries < 8; tries++) {  /* bounded: a server flooding session tickets */
        int ret = mbedtls_ssl_read(&slot->ssl, buffer, (size_t)capacity);
        if (ret > 0) {
            return ret;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;  /* TLS 1.3 tickets: not used (no session resumption) */
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF) {
            /* close_notify, or the TCP stream ended without it (many servers do that; so can an
             * attacker cutting the data short: br_tls_closed_cleanly tells the two apart) */
            slot->state = BR_NET_CLOSED;
            slot->clean_close = ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            return -1;
        }
        char what[300];
        SDL_snprintf(what, sizeof what, "receiving from %s:%u failed", slot->host, (unsigned)slot->port);
        br_tls_fail(slot, what, ret);
        return -1;
    }
    return 0;
}

bool br_tls_closed_cleanly(br_engine *e, br_tls_stream s)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return false;
    }
    if (!slot->secure) {
        return br_net_socket_state(e, slot->socket) == BR_NET_CLOSED;
    }
    return slot->state == BR_NET_CLOSED && slot->clean_close;
}

void br_tls_close(br_engine *e, br_tls_stream s)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return;
    }
    if (slot->secure && slot->state == BR_NET_OPEN) {
        mbedtls_ssl_close_notify(&slot->ssl);  /* queued on the socket; br_net_close flushes it */
    }
    br_tls_release(slot);
}

const char *br_tls_error(br_engine *e, br_tls_stream s)
{
    br_tls_slot *slot = br_tls_slot_get(br_tls_get(e), s);
    if (slot == NULL) {
        return "";
    }
    if (!slot->secure && slot->error[0] == '\0' && br_net_socket_state(e, slot->socket) == BR_NET_FAILED) {
        SDL_snprintf(slot->error, sizeof slot->error, "connection to %s:%u failed", slot->host, (unsigned)slot->port);
    }
    return br_tls_state(e, s) == BR_NET_FAILED ? slot->error : "";
}

/* ---- settings */

bool br_tls_set_ca_pem(br_engine *e, const char *pem, size_t size)
{
    br_tls *t = br_tls_get(e);
    if (t == NULL) {
        br_log_error("br_tls_set_ca_pem: call br_tls_register first");
        return false;
    }
    if (pem == NULL || size == 0) {
        br_log_error("br_tls_set_ca_pem: no PEM text");
        return false;
    }
    if (pem[size - 1] == '\0') {
        size--;
    }
    unsigned char *text = br_alloc(size + 1);  /* Mbed TLS parses PEM only NUL-terminated */
    mbedtls_x509_crt *chain = br_alloc(sizeof(mbedtls_x509_crt));
    if (text == NULL || chain == NULL) {
        br_free(text);
        br_free(chain);
        return false;
    }
    SDL_memcpy(text, pem, size);
    text[size] = '\0';
    mbedtls_x509_crt_init(chain);
    int ret = mbedtls_x509_crt_parse(chain, text, size + 1);
    br_free(text);
    int count = ret < 0 ? 0 : br_tls_chain_count(chain);
    if (count == 0) {
        char detail[200];
        mbedtls_strerror(ret, detail, sizeof detail);
        br_log_error("br_tls_set_ca_pem: no certificate parsed: %s", detail);
        br_tls_free_chain(chain);
        return false;
    }
    if (ret > 0) {
        br_log_warn("br_tls_set_ca_pem: %d certificates did not parse; using the other %d", ret, count);
    }
    /* Streams still handshaking read conf->ca_chain on their next step, so the old chain can go. */
    br_tls_free_chain(t->custom);
    t->custom = chain;
    br_tls_apply_roots(t);
    return true;
}

void br_tls_set_verify(br_engine *e, bool on)
{
    br_tls *t = br_tls_get(e);
    if (t == NULL) {
        br_log_error("br_tls_set_verify: call br_tls_register first");
        return;
    }
    t->verify = on;
}

/* ---- helpers */

bool br_tls_random(br_engine *e, void *out, size_t size)
{
    br_tls *t = br_tls_get(e);
    if (t == NULL || (out == NULL && size > 0)) {
        br_log_error("br_tls_random: call br_tls_register first, and pass a buffer");
        return false;
    }
    unsigned char *p = out;
    while (size > 0) {
        size_t chunk = size < MBEDTLS_CTR_DRBG_MAX_REQUEST ? size : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        int ret = mbedtls_ctr_drbg_random(&t->drbg, p, chunk);
        if (ret != 0) {
            char detail[200];
            mbedtls_strerror(ret, detail, sizeof detail);
            br_log_error("br_tls_random: %s", detail);
            return false;
        }
        p += chunk;
        size -= chunk;
    }
    return true;
}

void br_tls_sha1(const void *data, size_t size, uint8_t out[20])
{
    static const unsigned char nothing[1] = { 0 };
    mbedtls_sha1(data != NULL ? data : nothing, data != NULL ? size : 0, out);
}

size_t br_tls_base64_encode(const void *data, size_t size, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (data == NULL || size == 0) {
        return 0;
    }
    size_t length = 0;
    if (mbedtls_base64_encode((unsigned char *)out, capacity, &length, data, size) != 0) {
        out[0] = '\0';
        return 0;
    }
    return length;
}
