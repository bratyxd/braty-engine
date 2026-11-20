/* canvas3d_demo - 2D content in the 3D world (br_canvas3d).
 *
 *   canvas3d_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * In one View3D:
 *   - a cube with a billboarded nine-slice name plate (SDF text) above it;
 *   - a world-space sign with SDF text seen at a grazing angle, and a small sign right in front
 *     of the camera (both stay sharp);
 *   - tree sprites standing in the world (y-axis billboards) sorted with a transparent glass box;
 *   - a lit and an unlit canvas side by side (same content);
 *   - a texture-mode canvas whose content has a 2D filter (sepia + blur);
 *   - a button on a canvas: a click injected at frame 20 (through the normal input path, 3D
 *     picking and the canvas' 2D hit test) turns it green.
 */
#include "braty/braty_main.h"

typedef struct demo_state {
    br_node   view;
    br_node3d root;
    br_node3d button_canvas;
    br_node   button;
    br_node   button_bg;
    br_node   button_label;
    bool      clicked;
    uint64_t  frame;
    br_vec2   click_at;
} demo_state;

static br_color lin(uint32_t rgb)
{
    return br_color_to_linear(br_color_from_hex(rgb, 1.0f));
}

static br_color hex(uint32_t rgb)
{
    return br_color_from_hex(rgb, 1.0f);
}

/* ---- generated textures */

static br_texture make_tree_texture(br_engine *e)
{
    enum { W = 64, H = 96 };
    static uint8_t px[W * H * 4];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = &px[(y * W + x) * 4];
            float fx = (float)x - W * 0.5f + 0.5f;
            bool trunk = y >= 74 && SDL_fabsf(fx) < 5.0f;
            float half = (float)(y - 4) * 0.42f;        /* a triangle crown */
            bool crown = y >= 4 && y < 78 && SDL_fabsf(fx) < half && ((y - 4) % 24) < 20 + (y > 50 ? 4 : 0);
            if (trunk) {
                p[0] = 0x6B; p[1] = 0x45; p[2] = 0x2A; p[3] = 255;
            } else if (crown) {
                float shade = 0.75f + 0.25f * (fx < 0.0f ? 1.0f : 0.0f);
                p[0] = (uint8_t)(0x2E * shade); p[1] = (uint8_t)(0x8B * shade); p[2] = (uint8_t)(0x57 * shade); p[3] = 255;
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }
    return br_texture_create_rgba8(e, W, H, px, NULL);
}

static br_texture make_panel_texture(br_engine *e)
{
    enum { S = 48, B = 12 };
    static uint8_t px[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            uint8_t *p = &px[(y * S + x) * 4];
            /* rounded corners: distance to the inner rect */
            float dx = SDL_max(0.0f, SDL_max((float)B - (float)x - 0.5f, (float)x + 0.5f - (float)(S - B)));
            float dy = SDL_max(0.0f, SDL_max((float)B - (float)y - 0.5f, (float)y + 0.5f - (float)(S - B)));
            float d = SDL_sqrtf(dx * dx + dy * dy);
            float a = br_clampf((float)B - d, 0.0f, 1.0f);
            bool border = d > (float)B - 4.0f;
            p[0] = border ? 0xF4 : 0x1D;
            p[1] = border ? 0xA2 : 0x23;
            p[2] = border ? 0x61 : 0x3A;
            p[3] = (uint8_t)(a * (border ? 255.0f : 225.0f));
        }
    }
    return br_texture_create_rgba8(e, S, S, px, NULL);
}

/* ---- helpers */

static br_material3d make_material(br_engine *e, uint32_t rgb, float alpha, bool blend)
{
    br_material3d_desc d = br_material3d_default(BR_MATERIAL3D_PBR);
    d.base_color = lin(rgb);
    d.base_color.a = alpha;
    d.roughness = 0.5f;
    if (blend) {
        d.alpha_mode = BR_ALPHA_BLEND;
        d.roughness = 0.1f;
    }
    return br_material3d_create(e, &d);
}

