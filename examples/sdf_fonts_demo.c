/* sdf_fonts_demo - distance-field bitmap fonts: SDF, MSDF and MTSDF pages.
 *
 *   sdf_fonts_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Three one-glyph fonts from examples/fonts/gen_circle_sdf.py draw the same circle; a thin red
 * ring marks where its edge must be. Top: on the 2D stage (text_sdf shader). Bottom: on a
 * canvas3d in a View3D (canvas3d_sdf shader). Every disc must fill its ring exactly: an MTSDF
 * page is premultiplied on upload, and a shader that forgets to undo that draws it smaller.
 */
#include "braty/braty_main.h"

static const char *k_fonts[3] = { "fonts/circle_sdf.fnt", "fonts/circle_msdf.fnt", "fonts/circle_mtsdf.fnt" };
static const char *k_labels[3] = { "sdf", "msdf", "mtsdf" };

/* The circle glyph ("O") at `size` centred on (x, y) with the reference ring around it. The
 * glyph is a full em box; the circle's radius is 24/64 of it (gen_circle_sdf.py). */
static void add_circle(br_engine *e, br_node parent, br_font *font, float size, float x, float y)
{
    br_text_style st = br_text_style_default();
    st.font = font;
    st.font_size = size;
    st.fill = br_color_from_hex(0xF1FAEE, 1.0f);
    br_node t = br_text_create(e, "O", &st);
    br_text_set_anchor(e, t, 0.5f, 0.5f);
    br_node_set_position(e, t, br_vec2_make(x, y));
    br_node_add_child(e, parent, t);

    br_graphics_context *ctx = br_graphics_context_create(e);
    br_graphics_circle(ctx, x, y, size * 24.0f / 64.0f);
    br_stroke_style ring = br_graphics_stroke_style(2.0f, br_color_from_hex(0xE63946, 1.0f));
    br_graphics_stroke(ctx, &ring);
    br_node_add_child(e, parent, br_graphics_create(e, ctx));
    br_graphics_context_destroy(ctx);
}

static bool demo_init(br_engine *e)
{
    br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x1D3557, 1.0f));
    br_font *fonts[3];
    for (int i = 0; i < 3; i++) {
        fonts[i] = br_bitmap_font_load(e, k_fonts[i]);
        if (fonts[i] == NULL) {
            return false;
        }
    }

    /* top: the 2D stage */
    br_text_style label = br_text_style_default();
    label.font_size = 24.0f;
    label.fill = br_color_from_hex(0xA8DADC, 1.0f);
    for (int i = 0; i < 3; i++) {
        float x = 320.0f + 320.0f * (float)i;
        add_circle(e, br_stage_root(e), fonts[i], 192.0f, x, 170.0f);
        br_node t = br_text_create(e, k_labels[i], &label);
        br_text_set_anchor(e, t, 0.5f, 0.0f);
        br_node_set_position(e, t, br_vec2_make(x, 290.0f));
        br_node_add_child(e, br_stage_root(e), t);
    }

    /* bottom: the same row on a canvas3d facing the camera */
    br_node view = br_view3d_create(e, 1280.0f, 360.0f);
    br_node_set_position(e, view, br_vec2_make(0.0f, 360.0f));
    br_view3d_set_background(e, view, br_color_to_linear(br_color_from_hex(0x1D3557, 1.0f)));
    br_view3d_post(e, view)->bloom = false; /* a glow would hide the ring */
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.0f, 3.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_canvas3d_desc d = br_canvas3d_default(960.0f, 240.0f);
    d.pixels_per_unit = 100.0f;
    br_node3d canvas = br_canvas3d_create(e, &d);
    br_node3d_add_child(e, br_view3d_root(e, view), canvas);
    for (int i = 0; i < 3; i++) {
        add_circle(e, br_canvas3d_root(e, canvas), fonts[i], 192.0f, 160.0f + 320.0f * (float)i, 120.0f);
    }
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - SDF fonts demo";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR; /* the source tree (CMake), any build dir works */
    desc.init = demo_init;
    return desc;
}
