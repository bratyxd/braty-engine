/* br_arena.h - arenas: linear, stack, pool, scratch scopes and the per-frame arena.
 *
 *   br_arena_linear a;
 *   br_arena_linear_create(&a, "level", 64 * 1024, true);   // growable
 *   float *f = br_arena_linear_alloc(&a, 100 * sizeof(float));
 *   br_arena_linear_reset(&a);                               // everything at once
 *   br_arena_linear_destroy(&a);
 *
 * Every arena keeps a br_arena_stats (name, used, peak, capacity) as its `stats` member and
 * offers a br_allocator view so br_array/br_pool/br_map can live inside it. `*_create_in`
 * arenas use caller memory only (never the heap) and have a fixed capacity. Memory handed out
 * is zeroed. Overflow of a fixed arena returns NULL and logs. Arenas are single-threaded.
 */
#ifndef BR_ARENA_H
#define BR_ARENA_H

#include "braty/br_core.h"

#define BR_ARENA_NAME_SIZE 32
/* Alignment of br_*_alloc without an explicit one (enough for any C type and SIMD vectors). */
#define BR_ARENA_DEFAULT_ALIGNMENT 16
/* Size of each engine's frame arena (it grows by chained blocks when a frame needs more). */
#define BR_FRAME_ARENA_SIZE (1024 * 1024)

/* Numbers every arena reports; registered arenas are listed by the debug overlay. */
typedef struct br_arena_stats {
    char   name[BR_ARENA_NAME_SIZE];
    size_t used;      /* bytes handed out (including alignment padding) */
    size_t peak;      /* highest `used` since creation */
    size_t capacity;  /* bytes reserved */
} br_arena_stats;

/* One heap block of a linear arena; blocks chain newest-first. */
typedef struct br_arena_block {
    struct br_arena_block *prev;
    size_t                 size;  /* bytes of data after this header */
} br_arena_block;

/* ---- linear: bump allocation, reset all at once */

typedef struct br_arena_linear {
    br_arena_stats  stats;
    uint8_t        *base;       /* memory of the current block */
    size_t          size;       /* bytes in the current block */
    size_t          offset;     /* bytes used in the current block */
    br_arena_block *blocks;     /* heap blocks, newest first; NULL for caller memory */
    bool            growable;   /* chains a new block instead of failing */
    br_allocator    allocator;  /* view: alloc bumps, free does nothing */
} br_arena_linear;

/* Creates a heap-backed linear arena of `capacity` bytes; growable ones chain new blocks. */
bool br_arena_linear_create(br_arena_linear *a, const char *name, size_t capacity, bool growable);
/* Creates a fixed linear arena over caller memory (never touches the heap). */
bool br_arena_linear_create_in(br_arena_linear *a, const char *name, void *buffer, size_t size);
/* Frees the arena's heap blocks (caller memory is left alone). */
void br_arena_linear_destroy(br_arena_linear *a);
/* Returns `size` zeroed bytes aligned to BR_ARENA_DEFAULT_ALIGNMENT, NULL (and logs) when full. */
void *br_arena_linear_alloc(br_arena_linear *a, size_t size);
/* Returns `size` zeroed bytes aligned to `alignment` (a power of two), NULL (and logs) on failure. */
void *br_arena_linear_alloc_aligned(br_arena_linear *a, size_t size, size_t alignment);
/* Forgets every allocation. A grown arena merges its blocks into one of the total size. */
void br_arena_linear_reset(br_arena_linear *a);
/* Returns a br_allocator that allocates from the arena (free is a no-op). */
const br_allocator *br_arena_linear_allocator(br_arena_linear *a);

/* ---- stack: linear with markers, popped in LIFO order */

typedef struct br_arena_marker {
    size_t   offset;
    uint32_t depth;        /* 1 for the first marker pushed */
    uint32_t serial;       /* unique per push: tells a stale marker from a live one at its depth */
    uint32_t prev_serial;  /* serial of the marker that was newest when this one was pushed */
} br_arena_marker;

typedef struct br_arena_stack {
    br_arena_linear linear;       /* fixed capacity, never grows */
    uint32_t        depth;        /* markers currently pushed */
    uint32_t        order_errors; /* out-of-order pops caught (debug builds) */
    uint32_t        top_serial;   /* serial of the newest live marker, 0 = none */
    uint32_t        next_serial;  /* last serial handed out */
} br_arena_stack;

