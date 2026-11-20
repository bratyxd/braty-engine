/* test_assets - resolver, manifests, spritesheets, compressed textures, corrupt and missing
 * files, reference counting and background loading (headless: no window, no GPU).
 *
 * Fixtures are written at startup into <executable dir>/test_assets_data/. */
#include "braty/braty.h"
#include "br_test.h"

#include "stb/stb_image_write.h"

#include <SDL3/SDL.h>

#define DIR "test_assets_data/"

static bool write_file(br_engine *e, const char *path, const void *bytes, size_t size)
{
    return br_file_write_all(e, path, bytes, size);
}

static bool write_text(br_engine *e, const char *path, const char *text)
{
    return write_file(e, path, text, SDL_strlen(text));
}

typedef struct png_buffer {
    uint8_t data[8192];
    size_t  size;
} png_buffer;

static void png_append(void *context, void *data, int size)
{
    png_buffer *b = context;
    if (b->size + (size_t)size <= sizeof(b->data)) {
        SDL_memcpy(b->data + b->size, data, (size_t)size);
        b->size += (size_t)size;
    }
}

/* Writes a w x h opaque PNG. */
static void write_png(br_engine *e, const char *path, int w, int h)
{
    uint8_t pixels[64 * 64 * 4];
    for (int i = 0; i < w * h; i++) {
        pixels[i * 4 + 0] = (uint8_t)(i * 7);
        pixels[i * 4 + 1] = (uint8_t)(i * 3);
        pixels[i * 4 + 2] = 200;
        pixels[i * 4 + 3] = 255;
    }
    png_buffer b;
    b.size = 0;
    stbi_write_png_to_func(png_append, &b, w, h, 4, pixels, w * 4);
    write_file(e, path, b.data, b.size);
}

static void put_u32(uint8_t *p, size_t at, uint32_t v)
{
    p[at] = (uint8_t)v;
    p[at + 1] = (uint8_t)(v >> 8);
    p[at + 2] = (uint8_t)(v >> 16);
    p[at + 3] = (uint8_t)(v >> 24);
}

/* A DDS with one level of `size`x`size` BC1 (legacy DXT1 header), or BC7 behind a DX10 header. */
static size_t make_dds(uint8_t *out, uint32_t size, bool dx10)
{
    SDL_memset(out, 0, 148 + 4096);
    SDL_memcpy(out, "DDS ", 4);
    put_u32(out, 4, 124);
    put_u32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000);
    put_u32(out, 12, size);
    put_u32(out, 16, size);
    put_u32(out, 76, 32);
    put_u32(out, 80, 0x4);
    SDL_memcpy(out + 84, dx10 ? "DX10" : "DXT1", 4);
    size_t at = 128;
    if (dx10) {
        put_u32(out, 128, 98); /* BC7 */
        put_u32(out, 132, 3);  /* TEXTURE2D */
        put_u32(out, 140, 1);
        at = 148;
    }
    uint32_t blocks = (size / 4) * (size / 4);
    return at + blocks * (dx10 ? 16u : 8u);
}

/* A KTX 1 file of `size`x`size` with glInternalFormat `format` (BC1 = 0x83F1), one level. */
static size_t make_ktx(uint8_t *out, uint32_t size, uint32_t format)
{
    static const uint8_t magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    SDL_memset(out, 0, 64 + 4 + 4096);
    SDL_memcpy(out, magic, 12);
    put_u32(out, 12, 0x04030201);
    put_u32(out, 28, format);
    put_u32(out, 36, size);
    put_u32(out, 40, size);
    put_u32(out, 52, 1);
    put_u32(out, 56, 1);
    uint32_t bytes = (size / 4) * (size / 4) * 8;
    put_u32(out, 64, bytes);
    return 64 + 4 + bytes;
}

/* Steps the engine until `done` is set (or ~5 s pass). */
static bool step_until(br_engine *e, const bool *done)
{
    for (int i = 0; i < 5000 && !*done; i++) {
        br_engine_step(e, 1.0f / 60.0f);
        if (!*done) {
            SDL_Delay(1);
        }
    }
    return *done;
}

static void step_until_idle(br_engine *e)
{
    for (int i = 0; i < 5000 && br_assets_pending(e) > 0; i++) {
        br_engine_step(e, 1.0f / 60.0f);
        SDL_Delay(1);
    }
    br_engine_step(e, 1.0f / 60.0f); /* fire the waiters of the last results */
}

static bool texture_rect_equal(br_rect r, float x, float y, float w, float h)
{
    return r.x == x && r.y == y && r.width == w && r.height == h;
}

/* ---- resolver */

