/* graphics_demo - every br_graphics feature in an 8 x 5 grid (no animation, so screenshots compare).
 *
 *   graphics_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Row 1, shapes:  rect, round_rect, circle, ellipse, concave poly, regular_poly, round_poly, star.
 * Row 2, shapes:  chamfer_rect, fillet_rect (+), fillet_rect (-), arc (pie), arc_to, bezier
 *                 (with its control points), quadratic, svg path (heart).
 * Row 3, fills:   linear local, linear global over two shapes, radial, radial with inner radius,
 *                 texture pattern, pattern with a rotate/scale matrix, holes (cut), ring (cut + stroke).
 * Row 4, strokes: caps butt/round/square (pixel guides at the path ends), joins miter, bevel,
 *                 round, alignment inner/centre/outer (pixel guide = the path), dashes, dashed
 *                 circle + dashed svg curve, pixel lines (grid, a fill over it, a star over both).
 * Row 5:          one context shared by two nodes (cells 1 and 2: second node tinted, rotated,
 *                 scaled), transform stack, arc_to_svg flag combinations, miter limit 10 vs 2,
 *                 translucent and gradient strokes, texture(), a node scaled 3x whose pixel
 *                 line stays 1 pixel wide.
 */
#include "braty/braty_main.h"

enum { CELL_W = 160, CELL_H = 144, PAD = 12, INNER_W = CELL_W - 2 * PAD, INNER_H = CELL_H - 2 * PAD };

static br_color hex(uint32_t rgb)
{
    return br_color_from_hex(rgb, 1.0f);
}

/* A 16x16 two-colour checker (pattern fills and texture()). */
static br_texture make_checker(br_engine *e, uint32_t a, uint32_t b)
{
    uint8_t pixels[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t c = ((x / 8) + (y / 8)) % 2 == 0 ? a : b;
            uint8_t *p = &pixels[(y * 16 + x) * 4];
            p[0] = (uint8_t)(c >> 16);
            p[1] = (uint8_t)(c >> 8);
            p[2] = (uint8_t)c;
            p[3] = 255;
        }
    }
    br_texture_style style;
    SDL_zero(style);
    style.filter = BR_FILTER_NEAREST;
    return br_texture_create_rgba8(e, 16, 16, pixels, &style);
}

/* A graphics node over grid cell (col, row); returns its own context. */
static br_graphics_context *cell(br_engine *e, int col, int row)
{
    br_node n = br_graphics_create(e, NULL);
    br_node_set_position(e, n, br_vec2_make((float)(col * CELL_W + PAD), (float)(row * CELL_H + PAD)));
    br_node_add_child(e, br_stage_root(e), n);
    return br_graphics_context_of(e, n);
}

/* Cell backgrounds and borders, behind everything. */
static void draw_grid(br_engine *e)
{
    br_node n = br_graphics_create(e, NULL);
    br_node_add_child(e, br_stage_root(e), n);
    br_graphics_context *g = br_graphics_context_of(e, n);
    br_stroke_style border = br_graphics_stroke_style(1.0f, hex(0x2c3550));
    border.pixel_line = true;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 8; col++) {
            br_graphics_rect(g, (float)(col * CELL_W + 4), (float)(row * CELL_H + 4), CELL_W - 8, CELL_H - 8);
        }
    }
    br_graphics_fill_color(g, hex(0x182036));
    br_graphics_stroke(g, &border);
}

