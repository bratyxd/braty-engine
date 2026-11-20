/* test_graphics - triangulation counts/areas, holes, strokes, dashes, svg paths, shared contexts (headless). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>
#include <math.h>

static br_graphics_geometry_info geometry(br_graphics_context *ctx)
{
    br_graphics_geometry_info info;
    br_graphics_context_get_geometry(ctx, &info);
    return info;
}

static bool near(float a, float b, float eps)
{
    return br_nearly_equal(a, b, eps);
}

static void test_shapes(br_engine *e)
{
    br_color red = br_color_from_hex(0xff0000, 1.0f);
    br_graphics_context *ctx = br_graphics_context_create(e);

    br_graphics_rect(ctx, 10, 20, 100, 50);
    br_graphics_fill_color(ctx, red);
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(g.triangle_count == 2);
    BR_CHECK(g.vertex_count == 4);
    BR_CHECK(near(g.area, 5000.0f, 0.01f));
    BR_CHECK(near(g.bounds.x, 10.0f, 1e-4f) && near(g.bounds.width, 100.0f, 1e-4f));

    br_graphics_clear(ctx);
    br_graphics_circle(ctx, 0, 0, 50);
    br_graphics_fill_color(ctx, red);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == g.vertex_count - 2); /* fan */
    BR_CHECK(g.vertex_count >= 16);
    BR_CHECK(near(g.area, BR_PI * 2500.0f, BR_PI * 2500.0f * 0.01f));

    br_graphics_clear(ctx);
    br_graphics_round_rect(ctx, 0, 0, 100, 100, 20);
    br_graphics_fill_color(ctx, red);
    g = geometry(ctx);
    float expected = 100.0f * 100.0f - (4.0f - BR_PI) * 400.0f;
    BR_CHECK(near(g.area, expected, expected * 0.005f));

    /* a concave polygon (an L) goes through ear clipping: 6 points -> 4 triangles */
    br_graphics_clear(ctx);
    br_vec2 l_shape[6] = { { 0, 0 }, { 20, 0 }, { 20, 80 }, { 60, 80 }, { 60, 100 }, { 0, 100 } };
    br_graphics_poly(ctx, l_shape, 6, true);
    br_graphics_fill_color(ctx, red);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 4);
    BR_CHECK(near(g.area, 20.0f * 100.0f + 40.0f * 20.0f, 0.01f));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(10, 50)));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(40, 50)));

    /* star: 10 points, concave, 8 triangles; area = n * R * r * sin(pi / n) */
    br_graphics_clear(ctx);
    br_graphics_star(ctx, 0, 0, 5, 50, 20, 0);
    br_graphics_fill_color(ctx, red);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 8);
    BR_CHECK(near(g.area, 5.0f * 50.0f * 20.0f * SDL_sinf(BR_PI / 5.0f), 0.5f));

    /* chamfer: 100x100 minus four 10x10/2 corners */
    br_graphics_clear(ctx);
    br_graphics_chamfer_rect(ctx, 0, 0, 100, 100, 10);
    br_graphics_fill_color(ctx, red);
    BR_CHECK(near(geometry(ctx).area, 10000.0f - 200.0f, 0.05f));

    /* fillet < 0: four quarter circles scooped out */
    br_graphics_clear(ctx);
    br_graphics_fillet_rect(ctx, 0, 0, 100, 100, -20);
    br_graphics_fill_color(ctx, red);
    expected = 10000.0f - BR_PI * 400.0f;
    BR_CHECK(near(geometry(ctx).area, expected, expected * 0.005f));

    /* regular hexagon of radius 10: 3 * sqrt(3) / 2 * r^2 */
    br_graphics_clear(ctx);
    br_graphics_regular_poly(ctx, 0, 0, 10, 6, 0);
    br_graphics_fill_color(ctx, red);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 4);
    BR_CHECK(near(g.area, 1.5f * SDL_sqrtf(3.0f) * 100.0f, 0.01f));

    /* transform stack: a scaled rect, then restore */
    br_graphics_clear(ctx);
    br_graphics_save(ctx);
    br_graphics_scale(ctx, 2, 3);
    br_graphics_rect(ctx, 0, 0, 10, 10);
    br_graphics_restore(ctx);
    br_graphics_rect(ctx, 0, 0, 10, 10);
    br_graphics_fill_color(ctx, red);
    BR_CHECK(near(geometry(ctx).area, 600.0f + 100.0f, 0.01f));

    br_graphics_context_destroy(ctx);
}