static void test_resolver(br_engine *e)
{
    BR_CHECK(br_assets_resolution_of("img/hero@2x.png") == 2.0f);
    BR_CHECK(br_assets_resolution_of("hero.png") == 1.0f);
    BR_CHECK(br_assets_resolution_of("hero@1.5x.json") == 1.5f);
    BR_CHECK(br_assets_resolution_of("a@2x/hero.png") == 1.0f);
    BR_CHECK(br_assets_resolution_of("mail@home.png") == 1.0f);

    char out[256];
    BR_CHECK(br_assets_resolve(e, "hero@{1,2}x.png", out, sizeof(out)) && br_str_equal(out, "hero@1x.png"));
    br_assets_set_preferences(e, 2.0f, NULL);
    BR_CHECK(br_assets_resolve(e, "hero@{1,2}x.png", out, sizeof(out)) && br_str_equal(out, "hero@2x.png"));
    br_assets_set_preferences(e, 1.0f, "dds,png");
    BR_CHECK(br_assets_resolve(e, "tex.{png,dds}", out, sizeof(out)) && br_str_equal(out, "tex.dds"));
    br_assets_set_preferences(e, 2.0f, "png,dds");
    BR_CHECK(br_assets_resolve(e, "t@{1,2}x.{dds,png}", out, sizeof(out)) && br_str_equal(out, "t@2x.png"));
    br_assets_set_preferences(e, 1.0f, NULL);
    BR_CHECK(br_assets_resolve(e, "plain.png", out, sizeof(out)) && br_str_equal(out, "plain.png"));

    br_assets_add_alias(e, "hero", "images/hero.png");
    BR_CHECK(br_assets_resolve(e, "hero", out, sizeof(out)) && br_str_equal(out, "images/hero.png"));
    br_assets_set_base_path(e, "pack");
    BR_CHECK(br_assets_resolve(e, "x.png", out, sizeof(out)) && br_str_equal(out, "pack/x.png"));
    BR_CHECK(br_assets_resolve(e, "hero", out, sizeof(out)) && br_str_equal(out, "images/hero.png"));
    br_assets_set_base_path(e, NULL);
    BR_CHECK(br_assets_resolve(e, "x.png", out, sizeof(out)) && br_str_equal(out, "x.png"));
    char tiny[4];
    BR_CHECK(!br_assets_resolve(e, "too_long.png", tiny, sizeof(tiny)));
}

/* ---- manifests */

static void test_manifest(br_engine *e)
{
    const char *manifest =
        "{ \"bundles\": ["
        "  { \"name\": \"load\", \"assets\": ["
        "     { \"alias\": [\"logo\", \"brand\"], \"src\": [\"m/logo@2x.png\", \"m/logo.png\"] },"
        "     { \"alias\": \"sheet\", \"src\": \"m/sheet.json\", \"type\": \"spritesheet\" },"
        "     { \"src\": \"m/noalias.txt\" } ] },"
        "  { \"name\": \"short\", \"assets\": { \"music\": \"m/song.{ogg,wav}\" } } ] }";
    BR_CHECK(br_assets_add_manifest(e, manifest, SDL_strlen(manifest)));
    BR_CHECK(br_assets_bundle_size(e, "load") == 3);
    BR_CHECK(br_assets_bundle_size(e, "short") == 1);
    BR_CHECK(br_assets_bundle_size(e, "nope") == 0);
    BR_CHECK(br_str_equal(br_assets_bundle_path(e, "load", 0), "m/logo.png")); /* resolution 1 preferred */
    BR_CHECK(br_str_equal(br_assets_bundle_path(e, "load", 2), "m/noalias.txt"));
    BR_CHECK(br_assets_bundle_path(e, "load", 3) == NULL);
    BR_CHECK(br_str_equal(br_assets_bundle_path(e, "short", 0), "m/song.ogg"));
    char out[256];
    BR_CHECK(br_assets_resolve(e, "brand", out, sizeof(out)) && br_str_equal(out, "m/logo.png"));
    BR_CHECK(br_assets_resolve(e, "music", out, sizeof(out)) && br_str_equal(out, "m/song.ogg"));

    const char *bad = "{ \"bundles\": [ { \"name\": ";
    BR_CHECK(!br_assets_add_manifest(e, bad, SDL_strlen(bad)));
    const char *no_bundles = "{ \"assets\": [] }";
    BR_CHECK(!br_assets_add_manifest(e, no_bundles, SDL_strlen(no_bundles)));
    BR_CHECK(!br_assets_load_manifest(e, DIR "missing_manifest.json"));
    BR_CHECK(!br_assets_load_bundle(e, "nope", NULL, NULL));
}

/* ---- spritesheets */

