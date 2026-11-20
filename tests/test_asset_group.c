/* test_asset_group - asset groups (headless; PNGs in tests/data/scenes): reference counts,
 * shared assets between groups (Review Focus 2), async progress + done with failures,
 * destroying a group during its async load (Review Focus 1, also from inside its callback),
 * lazy entries, manifest bundles and br_assets_bundle_failed, owners. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <string.h>

static const float DT = 1.0f / 60.0f;

static void settle(br_engine *e)
{
    for (int i = 0; i < 4000 && br_assets_pending(e) > 0; i++) {
        br_engine_step(e, DT);
        SDL_Delay(i > 20 ? 1 : 0);
    }
    br_engine_step(e, DT);
}

static void test_sync_and_shared(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    br_asset_group *a = br_asset_group_create(e, "a");
    br_asset_group *b = br_asset_group_create(e, "b");
    BR_CHECK(br_asset_group_add(a, "scenes/red.png") && br_asset_group_add(a, "scenes/shared.png"));
    BR_CHECK(br_asset_group_add(a, "scenes/red.png") && br_asset_group_count(a) == 2); /* duplicates ignored */
    BR_CHECK(br_asset_group_add(b, "scenes/shared.png"));
    BR_CHECK(!br_asset_group_is_loaded(a) && br_asset_group_progress(a) == 0.0f);
    BR_CHECK(br_asset_group_load(a) && br_asset_group_load(b));
    BR_CHECK(br_asset_group_load(a)); /* loading again takes no second reference */
    BR_CHECK(br_asset_group_is_loaded(a) && br_asset_group_progress(a) == 1.0f);
    BR_CHECK(br_assets_ref_count(e, "scenes/red.png") == 1 && br_assets_ref_count(e, "scenes/shared.png") == 2);
    const char *owners[4];
    BR_CHECK(br_assets_owners(e, "scenes/shared.png", owners, 4) == 2);
    BR_CHECK(strcmp(owners[0], "a") == 0 && strcmp(owners[1], "b") == 0);

    /* your own reference and a group's are independent */
    BR_CHECK(br_assets_load(e, "scenes/red.png") != NULL && br_assets_ref_count(e, "scenes/red.png") == 2);
    br_asset_group_destroy(a); /* Review Focus 2: shared.png stays for b */
    BR_CHECK(br_assets_get(e, "scenes/shared.png") != NULL && br_assets_ref_count(e, "scenes/shared.png") == 1);
    BR_CHECK(br_assets_get(e, "scenes/red.png") != NULL && br_assets_ref_count(e, "scenes/red.png") == 1);
    br_assets_unload(e, "scenes/red.png");
    br_asset_group_unload(b);
    BR_CHECK(!br_asset_group_is_loaded(b) && br_asset_group_count(b) == 1); /* unloaded, still listed */
    BR_CHECK(br_assets_count(e) == assets);
    BR_CHECK(br_asset_group_load(b) && br_assets_ref_count(e, "scenes/shared.png") == 1); /* reload */
    br_asset_group_destroy(b);
    BR_CHECK(br_assets_count(e) == assets);
}

typedef struct progress_log {
    int             calls;
    float           last;
    int             done;
    uint32_t        failed;
    br_asset_group *destroy_on_first; /* destroyed from inside its own callback */
} progress_log;

static void on_progress(br_engine *e, br_asset_group *g, float progress, void *user)
{
    (void)e;
    progress_log *log = user;
    log->calls++;
    log->last = progress;
    if (log->destroy_on_first == g) {
        br_asset_group_destroy(g);
        log->destroy_on_first = NULL;
    }
}

static void on_done(br_engine *e, br_asset_group *g, uint32_t failed, void *user)
{
    (void)e;
    (void)g;
    progress_log *log = user;
    log->done++;
    log->failed = failed;
}

