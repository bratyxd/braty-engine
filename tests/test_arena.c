/* Arenas, br_allocator on containers, frame arena, heap statistics. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

static bool aligned(const void *p, size_t alignment)
{
    return ((uintptr_t)p & (alignment - 1)) == 0;
}

static void test_linear(void)
{
    br_arena_linear a;
    BR_CHECK(br_arena_linear_create(&a, "linear", 256, false));
    BR_CHECK(a.stats.capacity == 256 && a.stats.used == 0);
    BR_CHECK(SDL_strcmp(a.stats.name, "linear") == 0);

    size_t alignments[] = { 1, 2, 8, 16, 64 };
    for (size_t i = 0; i < BR_ARRAY_COUNT(alignments); i++) {
        br_arena_linear_alloc(&a, 1); /* knock the offset off alignment */
        uint8_t *p = br_arena_linear_alloc_aligned(&a, 3, alignments[i]);
        BR_CHECK(p != NULL && aligned(p, alignments[i]));
        BR_CHECK(p[0] == 0 && p[2] == 0);
    }
    BR_CHECK(br_arena_linear_alloc_aligned(&a, 4, 3) == NULL); /* not a power of two */

    br_arena_linear_reset(&a);
    BR_CHECK(a.stats.used == 0);
    BR_CHECK(br_arena_linear_alloc(&a, 100) != NULL);
    BR_CHECK(a.stats.used == 100);
    BR_CHECK(br_arena_linear_alloc(&a, 200) == NULL); /* overflow: NULL and a log line */
    BR_CHECK(a.stats.used == 100);
    BR_CHECK(br_arena_linear_alloc(&a, 140) != NULL); /* 112 + 140 = 252 still fits */
    BR_CHECK(a.stats.peak == 252);
    br_arena_linear_reset(&a);
    BR_CHECK(a.stats.used == 0 && a.stats.peak == 252);
    br_arena_linear_destroy(&a);

    /* growable: chains blocks, reset merges them into one */
    BR_CHECK(br_arena_linear_create(&a, "grow", 64, true));
    for (int i = 0; i < 10; i++) {
        BR_CHECK(br_arena_linear_alloc(&a, 48) != NULL);
    }
    BR_CHECK(a.stats.capacity > 64 && a.blocks->prev != NULL);
    BR_CHECK(br_arena_linear_alloc(&a, 1000) != NULL); /* bigger than any block */
    size_t grown = a.stats.capacity;
    br_arena_linear_reset(&a);
    BR_CHECK(a.blocks->prev == NULL && a.stats.capacity == grown && a.size == grown);
    br_arena_linear_destroy(&a);
    BR_CHECK(a.stats.capacity == 0 && SDL_strcmp(a.stats.name, "grow") == 0);
}

static void test_caller_memory(void)
{
    br_memory_stats before, after;
    uint8_t buffer[512];
    br_memory_get_stats(&before);

    br_arena_linear a;
    BR_CHECK(br_arena_linear_create_in(&a, "in", buffer, sizeof(buffer)));
    uint8_t *p = br_arena_linear_alloc(&a, 300);
    BR_CHECK(p >= buffer && p + 300 <= buffer + sizeof(buffer));
    BR_CHECK(br_arena_linear_alloc(&a, 300) == NULL); /* fixed: never grows */

    br_arena_stack s;
    BR_CHECK(br_arena_stack_create_in(&s, "stack-in", buffer, sizeof(buffer)));
    uint8_t *q = br_arena_stack_alloc(&s, 64, 32);
    BR_CHECK(q >= buffer && q + 64 <= buffer + sizeof(buffer) && aligned(q, 32));

    br_arena_pool pool;
    BR_CHECK(br_arena_pool_create_in(&pool, "pool-in", 40, 16, buffer, sizeof(buffer)));
    BR_CHECK(pool.block_size == 48 && pool.block_count >= 9);
    for (uint32_t i = 0; i < pool.block_count; i++) {
        uint8_t *b = br_arena_pool_alloc(&pool);
        BR_CHECK(b >= buffer && b + 48 <= buffer + sizeof(buffer) && aligned(b, 16));
    }
    BR_CHECK(br_arena_pool_alloc(&pool) == NULL);

    br_memory_get_stats(&after);
    BR_CHECK(after.total_allocations == before.total_allocations); /* no heap use at all */
}

