/* test_field_types - component field types beyond the first nine (long text, enum by name, vec4,
 * quaternion, 3D node reference, fixed-capacity arrays): registration checks, scene files, 3D
 * components (the glTF extras reader), play-test get / set, tweens, hot reload, saving, freeing
 * text with the row, and old files that keep loading (headless). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <string.h>

typedef struct guard {
    char     *line;           /* BR_FIELD_TEXT */
    int32_t   mode;           /* BR_FIELD_ENUM */
    br_vec4   glow;
    br_quat   turn;
    br_node3d marker;         /* BR_FIELD_NODE3D */
    uint32_t  point_count;
    br_vec2   points[4];      /* BR_FIELD_ARRAY of vec2 */
    uint32_t  weight_count;
    float     weights[3];     /* BR_FIELD_ARRAY of float */
    uint32_t  state_count;
    int32_t   states[2];      /* BR_FIELD_ARRAY of enum */
    uint32_t  friend_count;
    br_node   friends[2];     /* BR_FIELD_ARRAY of node */
    char      name[BR_FIELD_STRING_SIZE];
} guard;

static const char *const modes[] = { "idle", "patrol", "chase" };

static uint32_t register_guard(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "line", BR_FIELD_TEXT, offsetof(guard, line) },
        { "mode", BR_FIELD_ENUM, offsetof(guard, mode) },
        { "glow", BR_FIELD_VEC4, offsetof(guard, glow) },
        { "turn", BR_FIELD_QUAT, offsetof(guard, turn) },
        { "marker", BR_FIELD_NODE3D, offsetof(guard, marker) },
        { "points", BR_FIELD_ARRAY, offsetof(guard, points) },
        { "weights", BR_FIELD_ARRAY, offsetof(guard, weights) },
        { "states", BR_FIELD_ARRAY, offsetof(guard, states) },
        { "friends", BR_FIELD_ARRAY, offsetof(guard, friends) },
        { "name", BR_FIELD_STRING, offsetof(guard, name) },
    };
    br_component_desc desc = { "guard", sizeof(guard), fields, BR_ARRAY_COUNT(fields) };
    uint32_t id = br_component_register(e, &desc);
    BR_CHECK(id != 0);
    BR_CHECK(br_component_set_enum(e, id, "mode", modes, 3));
    BR_CHECK(br_component_set_array(e, id, "points", BR_FIELD_VEC2, 4, offsetof(guard, point_count)));
    BR_CHECK(br_component_set_array(e, id, "weights", BR_FIELD_FLOAT, 3, offsetof(guard, weight_count)));
    BR_CHECK(br_component_set_array(e, id, "states", BR_FIELD_ENUM, 2, offsetof(guard, state_count)));
    BR_CHECK(br_component_set_enum(e, id, "states", modes, 3));
    BR_CHECK(br_component_set_array(e, id, "friends", BR_FIELD_NODE, 2, offsetof(guard, friend_count)));
    return id;
}

static void test_register_checks(br_engine *e, uint32_t id)
{
    BR_CHECK(!br_component_set_enum(e, id, "glow", modes, 3));        /* not an enum */
    BR_CHECK(!br_component_set_enum(e, id, "mode", modes, 0));        /* no names */
    BR_CHECK(!br_component_set_enum(e, id, "nope", modes, 3));        /* no such field */
    BR_CHECK(!br_component_set_array(e, id, "glow", BR_FIELD_FLOAT, 4, offsetof(guard, point_count)));
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_VEC2, 1000, offsetof(guard, point_count))); /* past */
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_VEC2, 0, offsetof(guard, point_count)));
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_TEXT, 1, offsetof(guard, point_count)));
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_ARRAY, 1, offsetof(guard, point_count)));
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_VEC2, 4, offsetof(guard, points))); /* count inside */
    BR_CHECK(!br_component_set_array(e, id, "points", BR_FIELD_VEC2, 4, sizeof(guard)));            /* count past */
    BR_CHECK(!br_component_set_array(e, id, "weights", BR_FIELD_FLOAT, 0xFFFFFFFFu, offsetof(guard, point_count)));
    /* the failed calls kept what the good ones described */
    BR_CHECK(br_field_type_size(BR_FIELD_VEC4) == sizeof(br_vec4) && br_field_type_size(BR_FIELD_ARRAY) == 0);
    br_field_desc past[] = { { "far", BR_FIELD_QUAT, sizeof(guard) - 8 } };
    br_component_desc bad = { "bad_quat", sizeof(guard), past, 1 };
    BR_CHECK(br_component_register(e, &bad) == 0);
}