static void row_shapes(br_engine *e)
{
    br_color c = hex(0x4cc9f0);
    br_graphics_context *g;

    g = cell(e, 0, 0);
    br_graphics_rect(g, 8, 8, 120, 104);
    br_graphics_fill_color(g, c);

    g = cell(e, 1, 0);
    br_graphics_round_rect(g, 8, 8, 120, 104, 24);
    br_graphics_fill_color(g, c);

    g = cell(e, 2, 0);
    br_graphics_circle(g, 68, 60, 54);
    br_graphics_fill_color(g, c);

    g = cell(e, 3, 0);
    br_graphics_ellipse(g, 68, 60, 64, 38);
    br_graphics_fill_color(g, c);

    g = cell(e, 4, 0);
    br_vec2 arrow[7] = { { 8, 45 }, { 78, 45 }, { 78, 10 }, { 130, 60 }, { 78, 110 }, { 78, 75 }, { 8, 75 } };
    br_graphics_poly(g, arrow, 7, true);
    br_graphics_fill_color(g, c);

    g = cell(e, 5, 0);
    br_graphics_regular_poly(g, 68, 62, 56, 6, 0);
    br_graphics_fill_color(g, c);

    g = cell(e, 6, 0);
    br_graphics_round_poly(g, 68, 64, 58, 5, 16, 0);
    br_graphics_fill_color(g, c);

    g = cell(e, 7, 0);
    br_graphics_star(g, 68, 64, 5, 60, 24, 0);
    br_graphics_fill_color(g, c);
}

static void row_more_shapes(br_engine *e)
{
    br_color c = hex(0xf72585);
    br_graphics_context *g;

    g = cell(e, 0, 1);
    br_graphics_chamfer_rect(g, 8, 8, 120, 104, 26);
    br_graphics_fill_color(g, c);

    g = cell(e, 1, 1);
    br_graphics_fillet_rect(g, 8, 8, 120, 104, 34);
    br_graphics_fill_color(g, c);

    g = cell(e, 2, 1);
    br_graphics_fillet_rect(g, 8, 8, 120, 104, -34);
    br_graphics_fill_color(g, c);

    g = cell(e, 3, 1); /* pie: centre, arc, close */
    br_graphics_move_to(g, 68, 60);
    br_graphics_arc(g, 68, 60, 56, 0.5f, BR_TAU - 0.5f, false);
    br_graphics_close_path(g);
    br_graphics_fill_color(g, c);

    g = cell(e, 4, 1); /* two arc_to corners of different radius */
    br_graphics_move_to(g, 10, 112);
    br_graphics_arc_to(g, 10, 8, 126, 8, 50);
    br_graphics_arc_to(g, 126, 8, 126, 112, 16);
    br_graphics_line_to(g, 126, 112);
    br_graphics_close_path(g);
    br_graphics_fill_color(g, c);

    g = cell(e, 5, 1); /* cubic with its control polygon */
    br_stroke_style thin = br_graphics_stroke_style(1.0f, hex(0x8890b0));
    thin.pixel_line = true;
    br_graphics_move_to(g, 8, 104);
    br_graphics_line_to(g, 30, 0);
    br_graphics_move_to(g, 106, 120);
    br_graphics_line_to(g, 128, 16);
    br_graphics_stroke(g, &thin);
    br_graphics_move_to(g, 8, 104);
    br_graphics_bezier_curve_to(g, 30, 0, 106, 120, 128, 16);
    br_stroke_style curve = br_graphics_stroke_style(6.0f, c);
    curve.cap = BR_GRAPHICS_CAP_ROUND;
    br_graphics_stroke(g, &curve);
    br_graphics_circle(g, 30, 0, 4);
    br_graphics_circle(g, 106, 120, 4);
    br_graphics_fill_color(g, hex(0xffffff));

    g = cell(e, 6, 1);
    br_graphics_move_to(g, 8, 112);
    br_graphics_quadratic_curve_to(g, 68, -60, 128, 112);
    br_graphics_close_path(g);
    br_graphics_fill_color(g, c);

    g = cell(e, 7, 1);
    br_graphics_svg_path(g, "M68 110 C 10 72 0 32 30 18 C 50 9 64 22 68 34 C 72 22 86 9 106 18 C 136 32 126 72 68 110 Z");
    br_graphics_fill_color(g, c);
    br_stroke_style outline = br_graphics_stroke_style(4.0f, hex(0xffffff));
    outline.join = BR_GRAPHICS_JOIN_ROUND;
    br_graphics_stroke(g, &outline);
}