static void test_stack(void)
{
    br_arena_stack s;
    BR_CHECK(br_arena_stack_create(&s, "stack", 1024));
    BR_CHECK(br_arena_stack_alloc(&s, 10, 8) != NULL);
    br_arena_marker m1 = br_arena_stack_push_marker(&s);
    BR_CHECK(br_arena_stack_alloc(&s, 100, 8) != NULL);
    br_arena_marker m2 = br_arena_stack_push_marker(&s);
    BR_CHECK(br_arena_stack_alloc(&s, 100, 64) != NULL);
    size_t used = s.linear.stats.used;
#ifndef NDEBUG
    /* popping m1 while m2 is still pushed is caught in debug builds */
    BR_CHECK(!br_arena_stack_pop_to_marker(&s, m1));
    BR_CHECK(s.order_errors == 1);
    BR_CHECK(s.linear.stats.used == used);
#endif
    BR_CHECK(br_arena_stack_pop_to_marker(&s, m2));
    BR_CHECK(s.linear.stats.used == m2.offset && s.linear.stats.used < used);
    BR_CHECK(br_arena_stack_pop_to_marker(&s, m1));
    BR_CHECK(s.linear.stats.used == 10);
    BR_CHECK(!br_arena_stack_pop_to_marker(&s, m1)); /* already popped */
    /* R1: a stale marker at the newest depth used to be accepted (popping live allocations) */
    br_arena_marker a = br_arena_stack_push_marker(&s);
    BR_CHECK(br_arena_stack_alloc(&s, 16, 8) != NULL);
    BR_CHECK(br_arena_stack_pop_to_marker(&s, a));
    BR_CHECK(br_arena_stack_alloc(&s, 32, 8) != NULL); /* live, allocated after a was popped */
    br_arena_marker b = br_arena_stack_push_marker(&s);
    BR_CHECK(br_arena_stack_alloc(&s, 16, 8) != NULL);
    size_t live = s.linear.stats.used;
    BR_CHECK(!br_arena_stack_pop_to_marker(&s, a));
    BR_CHECK(s.linear.stats.used == live);
    BR_CHECK(br_arena_stack_pop_to_marker(&s, b));
    BR_CHECK(br_arena_stack_alloc(&s, 2000, 8) == NULL); /* overflow */
    BR_CHECK(s.linear.stats.peak >= used);
    br_arena_stack_destroy(&s);
}

static void test_pool(void)
{
    br_arena_pool p;
    BR_CHECK(br_arena_pool_create(&p, "pool", 24, 64, 4));
    BR_CHECK(p.block_size == 64 && p.stats.capacity == 256);
    void *blocks[4];
    for (int i = 0; i < 4; i++) {
        blocks[i] = br_arena_pool_alloc(&p);
        BR_CHECK(blocks[i] != NULL && aligned(blocks[i], 64));
    }
    BR_CHECK(p.stats.used == 256);
    BR_CHECK(br_arena_pool_alloc(&p) == NULL); /* exhausted */
    br_arena_pool_free(&p, blocks[2]);
    BR_CHECK(p.stats.used == 192);
    BR_CHECK(br_arena_pool_alloc(&p) == blocks[2]); /* freed block is handed out again */
    int outside = 0;
    br_arena_pool_free(&p, &outside); /* not ours: logged, ignored */
    BR_CHECK(p.stats.used == 256 && p.stats.peak == 256);
    /* R1: a double free used to hand the same block out twice */
    br_arena_pool_free(&p, blocks[1]);
    br_arena_pool_free(&p, blocks[1]); /* logged, ignored */
    BR_CHECK(p.stats.used == 192);
    void *x = br_arena_pool_alloc(&p);
    void *y = br_arena_pool_alloc(&p);
    BR_CHECK(x == blocks[1] && y == NULL);
    br_arena_pool_destroy(&p);
    br_arena_pool bad;
    BR_CHECK(!br_arena_pool_create(&bad, "bad", 24, 12, 4)); /* alignment not a power of two */
}