static void test_holes(br_engine *e)
{
    br_color c = br_color_from_hex(0x00ff00, 1.0f);
    br_graphics_context *ctx = br_graphics_context_create(e);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    br_graphics_fill_color(ctx, c);
    br_graphics_rect(ctx, 25, 25, 50, 50);
    br_graphics_cut(ctx);
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(near(g.area, 7500.0f, 0.05f));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(50, 50)));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(10, 50)));

    /* two holes, one of them a circle, plus a hole outside the shape (ignored) */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 200, 100);
    br_graphics_fill_color(ctx, c);
    br_graphics_rect(ctx, 10, 10, 30, 30);
    br_graphics_circle(ctx, 140, 50, 30);
    br_graphics_rect(ctx, 300, 300, 10, 10);
    br_graphics_cut(ctx);
    g = geometry(ctx);
    float circle = 0.0f;
    {
        br_graphics_context *probe = br_graphics_context_create(e);
        br_graphics_circle(probe, 140, 50, 30);
        br_graphics_fill_color(probe, c);
        circle = geometry(probe).area;
        br_graphics_context_destroy(probe);
    }
    BR_CHECK(near(g.area, 20000.0f - 900.0f - circle, 0.5f));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(140, 50)));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(90, 50)));

    /* fill + stroke from one path: cut applies to both */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    br_graphics_fill_color(ctx, c);
    br_stroke_style st = br_graphics_stroke_style(2.0f, c);
    br_graphics_stroke(ctx, &st);
    br_graphics_rect(ctx, 40, 40, 20, 20);
    br_graphics_cut(ctx);
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(50, 50)));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(40, 50))); /* the hole's outline is stroked */
    br_graphics_context_destroy(ctx);
}

static void test_strokes(br_engine *e)
{
    br_color c = br_color_from_hex(0x0000ff, 1.0f);
    br_graphics_context *ctx = br_graphics_context_create(e);

    /* open line, butt caps: exactly length * width */
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, 100, 0);
    br_stroke_style st = br_graphics_stroke_style(10.0f, c);
    br_graphics_stroke(ctx, &st);
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(g.triangle_count == 2);
    BR_CHECK(near(g.area, 1000.0f, 0.01f));
    BR_CHECK(near(g.bounds.y, -5.0f, 1e-4f) && near(g.bounds.height, 10.0f, 1e-4f));

    /* square caps add width/2 at each end */
    br_graphics_clear(ctx);
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, 100, 0);
    st.cap = BR_GRAPHICS_CAP_SQUARE;
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 1100.0f, 0.01f));

    /* round caps add a full circle of radius width/2 */
    br_graphics_clear(ctx);
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, 100, 0);
    st.cap = BR_GRAPHICS_CAP_ROUND;
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 1000.0f + BR_PI * 25.0f, BR_PI * 25.0f * 0.08f)); /* polygonal circle */

    /* closed square, centred miter joins: outer 110^2 - inner 90^2 */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    st = br_graphics_stroke_style(10.0f, c);
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 110.0f * 110.0f - 90.0f * 90.0f, 0.5f));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(-4, -4)));   /* miter corner */
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(50, 50)));

    /* alignment 1 = outside: outer 120^2 - 100^2 */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    st.alignment = 1.0f;
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 120.0f * 120.0f - 10000.0f, 0.5f));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(5, 50)));

    /* bevel joins cut each outer corner by a 5x5 triangle (alignment 1, width 10) */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    st.join = BR_GRAPHICS_JOIN_BEVEL;
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 4000.0f + 4.0f * 50.0f, 0.5f));

    /* round joins: four quarter circles of radius 10 */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 100, 100);
    st.join = BR_GRAPHICS_JOIN_ROUND;
    br_graphics_stroke(ctx, &st);
    BR_CHECK(near(geometry(ctx).area, 4000.0f + BR_PI * 100.0f, BR_PI * 100.0f * 0.04f));

    /* dashes: 100 long, 10 on 10 off -> 5 dashes of 10x4 */
    br_graphics_clear(ctx);
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, 100, 0);
    st = br_graphics_stroke_style(4.0f, c);
    st.dash_count = 2;
    st.dash[0] = 10.0f;
    st.dash[1] = 10.0f;
    br_graphics_stroke(ctx, &st);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 10);
    BR_CHECK(near(g.area, 5.0f * 40.0f, 0.01f));
    BR_CHECK(br_graphics_context_contains(ctx, br_vec2_make(5, 0)));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(15, 0)));

    /* dash offset 5 shifts the pattern: first dash is 5 long, last one 5 long */
    br_graphics_clear(ctx);
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, 100, 0);
    st.dash_offset = 5.0f;
    br_graphics_stroke(ctx, &st);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 12);
    BR_CHECK(near(g.area, 200.0f, 0.01f));
    BR_CHECK(!br_graphics_context_contains(ctx, br_vec2_make(7, 0)));

    /* pixel lines: no triangles, one segment per edge */
    br_graphics_clear(ctx);
    br_graphics_rect(ctx, 0, 0, 10, 10);
    st = br_graphics_stroke_style(1.0f, c);
    st.pixel_line = true;
    br_graphics_stroke(ctx, &st);
    g = geometry(ctx);
    BR_CHECK(g.triangle_count == 0 && g.line_count == 4);

    br_graphics_context_destroy(ctx);
}

