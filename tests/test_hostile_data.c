/* test_hostile_data - data files written to break the loaders: prefab cycles, brace patterns that
 * explode, counts and numbers out of range, over-long names, truncated and malformed scene files,
 * prefabs, manifests, spritesheets and game.json. Each must log and fail (or skip the bad part)
 * quickly, without crashing, hanging or allocating gigabytes. Headless.
 *
 * Fixtures are written at startup into <executable dir>/hostile_data/. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>
#include <string.h>

#define DIR "hostile_data/"

/* ---- log capture: the last error line and how many there were */

static SDL_LogOutputFunction g_default_log = NULL;
static void *g_default_log_user = NULL;
static char g_last_log[2048];
static int g_log_errors = 0;

static void capture_log(void *unused, int category, SDL_LogPriority priority, const char *message)
{
    (void)unused;
    if (priority >= SDL_LOG_PRIORITY_ERROR) {
        SDL_strlcpy(g_last_log, message, sizeof(g_last_log));
        g_log_errors++;
    }
    g_default_log(g_default_log_user, category, priority, message);
}

static bool log_has(const char *text)
{
    if (strstr(g_last_log, text) == NULL) {
        printf("  last logged error: '%s'\n  expected to contain: '%s'\n", g_last_log, text);
        return false;
    }
    return true;
}

static bool error_has(br_engine *e, const char *text)
{
    const char *err = br_scene_file_last_error(e);
    if (strstr(err, text) == NULL) {
        printf("  last error: '%s'\n  expected to contain: '%s'\n", err, text);
        return false;
    }
    return true;
}

static void write_text(br_engine *e, const char *path, const char *text)
{
    BR_CHECK(br_file_write_all(e, path, text, strlen(text)));
}

static br_node load(br_engine *e, const char *json)
{
    return br_scene_file_load_json(e, json, strlen(json), "hostile.scene.json", br_stage_root(e));
}

/* Loads json, expects exactly one more data error containing `text`, and that it took < 200 ms. */
static void expect_one_error(br_engine *e, const char *json, const char *text)
{
    uint32_t errors = br_scene_file_error_count(e);
    uint64_t start = SDL_GetTicks();
    br_node n = load(e, json);
    uint64_t ms = SDL_GetTicks() - start;
    BR_CHECK(ms < 200);
    BR_CHECK(br_scene_file_error_count(e) - errors >= 1);
    if (!error_has(e, text)) {
        printf("  (scene: %s)\n", json);
        BR_CHECK(false);
    }
    br_node_destroy(e, n);
}

/* ---- prefab cycles */