static void test_scratch(void)
{
    br_arena_linear a;
    BR_CHECK(br_arena_linear_create(&a, "scratch", 128, true));
    br_arena_linear_alloc(&a, 32);
    br_arena_scratch scratch = br_arena_scratch_begin(&a);
    br_arena_linear_alloc(&a, 64);
    br_arena_linear_alloc(&a, 500); /* chains a block */
    BR_CHECK(a.stats.capacity > 128);
    br_arena_scratch_end(scratch);
    BR_CHECK(a.stats.used == 32 && a.stats.capacity == 128 && a.blocks->prev == NULL);
    BR_CHECK(br_arena_linear_alloc(&a, 16) != NULL);
    br_arena_linear_destroy(&a);
}

static void test_containers_on_arenas(void)
{
    br_memory_stats before, after;
    uint8_t buffer[16 * 1024];
    br_arena_linear a;
    br_arena_linear_create_in(&a, "containers", buffer, sizeof(buffer));
    br_memory_get_stats(&before);

    br_array arr;
    br_array_init_with_allocator(&arr, sizeof(int), br_arena_linear_allocator(&a));
    for (int i = 0; i < 200; i++) {
        br_array_push(&arr, &i);
    }
    BR_CHECK(arr.count == 200 && *(int *)br_array_at(&arr, 123) == 123);
    BR_CHECK((uint8_t *)arr.data >= buffer && (uint8_t *)arr.data < buffer + sizeof(buffer));

    br_map m;
    br_map_init_with_allocator(&m, sizeof(int), br_arena_linear_allocator(&a));
    for (int i = 0; i < 40; i++) {
        char key[16];
        SDL_snprintf(key, sizeof(key), "k%d", i);
        br_map_set(&m, key, &i);
    }
    int *v = br_map_get(&m, "k17");
    BR_CHECK(v != NULL && *v == 17 && m.count == 40);
    BR_CHECK(br_map_remove(&m, "k3") && br_map_get(&m, "k3") == NULL);

    br_pool p;
    br_pool_init_with_allocator(&p, sizeof(double), br_arena_linear_allocator(&a));
    uint32_t index, generation;
    for (int i = 0; i < 50; i++) {
        BR_CHECK(br_pool_alloc(&p, &index, &generation) != NULL);
    }
    BR_CHECK(br_pool_release(&p, index, generation) && br_pool_get(&p, index, generation) == NULL);

    br_memory_get_stats(&after);
    BR_CHECK(after.total_allocations == before.total_allocations); /* all inside the buffer */
    br_array_free(&arr);
    br_map_free(&m);
    br_pool_free(&p);
    BR_CHECK(arr.allocator == br_arena_linear_allocator(&a)); /* free keeps the allocator */

    /* a pool arena serves small fixed-size requests through its allocator view */
    br_arena_pool pool;
    br_arena_pool_create(&pool, "nodes", 32, 16, 8);
    const br_allocator *pa = br_arena_pool_allocator(&pool);
    void *x = br_allocator_alloc(pa, 20);
    BR_CHECK(x != NULL && pool.stats.used == 32);
    BR_CHECK(br_allocator_alloc(pa, 33) == NULL);
    br_allocator_free(pa, x);
    BR_CHECK(pool.stats.used == 0);
    br_arena_pool_destroy(&pool);

    /* zero-initialised containers stay heap containers */
    br_array heap_array;
    SDL_zero(heap_array);
    heap_array.elem_size = sizeof(int);
    int one = 1;
    BR_CHECK(br_array_push(&heap_array, &one) != NULL);
    br_array_free(&heap_array);
}

static void test_heap_stats(void)
{
    br_memory_stats s0, s1, s2, s3;
    br_memory_get_stats(&s0);
    void *p = br_alloc(1000);
    br_memory_get_stats(&s1);
    BR_CHECK(s1.live_allocations == s0.live_allocations + 1);
    BR_CHECK(s1.live_bytes == s0.live_bytes + 1000);
    BR_CHECK(s1.total_allocations == s0.total_allocations + 1);
    BR_CHECK(s1.peak_bytes >= s1.live_bytes);
    p = br_realloc(p, 3000);
    br_memory_get_stats(&s2);
    BR_CHECK(s2.live_bytes == s0.live_bytes + 3000 && s2.live_allocations == s1.live_allocations);
    br_free(p);
    char *str = br_strdup("hello");
    br_free(str);
    br_memory_get_stats(&s3);
    BR_CHECK(s3.live_bytes == s0.live_bytes && s3.live_allocations == s0.live_allocations);
    BR_CHECK(s3.total_allocations == s0.total_allocations + 2);
    BR_CHECK(s3.peak_bytes >= s0.live_bytes + 3000);
}

