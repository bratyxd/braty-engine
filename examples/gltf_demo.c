/* gltf_demo - glTF models and 3D animation.
 *
 *   gltf_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *             [--foxes N]    extra skinned, animated foxes in a grid (benchmark; default 0)
 *             [--novsync]    measure frame time without vsync
 *             [--focus I]    close-up of row model I (0-5, left to right) or the fox (6)
 *
 * Main view (built in code): a row of Khronos sample models on a ground plane, lit by a sun with
 * cascaded shadows: NormalTangentMirrorTest (textured PBR, external .bin/.png), TextureCoordinate
 * Test (text on the texture: must read correctly), OrientationTest (asymmetric: arrows hit their
 * targets), TextureTransformTest (KHR_texture_transform), BoxAnimated (node animation),
 * AnimatedMorphCube (morph targets). In front: the Fox (skinned, loaded with br_assets_load_async)
 * cross-fading Survey -> Walk -> Run -> Survey (a 2 s fade starting every 3 s from t = 1 s).
 * Inset (bottom right): a second view built entirely from examples/models/gltf_demo.scene.json.
 * Left-drag orbits the main camera, the wheel zooms.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

static int g_foxes = 0;
static bool g_no_vsync = false;
static int g_focus = -1;

/* x of the row models (left to right), then the fox */
static const float g_row_x[7] = { -5.4f, -3.3f, -1.1f, 1.1f, 3.2f, 5.2f, -1.0f };

typedef struct demo_state {
    br_node         view;
    br_node3d       root;
    br_node3d       camera;
    br_orbit_camera orbit;
    br_node3d       fox;
    uint32_t        fox_clip;
    float           next_fade;
    br_node         json_view;
    br_node         info;
    float           time;
    uint64_t        last_ns;
    uint64_t        frame_ns_sum;
    uint64_t        animate_ns_sum;
    uint64_t        frame_count;
} demo_state;

/* Instantiates a model under the main root, scaled so its largest extent is `size`, standing on
 * the ground at (x, z). */
static br_node3d place_model(br_engine *e, demo_state *s, br_model *m, float x, float z, float size)
{
    br_node3d n = br_model_instantiate(e, m);
    br_vec3 ext = br_vec3_sub(m->bounds.max, m->bounds.min);
    float largest = SDL_max(ext.x, SDL_max(ext.y, ext.z));
    float k = largest > 0.0f ? size / largest : 1.0f;
    br_vec3 c = br_vec3_scale(br_vec3_add(m->bounds.min, m->bounds.max), 0.5f);
    br_node3d_set_scale(e, n, br_vec3_make(k, k, k));
    br_node3d_set_position(e, n, br_vec3_make(x - c.x * k, -m->bounds.min.y * k, z - c.z * k));
    br_node3d_add_child(e, s->root, n);
    return n;
}

/* Loads a model through the asset cache and places it (logs and skips on failure). */
static br_node3d load_and_place(br_engine *e, demo_state *s, const char *path, float x, float z, float size)
{
    br_node3d none = { 0, 0 };
    br_model *m = br_model_load(e, path);
    return m != NULL ? place_model(e, s, m, x, z, size) : none;
}

static void on_fox_loaded(br_engine *e, const char *path, void *asset, void *user)
{
    demo_state *s = user;
    (void)path;
    br_model *fox = asset;
    if (fox == NULL) {
        return;
    }
    /* --- snippet: docs/agents/gltf.md --- */
    s->fox = br_model_instantiate(e, fox);                    /* a "model3d" node */
    br_node3d_add_child(e, s->root, s->fox);
    br_node3d_set_scale(e, s->fox, br_vec3_make(0.025f, 0.025f, 0.025f));
    br_node3d_set_position(e, s->fox, br_vec3_make(g_row_x[6], 0.0f, 2.6f));
    br_node3d_set_euler(e, s->fox, 0.0f, 0.9f, 0.0f);
    br_animator3d_play(e, s->fox, "Survey");
    /* later, every 3 s: br_animator3d_cross_fade(e, s->fox, "Walk", 2.0f); */
    /* --- end snippet --- */
    br_node3d_set_visible(e, s->fox, g_focus < 0 || g_focus == 6); /* close-ups of the row: out of the way */
    s->fox_clip = 0;
    s->next_fade = 1.0f;

    /* benchmark: a grid of extra foxes behind the row, each with its own animator */
    for (int i = 0; i < g_foxes; i++) {
        br_node3d f = br_model_instantiate(e, fox);
        br_node3d_add_child(e, s->root, f);
        float gx = (float)(i % 20) - 9.5f;
        float gz = -4.0f - (float)(i / 20) * 1.6f;
        br_node3d_set_scale(e, f, br_vec3_make(0.01f, 0.01f, 0.01f));
        br_node3d_set_position(e, f, br_vec3_make(gx * 1.1f, 0.0f, gz));
        br_animator3d_play_index(e, f, (uint32_t)(i % 3));
        br_animator3d_get(e, f)->time = (float)i * 0.137f; /* desynchronise */
    }
}

