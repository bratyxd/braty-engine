/* animation_demo - the unified animation system for 2D and 3D.
 *
 *   animation_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12] [--vsync off]
 *                  [--bench-foxes N]   N extra skinned foxes, each with its own state machine
 *                  [--bench-tweens N]  N extra sprites, each moved by its own endless yoyo tween
 *
 * Left: a 2D character rig (nodes body, head, arms, legs) walking from examples/animation/
 * hero.anim.json, with a second layer masked to the right arm that blends a wave in and out
 * (two-layer blending); "step" markers in the walk clip count steps. Under it a sprite flipbook
 * driven by a clip made with br_anim_clip_from_frames (frames generated in code). Top right: a UI
 * panel brought in by a tween sequence (slide + fade in parallel, then a title pop, then a
 * callback). Right: a 3D view with the Fox driven by examples/animation/fox.animgraph.json
 * (idle -> walk -> run by a "speed" parameter the demo ramps up and down), a cube whose material
 * colour, emission and scale pulse (props.anim.json), and AnimatedMorphCube's morph weights
 * animated by a clip of our own. With --bench-*, the frame time and the animation systems' share
 * are logged at exit.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

enum { FLIP_FRAMES = 8, FLIP_SIZE = 48 };

static int g_bench_foxes = 0;
static int g_bench_tweens = 0;

typedef struct demo_state {
    br_node        hero;
    br_animator    hero_anim;
    int            steps;
    float          wave_weight;
    br_node        flip;
    br_anim_clip  *flip_clip;
    br_texture     flip_frames[FLIP_FRAMES];
    br_node        panel;
    br_node        panel_text;
    bool           panel_ready;
    br_node        view;
    br_node3d      fox;
    br_animator    fox_anim;
    br_animator   *bench_anims;
    br_node        info;
    float          time;
    uint64_t       last_ns;
    uint64_t       frame_ns_sum;
    uint64_t       anim_ns_sum[3];
    uint64_t       frame_count;
} demo_state;

/* ---- the 2D rig */

/* A rig joint: a container (what the clips animate) holding a coloured box (sprite sizes are
 * scale, so the box is a child and the joint's children are not stretched by it). */
static br_node rig_part(br_engine *e, br_node parent, const char *label, float w, float h, float ax, float ay,
                        float x, float y, uint32_t rgb)
{
    br_node joint = br_node_create_container(e);
    br_node_set_label(e, joint, label);
    br_node_set_position(e, joint, x, y);
    br_node_add_child(e, parent, joint);
    br_node box = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, box, w, h);
    br_sprite_set_anchor(e, box, ax, ay);
    br_node_set_tint(e, box, br_color_from_hex(rgb, 1.0f));
    br_node_add_child(e, joint, box);
    return joint;
}

static void on_hero_event(br_engine *e, const br_anim_event *ev, void *user)
{
    demo_state *s = user;
    (void)e;
    if (SDL_strcmp(ev->name, "step") == 0) {
        s->steps++;
    }
}

static void setup_hero(br_engine *e, demo_state *s)
{
    s->hero = br_node_create_container(e);
    br_node_set_label(e, s->hero, "hero");
    br_node_set_position(e, s->hero, 250.0f, 330.0f);
    br_node_set_scale(e, s->hero, 1.6f, 1.6f);
    br_node_add_child(e, br_scene_root(e), s->hero);
    rig_part(e, s->hero, "leg_l", 16.0f, 70.0f, 0.5f, 0.0f, -12.0f, 0.0f, 0x34495E);
    rig_part(e, s->hero, "leg_r", 16.0f, 70.0f, 0.5f, 0.0f, 12.0f, 0.0f, 0x2C3E50);
    br_node body = rig_part(e, s->hero, "body", 52.0f, 86.0f, 0.5f, 1.0f, 0.0f, 0.0f, 0x2E86C1);
    rig_part(e, body, "arm_l", 14.0f, 64.0f, 0.5f, 0.0f, -32.0f, -80.0f, 0x2471A3);
    rig_part(e, body, "head", 44.0f, 44.0f, 0.5f, 1.0f, 0.0f, -88.0f, 0xFFFFFF); /* tinted by the clip */
    rig_part(e, body, "arm_r", 14.0f, 64.0f, 0.5f, 0.0f, 32.0f, -80.0f, 0x2471A3);

    /* --- snippet: docs/agents/animation.md (2D rig + layer) --- */
    const br_anim_library *lib = br_anim_library_load(e, "animation/hero.anim.json");
    br_animator a = br_animator_add(e, br_anim_target_2d(s->hero));
    br_animator_add_library(e, a, lib);
    br_animator_play(e, a, "walk");                           /* base layer: the whole body */
    br_animator_set_layer(e, a, 1, 0.0f, false);              /* layer 1 overrides ... */
    br_animator_layer_add_mask(e, a, 1, "body/arm_r");        /* ... only the right arm */
    br_animator_play_layer(e, a, 1, "wave", 0.0f);
    br_animator_set_event_callback(e, a, on_hero_event, s);   /* "step" markers */
    /* --- end snippet --- */
    s->hero_anim = a;
}