static const char *g_sheet_hash =
    "{ \"frames\": {"
    "  \"plain.png\":   { \"frame\": {\"x\":0,\"y\":0,\"w\":16,\"h\":16}, \"rotated\": false, \"trimmed\": false,"
    "                     \"spriteSourceSize\": {\"x\":0,\"y\":0,\"w\":16,\"h\":16}, \"sourceSize\": {\"w\":16,\"h\":16},"
    "                     \"pivot\": {\"x\":0.5,\"y\":1} },"
    "  \"trim.png\":    { \"frame\": {\"x\":16,\"y\":0,\"w\":10,\"h\":12}, \"rotated\": false, \"trimmed\": true,"
    "                     \"spriteSourceSize\": {\"x\":3,\"y\":2,\"w\":10,\"h\":12}, \"sourceSize\": {\"w\":16,\"h\":16} },"
    "  \"rot.png\":     { \"frame\": {\"x\":32,\"y\":0,\"w\":20,\"h\":8}, \"rotated\": true, \"trimmed\": false,"
    "                     \"spriteSourceSize\": {\"x\":0,\"y\":0,\"w\":20,\"h\":8}, \"sourceSize\": {\"w\":20,\"h\":8} },"
    "  \"nine.png\":    { \"frame\": {\"x\":0,\"y\":16,\"w\":16,\"h\":16}, \"borders\": {\"left\":4,\"top\":5,\"right\":6,\"bottom\":7} }"
    "  },"
    "  \"animations\": { \"blink\": [\"plain.png\", \"trim.png\", \"not_there.png\"] },"
    "  \"meta\": { \"image\": \"atlas.png\", \"size\": {\"w\":64,\"h\":32}, \"scale\": \"1\" } }";