static void test_svg(br_engine *e)
{
    br_color c = br_color_from_hex(0xffffff, 1.0f);
    br_graphics_context *ctx = br_graphics_context_create(e);

    BR_CHECK(br_graphics_svg_path(ctx, "M10 10 H 90 V 90 H 10 Z"));
    br_graphics_fill_color(ctx, c);
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(g.triangle_count == 2);
    BR_CHECK(near(g.area, 6400.0f, 0.01f));

    /* relative commands with implicit repeats and no separators before minus signs */
    br_graphics_clear(ctx);
    BR_CHECK(br_graphics_svg_path(ctx, "m10,10l80 0 0 80-80 0z"));
    br_graphics_fill_color(ctx, c);
    BR_CHECK(near(geometry(ctx).area, 6400.0f, 0.01f));

    /* two arcs make a circle (compact flags "01") */
    br_graphics_clear(ctx);
    BR_CHECK(br_graphics_svg_path(ctx, "M0 50 A50 50 0 0 1 100 50 a50,50 0 01-100 0 Z"));
    br_graphics_fill_color(ctx, c);
    g = geometry(ctx);
    BR_CHECK(near(g.area, BR_PI * 2500.0f, BR_PI * 2500.0f * 0.01f));
    BR_CHECK(near(g.bounds.y, 0.0f, 0.1f) && near(g.bounds.height, 100.0f, 0.1f));

    /* cubic + smooth cubic + quadratic + smooth quadratic end where they say */
    br_graphics_clear(ctx);
    BR_CHECK(br_graphics_svg_path(ctx, "M0 0 C 10 -10 20 -10 30 0 S 50 10 60 0 Q 70 -10 80 0 T 100 0 L 100 50 L 0 50 Z"));
    br_graphics_fill_color(ctx, c);
    g = geometry(ctx);
    BR_CHECK(near(g.bounds.x, 0.0f, 1e-3f) && near(g.bounds.width, 100.0f, 1e-3f));
    BR_CHECK(g.area > 4500.0f && g.area < 5500.0f);

    /* errors are reported, not crashed on */
    br_graphics_clear(ctx);
    BR_CHECK(!br_graphics_svg_path(ctx, "M 10 10 X 5"));
    BR_CHECK(!br_graphics_svg_path(ctx, "M 10"));
    BR_CHECK(!br_graphics_svg_path(ctx, "10 10"));
    BR_CHECK(br_graphics_svg_path(ctx, ""));
    br_graphics_context_destroy(ctx);
}

static void test_styles(br_engine *e)
{
    br_graphics_context *ctx = br_graphics_context_create(e);
    br_fill_style lin = br_graphics_style_linear(BR_GRAPHICS_SPACE_LOCAL, 0, 0, 1, 0);
    br_graphics_style_add_stop(&lin, 0.0f, br_color_from_hex(0xff0000, 1.0f));
    br_graphics_style_add_stop(&lin, 1.0f, br_color_from_hex(0x0000ff, 1.0f));
    uint32_t textures = br_texture_count(e);
    br_graphics_rect(ctx, 0, 0, 10, 10);
    br_graphics_fill(ctx, &lin);
    br_graphics_rect(ctx, 20, 0, 10, 10);
    br_graphics_fill(ctx, &lin); /* same gradient: baked once */
    BR_CHECK(br_texture_count(e) == textures + 1);
    br_graphics_clear(ctx);
    BR_CHECK(br_texture_count(e) == textures);
    br_graphics_context_destroy(ctx);
}

