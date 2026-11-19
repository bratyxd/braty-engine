/* test_core: array, pool, map, strings, files, save data, systems and ticker (headless). */
#include "br_test.h"

#include "braty/br_app.h"

#include <SDL3/SDL.h>

static void test_array(void)
{
    br_array a;
    br_array_init(&a, sizeof(int));
    for (int i = 0; i < 100; i++) {
        br_array_push(&a, &i);
    }
    BR_CHECK(a.count == 100);
    BR_CHECK(*(int *)br_array_at(&a, 42) == 42);
    BR_CHECK(br_array_at(&a, 100) == NULL);

    br_array_remove_swap(&a, 0); /* last (99) moves to 0 */
    BR_CHECK(a.count == 99);
    BR_CHECK(*(int *)br_array_at(&a, 0) == 99);

    br_array_remove_ordered(&a, 0); /* now 1, 2, 3 ... */
    BR_CHECK(*(int *)br_array_at(&a, 0) == 1);

    int minus = -1;
    br_array_insert(&a, 0, &minus);
    BR_CHECK(*(int *)br_array_at(&a, 0) == -1);
    BR_CHECK(*(int *)br_array_at(&a, 1) == 1);

    br_array_resize(&a, 200);
    BR_CHECK(*(int *)br_array_at(&a, 199) == 0); /* grown elements are zeroed */

    br_array_pop(&a);
    BR_CHECK(a.count == 199);
    br_array_clear(&a);
    BR_CHECK(a.count == 0);
    br_array_free(&a);
    BR_CHECK(a.data == NULL);
}

static void test_pool(void)
{
    br_pool p;
    br_pool_init(&p, sizeof(float));
    uint32_t i0, g0, i1, g1;
    float *f0 = br_pool_alloc(&p, &i0, &g0);
    *f0 = 1.5f;
    float *f1 = br_pool_alloc(&p, &i1, &g1);
    *f1 = 2.5f;
    BR_CHECK(g0 != 0 && g1 != 0);
    BR_CHECK(i0 != i1);
    BR_CHECK(*(float *)br_pool_get(&p, i0, g0) == 1.5f);
    BR_CHECK(p.alive_count == 2);

    BR_CHECK(br_pool_release(&p, i0, g0));
    BR_CHECK(br_pool_get(&p, i0, g0) == NULL);      /* stale handle */
    BR_CHECK(!br_pool_release(&p, i0, g0));          /* double free is refused */
    BR_CHECK(br_pool_get(&p, i0, 0) == NULL);        /* generation 0 is never valid */
    BR_CHECK(br_pool_get(&p, 1000, 1) == NULL);      /* out of range */

    uint32_t i2, g2;
    float *f2 = br_pool_alloc(&p, &i2, &g2);
    BR_CHECK(i2 == i0);                              /* slot reused ... */
    BR_CHECK(g2 != g0);                              /* ... with a new generation */
    BR_CHECK(*f2 == 0.0f);                           /* and zeroed */
    BR_CHECK(br_pool_get(&p, i0, g0) == NULL);
    BR_CHECK(br_pool_slot_alive(&p, i1));
    BR_CHECK(br_pool_capacity(&p) == 2);
    br_pool_free(&p);
}

static void test_map(void)
{
    br_map m;
    br_map_init(&m, sizeof(int));
    char key[32];
    for (int i = 0; i < 1000; i++) {
        SDL_snprintf(key, sizeof(key), "key%d", i);
        br_map_set(&m, key, &i);
    }
    BR_CHECK(m.count == 1000);
    BR_CHECK(*(int *)br_map_get(&m, "key500") == 500);
    BR_CHECK(br_map_get(&m, "missing") == NULL);

    int seven = 7;
    br_map_set(&m, "key500", &seven);                /* overwrite keeps count */
    BR_CHECK(m.count == 1000);
    BR_CHECK(*(int *)br_map_get(&m, "key500") == 7);

    /* remove every even key; every odd key must still be found (backward-shift correctness) */
    for (int i = 0; i < 1000; i += 2) {
        SDL_snprintf(key, sizeof(key), "key%d", i);
        BR_CHECK(br_map_remove(&m, key));
    }
    BR_CHECK(!br_map_remove(&m, "key0"));
    BR_CHECK(m.count == 500);
    int found = 0;
    for (int i = 1; i < 1000; i += 2) {
        SDL_snprintf(key, sizeof(key), "key%d", i);
        int *v = br_map_get(&m, key);
        if (v != NULL && *v == i) {
            found++;
        }
    }
    BR_CHECK(found == 500);

    uint32_t slot = 0;
    const char *k;
    void *v;
    int iterated = 0;
    while (br_map_next(&m, &slot, &k, &v)) {
        iterated++;
    }
    BR_CHECK(iterated == 500);
    br_map_clear(&m);
    BR_CHECK(m.count == 0 && br_map_get(&m, "key1") == NULL);
    br_map_free(&m);
}