static void test_spritesheet(br_engine *e)
{
    write_png(e, DIR "atlas.png", 64, 32);
    write_text(e, DIR "sheet.json", g_sheet_hash);
    br_spritesheet *sheet = br_spritesheet_load(e, DIR "sheet.json");
    BR_CHECK(sheet != NULL);
    if (sheet == NULL) {
        return;
    }
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "sheet.json"), "spritesheet"));
    BR_CHECK(br_assets_ref_count(e, DIR "atlas.png") == 1); /* the sheet's reference */
    BR_CHECK(br_spritesheet_frame_count(sheet) == 4);
    BR_CHECK(br_str_equal(br_spritesheet_frame_name(sheet, 0), "plain.png"));
    BR_CHECK(br_spritesheet_frame_name(sheet, 4) == NULL);

    br_texture_info info;
    BR_CHECK(br_texture_get_info(e, br_spritesheet_frame(e, sheet, "plain.png"), &info));
    BR_CHECK(texture_rect_equal(info.frame.frame, 0, 0, 16, 16));
    BR_CHECK(info.frame.default_anchor.x == 0.5f && info.frame.default_anchor.y == 1.0f);
    BR_CHECK(info.frame.trim.width == 0.0f); /* not trimmed */
    BR_CHECK(info.source_width == 64 && info.source_height == 32);

    BR_CHECK(br_texture_get_info(e, br_spritesheet_frame(e, sheet, "trim.png"), &info));
    BR_CHECK(texture_rect_equal(info.frame.frame, 16, 0, 10, 12));
    BR_CHECK(texture_rect_equal(info.frame.trim, 3, 2, 10, 12));
    BR_CHECK(info.frame.orig.width == 16.0f && info.frame.orig.height == 16.0f);
    BR_CHECK(info.width == 16.0f && info.height == 16.0f); /* logical size = untrimmed size */

    BR_CHECK(br_texture_get_info(e, br_spritesheet_frame(e, sheet, "rot.png"), &info));
    BR_CHECK(info.frame.rotated);
    BR_CHECK(texture_rect_equal(info.frame.frame, 32, 0, 20, 8)); /* unrotated size; 8x20 in the atlas */
    BR_CHECK(info.width == 20.0f && info.height == 8.0f);
    BR_CHECK(info.uvs[0].x == (32.0f + 8.0f) / 64.0f && info.uvs[0].y == 0.0f);   /* top-left -> region's top-right */
    BR_CHECK(info.uvs[2].x == 32.0f / 64.0f && info.uvs[2].y == 20.0f / 32.0f); /* bottom-right -> bottom-left */

    float borders[4] = { 0, 0, 0, 0 };
    BR_CHECK(br_spritesheet_frame_borders(e, sheet, "nine.png", borders));
    BR_CHECK(borders[0] == 4.0f && borders[1] == 5.0f && borders[2] == 6.0f && borders[3] == 7.0f);
    BR_CHECK(!br_spritesheet_frame_borders(e, sheet, "plain.png", borders));
    BR_CHECK(br_spritesheet_frame(e, sheet, "nope.png").generation == 0);

    const br_texture *frames = NULL;
    BR_CHECK(br_spritesheet_animation(e, sheet, "blink", &frames) == 2); /* missing frame skipped */
    BR_CHECK(frames != NULL && frames[1].index == br_spritesheet_frame(e, sheet, "trim.png").index);
    BR_CHECK(br_spritesheet_animation(e, sheet, "nope", &frames) == 0 && frames == NULL);

    /* loading again is a cache hit with one more reference */
    BR_CHECK(br_spritesheet_load(e, DIR "sheet.json") == sheet);
    BR_CHECK(br_assets_ref_count(e, DIR "sheet.json") == 2);
    br_assets_unload(e, DIR "sheet.json");
    br_assets_unload(e, DIR "sheet.json");
    BR_CHECK(br_assets_get(e, DIR "sheet.json") == NULL);
    BR_CHECK(br_assets_get(e, DIR "atlas.png") == NULL); /* the image went with it */

    /* array format, @2x resolution from the file name */
    write_text(e, DIR "arr@2x.json",
               "{ \"frames\": [ { \"filename\": \"a\", \"frame\": {\"x\":0,\"y\":0,\"w\":8,\"h\":8} },"
               "                { \"filename\": \"b\", \"frame\": {\"x\":8,\"y\":0,\"w\":8,\"h\":4}, \"rotated\": true } ],"
               "  \"meta\": { \"image\": \"atlas.png\" } }");
    sheet = br_spritesheet_load(e, DIR "arr@2x.json");
    BR_CHECK(sheet != NULL && br_spritesheet_frame_count(sheet) == 2);
    BR_CHECK(br_texture_get_info(e, br_spritesheet_frame(e, sheet, "a"), &info));
    BR_CHECK(info.resolution == 2.0f && info.width == 4.0f);
    BR_CHECK(br_texture_get_info(e, br_spritesheet_frame(e, sheet, "b"), &info) && info.frame.rotated);
    br_assets_unload(e, DIR "arr@2x.json");

    /* multi-pack: the packs list each other; frames of either are found through the first */
    write_png(e, DIR "pack1.png", 16, 16);
    write_text(e, DIR "pack0.json",
               "{ \"frames\": { \"p0\": { \"frame\": {\"x\":0,\"y\":0,\"w\":8,\"h\":8} } },"
               "  \"animations\": { \"both\": [\"p0\", \"p1\"] },"
               "  \"meta\": { \"image\": \"atlas.png\", \"related_multi_packs\": [\"pack1.json\"] } }");
    write_text(e, DIR "pack1.json",
               "{ \"frames\": { \"p1\": { \"frame\": {\"x\":0,\"y\":0,\"w\":8,\"h\":8} } },"
               "  \"meta\": { \"image\": \"pack1.png\", \"related_multi_packs\": [\"pack0.json\"] } }");
    sheet = br_spritesheet_load(e, DIR "pack0.json");
    BR_CHECK(sheet != NULL);
    BR_CHECK(br_spritesheet_frame(e, sheet, "p1").generation != 0);
    BR_CHECK(br_spritesheet_animation(e, sheet, "both", &frames) == 2);
    BR_CHECK(br_assets_ref_count(e, DIR "pack1.json") == 1);
    br_assets_unload(e, DIR "pack0.json");
    BR_CHECK(br_assets_count(e) == 0);

    /* a plain .json is not claimed by the spritesheet loader */
    write_text(e, DIR "plain.json", "{ \"frames\": 3 }");
    BR_CHECK(br_assets_load(e, DIR "plain.json") != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "plain.json"), "json"));
    BR_CHECK(br_spritesheet_load(e, DIR "plain.json") == NULL); /* cached as json: refused */
    BR_CHECK(br_assets_ref_count(e, DIR "plain.json") == 1);
    br_assets_unload(e, DIR "plain.json");
}

/* ---- compressed textures and failures */

