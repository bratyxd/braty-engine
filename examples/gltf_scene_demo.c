/* gltf_scene_demo - a .glb level used as the scene, with no scene JSON.
 *
 *   br_scene_load(e, "gltf_scene/level.glb", BR_SCENE_SINGLE)   the file's nodes, camera, lights,
 *                                                                 autoplayed animation, batching
 *   a "spin" component that came from glTF node extras            iterated by a system
 *   kit parts ("gltf_scene/kit.glb#Crate") and an animated Fox    placed from gltf_scene/props.json
 *
 * The level and kit are written by tests/data/make_level_gltf.py. Flags: --no-batch (keep one
 * draw per repeated tree, to compare draw calls), --top (the file's second camera, "TopCam").
 */
#include "braty/braty_main.h"

typedef struct spin {
    float speed;   /* radians per second around the parent's Y axis */
} spin;

typedef struct pickup {
    int32_t value;
    char    kind[BR_FIELD_STRING_SIZE];
} pickup;

typedef struct demo {
    uint32_t spin_id;
    br_scene level;
    br_node  view;
    br_node  info;
} demo;

static bool g_no_batch;
static bool g_top;

/* Places every entry of props.json under the level's 3D root. */
static void place_props(br_engine *e, demo *d)
{
    void *bytes = NULL;
    size_t size = 0;
    if (!br_file_read_all(e, "gltf_scene/props.json", &bytes, &size)) {
        br_log_error("gltf_scene_demo: props.json missing");
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(bytes, size);
    br_free(bytes);
    br_node3d root = br_scene_root_3d(e, d->level);
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "props"))
    {
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, "source");
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(item, "position");
        const cJSON *yaw = cJSON_GetObjectItemCaseSensitive(item, "yaw");
        const cJSON *scale = cJSON_GetObjectItemCaseSensitive(item, "scale");
        const cJSON *anim = cJSON_GetObjectItemCaseSensitive(item, "animation");
        if (!cJSON_IsString(source)) {
            continue;
        }
        br_node3d n = br_gltf_instantiate(e, source->valuestring, root, NULL); /* kit parsed once, shared */
        if (cJSON_GetArraySize(pos) == 3) {
            br_node3d_set_position(e, n, br_vec3_make((float)cJSON_GetArrayItem(pos, 0)->valuedouble,
                                                      (float)cJSON_GetArrayItem(pos, 1)->valuedouble,
                                                      (float)cJSON_GetArrayItem(pos, 2)->valuedouble));
        }
        if (cJSON_IsNumber(yaw)) {
            br_node3d_set_euler(e, n, br_vec3_make(0.0f, (float)yaw->valuedouble * BR_DEG_TO_RAD, 0.0f));
        }
        if (cJSON_IsNumber(scale)) {
            float k = (float)scale->valuedouble;
            br_node3d_set_scale(e, n, br_vec3_make(k, k, k));
        }
        if (cJSON_IsString(anim)) {
            br_animator a = br_animator_add(e, br_anim_target_3d(n));
            br_animator_add_model_clips(e, a, br_model3d_model(e, n));
            br_animator_play(e, a, anim->valuestring);
        }
    }
    cJSON_Delete(doc);
}

/* Every 3D node that got a "spin" component from its glTF extras turns around its parent's Y. */
static void spin_system(br_engine *e, float dt, void *user)
{
    demo *d = user;
    uint32_t count = br_component3d_count(e, d->spin_id);
    for (uint32_t i = 0; i < count; i++) {
        const spin *s = br_component3d_at(e, d->spin_id, i);
        br_node3d n = br_component3d_owner_at(e, d->spin_id, i);
        if (!br_node3d_valid(e, n)) {
            continue;
        }
        br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), s->speed * dt);
        br_node3d_set_rotation(e, n, br_quat_mul(turn, br_node3d_rotation(e, n)));
    }
}

static void hud_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo *d = user;
    br_view3d_stats stats;
    SDL_zero(stats);
    br_view3d_get_stats(e, d->view, &stats);
    char line[256];
    SDL_snprintf(line, sizeof(line),
                 "level.glb is the scene (no scene JSON)   spinning coins from extras: %u\n"
                 "draw calls %u (%u opaque items, %u shadow)   batching %s",
                 br_component3d_count(e, d->spin_id), stats.draw_calls, stats.items_opaque, stats.items_shadow,
                 g_no_batch ? "off (--no-batch)" : "on");
    br_text_set_text(e, d->info, line);
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo *d = user;
    br_view3d_stats stats;
    SDL_zero(stats);
    br_view3d_get_stats(e, d->view, &stats);
    br_log_info("gltf_scene_demo: batching %s: %u draw calls, %u opaque items, %u shadow items",
                g_no_batch ? "off" : "on", stats.draw_calls, stats.items_opaque, stats.items_shadow);
    br_free(d);
}

static bool demo_init(br_engine *e)
{
    demo *d = br_alloc(sizeof(demo));
    if (d == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, d);
    /* components first: the level's extras fill them while it loads */
    static const br_field_desc spin_fields[] = { { "speed", BR_FIELD_FLOAT, offsetof(spin, speed) } };
    static const br_field_desc pickup_fields[] = {
        { "value", BR_FIELD_INT, offsetof(pickup, value) },
        { "kind", BR_FIELD_STRING, offsetof(pickup, kind) },
    };
    br_component_desc spin_desc = { "spin", sizeof(spin), spin_fields, BR_ARRAY_COUNT(spin_fields) };
    br_component_desc pickup_desc = { "pickup", sizeof(pickup), pickup_fields, BR_ARRAY_COUNT(pickup_fields) };
    d->spin_id = br_component_register(e, &spin_desc);
    br_component_register(e, &pickup_desc);

    br_gltf_options load;
    SDL_zero(load);
    load.no_batching = g_no_batch;
    load.camera = g_top ? "TopCam" : NULL;
    br_gltf_set_load_options(e, &load);
    d->level = br_scene_load(e, "gltf_scene/level.glb", BR_SCENE_SINGLE);
    d->view = br_node_find_by_label(e, br_scene_root_2d(e, d->level), "level", true);
    place_props(e, d);

    br_text_style ts = br_text_style_default();
    ts.font_size = 18.0f;
    ts.fill = br_color_from_hex(0x102030, 1.0f);
    d->info = br_text_create(e, "", &ts);
    br_node_set_position(e, d->info, br_vec2_make(16.0f, 12.0f));
    br_node_add_child(e, br_scene_root_2d(e, d->level), d->info);

    br_system_add(e, "demo.spin", BR_PHASE_UPDATE, 0, spin_system, d);
    br_system_add(e, "demo.hud", BR_PHASE_LATE_UPDATE, 500, hud_system, d);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--no-batch") == 0) {
            g_no_batch = true;
        } else if (SDL_strcmp(argv[i], "--top") == 0) {
            g_top = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - glTF level as a scene";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR; /* the source tree (CMake) */
    desc.init = demo_init;
    return desc;
}