static void test_prefab_cycles(br_engine *e)
{
    br_prefab_clear_cache(e);

    /* a prefab whose child is itself */
    write_text(e, DIR "self.prefab.json", "{ \"label\": \"self\", \"children\": [ { \"prefab\": \"" DIR "self.prefab.json\" } ] }");
    uint32_t errors = br_scene_file_error_count(e);
    br_node n = br_prefab_instantiate(e, DIR "self.prefab.json", br_stage_root(e), NULL);
    BR_CHECK(br_node_valid(e, n));
    BR_CHECK(br_node_child_count(e, n) == 0); /* the cyclic child is skipped */
    BR_CHECK(br_scene_file_error_count(e) - errors == 1);
    BR_CHECK(error_has(e, "includes itself"));
    br_node_destroy(e, n);

    /* A -> B -> A, the chain is logged */
    write_text(e, DIR "a.prefab.json", "{ \"label\": \"a\", \"children\": [ { \"prefab\": \"" DIR "b.prefab.json\" } ] }");
    write_text(e, DIR "b.prefab.json", "{ \"label\": \"b\", \"children\": [ { \"prefab\": \"" DIR "a.prefab.json\" } ] }");
    n = br_prefab_instantiate(e, DIR "a.prefab.json", br_stage_root(e), NULL);
    BR_CHECK(br_node_valid(e, n) && br_node_child_count(e, n) == 1);
    BR_CHECK(error_has(e, DIR "a.prefab.json -> " DIR "b.prefab.json -> " DIR "a.prefab.json"));
    br_node_destroy(e, n);

    /* a prefab based on itself */
    write_text(e, DIR "base.prefab.json", "{ \"prefab\": \"" DIR "base.prefab.json\", \"label\": \"base\" }");
    n = br_prefab_instantiate(e, DIR "base.prefab.json", br_stage_root(e), NULL);
    BR_CHECK(!br_node_valid(e, n));
    BR_CHECK(error_has(e, "includes itself"));

    /* two self-references, loaded as a br_scene (collection first, then the build): quick */
    write_text(e, DIR "twice.prefab.json",
               "{ \"children\": [ { \"prefab\": \"" DIR "twice.prefab.json\" }, { \"prefab\": \"" DIR "twice.prefab.json\" },"
               " { \"prefab\": \"" DIR "twice.prefab.json\" } ] }");
    write_text(e, DIR "twice.scene.json", "{ \"children\": [ { \"prefab\": \"" DIR "twice.prefab.json\" } ] }");
    uint64_t start = SDL_GetTicks();
    br_scene s = br_scene_load(e, DIR "twice.scene.json", BR_SCENE_ADDITIVE);
    BR_CHECK(SDL_GetTicks() - start < 1000);
    BR_CHECK(br_scene_is_loaded(e, s));
    BR_CHECK(error_has(e, "includes itself"));
    br_scene_unload(e, s);

    /* a chain of 20 distinct prefabs, each based on the next: stopped at 16 */
    for (int i = 0; i < 20; i++) {
        char path[128];
        char text[256];
        SDL_snprintf(path, sizeof(path), DIR "chain%d.prefab.json", i);
        SDL_snprintf(text, sizeof(text), "{ \"prefab\": \"" DIR "chain%d.prefab.json\" }", i + 1);
        write_text(e, path, i < 19 ? text : "{ \"label\": \"end\" }");
    }
    n = br_prefab_instantiate(e, DIR "chain0.prefab.json", br_stage_root(e), NULL);
    BR_CHECK(!br_node_valid(e, n));
    BR_CHECK(error_has(e, "nest more than 16"));

    /* children nested 400 deep: refused past the limit instead of overflowing the stack */
    static char deep[400 * 20 + 64];
    deep[0] = '\0';
    for (int i = 0; i < 400; i++) {
        SDL_strlcat(deep, "{ \"children\": [", sizeof(deep));
    }
    SDL_strlcat(deep, "{}", sizeof(deep));
    for (int i = 0; i < 400; i++) {
        SDL_strlcat(deep, "] }", sizeof(deep));
    }
    expect_one_error(e, deep, "nest deeper than");
    br_prefab_clear_cache(e);
}

/* ---- "{a,b}" patterns */

static void test_brace_patterns(br_engine *e)
{
    char name[512] = "x";
    for (int i = 0; i < 40; i++) {
        SDL_strlcat(name, "{a,b}", sizeof(name));
    }
    SDL_strlcat(name, ".png", sizeof(name));
    char out[512];
    uint64_t start = SDL_GetTicks();
    BR_CHECK(!br_assets_resolve(e, name, out, sizeof(out)));
    BR_CHECK(SDL_GetTicks() - start < 100);
    BR_CHECK(log_has("expands to more than"));

    BR_CHECK(br_assets_resolve(e, "x{a,b}{a,b}{a,b}{a,b}.png", out, sizeof(out)));
    BR_CHECK(strcmp(out, "xaaaa.png") == 0);

    char single[512] = "y";
    for (int i = 0; i < 150; i++) {
        SDL_strlcat(single, "{a}", sizeof(single));
    }
    BR_CHECK(br_assets_resolve(e, single, out, sizeof(out)));
    BR_CHECK(strlen(out) == 151);
}

/* ---- counts, indices and numbers out of range */

typedef struct hostile_comp {
    int32_t count;
} hostile_comp;