static void test_compressed_and_failures(br_engine *e)
{
    static uint8_t buf[148 + 4096];
    size_t size = make_dds(buf, 8, false);
    write_file(e, DIR "bc1.dds", buf, size);
    br_texture t = br_texture_load(e, DIR "bc1.dds");
    br_texture_info info;
    BR_CHECK(br_texture_get_info(e, t, &info) && info.width == 8.0f && info.height == 8.0f);
    BR_CHECK(t.index != br_texture_missing(e).index);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "bc1.dds"), "texture"));
    br_asset_info list[8];
    BR_CHECK(br_assets_list(e, list, 8) == 1 && list[0].byte_size == 32); /* 4 BC1 blocks */
    br_assets_unload(e, DIR "bc1.dds");

    size = make_dds(buf, 16, true);
    write_file(e, DIR "bc7.dds", buf, size);
    BR_CHECK(br_assets_load(e, DIR "bc7.dds") != NULL);
    br_assets_unload(e, DIR "bc7.dds");
    write_file(e, DIR "truncated.dds", buf, size - 1);
    BR_CHECK(br_assets_load(e, DIR "truncated.dds") == NULL);
    put_u32(buf, 128, 81); /* BC4 SNORM: no SDL format */
    write_file(e, DIR "snorm.dds", buf, size);
    BR_CHECK(br_assets_load(e, DIR "snorm.dds") == NULL);
    write_text(e, DIR "garbage.dds", "this is not a dds file at all, just text that is long enough to have a header....."
                                     "..................................................................................");
    BR_CHECK(br_assets_load(e, DIR "garbage.dds") == NULL);
    size = make_dds(buf, 6, false); /* not a multiple of the 4x4 block */
    write_file(e, DIR "odd.dds", buf, size);
    BR_CHECK(br_assets_load(e, DIR "odd.dds") == NULL);

    size = make_ktx(buf, 8, 0x83F1);
    write_file(e, DIR "bc1.ktx", buf, size);
    BR_CHECK(br_assets_load(e, DIR "bc1.ktx") != NULL);
    br_assets_unload(e, DIR "bc1.ktx");
    size = make_ktx(buf, 8, 0x9278); /* ETC2: logged as unsupported */
    write_file(e, DIR "etc2.ktx", buf, size);
    BR_CHECK(br_assets_load(e, DIR "etc2.ktx") == NULL);

    /* missing and corrupt files: NULL, nothing cached; textures fall back to magenta */
    BR_CHECK(br_assets_load(e, DIR "nope.json") == NULL);
    BR_CHECK(br_assets_load(e, DIR "file.unknownext") == NULL);
    write_text(e, DIR "broken.json", "{ \"a\": [1, 2");
    BR_CHECK(br_assets_load(e, DIR "broken.json") == NULL);
    write_text(e, DIR "broken.png", "not a png");
    BR_CHECK(br_assets_load(e, DIR "broken.png") == NULL);
    BR_CHECK(br_texture_load(e, DIR "broken.png").index == br_texture_missing(e).index);
    BR_CHECK(br_texture_load(e, DIR "nope.png").index == br_texture_missing(e).index);
    write_text(e, DIR "noimage.json", "{ \"frames\": {}, \"meta\": { \"image\": \"absent.png\" } }");
    BR_CHECK(br_spritesheet_load(e, DIR "noimage.json") == NULL);
    BR_CHECK(br_assets_count(e) == 0);

    /* a 4x4 BC1 claiming 16 mip levels: clamped to its chain (4x4, 2x2, 1x1 = 3 blocks) */
    size = make_dds(buf, 4, false);
    put_u32(buf, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000); /* DDSD_MIPMAPCOUNT */
    put_u32(buf, 28, 16);
    write_file(e, DIR "mips.dds", buf, size + 15 * 8); /* data for all 16 claimed levels */
    BR_CHECK(br_assets_load(e, DIR "mips.dds") != NULL);
    BR_CHECK(br_assets_list(e, list, 8) == 1 && list[0].byte_size == 24);
    br_assets_unload(e, DIR "mips.dds");

    /* a 128-byte header claiming 16384x16384 RGBA8 with 15 mips: fails before allocating */
    SDL_memset(buf, 0, 128);
    SDL_memcpy(buf, "DDS ", 4);
    put_u32(buf, 4, 124);
    put_u32(buf, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
    put_u32(buf, 12, 16384);
    put_u32(buf, 16, 16384);
    put_u32(buf, 28, 15);
    put_u32(buf, 76, 32);
    put_u32(buf, 80, 0x40); /* DDPF_RGB */
    put_u32(buf, 88, 32);
    put_u32(buf, 92, 0x000000ff);
    write_file(e, DIR "huge.dds", buf, 128);
    br_memory_stats before;
    br_memory_stats after;
    br_memory_get_stats(&before);
    BR_CHECK(br_assets_load(e, DIR "huge.dds") == NULL);
    br_memory_get_stats(&after);
    BR_CHECK(after.peak_bytes - before.peak_bytes < 16u * 1024u * 1024u);
    BR_CHECK(br_assets_count(e) == 0);
}

/* ---- reference counting */

