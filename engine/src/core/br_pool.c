/* br_pool: generational slots. A handle is (index, generation); release bumps the generation. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>

void br_pool_init(br_pool *p, uint32_t elem_size)
{
    br_pool_init_with_allocator(p, elem_size, NULL);
}

void br_pool_init_with_allocator(br_pool *p, uint32_t elem_size, const br_allocator *allocator)
{
    br_array_init_with_allocator(&p->items, elem_size, allocator);
    br_array_init_with_allocator(&p->generations, sizeof(uint32_t), allocator);
    br_array_init_with_allocator(&p->alive, sizeof(uint8_t), allocator);
    br_array_init_with_allocator(&p->free_slots, sizeof(uint32_t), allocator);
    p->alive_count = 0;
}

void br_pool_free(br_pool *p)
{
    br_array_free(&p->items);
    br_array_free(&p->generations);
    br_array_free(&p->alive);
    br_array_free(&p->free_slots);
    p->alive_count = 0;
}

void *br_pool_alloc(br_pool *p, uint32_t *index, uint32_t *generation)
{
    uint32_t slot;
    if (p->free_slots.count > 0) {
        slot = *(uint32_t *)br_array_at(&p->free_slots, p->free_slots.count - 1);
        br_array_pop(&p->free_slots);
    } else {
        slot = p->items.count;
        uint32_t first_generation = 1;
        uint8_t dead = 0;
        if (br_array_push(&p->items, NULL) == NULL ||
            br_array_push(&p->generations, &first_generation) == NULL ||
            br_array_push(&p->alive, &dead) == NULL) {
            return NULL;
        }
    }
    uint8_t *alive = br_array_at(&p->alive, slot);
    *alive = 1;
    p->alive_count += 1;
    void *data = br_array_at(&p->items, slot);
    SDL_memset(data, 0, p->items.elem_size);
    *index = slot;
    *generation = *(uint32_t *)br_array_at(&p->generations, slot);
    return data;
}

void *br_pool_get(const br_pool *p, uint32_t index, uint32_t generation)
{
    if (generation == 0 || index >= p->items.count) {
        return NULL;
    }
    const uint8_t *alive = br_array_at(&p->alive, index);
    const uint32_t *gen = br_array_at(&p->generations, index);
    if (*alive == 0 || *gen != generation) {
        return NULL;
    }
    return br_array_at(&p->items, index);
}

bool br_pool_release(br_pool *p, uint32_t index, uint32_t generation)
{
    if (br_pool_get(p, index, generation) == NULL) {
        return false;
    }
    uint8_t *alive = br_array_at(&p->alive, index);
    uint32_t *gen = br_array_at(&p->generations, index);
    *alive = 0;
    *gen += 1;
    if (*gen == 0) {
        *gen = 1; /* after 4 billion reuses, skip the invalid generation */
    }
    p->alive_count -= 1;
    return br_array_push(&p->free_slots, &index) != NULL;
}

uint32_t br_pool_capacity(const br_pool *p)
{
    return p->items.count;
}

bool br_pool_slot_alive(const br_pool *p, uint32_t index)
{
    const uint8_t *alive = br_array_at(&p->alive, index);
    return alive != NULL && *alive != 0;
}

uint32_t br_pool_slot_generation(const br_pool *p, uint32_t index)
{
    const uint32_t *gen = br_array_at(&p->generations, index);
    return gen != NULL ? *gen : 0;
}

void *br_pool_slot_data(const br_pool *p, uint32_t index)
{
    return br_array_at(&p->items, index);
}
