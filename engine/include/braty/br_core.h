/* br_core.h - memory, logging, containers, strings, files, save data.
 *
 * Everything here is plain C over SDL3. Containers own their memory and are freed with
 * their *_free function. Nothing here needs a window or a GPU.
 */
#ifndef BR_CORE_H
#define BR_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The engine context. Defined privately in engine/src/app/br_engine_internal.h. */
typedef struct br_engine br_engine;

/* Number of elements in a fixed-size array. */
#define BR_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- memory (SDL allocator underneath, so a game can swap it with SDL_SetMemoryFunctions) */

/* Allocates `size` zeroed bytes. Returns NULL on failure (and logs). */
void *br_alloc(size_t size);
/* Resizes a block from br_alloc/br_realloc. New bytes are NOT zeroed. */
void *br_realloc(void *ptr, size_t size);
/* Frees a block from br_alloc/br_realloc/br_strdup/br_file_read_all. NULL is fine.
 * Never mix with SDL_free/free: br_alloc blocks carry a size header (for br_memory_stats). */
void br_free(void *ptr);

/* Process-wide heap counters of br_alloc/br_realloc/br_free (thread-safe). */
typedef struct br_memory_stats {
    uint64_t live_allocations;   /* blocks not yet freed */
    uint64_t live_bytes;         /* bytes requested by those blocks */
    uint64_t total_allocations;  /* br_alloc calls (and br_realloc(NULL, ...)) since start */
    uint64_t peak_bytes;         /* highest live_bytes seen */
} br_memory_stats;

/* Copies the current heap counters into *out. They cover every engine in the process. */
void br_memory_get_stats(br_memory_stats *out);

/* ---- br_allocator: where a container gets its memory
 *
 * A plain struct of function pointers. A NULL `const br_allocator *` means the heap
 * (br_alloc/br_realloc/br_free). Arenas hand out one (br_arena_linear_allocator, ...).
 * alloc must return zeroed memory; realloc gets the old size so bump arenas can copy.
 */
typedef struct br_allocator {
    void *(*alloc)(void *user, size_t size);
    void *(*realloc)(void *user, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *user, void *ptr);
    void *user;
} br_allocator;

/* Allocates `size` zeroed bytes from `a` (NULL = heap). */
void *br_allocator_alloc(const br_allocator *a, size_t size);
/* Resizes a block from `a` (NULL = heap); ptr may be NULL. New bytes are NOT zeroed. */
void *br_allocator_realloc(const br_allocator *a, void *ptr, size_t old_size, size_t new_size);
/* Returns a block to `a` (NULL = heap). NULL ptr is fine. */
void br_allocator_free(const br_allocator *a, void *ptr);

/* ---- logging (SDL_Log underneath; category "braty") */

/* Logs a debug message (printf-style). Hidden unless the log priority is lowered. */
void br_log_debug(const char *fmt, ...);
/* Logs an informational message (printf-style). */
void br_log_info(const char *fmt, ...);
/* Logs a warning (printf-style). */
void br_log_warn(const char *fmt, ...);
/* Logs an error (printf-style). Engine functions call this before returning failure. */
void br_log_error(const char *fmt, ...);

/* ---- br_array: growable array of fixed-size elements
 *
 *   br_array a; br_array_init(&a, sizeof(float));
 *   float v = 1.0f; br_array_push(&a, &v);
 *   float *f = br_array_at(&a, 0);
 *   br_array_free(&a);
 *
 * Pointers into the array are invalidated by anything that grows it.
 */
typedef struct br_array {
    void    *data;
    uint32_t count;      /* elements in use */
    uint32_t capacity;   /* elements allocated */
    uint32_t elem_size;  /* bytes per element */
    const br_allocator *allocator; /* NULL = heap; must outlive the array */
} br_array;

