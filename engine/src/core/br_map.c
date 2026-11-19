/* br_map: string-keyed hash map, linear probing, backward-shift deletion (no tombstones). */
#include "braty/br_core.h"

#include <SDL3/SDL.h>

static uint8_t *br_map_value_at(const br_map *m, uint32_t slot)
{
    return m->values + (size_t)slot * m->value_size;
}

void br_map_init(br_map *m, uint32_t value_size)
{
    br_map_init_with_allocator(m, value_size, NULL);
}

void br_map_init_with_allocator(br_map *m, uint32_t value_size, const br_allocator *allocator)
{
    m->keys = NULL;
    m->hashes = NULL;
    m->values = NULL;
    m->capacity = 0;
    m->count = 0;
    m->value_size = value_size;
    m->allocator = allocator;
}

static char *br_map_copy_key(const br_map *m, const char *key)
{
    size_t len = SDL_strlen(key);
    char *copy = br_allocator_alloc(m->allocator, len + 1);
    if (copy != NULL) {
        SDL_memcpy(copy, key, len + 1);
    }
    return copy;
}

void br_map_free(br_map *m)
{
    for (uint32_t i = 0; i < m->capacity; i++) {
        br_allocator_free(m->allocator, m->keys[i]);
    }
    br_allocator_free(m->allocator, m->keys);
    br_allocator_free(m->allocator, m->hashes);
    br_allocator_free(m->allocator, m->values);
    br_map_init_with_allocator(m, m->value_size, m->allocator);
}

/* Returns the slot holding `key`, or the empty slot where it would go. Capacity must be > 0. */
static uint32_t br_map_find_slot(const br_map *m, const char *key, uint32_t hash)
{
    uint32_t mask = m->capacity - 1;
    uint32_t slot = hash & mask;
    while (m->keys[slot] != NULL) {
        if (m->hashes[slot] == hash && SDL_strcmp(m->keys[slot], key) == 0) {
            return slot;
        }
        slot = (slot + 1) & mask;
    }
    return slot;
}

static bool br_map_grow(br_map *m)
{
    uint32_t new_capacity = m->capacity > 0 ? m->capacity * 2 : 16;
    br_map bigger;
    br_map_init_with_allocator(&bigger, m->value_size, m->allocator);
    bigger.keys = br_allocator_alloc(m->allocator, sizeof(char *) * new_capacity);
    bigger.hashes = br_allocator_alloc(m->allocator, sizeof(uint32_t) * new_capacity);
    bigger.values = br_allocator_alloc(m->allocator, (size_t)new_capacity * m->value_size);
    if (bigger.keys == NULL || bigger.hashes == NULL || bigger.values == NULL) {
        br_allocator_free(m->allocator, bigger.keys);
        br_allocator_free(m->allocator, bigger.hashes);
        br_allocator_free(m->allocator, bigger.values);
        return false;
    }
    bigger.capacity = new_capacity;
    for (uint32_t i = 0; i < m->capacity; i++) {
        if (m->keys[i] == NULL) {
            continue;
        }
        uint32_t slot = br_map_find_slot(&bigger, m->keys[i], m->hashes[i]);
        bigger.keys[slot] = m->keys[i]; /* key memory moves over, not copied */
        bigger.hashes[slot] = m->hashes[i];
        SDL_memcpy(br_map_value_at(&bigger, slot), br_map_value_at(m, i), m->value_size);
        bigger.count += 1;
    }
    br_allocator_free(m->allocator, m->keys);
    br_allocator_free(m->allocator, m->hashes);
    br_allocator_free(m->allocator, m->values);
    *m = bigger;
    return true;
}

void *br_map_set(br_map *m, const char *key, const void *value)
{
    /* keep load factor <= 3/4 so probes stay short */
    if ((m->count + 1) * 4 > m->capacity * 3) {
        if (!br_map_grow(m)) {
            return NULL;
        }
    }
    uint32_t hash = br_str_hash(key);
    uint32_t slot = br_map_find_slot(m, key, hash);
    if (m->keys[slot] == NULL) {
        m->keys[slot] = br_map_copy_key(m, key);
        if (m->keys[slot] == NULL) {
            return NULL;
        }
        m->hashes[slot] = hash;
        m->count += 1;
    }
    uint8_t *dst = br_map_value_at(m, slot);
    if (value != NULL) {
        SDL_memcpy(dst, value, m->value_size);
    } else {
        SDL_memset(dst, 0, m->value_size);
    }
    return dst;
}

void *br_map_get(const br_map *m, const char *key)
{
    if (m->capacity == 0 || key == NULL) {
        return NULL;
    }
    uint32_t slot = br_map_find_slot(m, key, br_str_hash(key));
    if (m->keys[slot] == NULL) {
        return NULL;
    }
    return br_map_value_at(m, slot);
}

bool br_map_remove(br_map *m, const char *key)
{
    if (m->capacity == 0 || key == NULL) {
        return false;
    }
    uint32_t mask = m->capacity - 1;
    uint32_t hole = br_map_find_slot(m, key, br_str_hash(key));
    if (m->keys[hole] == NULL) {
        return false;
    }
    br_allocator_free(m->allocator, m->keys[hole]);
    m->keys[hole] = NULL;
    m->count -= 1;
    /* Backward shift: pull later entries of the same probe run into the hole. */
    uint32_t next = (hole + 1) & mask;
    while (m->keys[next] != NULL) {
        uint32_t home = m->hashes[next] & mask;
        /* distance from home to next vs. home to hole, both measured forward around the ring */
        uint32_t dist_next = (next - home) & mask;
        uint32_t dist_hole = (hole - home) & mask;
        if (dist_hole < dist_next) {
            m->keys[hole] = m->keys[next];
            m->hashes[hole] = m->hashes[next];
            SDL_memcpy(br_map_value_at(m, hole), br_map_value_at(m, next), m->value_size);
            m->keys[next] = NULL;
            hole = next;
        }
        next = (next + 1) & mask;
    }
    return true;
}

void br_map_clear(br_map *m)
{
    for (uint32_t i = 0; i < m->capacity; i++) {
        br_allocator_free(m->allocator, m->keys[i]);
        m->keys[i] = NULL;
    }
    m->count = 0;
}

bool br_map_next(const br_map *m, uint32_t *slot, const char **key, void **value)
{
    while (*slot < m->capacity) {
        uint32_t i = *slot;
        *slot += 1;
        if (m->keys[i] != NULL) {
            if (key != NULL) {
                *key = m->keys[i];
            }
            if (value != NULL) {
                *value = br_map_value_at(m, i);
            }
            return true;
        }
    }
    return false;
}
