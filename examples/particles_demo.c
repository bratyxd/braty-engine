/* particles_demo - a particle container with N bouncing, spinning particles (default 100000).
 *
 *   particles_demo [--count N] [--novsync] [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Particles use 4 frames of one generated atlas (one stored rotated, one trimmed), random tints
 * and scales. POSITION and ROTATION are dynamic (uploaded every frame); VERTEX, UVS and COLOR
 * are static (uploaded once, and again when the demo re-tints everything every 2 seconds via
 * br_particle_container_update). A second, small container top-left fades with its node alpha.
 * At shutdown it logs the average frame time (use --novsync to measure).
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

static int g_count = 100000;
static bool g_no_vsync = false;

enum { FRAMES = 4 };

typedef struct demo_state {
    br_texture atlas;
    br_texture frames[FRAMES];
    br_node    container;
    br_node    small;
    br_vec2   *velocity;    /* one per particle, parallel to the container's array */
    float     *spin;
    uint32_t   rng;
    int        last_retint;
    uint64_t   frame_ns_sum;
    uint64_t   frame_ns_count;
    uint64_t   last_ns;
} demo_state;

static float rnd(demo_state *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

static void put(uint8_t *p, int w, int x, int y, uint32_t rgba)
{
    uint8_t *px = &p[(y * w + x) * 4];
    px[0] = (uint8_t)(rgba >> 24);
    px[1] = (uint8_t)(rgba >> 16);
    px[2] = (uint8_t)(rgba >> 8);
    px[3] = (uint8_t)rgba;
}

/* 64x16 atlas: a disc, a ring, a square with a notch (stored rotated) and a trimmed star. */
static void make_atlas(br_engine *e, demo_state *s)
{
    enum { W = 64, H = 16 };
    static uint8_t p[W * H * 4];
    SDL_memset(p, 0, sizeof(p));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < 16; x++) {
            float dx = (float)x - 7.5f;
            float dy = (float)y - 7.5f;
            float r = SDL_sqrtf(dx * dx + dy * dy);
            if (r < 7.5f) {
                put(p, W, x, y, 0xFFFFFFFFu);
            }
            if (r < 7.5f && r > 4.5f) {
                put(p, W, 16 + x, y, 0xFFFFFFFFu);
            }
            /* logical notched square (notch top-left), stored rotated clockwise at x = 32 */
            bool notch = x < 6 && y < 6;
            if (!notch && x > 0 && y > 0 && x < 15 && y < 15) {
                put(p, W, 32 + (15 - y), x, 0xFFFFFFFFu);
            }
            /* star: 12x12 drawn at 50..61, 2..13 (trimmed from a 16x16 frame) */
            if (x >= 2 && x < 14 && y >= 2 && y < 14) {
                float sx = (float)x - 7.5f;
                float sy = (float)y - 7.5f;
                if (SDL_fabsf(sx) + SDL_fabsf(sy) < 6.0f || SDL_fabsf(sx) < 1.5f || SDL_fabsf(sy) < 1.5f) {
                    put(p, W, 48 + x, y, 0xFFFFFFFFu);
                }
            }
        }
    }
    s->atlas = br_texture_create_rgba8(e, W, H, p, NULL);
    br_texture_frame f;
    for (int i = 0; i < FRAMES; i++) {
        SDL_zero(f);
        f.frame = br_rect_make((float)(i * 16), 0, 16, 16);
        if (i == 2) {
            f.rotated = true;
        }
        if (i == 3) {
            f.frame = br_rect_make(50, 2, 12, 12);
            f.orig = br_rect_make(0, 0, 16, 16);
            f.trim = br_rect_make(2, 2, 12, 12);
        }
        s->frames[i] = br_texture_create_frame(e, s->atlas, &f);
    }
}

static uint32_t random_tint(demo_state *s)
{
    br_color c = br_color_from_hsl(rnd(s) * 360.0f, 0.8f, 0.6f, 1.0f);
    return br_color_to_hex(c);
}

