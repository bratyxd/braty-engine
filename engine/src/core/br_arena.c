/* Arenas: linear (bump, optionally chained blocks), stack (markers), pool (free list),
 * scratch scopes, and the engine's frame arena + the list of tracked arenas. */
#include "app/br_engine_internal.h"
#include "braty/br_arena.h"

#include <limits.h>

typedef struct br_arena_state {
    br_arena_linear frame;
    br_array        tracked;  /* const br_arena_stats * */
} br_arena_state;

static bool br_arena_check_alignment(const char *name, size_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        br_log_error("arena '%s': alignment %zu is not a power of two", name, alignment);
        return false;
    }
    return true;
}

/* Bytes to skip so that base + offset becomes a multiple of `alignment`. */
static size_t br_arena_padding(const uint8_t *base, size_t offset, size_t alignment)
{
    uintptr_t p = (uintptr_t)(base + offset);
    return (size_t)((alignment - (p & (alignment - 1))) & (alignment - 1));
}

static void br_arena_stats_init(br_arena_stats *stats, const char *name)
{
    SDL_zerop(stats);
    br_str_copy(stats->name, sizeof(stats->name), name != NULL ? name : "arena");
}

static void br_arena_stats_add(br_arena_stats *stats, size_t bytes)
{
    stats->used += bytes;
    if (stats->used > stats->peak) {
        stats->peak = stats->used;
    }
}

/* ---- linear */

static void *br_arena_linear_view_alloc(void *user, size_t size)
{
    return br_arena_linear_alloc(user, size);
}

static void *br_arena_linear_view_realloc(void *user, void *ptr, size_t old_size, size_t new_size)
{
    br_arena_linear *a = user;
    if (ptr == NULL) {
        return br_arena_linear_alloc(a, new_size);
    }
    if (new_size <= old_size) {
        return ptr;
    }
    /* the newest allocation can grow in place when the block has room */
    uint8_t *end = (uint8_t *)ptr + old_size;
    size_t extra = new_size - old_size;
    if (end == a->base + a->offset && extra <= a->size - a->offset) {
        a->offset += extra;
        br_arena_stats_add(&a->stats, extra);
        return ptr;
    }
    void *p = br_arena_linear_alloc(a, new_size);
    if (p != NULL) {
        SDL_memcpy(p, ptr, old_size);
    }
    return p;
}

static void br_arena_linear_view_free(void *user, void *ptr)
{
    (void)user;
    (void)ptr; /* bump arenas free everything at reset */
}

static void br_arena_linear_init(br_arena_linear *a, const char *name)
{
    SDL_zerop(a);
    br_arena_stats_init(&a->stats, name);
    a->allocator.alloc = br_arena_linear_view_alloc;
    a->allocator.realloc = br_arena_linear_view_realloc;
    a->allocator.free = br_arena_linear_view_free;
    a->allocator.user = a;
}

/* Chains a new heap block of at least `size` bytes and makes it current. */
static bool br_arena_linear_add_block(br_arena_linear *a, size_t size)
{
    if (size > SIZE_MAX - sizeof(br_arena_block)) {
        br_log_error("arena '%s': block of %zu bytes is too large", a->stats.name, size);
        return false;
    }
    br_arena_block *b = br_alloc(sizeof(br_arena_block) + size);
    if (b == NULL) {
        return false;
    }
    b->prev = a->blocks;
    b->size = size;
    a->blocks = b;
    a->base = (uint8_t *)(b + 1);
    a->size = size;
    a->offset = 0;
    a->stats.capacity += size;
    return true;
}

bool br_arena_linear_create(br_arena_linear *a, const char *name, size_t capacity, bool growable)
{
    br_arena_linear_init(a, name);
    a->growable = growable;
    if (capacity == 0) {
        br_log_error("arena '%s': capacity must be > 0", a->stats.name);
        return false;
    }
    return br_arena_linear_add_block(a, capacity);
}

