/* Background jobs: ranges cover every item once, br_job_run does not block, cancel, the
 * deterministic inline path, more jobs than slots, no allocation per job. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

#define ITEM_COUNT 10000

typedef struct squares {
    uint32_t out[ITEM_COUNT];
    uint8_t  visits[ITEM_COUNT];
} squares;

static squares table;

static void square_range(void *user, uint32_t begin, uint32_t end)
{
    squares *t = user;
    for (uint32_t i = begin; i < end; i++) {
        t->out[i] = i * i;
        t->visits[i]++;
    }
}

static bool squares_ok(const squares *t)
{
    for (uint32_t i = 0; i < ITEM_COUNT; i++) {
        if (t->out[i] != i * i || t->visits[i] != 1) {
            return false;
        }
    }
    return true;
}

/* A job that cannot finish until the test opens the gate. */
typedef struct gated {
    SDL_AtomicInt open;
    SDL_AtomicInt items;
} gated;

static void gated_range(void *user, uint32_t begin, uint32_t end)
{
    gated *g = user;
    while (SDL_GetAtomicInt(&g->open) == 0) {
        SDL_Delay(1);
    }
    SDL_AddAtomicInt(&g->items, (int)(end - begin));
}

static void slow_range(void *user, uint32_t begin, uint32_t end)
{
    SDL_Delay(1);
    SDL_AddAtomicInt(user, (int)(end - begin));
}

/* Steps the engine until the job is done, 10 s at most. */
static bool step_until_done(br_engine *e, br_job job)
{
    for (int i = 0; i < 10000 && !br_job_is_done(e, job); i++) {
        br_engine_step(e, 1.0f / 60.0f);
        SDL_Delay(1);
    }
    return br_job_is_done(e, job);
}

static void test_ranges(br_engine *e)
{
    br_job none = { 0, 0 };
    BR_CHECK(br_job_is_done(e, none));
    BR_CHECK(br_job_progress(e, none) == 1.0f);
    BR_CHECK(br_job_is_done(e, br_job_run(e, square_range, &table, 0, 16))); /* nothing to do */

    SDL_zero(table);
    br_job job = br_job_run(e, square_range, &table, ITEM_COUNT, 64);
    br_job_wait(e, job);
    BR_CHECK(br_job_is_done(e, job));
    BR_CHECK(br_job_progress(e, job) == 1.0f);
    BR_CHECK(squares_ok(&table));
    BR_CHECK(br_job_worker_count(e) >= 1 && br_job_worker_count(e) <= BR_JOB_MAX_WORKERS);

    SDL_zero(table);
    job = br_job_run(e, square_range, &table, ITEM_COUNT, 0); /* batch 0: one range */
    BR_CHECK(step_until_done(e, job));
    BR_CHECK(squares_ok(&table));

    SDL_zero(table);
    job = br_job_run(e, square_range, &table, ITEM_COUNT, 3000); /* last range is short */
    br_job_wait(e, job);
    BR_CHECK(squares_ok(&table));
}

static void test_does_not_block(br_engine *e)
{
    gated g;
    SDL_zero(g);
    br_job job = br_job_run(e, gated_range, &g, 40, 10);
    /* the job cannot finish, yet br_job_run returned and frames keep stepping */
    for (int i = 0; i < 5; i++) {
        br_engine_step(e, 1.0f / 60.0f); /* false headless: nothing was drawn */
    }
    BR_CHECK(!br_job_is_done(e, job));
    BR_CHECK(br_job_progress(e, job) < 1.0f);
    SDL_SetAtomicInt(&g.open, 1);
    BR_CHECK(step_until_done(e, job));
    BR_CHECK(SDL_GetAtomicInt(&g.items) == 40);
}

static void test_cancel(br_engine *e)
{
    SDL_AtomicInt items;
    SDL_SetAtomicInt(&items, 0);
    br_job job = br_job_run(e, slow_range, &items, 100000, 1); /* > 10 s of work */
    br_job_cancel(e, job);
    BR_CHECK(br_job_is_done(e, job));
    int after_cancel = SDL_GetAtomicInt(&items);
    BR_CHECK(after_cancel < 100000);
    SDL_Delay(20);
    BR_CHECK(SDL_GetAtomicInt(&items) == after_cancel); /* nothing runs after the cancel returned */
    br_job_cancel(e, job); /* a finished handle: no-op */

    br_job a = br_job_run(e, slow_range, &items, 100000, 1);
    br_job b = br_job_run(e, slow_range, &items, 100000, 1);
    br_job_cancel_all(e);
    BR_CHECK(br_job_is_done(e, a) && br_job_is_done(e, b));
}

static void test_more_jobs_than_slots(br_engine *e)
{
    gated g;
    SDL_zero(g);
    br_job jobs[BR_JOB_MAX];
    for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
        jobs[i] = br_job_run(e, gated_range, &g, 1, 1);
    }
    SDL_zero(table);
    br_job extra = br_job_run(e, square_range, &table, ITEM_COUNT, 64); /* no slot: runs here */
    BR_CHECK(br_job_is_done(e, extra));
    BR_CHECK(squares_ok(&table));
    BR_CHECK(!br_job_is_done(e, jobs[BR_JOB_MAX - 1]));
    SDL_SetAtomicInt(&g.open, 1);
    for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
        br_job_wait(e, jobs[i]);
    }
    BR_CHECK(SDL_GetAtomicInt(&g.items) == BR_JOB_MAX);
}

static void test_no_allocation(br_engine *e)
{
    br_memory_stats before, after;
    br_memory_get_stats(&before);
    for (int i = 0; i < 20; i++) {
        br_job_wait(e, br_job_run(e, square_range, &table, ITEM_COUNT, 64));
    }
    br_memory_get_stats(&after);
    BR_CHECK(after.total_allocations == before.total_allocations);
}

static void test_deterministic(void)
{
    br_engine *e = br_engine_create("braty-test-job-deterministic", NULL);
    BR_CHECK(e != NULL);
    br_app_set_deterministic(e, true, 1);
    SDL_zero(table);
    br_job job = br_job_run(e, square_range, &table, ITEM_COUNT, 64);
    BR_CHECK(br_job_is_done(e, job)); /* finished inside br_job_run: the same on every run */
    BR_CHECK(squares_ok(&table));
    BR_CHECK(br_job_worker_count(e) == 0);
    br_engine_destroy(e);
}

int main(void)
{
    br_engine *e = br_engine_create("braty-test-job", NULL);
    BR_CHECK(e != NULL);
    test_ranges(e);
    test_does_not_block(e);
    test_cancel(e);
    test_more_jobs_than_slots(e);
    test_no_allocation(e);

    /* destroying the engine with a job pending drops it instead of hanging or crashing */
    static SDL_AtomicInt items;
    br_job_run(e, slow_range, &items, 100000, 1);
    br_engine_destroy(e);
    int after_destroy = SDL_GetAtomicInt(&items);
    SDL_Delay(20);
    BR_CHECK(SDL_GetAtomicInt(&items) == after_destroy);

    test_deterministic();
    return br_test_finish();
}
