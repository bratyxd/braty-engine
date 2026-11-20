/* test_texture - the png/jpg texture loader: sync load, worker-thread decode + main-thread finish,
 * "@2x" resolution, corrupt files on several threads at once (headless: no GPU).
 *
 * Fixtures are written at startup into <executable dir>/test_texture_data/. */
#include "braty/braty.h"
#include "br_test.h"

#include "stb/stb_image_write.h"

#include <SDL3/SDL.h>

#define DIR "test_texture_data/"

static void png_to_array(void *context, void *data, int size)
{
    br_array_push_n(context, data, (uint32_t)size);
}

/* Writes a w x h PNG. */
static void write_png(br_engine *e, const char *path, int w, int h)
{
    uint8_t pixels[32 * 32 * 4];
    for (int i = 0; i < w * h * 4; i++) {
        pixels[i] = (uint8_t)(i * 13);
    }
    br_array png;
    br_array_init(&png, 1);
    stbi_write_png_to_func(png_to_array, &png, w, h, 4, pixels, w * 4);
    br_file_write_all(e, path, png.data, png.count);
    br_array_free(&png);
}

typedef struct load_result {
    bool  done;
    void *asset;
} load_result;

static void on_done(br_engine *e, const char *path, void *asset, void *user)
{
    (void)e;
    (void)path;
    load_result *r = user;
    r->done = true;
    r->asset = asset;
}

/* Steps the engine until every result is done (or ~5 s pass). */
static bool step_until_done(br_engine *e, load_result *results, int count)
{
    for (int frame = 0; frame < 5000; frame++) {
        bool all = true;
        for (int i = 0; i < count; i++) {
            all = all && results[i].done;
        }
        if (all) {
            return true;
        }
        br_engine_step(e, 1.0f / 60.0f);
        SDL_Delay(1);
    }
    return false;
}

static bool size_is(br_engine *e, br_texture t, float w, float h)
{
    br_texture_info info;
    return br_texture_get_info(e, t, &info) && info.width == w && info.height == h;
}

int main(void)
{
    br_engine *e = br_engine_create("test_texture", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    char dir[1024];
    BR_CHECK(br_file_resolve(e, "test_texture_data", dir, sizeof(dir)));
    SDL_CreateDirectory(dir);
    write_png(e, DIR "a.png", 12, 7);
    write_png(e, DIR "hero@2x.png", 32, 16);
    for (int i = 0; i < 8; i++) {
        char path[64];
        SDL_snprintf(path, sizeof(path), DIR "bad%d.png", i);
        br_file_write_all(e, path, "\x89PNG\r\n\x1a\n not really a png", 26);
    }

    /* synchronous: decode + finish on this thread */
    BR_CHECK(size_is(e, br_texture_load(e, DIR "a.png"), 12.0f, 7.0f));
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "a.png"), "texture"));

    /* async: decode on a worker, texture created in finish on the main thread; @2x = half size */
    load_result hero = { false, NULL };
    br_assets_load_async(e, DIR "hero@2x.png", NULL, on_done, &hero);
    BR_CHECK(step_until_done(e, &hero, 1) && hero.asset != NULL);
    if (hero.asset != NULL) {
        br_texture t = *(br_texture *)hero.asset;
        br_texture_info info;
        BR_CHECK(br_texture_get_info(e, t, &info) && info.resolution == 2.0f);
        BR_CHECK(info.width == 16.0f && info.height == 8.0f && info.source_width == 32);
    }

    /* corrupt files decoded on several workers at once: each fails cleanly, nothing cached */
    load_result bad[8];
    SDL_zeroa(bad);
    for (int i = 0; i < 8; i++) {
        char path[64];
        SDL_snprintf(path, sizeof(path), DIR "bad%d.png", i);
        br_assets_load_async(e, path, NULL, on_done, &bad[i]);
    }
    BR_CHECK(step_until_done(e, bad, 8));
    for (int i = 0; i < 8; i++) {
        BR_CHECK(bad[i].asset == NULL);
    }
    BR_CHECK(br_assets_ref_count(e, DIR "bad0.png") == 0);
    /* and synchronously: magenta fallback */
    br_texture fallback = br_texture_load(e, DIR "bad0.png");
    br_texture missing = br_texture_missing(e);
    BR_CHECK(fallback.index == missing.index && fallback.generation == missing.generation);

    /* render texture sizes come from data: negative, NaN and huge sizes are refused */
    br_texture ok = br_render_texture_create(e, 64.0f, 32.0f, 1.0f);
    BR_CHECK(ok.generation != 0);
    BR_CHECK(br_render_texture_create(e, -5.0f, 10.0f, 1.0f).generation == 0);
    BR_CHECK(br_render_texture_create(e, SDL_sqrtf(-1.0f), 10.0f, 1.0f).generation == 0);
    BR_CHECK(br_render_texture_create(e, 1e9f, 10.0f, 1.0f).generation == 0);
    BR_CHECK(br_render_texture_create(e, 10.0f, 10.0f, 1e9f).generation == 0);
    BR_CHECK(!br_render_texture_resize(e, ok, -1.0f, 10.0f));
    BR_CHECK(!br_render_texture_resize(e, ok, 1e9f, 10.0f));
    BR_CHECK(size_is(e, ok, 64.0f, 32.0f));
    br_texture_destroy(e, ok);

    /* a load of the wrong type does not keep a reference */
    uint32_t refs = br_assets_ref_count(e, DIR "a.png");
    BR_CHECK(br_frame_set_load(e, DIR "a.png") == NULL);
    BR_CHECK(br_assets_ref_count(e, DIR "a.png") == refs);

    br_engine_destroy(e);
    return br_test_finish();
}
