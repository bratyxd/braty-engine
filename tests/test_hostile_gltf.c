/* test_hostile_gltf - glTF files built to break the loader: duplicate scene roots, accessor
 * counts beyond their data or absurdly large, out-of-range indices, missing buffers, cyclic node
 * trees, truncated .glb files, URIs leaving the model's folder. Each must fail cleanly with an
 * error naming the field, without crashing, hanging or allocating gigabytes. Headless. */
#include "braty/braty.h"
#include "br_test.h"
#include <SDL3/SDL.h>

static SDL_LogOutputFunction g_prev_log;
static void *g_prev_log_user;
static char g_last_error[512];

static void capture_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user;
    if (priority >= SDL_LOG_PRIORITY_ERROR) {
        SDL_strlcpy(g_last_error, message, sizeof(g_last_error));
    }
    g_prev_log(g_prev_log_user, category, priority, message);
}

/* Parses doc; true if it was rejected with an error containing `needle` (NULL = any error). */
static bool rejected(br_engine *e, const char *path, const void *doc, size_t size, const char *needle)
{
    g_last_error[0] = '\0';
    br_model *m = br_model_parse(e, path, doc, size);
    if (m != NULL) {
        br_model_release(e, m);
        br_log_info("%s: parsed, expected a rejection", path);
        return false;
    }
    if (needle != NULL && SDL_strstr(g_last_error, needle) == NULL) {
        br_log_info("%s: error '%s' does not name '%s'", path, g_last_error, needle);
        return false;
    }
    return g_last_error[0] != '\0';
}

static bool rejected_text(br_engine *e, const char *path, const char *json, const char *needle)
{
    return rejected(e, path, json, SDL_strlen(json), needle);
}

#define TRIANGLE_HEAD "{\"asset\":{\"version\":\"2.0\"},"
/* 36 bytes: three float VEC3 positions (all zero is fine for these tests) */
#define B64_36 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

static void test_scene_roots(br_engine *e)
{
    char json[1024];
    size_t n = (size_t)SDL_snprintf(json, sizeof(json), TRIANGLE_HEAD "\"nodes\":[{\"name\":\"a\"}],\"scene\":0,\"scenes\":[{\"nodes\":[");
    for (int i = 0; i < 64; i++) {
        n += (size_t)SDL_snprintf(json + n, sizeof(json) - n, i + 1 < 64 ? "0," : "0]}]}");
    }
    BR_CHECK(rejected(e, "dup_roots.gltf", json, n, "scenes[0].nodes"));
    /* the same node once is fine */
    const char *ok = TRIANGLE_HEAD "\"nodes\":[{\"name\":\"a\"}],\"scene\":0,\"scenes\":[{\"nodes\":[0]}]}";
    br_model *m = br_model_parse(e, "one_root.gltf", ok, SDL_strlen(ok));
    BR_CHECK(m != NULL && m->root_count == 1);
    br_model_release(e, m);
}

static void test_accessor_counts(br_engine *e)
{
    /* R6 repro: count 2^62 + 1 wraps cgltf's size check and br_gltf_floats' allocation */
    const char *huge =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":16,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAA==\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4,\"byteStride\":4},{\"buffer\":0,\"byteOffset\":4,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5121,\"normalized\":true,\"count\":4611686018427387905,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4611686018427387905,\"type\":\"VEC3\"}],"
        "\"nodes\":[{\"name\":\"n\"}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]}";
    BR_CHECK(rejected_text(e, "huge_count.gltf", huge, "accessors[0].count"));

    /* a plausible count that is just longer than its bufferView */
    const char *past =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64," B64_36 "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}]}";
    BR_CHECK(rejected_text(e, "past_view.gltf", past, "accessors[0]"));

    /* no bufferView: implicit zeros, 96 bytes per vertex would be gigabytes */
    const char *zeros = TRIANGLE_HEAD "\"accessors\":[{\"componentType\":5126,\"count\":44739243,\"type\":\"VEC3\"}],"
                        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}]}";
    BR_CHECK(rejected_text(e, "zero_count.gltf", zeros, "accessors[0].count"));

    /* a bufferView whose offset wraps past the buffer */
    const char *view =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64," B64_36 "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":18446744073709551600,\"byteLength\":36}]}";
    BR_CHECK(rejected_text(e, "view_wrap.gltf", view, "bufferViews[0]"));

    /* a sparse part larger than its accessor and its views */
    const char *sparse =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64," B64_36 "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":2305843009213693952,"
        "\"indices\":{\"bufferView\":0,\"componentType\":5125},\"values\":{\"bufferView\":0}}}]}";
    BR_CHECK(rejected_text(e, "sparse.gltf", sparse, "accessors[0].sparse"));

    /* a buffer claiming far more bytes than its data URI holds (would allocate 1 TB first) */
    const char *claim = TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":1099511627776,\"uri\":\"data:application/octet-stream;base64,AAAA\"}]}";
    BR_CHECK(rejected_text(e, "claim.gltf", claim, "buffers[0].byteLength"));

    /* skin inverse binds that are not MAT4 */
    const char *skin =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64," B64_36 "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"nodes\":[{\"name\":\"j\"}],\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":0}]}";
    BR_CHECK(rejected_text(e, "skin.gltf", skin, "skins[0].inverseBindMatrices"));
}

