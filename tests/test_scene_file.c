/* test_scene_file - scene files, component fields, label references, prefabs, game.json,
 * hot reload, debug log capture (headless; assets from examples/). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <string.h>

/* ---- a component using every field type */

typedef struct every_field {
    bool     flag;
    int32_t  count;
    float    speed;
    br_vec2  velocity;
    br_vec3  axis;
    br_color color;
    char     name[BR_FIELD_STRING_SIZE];
    char     sound[BR_FIELD_STRING_SIZE];
    br_node  target;
} every_field;

static uint32_t register_every_field(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "flag", BR_FIELD_BOOL, offsetof(every_field, flag) },
        { "count", BR_FIELD_INT, offsetof(every_field, count) },
        { "speed", BR_FIELD_FLOAT, offsetof(every_field, speed) },
        { "velocity", BR_FIELD_VEC2, offsetof(every_field, velocity) },
        { "axis", BR_FIELD_VEC3, offsetof(every_field, axis) },
        { "color", BR_FIELD_COLOR, offsetof(every_field, color) },
        { "name", BR_FIELD_STRING, offsetof(every_field, name) },
        { "sound", BR_FIELD_ASSET, offsetof(every_field, sound) },
        { "target", BR_FIELD_NODE, offsetof(every_field, target) },
    };
    br_component_desc desc = { "every", sizeof(every_field), fields, BR_ARRAY_COUNT(fields) };
    return br_component_register(e, &desc);
}

static br_node load(br_engine *e, const char *json)
{
    return br_scene_file_load_json(e, json, strlen(json), "test.scene.json", br_scene_root(e));
}

