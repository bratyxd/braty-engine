/* br_net: fixed tables of SDL3_net objects behind {index, generation} handles. A slot's
 * generation is bumped when it is taken, so handles to a freed or reused slot read as stale. */
#include "braty/br_net.h"

#include "braty/br_app.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

typedef struct br_net_socket_slot {
    NET_Address      *address;       /* while resolving (br_net_connect) */
    NET_StreamSocket *stream;        /* after the name resolved, or accepted */
    uint64_t          linger_until;  /* closing: SDL_GetTicksNS deadline for queued bytes */
    uint32_t          generation;
    uint16_t          port;
    br_net_state      state;
    bool              used;
    bool              closing;       /* handle already freed by br_net_close, bytes still queued */
    char              host[256];     /* for log lines */
} br_net_socket_slot;

typedef struct br_net_server_slot {
    NET_Server *server;
    uint32_t    generation;
    bool        used;
} br_net_server_slot;

typedef struct br_net_udp_slot {
    NET_DatagramSocket *socket;
    uint32_t            generation;
    bool                used;
} br_net_udp_slot;

typedef struct br_net_address_slot {
    NET_Address *address;
    uint32_t     generation;
    bool         used;
    bool         from_receive;  /* made by br_net_udp_receive: reused for the same sender */
} br_net_address_slot;

typedef struct br_net {
    br_net_socket_slot  sockets[BR_NET_MAX_SOCKETS];
    br_net_server_slot  servers[BR_NET_MAX_SERVERS];
    br_net_udp_slot     udp[BR_NET_MAX_UDP];
    br_net_address_slot addresses[BR_NET_MAX_ADDRESSES];
    bool                address_full_logged;  /* one log line per filling of the address table */
} br_net;

static uint32_t br_net_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1 : generation + 1;
}

static br_net *br_net_get(br_engine *e)
{
    return e != NULL ? br_addon_get(e, "net") : NULL;
}

/* For the functions that start something: the state, or NULL + log when not registered. */
static br_net *br_net_get_logged(br_engine *e, const char *function)
{
    br_net *n = br_net_get(e);
    if (n == NULL) {
        br_log_error("%s: call br_net_register first", function);
    }
    return n;
}

static br_net_socket_slot *br_net_socket_slot_get(br_net *n, br_net_socket s)
{
    if (n == NULL || s.index >= BR_NET_MAX_SOCKETS) {
        return NULL;
    }
    br_net_socket_slot *slot = &n->sockets[s.index];
    return slot->used && !slot->closing && slot->generation == s.generation ? slot : NULL;
}

static br_net_server_slot *br_net_server_slot_get(br_net *n, br_net_server s)
{
    if (n == NULL || s.index >= BR_NET_MAX_SERVERS) {
        return NULL;
    }
    br_net_server_slot *slot = &n->servers[s.index];
    return slot->used && slot->generation == s.generation ? slot : NULL;
}

static br_net_udp_slot *br_net_udp_slot_get(br_net *n, br_net_udp u)
{
    if (n == NULL || u.index >= BR_NET_MAX_UDP) {
        return NULL;
    }
    br_net_udp_slot *slot = &n->udp[u.index];
    return slot->used && slot->generation == u.generation ? slot : NULL;
}

static br_net_address_slot *br_net_address_slot_get(br_net *n, br_net_address a)
{
    if (n == NULL || a.index >= BR_NET_MAX_ADDRESSES) {
        return NULL;
    }
    br_net_address_slot *slot = &n->addresses[a.index];
    return slot->used && slot->generation == a.generation ? slot : NULL;
}

/* A free socket slot, taken (zeroed, new generation); NULL + log when the table is full. */
static br_net_socket_slot *br_net_socket_take(br_net *n, const char *function, br_net_socket *out)
{
    for (uint32_t i = 0; i < BR_NET_MAX_SOCKETS; i++) {
        br_net_socket_slot *slot = &n->sockets[i];
        if (!slot->used) {
            uint32_t generation = br_net_next_generation(slot->generation);
            SDL_zerop(slot);
            slot->generation = generation;
            slot->used = true;
            out->index = i;
            out->generation = generation;
            return slot;
        }
    }
    br_log_error("%s: all %d sockets are in use (BR_NET_MAX_SOCKETS)", function, BR_NET_MAX_SOCKETS);
    return NULL;
}

static void br_net_socket_free(br_net_socket_slot *slot)
{
    NET_UnrefAddress(slot->address);  /* NULL is fine */
    NET_DestroyStreamSocket(slot->stream);
    slot->address = NULL;
    slot->stream = NULL;
    slot->used = false;
    slot->closing = false;
}