static br_node load(br_engine *e, const char *json)
{
    return br_scene_file_load_json(e, json, strlen(json), "fields.scene.json", br_stage_root(e));
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

static const char *scene =
    "{ \"label\": \"level\", \"children\": ["
    "  { \"label\": \"a\", \"components\": { \"guard\": {"
    "      \"line\": \"Halt! Who goes there?\", \"mode\": \"patrol\", \"glow\": [1, 2, 3, 4],"
    "      \"turn\": [0, 0.7071, 0, 0.7071], \"marker\": \"beacon\","
    "      \"points\": [[0, 0], [10, 0], { \"x\": 10, \"y\": 5 }], \"weights\": [0.5, 1.5],"
    "      \"states\": [\"chase\", 0], \"friends\": [\"b\", \"a\"], \"name\": \"Gus\" } } },"
    "  { \"label\": \"b\", \"components\": { \"guard\": { \"mode\": 2 } } },"
    "  { \"type\": \"view3d\", \"label\": \"world\", \"size\": [64, 64], \"scene\": ["
    "      { \"type\": \"group\", \"label\": \"beacon\" } ] } ] }";

static void test_scene_file(br_engine *e, uint32_t id)
{
    br_node root = load(e, scene);
    br_node a = br_node_find_by_label(e, root, "a", true);
    br_node b = br_node_find_by_label(e, root, "b", true);
    br_node world = br_node_find_by_label(e, root, "world", true);
    guard *g = br_component_get(e, a, id);
    BR_CHECK(g != NULL);
    if (g == NULL) {
        return;
    }
    BR_CHECK(g->line != NULL && strcmp(g->line, "Halt! Who goes there?") == 0);
    BR_CHECK(g->mode == 1);
    BR_CHECK(g->glow.x == 1.0f && g->glow.w == 4.0f);
    BR_CHECK(g->turn.y > 0.7f && g->turn.w > 0.7f);
    br_node3d beacon = br_node3d_find_by_label(e, br_view3d_root(e, world), "beacon", true);
    BR_CHECK(br_node3d_valid(e, beacon) && g->marker.index == beacon.index && g->marker.generation == beacon.generation);
    BR_CHECK(g->point_count == 3 && g->points[1].x == 10.0f && g->points[2].y == 5.0f);
    BR_CHECK(g->weight_count == 2 && g->weights[1] == 1.5f);
    BR_CHECK(g->state_count == 2 && g->states[0] == 2 && g->states[1] == 0);
    BR_CHECK(g->friend_count == 2 && br_node_equal(g->friends[0], b) && br_node_equal(g->friends[1], a));
    BR_CHECK(strcmp(g->name, "Gus") == 0);
    guard *gb = br_component_get(e, b, id);
    BR_CHECK(gb->mode == 2 && gb->line == NULL && gb->point_count == 0);

    /* write_field gives back what read_field takes, names for enums, labels for nodes */
    const br_component_desc *desc = br_component_desc_get(e, id);
    cJSON *mode = br_scene_file_write_field(e, &desc->fields[1], g);
    BR_CHECK(cJSON_IsString(mode) && strcmp(mode->valuestring, "patrol") == 0);
    cJSON_Delete(mode);
    for (uint32_t i = 0; i < desc->field_count; i++) {
        cJSON *v = br_scene_file_write_field(e, &desc->fields[i], br_component_get(e, a, id));
        BR_CHECK(v != NULL);
        BR_CHECK(br_scene_file_read_field(e, b, id, &desc->fields[i], v));
        cJSON_Delete(v);
    }
    g = br_component_get(e, a, id);
    gb = br_component_get(e, b, id);
    BR_CHECK(gb->line != g->line && strcmp(gb->line, g->line) == 0); /* a copy, not the same pointer */
    BR_CHECK(gb->mode == g->mode && gb->point_count == 3 && gb->points[2].y == 5.0f);
    BR_CHECK(gb->state_count == 2 && gb->states[0] == 2 && br_node_equal(gb->friends[0], b));
    BR_CHECK(gb->marker.index == beacon.index);
    br_node_destroy(e, root);
    br_engine_step(e, 1.0f / 60.0f);
}

static void test_errors(br_engine *e, uint32_t id)
{
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    BR_CHECK(br_component_add(e, n, id) != NULL);
    const br_component_desc *desc = br_component_desc_get(e, id);
    cJSON *v = cJSON_Parse("\"sprint\"");
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[1], v));
    BR_CHECK(error_has(e, "expected one of: idle, patrol, chase"));
    cJSON_Delete(v);
    v = cJSON_Parse("7");
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[1], v)); /* index out of range */
    cJSON_Delete(v);
    v = cJSON_Parse("[[1, 1], [2, 2], [3, 3], [4, 4], [5, 5]]");
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[5], v));
    BR_CHECK(error_has(e, "5 elements, the field holds at most 4"));
    cJSON_Delete(v);
    v = cJSON_Parse("[1, 2, 3]");
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[2], v));
    BR_CHECK(error_has(e, "expected [x, y, z, w]"));
    cJSON_Delete(v);
    v = cJSON_Parse("\"nowhere\"");
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[4], v));
    BR_CHECK(error_has(e, "no 3D node labelled 'nowhere'"));
    cJSON_Delete(v);
    /* a 63-byte string field still refuses longer text, loudly, and says what holds it */
    char long_name[200];
    SDL_memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    v = cJSON_CreateString(long_name);
    BR_CHECK(!br_scene_file_read_field(e, n, id, &desc->fields[9], v));
    BR_CHECK(error_has(e, "BR_FIELD_TEXT"));
    cJSON_Delete(v);
    br_node_destroy(e, n);
    br_engine_step(e, 1.0f / 60.0f);

    /* an array field nobody described cannot be read (and says how to fix it) */
    typedef struct bare { uint32_t count; float items[2]; } bare;
    static const br_field_desc bare_fields[] = { { "items", BR_FIELD_ARRAY, offsetof(bare, items) } };
    br_component_desc bare_desc = { "bare", sizeof(bare), bare_fields, 1 };
    BR_CHECK(br_component_register(e, &bare_desc) != 0);
    br_node r = load(e, "{ \"components\": { \"bare\": { \"items\": [1] } } }");
    BR_CHECK(error_has(e, "br_component_set_array"));
    br_node_destroy(e, r);
    br_engine_step(e, 1.0f / 60.0f);
}