static br_node find(br_engine *e, br_node root, const char *label)
{
    return br_node_find_by_label(e, root, label, true);
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

static uint32_t count_nodes(br_engine *e, br_node n)
{
    uint32_t total = 1;
    for (uint32_t i = 0; i < br_node_child_count(e, n); i++) {
        br_node c = br_node_child_at(e, n, i);
        if (br_node_valid(e, c)) {
            total += count_nodes(e, c);
        }
    }
    return total;
}

/* ---- every node type: load -> save -> load gives the same JSON */

static const char *every_type_scene =
    "{ \"type\": \"container\", \"label\": \"root\", \"assets\": [\"data/atlas.json\"], \"children\": ["
    " { \"type\": \"sprite\", \"label\": \"hero\", \"texture\": \"data/atlas.json#hero.png\", \"position\": [10, 20],"
    "   \"scale\": [2, 3], \"angle\": 90, \"tint\": \"#ff8800\", \"alpha\": 0.5, \"z_index\": 3, \"blend\": \"add\","
    "   \"pivot\": [1, 2], \"skew\": [0.1, 0], \"visible\": false, \"event_mode\": \"static\", \"cursor\": \"pointer\","
    "   \"hit_area\": { \"circle\": [0, 0, 10] }, \"on\": { \"click\": \"hit\" },"
    "   \"components\": { \"every\": { \"target\": \"panel\", \"count\": 7 } } },"
    " { \"type\": \"animated_sprite\", \"label\": \"walker\", \"animation\": \"data/atlas.json#walk\", \"fps\": 8 },"
    " { \"type\": \"animated_sprite\", \"label\": \"frames\", \"frames\": [\"gem.png\", { \"texture\": \"spark.png\", \"duration\": 0.5 }],"
    "   \"loop\": false, \"playing\": false },"
    " { \"type\": \"nine_slice_sprite\", \"label\": \"panel\", \"texture\": \"data/atlas.json#panel.png\", \"width\": 100,"
    "   \"height\": 50, \"edge_fill\": \"tile\" },"
    " { \"type\": \"tiling_sprite\", \"label\": \"tiles\", \"texture\": \"data/stripes.png\", \"width\": 64, \"height\": 32,"
    "   \"tile_scale\": [2, 2], \"mask\": { \"node\": \"shape\", \"kind\": \"scissor\" } },"
    " { \"type\": \"graphics\", \"label\": \"shape\", \"commands\": [[\"rect\", 0, 0, 10, 10], [\"fill\", \"red\"],"
    "   [\"circle\", 5, 5, 2], [\"cut\"], [\"stroke\", { \"width\": 2, \"color\": \"blue\", \"dash\": [2, 1] }]] },"
    " { \"type\": \"graphics\", \"label\": \"arrow\", \"svg\": \"M0 0 L10 0 L5 8 Z\", \"fill\": { \"linear\": [0, 0, 1, 1],"
    "   \"stops\": [[0, \"white\"], [1, \"black\"]] }, \"filters\": [{ \"type\": \"blur\", \"params\": { \"strength\": 4 } },"
    "   { \"type\": \"color_matrix\", \"presets\": [\"sepia\"] }] },"
    " { \"type\": \"mesh\", \"label\": \"tri\", \"positions\": [0, 0, 10, 0, 0, 10], \"colors\": [\"red\", \"lime\", \"blue\"] },"
    " { \"type\": \"mesh\", \"label\": \"plane\", \"texture\": \"data/stripes.png\", \"plane\": [3, 4] },"
    " { \"type\": \"mesh\", \"label\": \"persp\", \"texture\": \"data/stripes.png\","
    "   \"perspective\": { \"verts\": [4, 4], \"corners\": [[0, 0], [10, 0], [12, 10], [0, 9]] } },"
    " { \"type\": \"mesh\", \"label\": \"rope\", \"texture\": \"data/stripes.png\", \"rope\": { \"points\": [0, 0, 10, 5, 20, 0], \"width\": 4 } },"
    " { \"type\": \"particle_container\", \"label\": \"sparks\", \"texture\": \"spark.png\", \"dynamic\": [\"position\", \"color\"],"
    "   \"particles\": [{ \"position\": [1, 2], \"tint\": \"red\" }, { \"position\": [3, 4], \"scale\": 2 }] },"
    " { \"type\": \"text\", \"label\": \"title\", \"text\": \"Hello\", \"anchor\": [0.5, 0.5],"
    "   \"style\": { \"font_size\": 20, \"fill\": \"white\", \"stroke\": { \"width\": 2, \"color\": \"black\" }, \"align\": \"center\" } },"
    " { \"type\": \"bitmap_text\", \"label\": \"score\", \"text\": \"SCORE 1\", \"font\": \"fonts/pixel5x7.fnt\", \"size\": 14 },"
    " { \"type\": \"rich_text\", \"label\": \"rich\", \"text\": \"<b>a</b> <warn>b</warn>\","
    "   \"tag_styles\": { \"warn\": { \"color\": \"red\" } } }"
    "] }";

static void hit_handler(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
}

static void test_every_type_round_trip(br_engine *e)
{
    br_scene_file_add_handler(e, "hit", hit_handler, NULL);
    uint32_t errors = br_scene_file_error_count(e);
    br_node root = load(e, every_type_scene);
    BR_CHECK(br_node_valid(e, root));
    BR_CHECK(br_scene_file_error_count(e) == errors);
    if (br_scene_file_error_count(e) != errors) {
        printf("  unexpected error: %s\n", br_scene_file_last_error(e));
    }
    BR_CHECK(br_node_child_count(e, root) == 15);

    /* live values the loader set */
    br_node hero = find(e, root, "hero");
    BR_CHECK(br_node_position(e, hero).x == 10.0f && br_node_position(e, hero).y == 20.0f);
    BR_CHECK(br_node_scale(e, hero).y == 3.0f);
    BR_CHECK(br_nearly_equal(br_node_rotation(e, hero), SDL_PI_F / 2.0f, 1e-5f));
    BR_CHECK(br_color_to_hex(br_node_tint(e, hero)) == 0xff8800);
    BR_CHECK(br_node_blend_mode(e, hero) == BR_BLEND_ADD);
    BR_CHECK(!br_node_visible(e, hero));
    BR_CHECK(br_node_event_mode(e, hero) == BR_EVENT_MODE_STATIC);
    BR_CHECK(br_node_hit_area(e, hero).type == BR_HIT_AREA_CIRCLE);
    BR_CHECK(br_node_listener_count(e, hero) == 1);
    BR_CHECK(br_sprite_anchor(e, hero).x == 0.5f); /* the frame's pivot from the atlas */
    every_field *f = br_component_get(e, hero, br_component_find(e, "every"));
    BR_CHECK(f != NULL && f->count == 7 && br_node_equal(f->target, find(e, root, "panel")));
    BR_CHECK(br_animated_sprite_total_frames(e, find(e, root, "walker")) == 4);
    BR_CHECK(br_animated_sprite_playing(e, find(e, root, "walker")));
    BR_CHECK(!br_animated_sprite_playing(e, find(e, root, "frames")));
    BR_CHECK(br_nine_slice_get(e, find(e, root, "panel"))->left == 8.0f); /* borders from the sheet */
    BR_CHECK(br_nine_slice_get(e, find(e, root, "panel"))->edge_fill == BR_NINE_SLICE_TILE);
    BR_CHECK(br_tiling_sprite_get(e, find(e, root, "tiles"))->tile_scale.x == 2.0f);
    br_mask_kind kind = BR_MASK_NONE;
    BR_CHECK(br_node_equal(br_node_mask(e, find(e, root, "tiles"), &kind, NULL), find(e, root, "shape")));
    BR_CHECK(kind == BR_MASK_SCISSOR);
    br_graphics_geometry_info gi;
    br_graphics_context_get_geometry(br_graphics_context_of(e, find(e, root, "shape")), &gi);
    BR_CHECK(gi.triangle_count > 0);
    BR_CHECK(br_filters_count(e, find(e, root, "arrow")) == 2);
    uint32_t count = 0;
    br_mesh_positions(e, find(e, root, "plane"), &count);
    BR_CHECK(count == 12);
    BR_CHECK(br_mesh_rope_points(e, find(e, root, "rope"), &count) != NULL && count == 3);
    br_particle_container_particles(e, find(e, root, "sparks"), &count);
    BR_CHECK(count == 2);
    BR_CHECK(strcmp(br_text_get_text(e, find(e, root, "title")), "Hello") == 0);
    br_text_style style;
    BR_CHECK(br_text_get_style(e, find(e, root, "title"), &style) && style.font_size == 20.0f &&
             style.stroke_width == 2.0f && style.align == BR_TEXT_ALIGN_CENTER);
    BR_CHECK(br_text_get_style(e, find(e, root, "score"), &style) && style.font != NULL &&
             br_font_is_bitmap(style.font));
    BR_CHECK(strcmp(br_rich_text_plain(e, find(e, root, "rich")), "a b") == 0);

    /* load -> save -> load: the second save equals the first */
    cJSON *first = br_scene_file_to_json(e, root);
    char *text = cJSON_Print(first);
    br_node again = load(e, text);
    cJSON *second = br_scene_file_to_json(e, again);
    BR_CHECK(br_scene_file_error_count(e) == errors);
    BR_CHECK(cJSON_Compare(first, second, true));
    BR_CHECK(count_nodes(e, root) == count_nodes(e, again));
    br_node hero2 = find(e, again, "hero");
    BR_CHECK(br_nearly_equal(br_node_rotation(e, hero2), SDL_PI_F / 2.0f, 1e-5f));
    BR_CHECK(br_color_to_hex(br_node_tint(e, hero2)) == 0xff8800);
    f = br_component_get(e, hero2, br_component_find(e, "every"));
    BR_CHECK(f != NULL && br_node_equal(f->target, find(e, again, "panel")));

    cJSON_free(text);
    cJSON_Delete(first);
    cJSON_Delete(second);
    br_node_destroy(e, root);
    br_node_destroy(e, again);
}

/* ---- asset collection */

static void collect_path(const char *path, void *user)
{
    char *list = user;
    SDL_strlcat(list, path, 1024);
    SDL_strlcat(list, ";", 1024);
}

static void test_collect_assets(br_engine *e)
{
    char list[1024] = "";
    cJSON *doc = cJSON_Parse(every_type_scene);
    br_scene_file_collect_assets(e, doc, collect_path, list);
    cJSON_Delete(doc);
    BR_CHECK(strstr(list, "data/atlas.json;") != NULL);
    BR_CHECK(strstr(list, "data/stripes.png;") != NULL);
    BR_CHECK(strstr(list, "fonts/pixel5x7.fnt;") != NULL);
    BR_CHECK(strstr(list, "#") == NULL); /* frame references report their sheet */
}

/* ---- component fields of every type */

static void test_component_fields(br_engine *e)
{
    uint32_t id = br_component_find(e, "every");
    br_node root = load(e, "{ \"children\": ["
                           " { \"label\": \"a\", \"components\": { \"every\": {"
                           "   \"flag\": true, \"count\": -3, \"speed\": 2.5, \"velocity\": [1, 2], \"axis\": [3, 4, 5],"
                           "   \"color\": \"#336699\", \"name\": \"bob\", \"sound\": \"sounds/jump.wav\", \"target\": \"b\" } } },"
                           " { \"label\": \"b\", \"components\": { \"every\": { \"velocity\": { \"x\": 7, \"y\": 8 },"
                           "   \"color\": [1, 0, 0, 0.5], \"target\": null } } } ] }");
    br_node a = find(e, root, "a");
    br_node b = find(e, root, "b");
    every_field *fa = br_component_get(e, a, id);
    BR_CHECK(fa != NULL);
    if (fa == NULL) {
        return;
    }
    BR_CHECK(fa->flag);
    BR_CHECK(fa->count == -3);
    BR_CHECK(fa->speed == 2.5f);
    BR_CHECK(fa->velocity.x == 1.0f && fa->velocity.y == 2.0f);
    BR_CHECK(fa->axis.x == 3.0f && fa->axis.y == 4.0f && fa->axis.z == 5.0f);
    BR_CHECK(br_color_to_hex(fa->color) == 0x336699);
    BR_CHECK(strcmp(fa->name, "bob") == 0);
    BR_CHECK(strcmp(fa->sound, "sounds/jump.wav") == 0);
    BR_CHECK(br_node_equal(fa->target, b)); /* referenced before b was built */
    every_field *fb = br_component_get(e, b, id);
    BR_CHECK(fb->velocity.x == 7.0f && fb->velocity.y == 8.0f);
    BR_CHECK(fb->color.a == 0.5f);
    BR_CHECK(fb->target.generation == 0);

    /* write_field gives back what read_field takes */
    const br_component_desc *desc = br_component_get_desc(e, id);
    for (uint32_t i = 0; i < desc->field_count; i++) {
        cJSON *v = br_scene_file_write_field(e, &desc->fields[i], fa);
        BR_CHECK(v != NULL);
        BR_CHECK(br_scene_file_read_field(e, b, id, &desc->fields[i], v));
        cJSON_Delete(v);
    }
    fa = br_component_get(e, a, id);
    fb = br_component_get(e, b, id);
    BR_CHECK(memcmp(fa, fb, sizeof(every_field)) == 0);
    br_node_destroy(e, root);
}

/* ---- errors: file, JSON path and reason; bad nodes skipped, the rest loads */

static void test_errors(br_engine *e)
{
    uint32_t errors = br_scene_file_error_count(e);
    br_node bad = load(e, "{ \"children\": [ { \"type\": \"sprite\", ] }");
    BR_CHECK(!br_node_valid(e, bad));
    BR_CHECK(error_has(e, "test.scene.json: JSON syntax error at line 1"));

    br_node root = load(e, "{ \"label\": \"root\", \"children\": ["
                           " { \"label\": \"ok0\", \"tint\": \"notacolour\" },"
                           " { \"type\": \"no_such_type\", \"label\": \"skipped\" },"
                           " { \"label\": \"ok2\", \"components\": { \"every\": { \"speed\": \"fast\" } } },"
                           " 42,"
                           " { \"label\": \"ok4\", \"position\": [1], \"children\": ["
                           "   { \"type\": \"graphics\", \"commands\": [[\"rect\", 0, 0], [\"spin\", 1]] } ] },"
                           " { \"label\": \"ok5\", \"components\": { \"nope\": {}, \"every\": { \"wings\": 2, \"target\": \"ghost\" } } }"
                           "] }");
    BR_CHECK(br_node_valid(e, root));
    BR_CHECK(br_node_child_count(e, root) == 4); /* the unknown type and the 42 are skipped */
    BR_CHECK(br_node_valid(e, find(e, root, "ok0")));
    BR_CHECK(!br_node_valid(e, find(e, root, "skipped")));
    BR_CHECK(br_node_valid(e, find(e, root, "ok5")));
    BR_CHECK(br_scene_file_error_count(e) - errors == 11);

    /* each error message has the path; check a few by re-loading single cases */
    load(e, "{ \"children\": [ {}, {}, { \"components\": { \"every\": { \"speed\": \"fast\" } } } ] }");
    BR_CHECK(error_has(e, "test.scene.json: children[2].components.every.speed: expected a number"));
    load(e, "{ \"children\": [ { \"tint\": \"notacolour\" } ] }");
    BR_CHECK(error_has(e, "children[0].tint: 'notacolour' is not a colour"));
    load(e, "{ \"children\": [ { \"type\": \"no_such_type\" } ] }");
    BR_CHECK(error_has(e, "children[0].type: unknown node type 'no_such_type'"));
    load(e, "{ \"children\": [ { \"type\": \"graphics\", \"commands\": [[\"rect\", 0], [\"rect\", 0, 0]] } ] }");
    BR_CHECK(error_has(e, "children[0].commands[1]: bad arguments, expected [\"rect\", x, y, w, h]"));
    load(e, "{ \"children\": [ { \"components\": { \"every\": { \"target\": \"ghost\" } } } ] }");
    BR_CHECK(error_has(e, "children[0].components.every.target: no node labelled 'ghost'"));
    load(e, "{ \"children\": [ { \"components\": { \"every\": { \"wings\": 1 } } } ] }");
    BR_CHECK(error_has(e, "children[0].components.every.wings: component 'every' has no field 'wings'"));
    load(e, "{ \"children\": [ { \"type\": \"sprite\", \"texture\": \"data/atlas.json#nope.png\" } ] }");
    BR_CHECK(error_has(e, "children[0].texture: no frame 'nope.png' in spritesheet 'data/atlas.json'"));
    load(e, "{ \"filters\": [ { \"type\": \"blur\" }, { \"type\": \"glow\" } ] }");
    BR_CHECK(error_has(e, "filters[1].type: unknown value 'glow'"));
    br_node_destroy(e, root);
}

/* ---- prefabs */

static void test_prefabs(br_engine *e, const char *dir)
{
    char prefab_path[1024];
    SDL_snprintf(prefab_path, sizeof(prefab_path), "%stest_enemy.prefab.json", dir);
    const char *prefab = "{ \"label\": \"enemy\", \"tint\": \"red\","
                         "  \"components\": { \"every\": { \"speed\": 1, \"count\": 2 } },"
                         "  \"children\": [ { \"label\": \"body\" } ] }";
    BR_CHECK(br_file_write_all(e, prefab_path, prefab, strlen(prefab)));
    br_prefab_clear_cache(e);
    uint32_t id = br_component_find(e, "every");

    cJSON *overrides = cJSON_Parse("{ \"position\": [5, 6], \"components\": { \"every\": { \"speed\": 9 } },"
                                   "  \"children\": [ { \"label\": \"hat\" } ] }");
    br_node a = br_prefab_instantiate(e, prefab_path, br_scene_root(e), overrides);
    br_node b = br_prefab_instantiate(e, prefab_path, br_scene_root(e), NULL);
    cJSON_Delete(overrides);
    BR_CHECK(br_node_valid(e, a) && br_node_valid(e, b));
    every_field *fa = br_component_get(e, a, id);
    every_field *fb = br_component_get(e, b, id);
    BR_CHECK(fa != NULL && fa->speed == 9.0f && fa->count == 2); /* merged, not replaced */
    BR_CHECK(fb != NULL && fb->speed == 1.0f);
    BR_CHECK(br_node_position(e, a).x == 5.0f && br_node_position(e, b).x == 0.0f);
    BR_CHECK(br_color_to_hex(br_node_tint(e, a)) == 0xff0000);
    BR_CHECK(br_node_child_count(e, a) == 2 && br_node_child_count(e, b) == 1); /* children appended */

    /* the same from a scene file: { "prefab": ... } nodes */
    char scene[2048];
    SDL_snprintf(scene, sizeof(scene),
                 "{ \"children\": [ { \"prefab\": \"%s\", \"label\": \"e1\" },"
                 "  { \"prefab\": \"%s\", \"label\": \"e2\", \"components\": { \"every\": { \"count\": 5 } } },"
                 "  { \"prefab\": \"missing.prefab.json\" } ] }",
                 prefab_path, prefab_path);
    br_node root = load(e, scene);
    BR_CHECK(br_node_child_count(e, root) == 2);
    every_field *f2 = br_component_get(e, find(e, root, "e2"), id);
    BR_CHECK(f2 != NULL && f2->count == 5 && f2->speed == 1.0f);
    BR_CHECK(error_has(e, "cannot read prefab 'missing.prefab.json'"));
    br_node_destroy(e, a);
    br_node_destroy(e, b);
    br_node_destroy(e, root);
}

/* ---- save to disk, load from disk, hot reload */

static void test_save_and_reload(br_engine *e, const char *dir)
{
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%stest_saved.scene.json", dir);
    br_node src = load(e, "{ \"label\": \"level\", \"children\": [ { \"label\": \"one\", \"position\": [3, 4] } ] }");
    br_node_set_position(e, find(e, src, "one"), 30, 40); /* live state is what gets saved */
    BR_CHECK(br_scene_file_save(e, src, path));
    br_node loaded = br_scene_file_load(e, path, br_scene_root(e));
    BR_CHECK(br_node_valid(e, loaded));
    BR_CHECK(br_node_position(e, find(e, loaded, "one")).x == 30.0f);

    SDL_Delay(20); /* a different modification time */
    const char *edited = "{ \"label\": \"level\", \"children\": [ { \"label\": \"two\" }, { \"label\": \"three\" } ] }";
    BR_CHECK(br_file_write_all(e, path, edited, strlen(edited)));
    BR_CHECK(br_scene_file_poll(e) == 1);
    br_engine_step(e, 1.0f / 60.0f); /* the old children are freed at the flush */
    BR_CHECK(br_node_valid(e, loaded)); /* same root, new content */
    BR_CHECK(!br_node_valid(e, find(e, loaded, "one")));
    BR_CHECK(br_node_valid(e, find(e, loaded, "three")));
    BR_CHECK(br_scene_file_poll(e) == 0);

    /* a broken edit keeps the old scene */
    SDL_Delay(20);
    BR_CHECK(br_file_write_all(e, path, "{ \"children\": [", 15));
    BR_CHECK(br_scene_file_poll(e) == 0);
    BR_CHECK(br_node_valid(e, find(e, loaded, "three")));
    br_node_destroy(e, src);
    br_node_destroy(e, loaded);
}

/* ---- hot reload from inside a frame: the old tree's assets outlive its nodes until the flush */

typedef struct reload_in_frame {
    const char *path;
    br_node     root;
    bool        done;
} reload_in_frame;

static void reload_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    reload_in_frame *r = user;
    if (!r->done) {
        r->done = true;
        BR_CHECK(br_scene_file_reload(e, r->path, r->root));
    }
}