static void test_async(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    br_asset_group *g = br_asset_group_create(e, "async");
    br_asset_group_add(g, "scenes/red.png");
    br_asset_group_add(g, "scenes/green.png");
    br_asset_group_add(g, "scenes/blue.png");
    br_asset_group_add(g, "scenes/missing.png");
    progress_log log;
    SDL_zero(log);
    br_asset_group_load_async(g, on_progress, on_done, &log);
    settle(e);
    BR_CHECK(log.done == 1 && log.failed == 1 && log.last == 1.0f && log.calls >= 1 && log.calls <= 4);
    BR_CHECK(br_asset_group_is_loaded(g) && br_asset_group_failed(g) == 1);
    BR_CHECK(br_asset_group_get(g, "scenes/blue.png") != NULL && br_asset_group_get(g, "scenes/missing.png") == NULL);
    br_asset_group_destroy(g);
    BR_CHECK(br_assets_count(e) == assets);

    /* destroyed from inside its own progress callback: no more callbacks, nothing leaks */
    g = br_asset_group_create(e, "self_destroy");
    br_asset_group_add(g, "scenes/red.png");
    br_asset_group_add(g, "scenes/green.png");
    SDL_zero(log);
    log.destroy_on_first = g;
    br_asset_group_load_async(g, on_progress, on_done, &log);
    settle(e);
    BR_CHECK(log.calls == 1 && log.done == 0);
    BR_CHECK(br_assets_count(e) == assets);

    /* Review Focus 1 at the group level: destroy or unload mid-load, 200 times */
    for (int i = 0; i < 200; i++) {
        br_asset_group *h = br_asset_group_create(e, "churn");
        br_asset_group_add(h, i % 2 == 0 ? "scenes/red.png" : "scenes/blue.png");
        br_asset_group_add(h, "scenes/shared.png");
        SDL_zero(log);
        br_asset_group_load_async(h, on_progress, on_done, &log);
        for (int f = 0; f < i % 3; f++) {
            br_engine_step(e, DT);
        }
        if (i % 4 == 0) {
            SDL_Delay(1);
        }
        if (i % 2 == 0) {
            br_asset_group_destroy(h);
        } else {
            br_asset_group_unload(h);
            int done_before = log.done;
            br_engine_step(e, DT);
            BR_CHECK(log.done == done_before); /* unloaded: no callback afterwards */
            br_asset_group_destroy(h);
        }
    }
    settle(e);
    BR_CHECK(br_assets_count(e) == assets && br_assets_pending(e) == 0);
}

static void test_lazy_bundle_capture(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    const char *manifest = "{ \"bundles\": [ { \"name\": \"colors\", \"assets\": ["
                           " { \"alias\": \"r\", \"src\": \"scenes/red.png\" },"
                           " { \"alias\": \"gone\", \"src\": \"scenes/nope.png\" } ] } ] }";
    BR_CHECK(br_assets_add_manifest(e, manifest, strlen(manifest)));
    br_asset_group *g = br_asset_group_create(e, "bundled");
    BR_CHECK(br_asset_group_add_manifest_bundle(g, "colors") && br_asset_group_count(g) == 2);
    BR_CHECK(!br_asset_group_add_manifest_bundle(g, "no_such_bundle"));
    BR_CHECK(br_asset_group_add_lazy(g, "scenes/extra.png"));
    BR_CHECK(!br_asset_group_load(g)); /* nope.png fails */
    BR_CHECK(br_asset_group_failed(g) == 1 && br_asset_group_is_loaded(g));
    BR_CHECK(br_assets_get(e, "r") != NULL && br_assets_get(e, "scenes/extra.png") == NULL);
    BR_CHECK(br_asset_group_get(g, "scenes/extra.png") != NULL && br_assets_ref_count(e, "scenes/extra.png") == 1);
    br_asset_group_destroy(g);
    BR_CHECK(br_assets_count(e) == assets);

    /* API_REVIEW item 14: a bundle load can report what failed */
    int calls = 0;
    BR_CHECK(br_assets_load_bundle(e, "colors", NULL, &calls));
    settle(e);
    BR_CHECK(br_assets_bundle_failed(e, "colors") == 1);
    const char *owners[2];
    BR_CHECK(br_assets_owners(e, "r", owners, 2) == 1 && strcmp(owners[0], "bundle:colors") == 0);
    br_assets_unload_bundle(e, "colors");
    BR_CHECK(br_assets_count(e) == assets);
}