/* ---- the flipbook: a ring with one bright segment, generated frame by frame */

static void setup_flipbook(br_engine *e, demo_state *s)
{
    uint8_t px[FLIP_SIZE * FLIP_SIZE * 4];
    br_anim_frame frames[FLIP_FRAMES];
    for (int f = 0; f < FLIP_FRAMES; f++) {
        for (int y = 0; y < FLIP_SIZE; y++) {
            for (int x = 0; x < FLIP_SIZE; x++) {
                float dx = (float)x + 0.5f - FLIP_SIZE * 0.5f;
                float dy = (float)y + 0.5f - FLIP_SIZE * 0.5f;
                float r = SDL_sqrtf(dx * dx + dy * dy);
                float angle = SDL_atan2f(dy, dx) + BR_PI; /* 0 .. 2 pi */
                int segment = (int)(angle / (2.0f * BR_PI) * FLIP_FRAMES) % FLIP_FRAMES;
                bool ring = r > 12.0f && r < 22.0f;
                uint8_t *p = &px[(y * FLIP_SIZE + x) * 4];
                p[0] = segment == f ? 255 : 70;
                p[1] = segment == f ? 200 : 90;
                p[2] = segment == f ? 60 : 120;
                p[3] = ring ? 255 : 0;
            }
        }
        s->flip_frames[f] = br_texture_create_rgba8(e, FLIP_SIZE, FLIP_SIZE, px, NULL);
        frames[f].texture = s->flip_frames[f];
        frames[f].duration = f == 0 ? 0.3f : 0.08f; /* the first frame holds longer */
    }
    s->flip = br_sprite_create(e, s->flip_frames[0]);
    br_sprite_set_anchor(e, s->flip, 0.5f, 0.5f);
    br_node_set_position(e, s->flip, 250.0f, 560.0f);
    br_node_set_scale(e, s->flip, 2.0f, 2.0f);
    br_node_add_child(e, br_scene_root(e), s->flip);
    /* the sprites module's frame list becomes a clip: one STEP track on "sprite.texture" */
    s->flip_clip = br_anim_clip_from_frames("spin", frames, FLIP_FRAMES);
    br_animator fa = br_animator_add(e, br_anim_target_2d(s->flip));
    br_animator_add_clip(e, fa, s->flip_clip);
    br_animator_play(e, fa, "spin");
}

/* ---- the UI panel: a tween sequence */

static void on_panel_ready(br_engine *e, br_tween t, void *user)
{
    demo_state *s = user;
    (void)t;
    s->panel_ready = true;
    br_text_set_text(e, s->panel_text, "Panel ready");
}