static void test_reload_in_frame(br_engine *e, const char *dir)
{
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%stest_reload_font.scene.json", dir);
    const char *font = "fonts/pixel5x7.fnt";
    uint32_t refs = br_assets_ref_count(e, font);
    const char *with_text = "{ \"children\": [ { \"type\": \"bitmap_text\", \"text\": \"HI\", \"font\": \"fonts/pixel5x7.fnt\" } ] }";
    BR_CHECK(br_file_write_all(e, path, with_text, strlen(with_text)));
    br_node root = br_scene_file_load(e, path, br_scene_root(e));
    BR_CHECK(br_assets_ref_count(e, font) == refs + 1);
    const char *empty = "{ \"children\": [] }";
    BR_CHECK(br_file_write_all(e, path, empty, strlen(empty)));
    reload_in_frame r = { path, root, false };
    br_system_add(e, "test.reload", BR_PHASE_UPDATE, 0, reload_system, &r);
    br_engine_step(e, 1.0f / 60.0f); /* reloads; the text node dies at the next flush */
    BR_CHECK(r.done);
    BR_CHECK(br_assets_ref_count(e, font) == refs + 1); /* still held for the dying node */
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_assets_ref_count(e, font) == refs);
    br_system_remove(e, "test.reload");
    br_node_destroy(e, root);
}