bool br_arena_linear_create_in(br_arena_linear *a, const char *name, void *buffer, size_t size)
{
    br_arena_linear_init(a, name);
    if (buffer == NULL || size == 0) {
        br_log_error("arena '%s': no caller memory given", a->stats.name);
        return false;
    }
    a->base = buffer;
    a->size = size;
    a->stats.capacity = size;
    return true;
}

static void br_arena_linear_free_blocks(br_arena_linear *a, br_arena_block *keep)
{
    while (a->blocks != keep) {
        br_arena_block *prev = a->blocks->prev;
        a->stats.capacity -= a->blocks->size;
        br_free(a->blocks);
        a->blocks = prev;
    }
}

void br_arena_linear_destroy(br_arena_linear *a)
{
    char name[BR_ARENA_NAME_SIZE];
    br_str_copy(name, sizeof(name), a->stats.name);
    br_arena_linear_free_blocks(a, NULL);
    br_arena_linear_init(a, name);
}

void *br_arena_linear_alloc(br_arena_linear *a, size_t size)
{
    return br_arena_linear_alloc_aligned(a, size, BR_ARENA_DEFAULT_ALIGNMENT);
}

void *br_arena_linear_alloc_aligned(br_arena_linear *a, size_t size, size_t alignment)
{
    if (!br_arena_check_alignment(a->stats.name, alignment)) {
        return NULL;
    }
    if (a->base == NULL) {
        br_log_error("arena '%s': not created", a->stats.name);
        return NULL;
    }
    size_t pad = br_arena_padding(a->base, a->offset, alignment);
    size_t room = a->size - a->offset;
    if (size > room || pad > room - size) {
        if (!a->growable) {
            br_log_error("arena '%s' is full: %zu bytes requested, %zu of %zu used", a->stats.name, size,
                         a->stats.used, a->stats.capacity);
            return NULL;
        }
        size_t need = size <= SIZE_MAX - alignment ? size + alignment : SIZE_MAX;
        if (!br_arena_linear_add_block(a, need > a->size ? need : a->size)) {
            return NULL;
        }
        pad = br_arena_padding(a->base, a->offset, alignment);
    }
    uint8_t *p = a->base + a->offset + pad;
    a->offset += pad + size;
    br_arena_stats_add(&a->stats, pad + size);
    SDL_memset(p, 0, size);
    return p;
}

void br_arena_linear_reset(br_arena_linear *a)
{
    if (a->blocks != NULL && a->blocks->prev != NULL) {
        /* grown: replace the chain by one block of the total size so it stops chaining */
        br_arena_block *oldest = a->blocks;
        while (oldest->prev != NULL) {
            oldest = oldest->prev;
        }
        size_t total = a->stats.capacity;
        br_arena_block *chain = a->blocks;
        a->blocks = NULL;
        a->stats.capacity = 0;
        if (br_arena_linear_add_block(a, total)) {
            br_arena_block *merged = a->blocks;
            a->blocks = chain;
            br_arena_linear_free_blocks(a, NULL);
            a->blocks = merged;
            a->stats.capacity = total;
        } else {
            a->blocks = chain;
            a->stats.capacity = total;
            br_arena_linear_free_blocks(a, oldest);
            a->base = (uint8_t *)(oldest + 1);
            a->size = oldest->size;
        }
    }
    a->offset = 0;
    a->stats.used = 0;
}

const br_allocator *br_arena_linear_allocator(br_arena_linear *a)
{
    return &a->allocator;
}

/* ---- scratch */

br_arena_scratch br_arena_scratch_begin(br_arena_linear *a)
{
    br_arena_scratch scratch;
    scratch.arena = a;
    scratch.block = a->blocks;
    scratch.offset = a->offset;
    scratch.used = a->stats.used;
    return scratch;
}

void br_arena_scratch_end(br_arena_scratch scratch)
{
    br_arena_linear *a = scratch.arena;
    if (a->blocks != scratch.block) {
        br_arena_linear_free_blocks(a, scratch.block);
        a->base = (uint8_t *)(scratch.block + 1);
        a->size = scratch.block->size;
    }
    a->offset = scratch.offset;
    a->stats.used = scratch.used;
}