/* Initialises an empty heap array of elements of `elem_size` bytes. Allocates nothing. */
void br_array_init(br_array *a, uint32_t elem_size);
/* Like br_array_init, but storage comes from `allocator` (NULL = heap). */
void br_array_init_with_allocator(br_array *a, uint32_t elem_size, const br_allocator *allocator);
/* Frees the storage and resets the array to empty (elem_size and allocator are kept). */
void br_array_free(br_array *a);
/* Makes room for at least `capacity` elements. */
bool br_array_reserve(br_array *a, uint32_t capacity);
/* Sets count, growing if needed; new elements are zeroed. */
bool br_array_resize(br_array *a, uint32_t count);
/* Appends one element (copied from `elem`, or zeroed when elem is NULL). Returns it, NULL on failure. */
void *br_array_push(br_array *a, const void *elem);
/* Appends `n` elements (copied from `elems`, or zeroed when NULL). Returns the first, NULL on failure. */
void *br_array_push_n(br_array *a, const void *elems, uint32_t n);
/* Inserts one element at `index` (shifting later ones up). Returns it, NULL on failure. */
void *br_array_insert(br_array *a, uint32_t index, const void *elem);
/* Returns a pointer to element `index`, or NULL if out of range. */
void *br_array_at(const br_array *a, uint32_t index);
/* Removes the last element. */
void br_array_pop(br_array *a);
/* Removes element `index` by moving the last element into its place (order not kept). */
void br_array_remove_swap(br_array *a, uint32_t index);
/* Removes element `index` and shifts later elements down (order kept). */
void br_array_remove_ordered(br_array *a, uint32_t index);
/* Sets count to 0, keeps the storage. */
void br_array_clear(br_array *a);

/* ---- br_pool: slots addressed by (index, generation) handles
 *
 * Freeing a slot bumps its generation, so old handles stop resolving instead of pointing at
 * a reused slot. Generation 0 is never handed out, so a zeroed handle is always invalid.
 * Slot storage never moves while the pool does not grow; growth may move it, so do not keep
 * pointers across br_pool_alloc.
 */
typedef struct br_pool {
    br_array items;        /* elem_size bytes per slot */
    br_array generations;  /* uint32_t per slot, starts at 1, bumped on release */
    br_array alive;        /* uint8_t per slot, 1 = in use */
    br_array free_slots;   /* uint32_t indices of free slots */
    uint32_t alive_count;
} br_pool;

/* Initialises an empty heap pool of elements of `elem_size` bytes. */
void br_pool_init(br_pool *p, uint32_t elem_size);
/* Like br_pool_init, but all storage comes from `allocator` (NULL = heap). */
void br_pool_init_with_allocator(br_pool *p, uint32_t elem_size, const br_allocator *allocator);
/* Frees all storage. */
void br_pool_free(br_pool *p);
/* Allocates a zeroed slot. Writes its handle to *index / *generation. Returns the data, NULL on failure. */
void *br_pool_alloc(br_pool *p, uint32_t *index, uint32_t *generation);
/* Returns the slot data if the handle is still alive, else NULL. */
void *br_pool_get(const br_pool *p, uint32_t index, uint32_t generation);
/* Frees the slot if the handle is alive. Returns false for stale or invalid handles. */
bool br_pool_release(br_pool *p, uint32_t index, uint32_t generation);
/* Number of slots ever created (alive or free); valid indices are 0..capacity-1. */
uint32_t br_pool_capacity(const br_pool *p);
/* Returns true if slot `index` is alive. Use with br_pool_capacity to iterate. */
bool br_pool_slot_alive(const br_pool *p, uint32_t index);
/* Returns the current generation of slot `index` (0 if out of range). */
uint32_t br_pool_slot_generation(const br_pool *p, uint32_t index);
/* Returns the data of slot `index` without a generation check (NULL if out of range). */
void *br_pool_slot_data(const br_pool *p, uint32_t index);