static void test_nodes(br_engine *e)
{
    br_graphics_context *shared = br_graphics_context_create(e);
    br_graphics_rect(shared, 0, 0, 40, 20);
    br_graphics_fill_color(shared, br_color_from_hex(0xffffff, 1.0f));
    br_node a = br_graphics_create(e, shared);
    br_node b = br_graphics_create(e, shared);
    br_node own = br_graphics_create(e, NULL);
    BR_CHECK(br_graphics_context_of(e, a) == shared);
    BR_CHECK(br_graphics_context_of(e, b) == shared);
    BR_CHECK(br_graphics_context_of(e, own) != NULL && br_graphics_context_of(e, own) != shared);
    br_graphics_context_destroy(shared); /* nodes keep it alive */
    br_node_add_child(e, br_stage_root(e), a);
    br_node_set_position(e, a, br_vec2_make(100, 100));
    br_rect r = br_node_local_bounds(e, a);
    BR_CHECK(near(r.width, 40.0f, 1e-4f) && near(r.height, 20.0f, 1e-4f));
    BR_CHECK(br_node_type_hit_test(e, a, br_vec2_make(10, 10)));
    BR_CHECK(!br_node_type_hit_test(e, a, br_vec2_make(50, 10)));
    br_node_destroy(e, a);
    BR_CHECK(br_graphics_context_of(e, b) == shared);
    BR_CHECK(near(geometry(br_graphics_context_of(e, b)).area, 800.0f, 0.01f));
    br_node_destroy(e, b);
    br_node_destroy(e, own);
}

/* Bad numbers (from data files) must not hang, blow up memory or poison the bounds. */
static void test_bad_input(br_engine *e)
{
    br_color white = br_color_from_hex(0xffffff, 1.0f);
    br_graphics_context *ctx = br_graphics_context_create(e);
    uint64_t t0 = SDL_GetTicks();

    /* a dashed line far longer than the dash: the piece count is capped */
    br_stroke_style st = br_graphics_stroke_style(2.0f, white);
    st.dash_count = 2;
    st.dash[0] = 1.0f;
    st.dash[1] = 1.0f;
    br_graphics_move_to(ctx, -1e7f, 0);
    br_graphics_line_to(ctx, 1e7f, 0); /* 2e7 long: a float accumulator never got past 2^24 */
    br_graphics_stroke(ctx, &st);
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(g.triangle_count <= 2 * 4 * 100000); /* <= BR_GRAPHICS_MAX_DASH_PIECES quads (+ caps) */

    /* NaN / inf points are dropped: the bounds stay finite and cover the good points */
    br_graphics_clear(ctx);
    br_graphics_move_to(ctx, 0, 0);
    br_graphics_line_to(ctx, NAN, NAN);
    br_graphics_line_to(ctx, INFINITY, 0);
    br_graphics_line_to(ctx, 16, 5);
    br_graphics_line_to(ctx, 0, 5);
    br_graphics_fill_color(ctx, white);
    g = geometry(ctx);
    BR_CHECK(near(g.bounds.x, 0.0f, 1e-4f) && near(g.bounds.width, 16.0f, 1e-4f) && near(g.bounds.height, 5.0f, 1e-4f));

    /* NaN radius / angles / control points: a few vertices at most, no hang */
    br_graphics_clear(ctx);
    br_graphics_arc(ctx, 0, 0, NAN, 0, NAN, false);
    br_graphics_bezier_curve_to(ctx, NAN, 0, 0, NAN, 10, 10);
    br_graphics_quadratic_curve_to(ctx, INFINITY, 0, 10, 10);
    br_graphics_circle(ctx, 0, 0, NAN);
    br_graphics_ellipse(ctx, 0, 0, INFINITY, 1);
    br_stroke_style thin = br_graphics_stroke_style(1.0f, white);
    br_graphics_stroke(ctx, &thin);
    g = geometry(ctx);
    BR_CHECK(g.vertex_count < 2000);

    /* huge side / point counts are capped */
    br_graphics_clear(ctx);
    br_graphics_regular_poly(ctx, 0, 0, 10, 1000000000u, 0);
    br_graphics_round_poly(ctx, 0, 0, 10, 1000000000u, 2, 0);
    br_graphics_star(ctx, 0, 0, 3000000000u, 10, 5, 0);
    br_graphics_fill_color(ctx, white);
    g = geometry(ctx);
    BR_CHECK(g.vertex_count <= 3 * 4096 * 8);

    /* a concave fill with too many points is skipped (ear clipping is O(n^2)) */
    br_graphics_clear(ctx);
    br_graphics_star(ctx, 0, 0, 4096, 100, 50, 0); /* 8192 points: the limit, still filled */
    br_graphics_fill_color(ctx, white);
    BR_CHECK(geometry(ctx).triangle_count > 0);
    br_vec2 *zigzag = br_alloc(sizeof(br_vec2) * 20000);
    for (uint32_t i = 0; i < 10000; i++) {
        zigzag[i] = br_vec2_make((float)i, (i % 2) ? 10.0f : 0.0f);
        zigzag[19999 - i] = br_vec2_make((float)i, (i % 2) ? 30.0f : 20.0f);
    }
    br_graphics_clear(ctx);
    br_graphics_poly(ctx, zigzag, 20000, true);
    br_graphics_fill_color(ctx, white);
    BR_CHECK(geometry(ctx).triangle_count == 0);
    br_free(zigzag);

    BR_CHECK(SDL_GetTicks() - t0 < 5000);
    br_graphics_context_destroy(ctx);
}

