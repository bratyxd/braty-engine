/* test_query - br_query: matching, without, deferred changes, misuse detection, benchmark (headless). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_timer.h>

typedef struct comp_a { float value; } comp_a;
typedef struct comp_b { float value; } comp_b;
typedef struct comp_c { float value; } comp_c;

typedef struct ids3 { uint32_t a, b, c; } ids3;

static ids3 register_components(br_engine *e)
{
    br_component_desc da = { "a", sizeof(comp_a), NULL, 0 };
    br_component_desc db = { "b", sizeof(comp_b), NULL, 0 };
    br_component_desc dc = { "c", sizeof(comp_c), NULL, 0 };
    ids3 ids;
    ids.a = br_component_register(e, &da);
    ids.b = br_component_register(e, &db);
    ids.c = br_component_register(e, &dc);
    return ids;
}

static void test_matching(void)
{
    br_engine *e = br_engine_create("test_query", NULL);
    ids3 id = register_components(e);
    /* node i has a always, b when i is even, c when i % 3 == 0 */
    br_node nodes[12];
    for (int i = 0; i < 12; i++) {
        nodes[i] = br_node_create_container(e);
        ((comp_a *)br_component_add(e, nodes[i], id.a))->value = (float)i;
        if (i % 2 == 0) {
            ((comp_b *)br_component_add(e, nodes[i], id.b))->value = (float)(i * 10);
        }
        if (i % 3 == 0) {
            ((comp_c *)br_component_add(e, nodes[i], id.c))->value = (float)(i * 100);
        }
    }

    /* a + b: even nodes; data[] follows the ids order whatever array drives */
    uint32_t ab[] = { id.a, id.b };
    br_query q = br_query_begin(e, ab, 2);
    BR_CHECK(br_query_upper_bound(&q) == 6);   /* b is the smaller array */
    int visited = 0;
    bool data_ok = true;
    while (br_query_next(&q)) {
        comp_a *a = q.data[0];
        comp_b *b = q.data[1];
        data_ok = data_ok && b->value == a->value * 10.0f && br_component_get(e, q.node, id.a) == a;
        visited++;
    }
    br_query_end(&q);
    BR_CHECK(visited == 6 && data_ok);

    /* a + b + c: i % 6 == 0 -> 0, 6 */
    uint32_t abc[] = { id.a, id.b, id.c };
    q = br_query_begin(e, abc, 3);
    visited = 0;
    while (br_query_next(&q)) {
        comp_c *c = q.data[2];
        BR_CHECK(((int)c->value / 100) % 6 == 0);
        visited++;
    }
    br_query_end(&q);
    BR_CHECK(visited == 2);

    /* a without c: 12 - 4 = 8 */
    uint32_t a_only[] = { id.a };
    q = br_query_begin(e, a_only, 1);
    br_query_without(&q, id.c);
    visited = 0;
    while (br_query_next(&q)) {
        BR_CHECK(br_component_get(e, q.node, id.c) == NULL);
        visited++;
    }
    br_query_end(&q);
    BR_CHECK(visited == 8);

    /* bad input: empty query, no crash */
    uint32_t bad[] = { 999 };
    q = br_query_begin(e, bad, 1);
    BR_CHECK(!br_query_next(&q));
    br_query_end(&q);
    q = br_query_begin(e, ab, 0);
    BR_CHECK(!br_query_next(&q));
    br_query_end(&q);
    uint32_t five[] = { id.a, id.b, id.c, id.a, id.b };
    q = br_query_begin(e, five, 5);
    BR_CHECK(!br_query_next(&q));
    br_query_end(&q);

    /* empty component array: nothing to visit */
    br_component_desc dd = { "d", sizeof(comp_a), NULL, 0 };
    uint32_t d_id = br_component_register(e, &dd);
    uint32_t ad[] = { id.a, d_id };
    q = br_query_begin(e, ad, 2);
    BR_CHECK(br_query_upper_bound(&q) == 0 && !br_query_next(&q));
    br_query_end(&q);

    br_engine_destroy(e);
}