/* ---- game.json */

static void test_game_config(void)
{
    br_game_config c;
    const char *json = "{ \"window\": { \"title\": \"Test\", \"width\": 640, \"fullscreen\": true },"
                       "  \"start_scene\": \"a.scene.json\", \"ticker\": { \"max_fps\": 30 },"
                       "  \"clear_color\": \"#102030\", \"debug\": { \"overlay\": true, \"toggle_key\": \"F2\" },"
                       "  \"game\": { \"lives\": 3 } }";
    BR_CHECK(br_game_config_parse(json, strlen(json), "game.json", &c));
    BR_CHECK(strcmp(c.title, "Test") == 0);
    BR_CHECK(c.width == 640 && c.height == 720); /* height keeps its default */
    BR_CHECK(c.fullscreen && c.vsync && c.resizable);
    BR_CHECK(strcmp(c.start_scene, "a.scene.json") == 0 && c.asset_root[0] == '\0');
    BR_CHECK(c.max_fps == 30.0f && c.min_fps == 10.0f && c.speed == 1.0f);
    BR_CHECK(c.has_clear_color && br_color_to_hex(c.clear_color) == 0x102030);
    BR_CHECK(c.debug_overlay && strcmp(c.debug_key, "F2") == 0);
    BR_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(c.json, "game"), "lives") != NULL);
    br_app_desc desc = br_game_config_app_desc(&c);
    BR_CHECK(desc.width == 640 && strcmp(desc.title, "Test") == 0 && desc.asset_root == NULL);
    char *argv[] = { "game", "--debug", "--scene", "b.scene.json" };
    c.debug_overlay = false;
    br_game_config_args(&c, 4, argv);
    BR_CHECK(c.debug_overlay && strcmp(c.start_scene, "b.scene.json") == 0);
    br_game_config_free(&c);

    /* bad values keep defaults; bad JSON fails with defaults */
    const char *bad = "{ \"window\": { \"width\": \"wide\", \"vsync\": 1 }, \"clear_color\": \"nope\" }";
    BR_CHECK(br_game_config_parse(bad, strlen(bad), "game.json", &c));
    BR_CHECK(c.width == 1280 && c.vsync && !c.has_clear_color);
    br_game_config_free(&c);
    BR_CHECK(!br_game_config_parse("{ \"window\": ", 12, "game.json", &c));
    BR_CHECK(c.width == 1280 && strcmp(c.title, "braty") == 0 && c.json == NULL);
    BR_CHECK(!br_game_config_load(NULL, "no_such_game.json", &c));

    /* a string too long for its field keeps the default (it is not blanked) */
    char long_json[512];
    char title[200];
    memset(title, 'x', sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    SDL_snprintf(long_json, sizeof(long_json), "{ \"window\": { \"title\": \"%s\" } }", title);
    BR_CHECK(br_game_config_parse(long_json, strlen(long_json), "game.json", &c));
    BR_CHECK(strcmp(c.title, "braty") == 0);
    br_game_config_free(&c);
}

