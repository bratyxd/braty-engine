/* test_pack: the storage layer - path normalisation, .brpack mounting and reads, loose files
 * over packs, hostile packs (hand-built), async loads from a pack on the worker thread, the log
 * file in user storage.
 *
 *   test_pack                    unit tests
 *   test_pack PACK FOLDER        every file of FOLDER read through PACK alone matches the loose
 *                                file (CTest runs it on a pack built by tools/pack_assets.py)
 */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

/* ---- log capture: hostile packs must name the pack and the entry */

static SDL_LogOutputFunction g_prev_log;
static void *g_prev_log_user;
static char g_log[8192];

static void SDLCALL capture_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user;
    if (priority >= SDL_LOG_PRIORITY_ERROR) {
        SDL_strlcat(g_log, message, sizeof(g_log));
        SDL_strlcat(g_log, "\n", sizeof(g_log));
    }
    g_prev_log(g_prev_log_user, category, priority, message);
}

static bool logged(const char *text)
{
    return SDL_strstr(g_log, text) != NULL;
}

/* ---- a hand-built pack */

typedef struct pack_file {
    const char *path;
    const char *data;
} pack_file;

typedef struct pack_bytes {
    uint8_t data[4096];
    size_t  size;
} pack_bytes;

static uint64_t fnv1a64(const void *bytes, size_t size)
{
    const uint8_t *p = bytes;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Entries in the order given (so tests can build unsorted or duplicate tables). */
static void build_pack(pack_bytes *out, const pack_file *files, uint32_t count)
{
    uint32_t toc = 0;
    for (uint32_t i = 0; i < count; i++) {
        toc += 26 + (uint32_t)SDL_strlen(files[i].path);
    }
    SDL_memcpy(out->data, "BRPK", 4);
    put_u32(out->data + 4, 1);
    put_u32(out->data + 8, count);
    put_u32(out->data + 12, toc);
    size_t at = 16;
    uint64_t offset = 16 + toc;
    for (uint32_t i = 0; i < count; i++) {
        size_t len = SDL_strlen(files[i].data);
        size_t path_len = SDL_strlen(files[i].path);
        put_u64(out->data + at, offset);
        put_u64(out->data + at + 8, len);
        put_u64(out->data + at + 16, fnv1a64(files[i].data, len));
        out->data[at + 24] = (uint8_t)path_len;
        out->data[at + 25] = (uint8_t)(path_len >> 8);
        SDL_memcpy(out->data + at + 26, files[i].path, path_len);
        at += 26 + path_len;
        offset += len;
    }
    for (uint32_t i = 0; i < count; i++) {
        size_t len = SDL_strlen(files[i].data);
        SDL_memcpy(out->data + at, files[i].data, len);
        at += len;
    }
    out->size = at;
}

static const char *pack_path(const char *name)
{
    static char paths[4][1024]; /* a few at once: callers hold one while asking for the next */
    static int next = 0;
    char *path = paths[next];
    next = (next + 1) % 4;
    SDL_snprintf(path, sizeof(paths[0]), "%spack_tests/%s", SDL_GetBasePath(), name);
    return path;
}

static const char *write_pack(const char *name, const void *data, size_t size)
{
    const char *path = pack_path(name);
    BR_CHECK(SDL_SaveFile(path, data, size));
    return path;
}

static bool read_text(br_engine *e, const char *path, const char *expect)
{
    void *bytes = NULL;
    size_t size = 0;
    bool ok = br_file_read_all(e, path, &bytes, &size) && size == SDL_strlen(expect) &&
              SDL_memcmp(bytes, expect, size) == 0 && ((char *)bytes)[size] == 0;
    br_free(bytes);
    return ok;
}

static const pack_file g_files[] = {
    { "a.txt", "alpha" },
    { "dir/b.json", "{\"b\": 2}" },
    { "dir/sub/c.txt", "gamma" },
};

/* ---- tests */

static void test_normalize(void)
{
    char out[64];
    BR_CHECK(br_file_normalize("a/b.txt", out, sizeof(out)) && SDL_strcmp(out, "a/b.txt") == 0);
    BR_CHECK(br_file_normalize("a\\b.txt", out, sizeof(out)) && SDL_strcmp(out, "a/b.txt") == 0);
    BR_CHECK(br_file_normalize("./a//b/./c.txt", out, sizeof(out)) && SDL_strcmp(out, "a/b/c.txt") == 0);
    BR_CHECK(br_file_normalize("a/x/../b.txt", out, sizeof(out)) && SDL_strcmp(out, "a/b.txt") == 0);
    BR_CHECK(br_file_normalize("Dir/File.TXT", out, sizeof(out)) && SDL_strcmp(out, "Dir/File.TXT") == 0); /* case kept */
    BR_CHECK(br_file_normalize("a..b/..c", out, sizeof(out)) && SDL_strcmp(out, "a..b/..c") == 0);
    BR_CHECK(!br_file_normalize("../a.txt", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("a/../../a.txt", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("..\\a.txt", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("/abs.txt", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("C:/abs.txt", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("a/..", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("a\nb", out, sizeof(out)));
    BR_CHECK(!br_file_normalize("a/b.txt", out, 4)); /* too long for out */
}

static void test_mount_and_read(void)
{
    pack_bytes pack;
    build_pack(&pack, g_files, BR_ARRAY_COUNT(g_files));
    const char *path = write_pack("good.brpack", pack.data, pack.size);
    /* the asset root has no loose files: everything must come from the pack */
    br_engine *e = br_engine_create("test_pack", pack_path("empty_root"));
    BR_CHECK(br_file_mount_pack(e, path));
    BR_CHECK(br_file_pack_count(e) == 1);
    BR_CHECK(read_text(e, "a.txt", "alpha"));
    BR_CHECK(read_text(e, "dir/sub/c.txt", "gamma"));
    BR_CHECK(read_text(e, "dir\\b.json", "{\"b\": 2}"));      /* normalised lookups */
    BR_CHECK(read_text(e, "./dir/x/../sub/c.txt", "gamma"));
    BR_CHECK(br_file_exists(e, "dir/b.json"));
    BR_CHECK(!br_file_exists(e, "dir"));                      /* folders are not entries */
    BR_CHECK(!br_file_exists(e, "A.TXT"));                    /* packs are case-sensitive */
    BR_CHECK(!read_text(e, "../a.txt", "alpha"));             /* never escapes the root */
    BR_CHECK(!read_text(e, "missing.txt", ""));
    /* an asset loader reads through the same layer */
    BR_CHECK(br_assets_load(e, "dir/b.json") != NULL);
    br_assets_unload(e, "dir/b.json");
    br_file_unmount_packs(e);
    BR_CHECK(br_file_pack_count(e) == 0);
    BR_CHECK(!br_file_exists(e, "a.txt"));
    br_engine_destroy(e);
}

static void test_loose_override(void)
{
    pack_bytes pack;
    build_pack(&pack, g_files, BR_ARRAY_COUNT(g_files));
    const char *path = write_pack("override.brpack", pack.data, pack.size);
    br_engine *e = br_engine_create("test_pack", pack_path("loose_root"));
    BR_CHECK(br_file_write_all(e, "a.txt", "loose alpha", 11)); /* writes are loose files */
    BR_CHECK(br_file_mount_pack(e, path));
    br_file_set_loose_first(e, true);
    BR_CHECK(br_file_loose_first(e));
    BR_CHECK(read_text(e, "a.txt", "loose alpha"));
    BR_CHECK(read_text(e, "dir/b.json", "{\"b\": 2}")); /* no loose file: the pack still serves it */
    br_file_set_loose_first(e, false);
    BR_CHECK(read_text(e, "a.txt", "alpha"));
    /* two packs: the first mounted wins */
    const pack_file second[] = { { "a.txt", "second" }, { "only2.txt", "two" } };
    build_pack(&pack, second, BR_ARRAY_COUNT(second));
    BR_CHECK(br_file_mount_pack(e, write_pack("second.brpack", pack.data, pack.size)));
    BR_CHECK(read_text(e, "a.txt", "alpha"));
    BR_CHECK(read_text(e, "only2.txt", "two"));
    br_engine_destroy(e);
}

/* Mounting `data` must fail, log `expect`, mount nothing, and leave loose files working. */
static bool rejected(br_engine *e, const char *name, const void *data, size_t size, const char *expect)
{
    g_log[0] = 0;
    bool mounted = br_file_mount_pack(e, write_pack(name, data, size));
    bool ok = !mounted && br_file_pack_count(e) == 0 && logged(name) && (expect == NULL || logged(expect));
    if (!ok) {
        SDL_Log("hostile pack '%s' (expect '%s'): mounted %d, log:\n%s", name, expect != NULL ? expect : "", mounted, g_log);
    }
    if (mounted) {
        br_file_unmount_packs(e);
    }
    return ok && read_text(e, "loose.txt", "loose");
}

static void test_hostile_packs(void)
{
    br_engine *e = br_engine_create("test_pack", pack_path("hostile_root"));
    BR_CHECK(br_file_write_all(e, "loose.txt", "loose", 5));
    pack_bytes good;
    build_pack(&good, g_files, BR_ARRAY_COUNT(g_files));
    pack_bytes bad;

    /* every truncation of a good pack (the last entry ends at the end of the file) */
    int accepted = 0;
    for (size_t cut = 0; cut < good.size; cut++) {
        g_log[0] = 0;
        if (br_file_mount_pack(e, write_pack("truncated.brpack", good.data, cut))) {
            accepted++;
            br_file_unmount_packs(e);
        }
    }
    BR_CHECK(accepted == 0);
    BR_CHECK(rejected(e, "short.brpack", good.data, 7, "truncated header"));

    bad = good;
    bad.data[0] = 'X';
    BR_CHECK(rejected(e, "magic.brpack", bad.data, bad.size, "wrong magic"));
    bad = good;
    put_u32(bad.data + 4, 2);
    BR_CHECK(rejected(e, "version.brpack", bad.data, bad.size, "version 2"));
    bad = good;
    put_u32(bad.data + 12, 0xFFFFFFF0u);
    BR_CHECK(rejected(e, "toc_size.brpack", bad.data, bad.size, "larger than the file"));
    bad = good;
    put_u32(bad.data + 8, 0xFFFFFFFFu);
    BR_CHECK(rejected(e, "count.brpack", bad.data, bad.size, "cannot fit"));
    bad = good;
    put_u32(bad.data + 12, 106 - 1); /* the table ends inside the last path */
    BR_CHECK(rejected(e, "toc_short.brpack", bad.data, bad.size, "bad path length"));
    bad = good;
    put_u32(bad.data + 8, 2); /* fewer: stray bytes at the end of the table */
    BR_CHECK(rejected(e, "count2.brpack", bad.data, bad.size, "stray bytes"));

    /* entry 0 is "a.txt": offset at 16, size at 24, path length at 40 */
    bad = good;
    put_u64(bad.data + 16, good.size);
    BR_CHECK(rejected(e, "offset.brpack", bad.data, bad.size, "entry 'a.txt': points past the end"));
    bad = good;
    put_u64(bad.data + 24, good.size + 1);
    BR_CHECK(rejected(e, "size.brpack", bad.data, bad.size, "entry 'a.txt': points past the end"));
    bad = good;
    put_u64(bad.data + 16, 0xFFFFFFFFFFFFFFF8ull); /* offset + size wraps to a small number */
    put_u64(bad.data + 24, 16);
    BR_CHECK(rejected(e, "overflow.brpack", bad.data, bad.size, "entry 'a.txt': points past the end"));
    bad = good;
    bad.data[40] = 0;
    BR_CHECK(rejected(e, "path0.brpack", bad.data, bad.size, "bad path length"));
    bad = good;
    bad.data[40] = 0xFF;
    bad.data[41] = 0xFF;
    BR_CHECK(rejected(e, "path_long.brpack", bad.data, bad.size, "bad path length"));

    const pack_file dup[] = { { "a.txt", "1" }, { "a.txt", "2" } };
    build_pack(&bad, dup, BR_ARRAY_COUNT(dup));
    BR_CHECK(rejected(e, "dup.brpack", bad.data, bad.size, "entry 'a.txt': duplicate"));
    const pack_file unsorted[] = { { "b.txt", "1" }, { "a.txt", "2" } };
    build_pack(&bad, unsorted, BR_ARRAY_COUNT(unsorted));
    BR_CHECK(rejected(e, "unsorted.brpack", bad.data, bad.size, "entry 'a.txt': duplicate or out of order"));

    const char *hostile[] = { "../escape.txt", "a/../../escape.txt", "/abs.txt", "C:/win.ini", "a\\b.txt",
                              "a//b.txt",     "./a.txt",            "a/.",      "a/",         "x:y.txt" };
    for (size_t i = 0; i < BR_ARRAY_COUNT(hostile); i++) {
        const pack_file one[] = { { hostile[i], "evil" } };
        build_pack(&bad, one, 1);
        BR_CHECK(rejected(e, "hostile_path.brpack", bad.data, bad.size, "bad path"));
    }
    /* a NUL inside the stored path */
    const pack_file nul[] = { { "abc.txt", "1" } };
    build_pack(&bad, nul, 1);
    bad.data[16 + 26 + 1] = 0;
    BR_CHECK(rejected(e, "nul_path.brpack", bad.data, bad.size, "bad path"));

    BR_CHECK(rejected(e, "empty.brpack", "", 0, "truncated header"));
    g_log[0] = 0;
    BR_CHECK(!br_file_mount_pack(e, pack_path("does_not_exist.brpack")));
    BR_CHECK(logged("does_not_exist.brpack"));

    /* a corrupt byte in an entry: the mount succeeds, the read fails naming pack and entry */
    bad = good;
    bad.data[bad.size - 1] ^= 0x20;
    BR_CHECK(br_file_mount_pack(e, write_pack("corrupt.brpack", bad.data, bad.size)));
    g_log[0] = 0;
    BR_CHECK(!read_text(e, "dir/sub/c.txt", "gamma"));
    BR_CHECK(logged("corrupt.brpack") && logged("entry 'dir/sub/c.txt': hash mismatch"));
    BR_CHECK(read_text(e, "a.txt", "alpha")); /* the other entries are fine */
    /* the pack shrinks after mounting: the read fails cleanly */
    write_pack("corrupt.brpack", bad.data, 20);
    g_log[0] = 0;
    BR_CHECK(!read_text(e, "a.txt", "alpha"));
    BR_CHECK(logged("corrupt.brpack") && logged("entry 'a.txt'"));
    BR_CHECK(read_text(e, "loose.txt", "loose"));
    br_engine_destroy(e);
}

typedef struct async_result {
    int done;
    int loaded;
} async_result;

static void on_done(br_engine *e, const char *path, void *asset, void *user)
{
    (void)e;
    (void)path;
    async_result *r = user;
    r->done++;
    r->loaded += asset != NULL ? 1 : 0;
}

/* The asset worker reads packs while the main thread mounts more and reads too. */
static void test_async_from_pack(void)
{
    enum { FILES = 64 };
    static char names[FILES][32];
    static char texts[FILES][32];
    pack_file files[FILES];
    for (int i = 0; i < FILES; i++) {
        SDL_snprintf(names[i], sizeof(names[i]), "async/%02d.json", i);
        SDL_snprintf(texts[i], sizeof(texts[i]), "{\"n\": %d}", i);
        files[i].path = names[i];
        files[i].data = texts[i];
    }
    static pack_bytes pack;
    build_pack(&pack, files, FILES);
    br_engine *e = br_engine_create("test_pack", pack_path("empty_root"));
    BR_CHECK(br_file_mount_pack(e, write_pack("async.brpack", pack.data, pack.size)));
    async_result r = { 0, 0 };
    for (int i = 0; i < FILES; i++) {
        br_assets_load_async(e, names[i], NULL, on_done, &r);
    }
    pack_bytes other;
    build_pack(&other, g_files, BR_ARRAY_COUNT(g_files));
    for (int i = 0; i < 20; i++) {
        BR_CHECK(br_file_mount_pack(e, write_pack(i % 2 == 0 ? "async_a.brpack" : "async_b.brpack", other.data, other.size)));
        BR_CHECK(read_text(e, "a.txt", "alpha"));
    }
    for (int i = 0; i < 5000 && (br_assets_pending(e) > 0 || r.done < FILES); i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    BR_CHECK(r.done == FILES && r.loaded == FILES);
    for (int i = 0; i < FILES; i++) {
        br_assets_unload(e, names[i]);
    }
    br_engine_destroy(e);
}

static void test_log_file(void)
{
    br_engine *e = br_engine_create("test_pack", NULL);
    br_engine_set_save_name(e, "braty-tests", "test_pack");
    br_save_remove(e, "test_log.txt");
    br_save_set_log_file(e, "test_log.txt");
    br_log_error("pack test: this line goes to the log file");
    br_engine_destroy(e); /* the file is written at shutdown */
    e = br_engine_create("test_pack", NULL);
    br_engine_set_save_name(e, "braty-tests", "test_pack");
    void *bytes = NULL;
    size_t size = 0;
    BR_CHECK(br_save_read(e, "test_log.txt", &bytes, &size));
    BR_CHECK(bytes != NULL && SDL_strstr(bytes, "ERROR: pack test: this line goes to the log file\n") != NULL);
    br_free(bytes);
    br_save_remove(e, "test_log.txt");
    br_engine_destroy(e);
}

/* game.json "packs" (exe-relative, a missing one is a warning) and --pack (cwd-relative) */
static bool fail_init(br_engine *e)
{
    (void)e;
    br_log_error("pack test: init refuses to start");
    return false;
}

static void test_config_and_args(void)
{
    pack_bytes pack;
    build_pack(&pack, g_files, BR_ARRAY_COUNT(g_files));
    write_pack("config.brpack", pack.data, pack.size);
    const pack_file second[] = { { "a.txt", "from --pack" } };
    pack_bytes cli;
    build_pack(&cli, second, 1);
    write_pack("cli.brpack", cli.data, cli.size);

    br_engine *e = br_engine_create("test_pack", pack_path("empty_root"));
    const char *json = "{ \"packs\": [\"pack_tests/missing.brpack\", \"pack_tests/config.brpack\"] }";
    br_game_config config;
    BR_CHECK(br_game_config_parse(json, SDL_strlen(json), "game.json", &config));
    br_game_config_apply(e, &config);
    br_game_config_free(&config);
    BR_CHECK(br_file_pack_count(e) == 1);
    BR_CHECK(read_text(e, "dir/b.json", "{\"b\": 2}"));
    br_engine_destroy(e);
    g_log[0] = 0;
    const char *bad = "{ \"packs\": \"x.brpack\" }";
    BR_CHECK(br_game_config_parse(bad, SDL_strlen(bad), "game.json", &config));
    BR_CHECK(logged("game.json: packs: expected an array"));
    br_game_config_free(&config);

    /* --pack is relative to the working directory and wins over game.json packs */
    char *cwd = SDL_GetCurrentDirectory();
    char cli_path[1024];
    const char *full = pack_path("cli.brpack");
    BR_CHECK(cwd != NULL && SDL_strncmp(full, cwd, SDL_strlen(cwd)) != 0); /* so an absolute path is needed */
    SDL_strlcpy(cli_path, full, sizeof(cli_path));
    SDL_free(cwd);
    char root[1024];
    SDL_strlcpy(root, pack_path("empty_root"), sizeof(root));
    const char *game = "{ \"packs\": [\"pack_tests/config.brpack\"] }";
    BR_CHECK(SDL_SaveFile(pack_path("game.json"), game, SDL_strlen(game)));
    char *argv[] = { "test_pack", "--headless", "--pack", cli_path, "--assets", root };
    br_app_desc desc = br_game_config_main("pack_tests/game.json", 6, argv, NULL);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 6, argv) == SDL_APP_CONTINUE);
    BR_CHECK(br_file_pack_count(state) == 2);
    BR_CHECK(read_text(state, "a.txt", "from --pack"));
    BR_CHECK(read_text(state, "dir/b.json", "{\"b\": 2}"));
    br_app_quit(state, SDL_APP_SUCCESS);

    /* a game that cannot start in a scripted run returns at once: no dialog to block on */
    char *headless[] = { "test_pack", "--headless" };
    char *frames[] = { "test_pack", "--frames", "1" };
    char *playtest[] = { "test_pack", "--headless", "--playtest", "none.json" };
    char **runs[] = { headless, frames, playtest };
    int counts[] = { 2, 3, 4 };
    for (int i = 0; i < 3; i++) {
        SDL_zero(desc);
        desc.title = "test_pack";
        desc.init = fail_init;
        state = NULL;
        uint64_t start = SDL_GetTicks();
        BR_CHECK(br_app_init(&state, &desc, counts[i], runs[i]) == SDL_APP_FAILURE);
        BR_CHECK(SDL_GetTicks() - start < 10000);
        br_app_quit(state, SDL_APP_FAILURE);
    }
}

/* ---- test_pack PACK FOLDER: the pack serves every file of FOLDER with the same bytes */

typedef struct roundtrip {
    br_engine  *packed;
    br_engine  *loose;
    const char *root;
    int         files;
    int         mismatches;
} roundtrip;

static SDL_EnumerationResult SDLCALL compare_file(void *user, const char *dir, const char *name)
{
    roundtrip *rt = user;
    char full[1024];
    SDL_snprintf(full, sizeof(full), "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full, &info)) {
        return SDL_ENUM_FAILURE;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_strlcat(full, "/", sizeof(full));
        return SDL_EnumerateDirectory(full, compare_file, rt) ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
    }
    const char *rel = full + SDL_strlen(rt->root);
    void *a = NULL;
    void *b = NULL;
    size_t a_size = 0;
    size_t b_size = 0;
    bool ok = br_file_read_all(rt->packed, rel, &a, &a_size) && br_file_read_all(rt->loose, rel, &b, &b_size) &&
              a_size == b_size && SDL_memcmp(a, b, a_size) == 0;
    if (!ok) {
        SDL_Log("pack and loose differ: %s", rel);
        rt->mismatches++;
    }
    rt->files++;
    br_free(a);
    br_free(b);
    return SDL_ENUM_CONTINUE;
}

static int roundtrip_main(const char *pack, const char *folder)
{
    roundtrip rt;
    char dir[1024];
    char root[1024];
    BR_CHECK(br_file_resolve_cwd(folder, dir, sizeof(dir))); /* command-line paths: the working directory */
    SDL_snprintf(root, sizeof(root), "%s/", dir);
    folder = dir;
    rt.root = root;
    rt.files = 0;
    rt.mismatches = 0;
    rt.packed = br_engine_create("test_pack", pack_path("empty_root"));
    rt.loose = br_engine_create("test_pack", folder);
    char pack_full[1024];
    BR_CHECK(br_file_resolve_cwd(pack, pack_full, sizeof(pack_full)));
    BR_CHECK(br_file_mount_pack(rt.packed, pack_full));
    BR_CHECK(SDL_EnumerateDirectory(root, compare_file, &rt));
    SDL_Log("pack round trip: %d files, %d differ", rt.files, rt.mismatches);
    BR_CHECK(rt.files > 0 && rt.mismatches == 0);
    br_engine_destroy(rt.loose);
    br_engine_destroy(rt.packed);
    return br_test_finish();
}

int main(int argc, char **argv)
{
    SDL_CreateDirectory(pack_path(""));
    SDL_CreateDirectory(pack_path("empty_root"));
    SDL_CreateDirectory(pack_path("loose_root"));
    SDL_CreateDirectory(pack_path("hostile_root"));
    if (argc == 3) {
        return roundtrip_main(argv[1], argv[2]);
    }
    SDL_GetLogOutputFunction(&g_prev_log, &g_prev_log_user);
    SDL_SetLogOutputFunction(capture_log, NULL);
    test_normalize();
    test_mount_and_read();
    test_loose_override();
    test_hostile_packs();
    test_async_from_pack();
    test_log_file();
    test_config_and_args();
    SDL_SetLogOutputFunction(g_prev_log, g_prev_log_user);
    return br_test_finish();
}