static void test_deferred(void)
{
    br_engine *e = br_engine_create("test_query", NULL);
    ids3 id = register_components(e);
    for (int i = 0; i < 10; i++) {
        br_node n = br_node_create_container(e);
        ((comp_a *)br_component_add(e, n, id.a))->value = (float)i;
    }
    /* every node gets b, odd nodes lose a, node 0 is destroyed: all deferred */
    uint32_t a_only[] = { id.a };
    br_query q = br_query_begin(e, a_only, 1);
    int visited = 0;
    br_node first = { 0, 0 };
    while (br_query_next(&q)) {
        comp_a *a = q.data[0];
        comp_b init = { a->value + 0.5f };
        br_query_defer_add(&q, q.node, id.b, &init, sizeof(init));
        if ((int)a->value % 2 == 1) {
            br_query_defer_remove(&q, q.node, id.a);
        }
        if (a->value == 0.0f) {
            first = q.node;
            br_query_defer_destroy(&q, q.node);
        }
        /* nothing changed yet */
        BR_CHECK(br_component_get(e, q.node, id.b) == NULL);
        visited++;
    }
    BR_CHECK(visited == 10 && br_component_count(e, id.a) == 10);
    br_query_end(&q);
    BR_CHECK(!br_node_valid(e, first));                  /* destroyed (headless: flushed at once) */
    BR_CHECK(br_component_count(e, id.b) == 9);
    BR_CHECK(br_component_count(e, id.a) == 4);          /* 2, 4, 6, 8 */
    bool values_ok = true;
    for (uint32_t i = 0; i < br_component_count(e, id.b); i++) {
        comp_b *b = br_component_at(e, id.b, i);
        values_ok = values_ok && b->value - (float)(int)b->value == 0.5f;
    }
    BR_CHECK(values_ok);

    /* a zeroed add (no initial data) */
    q = br_query_begin(e, a_only, 1);
    while (br_query_next(&q)) {
        br_query_defer_add(&q, q.node, id.c, NULL, 0);
    }
    br_query_end(&q);
    BR_CHECK(br_component_count(e, id.c) == 4);
    BR_CHECK(((comp_c *)br_component_at(e, id.c, 0))->value == 0.0f);

    /* misuse: a direct remove of the walked array stops the loop instead of skipping rows */
    q = br_query_begin(e, a_only, 1);
    visited = 0;
    while (br_query_next(&q)) {
        br_component_remove(e, q.node, id.a);
        visited++;
    }
    br_query_end(&q);
    BR_CHECK(visited == 1);
    br_engine_destroy(e);
}

/* Inside a frame, br_node_destroy is deferred to the next flush: the query must skip such nodes. */
typedef struct pending_test {
    ids3     id;
    br_node  victim;
    int      visited;
} pending_test;

static void pending_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    pending_test *t = user;
    br_node_destroy(e, t->victim);
    BR_CHECK(br_component_count(e, t->id.a) == 3);   /* row still there until the flush */
    uint32_t a_only[] = { t->id.a };
    br_query q = br_query_begin(e, a_only, 1);
    while (br_query_next(&q)) {
        t->visited++;
    }
    br_query_end(&q);
}

static void test_pending_destroy(void)
{
    br_engine *e = br_engine_create("test_query", NULL);
    pending_test t;
    SDL_zero(t);
    t.id = register_components(e);
    for (int i = 0; i < 3; i++) {
        br_node n = br_node_create_container(e);
        br_component_add(e, n, t.id.a);
        t.victim = n;
    }
    br_system_add(e, "query.pending", BR_PHASE_UPDATE, 0, pending_system, &t);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(t.visited == 2);
    br_engine_destroy(e);
}

/* 100k nodes x 3 components: br_query vs the hand-written br_component_at + br_component_get loop. */
static void test_benchmark(void)
{
    const uint32_t n = 100000;
    const int rounds = 20;
    br_engine *e = br_engine_create("test_query", NULL);
    ids3 id = register_components(e);
    for (uint32_t i = 0; i < n; i++) {
        br_node node = br_node_create_container(e);
        ((comp_a *)br_component_add(e, node, id.a))->value = 1.0f;
        ((comp_b *)br_component_add(e, node, id.b))->value = 2.0f;
        ((comp_c *)br_component_add(e, node, id.c))->value = 0.0f;
    }
    uint32_t abc[] = { id.a, id.b, id.c };

    uint64_t t0 = SDL_GetTicksNS();
    uint32_t matched = 0;
    for (int r = 0; r < rounds; r++) {
        br_query q = br_query_begin(e, abc, 3);
        while (br_query_next(&q)) {
            comp_a *a = q.data[0];
            comp_b *b = q.data[1];
            comp_c *c = q.data[2];
            c->value += a->value * b->value;
            matched++;
        }
        br_query_end(&q);
    }
    uint64_t t1 = SDL_GetTicksNS();
    for (int r = 0; r < rounds; r++) {
        uint32_t count = br_component_count(e, id.a);
        for (uint32_t i = 0; i < count; i++) {
            comp_a *a = br_component_at(e, id.a, i);
            br_node owner = br_component_owner_at(e, id.a, i);
            comp_b *b = br_component_get(e, owner, id.b);
            comp_c *c = br_component_get(e, owner, id.c);
            if (b != NULL && c != NULL) {
                c->value += a->value * b->value;
            }
        }
    }
    uint64_t t2 = SDL_GetTicksNS();

    BR_CHECK(matched == n * (uint32_t)rounds);
    BR_CHECK(((comp_c *)br_component_at(e, id.c, n - 1))->value == 2.0f * 2.0f * (float)rounds);
    double total = (double)n * (double)rounds;
    printf("benchmark: %u nodes x 3 components, %d rounds\n", n, rounds);
    printf("  br_query:          %.2f ns per matched entity\n", (double)(t1 - t0) / total);
    printf("  hand-written loop: %.2f ns per matched entity\n", (double)(t2 - t1) / total);
    br_engine_destroy(e);
}

int main(void)
{
    test_matching();
    test_deferred();
    test_pending_destroy();
    test_benchmark();
    return br_test_finish();
}