static br_material3d make_material(br_engine *e, uint32_t srgb, float roughness)
{
    br_material3d_desc d = br_material3d_default(BR_MATERIAL3D_PBR);
    d.base_color = br_color_srgb_to_linear(br_color_from_hex(srgb, 1.0f));
    d.roughness = roughness;
    return br_material3d_create(e, &d);
}

static void setup_main_view(br_engine *e, demo_state *s)
{
    s->view = br_view3d_create(e, 1280.0f, 720.0f);
    br_view3d_set_auto_resize(e, s->view, true);
    br_view3d_set_msaa(e, s->view, 4);
    br_view3d_set_background(e, s->view, br_color_srgb_to_linear(br_color_from_hex(0x30384A, 1.0f)));
    br_view3d_set_environment(e, s->view, br_color_srgb_to_linear(br_color_from_hex(0xA8C0E8, 1.0f)),
                              br_color_srgb_to_linear(br_color_from_hex(0x5A4A3A, 1.0f)), 0.45f);
    br_node_add_child(e, br_scene_root(e), s->view);
    s->root = br_view3d_root(e, s->view);
    s->camera = br_view3d_camera(e, s->view);
    br_camera3d_get(e, s->camera)->fov_y = 42.0f * BR_DEG_TO_RAD;
    s->orbit = br_orbit_camera_make(br_vec3_make(0.0f, 0.9f, 0.5f), 10.5f, 0.0f, 0.28f);
    if (g_focus >= 0 && g_focus < 7) {
        s->orbit = br_orbit_camera_make(br_vec3_make(g_row_x[g_focus], 0.8f, g_focus == 6 ? 2.6f : 0.0f), 3.2f, g_focus == 5 ? 0.7f : 0.0f, g_focus == 5 ? 0.45f : 0.2f);
    }
    br_orbit_camera_apply(e, &s->orbit, s->camera);

    br_node3d ground = br_mesh3d_create(e, br_geometry3d_plane(e, 60.0f, 60.0f, 1, 1), make_material(e, 0x6B7078, 0.9f));
    br_mesh3d_get(e, ground)->cast_shadows = false;
    br_node3d_add_child(e, s->root, ground);

    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 2.2f;
    sun.color = br_color_srgb_to_linear(br_color_from_hex(0xFFF0DC, 1.0f));
    sun.cast_shadows = true;
    sun.cascade_count = 3;
    sun.shadow_distance = 40.0f;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_add_child(e, s->root, sun_node);
    br_node3d_set_position(e, sun_node, br_vec3_make(-5.0f, 9.0f, 7.0f));
    br_node3d_look_at(e, sun_node, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    /* the row: every model loaded synchronously through the asset cache */
    load_and_place(e, s, "NormalTangentMirrorTest/NormalTangentMirrorTest.gltf", g_row_x[0], 0.0f, 1.9f);
    load_and_place(e, s, "TextureCoordinateTest/TextureCoordinateTest.glb", g_row_x[1], 0.0f, 1.6f);
    load_and_place(e, s, "OrientationTest/OrientationTest.glb", g_row_x[2], 0.0f, 1.8f);
    load_and_place(e, s, "TextureTransformTest/TextureTransformTest.gltf", g_row_x[3], 0.0f, 1.8f);
    br_node3d box = load_and_place(e, s, "BoxAnimated/BoxAnimated.glb", g_row_x[4], 0.0f, 1.3f);
    br_animator3d_play_index(e, box, 0);
    br_node3d cube = load_and_place(e, s, "AnimatedMorphCube/AnimatedMorphCube.glb", g_row_x[5], 0.0f, 1.2f);
    br_animator3d_play(e, cube, "Square");

    /* the skinned fox: background load, instantiated in the completion callback */
    br_assets_load_async(e, "Fox/Fox.glb", NULL, on_fox_loaded, s);
}

static void setup_2d(br_engine *e, demo_state *s)
{
    /* inset: a view built entirely from a scene file */
    s->json_view = br_scene_file_load(e, "gltf_demo.scene.json", br_scene_root(e));
    br_text_style ts = br_text_style_default();
    ts.font_size = 18.0f;
    ts.fill = br_color_from_hex(0xF0F0F0, 1.0f);
    s->info = br_text_create(e, "", &ts);
    br_node_set_position(e, s->info, 16.0f, 12.0f);
    br_node_add_child(e, br_scene_root(e), s->info);
    br_node label = br_text_create(e, "from gltf_demo.scene.json", &ts);
    br_node_set_position(e, label, 850.0f, 430.0f);
    br_node_add_child(e, br_scene_root(e), label);
}

static void demo_update(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    static const char *const clips[3] = { "Survey", "Walk", "Run" };
    s->time += dt;
    br_orbit_camera_update(e, &s->orbit, s->camera);
    if (br_node3d_valid(e, s->fox) && s->time >= s->next_fade) {
        s->fox_clip = (s->fox_clip + 1) % 3;
        br_animator3d_cross_fade(e, s->fox, clips[s->fox_clip], 2.0f);
        s->next_fade += 3.0f;
    }

    uint64_t animate_ns = 0;
    for (uint32_t i = 0; i < br_system_count(e); i++) {
        br_system_info info;
        if (br_system_get_info(e, i, &info) && SDL_strcmp(info.name, "gltf.animate") == 0) {
            animate_ns = info.last_time_ns;
        }
    }
    if (br_app_deterministic(e)) {
        animate_ns = 0; /* the on-screen timing would make every golden image differ */
    }
    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0 && br_ticker_frame(e) > 30) {
        s->frame_ns_sum += now - s->last_ns;
        s->animate_ns_sum += animate_ns;
        s->frame_count++;
    }
    s->last_ns = now;

    char line[256];
    br_animator3d *a = br_animator3d_get(e, s->fox);
    if (a != NULL && a->fade_clip >= 0) {
        SDL_snprintf(line, sizeof(line), "Fox: %s -> %s (fade %.0f%%)   animators %u   gltf.animate %.3f ms",
                     clips[a->fade_clip], clips[a->clip], 100.0f * a->fade_elapsed / a->fade_duration,
                     br_animator3d_count(e), (double)animate_ns / 1e6);
    } else {
        SDL_snprintf(line, sizeof(line), "Fox: %s   animators %u   gltf.animate %.3f ms",
                     a != NULL && a->clip >= 0 ? clips[a->clip] : "loading...", br_animator3d_count(e),
                     (double)animate_ns / 1e6);
    }
    br_text_set_text(e, s->info, line);
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    (void)e;
    double n = s->frame_count > 0 ? (double)s->frame_count : 1.0;
    br_log_info("gltf_demo: %d extra foxes, %u animators: avg frame %.3f ms, gltf.animate %.3f ms over %llu frames (%s)",
                g_foxes, br_animator3d_count(e), (double)s->frame_ns_sum / n / 1e6, (double)s->animate_ns_sum / n / 1e6,
                (unsigned long long)s->frame_count, g_no_vsync ? "vsync off" : "vsync on");
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    if (g_no_vsync) {
        br_gpu_set_vsync(br_engine_gpu(e), false);
    }
    setup_main_view(e, s);
    setup_2d(e, s);
    br_system_add(e, "demo.update", BR_PHASE_UPDATE, 0, demo_update, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--foxes") == 0 && i + 1 < argc) {
            g_foxes = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--focus") == 0 && i + 1 < argc) {
            g_focus = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--novsync") == 0) {
            g_no_vsync = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - glTF demo";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR "/models"; /* the source tree (CMake) */
    desc.init = demo_init;
    return desc;
}