static void br_net_address_free(br_net *n, br_net_address_slot *slot)
{
    NET_UnrefAddress(slot->address);
    slot->address = NULL;
    slot->used = false;
    n->address_full_logged = false;
}

/* Moves one socket on: resolve -> connect -> OPEN / FAILED, flush queued bytes, finish a close. */
static void br_net_socket_update(br_net_socket_slot *slot, uint64_t now)
{
    if (slot->closing) {
        int pending = NET_GetStreamSocketPendingWrites(slot->stream);
        if (pending <= 0 || now >= slot->linger_until) {
            br_net_socket_free(slot);
        }
        return;
    }
    if (slot->state == BR_NET_CONNECTING && slot->stream == NULL) {
        NET_Status status = NET_GetAddressStatus(slot->address);
        if (status == NET_WAITING) {
            return;
        }
        if (status == NET_FAILURE) {
            br_log_error("net: cannot resolve '%s': %s", slot->host, SDL_GetError());
            slot->state = BR_NET_FAILED;
            return;
        }
        slot->stream = NET_CreateClient(slot->address, slot->port, 0);
        NET_UnrefAddress(slot->address);  /* the stream keeps its own reference */
        slot->address = NULL;
        if (slot->stream == NULL) {
            br_log_error("net: cannot connect to %s:%u: %s", slot->host, (unsigned)slot->port, SDL_GetError());
            slot->state = BR_NET_FAILED;
            return;
        }
    }
    if (slot->state == BR_NET_CONNECTING) {
        NET_Status status = NET_GetConnectionStatus(slot->stream);
        if (status == NET_SUCCESS) {
            slot->state = BR_NET_OPEN;
        } else if (status == NET_FAILURE) {
            br_log_error("net: cannot connect to %s:%u: %s", slot->host, (unsigned)slot->port, SDL_GetError());
            slot->state = BR_NET_FAILED;
        }
        return;
    }
    if (slot->state == BR_NET_OPEN && NET_GetStreamSocketPendingWrites(slot->stream) < 0) {
        br_log_error("net: sending to %s:%u failed: %s", slot->host, (unsigned)slot->port, SDL_GetError());
        slot->state = BR_NET_FAILED;
    }
}

static void br_net_update(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    br_net *n = user;
    uint64_t now = SDL_GetTicksNS();
    for (uint32_t i = 0; i < BR_NET_MAX_SOCKETS; i++) {
        if (n->sockets[i].used) {
            br_net_socket_update(&n->sockets[i], now);
        }
    }
}

static void br_net_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_net *n = user;
    for (uint32_t i = 0; i < BR_NET_MAX_SOCKETS; i++) {
        if (n->sockets[i].used) {
            br_net_socket_free(&n->sockets[i]);
        }
    }
    for (uint32_t i = 0; i < BR_NET_MAX_SERVERS; i++) {
        NET_DestroyServer(n->servers[i].server);
    }
    for (uint32_t i = 0; i < BR_NET_MAX_UDP; i++) {
        NET_DestroyDatagramSocket(n->udp[i].socket);
    }
    for (uint32_t i = 0; i < BR_NET_MAX_ADDRESSES; i++) {
        NET_UnrefAddress(n->addresses[i].address);
    }
    br_free(n);
    NET_Quit();
}

bool br_net_register(br_engine *e)
{
    if (e == NULL) {
        br_log_error("br_net_register: no engine");
        return false;
    }
    if (br_net_get(e) != NULL) {
        return true;
    }
    if (!NET_Init()) {
        br_log_error("br_net_register: SDL3_net failed to start: %s", SDL_GetError());
        return false;
    }
    br_net *n = br_alloc(sizeof(br_net));
    if (n == NULL) {
        NET_Quit();
        return false;
    }
    SDL_zerop(n);
    if (!br_addon_add(e, "net", n)) {
        br_free(n);
        NET_Quit();
        return false;
    }
    br_system_add(e, "net.update", BR_PHASE_INPUT, BR_NET_UPDATE_PRIORITY, br_net_update, n);
    br_engine_on_shutdown(e, br_net_shutdown, n);
    return true;
}

/* ---- TCP */