static void test_indices_and_buffers(br_engine *e)
{
    /* positions (36 bytes) + u16 indices 0, 1, 5 (6 bytes + 2 padding): index 5 of 3 vertices */
    const char *bad_index =
        TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":44,\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAUAAAA=\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],\"nodes\":[{\"mesh\":0}]}";
    BR_CHECK(rejected_text(e, "bad_index.gltf", bad_index, NULL));

    /* an external buffer that does not exist */
    const char *missing = TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36,\"uri\":\"missing_buffer.bin\"}],"
                          "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                          "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}]}";
    BR_CHECK(rejected_text(e, "missing.gltf", missing, "cannot load buffers"));

    /* a buffer with no uri in a .gltf (no GLB binary chunk): no data behind the accessor */
    const char *nodata = TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":36}],"
                         "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                         "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
                         "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}]}";
    BR_CHECK(rejected_text(e, "nodata.gltf", nodata, "no data"));

    /* cyclic node tree */
    const char *cycle = TRIANGLE_HEAD "\"nodes\":[{\"children\":[1]},{\"children\":[2]},{\"children\":[0]}]}";
    BR_CHECK(rejected_text(e, "cycle.gltf", cycle, NULL));
    const char *self = TRIANGLE_HEAD "\"nodes\":[{\"children\":[0]}]}";
    BR_CHECK(rejected_text(e, "self.gltf", self, NULL));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* A valid one-triangle .glb; every shorter prefix must be rejected. */
static void test_truncated_glb(br_engine *e)
{
    const char *json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":36}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                       "\"min\":[0,0,0],\"max\":[1,1,0]}],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                       "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    uint32_t json_len = (uint32_t)SDL_strlen(json);
    uint32_t json_padded = (json_len + 3u) & ~3u;
    uint32_t total = 12 + 8 + json_padded + 8 + 36;
    uint8_t glb[1024];
    BR_CHECK(total <= sizeof(glb));
    SDL_memset(glb, 0, sizeof(glb));
    put_u32(glb + 0, 0x46546C67u); /* "glTF" */
    put_u32(glb + 4, 2);
    put_u32(glb + 8, total);
    put_u32(glb + 12, json_padded);
    put_u32(glb + 16, 0x4E4F534Au); /* "JSON" */
    SDL_memcpy(glb + 20, json, json_len);
    SDL_memset(glb + 20 + json_len, ' ', json_padded - json_len);
    uint8_t *bin = glb + 20 + json_padded;
    put_u32(bin, 36);
    put_u32(bin + 4, 0x004E4942u); /* "BIN\0" */
    float pos[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    SDL_memcpy(bin + 8, pos, sizeof(pos));

    br_model *m = br_model_parse(e, "tri.glb", glb, total);
    BR_CHECK(m != NULL && m->primitive_count == 1);
    br_model_release(e, m);
    int parsed = 0;
    uint32_t cuts[] = { 0, 4, 11, 12, 19, 20, 21, 20 + json_len / 2, 20 + json_padded, 27 + json_padded,
                        28 + json_padded, total - 4, total - 1 };
    for (size_t i = 0; i < BR_ARRAY_COUNT(cuts); i++) {
        br_model *t = br_model_parse(e, "truncated.glb", glb, cuts[i]);
        parsed += t != NULL ? 1 : 0;
        br_model_release(e, t);
    }
    BR_CHECK(parsed == 0);
    /* a header that claims more bytes than there are */
    put_u32(glb + 8, 0xFFFFFFF0u);
    BR_CHECK(rejected(e, "long_header.glb", glb, total, NULL));
}

static void test_uris(br_engine *e)
{
    const char *bad[] = {
        "../secret.bin",
        "textures/../../secret.bin",
        "%2E%2E/secret.bin",
        "..\\\\secret.bin",
        "/etc/passwd",
        "C:/Windows/win.ini",
        "file:///etc/passwd",
    };
    for (size_t i = 0; i < BR_ARRAY_COUNT(bad); i++) {
        char doc[512];
        SDL_snprintf(doc, sizeof(doc), TRIANGLE_HEAD "\"buffers\":[{\"byteLength\":4,\"uri\":\"%s\"}]}", bad[i]);
        BR_CHECK(rejected_text(e, "models/escape.gltf", doc, "buffers[0].uri"));
        SDL_snprintf(doc, sizeof(doc), TRIANGLE_HEAD "\"images\":[{\"uri\":\"%s\"}]}", bad[i]);
        BR_CHECK(rejected_text(e, "models/escape.gltf", doc, "images[0].uri"));
    }
    /* a name that merely contains dots is fine (the file is missing: a warning, the model loads) */
    const char *dots = TRIANGLE_HEAD "\"images\":[{\"uri\":\"a..b/..c.png\"}]}";
    br_model *m = br_model_parse(e, "models/dots.gltf", dots, SDL_strlen(dots));
    BR_CHECK(m != NULL);
    br_model_release(e, m);
}

int main(void)
{
    SDL_GetLogOutputFunction(&g_prev_log, &g_prev_log_user);
    SDL_SetLogOutputFunction(capture_log, NULL);
    br_engine *e = br_engine_create("test_hostile_gltf", "../../tests/data"); /* relative to the exe */
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_scene_roots(e);
        test_accessor_counts(e);
        test_indices_and_buffers(e);
        test_truncated_glb(e);
        test_uris(e);
        br_engine_destroy(e);
    }
    br_memory_stats stats;
    br_memory_get_stats(&stats);
    BR_CHECK(stats.peak_bytes < 64u * 1024u * 1024u); /* no hostile file got a big allocation */
    SDL_SetLogOutputFunction(g_prev_log, g_prev_log_user);
    return br_test_finish();
}