static void test_ref_counting(br_engine *e)
{
    write_text(e, DIR "a.json", "{ \"value\": 7 }");
    write_text(e, DIR "b.txt", "hello");
    write_text(e, DIR "c.bin", "\x01\x02\x03");
    void *a = br_assets_load(e, DIR "a.json");
    BR_CHECK(a != NULL);
    BR_CHECK(br_assets_load(e, DIR "a.json") == a);
    BR_CHECK(br_assets_ref_count(e, DIR "a.json") == 2);
    br_assets_add_alias(e, "a", DIR "a.json");
    BR_CHECK(br_assets_load(e, "a") == a && br_assets_ref_count(e, "a") == 3);
    br_assets_unload(e, "a");
    br_assets_unload(e, DIR "a.json");
    BR_CHECK(br_assets_get(e, DIR "a.json") == a && br_assets_ref_count(e, DIR "a.json") == 1);
    br_assets_unload(e, DIR "a.json");
    BR_CHECK(br_assets_get(e, DIR "a.json") == NULL && br_assets_ref_count(e, DIR "a.json") == 0);
    br_assets_unload(e, DIR "a.json"); /* extra unloads are harmless */

    const char *text = br_assets_load(e, DIR "b.txt");
    BR_CHECK(text != NULL && br_str_equal(text, "hello"));
    const br_asset_blob *blob = br_assets_load(e, DIR "c.bin");
    BR_CHECK(blob != NULL && blob->size == 3 && blob->data[2] == 3);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "c.bin"), "binary"));
    br_asset_info list[4];
    uint32_t n = br_assets_list(e, list, 4);
    BR_CHECK(n == 2);
    for (uint32_t i = 0; i < n && i < 4; i++) {
        BR_CHECK(list[i].ref_count == 1 && !list[i].loading && list[i].byte_size > 0 && list[i].type != NULL);
    }
    BR_CHECK(br_assets_list(e, NULL, 0) == 2);
    br_assets_unload_all(e);
    BR_CHECK(br_assets_count(e) == 0);
}

/* ---- background loading */

typedef struct async_result {
    bool  done;
    int   calls;
    void *asset;
} async_result;

static void on_done(br_engine *e, const char *path, void *asset, void *user)
{
    (void)e;
    (void)path;
    async_result *r = user;
    r->calls++;
    r->asset = asset;
    r->done = true;
}

typedef struct progress_log {
    int   calls;
    float last;
} progress_log;

static void on_progress(br_engine *e, const char *bundle, float progress, void *user)
{
    (void)e;
    (void)bundle;
    progress_log *p = user;
    BR_CHECK(progress > p->last);
    p->calls++;
    p->last = progress;
}