br_net_socket br_net_connect(br_engine *e, const char *host, uint16_t port)
{
    br_net_socket none = { 0, 0 };
    br_net *n = br_net_get_logged(e, "br_net_connect");
    if (n == NULL) {
        return none;
    }
    if (host == NULL || host[0] == '\0' || port == 0) {
        br_log_error("br_net_connect: needs a host and a port");
        return none;
    }
    br_net_socket handle;
    br_net_socket_slot *slot = br_net_socket_take(n, "br_net_connect", &handle);
    if (slot == NULL) {
        return none;
    }
    SDL_strlcpy(slot->host, host, sizeof(slot->host));
    slot->port = port;
    slot->state = BR_NET_CONNECTING;
    slot->address = NET_ResolveHostname(host);
    if (slot->address == NULL) {
        br_log_error("br_net_connect: cannot resolve '%s': %s", host, SDL_GetError());
        slot->state = BR_NET_FAILED;
    }
    return handle;
}

br_net_state br_net_socket_state(br_engine *e, br_net_socket s)
{
    br_net_socket_slot *slot = br_net_socket_slot_get(br_net_get(e), s);
    return slot != NULL ? slot->state : BR_NET_CLOSED;
}

bool br_net_send(br_engine *e, br_net_socket s, const void *data, size_t size)
{
    br_net_socket_slot *slot = br_net_socket_slot_get(br_net_get(e), s);
    if (slot == NULL || slot->state != BR_NET_OPEN) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (data == NULL || size > (size_t)SDL_MAX_SINT32) {
        br_log_error("br_net_send: no data, or more than INT_MAX bytes in one call");
        return false;
    }
    if (!NET_WriteToStreamSocket(slot->stream, data, (int)size)) {
        br_log_error("net: sending to %s:%u failed: %s", slot->host, (unsigned)slot->port, SDL_GetError());
        slot->state = BR_NET_FAILED;
        return false;
    }
    return true;
}

size_t br_net_send_pending(br_engine *e, br_net_socket s)
{
    br_net_socket_slot *slot = br_net_socket_slot_get(br_net_get(e), s);
    if (slot == NULL || slot->stream == NULL) {
        return 0;
    }
    int pending = NET_GetStreamSocketPendingWrites(slot->stream);
    return pending > 0 ? (size_t)pending : 0;
}

int br_net_receive(br_engine *e, br_net_socket s, void *buffer, int capacity)
{
    br_net_socket_slot *slot = br_net_socket_slot_get(br_net_get(e), s);
    if (slot == NULL || slot->state == BR_NET_CLOSED || slot->state == BR_NET_FAILED) {
        return -1;
    }
    if (slot->state == BR_NET_CONNECTING || buffer == NULL || capacity <= 0) {
        return 0;
    }
    int got = NET_ReadFromStreamSocket(slot->stream, buffer, capacity);
    if (got < 0) {
        slot->state = BR_NET_CLOSED;  /* end of stream, or reset: the peer went away */
        return -1;
    }
    return got;
}

void br_net_close(br_engine *e, br_net_socket s)
{
    br_net_socket_slot *slot = br_net_socket_slot_get(br_net_get(e), s);
    if (slot == NULL) {
        return;
    }
    if (slot->state == BR_NET_OPEN && NET_GetStreamSocketPendingWrites(slot->stream) > 0) {
        slot->closing = true;
        slot->linger_until = SDL_GetTicksNS() + (uint64_t)(BR_NET_CLOSE_LINGER * 1e9f);
        return;
    }
    br_net_socket_free(slot);
}

br_net_server br_net_listen(br_engine *e, uint16_t port)
{
    br_net_server none = { 0, 0 };
    br_net *n = br_net_get_logged(e, "br_net_listen");
    if (n == NULL) {
        return none;
    }
    for (uint32_t i = 0; i < BR_NET_MAX_SERVERS; i++) {
        br_net_server_slot *slot = &n->servers[i];
        if (slot->used) {
            continue;
        }
        NET_Server *server = NET_CreateServer(NULL, port, 0);
        if (server == NULL) {
            br_log_error("br_net_listen: cannot listen on port %u: %s", (unsigned)port, SDL_GetError());
            return none;
        }
        slot->server = server;
        slot->generation = br_net_next_generation(slot->generation);
        slot->used = true;
        br_net_server handle = { i, slot->generation };
        return handle;
    }
    br_log_error("br_net_listen: all %d servers are in use (BR_NET_MAX_SERVERS)", BR_NET_MAX_SERVERS);
    return none;
}