static void setup_panel(br_engine *e, demo_state *s)
{
    s->panel = br_node_create_container(e);
    br_node_set_position(e, s->panel, 1400.0f, 30.0f);
    br_node_set_alpha(e, s->panel, 0.0f);
    br_node_add_child(e, br_scene_root(e), s->panel);
    br_node bg = rig_part(e, s->panel, "bg", 300.0f, 90.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0x1B2631);
    rig_part(e, bg, "bar", 300.0f, 6.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0xF5B041);
    br_text_style ts = br_text_style_default();
    ts.font_size = 26.0f;
    ts.fill = br_color_from_hex(0xF0F3F4, 1.0f);
    s->panel_text = br_text_create(e, "Tween sequence", &ts);
    br_text_set_anchor(e, s->panel_text, 0.5f, 0.5f);
    br_node_set_position(e, s->panel_text, 150.0f, 50.0f);
    br_node_set_scale(e, s->panel_text, 0.0f, 0.0f);
    br_node_add_child(e, s->panel, s->panel_text);

    /* --- snippet: docs/agents/animation.md (tween sequence) --- */
    br_anim_target panel = br_anim_target_2d(s->panel);
    float slide_to[2] = { 950.0f, 30.0f };
    float pop_to[2] = { 1.0f, 1.0f };
    br_tween_sequence seq = br_tween_sequence_begin(1);
    br_tween_sequence_wait(&seq, 0.2f);
    br_tween_sequence_add(e, &seq, br_tween_to(e, panel, "position", slide_to, 0.6f, BR_EASE_OUT_BACK));
    br_tween_sequence_join(e, &seq, br_tween_to_f(e, panel, "alpha", 1.0f, 0.4f, BR_EASE_OUT_QUAD));
    br_tween_sequence_add(e, &seq, br_tween_to(e, br_anim_target_2d(s->panel_text), "scale", pop_to, 0.5f,
                                               BR_EASE_OUT_ELASTIC));
    br_tween_sequence_call(e, &seq, on_panel_ready, s);
    /* --- end snippet --- */
}

/* ---- the 3D view */

static br_material3d make_material(br_engine *e, uint32_t srgb, float roughness)
{
    br_material3d_desc d = br_material3d_default(BR_MATERIAL3D_PBR);
    d.base_color = br_color_srgb_to_linear(br_color_from_hex(srgb, 1.0f));
    d.roughness = roughness;
    return br_material3d_create(e, &d);
}

static br_animator add_fox(br_engine *e, br_node3d root, br_model *fox, const br_anim_graph *graph, br_vec3 pos,
                           float scale, float yaw)
{
    br_node3d f = br_model_instantiate(e, fox);
    br_node3d_add_child(e, root, f);
    br_node3d_set_position(e, f, pos);
    br_node3d_set_scale(e, f, br_vec3_make(scale, scale, scale));
    br_node3d_set_euler(e, f, 0.0f, yaw, 0.0f);
    /* --- snippet: docs/agents/animation.md (glTF + state machine) --- */
    br_animator a = br_animator_add(e, br_anim_target_3d(f));
    br_animator_add_model_clips(e, a, fox);   /* Survey, Walk, Run (keys shared with the model) */
    br_animator_set_graph(e, a, graph);       /* idle -> walk -> run by "speed" */
    /* every frame: br_animator_set_float(e, a, "speed", speed); */
    /* --- end snippet --- */
    return a;
}

