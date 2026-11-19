/* br_array: growable array of fixed-size elements. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>

static uint8_t *br_array_bytes(const br_array *a, uint32_t index)
{
    return (uint8_t *)a->data + (size_t)index * a->elem_size;
}

void br_array_init(br_array *a, uint32_t elem_size)
{
    a->data = NULL;
    a->count = 0;
    a->capacity = 0;
    a->elem_size = elem_size;
}

void br_array_free(br_array *a)
{
    br_free(a->data);
    a->data = NULL;
    a->count = 0;
    a->capacity = 0;
}

bool br_array_reserve(br_array *a, uint32_t capacity)
{
    if (capacity <= a->capacity) {
        return true;
    }
    uint32_t new_capacity = a->capacity > 0 ? a->capacity : 8;
    while (new_capacity < capacity) {
        new_capacity *= 2;
    }
    void *data = br_realloc(a->data, (size_t)new_capacity * a->elem_size);
    if (data == NULL) {
        return false;
    }
    a->data = data;
    a->capacity = new_capacity;
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
    if (!br_array_reserve(a, a->count + 1)) {
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