/* ---- stack */

bool br_arena_stack_create(br_arena_stack *s, const char *name, size_t capacity)
{
    s->depth = 0;
    s->order_errors = 0;
    s->top_serial = 0;
    s->next_serial = 0;
    return br_arena_linear_create(&s->linear, name, capacity, false);
}

bool br_arena_stack_create_in(br_arena_stack *s, const char *name, void *buffer, size_t size)
{
    s->depth = 0;
    s->order_errors = 0;
    s->top_serial = 0;
    s->next_serial = 0;
    return br_arena_linear_create_in(&s->linear, name, buffer, size);
}

void br_arena_stack_destroy(br_arena_stack *s)
{
    br_arena_linear_destroy(&s->linear);
    s->depth = 0;
    s->top_serial = 0;
}

void *br_arena_stack_alloc(br_arena_stack *s, size_t size, size_t alignment)
{
    return br_arena_linear_alloc_aligned(&s->linear, size, alignment);
}

br_arena_marker br_arena_stack_push_marker(br_arena_stack *s)
{
    br_arena_marker m;
    s->depth += 1;
    m.offset = s->linear.offset;
    m.depth = s->depth;
    s->next_serial += 1;
    if (s->next_serial == 0) {
        s->next_serial = 1; /* 0 means "no marker" */
    }
    m.serial = s->next_serial;
    m.prev_serial = s->top_serial;
    s->top_serial = m.serial;
    return m;
}

bool br_arena_stack_pop_to_marker(br_arena_stack *s, br_arena_marker m)
{
    bool stale = m.depth == s->depth && m.serial != s->top_serial; /* popped, then another pushed */
    if (m.depth == 0 || m.depth > s->depth || m.offset > s->linear.offset || stale) {
        br_log_error("stack arena '%s': marker %u is not live (depth %u)", s->linear.stats.name, m.depth, s->depth);
        return false;
    }
#ifndef NDEBUG
    if (m.depth != s->depth) {
        s->order_errors += 1;
        br_log_error("stack arena '%s': marker %u popped out of order (newest is %u)", s->linear.stats.name, m.depth,
                     s->depth);
        return false;
    }
#endif
    /* release builds pop the newer markers along with it */
    s->linear.offset = m.offset;
    s->linear.stats.used = m.offset;
    s->depth = m.depth - 1;
    s->top_serial = m.prev_serial;
    return true;
}

const br_allocator *br_arena_stack_allocator(br_arena_stack *s)
{
    return &s->linear.allocator;
}

/* ---- pool */

static void *br_arena_pool_view_alloc(void *user, size_t size)
{
    br_arena_pool *p = user;
    if (size > p->block_size) {
        br_log_error("pool arena '%s': %zu bytes requested, blocks are %zu", p->stats.name, size, p->block_size);
        return NULL;
    }
    return br_arena_pool_alloc(p);
}

static void *br_arena_pool_view_realloc(void *user, void *ptr, size_t old_size, size_t new_size)
{
    (void)old_size;
    if (ptr == NULL) {
        return br_arena_pool_view_alloc(user, new_size);
    }
    br_arena_pool *p = user;
    if (new_size > p->block_size) {
        br_log_error("pool arena '%s': cannot grow a block to %zu bytes", p->stats.name, new_size);
        return NULL;
    }
    return ptr;
}

static void br_arena_pool_view_free(void *user, void *ptr)
{
    br_arena_pool_free(user, ptr);
}

/* Bytes of the free-bit array for `count` blocks. */
static size_t br_arena_pool_bits_size(size_t count)
{
    return (count + 7) / 8;
}

/* Shared part of create/create_in: `memory` must hold `count` blocks of p->block_size followed
 * by br_arena_pool_bits_size(count) bytes for the free bits. */