static void setup_3d(br_engine *e, demo_state *s)
{
    s->view = br_view3d_create(e, 700.0f, 470.0f);
    br_node_set_position(e, s->view, 540.0f, 200.0f);
    br_view3d_set_msaa(e, s->view, 4);
    br_view3d_set_background(e, s->view, br_color_srgb_to_linear(br_color_from_hex(0x27303F, 1.0f)));
    br_view3d_set_environment(e, s->view, br_color_srgb_to_linear(br_color_from_hex(0xA8C0E8, 1.0f)),
                              br_color_srgb_to_linear(br_color_from_hex(0x5A4A3A, 1.0f)), 0.5f);
    br_node_add_child(e, br_scene_root(e), s->view);
    br_node3d root = br_view3d_root(e, s->view);
    br_node3d cam = br_view3d_camera(e, s->view);
    br_camera3d_get(e, cam)->fov_y = 40.0f * BR_DEG_TO_RAD;
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 2.6f, 6.8f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.7f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    br_node3d ground = br_mesh3d_create(e, br_geometry3d_plane(e, 40.0f, 40.0f, 1, 1), make_material(e, 0x6B7078, 0.9f));
    br_mesh3d_get(e, ground)->cast_shadows = false;
    br_node3d_add_child(e, root, ground);
    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 2.4f;
    sun.cast_shadows = true;
    sun.shadow_distance = 30.0f;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_add_child(e, root, sun_node);
    br_node3d_set_position(e, sun_node, br_vec3_make(-4.0f, 8.0f, 6.0f));
    br_node3d_look_at(e, sun_node, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    const br_anim_library *props = br_anim_library_load(e, "animation/props.anim.json");
    /* a cube whose material colour, emission and scale pulse */
    br_node3d cube = br_mesh3d_create(e, br_geometry3d_cube(e, br_vec3_make(0.8f, 0.8f, 0.8f)), make_material(e, 0x3A6CFF, 0.4f));
    br_node3d_add_child(e, root, cube);
    br_node3d_set_position(e, cube, br_vec3_make(-2.3f, 0.6f, 0.0f));
    br_node3d_set_euler(e, cube, 0.3f, 0.6f, 0.0f);
    br_animator ca = br_animator_add(e, br_anim_target_3d(cube));
    br_animator_add_library(e, ca, props);
    br_animator_play(e, ca, "pulse");
    /* AnimatedMorphCube: our own clip on its morph weights ("#0:morph_weights") */
    br_model *morph = br_model_load(e, "models/AnimatedMorphCube/AnimatedMorphCube.glb");
    if (morph != NULL) {
        br_node3d m = br_model_instantiate(e, morph);
        br_node3d_add_child(e, root, m);
        br_node3d_set_position(e, m, br_vec3_make(2.3f, 0.6f, 0.0f));
        br_node3d_set_scale(e, m, br_vec3_make(0.5f, 0.5f, 0.5f));
        br_animator ma = br_animator_add(e, br_anim_target_3d(m));
        br_animator_add_library(e, ma, props);
        br_animator_play(e, ma, "breathe");
    }
    /* the Fox with a state machine */
    br_model *fox = br_model_load(e, "models/Fox/Fox.glb");
    const br_anim_graph *graph = br_anim_graph_load(e, "animation/fox.animgraph.json");
    if (fox == NULL || graph == NULL) {
        return;
    }
    s->fox_anim = add_fox(e, root, fox, graph, br_vec3_make(0.0f, 0.0f, 0.6f), 0.022f, 0.9f);
    if (g_bench_foxes > 0) {
        s->bench_anims = br_alloc(sizeof(br_animator) * (size_t)g_bench_foxes);
    }
    for (int i = 0; s->bench_anims != NULL && i < g_bench_foxes; i++) {
        float gx = (float)(i % 40) - 19.5f;
        float gz = -3.0f - (float)(i / 40) * 0.9f;
        s->bench_anims[i] = add_fox(e, root, fox, graph, br_vec3_make(gx * 0.6f, 0.0f, gz), 0.008f, 0.0f);
    }
}

/* ---- benchmark sprites: one endless yoyo tween each */

static void setup_bench_tweens(br_engine *e)
{
    br_node layer = br_node_create_container(e);
    br_node_add_child(e, br_scene_root(e), layer);
    for (int i = 0; i < g_bench_tweens; i++) {
        br_node n = br_sprite_create(e, br_texture_white(e));
        br_sprite_set_size(e, n, 3.0f, 3.0f);
        float x = br_engine_random_float(e) * 1280.0f;
        float y = br_engine_random_float(e) * 720.0f;
        br_node_set_position(e, n, x, y);
        br_node_set_tint(e, n, br_color_from_hsv(br_engine_random_float(e), 0.6f, 1.0f, 1.0f));
        br_node_add_child(e, layer, n);
        float to[2] = { x + (br_engine_random_float(e) - 0.5f) * 200.0f, y + (br_engine_random_float(e) - 0.5f) * 200.0f };
        br_tween t = br_tween_to(e, br_anim_target_2d(n), "position", to, 1.0f + br_engine_random_float(e) * 2.0f,
                                 BR_EASE_IN_OUT_SINE);
        br_tween_set_repeat(e, t, -1, true);
    }
}

/* ---- per frame */

static const char *const g_timed[3] = { "animation.update", "animation.tweens", "gltf.animate" };

static void demo_update(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    s->time += dt;
    /* the wave layer fades in and out over 6 s */
    float w = 0.5f - 0.5f * SDL_cosf(s->time * (2.0f * BR_PI / 6.0f));
    br_animator_set_layer(e, s->hero_anim, 1, w, false);
    /* the fox's speed ramps 0 -> 1 -> 0 over 12 s: idle, walk, run, walk, idle */
    float phase = SDL_fmodf(s->time, 12.0f) / 6.0f;
    float speed = phase < 1.0f ? phase : 2.0f - phase;
    br_animator_set_float(e, s->fox_anim, "speed", speed);
    for (int i = 0; s->bench_anims != NULL && i < g_bench_foxes; i++) {
        float p = SDL_fmodf(s->time + (float)i * 0.37f, 12.0f) / 6.0f;
        br_animator_set_float(e, s->bench_anims[i], "speed", p < 1.0f ? p : 2.0f - p);
    }

    uint64_t ns[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < br_system_count(e); i++) {
        br_system_info info;
        for (int k = 0; k < 3 && br_system_get_info(e, i, &info); k++) {
            ns[k] += SDL_strcmp(info.name, g_timed[k]) == 0 ? info.last_time_ns : 0;
        }
    }
    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0 && br_ticker_frame(e) > 30) {
        s->frame_ns_sum += now - s->last_ns;
        for (int k = 0; k < 3; k++) {
            s->anim_ns_sum[k] += ns[k];
        }
        s->frame_count++;
    }
    s->last_ns = now;

    uint32_t animators = 0;
    uint32_t tracks = 0;
    br_animation_stats(e, &animators, &tracks);
    const char *state = br_animator_state(e, s->fox_anim);
    char line[256];
    double anim_ms = (double)(ns[0] + ns[1] + ns[2]) / 1e6;
    if (br_app_deterministic(e)) {
        anim_ms = 0.0; /* wall-clock numbers would make every golden image differ */
    }
    SDL_snprintf(line, sizeof(line),
                 "Fox state: %s (speed %.2f)   wave layer %.0f%%   steps %d\nanimators %u, tracks %u, tweens %u   animation %.3f ms",
                 state != NULL ? state : "-", (double)speed, (double)(w * 100.0f), s->steps, animators, tracks,
                 br_tween_count(e), anim_ms);
    br_text_set_text(e, s->info, line);
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    if (g_bench_foxes > 0 || g_bench_tweens > 0) {
        double n = s->frame_count > 0 ? (double)s->frame_count : 1.0;
        double frame = (double)s->frame_ns_sum / n / 1e6;
        double upd = (double)s->anim_ns_sum[0] / n / 1e6;
        double tw = (double)s->anim_ns_sum[1] / n / 1e6;
        double skin = (double)s->anim_ns_sum[2] / n / 1e6;
        double total = upd + tw + skin;
        br_log_info("animation_demo: %d bench foxes, %d bench tweens, %u animators: avg frame %.3f ms, animation %.3f ms "
                    "(%.0f%%: animators %.3f, tweens %.3f, skinning %.3f) over %llu frames",
                    g_bench_foxes, g_bench_tweens, br_animator_count(e), frame, total,
                    frame > 0.0 ? 100.0 * total / frame : 0.0, upd, tw, skin, (unsigned long long)s->frame_count);
    }
    br_animator_remove(e, br_animator_get(e, br_anim_target_2d(s->flip))); /* clips must outlive their animators */
    br_anim_clip_destroy(s->flip_clip);
    br_free(s->bench_anims);
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    setup_bench_tweens(e);
    setup_hero(e, s);
    setup_flipbook(e, s);
    setup_3d(e, s);
    setup_panel(e, s);
    br_text_style ts = br_text_style_default();
    ts.font_size = 18.0f;
    ts.fill = br_color_from_hex(0xF0F0F0, 1.0f);
    s->info = br_text_create(e, "", &ts);
    br_node_set_position(e, s->info, 16.0f, 12.0f);
    br_node_add_child(e, br_scene_root(e), s->info);
    br_system_add(e, "demo.update", BR_PHASE_UPDATE, 0, demo_update, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--bench-foxes") == 0 && i + 1 < argc) {
            g_bench_foxes = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--bench-tweens") == 0 && i + 1 < argc) {
            g_bench_tweens = SDL_atoi(argv[++i]);
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - animation demo";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR; /* the source tree (CMake) */
    desc.init = demo_init;
    return desc;
}