static void test_async(br_engine *e)
{
    write_text(e, DIR "async.json", "{ \"x\": 1 }");
    async_result r1 = { false, 0, NULL };
    async_result r2 = { false, 0, NULL };
    br_assets_load_async(e, DIR "async.json", NULL, on_done, &r1);
    br_assets_load_async(e, DIR "async.json", NULL, on_done, &r2);
    BR_CHECK(br_assets_ref_count(e, DIR "async.json") == 2); /* references are taken at once */
    BR_CHECK(!r1.done);                                       /* callbacks only from br_engine_step */
    BR_CHECK(step_until(e, &r2.done) && r1.done);
    BR_CHECK(r1.asset != NULL && r1.asset == r2.asset && r1.calls == 1);
    BR_CHECK(br_assets_get(e, DIR "async.json") == r1.asset);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "async.json"), "json"));

    /* already cached: still reported from a step */
    async_result r3 = { false, 0, NULL };
    br_assets_load_async(e, DIR "async.json", NULL, on_done, &r3);
    BR_CHECK(!r3.done && step_until(e, &r3.done) && r3.asset == r1.asset);
    BR_CHECK(br_assets_ref_count(e, DIR "async.json") == 3);
    br_assets_unload_all(e);

    /* decode on a worker, finish on the main thread (DDS) */
    static uint8_t buf[148 + 4096];
    size_t size = make_dds(buf, 8, true);
    write_file(e, DIR "async.dds", buf, size);
    async_result r4 = { false, 0, NULL };
    br_assets_load_async(e, DIR "async.dds", NULL, on_done, &r4);
    BR_CHECK(step_until(e, &r4.done) && r4.asset != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "async.dds"), "texture"));

    /* failure: NULL, nothing cached */
    async_result r5 = { false, 0, NULL };
    br_assets_load_async(e, DIR "async_missing.png", NULL, on_done, &r5);
    BR_CHECK(step_until(e, &r5.done) && r5.asset == NULL);
    BR_CHECK(br_assets_ref_count(e, DIR "async_missing.png") == 0);

    /* unloaded before it finished: dropped, the callback gets NULL */
    write_text(e, DIR "cancel.txt", "bye");
    async_result r6 = { false, 0, NULL };
    br_assets_load_async(e, DIR "cancel.txt", NULL, on_done, &r6);
    br_assets_unload(e, DIR "cancel.txt");
    BR_CHECK(step_until(e, &r6.done) && r6.asset == NULL);
    step_until_idle(e);
    BR_CHECK(br_assets_get(e, DIR "cancel.txt") == NULL);

    /* a synchronous load while an async one is in flight: one entry, both references */
    write_text(e, DIR "race.txt", "race");
    async_result r7 = { false, 0, NULL };
    br_assets_load_async(e, DIR "race.txt", NULL, on_done, &r7);
    void *sync = br_assets_load(e, DIR "race.txt");
    BR_CHECK(sync != NULL);
    BR_CHECK(step_until(e, &r7.done) && r7.asset == sync);
    step_until_idle(e);
    BR_CHECK(br_assets_get(e, DIR "race.txt") == sync && br_assets_ref_count(e, DIR "race.txt") == 2);
    br_assets_unload_all(e);

    /* background prefetch: cached without references; the first load takes one */
    write_text(e, DIR "bg.txt", "background");
    const char *paths[1] = { DIR "bg.txt" };
    br_assets_background_load(e, paths, 1);
    step_until_idle(e);
    BR_CHECK(br_assets_get(e, DIR "bg.txt") != NULL && br_assets_ref_count(e, DIR "bg.txt") == 0);
    BR_CHECK(br_assets_load(e, DIR "bg.txt") != NULL && br_assets_ref_count(e, DIR "bg.txt") == 1);
    br_assets_unload(e, DIR "bg.txt");
    BR_CHECK(br_assets_get(e, DIR "bg.txt") == NULL);

    /* a bundle: progress after each asset, the last call is 1 (failures count as finished) */
    write_png(e, DIR "atlas.png", 64, 32);
    write_text(e, DIR "sheet.json", g_sheet_hash);
    write_text(e, DIR "level.json", "[1, 2, 3]");
    const char *manifest =
        "{ \"bundles\": [ { \"name\": \"level\", \"assets\": ["
        "  { \"alias\": \"sheet\", \"src\": \"" DIR "sheet.json\" },"
        "  { \"alias\": \"data\", \"src\": \"" DIR "level.json\" },"
        "  { \"alias\": \"img\", \"src\": \"" DIR "bc{7,1}.dds\" },"
        "  { \"alias\": \"gone\", \"src\": \"" DIR "gone.png\" } ] } ] }";
    BR_CHECK(br_assets_add_manifest(e, manifest, SDL_strlen(manifest)));
    progress_log progress = { 0, 0.0f };
    br_assets_set_frame_budget(e, 0.0f);
    BR_CHECK(br_assets_load_bundle(e, "level", on_progress, &progress));
    for (int i = 0; i < 5000 && progress.last < 1.0f; i++) {
        br_engine_step(e, 1.0f / 60.0f);
        SDL_Delay(1);
    }
    BR_CHECK(progress.calls == 4 && progress.last == 1.0f);
    BR_CHECK(br_str_equal(br_assets_type(e, "sheet"), "spritesheet"));
    BR_CHECK(br_str_equal(br_assets_type(e, "data"), "json"));
    BR_CHECK(br_str_equal(br_assets_type(e, "img"), "texture"));
    BR_CHECK(br_assets_get(e, "gone") == NULL);
    BR_CHECK(br_assets_ref_count(e, "sheet") == 1 && br_assets_ref_count(e, DIR "atlas.png") == 1);
    br_assets_unload_bundle(e, "level");
    BR_CHECK(br_assets_count(e) == 0);
    br_assets_set_frame_budget(e, 4.0f);

    /* left in flight at shutdown: br_engine_destroy joins the workers and frees everything */
    br_assets_load_async(e, DIR "level.json", NULL, NULL, NULL);
    br_assets_load_async(e, DIR "async.dds", NULL, NULL, NULL);
}

/* ---- loader choice: a claimed load that fails falls back; self-loads are refused */

static int g_warnings = 0;
static SDL_LogOutputFunction g_default_log = NULL;
static void *g_default_log_user = NULL;

static void count_warnings(void *unused, int category, SDL_LogPriority priority, const char *message)
{
    (void)unused;
    if (priority == SDL_LOG_PRIORITY_WARN) {
        g_warnings++;
    }
    g_default_log(g_default_log_user, category, priority, message);
}