static void row_fills(br_engine *e, br_texture checker)
{
    br_graphics_context *g;

    g = cell(e, 0, 2); /* local: 0..1 across the shape */
    br_fill_style lin = br_graphics_style_linear(BR_GRAPHICS_SPACE_LOCAL, 0, 0, 1, 0);
    br_graphics_style_add_stop(&lin, 0.0f, hex(0xff006e));
    br_graphics_style_add_stop(&lin, 0.5f, hex(0xffbe0b));
    br_graphics_style_add_stop(&lin, 1.0f, hex(0x3a86ff));
    br_graphics_round_rect(g, 8, 8, 120, 104, 12);
    br_graphics_fill(g, &lin);

    g = cell(e, 1, 2); /* global: one gradient continues across two shapes */
    br_fill_style glob = br_graphics_style_linear(BR_GRAPHICS_SPACE_GLOBAL, 0, 0, INNER_W, INNER_H);
    br_graphics_style_add_stop(&glob, 0.0f, hex(0x06d6a0));
    br_graphics_style_add_stop(&glob, 1.0f, hex(0x7209b7));
    br_graphics_rect(g, 4, 4, 60, 112);
    br_graphics_circle(g, 100, 60, 34);
    br_graphics_fill(g, &glob);

    g = cell(e, 2, 2);
    br_fill_style rad = br_graphics_style_radial(BR_GRAPHICS_SPACE_LOCAL, 0.5f, 0.5f, 0.0f, 0.5f);
    br_graphics_style_add_stop(&rad, 0.0f, hex(0xffffff));
    br_graphics_style_add_stop(&rad, 0.4f, hex(0xffbe0b));
    br_graphics_style_add_stop(&rad, 1.0f, br_color_from_hex(0xfb5607, 0.0f));
    br_graphics_circle(g, 68, 60, 56);
    br_graphics_fill(g, &rad);

    g = cell(e, 3, 2); /* rings start at inner_radius */
    br_fill_style rings = br_graphics_style_radial(BR_GRAPHICS_SPACE_GLOBAL, 68, 60, 25, 60);
    br_graphics_style_add_stop(&rings, 0.0f, hex(0x3a86ff));
    br_graphics_style_add_stop(&rings, 0.5f, hex(0xffffff));
    br_graphics_style_add_stop(&rings, 1.0f, hex(0x3a86ff));
    br_graphics_rect(g, 8, 8, 120, 104);
    br_graphics_fill(g, &rings);

    g = cell(e, 4, 2);
    br_fill_style pat = br_graphics_style_pattern(checker, br_mat3_identity());
    br_graphics_round_rect(g, 8, 8, 120, 104, 20);
    br_graphics_fill(g, &pat);

    g = cell(e, 5, 2); /* pattern matrix: scaled 1.5x and rotated 30 degrees, tinted */
    br_mat3 m = br_mat3_mul(br_mat3_rotation(30.0f * BR_DEG_TO_RAD), br_mat3_scaling(1.5f, 1.5f));
    br_fill_style pat2 = br_graphics_style_pattern(checker, m);
    pat2.color = hex(0xffd6a5);
    br_graphics_star(g, 68, 64, 6, 60, 34, 0);
    br_graphics_fill(g, &pat2);

    g = cell(e, 6, 2); /* holes */
    br_graphics_rect(g, 8, 8, 120, 104);
    br_graphics_fill_color(g, hex(0x80ed99));
    br_graphics_circle(g, 46, 60, 24);
    br_graphics_rect(g, 84, 24, 30, 30);
    br_graphics_star(g, 100, 88, 5, 16, 7, 0);
    br_graphics_cut(g);

    g = cell(e, 7, 2); /* ring: fill + stroke, both cut */
    br_graphics_circle(g, 68, 60, 54);
    br_graphics_fill_color(g, hex(0x80ed99));
    br_stroke_style ring_line = br_graphics_stroke_style(4.0f, hex(0xffffff));
    br_graphics_stroke(g, &ring_line);
    br_graphics_circle(g, 68, 60, 28);
    br_graphics_cut(g);
}