/* ---- game.json applied to a headless engine */

static void test_game_config_apply(br_engine *e)
{
    br_game_config c;
    const char *json = "{ \"input\": { \"jump\": { \"keys\": [\"Space\"] } }, \"debug\": { \"overlay\": true },"
                       "  \"ticker\": { \"speed\": 0.5 }, \"audio\": { \"buses\": { \"music\": 0.5 } } }";
    BR_CHECK(br_game_config_parse(json, strlen(json), "game.json", &c));
    br_node none = br_game_config_apply(e, &c);
    BR_CHECK(!br_node_valid(e, none)); /* no start scene */
    BR_CHECK(c.json == NULL && br_game_config_json(e) != NULL);
    BR_CHECK(br_debug_is_open(e));
    BR_CHECK(br_audio_bus_find(e, "music") > 0);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_down(e, "jump"));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    br_input_inject_key(e, SDL_SCANCODE_F1, true); /* toggles the overlay */
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_debug_is_open(e));
    br_ticker_set_speed(e, 1.0f);
}

/* ---- unknown keys: warned with file and JSON path, not errors, the rest still loads */

/* Log lines since `first` containing text (run this test while the log is not full). */
static uint32_t log_count_since(br_engine *e, uint32_t first, const char *text)
{
    uint32_t found = 0;
    for (uint32_t i = first; i < br_debug_log_count(e); i++) {
        found += strstr(br_debug_log_line(e, i), text) != NULL ? 1 : 0;
    }
    return found;
}

