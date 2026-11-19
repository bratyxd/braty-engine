/* profiler_demo - the profiler panel (F2), the corner readout and three load experiments.
 *
 *   profiler_demo [--gpu-load N] [--cpu-load MS] [--draw-load N] [--profiler-hud]
 *                 [--profile out.json] [--vsync off] [--frames N --screenshot out.png]
 *
 * A few hundred bouncing sprites under a game scope ("demo.move"). The panel is open unless
 * --profiler-hud is given (then only the corner readout shows). Loads, to see the verdict flip:
 *   --gpu-load N   N full-screen, almost transparent quads: one draw call, N x the fill work,
 *                  so the GPU estimate rises and the verdict reads "GPU bound"
 *   --cpu-load MS  a busy loop of MS milliseconds in an UPDATE system: "CPU bound (update)"
 *   --draw-load N  N tiny sprites alternating blend modes, so every one is its own draw call:
 *                  "CPU bound (render submit)"
 * Run the experiments with --vsync off (vsync turns any of them into "vsync limited" while the
 * frame still fits the refresh period). docs/PROFILING.md lists measured numbers.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

enum { BALLS = 300 };

typedef struct demo_state {
    br_node  balls[BALLS];
    br_vec2  velocity[BALLS];
    float    cpu_load_ms;
    uint32_t gpu_load;
    uint32_t draw_load;
    br_node  load_root;
} demo_state;

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

/* Moves the balls; the inner scope shows how a game adds its own. */
static void demo_move(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    br_profile_begin(e, "demo.move");
    for (uint32_t i = 0; i < BALLS; i++) {
        br_vec2 p = br_node_position(e, s->balls[i]);
        br_vec2 v = s->velocity[i];
        p.x += v.x * dt;
        p.y += v.y * dt;
        if (p.x < 0.0f || p.x > (float)w) {
            s->velocity[i].x = -v.x;
        }
        if (p.y < 0.0f || p.y > (float)h) {
            s->velocity[i].y = -v.y;
        }
        br_node_set_position(e, s->balls[i], p.x, p.y);
    }
    br_profile_end(e);
}

/* --cpu-load: burns the given time on the CPU, like an expensive game system would. */
static void demo_cpu_load(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    demo_state *s = user;
    uint64_t until = SDL_GetTicksNS() + (uint64_t)((double)s->cpu_load_ms * 1e6);
    volatile uint32_t sink = 0;
    while (SDL_GetTicksNS() < until) {
        sink = sink * 1664525u + 1013904223u;
    }
}

/* Keeps the --gpu-load quads covering the window. */
static void demo_fit_load(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    uint32_t count = br_node_child_count(e, s->load_root);
    for (uint32_t i = 0; i < count && i < s->gpu_load; i++) {
        br_node_set_scale(e, br_node_child_at(e, s->load_root, i), (float)w, (float)h);
    }
}

static void demo_parse_args(br_engine *e, demo_state *s)
{
    int argc = 0;
    char **argv = br_engine_args(e, &argc);
    for (int i = 1; i + 1 < argc; i++) {
        if (SDL_strcmp(argv[i], "--gpu-load") == 0) {
            s->gpu_load = (uint32_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--cpu-load") == 0) {
            s->cpu_load_ms = (float)SDL_atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--draw-load") == 0) {
            s->draw_load = (uint32_t)SDL_atoi(argv[++i]);
        }
    }
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    demo_parse_args(e, s);
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        br_gpu_set_clear_color(g, br_color_from_hex(0x1d2333, 1.0f));
    }
    br_node stage = br_scene_root(e);
    br_texture white = br_texture_white(e);

    /* the load layers go under the balls */
    s->load_root = br_node_create_container(e);
    br_node_add_child(e, stage, s->load_root);
    for (uint32_t i = 0; i < s->gpu_load; i++) {
        br_node quad = br_sprite_create(e, white);
        br_node_set_tint(e, quad, br_color_make(0.3f, 0.5f, 1.0f, 1.0f));
        br_node_set_alpha(e, quad, 0.004f);
        br_node_add_child(e, s->load_root, quad);
    }
    for (uint32_t i = 0; i < s->draw_load; i++) {
        br_node dot = br_sprite_create(e, white);
        br_node_set_position(e, dot, (float)(i % 400) * 3.0f, 400.0f + (float)(i / 400 % 100) * 3.0f);
        br_node_set_scale(e, dot, 2.0f, 2.0f);
        br_node_set_blend_mode(e, dot, i % 2 == 0 ? BR_BLEND_NORMAL : BR_BLEND_ADD); /* a draw call each */
        br_node_add_child(e, stage, dot);
    }
    for (uint32_t i = 0; i < BALLS; i++) {
        br_node ball = br_sprite_create(e, white);
        br_node_set_scale(e, ball, 10.0f, 10.0f);
        br_node_set_position(e, ball, br_engine_random_float(e) * 600.0f, br_engine_random_float(e) * 700.0f);
        br_node_set_tint(e, ball, br_color_from_hsl((float)i * 1.2f, 0.7f, 0.6f, 1.0f));
        br_node_add_child(e, stage, ball);
        s->balls[i] = ball;
        s->velocity[i] = br_vec2_make(br_engine_random_float(e) * 300.0f - 150.0f,
                                      br_engine_random_float(e) * 300.0f - 150.0f);
    }

    br_system_add(e, "demo.move", BR_PHASE_UPDATE, 0, demo_move, s);
    br_system_add(e, "demo.fit_load", BR_PHASE_LATE_UPDATE, 0, demo_fit_load, s);
    if (s->cpu_load_ms > 0.0f) {
        br_system_add(e, "demo.cpu_load", BR_PHASE_UPDATE, 10, demo_cpu_load, s);
    }

    /* the panel (F2) is open from the start; --profiler-hud shows only the corner readout */
    br_profiler_set_open(e, !br_profiler_hud(e));
    br_profiler_set_budget(e, "demo.cpu_load", 20.0f); /* logs once when a frame's busy loop exceeds 20 ms */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - profiler demo";
    desc.init = demo_init;
    return desc;
}