static void test_strings(void)
{
    char *dup = br_strdup("hello");
    BR_CHECK(br_str_equal(dup, "hello"));
    br_free(dup);
    BR_CHECK(br_str_equal(NULL, NULL));
    BR_CHECK(!br_str_equal("a", NULL));
    BR_CHECK(br_str_starts_with("sprite.png", "sprite"));
    BR_CHECK(br_str_ends_with("sprite.png", ".png"));
    BR_CHECK(!br_str_ends_with("png", "sprite.png"));

    char small[4];
    BR_CHECK(!br_str_copy(small, sizeof(small), "toolong"));
    BR_CHECK(br_str_equal(small, "too"));

    char path[64];
    BR_CHECK(br_path_join(path, sizeof(path), "assets", "img/a.png"));
    BR_CHECK(br_str_equal(path, "assets/img/a.png"));
    BR_CHECK(br_path_join(path, sizeof(path), "assets/", "/a.png"));
    BR_CHECK(br_str_equal(path, "assets/a.png"));
    BR_CHECK(br_str_equal(br_path_extension("a/b.c/file.json"), "json"));
    BR_CHECK(br_str_equal(br_path_extension("a.b/file"), ""));
    BR_CHECK(br_path_is_absolute("/usr") && br_path_is_absolute("C:/x") && !br_path_is_absolute("rel/x"));
    BR_CHECK(br_str_hash("a") != br_str_hash("b"));
}

static void test_files(br_engine *e)
{
    const char *text = "braty file test";
    BR_CHECK(br_file_write_all(e, "test_core_file.txt", text, SDL_strlen(text)));
    BR_CHECK(br_file_exists(e, "test_core_file.txt"));
    void *bytes = NULL;
    size_t size = 0;
    BR_CHECK(br_file_read_all(e, "test_core_file.txt", &bytes, &size));
    BR_CHECK(size == SDL_strlen(text));
    BR_CHECK(bytes != NULL && br_str_equal(bytes, text)); /* NUL-terminated */
    br_free(bytes);
    char full[1024];
    BR_CHECK(br_file_resolve(e, "test_core_file.txt", full, sizeof(full)));
    BR_CHECK(SDL_RemovePath(full));

    /* missing file: returns false, logs, does not crash */
    BR_CHECK(!br_file_read_all(e, "does/not/exist.bin", &bytes, &size));
    BR_CHECK(bytes == NULL && size == 0);
    BR_CHECK(!br_file_exists(e, "does/not/exist.bin"));

    /* save data round trip */
    const char *save = "{\"score\":42}";
    BR_CHECK(br_save_write(e, "slot1.json", save, SDL_strlen(save)));
    BR_CHECK(br_save_read(e, "slot1.json", &bytes, &size));
    BR_CHECK(bytes != NULL && br_str_equal(bytes, save));
    br_free(bytes);
    BR_CHECK(br_save_remove(e, "slot1.json"));
    BR_CHECK(!br_save_read(e, "slot1.json", &bytes, &size));
}

/* ---- systems: order by phase, then priority, then adding order */

typedef struct order_log {
    char text[64];
} order_log;

static void append(order_log *log, const char *s)
{
    SDL_strlcat(log->text, s, sizeof(log->text));
}