static bool log_has_since(br_engine *e, uint32_t first, const char *text)
{
    if (log_count_since(e, first, text) == 0) {
        printf("  no log line since %u contains '%s'\n", first, text);
        return false;
    }
    return true;
}

static void test_unknown_keys(br_engine *e)
{
    uint32_t errors = br_scene_file_error_count(e);
    uint32_t first = br_debug_log_count(e);
    const char *json = "{ \"type\": \"container\", \"label\": \"typos\", \"children\": ["
                       " { \"type\": \"sprite\", \"posiiton\": [1, 2], \"anchor\": [0.5, 0.5] },"
                       " { \"type\": \"text\", \"text\": \"x\", \"style\": { \"font_szie\": 12 } },"
                       " { \"label\": \"plain\", \"position\": [3, 4], \"colour\": \"red\" } ] }";
    br_node root = load(e, json);
    BR_CHECK(br_node_valid(e, root));
    BR_CHECK(br_scene_file_error_count(e) == errors); /* warnings only */
    BR_CHECK(log_has_since(e, first, "test.scene.json: children[0].posiiton: unknown key"));
    BR_CHECK(log_has_since(e, first, "test.scene.json: children[1].style.font_szie: unknown key"));
    BR_CHECK(log_has_since(e, first, "test.scene.json: children[2].colour: unknown key"));
    BR_CHECK(br_node_position(e, find(e, root, "plain")).x == 3.0f); /* known keys still apply */
    br_node_destroy(e, root);

    /* the every-type scene uses only known keys: no warning */
    first = br_debug_log_count(e);
    root = load(e, every_type_scene);
    BR_CHECK(log_count_since(e, first, "unknown key") == 0);
    br_node_destroy(e, root);

    first = br_debug_log_count(e);
    br_game_config c;
    const char *cfg = "{ \"window\": { \"widht\": 640 }, \"lives\": 3, \"game\": { \"anything\": 1 },"
                      "  \"input\": { \"jump\": { \"key\": [\"Space\"] } } }";
    BR_CHECK(br_game_config_parse(cfg, strlen(cfg), "game.json", &c));
    BR_CHECK(log_has_since(e, first, "game.json: window.widht: unknown key"));
    BR_CHECK(log_has_since(e, first, "game.json: lives: unknown key"));
    BR_CHECK(log_has_since(e, first, "game.json: input.jump.key: unknown key"));
    BR_CHECK(log_count_since(e, first, "unknown key") == 3); /* "game" contents are the game's */
    br_game_config_free(&c);
}