/* A zig-zag stroked with one join style. */
static void join_cell(br_engine *e, int col, br_graphics_join join)
{
    br_graphics_context *g = cell(e, col, 3);
    br_vec2 zig[5] = { { 14, 100 }, { 40, 20 }, { 68, 100 }, { 96, 20 }, { 122, 100 } };
    br_graphics_poly(g, zig, 5, false);
    br_stroke_style st = br_graphics_stroke_style(14.0f, hex(0xffbe0b));
    st.join = join;
    br_graphics_stroke(g, &st);
    br_stroke_style guide = br_graphics_stroke_style(1.0f, hex(0x182036));
    guide.pixel_line = true;
    br_graphics_stroke(g, &guide);
}

static void row_strokes(br_engine *e)
{
    br_graphics_context *g;
    br_stroke_style guide = br_graphics_stroke_style(1.0f, hex(0xffffff));
    guide.pixel_line = true;

    g = cell(e, 0, 3); /* caps: butt, round, square; guides mark the path ends */
    br_graphics_cap caps[3] = { BR_GRAPHICS_CAP_BUTT, BR_GRAPHICS_CAP_ROUND, BR_GRAPHICS_CAP_SQUARE };
    for (int i = 0; i < 3; i++) {
        br_graphics_move_to(g, 30, 24 + 36 * (float)i);
        br_graphics_line_to(g, 106, 24 + 36 * (float)i);
        br_stroke_style st = br_graphics_stroke_style(18.0f, hex(0xffbe0b));
        st.cap = caps[i];
        br_graphics_stroke(g, &st);
    }
    br_graphics_move_to(g, 30, 4);
    br_graphics_line_to(g, 30, 116);
    br_graphics_move_to(g, 106, 4);
    br_graphics_line_to(g, 106, 116);
    br_graphics_stroke(g, &guide);

    join_cell(e, 1, BR_GRAPHICS_JOIN_MITER);
    join_cell(e, 2, BR_GRAPHICS_JOIN_BEVEL);
    join_cell(e, 3, BR_GRAPHICS_JOIN_ROUND);

    g = cell(e, 4, 3); /* alignment 0 (inside), 0.5, 1 (outside); the guide is the path */
    for (int i = 0; i < 3; i++) {
        float x = 12 + 42 * (float)i;
        br_graphics_rect(g, x, 46, 28, 28);
        br_stroke_style st = br_graphics_stroke_style(8.0f, hex(0xffbe0b));
        st.alignment = 0.5f * (float)i;
        br_graphics_stroke(g, &st);
        br_graphics_rect(g, x, 46, 28, 28);
        br_graphics_stroke(g, &guide);
    }

    g = cell(e, 5, 3); /* dash patterns */
    float patterns[4][4] = { { 12, 6, 0, 0 }, { 0.1f, 8, 0, 0 }, { 20, 5, 5, 5 }, { 8, 0, 0, 0 } };
    uint32_t counts[4] = { 2, 2, 4, 1 };
    for (int i = 0; i < 4; i++) {
        br_graphics_move_to(g, 8, 14 + 22 * (float)i);
        br_graphics_line_to(g, 128, 14 + 22 * (float)i);
        br_stroke_style st = br_graphics_stroke_style(6.0f, hex(0xffbe0b));
        st.dash_count = counts[i];
        for (uint32_t k = 0; k < counts[i]; k++) {
            st.dash[k] = patterns[i][k];
        }
        if (i == 1) {
            st.cap = BR_GRAPHICS_CAP_ROUND; /* dots */
        }
        br_graphics_stroke(g, &st);
    }
    br_graphics_rect(g, 8, 98, 120, 16);
    br_stroke_style box = br_graphics_stroke_style(2.0f, hex(0xffffff));
    box.dash_count = 2;
    box.dash[0] = 6;
    box.dash[1] = 4;
    box.dash_offset = 3;
    br_graphics_stroke(g, &box);

    g = cell(e, 6, 3); /* dashed circle with round caps, dashed svg curve */
    br_graphics_circle(g, 68, 44, 36);
    br_stroke_style dc = br_graphics_stroke_style(6.0f, hex(0xffbe0b));
    dc.dash_count = 2;
    dc.dash[0] = 10;
    dc.dash[1] = 8;
    dc.cap = BR_GRAPHICS_CAP_ROUND;
    br_graphics_stroke(g, &dc);
    br_graphics_svg_path(g, "M8 112 q 30 -30 60 0 t 60 0");
    br_stroke_style ds = br_graphics_stroke_style(3.0f, hex(0xffffff));
    ds.dash_count = 2;
    ds.dash[0] = 6;
    ds.dash[1] = 3;
    br_graphics_stroke(g, &ds);

    g = cell(e, 7, 3); /* pixel lines: a grid and a star */
    for (int i = 0; i <= 8; i++) {
        br_graphics_move_to(g, 4 + 16 * (float)i, 4);
        br_graphics_line_to(g, 4 + 16 * (float)i, 116);
    }
    for (int i = 0; i <= 7; i++) {
        br_graphics_move_to(g, 4, 4 + 16 * (float)i);
        br_graphics_line_to(g, 132, 4 + 16 * (float)i);
    }
    br_stroke_style grid = guide;
    grid.fill.color = hex(0x4a5578);
    br_graphics_stroke(g, &grid);
    br_graphics_rect(g, 36, 36, 64, 48); /* covers the grid, under the star: instruction order */
    br_graphics_fill_color(g, hex(0x3a86ff));
    br_graphics_star(g, 68, 62, 5, 52, 22, 0);
    br_graphics_stroke(g, &guide);
}