static void sys_a(br_engine *e, float dt, void *user) { (void)e; (void)dt; append(user, "a"); }
static void sys_b(br_engine *e, float dt, void *user) { (void)e; (void)dt; append(user, "b"); }
static void sys_c(br_engine *e, float dt, void *user) { (void)e; (void)dt; append(user, "c"); }
static void sys_d(br_engine *e, float dt, void *user) { (void)e; (void)dt; append(user, "d"); }
static void sys_r(br_engine *e, float dt, void *user) { (void)e; (void)dt; append(user, "R"); }

static void sys_adder(br_engine *e, float dt, void *user)
{
    (void)dt;
    append(user, "+");
    /* adding while systems run must be safe; the new one runs from the next frame */
    br_system_add(e, "late_added", BR_PHASE_INPUT, -100, sys_d, user);
}

static int shutdown_calls = 0;
static void on_shutdown(br_engine *e, void *user)
{
    (void)e;
    (void)user;
    shutdown_calls++;
}

static void test_systems(void)
{
    br_engine *e = br_engine_create("braty-test", NULL);
    BR_CHECK(e != NULL);
    order_log log = { { 0 } };
    br_system_add(e, "c", BR_PHASE_LATE_UPDATE, 0, sys_c, &log);
    br_system_add(e, "b2", BR_PHASE_UPDATE, 5, sys_b, &log);
    br_system_add(e, "a", BR_PHASE_UPDATE, -5, sys_a, &log);
    br_system_add(e, "b1", BR_PHASE_UPDATE, 5, sys_b, &log);
    br_system_add(e, "render", BR_PHASE_RENDER, 0, sys_r, &log);
    br_system_add(e, "adder", BR_PHASE_INPUT, 0, sys_adder, &log);
    br_system_add(e, "a", BR_PHASE_UPDATE, 0, sys_a, &log); /* duplicate name: refused */
    BR_CHECK(br_system_count(e) == 6);

    /* headless: RENDER phase does not run, so no "R" */
    BR_CHECK(!br_engine_step(e, 0.016f));
    BR_CHECK(br_str_equal(log.text, "+abbc"));
    BR_CHECK(br_system_count(e) == 7);

    log.text[0] = 0;
    br_system_set_enabled(e, "b1", false);
    br_engine_step(e, 0.016f);
    BR_CHECK(br_str_equal(log.text, "d+abc")); /* late_added (priority -100) first, b1 off */

    br_system_info info;
    BR_CHECK(br_system_get_info(e, 0, &info) && br_str_equal(info.name, "late_added"));
    BR_CHECK(!br_system_get_info(e, 99, &info));

    BR_CHECK(br_ticker_frame(e) == 2);
    BR_CHECK(br_nearly_equal((float)br_ticker_elapsed(e), 0.032f, 1e-6f));
    BR_CHECK(br_nearly_equal(br_ticker_delta(e), 0.016f, 1e-6f));

    br_engine_on_shutdown(e, on_shutdown, NULL);
    br_engine_on_shutdown(e, on_shutdown, NULL);
    test_files(e);
    br_engine_destroy(e);
    BR_CHECK(shutdown_calls == 2);
}

static void test_ticker(void)
{
    BR_CHECK(br_nearly_equal(br_ticker_compute_delta(0.016f, 10.0f, 1.0f), 0.016f, 1e-6f));
    BR_CHECK(br_nearly_equal(br_ticker_compute_delta(5.0f, 10.0f, 1.0f), 0.1f, 1e-6f));   /* clamped */
    BR_CHECK(br_nearly_equal(br_ticker_compute_delta(5.0f, 0.0f, 1.0f), 5.0f, 1e-6f));    /* no clamp */
    BR_CHECK(br_nearly_equal(br_ticker_compute_delta(0.02f, 10.0f, 0.5f), 0.01f, 1e-6f)); /* slow-mo */
    BR_CHECK(br_ticker_compute_delta(-1.0f, 10.0f, 1.0f) == 0.0f);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_array();
    test_pool();
    test_map();
    test_strings();
    test_systems();
    test_ticker();
    return br_test_finish();
}
