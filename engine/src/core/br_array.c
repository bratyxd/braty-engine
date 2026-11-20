/* br_array: growable array of fixed-size elements. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>

static uint8_t *br_array_bytes(const br_array *a, uint32_t index)
{
    return (uint8_t *)a->data + (size_t)index * a->elem_size;
}

void br_array_init(br_array *a, uint32_t elem_size)
{
    br_array_init_with_allocator(a, elem_size, NULL);
}

void br_array_init_with_allocator(br_array *a, uint32_t elem_size, const br_allocator *allocator)
{
    a->data = NULL;
    a->count = 0;
    a->capacity = 0;
    a->elem_size = elem_size;
    a->allocator = allocator;
}

void br_array_free(br_array *a)
{
    br_allocator_free(a->allocator, a->data);
    a->data = NULL;
    a->count = 0;
    a->capacity = 0;
}

bool br_array_reserve(br_array *a, uint32_t capacity)
{
    if (capacity <= a->capacity) {
        return true;
    }
    /* uint64: doubling past 2^31 must not wrap (it would loop forever) */
    uint64_t new_capacity = a->capacity > 0 ? a->capacity : 8;
    while (new_capacity < capacity) {
        new_capacity *= 2;
    }
    if (new_capacity > UINT32_MAX) {
        new_capacity = UINT32_MAX;
    }
    if (a->elem_size > 0 && new_capacity > SIZE_MAX / a->elem_size) {
        br_log_error("br_array: %u elements of %u bytes do not fit in memory", capacity, a->elem_size);
        return false;
    }
    void *data = br_allocator_realloc(a->allocator, a->data, (size_t)a->capacity * a->elem_size,
                                      (size_t)new_capacity * a->elem_size);
    if (data == NULL) {
        return false;
    }
    a->data = data;
    a->capacity = (uint32_t)new_capacity;
    return true;
}

bool br_array_resize(br_array *a, uint32_t count)
{
    if (!br_array_reserve(a, count)) {
        return false;
    }
    if (count > a->count) {
        SDL_memset(br_array_bytes(a, a->count), 0, (size_t)(count - a->count) * a->elem_size);
    }
    a->count = count;
    return true;
}

void *br_array_push_n(br_array *a, const void *elems, uint32_t n)
{
    if (n > UINT32_MAX - a->count) {
        br_log_error("br_array: %u + %u elements overflow the count", a->count, n);
        return NULL;
    }
    if (!br_array_reserve(a, a->count + n)) {
        return NULL;
    }
    uint8_t *dst = br_array_bytes(a, a->count);
    if (elems != NULL) {
        SDL_memcpy(dst, elems, (size_t)n * a->elem_size);
    } else {
        SDL_memset(dst, 0, (size_t)n * a->elem_size);
    }
    a->count += n;
    return dst;
}

void *br_array_push(br_array *a, const void *elem)
{
    return br_array_push_n(a, elem, 1);
}

void *br_array_insert(br_array *a, uint32_t index, const void *elem)
{
    if (index >= a->count) {
        return br_array_push(a, elem);
    }
    if (a->count == UINT32_MAX || !br_array_reserve(a, a->count + 1)) {
        return NULL;
    }
    uint8_t *dst = br_array_bytes(a, index);
    SDL_memmove(dst + a->elem_size, dst, (size_t)(a->count - index) * a->elem_size);
    if (elem != NULL) {
        SDL_memcpy(dst, elem, a->elem_size);
    } else {
        SDL_memset(dst, 0, a->elem_size);
    }
    a->count += 1;
    return dst;
}

void *br_array_at(const br_array *a, uint32_t index)
{
    if (index >= a->count) {
        return NULL;
    }
    return br_array_bytes(a, index);
}

void br_array_pop(br_array *a)
{
    if (a->count > 0) {
        a->count -= 1;
    }
}

void br_array_remove_swap(br_array *a, uint32_t index)
{
    if (index >= a->count) {
        return;
    }
    uint32_t last = a->count - 1;
    if (index != last) {
        SDL_memcpy(br_array_bytes(a, index), br_array_bytes(a, last), a->elem_size);
    }
    a->count = last;
}

void br_array_remove_ordered(br_array *a, uint32_t index)
{
    if (index >= a->count) {
        return;
    }
    uint8_t *dst = br_array_bytes(a, index);
    SDL_memmove(dst, dst + a->elem_size, (size_t)(a->count - index - 1) * a->elem_size);
    a->count -= 1;
}

void br_array_clear(br_array *a)
{
    a->count = 0;
}

bool br_u32_mul_ok(uint32_t a, uint32_t b, uint32_t *out)
{
    uint64_t product = (uint64_t)a * b;
    *out = (uint32_t)product;
    return product <= UINT32_MAX;
}

bool br_size_mul_ok(size_t a, size_t b, size_t *out)
{
    *out = a * b;
    return a == 0 || *out / a == b;
}