/* Creates a heap-backed stack arena of `capacity` bytes (fixed). */
bool br_arena_stack_create(br_arena_stack *s, const char *name, size_t capacity);
/* Creates a stack arena over caller memory (never touches the heap). */
bool br_arena_stack_create_in(br_arena_stack *s, const char *name, void *buffer, size_t size);
/* Frees the stack's heap memory. */
void br_arena_stack_destroy(br_arena_stack *s);
/* Returns `size` zeroed bytes aligned to `alignment` (power of two), NULL (and logs) when full. */
void *br_arena_stack_alloc(br_arena_stack *s, size_t size, size_t alignment);
/* Remembers the current top; pop_to_marker frees everything allocated after it. */
br_arena_marker br_arena_stack_push_marker(br_arena_stack *s);
/* Frees back to `m`. A stale marker (already popped) at the newest depth is rejected; debug
 * builds reject any marker that is not the newest (logs, returns false). Release builds pop an
 * older live marker together with the newer ones. */
bool br_arena_stack_pop_to_marker(br_arena_stack *s, br_arena_marker m);
/* Returns a br_allocator over the stack (free is a no-op). */
const br_allocator *br_arena_stack_allocator(br_arena_stack *s);

/* ---- pool: fixed-size blocks with a free list */

typedef struct br_arena_pool {
    br_arena_stats stats;
    uint8_t       *memory;       /* first block (aligned) */
    void          *heap;         /* br_alloc block when heap-backed, else NULL */
    void          *free_list;    /* next free block; each free block stores the next pointer */
    uint8_t       *free_bits;    /* one bit per block, 1 = free: catches double frees */
    size_t         block_size;   /* rounded up to the alignment */
    uint32_t       block_count;
    br_allocator   allocator;    /* view: alloc takes a block (size <= block_size), free returns it */
} br_arena_pool;

/* Creates a heap-backed pool of `block_count` blocks of `block_size` bytes aligned to `alignment`. */
bool br_arena_pool_create(br_arena_pool *p, const char *name, size_t block_size, size_t alignment,
                          uint32_t block_count);
/* Creates a pool over caller memory: as many blocks as fit in `size` bytes (with one free bit
 * per block kept at the end of the buffer). */
bool br_arena_pool_create_in(br_arena_pool *p, const char *name, size_t block_size, size_t alignment,
                             void *buffer, size_t size);
/* Frees the pool's heap memory. */
void br_arena_pool_destroy(br_arena_pool *p);
/* Takes one zeroed block, NULL (and logs) when every block is in use. */
void *br_arena_pool_alloc(br_arena_pool *p);
/* Returns a block to the pool. Pointers that are not a block of this pool, and blocks that are
 * already free (double free), are logged and ignored. */
void br_arena_pool_free(br_arena_pool *p, void *block);
/* Returns a br_allocator over the pool (requests larger than block_size fail). */
const br_allocator *br_arena_pool_allocator(br_arena_pool *p);

/* ---- scratch: a temporary scope over a linear arena
 *
 *   br_arena_scratch scratch = br_arena_scratch_begin(&a);
 *   ... br_arena_linear_alloc(&a, ...) ...
 *   br_arena_scratch_end(scratch);   // everything since begin is gone
 */
typedef struct br_arena_scratch {
    br_arena_linear *arena;
    br_arena_block  *block;
    size_t           offset;
    size_t           used;
} br_arena_scratch;

/* Saves the arena's current top. */
br_arena_scratch br_arena_scratch_begin(br_arena_linear *a);
/* Frees everything allocated since the matching begin (and blocks chained since then). */
void br_arena_scratch_end(br_arena_scratch scratch);

/* ---- per-engine: the frame arena and the list of named arenas */

/* Returns `size` zeroed bytes valid until the start of the next frame (16-byte aligned). */
void *br_frame_alloc(br_engine *e, size_t size);
/* Returns the engine's frame arena (for its allocator or a scratch scope). */
br_arena_linear *br_frame_arena(br_engine *e);
/* Lists an arena's stats in the debug overlay. Untrack it before destroying the arena. */
bool br_arena_track(br_engine *e, const br_arena_stats *stats);
/* Removes an arena from the list. */
void br_arena_untrack(br_engine *e, const br_arena_stats *stats);
/* Number of tracked arenas (the frame arena is always the first). */
uint32_t br_arena_count(br_engine *e);
/* Returns tracked arena `index`, or NULL if out of range. */
const br_arena_stats *br_arena_at(br_engine *e, uint32_t index);
/* Creates the frame arena and its reset system. Called by the engine; games never call it. */
void br_arena_register(br_engine *e);

#endif