/* Text far longer than any fixed buffer: kept whole, freed with the row, no leak. */
static void test_long_text(br_engine *e, uint32_t id)
{
    br_memory_stats before;
    br_memory_get_stats(&before);
    size_t len = 100000;
    char *big = SDL_malloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        big[i] = (char)('a' + (char)(i % 26));
    }
    big[len] = '\0';
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    br_component_add(e, n, id);
    const br_component_desc *desc = br_component_desc_get(e, id);
    cJSON *v = cJSON_CreateString(big);
    BR_CHECK(br_scene_file_read_field(e, n, id, &desc->fields[0], v));
    BR_CHECK(br_scene_file_read_field(e, n, id, &desc->fields[0], v)); /* re-read frees the old copy */
    cJSON_Delete(v);
    guard *g = br_component_get(e, n, id);
    BR_CHECK(g->line != NULL && strlen(g->line) == len && memcmp(g->line, big, len) == 0);
    cJSON *out = br_scene_file_write_field(e, &desc->fields[0], g);
    BR_CHECK(cJSON_IsString(out) && strlen(out->valuestring) == len);
    cJSON_Delete(out);
    BR_CHECK(br_component_set_text(&g->line, "short"));
    BR_CHECK(strcmp(g->line, "short") == 0);
    br_component_remove(e, n, id); /* frees the text */
    br_node_destroy(e, n);
    br_engine_step(e, 1.0f / 60.0f);
    SDL_free(big);
    br_memory_stats after;
    br_memory_get_stats(&after);
    BR_CHECK(after.live_allocations == before.live_allocations);

    /* swap-remove frees the removed row's text and keeps the moved row's */
    br_node x = br_node_create_container(e);
    br_node y = br_node_create_container(e);
    guard *gx = br_component_add(e, x, id);
    br_component_set_text(&gx->line, "first");
    guard *gy = br_component_add(e, y, id);
    br_component_set_text(&gy->line, "second");
    br_component_remove(e, x, id);
    gy = br_component_get(e, y, id);
    BR_CHECK(gy != NULL && strcmp(gy->line, "second") == 0);
    br_node_destroy(e, x);
    br_node_destroy(e, y); /* destroying the node frees it too */
    br_engine_step(e, 1.0f / 60.0f);
    br_memory_get_stats(&after);
    BR_CHECK(after.live_allocations == before.live_allocations);
}