br_net_socket br_net_accept(br_engine *e, br_net_server server)
{
    br_net_socket none = { 0, 0 };
    br_net *n = br_net_get(e);
    br_net_server_slot *slot = br_net_server_slot_get(n, server);
    if (slot == NULL) {
        return none;
    }
    NET_StreamSocket *client = NULL;
    if (!NET_AcceptClient(slot->server, &client)) {
        br_log_error("br_net_accept: %s", SDL_GetError());
        return none;
    }
    if (client == NULL) {
        return none;
    }
    br_net_socket handle;
    br_net_socket_slot *s = br_net_socket_take(n, "br_net_accept", &handle);
    if (s == NULL) {
        NET_DestroyStreamSocket(client);  /* dropped: the peer sees the connection close */
        return none;
    }
    s->stream = client;
    s->state = BR_NET_OPEN;
    NET_Address *peer = NET_GetStreamSocketAddress(client);
    const char *text = NET_GetAddressString(peer);
    SDL_strlcpy(s->host, text != NULL ? text : "?", sizeof(s->host));
    NET_UnrefAddress(peer);
    return handle;
}

void br_net_server_close(br_engine *e, br_net_server server)
{
    br_net_server_slot *slot = br_net_server_slot_get(br_net_get(e), server);
    if (slot != NULL) {
        NET_DestroyServer(slot->server);
        slot->server = NULL;
        slot->used = false;
    }
}

/* ---- addresses */

/* A free address slot holding `address` (a reference the slot now owns), or a zero handle. */
static br_net_address br_net_address_take(br_net *n, NET_Address *address, bool from_receive)
{
    br_net_address handle = { 0, 0 };
    for (uint32_t i = 0; i < BR_NET_MAX_ADDRESSES; i++) {
        br_net_address_slot *slot = &n->addresses[i];
        if (!slot->used) {
            slot->address = address;
            slot->generation = br_net_next_generation(slot->generation);
            slot->used = true;
            slot->from_receive = from_receive;
            handle.index = i;
            handle.generation = slot->generation;
            return handle;
        }
    }
    return handle;
}

br_net_address br_net_resolve(br_engine *e, const char *host)
{
    br_net_address none = { 0, 0 };
    br_net *n = br_net_get_logged(e, "br_net_resolve");
    if (n == NULL) {
        return none;
    }
    if (host == NULL || host[0] == '\0') {
        br_log_error("br_net_resolve: needs a host");
        return none;
    }
    NET_Address *address = NET_ResolveHostname(host);
    if (address == NULL) {
        br_log_error("br_net_resolve: cannot resolve '%s': %s", host, SDL_GetError());
        return none;
    }
    br_net_address handle = br_net_address_take(n, address, false);
    if (handle.generation == 0) {
        br_log_error("br_net_resolve: all %d addresses are in use (BR_NET_MAX_ADDRESSES)", BR_NET_MAX_ADDRESSES);
        NET_UnrefAddress(address);
    }
    return handle;
}

br_net_state br_net_address_state(br_engine *e, br_net_address a)
{
    br_net_address_slot *slot = br_net_address_slot_get(br_net_get(e), a);
    if (slot == NULL) {
        return BR_NET_CLOSED;
    }
    NET_Status status = NET_GetAddressStatus(slot->address);
    return status == NET_SUCCESS ? BR_NET_OPEN : status == NET_WAITING ? BR_NET_CONNECTING : BR_NET_FAILED;
}

bool br_net_address_get_string(br_engine *e, br_net_address a, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return false;
    }
    out[0] = '\0';
    br_net_address_slot *slot = br_net_address_slot_get(br_net_get(e), a);
    if (slot == NULL || NET_GetAddressStatus(slot->address) != NET_SUCCESS) {
        return false;
    }
    const char *text = NET_GetAddressString(slot->address);
    if (text == NULL) {
        return false;
    }
    SDL_strlcpy(out, text, capacity);
    return true;
}

void br_net_address_release(br_engine *e, br_net_address a)
{
    br_net *n = br_net_get(e);
    br_net_address_slot *slot = br_net_address_slot_get(n, a);
    if (slot != NULL) {
        br_net_address_free(n, slot);
    }
}

/* ---- UDP */

