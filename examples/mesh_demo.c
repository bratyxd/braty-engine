/* mesh_demo - meshes: plain (vertex colours), simple, plane, rope, perspective, custom shader,
 * and one mesh above 65535 vertices.
 *
 *   mesh_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Row 1: a hexagon fan with vertex colours | a MeshSimple quad from flat float arrays |
 *        a 12x12 plane with a waving grid | a perspective card turning around its vertical axis.
 * Row 2: a rope snake (texture stretched over it) | a rope with a repeating texture |
 *        a plane drawn with a custom fragment shader (engine/shaders/tiling.frag.hlsl, uniforms
 *        repeat the checker 3x2) | a 300x300-vertex plane (90000 vertices: one custom draw).
 */
#include "braty/braty_main.h"

#include "tiling.frag.h"

enum { ROPE_POINTS = 24, WAVE = 12 };

typedef struct demo_state {
    br_texture checker;
    br_texture stripes;
    br_texture stripes_repeat;
    br_node    plane;
    br_vec2    plane_rest[WAVE * WAVE];
    br_node    card;
    br_node    rope;
    br_node    rope_repeat;
} demo_state;

static void put(uint8_t *p, int w, int x, int y, uint32_t rgba)
{
    uint8_t *px = &p[(y * w + x) * 4];
    px[0] = (uint8_t)(rgba >> 24);
    px[1] = (uint8_t)(rgba >> 16);
    px[2] = (uint8_t)(rgba >> 8);
    px[3] = (uint8_t)rgba;
}

static br_texture make_checker(br_engine *e)
{
    enum { S = 128 };
    static uint8_t p[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            bool light = ((x / 16) + (y / 16)) % 2 == 0;
            uint32_t c = light ? 0xF0F0F0FFu : (0x2050A0FFu + ((uint32_t)x << 24));
            if (x < 4 && y < 4) {
                c = 0xFF2020FFu; /* red top-left marker */
            }
            put(p, S, x, y, c);
        }
    }
    return br_texture_create_rgba8(e, S, S, p, NULL);
}

static br_texture make_stripes(br_engine *e, br_address_mode address)
{
    enum { W = 64, H = 20 };
    uint8_t p[W * H * 4];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t c = (x / 8) % 2 == 0 ? 0x40D070FFu : 0xF0E060FFu;
            if (y < 2 || y >= H - 2) {
                c = 0x204020FFu;
            }
            put(p, W, x, y, c);
        }
    }
    br_texture_style style = { BR_FILTER_LINEAR, address, false, 1.0f };
    return br_texture_create_rgba8(e, W, H, p, &style);
}

static void add(br_engine *e, br_node n, float x, float y)
{
    br_node_set_position(e, n, br_vec2_make(x, y));
    br_node_add_child(e, br_stage_root(e), n);
}