static br_node3d add_mesh(br_engine *e, demo_state *s, br_geometry3d geo, br_material3d mat, br_vec3 p)
{
    br_node3d n = br_mesh3d_create(e, geo, mat);
    br_node3d_set_position(e, n, p);
    br_node3d_add_child(e, s->root, n);
    return n;
}

static br_text_style sdf_style(br_engine *e, float size, uint32_t rgb)
{
    br_text_style st = br_text_style_default();
    st.font = br_canvas3d_sdf_font(e);
    st.font_size = size;
    st.fill = hex(rgb);
    return st;
}

static br_node add_text(br_engine *e, br_node parent, const char *text, const br_text_style *st, float x, float y)
{
    br_node t = br_text_create(e, text, st);
    br_text_set_anchor(e, t, 0.5f, 0.5f);
    br_node_set_position(e, t, x, y);
    br_node_add_child(e, parent, t);
    return t;
}

/* A panel canvas: coloured stripes + a label (used for lit / unlit / texture mode). */
static br_node3d make_panel(br_engine *e, demo_state *s, const char *label, br_vec3 pos, float yaw)
{
    br_canvas3d_desc d = br_canvas3d_default(300.0f, 200.0f);
    br_node3d c = br_canvas3d_create(e, &d);
    br_node root = br_canvas3d_root(e, c);
    br_node g = br_graphics_create(e, NULL);
    br_graphics_context *gc = br_graphics_context_of(e, g);
    uint32_t colors[4] = { 0xE63946, 0xF4A261, 0x2A9D8F, 0x457B9D };
    for (int i = 0; i < 4; i++) {
        br_graphics_rect(gc, 0.0f, (float)i * 50.0f, 300.0f, 50.0f);
        br_graphics_fill_color(gc, hex(colors[i]));
    }
    br_graphics_circle(gc, 230.0f, 100.0f, 50.0f);
    br_graphics_fill_color(gc, hex(0xF1FAEE));
    br_node_add_child(e, root, g);
    br_text_style st = sdf_style(e, 44.0f, 0x1D3557);
    add_text(e, root, label, &st, 100.0f, 100.0f);
    br_node3d_set_position(e, c, pos);
    br_node3d_set_euler(e, c, br_vec3_make(0.0f, yaw, 0.0f));
    br_node3d_add_child(e, s->root, c);
    return c;
}

/* ---- the button */

static void button_click(br_engine *e, br_event *ev, void *user)
{
    (void)ev;
    demo_state *s = user;
    s->clicked = true;
    br_node_set_tint(e, s->button_bg, br_color_from_hex(0x52B788, 1.0f));
    br_text_set_text(e, s->button_label, "Clicked!");
}

static void make_button(br_engine *e, demo_state *s)
{
    br_canvas3d_desc d = br_canvas3d_default(260.0f, 90.0f);
    s->button_canvas = br_canvas3d_create(e, &d);
    br_node root = br_canvas3d_root(e, s->button_canvas);
    s->button = br_node_create_container(e);
    br_node_set_event_mode(e, s->button, BR_EVENT_MODE_STATIC);
    br_node_add_child(e, root, s->button);
    s->button_bg = br_graphics_create(e, NULL);
    br_graphics_context *g = br_graphics_context_of(e, s->button_bg);
    br_graphics_round_rect(g, 4.0f, 4.0f, 252.0f, 82.0f, 18.0f);
    br_graphics_fill_color(g, hex(0xFFFFFF));
    br_stroke_style st = br_graphics_stroke_style(5.0f, hex(0xFFFFFF));
    br_graphics_stroke(g, &st);
    br_node_set_tint(e, s->button_bg, hex(0xC1121F));
    br_node_add_child(e, s->button, s->button_bg);
    br_text_style ts = sdf_style(e, 40.0f, 0xFFFFFF);
    s->button_label = add_text(e, s->button, "Click me", &ts, 130.0f, 45.0f);
    br_node_on(e, s->button, BR_EVENT_CLICK, button_click, s);
    br_node3d_set_position(e, s->button_canvas, br_vec3_make(0.0f, 2.7f, -1.0f));
    br_node3d_add_child(e, s->root, s->button_canvas);
}