static void test_ranges(br_engine *e)
{
    static const br_field_desc fields[] = { { "count", BR_FIELD_INT, offsetof(hostile_comp, count) } };
    br_component_desc desc = { "hostile", sizeof(hostile_comp), fields, 1 };
    br_component_register(e, &desc);

    expect_one_error(e, "{ \"type\": \"graphics\", \"commands\": [[\"regular_poly\", 0, 0, 10, 1e9], [\"fill\", \"red\"]] }",
                     "out of range");
    expect_one_error(e, "{ \"type\": \"graphics\", \"commands\": [[\"regular_poly\", 0, 0, 10, -5]] }", "out of range");
    expect_one_error(e, "{ \"type\": \"graphics\", \"commands\": [[\"round_poly\", 0, 0, 10, 1e12, 2]] }",
                     "out of range");
    expect_one_error(e, "{ \"type\": \"graphics\", \"commands\": [[\"star\", 0, 0, 4e9, 10]] }", "out of range");
    expect_one_error(e, "{ \"type\": \"mesh\", \"plane\": [100000, 100000] }", "out of range");
    expect_one_error(e, "{ \"type\": \"mesh\", \"perspective\": { \"verts\": [1e9, 2] } }", "out of range");
    expect_one_error(e, "{ \"type\": \"mesh\", \"positions\": [0, 0, 1, 0, 0, 1], \"indices\": [0, 1, 1e20] }",
                     "indices");
    expect_one_error(e, "{ \"type\": \"mesh\", \"positions\": [0, 0, 1, 0, 0, 1], \"indices\": [0, 1, 1.5] }",
                     "indices");
    expect_one_error(e, "{ \"filters\": [ { \"type\": \"blur\", \"quality\": 100000000 } ] }", "out of range");
    expect_one_error(e, "{ \"filters\": [ { \"type\": \"blur\", \"kernel_size\": -1e30 } ] }", "out of range");
    expect_one_error(e, "{ \"tint\": 1e20 }", "out of range");
    expect_one_error(e, "{ \"tint\": -1 }", "out of range");
    expect_one_error(e, "{ \"z_index\": 1e20 }", "out of range");
    expect_one_error(e, "{ \"position\": [1e300, 0] }", "out of range");
    expect_one_error(e, "{ \"components\": { \"hostile\": { \"count\": 1e20 } } }", "out of range");
    expect_one_error(e, "{ \"components\": { \"hostile\": { \"count\": -3e9 } } }", "out of range");

    /* in range still works */
    br_node ok = load(e, "{ \"tint\": 16711680, \"z_index\": -2, \"components\": { \"hostile\": { \"count\": -2147483648 } } }");
    BR_CHECK(br_color_to_hex(br_node_tint(e, ok)) == 0xff0000);
    BR_CHECK(br_node_z_index(e, ok) == -2);
    hostile_comp *c = br_component_get(e, ok, br_component_find(e, "hostile"));
    BR_CHECK(c != NULL && c->count == INT32_MIN);
    br_node_destroy(e, ok);
}

/* ---- names longer than every buffer */

static void test_long_names(br_engine *e)
{
    static char json[1024];
    char name[620];
    SDL_memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    SDL_snprintf(json, sizeof(json), "{ \"type\": \"sprite\", \"texture\": \"%s.png\" }", name);
    uint32_t errors = br_scene_file_error_count(e);
    br_node n = load(e, json);
    BR_CHECK(br_scene_file_error_count(e) - errors == 1);
    BR_CHECK(error_has(e, "cannot be resolved"));
    br_node_destroy(e, n);
}

/* ---- truncated and malformed files */