/* 3D components (view3d JSON uses the glTF extras reader): every new type, names under the view. */
static void test_components3d(br_engine *e, uint32_t id, bool measure)
{
    br_memory_stats before;
    br_memory_get_stats(&before);
    br_node root = load(e, "{ \"type\": \"view3d\", \"size\": [64, 64], \"scene\": ["
                           "  { \"type\": \"group\", \"label\": \"crate\", \"components\": { \"guard\": {"
                           "      \"line\": \"Ein sehr langer deutscher Satz, der nicht in dreiundsechzig Bytes passt.\","
                           "      \"mode\": \"chase\", \"glow\": [0, 1, 0, 1], \"marker\": \"lamp\","
                           "      \"weights\": [1, 2, 3], \"states\": [\"idle\"] } } },"
                           "  { \"type\": \"group\", \"label\": \"lamp\" } ] }");
    br_node3d crate = br_node3d_find_by_label(e, br_view3d_root(e, root), "crate", true);
    br_node3d lamp = br_node3d_find_by_label(e, br_view3d_root(e, root), "lamp", true);
    guard *g = br_component3d_get(e, crate, id);
    BR_CHECK(g != NULL);
    if (g != NULL) {
        BR_CHECK(g->line != NULL && strlen(g->line) > 63);
        BR_CHECK(g->mode == 2 && g->glow.y == 1.0f);
        BR_CHECK(g->marker.index == lamp.index && g->marker.generation == lamp.generation);
        BR_CHECK(g->weight_count == 3 && g->weights[2] == 3.0f && g->state_count == 1 && g->states[0] == 0);
    }
    br_node_destroy(e, root);
    br_engine_step(e, 1.0f / 60.0f);
    br_memory_stats after;
    br_memory_get_stats(&after);
    /* the 3D row's text went with it (the first run allocates the 3D rows' tables) */
    BR_CHECK(!measure || after.live_allocations == before.live_allocations);
}

