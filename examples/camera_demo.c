/* camera_demo - Unity-style 2D cameras: split screen, a minimap, a UI layer anchored to corners.
 *
 *   camera_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *               [--size WxH]   window size (default 1280x720; try 1680x720 and 540x960)
 *
 * World (layer 1, 2400 x 1400 stage units): a tiled ground, trees and two players moving on
 * fixed paths. Camera 1 (left half) and camera 2 (right half) follow one player each
 * (camera.follow: damping, dead zone, bounds); camera 2 shakes at frame 40 (camera.shake).
 * A third camera renders the whole world into a render texture (minimap), with markers on
 * layer 4 that only it draws. The UI camera (depth 10, layer 2, clear nothing) shows panels
 * anchored to the corners (camera.anchor) and the minimap sprite at the top centre.
 * Every camera keeps its vertical extent at any window size; the horizontal extent follows.
 */
#include "braty/braty_main.h"

static int g_width = 1280;
static int g_height = 720;

enum { WORLD_W = 2400, WORLD_H = 1400, TILE = 100 };
#define LAYER_WORLD 1u
#define LAYER_UI 2u
#define LAYER_MAP 4u

typedef struct demo_state {
    br_node    players[2];
    br_node    markers[2];
    br_node    cameras[2];
    br_node    minimap_camera;
    br_node    ui_camera;
    br_texture minimap;
    float      time;
    uint64_t   frame;
} demo_state;

static br_color hex(uint32_t rgb)
{
    return br_color_from_hex(rgb, 1.0f);
}

static br_vec2 player_position(int i, float t)
{
    if (i == 0) {
        return br_vec2_make(700.0f + 420.0f * SDL_cosf(t * 0.7f), 700.0f + 380.0f * SDL_sinf(t * 0.7f));
    }
    return br_vec2_make(1700.0f + 450.0f * SDL_sinf(t * 0.6f), 700.0f + 300.0f * SDL_sinf(t * 1.2f));
}

static void build_world(demo_state *s, br_engine *e)
{
    br_node root = br_scene_root(e);
    br_node ground = br_graphics_create(e, NULL);
    br_graphics_context *g = br_graphics_context_of(e, ground);
    for (int y = 0; y < WORLD_H / TILE; y++) {
        for (int x = 0; x < WORLD_W / TILE; x++) {
            br_graphics_rect(g, (float)(x * TILE), (float)(y * TILE), (float)TILE, (float)TILE);
            br_graphics_fill_color(g, hex(((x + y) & 1) ? 0x5E8C4A : 0x6E9C55));
        }
    }
    br_stroke_style border = br_graphics_stroke_style(12.0f, hex(0x3A2A1A));
    br_graphics_rect(g, 0.0f, 0.0f, (float)WORLD_W, (float)WORLD_H);
    br_graphics_stroke(g, &border);
    br_node_add_child(e, root, ground);

    br_node trees = br_graphics_create(e, NULL);
    br_graphics_context *t = br_graphics_context_of(e, trees);
    uint32_t seed = 12345u;
    for (int i = 0; i < 60; i++) {
        seed = seed * 1664525u + 1013904223u;
        float x = 60.0f + (float)(seed >> 8) / 16777216.0f * (WORLD_W - 120.0f);
        seed = seed * 1664525u + 1013904223u;
        float y = 60.0f + (float)(seed >> 8) / 16777216.0f * (WORLD_H - 120.0f);
        br_graphics_circle(t, x, y + 8.0f, 34.0f);
        br_graphics_fill_color(t, br_color_from_hex(0x000000, 0.25f));
        br_graphics_circle(t, x, y, 32.0f);
        br_graphics_fill_color(t, hex(0x2F5D2A));
        br_graphics_circle(t, x - 8.0f, y - 8.0f, 14.0f);
        br_graphics_fill_color(t, hex(0x4C8A3F));
    }
    br_node_add_child(e, root, trees);

    uint32_t colors[2] = { 0xE63946, 0x3A86FF };
    for (int i = 0; i < 2; i++) {
        br_node p = br_graphics_create(e, NULL);
        br_graphics_context *pg = br_graphics_context_of(e, p);
        if (i == 0) {
            br_graphics_circle(pg, 0.0f, 0.0f, 30.0f);
        } else {
            br_graphics_star(pg, 0.0f, 0.0f, 5, 36.0f, 16.0f, 0.0f);
        }
        br_graphics_fill_color(pg, hex(colors[i]));
        br_stroke_style st = br_graphics_stroke_style(4.0f, hex(0xFFFFFF));
        br_graphics_stroke(pg, &st);
        br_node_add_child(e, root, p);
        s->players[i] = p;
        /* a big dot only the minimap camera draws */
        br_node m = br_graphics_create(e, NULL);
        br_graphics_circle(br_graphics_context_of(e, m), 0.0f, 0.0f, 70.0f);
        br_graphics_fill_color(br_graphics_context_of(e, m), hex(colors[i]));
        br_node_set_layer_bits(e, m, LAYER_MAP);
        br_node_add_child(e, root, m);
        s->markers[i] = m;
    }
}