static void br_arena_pool_build(br_arena_pool *p, uint8_t *memory, uint32_t count)
{
    p->memory = memory;
    p->block_count = count;
    p->free_list = NULL;
    p->free_bits = memory + (size_t)count * p->block_size;
    SDL_memset(p->free_bits, 0xFF, br_arena_pool_bits_size(count));
    for (uint32_t i = count; i > 0; i--) {
        void **block = (void **)(memory + (size_t)(i - 1) * p->block_size);
        *block = p->free_list;
        p->free_list = block;
    }
    p->stats.capacity = (size_t)count * p->block_size;
}

static bool br_arena_pool_init(br_arena_pool *p, const char *name, size_t block_size, size_t *alignment)
{
    SDL_zerop(p);
    br_arena_stats_init(&p->stats, name);
    p->allocator.alloc = br_arena_pool_view_alloc;
    p->allocator.realloc = br_arena_pool_view_realloc;
    p->allocator.free = br_arena_pool_view_free;
    p->allocator.user = p;
    if (!br_arena_check_alignment(p->stats.name, *alignment)) {
        return false;
    }
    if (block_size == 0 || block_size > SIZE_MAX / 2) {
        br_log_error("pool arena '%s': bad block size %zu", p->stats.name, block_size);
        return false;
    }
    /* free blocks hold the next pointer, so blocks are at least pointer-sized and -aligned */
    if (*alignment < _Alignof(void *)) {
        *alignment = _Alignof(void *);
    }
    if (block_size < sizeof(void *)) {
        block_size = sizeof(void *);
    }
    p->block_size = (block_size + *alignment - 1) & ~(*alignment - 1);
    return true;
}

bool br_arena_pool_create(br_arena_pool *p, const char *name, size_t block_size, size_t alignment,
                          uint32_t block_count)
{
    if (!br_arena_pool_init(p, name, block_size, &alignment)) {
        return false;
    }
    size_t bits = br_arena_pool_bits_size(block_count);
    if (block_count == 0 || p->block_size > (SIZE_MAX - alignment - bits) / block_count) {
        br_log_error("pool arena '%s': bad block count %u", p->stats.name, block_count);
        return false;
    }
    p->heap = br_alloc(p->block_size * block_count + bits + alignment);
    if (p->heap == NULL) {
        return false;
    }
    uint8_t *memory = (uint8_t *)p->heap + br_arena_padding(p->heap, 0, alignment);
    br_arena_pool_build(p, memory, block_count);
    return true;
}

bool br_arena_pool_create_in(br_arena_pool *p, const char *name, size_t block_size, size_t alignment,
                             void *buffer, size_t size)
{
    if (!br_arena_pool_init(p, name, block_size, &alignment)) {
        return false;
    }
    size_t pad = buffer != NULL ? br_arena_padding(buffer, 0, alignment) : 0;
    size_t room = buffer != NULL && size > pad ? size - pad : 0;
    size_t count = room / p->block_size;
    while (count > 0 && count * p->block_size + br_arena_pool_bits_size(count) > room) {
        count--; /* the free bits live after the last block */
    }
    if (count == 0) {
        br_log_error("pool arena '%s': caller memory holds no block", p->stats.name);
        return false;
    }
    br_arena_pool_build(p, (uint8_t *)buffer + pad, count < UINT32_MAX ? (uint32_t)count : UINT32_MAX);
    return true;
}

void br_arena_pool_destroy(br_arena_pool *p)
{
    br_free(p->heap);
    p->heap = NULL;
    p->memory = NULL;
    p->free_list = NULL;
    p->free_bits = NULL;
    p->block_count = 0;
    p->stats.used = 0;
    p->stats.capacity = 0;
}

void *br_arena_pool_alloc(br_arena_pool *p)
{
    if (p->free_list == NULL) {
        br_log_error("pool arena '%s' is exhausted (%u blocks)", p->stats.name, p->block_count);
        return NULL;
    }
    void **block = p->free_list;
    p->free_list = *block;
    size_t index = (size_t)((uint8_t *)block - p->memory) / p->block_size;
    p->free_bits[index / 8] &= (uint8_t)~(1u << (index % 8));
    SDL_memset(block, 0, p->block_size);
    br_arena_stats_add(&p->stats, p->block_size);
    return block;
}