static void test_malformed_files(br_engine *e)
{
    uint32_t errors = br_scene_file_error_count(e);

    /* scene files */
    write_text(e, DIR "cut.scene.json", "{ \"children\": [ { \"type\": \"sprite\", \"texture\": \"a.p");
    BR_CHECK(!br_node_valid(e, br_scene_file_load(e, DIR "cut.scene.json", br_stage_root(e))));
    BR_CHECK(error_has(e, "JSON syntax error"));
    br_scene s = br_scene_load(e, DIR "cut.scene.json", BR_SCENE_ADDITIVE);
    br_scene_unload(e, s);
    write_text(e, DIR "empty.scene.json", "");
    BR_CHECK(!br_node_valid(e, br_scene_file_load(e, DIR "empty.scene.json", br_stage_root(e))));
    write_text(e, DIR "array.scene.json", "[1, 2, 3]");
    BR_CHECK(!br_node_valid(e, br_scene_file_load(e, DIR "array.scene.json", br_stage_root(e))));
    br_node n = load(e, "{ \"type\": 5, \"children\": 5 }");
    BR_CHECK(!br_node_valid(e, n));
    n = load(e, "{ \"children\": 5, \"components\": [1], \"assets\": 7, \"filters\": \"x\", \"hit_area\": 3,"
                " \"mask\": 7, \"on\": [1], \"layer\": 4, \"prefab_x\": 1, \"position\": \"left\" }");
    BR_CHECK(br_node_valid(e, n));
    br_node_destroy(e, n);
    n = load(e, "{ \"children\": [ { \"prefab\": 5 }, { \"prefab\": \"\" }, { \"prefab\": \"" DIR "none.prefab.json\" } ] }");
    BR_CHECK(br_node_valid(e, n) && br_node_child_count(e, n) == 0);
    br_node_destroy(e, n);

    /* prefabs */
    write_text(e, DIR "cut.prefab.json", "{ \"label\": \"x\", \"children\": [");
    BR_CHECK(!br_node_valid(e, br_prefab_instantiate(e, DIR "cut.prefab.json", br_stage_root(e), NULL)));
    BR_CHECK(error_has(e, "not valid JSON"));
    write_text(e, DIR "list.prefab.json", "[ { \"label\": \"x\" } ]");
    BR_CHECK(!br_node_valid(e, br_prefab_instantiate(e, DIR "list.prefab.json", br_stage_root(e), NULL)));
    BR_CHECK(error_has(e, "must be one node object"));
    BR_CHECK(br_scene_file_error_count(e) - errors >= 8);

    /* manifests */
    const char *manifests[] = {
        "{ \"bundles\": [ { \"name\": \"x\", \"assets\": [",
        "{ \"bundles\": 5 }",
        "{ \"bundles\": [ 5, { \"name\": 5 } ] }",
        "{ \"bundles\": [ { \"name\": \"m\", \"assets\": [ 5, { \"src\": 5 }, { \"alias\": 3, \"src\": [] } ] } ] }",
        "{ \"bundles\": [ { \"name\": \"m2\", \"assets\": 9 } ] }",
        "",
    };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(manifests); i++) {
        write_text(e, DIR "bad.manifest.json", manifests[i]);
        BR_CHECK(!br_assets_load_manifest(e, DIR "bad.manifest.json"));
    }
    BR_CHECK(!br_assets_load_manifest(e, DIR "no_such.manifest.json"));

    /* spritesheets */
    const char *sheets[] = {
        "{ \"frames\": { \"a\": { \"frame\": { \"x\": 0 } } }, \"meta\": { \"image\": \"x.png\"",
        "{ \"frames\": \"none\", \"meta\": { \"image\": \"x.png\" } }",
        "{ \"frames\": {}, \"meta\": { \"image\": 5 } }",
        "{ \"frames\": {}, \"meta\": 5 }",
        "{ \"frames\": [], \"meta\": { \"image\": \"" DIR "cut.scene.json\" } }",
        "",
    };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(sheets); i++) {
        write_text(e, DIR "bad_sheet.json", sheets[i]);
        BR_CHECK(br_spritesheet_load(e, DIR "bad_sheet.json") == NULL);
    }

    /* game.json */
    br_game_config c;
    BR_CHECK(!br_game_config_parse("{ \"window\": { \"title\": \"x", 25, "game.json", &c));
    BR_CHECK(!br_game_config_parse("", 0, "game.json", &c));
    BR_CHECK(!br_game_config_parse("[1]", 3, "game.json", &c));
    static char long_title[1024];
    char title[300];
    SDL_memset(title, 't', sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    SDL_snprintf(long_title, sizeof(long_title),
                 "{ \"window\": { \"title\": \"%s\", \"width\": 1e20, \"height\": -4 },"
                 "  \"debug\": { \"toggle_key\": \"%s\" }, \"ticker\": 5 }",
                 title, title);
    BR_CHECK(br_game_config_parse(long_title, strlen(long_title), "game.json", &c));
    BR_CHECK(strcmp(c.title, "braty") == 0 && strcmp(c.debug_key, "F1") == 0); /* defaults kept */
    BR_CHECK(c.width == 1280 && c.height == 720);
    br_game_config_free(&c);

    const char *odd = "{ \"window\": 5, \"input\": [1, 2], \"scenes\": [\"a\"],"
                      "  \"audio\": { \"buses\": [1, 2], \"sprites\": [3, { }], \"master_volume\": \"loud\" } }";
    BR_CHECK(br_game_config_parse(odd, strlen(odd), "game.json", &c));
    int before = g_log_errors;
    br_game_config_apply(e, &c); /* takes the JSON; logs, does not crash */
    BR_CHECK(g_log_errors > before);
    BR_CHECK(br_audio_bus_find(e, "") <= 0);
}

int main(int argc, char **argv)
{
    br_engine *e = br_engine_create("test_hostile_data", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    char dir[1024];
    BR_CHECK(br_file_resolve(e, "hostile_data", dir, sizeof(dir)));
    SDL_CreateDirectory(dir);
    SDL_GetLogOutputFunction(&g_default_log, &g_default_log_user);
    SDL_SetLogOutputFunction(capture_log, NULL);

    /* one group by name (test_hostile_data prefabs), else all */
    const char *only = argc > 1 ? argv[1] : "";
    if (only[0] == 0 || strcmp(only, "braces") == 0) {
        test_brace_patterns(e);
    }
    if (only[0] == 0 || strcmp(only, "ranges") == 0) {
        test_ranges(e);
    }
    if (only[0] == 0 || strcmp(only, "names") == 0) {
        test_long_names(e);
    }
    if (only[0] == 0 || strcmp(only, "files") == 0) {
        test_malformed_files(e);
    }
    if (only[0] == 0 || strcmp(only, "prefabs") == 0) {
        test_prefab_cycles(e);
    }

    SDL_SetLogOutputFunction(g_default_log, g_default_log_user);
    br_engine_destroy(e);
    return br_test_finish();
}