static br_node make_follow_camera(br_engine *e, br_node target, float viewport_x, uint32_t background)
{
    br_camera c = br_camera_default_2d();
    c.orthographic_size = 300.0f;                          /* 600 stage units tall at any window size */
    c.viewport_rect = br_rect_make(viewport_x, 0.0f, 0.5f, 1.0f);
    c.culling_mask = LAYER_WORLD;
    c.background_color = hex(background);
    br_node cam = br_camera_create(e, &c);
    br_vec2 start = br_node_position(e, target);
    br_node_set_position(e, cam, start.x, start.y);
    br_node_add_child(e, br_scene_root(e), cam);
    br_camera_follow *f = br_component_add(e, cam, br_camera_follow_component(e));
    f->target = target;
    f->damping = 0.15f;
    f->dead_zone = br_vec2_make(20.0f, 20.0f);
    f->bounds = br_rect_make(0.0f, 0.0f, (float)WORLD_W, (float)WORLD_H);
    f->use_bounds = true;
    return cam;
}

/* A UI panel with a label, pinned to a corner of the UI camera's viewport. */
static void make_panel(br_engine *e, demo_state *s, const char *text, br_vec2 anchor, float w, float h)
{
    br_node n = br_node_create_container(e);
    br_node_set_layer_bits(e, n, LAYER_UI);
    br_node_add_child(e, br_scene_root(e), n);
    /* the content grows away from the anchored corner */
    float x0 = -anchor.x * w;
    float y0 = -anchor.y * h;
    br_node bg = br_graphics_create(e, NULL);
    br_graphics_context *g = br_graphics_context_of(e, bg);
    br_graphics_round_rect(g, x0, y0, w, h, 10.0f);
    br_graphics_fill_color(g, br_color_from_hex(0x10131C, 0.8f));
    br_stroke_style st = br_graphics_stroke_style(2.0f, hex(0xF1FAEE));
    br_graphics_stroke(g, &st);
    br_node_add_child(e, n, bg);
    br_text_style ts = br_text_style_default();
    ts.font_size = 22.0f;
    ts.fill = hex(0xF1FAEE);
    br_node label = br_text_create(e, text, &ts);
    br_text_set_anchor(e, label, 0.5f, 0.5f);
    br_node_set_position(e, label, x0 + w * 0.5f, y0 + h * 0.5f);
    br_node_add_child(e, n, label);
    br_camera_anchor *a = br_component_add(e, n, br_camera_anchor_component(e));
    a->camera = s->ui_camera;
    a->anchor = anchor;
    a->offset = br_vec2_make(anchor.x < 0.5f ? 16.0f : (anchor.x > 0.5f ? -16.0f : 0.0f),
                             anchor.y < 0.5f ? 16.0f : -16.0f);
    a->safe_area = true;
}