/* ---- br_map: string key -> fixed-size value, open addressing
 *
 *   br_map m; br_map_init(&m, sizeof(uint32_t));
 *   uint32_t id = 7; br_map_set(&m, "sprite", &id);
 *   uint32_t *found = br_map_get(&m, "sprite");
 *
 * Keys are copied. Value pointers are invalidated by br_map_set / br_map_remove.
 */
typedef struct br_map {
    char    **keys;       /* NULL = empty slot */
    uint32_t *hashes;
    uint8_t  *values;     /* capacity * value_size bytes */
    uint32_t  capacity;   /* power of two, or 0 */
    uint32_t  count;
    uint32_t  value_size;
    const br_allocator *allocator; /* NULL = heap; also used for the copied keys */
} br_map;

/* Initialises an empty heap map whose values are `value_size` bytes. */
void br_map_init(br_map *m, uint32_t value_size);
/* Like br_map_init, but storage and copied keys come from `allocator` (NULL = heap). */
void br_map_init_with_allocator(br_map *m, uint32_t value_size, const br_allocator *allocator);
/* Frees keys and storage. */
void br_map_free(br_map *m);
/* Inserts or overwrites `key`. Returns the stored value, NULL on failure. */
void *br_map_set(br_map *m, const char *key, const void *value);
/* Returns the value for `key`, or NULL if absent. */
void *br_map_get(const br_map *m, const char *key);
/* Removes `key`. Returns false if it was absent. */
bool br_map_remove(br_map *m, const char *key);
/* Removes everything, keeps the storage. */
void br_map_clear(br_map *m);
/* Iteration: call with *slot = 0, repeat while it returns true. Fills key and value. */
bool br_map_next(const br_map *m, uint32_t *slot, const char **key, void **value);

/* ---- strings */

/* Copies a string into new memory (free with br_free). NULL -> NULL. */
char *br_strdup(const char *s);
/* True if both strings are equal (NULL equals only NULL). */
bool br_str_equal(const char *a, const char *b);
/* True if `s` starts with `prefix`. */
bool br_str_starts_with(const char *s, const char *prefix);
/* True if `s` ends with `suffix`. */
bool br_str_ends_with(const char *s, const char *suffix);
/* Copies at most dst_size-1 bytes and always terminates. Returns false if truncated. */
bool br_str_copy(char *dst, size_t dst_size, const char *src);
/* FNV-1a 32-bit hash of a string. */
uint32_t br_str_hash(const char *s);
/* Joins two path parts with one '/', always terminated. Returns false if truncated. */
bool br_path_join(char *dst, size_t dst_size, const char *a, const char *b);
/* Returns the text after the last '.' of the last path segment ("" if none). Points into `path`. */
const char *br_path_extension(const char *path);
/* True for "/x", "\\x" and "C:..." style paths. */
bool br_path_is_absolute(const char *path);

/* ---- files (SDL_IOStream underneath). Relative paths are relative to the asset root. */

/* Reads a whole file. *bytes is NUL-terminated (not counted in *size); free with br_free. */
bool br_file_read_all(br_engine *e, const char *path, void **bytes, size_t *size);
/* Writes a whole file, replacing it. */
bool br_file_write_all(br_engine *e, const char *path, const void *bytes, size_t size);
/* True if the file exists. */
bool br_file_exists(br_engine *e, const char *path);
/* Resolves `path` against the asset root into `out`. Absolute paths are copied unchanged. */
bool br_file_resolve(br_engine *e, const char *path, char *out, size_t out_size);

/* ---- save data (SDL_Storage user storage, per org "braty" + the game title) */

/* Reads a save file. *bytes is NUL-terminated; free with br_free. False if missing. */
bool br_save_read(br_engine *e, const char *path, void **bytes, size_t *size);
/* Writes a save file, replacing it. */
bool br_save_write(br_engine *e, const char *path, const void *bytes, size_t size);
/* Deletes a save file. */
bool br_save_remove(br_engine *e, const char *path);

#endif