static void test_deterministic(void)
{
    br_engine *e = br_engine_create("test_asset_group_det", "../../tests/data");
    br_app_set_deterministic(e, true, 1);
    br_asset_group *g = br_asset_group_create(e, "ordered");
    br_asset_group_add(g, "scenes/red.png");
    br_asset_group_add(g, "scenes/green.png");
    br_asset_group_add(g, "scenes/blue.png");
    progress_log log;
    SDL_zero(log);
    br_asset_group_load_async(g, on_progress, on_done, &log);
    BR_CHECK(br_assets_get(e, "scenes/red.png") == NULL); /* nothing before the first frame */
    br_engine_step(e, DT);
    BR_CHECK(br_assets_get(e, "scenes/red.png") != NULL && br_assets_get(e, "scenes/green.png") == NULL);
    BR_CHECK(log.calls == 1 && log.last > 0.3f && log.last < 0.4f);
    br_engine_step(e, DT);
    BR_CHECK(br_assets_get(e, "scenes/green.png") != NULL && br_assets_get(e, "scenes/blue.png") == NULL);
    br_engine_step(e, DT);
    BR_CHECK(log.calls == 3 && log.done == 1 && log.failed == 0);
    br_engine_destroy(e); /* the group is freed at shutdown */
}

/* ---- a failed async load, then another owner loads the path in the same frame */

typedef struct other_owner {
    bool  done;
    void *asset;
} other_owner;

/* Waiter of the failed load: the file appears and someone else loads it synchronously. */
static void load_after_failure(br_engine *e, const char *path, void *asset, void *user)
{
    (void)path;
    (void)asset;
    other_owner *o = user;
    o->done = true;
    br_file_write_all(e, "group_appears.txt", "hi", 2);
    o->asset = br_assets_load(e, "group_appears.txt");
}

static void remove_fixture(br_engine *e, const char *name)
{
    char full[1024];
    if (br_file_resolve(e, name, full, sizeof(full))) {
        SDL_RemovePath(full);
    }
}

static void test_failure_then_other_owner(br_engine *e)
{
    remove_fixture(e, "group_appears.txt");
    br_asset_group *g = br_asset_group_create(e, "appear");
    BR_CHECK(br_asset_group_add(g, "group_appears.txt"));
    br_asset_group_load_async(g, NULL, NULL, NULL); /* fails: no such file yet */
    other_owner o = { false, NULL };
    br_assets_load_async(e, "group_appears.txt", NULL, load_after_failure, &o);
    for (int i = 0; i < 4000 && !o.done; i++) {
        br_engine_step(e, DT);
        SDL_Delay(1);
    }
    settle(e);
    BR_CHECK(o.done && o.asset != NULL);
    BR_CHECK(br_asset_group_failed(g) == 1); /* the group's own load failed */
    BR_CHECK(br_assets_ref_count(e, "group_appears.txt") == 1);
    br_asset_group_destroy(g); /* must not drop the other owner's reference */
    BR_CHECK(br_assets_get(e, "group_appears.txt") != NULL);
    BR_CHECK(br_assets_ref_count(e, "group_appears.txt") == 1);
    br_assets_unload(e, "group_appears.txt");
    remove_fixture(e, "group_appears.txt");
}

int main(void)
{
    br_engine *e = br_engine_create("test_asset_group", "../../tests/data"); /* relative to the exe */
    test_sync_and_shared(e);
    test_async(e);
    test_lazy_bundle_capture(e);
    test_failure_then_other_owner(e);
    br_engine_destroy(e);
    test_deterministic();
    return br_test_finish();
}