static void build_ui(demo_state *s, br_engine *e)
{
    br_camera c = br_camera_default_2d();          /* 720 UI units tall, centred on (640, 360) */
    c.culling_mask = LAYER_UI;
    c.depth = 10.0f;
    c.clear_flags = BR_CAMERA_CLEAR_NOTHING;
    s->ui_camera = br_camera_create(e, &c);
    br_node_set_position(e, s->ui_camera, 640.0f, 360.0f);
    br_node_add_child(e, br_scene_root(e), s->ui_camera);

    /* split line: anchored to the top centre, 720 units tall = the whole UI height */
    br_node line = br_graphics_create(e, NULL);
    br_graphics_rect(br_graphics_context_of(e, line), -3.0f, 0.0f, 6.0f, 720.0f);
    br_graphics_fill_color(br_graphics_context_of(e, line), hex(0x10131C));
    br_node_set_layer_bits(e, line, LAYER_UI);
    br_node_add_child(e, br_scene_root(e), line);
    br_camera_anchor *a = br_component_add(e, line, br_camera_anchor_component(e));
    a->camera = s->ui_camera;
    a->anchor = br_vec2_make(0.5f, 0.0f);

    char size[64];
    SDL_snprintf(size, sizeof(size), "%d x %d", g_width, g_height);
    make_panel(e, s, "Player 1", br_vec2_make(0.0f, 0.0f), 150.0f, 44.0f);
    make_panel(e, s, "Player 2", br_vec2_make(1.0f, 0.0f), 150.0f, 44.0f);
    make_panel(e, s, "split screen", br_vec2_make(0.0f, 1.0f), 170.0f, 44.0f);
    make_panel(e, s, size, br_vec2_make(1.0f, 1.0f), 170.0f, 44.0f);

    /* minimap: a sprite showing the minimap camera's render texture */
    br_node map = br_sprite_create(e, s->minimap);
    br_sprite_set_anchor(e, map, 0.5f, 0.0f);
    br_node_set_layer_bits(e, map, LAYER_UI);
    br_node_add_child(e, br_scene_root(e), map);
    a = br_component_add(e, map, br_camera_anchor_component(e));
    a->camera = s->ui_camera;
    a->anchor = br_vec2_make(0.5f, 0.0f);
    a->offset = br_vec2_make(0.0f, 12.0f);
}

static void demo_system(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    s->time += dt;
    s->frame++;
    for (int i = 0; i < 2; i++) {
        br_vec2 p = player_position(i, s->time);
        br_node_set_position(e, s->players[i], p.x, p.y);
        br_node_set_position(e, s->markers[i], p.x, p.y);
    }
    br_node_set_rotation(e, s->players[1], s->time * 2.0f);
    if (s->frame == 40) {
        br_camera_shake_add(e, s->cameras[1], 0.7f);
    }
}

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    build_world(s, e);
    for (int i = 0; i < 2; i++) {
        br_vec2 p = player_position(i, 0.0f);
        br_node_set_position(e, s->players[i], p.x, p.y);
    }
    s->cameras[0] = make_follow_camera(e, s->players[0], 0.0f, 0x1D3557);
    s->cameras[1] = make_follow_camera(e, s->players[1], 0.5f, 0x3D1D57);

    /* minimap: the whole world (1400 tall) into a 320 x 180 render texture, world + markers */
    s->minimap = br_render_texture_create(e, 320.0f, 180.0f, 1.0f);
    br_camera m = br_camera_default_2d();
    m.orthographic_size = WORLD_H * 0.5f;
    m.culling_mask = LAYER_WORLD | LAYER_MAP;
    m.target_texture = s->minimap;
    m.background_color = hex(0x10131C);
    s->minimap_camera = br_camera_create(e, &m);
    br_node_set_position(e, s->minimap_camera, WORLD_W * 0.5f, WORLD_H * 0.5f);
    br_node_add_child(e, br_scene_root(e), s->minimap_camera);

    build_ui(s, e);
    br_system_add(e, "demo.move", BR_PHASE_UPDATE, 0, demo_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0;
            int h = 0;
            if (SDL_sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_width = w;
                g_height = h;
            }
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - camera demo";
    desc.width = g_width;
    desc.height = g_height;
    desc.init = demo_init;
    return desc;
}