static void setup(br_engine *e, demo_state *s)
{
    /* hexagon fan with vertex colours over the white texture */
    br_vec2 pos[7];
    br_vec2 uv[7];
    uint32_t colors[7];
    uint32_t idx[18];
    pos[0] = br_vec2_make(0, 0);
    colors[0] = br_render_pack_color(br_color_make(1, 1, 1, 1), 1.0f);
    for (int i = 0; i < 6; i++) {
        float a = (float)i / 6.0f * 2.0f * BR_PI;
        pos[i + 1] = br_vec2_make(SDL_cosf(a) * 90.0f, SDL_sinf(a) * 90.0f);
        colors[i + 1] = br_render_pack_color(br_color_from_hsl((float)i * 60.0f, 0.9f, 0.55f, 1.0f), 1.0f);
        idx[i * 3 + 0] = 0;
        idx[i * 3 + 1] = (uint32_t)i + 1;
        idx[i * 3 + 2] = (uint32_t)((i + 1) % 6) + 1;
    }
    for (int i = 0; i < 7; i++) {
        uv[i] = br_vec2_make(0.5f, 0.5f);
    }
    add(e, br_mesh_create(e, br_texture_white(e), pos, uv, colors, 7, idx, 18), 120, 120);

    /* MeshSimple: flat arrays */
    const float verts[8] = { 0, 0, 160, 20, 140, 160, 10, 140 };
    const float uvs[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    const uint32_t quad[6] = { 0, 1, 2, 0, 2, 3 };
    add(e, br_mesh_simple_create(e, s->checker, verts, uvs, 4, quad, 6), 240, 40);

    /* snippet: docs/agents/mesh.md */
    br_node plane = br_mesh_plane_create(e, s->checker, WAVE, WAVE);
    uint32_t count = 0;
    br_vec2 *p = br_mesh_positions(e, plane, &count); /* edit in place, every frame */
    for (uint32_t i = 0; i < count; i++) {
        s->plane_rest[i] = p[i];
    }
    br_node_set_scale(e, plane, br_vec2_make(1.3f, 1.3f));
    add(e, plane, 450, 30);
    /* end snippet */
    s->plane = plane;

    s->card = br_perspective_mesh_create(e, s->checker, 10, 10);
    add(e, s->card, 750, 30);

    br_vec2 rope_points[ROPE_POINTS];
    for (int i = 0; i < ROPE_POINTS; i++) {
        rope_points[i] = br_vec2_make((float)i * 14.0f, 0.0f);
    }
    s->rope = br_mesh_rope_create(e, s->stripes, rope_points, ROPE_POINTS, 0.0f);
    add(e, s->rope, 30, 360);
    s->rope_repeat = br_mesh_rope_create(e, s->stripes_repeat, rope_points, ROPE_POINTS, 0.5f);
    add(e, s->rope_repeat, 30, 460);

    /* custom shader: tiling.frag repeats the texture 3x2 inside its frame */
    br_node custom = br_mesh_plane_create(e, s->checker, 2, 2);
    br_vec2 *cuv = br_mesh_uvs(e, custom, &count);
    for (uint32_t i = 0; i < count; i++) {
        cuv[i] = br_vec2_make(cuv[i].x * 3.0f, cuv[i].y * 2.0f);
    }
    float uniforms[12] = { 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1 }; /* origin, x step, y step, clamp */
    br_mesh_set_shader(e, custom, NULL, &br_shader_tiling_frag, NULL, 0, uniforms, sizeof(uniforms));
    br_node_set_scale(e, custom, br_vec2_make(1.5f, 1.0f));
    add(e, custom, 420, 330);

    /* above the batcher limit: one 32-bit-index custom draw */
    br_node big = br_mesh_plane_create(e, s->checker, 300, 300);
    br_vec2 *bp = br_mesh_positions(e, big, &count);
    for (uint32_t i = 0; i < count; i++) {
        float r = bp[i].x / 128.0f * 0.8f + 0.2f;
        float a = bp[i].y / 128.0f * 2.0f * BR_PI;
        bp[i] = br_vec2_make(64.0f + SDL_cosf(a) * r * 80.0f, 64.0f + SDL_sinf(a) * r * 80.0f);
    }
    add(e, big, 800, 330);
    br_log_info("mesh_demo: big mesh has %u vertices", count);
}

static void animate_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);
    uint32_t count = 0;
    br_vec2 *p = br_mesh_positions(e, s->plane, &count);
    for (uint32_t i = 0; i < count; i++) {
        br_vec2 r = s->plane_rest[i];
        p[i] = br_vec2_make(r.x + SDL_sinf(t * 2.0f + r.y * 0.08f) * 6.0f, r.y + SDL_cosf(t * 1.7f + r.x * 0.07f) * 6.0f);
    }

    /* a card turning around its vertical axis */
    float c = SDL_cosf(t * 0.8f + 0.6f);
    float half = 64.0f * c;
    float lean = 18.0f * SDL_sinf(t * 0.8f + 0.6f);
    br_perspective_mesh_set_corners(e, s->card, br_vec2_make(128 - half, lean), br_vec2_make(128 + half, -lean),
                                    br_vec2_make(128 + half, 200 + lean), br_vec2_make(128 - half, 200 - lean));

    br_node ropes[2] = { s->rope, s->rope_repeat };
    for (int r = 0; r < 2; r++) {
        br_vec2 *pts = br_mesh_rope_points(e, ropes[r], &count);
        for (uint32_t i = 0; i < count; i++) {
            pts[i].y = SDL_sinf(t * 3.0f + (float)i * 0.4f + (float)r) * 30.0f;
        }
    }
}

static void demo_shutdown(br_engine *e, void *user)
{
    br_render_stats stats;
    br_render_get_stats(e, &stats);
    br_log_info("mesh_demo: last frame %u draw calls, %u batches, %u vertices", stats.draw_calls, stats.batches,
                stats.vertices);
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x202430, 1.0f));
    s->checker = make_checker(e);
    s->stripes = make_stripes(e, BR_ADDRESS_CLAMP);
    s->stripes_repeat = make_stripes(e, BR_ADDRESS_REPEAT);
    setup(e, s);
    br_system_add(e, "demo.animate", BR_PHASE_UPDATE, 0, animate_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - mesh demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