static void test_playtest(br_engine *e, uint32_t id)
{
    br_node root = load(e, scene);
    cJSON *v = br_playtest_get(e, "a:guard.mode");
    BR_CHECK(cJSON_IsString(v) && strcmp(v->valuestring, "patrol") == 0);
    cJSON_Delete(v);
    v = br_playtest_get(e, "a:guard.points.1");
    BR_CHECK(cJSON_IsArray(v) && cJSON_GetArrayItem(v, 0)->valuedouble == 10.0);
    cJSON_Delete(v);
    v = br_playtest_get(e, "a:guard.glow.w");
    BR_CHECK(cJSON_IsNumber(v) && v->valuedouble == 4.0);
    cJSON_Delete(v);
    v = br_playtest_get(e, "a:guard.marker");
    BR_CHECK(cJSON_IsString(v) && strcmp(v->valuestring, "beacon") == 0);
    cJSON_Delete(v);

    cJSON *chase = cJSON_CreateString("chase");
    BR_CHECK(br_playtest_set(e, "a:guard.mode", chase));
    cJSON_Delete(chase);
    cJSON *bad = cJSON_CreateString("dance");
    BR_CHECK(!br_playtest_set(e, "a:guard.mode", bad));
    cJSON_Delete(bad);
    cJSON *text = cJSON_CreateString("Ein sehr langer Satz, der in keinen Puffer mit 63 Bytes passt, und doch ganz bleibt.");
    BR_CHECK(br_playtest_set(e, "a:guard.line", text));
    cJSON_Delete(text);
    cJSON *seven = cJSON_CreateNumber(7);
    BR_CHECK(br_playtest_set(e, "a:guard.weights.0", seven)); /* one element of an array */
    cJSON_Delete(seven);
    cJSON *list = cJSON_Parse("[1, 2, 3]");
    BR_CHECK(br_playtest_set(e, "a:guard.weights", list));
    cJSON_Delete(list);
    cJSON *too_many = cJSON_Parse("[1, 2, 3, 4]");
    BR_CHECK(!br_playtest_set(e, "a:guard.weights", too_many));
    cJSON_Delete(too_many);
    guard *g = br_component_get(e, br_node_find_by_label(e, root, "a", true), id);
    BR_CHECK(g->mode == 2 && strlen(g->line) > 63 && g->weight_count == 3 && g->weights[2] == 3.0f);
    br_node_destroy(e, root);
    br_engine_step(e, 1.0f / 60.0f);
}