/* ---- debug: log capture, selection */

static void test_debug(br_engine *e)
{
    uint32_t before = br_debug_log_count(e);
    br_log_info("debug console line %d", 42);
    BR_CHECK(br_debug_log_count(e) == (before < BR_DEBUG_LOG_LINES ? before + 1 : BR_DEBUG_LOG_LINES));
    BR_CHECK(strstr(br_debug_log_line(e, br_debug_log_count(e) - 1), "debug console line 42") != NULL);
    br_node n = br_node_create_container(e);
    br_debug_select(e, n);
    BR_CHECK(br_node_equal(br_debug_selected(e), n));
    br_node_destroy(e, n);
    BR_CHECK(br_debug_selected(e).generation == 0);
}

int main(void)
{
    br_engine *e = br_engine_create("test_scene_file", "../../examples");
    if (e == NULL) {
        printf("no engine\n");
        return 1;
    }
    char dir[1024];
    const char *base = SDL_GetBasePath();
    SDL_strlcpy(dir, base != NULL ? base : "", sizeof(dir));
    for (char *c = dir; *c != 0; c++) {
        if (*c == 92) {
            *c = '/'; /* backslash: the path goes into JSON strings */
        }
    }
    register_every_field(e);

    test_unknown_keys(e); /* first: it reads the log, which keeps BR_DEBUG_LOG_LINES lines */
    test_every_type_round_trip(e);
    test_collect_assets(e);
    test_component_fields(e);
    test_errors(e);
    test_prefabs(e, dir);
    test_save_and_reload(e, dir);
    test_reload_in_frame(e, dir);
    test_game_config();
    test_game_config_apply(e);
    test_debug(e);

    br_engine_destroy(e);
    return br_test_finish();
}
