/* Memory and logging: thin wrappers so every allocation and message goes through SDL.
 * br_alloc counts blocks and bytes (br_memory_get_stats); br_allocator dispatch lives here too. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>
#include <stdarg.h>

/* Every heap block starts with this header so br_free knows how many bytes leave.
 * 16 bytes on every platform so the user pointer stays 16-byte aligned (max_align_t is missing
 * in MSVC's C headers and only 8 on Apple arm64). */
typedef union br_alloc_header {
    size_t size;
    char   pad[16];
} br_alloc_header;

// ponytail: process-wide counters under one spinlock (the heap itself is process-wide);
// per-engine budgets would need an engine pointer in every br_alloc call.
static SDL_SpinLock br_memory_lock;
static br_memory_stats br_memory_counters;

static void br_memory_count(size_t added, size_t removed, int64_t blocks, bool new_block)
{
    SDL_LockSpinlock(&br_memory_lock);
    br_memory_counters.live_bytes += added;
    br_memory_counters.live_bytes -= removed;
    br_memory_counters.live_allocations = (uint64_t)((int64_t)br_memory_counters.live_allocations + blocks);
    if (new_block) {
        br_memory_counters.total_allocations += 1;
    }
    if (br_memory_counters.live_bytes > br_memory_counters.peak_bytes) {
        br_memory_counters.peak_bytes = br_memory_counters.live_bytes;
    }
    SDL_UnlockSpinlock(&br_memory_lock);
}

void br_memory_get_stats(br_memory_stats *out)
{
    SDL_LockSpinlock(&br_memory_lock);
    *out = br_memory_counters;
    SDL_UnlockSpinlock(&br_memory_lock);
}

void *br_alloc(size_t size)
{
    br_alloc_header *h = NULL;
    if (size <= SIZE_MAX - sizeof(br_alloc_header)) {
        h = SDL_calloc(1, sizeof(br_alloc_header) + size);
    }
    if (h == NULL) {
        br_log_error("out of memory allocating %zu bytes", size);
        return NULL;
    }
    h->size = size;
    br_memory_count(size, 0, 1, true);
    return h + 1;
}

void *br_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return br_alloc(size);
    }
    br_alloc_header *old = (br_alloc_header *)ptr - 1;
    size_t old_size = old->size;
    br_alloc_header *h = NULL;
    if (size <= SIZE_MAX - sizeof(br_alloc_header)) {
        h = SDL_realloc(old, sizeof(br_alloc_header) + size);
    }
    if (h == NULL) {
        br_log_error("out of memory reallocating %zu bytes", size);
        return NULL;
    }
    h->size = size;
    br_memory_count(size, old_size, 0, false);
    return h + 1;
}

void br_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    br_alloc_header *h = (br_alloc_header *)ptr - 1;
    br_memory_count(0, h->size, -1, false);
    SDL_free(h);
}

void *br_allocator_alloc(const br_allocator *a, size_t size)
{
    return a != NULL ? a->alloc(a->user, size) : br_alloc(size);
}

void *br_allocator_realloc(const br_allocator *a, void *ptr, size_t old_size, size_t new_size)
{
    return a != NULL ? a->realloc(a->user, ptr, old_size, new_size) : br_realloc(ptr, new_size);
}

void br_allocator_free(const br_allocator *a, void *ptr)
{
    if (a != NULL) {
        a->free(a->user, ptr);
    } else {
        br_free(ptr);
    }
}

static void br_log_v(SDL_LogPriority priority, const char *fmt, va_list args)
{
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, priority, fmt, args);
}

void br_log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_DEBUG, fmt, args);
    va_end(args);
}

void br_log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_INFO, fmt, args);
    va_end(args);
}

void br_log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_WARN, fmt, args);
    va_end(args);
}

void br_log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_ERROR, fmt, args);
    va_end(args);
}