/* line_to / arc after close_path continue from the closed sub-path's start (canvas, SVG). */
static void test_close_path(br_engine *e)
{
    br_color white = br_color_from_hex(0xffffff, 1.0f);
    br_stroke_style st = br_graphics_stroke_style(2.0f, white);
    br_graphics_context *a = br_graphics_context_create(e);
    br_graphics_move_to(a, 0, 0);
    br_graphics_line_to(a, 100, 0);
    br_graphics_line_to(a, 100, 100);
    br_graphics_close_path(a);
    br_graphics_line_to(a, 0, 100);
    br_graphics_stroke(a, &st);
    br_graphics_context *b = br_graphics_context_create(e);
    br_graphics_move_to(b, 0, 0);
    br_graphics_line_to(b, 100, 0);
    br_graphics_line_to(b, 100, 100);
    br_graphics_close_path(b);
    br_graphics_move_to(b, 0, 0);
    br_graphics_line_to(b, 0, 100);
    br_graphics_stroke(b, &st);
    BR_CHECK(near(geometry(a).area, geometry(b).area, 0.5f));

    /* the same in SVG: "Z V50" draws from the start (0,0) down to (0,50) */
    br_graphics_clear(a);
    BR_CHECK(br_graphics_svg_path(a, "M0 0 H100 V100 Z V50"));
    br_graphics_stroke(a, &st);
    br_graphics_clear(b);
    BR_CHECK(br_graphics_svg_path(b, "M0 0 H100 V100 Z M0 0 V50"));
    br_graphics_stroke(b, &st);
    BR_CHECK(near(geometry(a).area, geometry(b).area, 0.5f));
    br_graphics_context_destroy(a);
    br_graphics_context_destroy(b);
}

/* SVG numbers: the SVG grammar only (no nan / inf / hex), whatever the C locale. */
static void test_svg_numbers(br_engine *e)
{
    br_graphics_context *ctx = br_graphics_context_create(e);
    BR_CHECK(!br_graphics_svg_path(ctx, "M0 0 L nan 0"));
    BR_CHECK(!br_graphics_svg_path(ctx, "M0 0 L inf 0"));
    BR_CHECK(!br_graphics_svg_path(ctx, "M0 0 L infinity 0"));
    BR_CHECK(!br_graphics_svg_path(ctx, "M0 0 L 0x10 0"));
    BR_CHECK(!br_graphics_svg_path(ctx, "M0 0 L 1e999 0"));
    br_graphics_clear(ctx);
    BR_CHECK(br_graphics_svg_path(ctx, "M-1.5e1,.5L+2.25-3E0l.5.5 1e-1 2Z"));
    br_graphics_fill_color(ctx, br_color_from_hex(0xffffff, 1.0f));
    br_graphics_geometry_info g = geometry(ctx);
    BR_CHECK(near(g.bounds.x, -15.0f, 1e-4f) && near(g.bounds.y, -3.0f, 1e-4f));
    BR_CHECK(near(g.bounds.width, 17.85f, 1e-3f) && near(g.bounds.height, 3.5f, 1e-3f));
    br_graphics_context_destroy(ctx);
}

int main(void)
{
    br_engine *e = br_engine_create("test_graphics", NULL);
    test_bad_input(e);
    test_close_path(e);
    test_svg_numbers(e);
    test_shapes(e);
    test_holes(e);
    test_strokes(e);
    test_svg(e);
    test_styles(e);
    test_nodes(e);
    br_engine_destroy(e);
    return br_test_finish();
}