static void row_misc(br_engine *e, br_texture checker)
{
    br_graphics_context *g;

    /* one context, two nodes: the second is tinted, rotated and scaled */
    br_graphics_context *shared = br_graphics_context_create(e);
    br_graphics_star(shared, 0, 0, 5, 50, 20, 0);
    br_graphics_fill_color(shared, hex(0xffffff));
    br_graphics_circle(shared, 0, 0, 12);
    br_graphics_fill_color(shared, hex(0xf72585));
    br_node a = br_graphics_create(e, shared);
    br_node b = br_graphics_create(e, shared);
    br_graphics_context_destroy(shared); /* the nodes keep it alive */
    br_node_set_position(e, a, br_vec2_make(0 * CELL_W + CELL_W / 2, 4 * CELL_H + CELL_H / 2 + 4));
    br_node_set_position(e, b, br_vec2_make(1 * CELL_W + CELL_W / 2, 4 * CELL_H + CELL_H / 2 + 4));
    br_node_set_rotation(e, b, 0.4f);
    br_node_set_scale(e, b, br_vec2_make(0.75f, 1.1f));
    br_node_set_tint(e, b, hex(0x4cc9f0));
    br_node_add_child(e, br_stage_root(e), a);
    br_node_add_child(e, br_stage_root(e), b);

    g = cell(e, 2, 4); /* transform stack: a fan of rotated rects */
    for (int i = 0; i < 8; i++) {
        br_graphics_save(g);
        br_graphics_translate(g, 68, 60);
        br_graphics_rotate(g, BR_TAU * (float)i / 8.0f);
        br_graphics_rect(g, 14, -5, 40, 10);
        br_graphics_restore(g);
    }
    br_graphics_fill_color(g, hex(0x80ed99));
    br_graphics_save(g);
    br_graphics_translate(g, 68, 60);
    br_graphics_scale(g, 1.0f, 0.5f);
    br_graphics_circle(g, 0, 0, 10);
    br_graphics_restore(g);
    br_graphics_fill_color(g, hex(0xffffff));

    g = cell(e, 3, 4); /* arc_to_svg: the four large/sweep combinations between the same two points */
    br_color arc_colors[4] = { hex(0xff006e), hex(0xffbe0b), hex(0x3a86ff), hex(0x80ed99) };
    for (int i = 0; i < 4; i++) {
        br_graphics_move_to(g, 50, 50);
        br_graphics_arc_to_svg(g, 30, 22, 0.0f, i / 2 == 1, i % 2 == 1, 86, 70);
        br_stroke_style st = br_graphics_stroke_style(3.0f, arc_colors[i]);
        br_graphics_stroke(g, &st);
    }

    g = cell(e, 4, 4); /* the same sharp V: miter limit 10 keeps the point, 2 bevels it */
    for (int i = 0; i < 2; i++) {
        float x = 18 + 60 * (float)i;
        br_vec2 v[3] = { { x, 100 }, { x + 20, 30 }, { x + 40, 100 } };
        br_graphics_poly(g, v, 3, false);
        br_stroke_style st = br_graphics_stroke_style(10.0f, hex(0xffbe0b));
        st.miter_limit = i == 0 ? 10.0f : 2.0f;
        br_graphics_stroke(g, &st);
    }

    g = cell(e, 5, 4); /* translucent stroke over a fill (corners must not be darker), gradient stroke */
    br_graphics_rect(g, 14, 12, 60, 50);
    br_graphics_fill_color(g, hex(0x3a86ff));
    br_stroke_style half = br_graphics_stroke_style(14.0f, br_color_from_hex(0xffffff, 0.5f));
    br_graphics_stroke(g, &half);
    br_graphics_round_rect(g, 50, 70, 76, 40, 12);
    br_stroke_style grad = br_graphics_stroke_style(8.0f, hex(0xffffff));
    grad.fill = br_graphics_style_linear(BR_GRAPHICS_SPACE_LOCAL, 0, 0, 1, 0);
    br_graphics_style_add_stop(&grad.fill, 0.0f, hex(0xff006e));
    br_graphics_style_add_stop(&grad.fill, 1.0f, hex(0x06d6a0));
    br_graphics_stroke(g, &grad);

    g = cell(e, 6, 4); /* texture(): natural size, stretched, tinted */
    br_graphics_texture(g, checker, hex(0xffffff), 8, 8, 0, 0);
    br_graphics_texture(g, checker, hex(0xffffff), 32, 8, 96, 48);
    br_graphics_texture(g, checker, hex(0xff8fab), 8, 64, 120, 48);

    /* node scaled 3x: the normal stroke scales, the pixel line stays 1 pixel */
    br_node big = br_graphics_create(e, NULL);
    g = br_graphics_context_of(e, big);
    br_graphics_rect(g, 0, 0, 36, 30);
    br_stroke_style wide = br_graphics_stroke_style(2.0f, hex(0xffbe0b));
    br_graphics_stroke(g, &wide);
    br_graphics_move_to(g, 4, 26);
    br_graphics_line_to(g, 32, 4);
    br_graphics_move_to(g, 4, 4);
    br_graphics_line_to(g, 32, 26);
    br_stroke_style px = br_graphics_stroke_style(1.0f, hex(0xffffff));
    px.pixel_line = true;
    br_graphics_stroke(g, &px);
    br_node_set_scale(e, big, br_vec2_make(3.0f, 3.0f));
    br_node_set_position(e, big, br_vec2_make(7 * CELL_W + PAD + 14, 4 * CELL_H + PAD + 15));
    br_node_add_child(e, br_stage_root(e), big);
}

static bool demo_init(br_engine *e)
{
    br_gpu_set_clear_color(br_engine_gpu(e), hex(0x0d1120));
    br_texture checker = make_checker(e, 0xeeeeee, 0x7b2cbf);
    draw_grid(e);
    row_shapes(e);
    row_more_shapes(e);
    row_fills(e, checker);
    row_strokes(e);
    row_misc(e, checker);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - graphics";
    desc.width = 8 * CELL_W;
    desc.height = 5 * CELL_H;
    desc.init = demo_init;
    return desc;
}