void br_arena_pool_free(br_arena_pool *p, void *block)
{
    if (block == NULL) {
        return;
    }
    uintptr_t b = (uintptr_t)block;
    uintptr_t first = (uintptr_t)p->memory;
    if (p->memory == NULL || b < first || b - first >= p->stats.capacity || (b - first) % p->block_size != 0) {
        br_log_error("pool arena '%s': %p is not one of its blocks", p->stats.name, block);
        return;
    }
    size_t index = (size_t)(b - first) / p->block_size;
    uint8_t bit = (uint8_t)(1u << (index % 8));
    if ((p->free_bits[index / 8] & bit) != 0) {
        br_log_error("pool arena '%s': double free of block %zu", p->stats.name, index);
        return;
    }
    p->free_bits[index / 8] |= bit;
    *(void **)block = p->free_list;
    p->free_list = block;
    p->stats.used -= p->block_size;
}

const br_allocator *br_arena_pool_allocator(br_arena_pool *p)
{
    return &p->allocator;
}

/* ---- per engine: frame arena + tracked arenas */

static void br_arena_frame_reset(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_arena_linear_reset(&e->arena->frame);
}

static void br_arena_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_arena_linear_destroy(&e->arena->frame);
    br_array_free(&e->arena->tracked);
    br_free(e->arena);
    e->arena = NULL;
}

void br_arena_register(br_engine *e)
{
    br_arena_state *s = br_alloc(sizeof(br_arena_state));
    if (s == NULL) {
        return;
    }
    br_array_init(&s->tracked, sizeof(const br_arena_stats *));
    if (!br_arena_linear_create(&s->frame, "frame", BR_FRAME_ARENA_SIZE, true)) {
        br_free(s);
        return;
    }
    e->arena = s;
    br_arena_track(e, &s->frame.stats);
    /* first system of the frame (POST_RENDER does not run headless) */
    br_system_add(e, "arena.frame_reset", BR_PHASE_INPUT, INT_MIN + 1, br_arena_frame_reset, NULL);
    br_engine_on_shutdown(e, br_arena_shutdown, NULL);
}

void *br_frame_alloc(br_engine *e, size_t size)
{
    if (e == NULL || e->arena == NULL) {
        br_log_error("br_frame_alloc: engine has no frame arena");
        return NULL;
    }
    return br_arena_linear_alloc(&e->arena->frame, size);
}

br_arena_linear *br_frame_arena(br_engine *e)
{
    return e != NULL && e->arena != NULL ? &e->arena->frame : NULL;
}

bool br_arena_track(br_engine *e, const br_arena_stats *stats)
{
    if (e == NULL || e->arena == NULL || stats == NULL) {
        br_log_error("br_arena_track: no engine or arena");
        return false;
    }
    for (uint32_t i = 0; i < e->arena->tracked.count; i++) {
        if (*(const br_arena_stats **)br_array_at(&e->arena->tracked, i) == stats) {
            return true;
        }
    }
    return br_array_push(&e->arena->tracked, &stats) != NULL;
}

void br_arena_untrack(br_engine *e, const br_arena_stats *stats)
{
    if (e == NULL || e->arena == NULL) {
        return;
    }
    for (uint32_t i = 0; i < e->arena->tracked.count; i++) {
        if (*(const br_arena_stats **)br_array_at(&e->arena->tracked, i) == stats) {
            br_array_remove_ordered(&e->arena->tracked, i);
            return;
        }
    }
}

uint32_t br_arena_count(br_engine *e)
{
    return e != NULL && e->arena != NULL ? e->arena->tracked.count : 0;
}

const br_arena_stats *br_arena_at(br_engine *e, uint32_t index)
{
    if (e == NULL || e->arena == NULL) {
        return NULL;
    }
    const br_arena_stats **slot = br_array_at(&e->arena->tracked, index);
    return slot != NULL ? *slot : NULL;
}