/* Injects a mouse click on the button at frames 20 (move + down) and 22 (up), through SDL events. */
static void click_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    s->frame++;
    if (s->frame == 20) {
        br_view3d_world_to_screen(e, s->view, br_node3d_world_position(e, s->button_canvas), &s->click_at);
        br_input_inject_pointer_move(e, s->click_at.x, s->click_at.y);
        br_input_inject_pointer_down(e, s->click_at.x, s->click_at.y, SDL_BUTTON_LEFT);
    } else if (s->frame == 22) {
        br_input_inject_pointer_up(e, s->click_at.x, s->click_at.y, SDL_BUTTON_LEFT);
        br_log_info("canvas3d_demo: click at (%.0f, %.0f) -> %s", (double)s->click_at.x, (double)s->click_at.y,
                    s->clicked ? "button clicked" : "MISSED");
    }
}

/* ---- scene */

static void build_scene(br_engine *e, demo_state *s)
{
    s->view = br_view3d_create(e, 1280.0f, 720.0f);
    br_view3d_set_auto_resize(e, s->view, true);
    br_view3d_set_msaa(e, s->view, 4);
    br_view3d_set_background(e, s->view, lin(0x8EC5F0));
    br_view3d_set_environment(e, s->view, lin(0xBFD9F2), lin(0x5A4A3A), 0.45f);
    br_node_add_child(e, br_stage_root(e), s->view);
    s->root = br_view3d_root(e, s->view);
    br_node3d cam = br_view3d_camera(e, s->view);
    br_camera3d_get(e, cam)->fov_y = 55.0f * BR_DEG_TO_RAD;
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 2.6f, 7.5f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 1.3f, -1.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    br_node3d ground = add_mesh(e, s, br_geometry3d_plane(e, 40.0f, 40.0f, 1, 1), make_material(e, 0x7A9A6A, 1.0f, false),
                                br_vec3_make(0.0f, 0.0f, 0.0f));
    br_mesh3d_get(e, ground)->cast_shadows = false;
    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 2.2f;
    sun.color = lin(0xFFF2DD);
    sun.cast_shadows = true;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_add_child(e, s->root, sun_node);
    br_node3d_set_position(e, sun_node, br_vec3_make(8.0f, 10.0f, 4.0f));
    br_node3d_look_at(e, sun_node, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    /* hero cube with a billboarded nine-slice name plate */
    add_mesh(e, s, br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f)), make_material(e, 0xC1121F, 1.0f, false),
             br_vec3_make(-3.2f, 0.5f, 0.5f));
    br_node3d plate = br_nine_slice3d_create(e, make_panel_texture(e), 12, 12, 12, 12, 240.0f, 64.0f, 200.0f);
    br_canvas3d_get(e, plate)->billboard = BR_CANVAS3D_BILLBOARD_FULL;
    br_text_style name = sdf_style(e, 34.0f, 0xF1FAEE);
    add_text(e, br_canvas3d_root(e, plate), "Hero  Lv 7", &name, 120.0f, 32.0f);
    br_node3d_set_position(e, plate, br_vec3_make(-3.2f, 1.45f, 0.5f));
    br_node3d_add_child(e, s->root, plate);

    /* a long sign seen at a grazing angle, and a small one right in front of the camera */
    br_canvas3d_desc sign = br_canvas3d_default(900.0f, 120.0f);
    br_node3d far_sign = br_canvas3d_create(e, &sign);
    br_text_style big = sdf_style(e, 80.0f, 0x1D3557);
    br_node sign_bg = br_graphics_create(e, NULL);
    br_graphics_rect(br_graphics_context_of(e, sign_bg), 0.0f, 0.0f, 900.0f, 120.0f);
    br_graphics_fill_color(br_graphics_context_of(e, sign_bg), hex(0xF1FAEE));
    br_node_add_child(e, br_canvas3d_root(e, far_sign), sign_bg);
    add_text(e, br_canvas3d_root(e, far_sign), "SDF text stays sharp", &big, 450.0f, 60.0f);
    br_node3d_set_position(e, far_sign, br_vec3_make(4.4f, 1.3f, -2.6f));
    br_node3d_set_euler(e, far_sign, br_vec3_make(0.0f, -72.0f * BR_DEG_TO_RAD, 0.0f));
    br_node3d_add_child(e, s->root, far_sign);

    br_text_style small = sdf_style(e, 20.0f, 0xFFFFFF);
    small.stroke_width = 3.0f;
    small.stroke_color = hex(0x1D3557);
    br_node3d near_text = br_text3d_create(e, "close-up: 20 px SDF text, 400 px per unit", &small, 400.0f);
    br_node3d_set_position(e, near_text, br_vec3_make(0.55f, 1.95f, 6.2f));
    br_node3d_set_euler(e, near_text, br_vec3_make(-10.0f * BR_DEG_TO_RAD, 0.0f, 0.0f));
    br_node3d_add_child(e, s->root, near_text);

    /* trees (y-axis billboards) around a transparent glass box: sorted back to front together */
    br_texture tree = make_tree_texture(e);
    br_vec3 trees[4] = { { 1.6f, 0.0f, -4.5f }, { 2.6f, 0.0f, -0.5f }, { 1.4f, 0.0f, 1.6f }, { 3.5f, 0.0f, 2.4f } };
    for (int i = 0; i < 4; i++) {
        br_node3d t = br_sprite3d_create(e, tree, 40.0f);
        br_canvas3d_desc *td = br_canvas3d_get(e, t);
        td->pivot = br_vec2_make(0.5f, 1.0f);        /* stands on its trunk */
        td->billboard = BR_CANVAS3D_BILLBOARD_Y_AXIS;
        td->lit = true;
        td->alpha_cutoff = i == 0 ? 0.5f : 0.0f;    /* the far one alpha-tested in the opaque pass */
        br_node3d_set_position(e, t, trees[i]);
        br_node3d_add_child(e, s->root, t);
    }
    add_mesh(e, s, br_geometry3d_cube(e, br_vec3_make(1.4f, 1.4f, 1.4f)), make_material(e, 0x9AD1FF, 0.35f, true),
             br_vec3_make(2.2f, 0.7f, 0.6f));

    /* lit and unlit canvases (same content), turned away from the sun: the lit one gets ambient only */
    br_node3d lit = make_panel(e, s, "lit", br_vec3_make(-2.2f, 1.2f, -3.0f), -35.0f * BR_DEG_TO_RAD);
    br_canvas3d_get(e, lit)->lit = true;
    make_panel(e, s, "unlit", br_vec3_make(0.6f, 1.2f, -3.2f), -35.0f * BR_DEG_TO_RAD);

    /* texture mode: the content has a 2D filter, which geometry mode cannot draw */
    br_node3d tex = make_panel(e, s, "filter", br_vec3_make(-5.4f, 1.3f, -2.2f), 55.0f * BR_DEG_TO_RAD);
    br_canvas3d_get(e, tex)->mode = BR_CANVAS3D_TEXTURE;
    br_node content = br_canvas3d_content(e, tex); /* the stripes graphics */
    br_filter f[2];
    f[0] = br_filter_color_matrix();
    br_color_matrix_sepia(f[0].matrix, false);
    f[1] = br_filter_blur(4.0f, 2, 5);
    br_filters_set(e, content, f, 2);

    make_button(e, s);
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
    build_scene(e, s);
    br_system_add(e, "demo.click", BR_PHASE_INPUT, -3000000, click_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - canvas3d demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