static void test_loader_choice(br_engine *e)
{
    /* game data that uses "frames" and "meta" is plain JSON */
    write_text(e, DIR "tuning.json", "{ \"meta\": { \"author\": \"me\" }, \"frames\": 30, \"speed\": 4 }");
    BR_CHECK(br_assets_load(e, DIR "tuning.json") != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "tuning.json"), "json"));
    br_assets_unload(e, DIR "tuning.json");
    write_text(e, DIR "nested.json", "{ \"level\": { \"frames\": {}, \"meta\": { \"image\": \"x.png\" } } }");
    BR_CHECK(br_assets_load(e, DIR "nested.json") != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "nested.json"), "json"));
    br_assets_unload(e, DIR "nested.json");

    /* shaped like a sheet, but the sheet load fails: the next loader (json) gets the file */
    write_text(e, DIR "almost_sheet.json", "{ \"frames\": {}, \"meta\": { \"image\": \"absent.png\" } }");
    BR_CHECK(br_assets_load(e, DIR "almost_sheet.json") != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "almost_sheet.json"), "json"));
    br_assets_unload(e, DIR "almost_sheet.json");
    async_result r = { false, 0, NULL };
    br_assets_load_async(e, DIR "almost_sheet.json", NULL, on_done, &r);
    BR_CHECK(step_until(e, &r.done) && r.asset != NULL);
    BR_CHECK(br_str_equal(br_assets_type(e, DIR "almost_sheet.json"), "json"));
    br_assets_unload(e, DIR "almost_sheet.json");

    /* a multi-pack that lists itself: refused, no second copy, back to baseline after one unload */
    uint32_t baseline = br_assets_count(e);
    write_png(e, DIR "selfpack.png", 16, 16);
    write_text(e, DIR "selfpack.json",
               "{ \"frames\": { \"s0\": { \"frame\": {\"x\":0,\"y\":0,\"w\":8,\"h\":8} } },"
               "  \"meta\": { \"image\": \"selfpack.png\", \"related_multi_packs\": [\"selfpack.json\"] } }");
    br_spritesheet *sheet = br_spritesheet_load(e, DIR "selfpack.json");
    BR_CHECK(sheet != NULL);
    BR_CHECK(br_assets_ref_count(e, DIR "selfpack.json") == 1);
    br_assets_unload(e, DIR "selfpack.json");
    BR_CHECK(br_assets_count(e) == baseline);

    /* bare frame names: the first loaded sheet with the name wins (a warning names the other),
     * and a scene using one owns a reference on that sheet */
    write_text(e, DIR "dup_a.json",
               "{ \"frames\": { \"dup.png\": { \"frame\": {\"x\":0,\"y\":0,\"w\":8,\"h\":8} } }, \"meta\": { \"image\": \"selfpack.png\" } }");
    write_text(e, DIR "dup_b.json",
               "{ \"frames\": { \"dup.png\": { \"frame\": {\"x\":8,\"y\":0,\"w\":4,\"h\":4} } }, \"meta\": { \"image\": \"selfpack.png\" } }");
    SDL_GetLogOutputFunction(&g_default_log, &g_default_log_user);
    SDL_SetLogOutputFunction(count_warnings, NULL);
    br_spritesheet *a = br_spritesheet_load(e, DIR "dup_a.json");
    br_spritesheet *b = br_spritesheet_load(e, DIR "dup_b.json");
    SDL_SetLogOutputFunction(g_default_log, g_default_log_user);
    BR_CHECK(a != NULL && b != NULL && g_warnings == 1);
    BR_CHECK(br_data_texture(e, "dup.png").index == br_spritesheet_frame(e, a, "dup.png").index);
    br_assets_unload(e, DIR "dup_a.json"); /* br_data_texture took one reference: still loaded */
    BR_CHECK(br_assets_ref_count(e, DIR "dup_a.json") == 1);
    br_assets_unload(e, DIR "dup_a.json");
    const char *scene = "{ \"type\": \"sprite\", \"texture\": \"dup.png\" }";
    br_node n = br_scene_file_load_json(e, scene, SDL_strlen(scene), "bare.scene.json", br_stage_root(e));
    BR_CHECK(br_assets_ref_count(e, DIR "dup_b.json") == 2); /* the scene's reference */
    br_assets_unload(e, DIR "dup_b.json");                   /* the other owner lets go */
    BR_CHECK(br_assets_get(e, DIR "dup_b.json") != NULL);    /* still there for the scene */
    br_node_destroy(e, n);
    br_engine_step(e, 1.0f / 60.0f); /* node flush, then the scene's references go */
    BR_CHECK(br_assets_count(e) == baseline);
}

int main(void)
{
    br_engine *e = br_engine_create("test_assets", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    char dir[1024];
    BR_CHECK(br_file_resolve(e, "test_assets_data", dir, sizeof(dir)));
    SDL_CreateDirectory(dir);
    test_resolver(e);
    test_manifest(e);
    test_spritesheet(e);
    test_compressed_and_failures(e);
    test_ref_counting(e);
    test_loader_choice(e);
    test_async(e);
    br_engine_destroy(e);
    return br_test_finish();
}