static void setup(br_engine *e, demo_state *s)
{
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    s->velocity = br_alloc(sizeof(br_vec2) * (size_t)g_count);
    s->spin = br_alloc(sizeof(float) * (size_t)g_count);

    /* snippet: docs/agents/particles.md */
    br_node pc = br_particle_container_create(e, s->atlas, BR_PARTICLE_POSITION | BR_PARTICLE_ROTATION);
    br_particle *p = br_particle_container_add(e, pc, (uint32_t)g_count);
    for (int i = 0; p != NULL && i < g_count; i++) {
        p[i].x = rnd(s) * (float)w;
        p[i].y = rnd(s) * (float)h;
        p[i].scale_x = p[i].scale_y = 0.5f + rnd(s);
        p[i].anchor_x = p[i].anchor_y = 0.5f;
        p[i].texture = s->frames[i % FRAMES];
        p[i].tint = random_tint(s);
        p[i].alpha = 0.6f + 0.4f * rnd(s);
    }
    br_node_add_child(e, br_stage_root(e), pc);
    /* end snippet */
    s->container = pc;
    for (int i = 0; i < g_count; i++) {
        s->velocity[i] = br_vec2_make((rnd(s) - 0.5f) * 300.0f, (rnd(s) - 0.5f) * 300.0f);
        s->spin[i] = (rnd(s) - 0.5f) * 6.0f;
    }

    /* small static container: four big frames side by side, faded through the node alpha */
    s->small = br_particle_container_create(e, s->atlas, 0);
    br_particle *q = br_particle_container_add(e, s->small, FRAMES);
    for (int i = 0; q != NULL && i < FRAMES; i++) {
        q[i].x = 40.0f + (float)i * 80.0f;
        q[i].y = 40.0f;
        q[i].scale_x = q[i].scale_y = 4.0f;
        q[i].anchor_x = q[i].anchor_y = 0.5f;
        q[i].texture = s->frames[i];
        q[i].tint = 0xFFE080;
    }
    br_node_set_position(e, s->small, 20, 20);
    br_node_add_child(e, br_stage_root(e), s->small);
}

static void particle_system(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    uint32_t count = 0;
    br_particle *p = br_particle_container_particles(e, s->container, &count);
    for (uint32_t i = 0; i < count; i++) {
        p[i].x += s->velocity[i].x * dt;
        p[i].y += s->velocity[i].y * dt;
        p[i].rotation += s->spin[i] * dt;
        if (p[i].x < 0.0f || p[i].x > (float)w) {
            s->velocity[i].x = -s->velocity[i].x;
        }
        if (p[i].y < 0.0f || p[i].y > (float)h) {
            s->velocity[i].y = -s->velocity[i].y;
        }
    }
    /* every 2 s: new colours for everyone; COLOR is static, so flag it */
    float t = (float)br_ticker_elapsed(e);
    if ((int)(t / 2.0f) != s->last_retint) {
        s->last_retint = (int)(t / 2.0f);
        for (uint32_t i = 0; i < count; i++) {
            p[i].tint = random_tint(s);
        }
        br_particle_container_update(e, s->container);
    }
    br_node_set_alpha(e, s->small, 0.55f + 0.45f * SDL_sinf(t * 2.0f));

    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0 && br_ticker_frame(e) > 10) {
        s->frame_ns_sum += now - s->last_ns;
        s->frame_ns_count += 1;
    }
    s->last_ns = now;
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    br_render_stats stats;
    br_render_get_stats(e, &stats);
    double avg = s->frame_ns_count > 0 ? (double)s->frame_ns_sum / (double)s->frame_ns_count / 1e6 : 0.0;
    br_log_info("particles_demo: %d particles, avg frame %.2f ms over %llu frames (%s), %u draw calls",
                g_count, avg, (unsigned long long)s->frame_ns_count, g_no_vsync ? "vsync off" : "vsync on",
                stats.draw_calls);
    br_system_info info;
    for (uint32_t i = 0; br_system_get_info(e, i, &info); i++) {
        if (info.last_time_ns > 100000) {
            br_log_info("  system %-24s %.2f ms", info.name, (double)info.last_time_ns / 1e6);
        }
    }
    br_free(s->velocity);
    br_free(s->spin);
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    s->rng = 777u;
    if (g_no_vsync) {
        br_app_set_vsync(e, BR_VSYNC_OFF);
    }
    br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x0C0E18, 1.0f));
    make_atlas(e, s);
    setup(e, s);
    br_system_add(e, "demo.particles", BR_PHASE_UPDATE, 0, particle_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            g_count = SDL_atoi(argv[++i]);
            if (g_count < 1) {
                g_count = 1;
            }
        } else if (SDL_strcmp(argv[i], "--novsync") == 0) {
            g_no_vsync = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - particles demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