static void test_frame_arena(void)
{
    br_engine *e = br_engine_create("braty-test-arena", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return;
    }
    br_arena_linear *frame = br_frame_arena(e);
    BR_CHECK(frame != NULL && frame->stats.capacity == BR_FRAME_ARENA_SIZE);
    BR_CHECK(br_arena_count(e) == 3 && br_arena_at(e, 0) == &frame->stats);

    uint8_t *p = br_frame_alloc(e, 4096);
    BR_CHECK(p != NULL && aligned(p, 16) && frame->stats.used >= 4096);
    br_engine_step(e, 0.016f);
    BR_CHECK(frame->stats.used == 0); /* reset at the start of every frame */
    BR_CHECK(br_frame_alloc(e, 2 * BR_FRAME_ARENA_SIZE) != NULL); /* grows */
    br_engine_step(e, 0.016f);
    BR_CHECK(frame->stats.used == 0 && frame->blocks->prev == NULL);

    br_arena_linear mine;
    br_arena_linear_create(&mine, "mine", 1024, false);
    BR_CHECK(br_arena_track(e, &mine.stats));
    BR_CHECK(br_arena_count(e) == 4 && SDL_strcmp(br_arena_at(e, 3)->name, "mine") == 0);
    br_arena_untrack(e, &mine.stats);
    BR_CHECK(br_arena_count(e) == 3 && br_arena_at(e, 3) == NULL);
    br_arena_linear_destroy(&mine);

    /* level and game arenas: survive frames; level is reset by the game, both grow when full */
    br_arena_linear *level = br_level_arena(e);
    br_arena_linear *game = br_game_arena(e);
    BR_CHECK(level != NULL && level->stats.capacity == BR_LEVEL_ARENA_SIZE && br_arena_at(e, 1) == &level->stats);
    BR_CHECK(game != NULL && game->stats.capacity == BR_GAME_ARENA_SIZE && br_arena_at(e, 2) == &game->stats);
    BR_CHECK(br_arena_preallocate(e, 0, 64 * 1024, 32 * 1024));
    BR_CHECK(level->stats.capacity == 64 * 1024 && game->stats.capacity == 32 * 1024);
    BR_CHECK(frame->stats.capacity >= BR_FRAME_ARENA_SIZE); /* 0 = kept */
    uint8_t *lp = br_level_alloc(e, 1000);
    uint8_t *gp = br_game_alloc(e, 1000);
    BR_CHECK(lp != NULL && gp != NULL && aligned(lp, 16) && aligned(gp, 16));
    lp[999] = 7;
    gp[999] = 9;
    br_engine_step(e, 0.016f);
    BR_CHECK(lp[999] == 7 && gp[999] == 9 && level->stats.used >= 1000 && game->stats.used >= 1000);
    BR_CHECK(!br_arena_preallocate(e, 0, 128 * 1024, 0)); /* in use: refused, nothing changes */
    BR_CHECK(level->stats.capacity == 64 * 1024 && lp[999] == 7);
    BR_CHECK(br_level_alloc(e, 200 * 1024) != NULL); /* over the preallocated size: grows */
    BR_CHECK(level->stats.capacity > 64 * 1024);
    br_level_arena_reset(e);
    BR_CHECK(level->stats.used == 0 && level->blocks->prev == NULL && game->stats.used >= 1000 && gp[999] == 9);
    br_engine_destroy(e);
}

int main(void)
{
    test_linear();
    test_caller_memory();
    test_stack();
    test_pool();
    test_scratch();
    test_containers_on_arenas();
    test_heap_stats();
    test_frame_arena();
    return br_test_finish();
}