static void test_tween(br_engine *e, uint32_t id)
{
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    guard *g = br_component_add(e, n, id);
    g->weight_count = 3;
    float to4[4] = { 4, 8, 12, 16 };
    br_tween_to(e, br_anim_target_2d(n), "guard.glow", to4, 1.0f, BR_EASE_LINEAR);
    br_tween_to_f(e, br_anim_target_2d(n), "guard.weights.2", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_to_f(e, br_anim_target_2d(n), "guard.mode", 2.0f, 1.0f, BR_EASE_LINEAR);
    for (int i = 0; i < 30; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    g = br_component_get(e, n, id);
    BR_CHECK(g->glow.x > 1.5f && g->glow.x < 2.5f && g->glow.w > 7.0f && g->glow.w < 9.0f);
    BR_CHECK(g->weights[2] > 4.0f && g->weights[2] < 6.0f && g->weights[0] == 0.0f);
    for (int i = 0; i < 40; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    BR_CHECK(g->mode == 2 && g->weights[2] == 10.0f);
    /* not animatable: text, an element past the capacity, a whole array */
    BR_CHECK(!br_tween_alive(e, br_tween_to_f(e, br_anim_target_2d(n), "guard.weights.3", 1.0f, 1.0f, BR_EASE_LINEAR)));
    BR_CHECK(!br_tween_alive(e, br_tween_to_f(e, br_anim_target_2d(n), "guard.weights", 1.0f, 1.0f, BR_EASE_LINEAR)));
    BR_CHECK(!br_tween_alive(e, br_tween_to_f(e, br_anim_target_2d(n), "guard.line", 1.0f, 1.0f, BR_EASE_LINEAR)));
    br_node_destroy(e, n);
    br_engine_step(e, 1.0f / 60.0f);
}

/* Saved files keep the new types; hot reload re-reads them (text replaced, nothing leaked). */
static void test_save_and_reload(br_engine *e, uint32_t id, const char *dir)
{
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%stest_field_types.scene.json", dir);
    br_node src = load(e, scene);
    BR_CHECK(br_scene_file_save(e, src, path));
    br_node_destroy(e, src);
    br_engine_step(e, 1.0f / 60.0f);
    br_node loaded = br_scene_file_load(e, path, br_stage_root(e));
    guard *g = br_component_get(e, br_node_find_by_label(e, loaded, "a", true), id);
    BR_CHECK(g != NULL && g->line != NULL && strcmp(g->line, "Halt! Who goes there?") == 0);
    BR_CHECK(g != NULL && g->mode == 1 && g->point_count == 3 && g->state_count == 2 && g->friend_count == 2);

    SDL_Delay(20); /* a different modification time */
    const char *edited = "{ \"label\": \"level\", \"components\": { \"guard\": { \"line\": \"Stop.\", \"mode\": \"idle\" } } }";
    BR_CHECK(br_file_write_all(e, path, edited, strlen(edited)));
    BR_CHECK(br_scene_file_poll(e) == 1);
    br_engine_step(e, 1.0f / 60.0f);
    g = br_component_get(e, loaded, id);
    BR_CHECK(g != NULL && g->line != NULL && strcmp(g->line, "Stop.") == 0 && g->mode == 0);
    br_node_destroy(e, loaded);
    br_engine_step(e, 1.0f / 60.0f);
}

/* Review focus: old scene / save files keep loading. A file written before the new types (63-byte
 * string fields) loads into a component whose field became BR_FIELD_TEXT, and an old component
 * keeps reading and writing exactly what it did. */
typedef struct sign_old { char text[BR_FIELD_STRING_SIZE]; int32_t size; } sign_old;
typedef struct sign_new { char *text; int32_t size; } sign_new;

static void test_old_files(br_engine *e)
{
    static const br_field_desc old_fields[] = {
        { "text", BR_FIELD_STRING, offsetof(sign_old, text) },
        { "size", BR_FIELD_INT, offsetof(sign_old, size) },
    };
    static const br_field_desc new_fields[] = {
        { "text", BR_FIELD_TEXT, offsetof(sign_new, text) },
        { "size", BR_FIELD_INT, offsetof(sign_new, size) },
    };
    br_component_desc d_old = { "sign_old", sizeof(sign_old), old_fields, 2 };
    br_component_desc d_new = { "sign_new", sizeof(sign_new), new_fields, 2 };
    uint32_t id_old = br_component_register(e, &d_old);
    uint32_t id_new = br_component_register(e, &d_new);
    const char *old_file = "{ \"children\": [ { \"label\": \"s\", \"components\": {"
                           " \"sign_old\": { \"text\": \"PUSH CRATES\", \"size\": 3 },"
                           " \"sign_new\": { \"text\": \"PUSH CRATES\", \"size\": 3 } } } ] }";
    uint32_t errors = br_scene_file_error_count(e);
    br_node root = load(e, old_file);
    BR_CHECK(br_scene_file_error_count(e) == errors);
    br_node s = br_node_find_by_label(e, root, "s", true);
    sign_old *so = br_component_get(e, s, id_old);
    sign_new *sn = br_component_get(e, s, id_new);
    BR_CHECK(so != NULL && strcmp(so->text, "PUSH CRATES") == 0 && so->size == 3);
    BR_CHECK(sn != NULL && sn->text != NULL && strcmp(sn->text, "PUSH CRATES") == 0 && sn->size == 3);
    cJSON *saved = br_scene_file_to_json(e, root);
    char *printed = cJSON_PrintUnformatted(saved);
    BR_CHECK(printed != NULL && strstr(printed, "\"sign_old\":{\"text\":\"PUSH CRATES\",\"size\":3}") != NULL);
    BR_CHECK(printed != NULL && strstr(printed, "\"sign_new\":{\"text\":\"PUSH CRATES\",\"size\":3}") != NULL);
    cJSON_free(printed);
    cJSON_Delete(saved);
    br_node_destroy(e, root);
    br_engine_step(e, 1.0f / 60.0f);
}

int main(void)
{
    br_engine *e = br_engine_create("test_field_types", BRATY_SOURCE_DIR "/examples");
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    char dir[1024];
    const char *base = SDL_GetBasePath();
    SDL_strlcpy(dir, base != NULL ? base : "", sizeof(dir));
    uint32_t id = register_guard(e);
    test_register_checks(e, id);
    test_scene_file(e, id);
    test_errors(e, id);
    test_long_text(e, id);
    test_components3d(e, id, false);
    test_components3d(e, id, true);
    test_playtest(e, id);
    test_tween(e, id);
    test_save_and_reload(e, id, dir);
    test_old_files(e);
    br_engine_destroy(e);
    return br_test_finish();
}