br_net_udp br_net_udp_open(br_engine *e, uint16_t port)
{
    br_net_udp none = { 0, 0 };
    br_net *n = br_net_get_logged(e, "br_net_udp_open");
    if (n == NULL) {
        return none;
    }
    for (uint32_t i = 0; i < BR_NET_MAX_UDP; i++) {
        br_net_udp_slot *slot = &n->udp[i];
        if (slot->used) {
            continue;
        }
        NET_DatagramSocket *socket = NET_CreateDatagramSocket(NULL, port, 0);
        if (socket == NULL) {
            br_log_error("br_net_udp_open: cannot bind port %u: %s", (unsigned)port, SDL_GetError());
            return none;
        }
        slot->socket = socket;
        slot->generation = br_net_next_generation(slot->generation);
        slot->used = true;
        br_net_udp handle = { i, slot->generation };
        return handle;
    }
    br_log_error("br_net_udp_open: all %d UDP sockets are in use (BR_NET_MAX_UDP)", BR_NET_MAX_UDP);
    return none;
}

bool br_net_udp_send(br_engine *e, br_net_udp u, br_net_address to, uint16_t port, const void *data, size_t size)
{
    br_net *n = br_net_get(e);
    br_net_udp_slot *slot = br_net_udp_slot_get(n, u);
    br_net_address_slot *address = br_net_address_slot_get(n, to);
    if (slot == NULL || address == NULL || NET_GetAddressStatus(address->address) != NET_SUCCESS) {
        return false;
    }
    if (data == NULL || size == 0 || size > BR_NET_MAX_DATAGRAM || port == 0) {
        br_log_error("br_net_udp_send: needs 1..%d bytes and a port", BR_NET_MAX_DATAGRAM);
        return false;
    }
    if (!NET_SendDatagram(slot->socket, address->address, port, data, (int)size)) {
        br_log_error("br_net_udp_send: %s", SDL_GetError());
        return false;
    }
    return true;
}

/* The address handle for a datagram's sender: the slot made earlier for the same host, else a
 * new one (zero + one log line when the table is full). */
static br_net_address br_net_sender(br_net *n, NET_Address *sender)
{
    for (uint32_t i = 0; i < BR_NET_MAX_ADDRESSES; i++) {
        br_net_address_slot *slot = &n->addresses[i];
        if (slot->used && slot->from_receive && NET_CompareAddresses(slot->address, sender) == 0) {
            br_net_address handle = { i, slot->generation };
            return handle;
        }
    }
    br_net_address handle = br_net_address_take(n, NET_RefAddress(sender), true);
    if (handle.generation == 0) {
        NET_UnrefAddress(sender);
        if (!n->address_full_logged) {
            br_log_warn("br_net_udp_receive: all %d addresses are in use; senders read as zero handles",
                        BR_NET_MAX_ADDRESSES);
            n->address_full_logged = true;
        }
    }
    return handle;
}

int br_net_udp_receive(br_engine *e, br_net_udp u, void *buffer, int capacity, br_net_address *from, uint16_t *from_port)
{
    br_net_address none = { 0, 0 };
    if (from != NULL) {
        *from = none;
    }
    if (from_port != NULL) {
        *from_port = 0;
    }
    br_net *n = br_net_get(e);
    br_net_udp_slot *slot = br_net_udp_slot_get(n, u);
    if (slot == NULL) {
        return -1;
    }
    if (buffer == NULL || capacity <= 0) {
        return 0;
    }
    /* ponytail: SDL3_net allocates one block per received datagram (SDL_malloc, not br_alloc);
     * recvfrom into a buffer of ours would need platform socket headers. */
    for (int tries = 0; tries < 64; tries++) {  /* bounded: a flood of oversized datagrams cannot stall a frame */
        NET_Datagram *datagram = NULL;
        if (!NET_ReceiveDatagram(slot->socket, &datagram)) {
            /* e.g. Windows reports an ICMP "port unreachable" for an earlier send here */
            br_log_warn("br_net_udp_receive: %s", SDL_GetError());
            return 0;
        }
        if (datagram == NULL) {
            return 0;
        }
        if (datagram->buflen > capacity) {
            br_log_warn("br_net_udp_receive: dropped a %d-byte datagram (buffer holds %d)", datagram->buflen, capacity);
            NET_DestroyDatagram(datagram);
            continue;
        }
        int size = datagram->buflen;
        SDL_memcpy(buffer, datagram->buf, (size_t)size);
        if (from != NULL) {
            *from = br_net_sender(n, datagram->addr);
        }
        if (from_port != NULL) {
            *from_port = datagram->port;
        }
        NET_DestroyDatagram(datagram);
        return size;
    }
    return 0;
}

void br_net_udp_close(br_engine *e, br_net_udp u)
{
    br_net_udp_slot *slot = br_net_udp_slot_get(br_net_get(e), u);
    if (slot != NULL) {
        NET_DestroyDatagramSocket(slot->socket);
        slot->socket = NULL;
        slot->used = false;
    }
}
