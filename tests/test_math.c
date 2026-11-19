/* test_math: mat3 (Pixi semantics), mat4, quat, shapes, intersections, colour. */
#include "br_test.h"

#include "braty/br_math.h"

#include <math.h>
#include <string.h>

#define EPS 1e-4f

static int vec2_near(br_vec2 a, float x, float y)
{
    return br_nearly_equal(a.x, x, EPS) && br_nearly_equal(a.y, y, EPS);
}

static int vec3_near(br_vec3 a, float x, float y, float z)
{
    return br_nearly_equal(a.x, x, EPS) && br_nearly_equal(a.y, y, EPS) && br_nearly_equal(a.z, z, EPS);
}

static void test_vec(void)
{
    br_vec2 v = br_vec2_normalize(br_vec2_make(3.0f, 4.0f));
    BR_CHECK(vec2_near(v, 0.6f, 0.8f));
    BR_CHECK(br_nearly_equal(br_vec2_length(br_vec2_make(3.0f, 4.0f)), 5.0f, EPS));
    BR_CHECK(vec2_near(br_vec2_normalize(br_vec2_make(0.0f, 0.0f)), 0.0f, 0.0f));
    /* +90 degrees turns +x into +y (clockwise on a y-down screen) */
    BR_CHECK(vec2_near(br_vec2_rotate(br_vec2_make(1.0f, 0.0f), BR_PI / 2.0f), 0.0f, 1.0f));
    BR_CHECK(vec3_near(br_vec3_cross(br_vec3_make(1, 0, 0), br_vec3_make(0, 1, 0)), 0, 0, 1));
}

static void test_mat3(void)
{
    /* Pixi: rotation 90deg maps (1,0) to (0,1) */
    br_mat3 r = br_mat3_rotation(BR_PI / 2.0f);
    BR_CHECK(vec2_near(br_mat3_apply(r, br_vec2_make(1.0f, 0.0f)), 0.0f, 1.0f));

    /* translate/scale/rotate apply after the existing transform, like Pixi */
    br_mat3 m = br_mat3_identity();
    m = br_mat3_scale(m, 2.0f, 2.0f);
    m = br_mat3_translate(m, 10.0f, 0.0f);
    BR_CHECK(vec2_near(br_mat3_apply(m, br_vec2_make(1.0f, 1.0f)), 12.0f, 2.0f));

    /* invert round trip */
    br_mat3 t = br_mat3_from_transform(100.0f, 50.0f, 8.0f, 4.0f, 2.0f, 3.0f, 0.7f, 0.1f, -0.2f);
    bool ok = false;
    br_mat3 inv = br_mat3_invert(t, &ok);
    BR_CHECK(ok);
    BR_CHECK(br_mat3_nearly_equal(br_mat3_mul(t, inv), br_mat3_identity(), EPS));
    br_vec2 p = br_vec2_make(3.0f, -7.0f);
    BR_CHECK(vec2_near(br_mat3_apply_inverse(t, br_mat3_apply(t, p)), p.x, p.y));

    /* pivot lands on position */
    br_mat3 pivoted = br_mat3_from_transform(100.0f, 50.0f, 8.0f, 4.0f, 2.0f, 3.0f, 0.7f, 0.0f, 0.0f);
    BR_CHECK(vec2_near(br_mat3_apply(pivoted, br_vec2_make(8.0f, 4.0f)), 100.0f, 50.0f));

    /* from_transform == translate * rotate * scale when no skew/pivot */
    br_mat3 composed = br_mat3_mul(br_mat3_translation(5.0f, 6.0f),
                                   br_mat3_mul(br_mat3_rotation(0.3f), br_mat3_scaling(2.0f, 4.0f)));
    br_mat3 direct = br_mat3_from_transform(5.0f, 6.0f, 0.0f, 0.0f, 2.0f, 4.0f, 0.3f, 0.0f, 0.0f);
    BR_CHECK(br_mat3_nearly_equal(composed, direct, EPS));

    /* decompose recovers the parts */
    br_vec2 pos, scale, skew;
    float rot;
    br_mat3_decompose(direct, &pos, &scale, &rot, &skew);
    BR_CHECK(vec2_near(pos, 5.0f, 6.0f));
    BR_CHECK(vec2_near(scale, 2.0f, 4.0f));
    BR_CHECK(br_nearly_equal(rot, 0.3f, EPS));
    BR_CHECK(vec2_near(skew, 0.0f, 0.0f));

    br_mat3 singular = br_mat3_scaling(0.0f, 1.0f);
    br_mat3_invert(singular, &ok);
    BR_CHECK(!ok);
}

static void test_mat4_quat(void)
{
    br_mat4 m = br_mat4_from_trs(br_vec3_make(1, 2, 3), br_quat_from_axis_angle(br_vec3_make(0, 1, 0), 0.5f),
                                 br_vec3_make(2, 2, 2));
    bool ok = false;
    br_mat4 inv = br_mat4_invert(m, &ok);
    BR_CHECK(ok);
    BR_CHECK(br_mat4_nearly_equal(br_mat4_mul(m, inv), br_mat4_identity(), EPS));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_transpose(br_mat4_transpose(m)), m, 0.0f));

    /* 2D ortho: top-left pixel -> (-1, 1), bottom-right -> (1, -1), depth in [0,1] */
    br_mat4 ortho = br_mat4_ortho(0.0f, 800.0f, 600.0f, 0.0f, -1.0f, 1.0f);
    br_vec3 tl = br_mat4_transform_point(ortho, br_vec3_make(0, 0, 0));
    br_vec3 br = br_mat4_transform_point(ortho, br_vec3_make(800, 600, 0));
    BR_CHECK(vec3_near(tl, -1.0f, 1.0f, 0.5f));
    BR_CHECK(vec3_near(br, 1.0f, -1.0f, 0.5f));

    /* perspective: near plane -> depth 0, far plane -> depth 1 */
    br_mat4 proj = br_mat4_perspective(1.0f, 1.5f, 0.1f, 100.0f);
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(proj, br_vec3_make(0, 0, -0.1f)).z, 0.0f, EPS));
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(proj, br_vec3_make(0, 0, -100.0f)).z, 1.0f, EPS));

    /* look_at: the target ends up straight ahead (-Z) */
    br_mat4 view = br_mat4_look_at(br_vec3_make(0, 0, 5), br_vec3_make(0, 0, 0), br_vec3_make(0, 1, 0));
    BR_CHECK(vec3_near(br_mat4_transform_point(view, br_vec3_make(0, 0, 0)), 0, 0, -5));

    /* quaternion rotation agrees with the matrix */
    br_quat q = br_quat_from_axis_angle(br_vec3_make(0, 0, 1), BR_PI / 2.0f);
    BR_CHECK(vec3_near(br_quat_rotate(q, br_vec3_make(1, 0, 0)), 0, 1, 0));
    BR_CHECK(vec3_near(br_mat4_transform_dir(br_mat4_from_quat(q), br_vec3_make(1, 0, 0)), 0, 1, 0));

    /* slerp halfway between identity and 90deg is 45deg */
    br_quat half = br_quat_slerp(br_quat_identity(), q, 0.5f);
    br_quat expect = br_quat_from_axis_angle(br_vec3_make(0, 0, 1), BR_PI / 4.0f);
    BR_CHECK(br_nearly_equal(fabsf(br_quat_dot(half, expect)), 1.0f, EPS));

    /* euler X then Y then Z */
    br_quat e = br_quat_from_euler(0.0f, 0.0f, BR_PI / 2.0f);
    BR_CHECK(vec3_near(br_quat_rotate(e, br_vec3_make(1, 0, 0)), 0, 1, 0));
}

static void test_shapes(void)
{
    br_rect r = br_rect_make(10, 10, 20, 10);
    BR_CHECK(br_rect_contains(r, 10, 10));
    BR_CHECK(!br_rect_contains(r, 30, 10)); /* right edge exclusive */
    BR_CHECK(br_rect_intersects(r, br_rect_make(25, 15, 10, 10)));
    BR_CHECK(!br_rect_intersects(r, br_rect_make(30, 10, 5, 5)));
    br_rect i = br_rect_intersection(r, br_rect_make(25, 15, 10, 10));
    BR_CHECK(i.x == 25 && i.y == 15 && i.width == 5 && i.height == 5);
    br_rect u = br_rect_union(r, br_rect_make(0, 0, 1, 1));
    BR_CHECK(u.x == 0 && u.y == 0 && u.width == 30 && u.height == 20);
    br_rect rb = br_rect_transform_bounds(br_rect_make(0, 0, 10, 10), br_mat3_rotation(BR_PI / 4.0f));
    BR_CHECK(br_nearly_equal(rb.width, 14.1421f, 1e-3f));

    br_circle c = { 0, 0, 5 };
    BR_CHECK(br_circle_contains(c, 3, 4));
    BR_CHECK(!br_circle_contains(c, 4, 4));
    br_ellipse el = { 0, 0, 10, 5 };
    BR_CHECK(br_ellipse_contains(el, 9, 0));
    BR_CHECK(!br_ellipse_contains(el, 0, 6));

    br_rounded_rect rr = { 0, 0, 100, 100, 20 };
    BR_CHECK(br_rounded_rect_contains(rr, 50, 1));
    BR_CHECK(br_rounded_rect_contains(rr, 10, 10));   /* inside corner circle */
    BR_CHECK(!br_rounded_rect_contains(rr, 1, 1));    /* cut-off corner */

    br_triangle t = { 0, 0, 10, 0, 0, 10 };
    BR_CHECK(br_triangle_contains(t, 2, 2));
    BR_CHECK(!br_triangle_contains(t, 8, 8));
    br_triangle t_ccw = { 0, 0, 0, 10, 10, 0 };
    BR_CHECK(br_triangle_contains(t_ccw, 2, 2));

    /* concave "U" polygon: the notch is outside */
    br_vec2 pts[] = { { 0, 0 }, { 30, 0 }, { 30, 30 }, { 20, 30 }, { 20, 10 }, { 10, 10 }, { 10, 30 }, { 0, 30 } };
    br_polygon poly = { pts, 8 };
    BR_CHECK(br_polygon_contains(poly, 5, 20));
    BR_CHECK(!br_polygon_contains(poly, 15, 20));
    br_rect pb = br_polygon_bounds(poly);
    BR_CHECK(pb.width == 30 && pb.height == 30);
    BR_CHECK(br_nearly_equal(fabsf(br_polygon_signed_area(poly)), 700.0f, EPS));

    br_vec2 hit;
    BR_CHECK(br_segment_intersect(br_vec2_make(0, 0), br_vec2_make(10, 10), br_vec2_make(0, 10),
                                  br_vec2_make(10, 0), &hit));
    BR_CHECK(vec2_near(hit, 5, 5));
    BR_CHECK(!br_segment_intersect(br_vec2_make(0, 0), br_vec2_make(1, 1), br_vec2_make(0, 10),
                                   br_vec2_make(10, 0), NULL));
    BR_CHECK(br_line_intersect(br_vec2_make(0, 0), br_vec2_make(1, 1), br_vec2_make(0, 10),
                               br_vec2_make(10, 0), &hit));
    BR_CHECK(vec2_near(hit, 5, 5));
    BR_CHECK(!br_line_intersect(br_vec2_make(0, 0), br_vec2_make(1, 0), br_vec2_make(0, 1),
                                br_vec2_make(1, 1), NULL));
    BR_CHECK(br_nearly_equal(br_segment_distance(br_vec2_make(5, 5), br_vec2_make(0, 0), br_vec2_make(10, 0)),
                             5.0f, EPS));
}

static int color_near(br_color c, float r, float g, float b, float a)
{
    return br_nearly_equal(c.r, r, 0.002f) && br_nearly_equal(c.g, g, 0.002f) && br_nearly_equal(c.b, b, 0.002f) &&
           br_nearly_equal(c.a, a, 0.002f);
}

static void test_color(void)
{
    br_color c;
    BR_CHECK(br_color_parse("#ff8000", &c) && color_near(c, 1.0f, 128.0f / 255.0f, 0.0f, 1.0f));
    BR_CHECK(br_color_parse("#f00", &c) && color_near(c, 1, 0, 0, 1));
    BR_CHECK(br_color_parse("#f008", &c) && color_near(c, 1, 0, 0, 8.0f / 15.0f));
    BR_CHECK(br_color_parse("#00ff0080", &c) && color_near(c, 0, 1, 0, 128.0f / 255.0f));
    BR_CHECK(br_color_parse("0x0000ff", &c) && color_near(c, 0, 0, 1, 1));
    BR_CHECK(br_color_parse("rgb(255, 0, 0)", &c) && color_near(c, 1, 0, 0, 1));
    BR_CHECK(br_color_parse("rgba(0,0,255,0.5)", &c) && color_near(c, 0, 0, 1, 0.5f));
    BR_CHECK(br_color_parse("CornflowerBlue", &c) && br_color_to_hex(c) == 0x6495ed);
    BR_CHECK(br_color_parse("transparent", &c) && c.a == 0.0f);
    BR_CHECK(!br_color_parse("#12345", &c));
    BR_CHECK(!br_color_parse("notacolor", &c));
    BR_CHECK(!br_color_parse("#zzzzzz", &c));

    BR_CHECK(br_color_to_hex(br_color_from_hex(0x123456, 1.0f)) == 0x123456);
    br_color rgba = br_color_from_hex_rgba(0x11223344);
    BR_CHECK(br_nearly_equal(rgba.a, 0x44 / 255.0f, 1e-6f));

    uint32_t packed = br_color_to_rgba8(br_color_make(1.0f, 0.0f, 0.5f, 1.0f));
    unsigned char bytes[4];
    memcpy(bytes, &packed, 4);
    BR_CHECK(bytes[0] == 255 && bytes[1] == 0 && bytes[2] == 128 && bytes[3] == 255);

    /* HSL round trip and known values */
    BR_CHECK(color_near(br_color_from_hsl(120.0f, 1.0f, 0.5f, 1.0f), 0, 1, 0, 1));
    float h, s, l;
    br_color_to_hsl(br_color_from_hex(0x6495ed, 1.0f), &h, &s, &l);
    br_color back = br_color_from_hsl(h, s, l, 1.0f);
    BR_CHECK(br_color_to_hex(back) == 0x6495ed);

    br_color pm = br_color_premultiply(br_color_make(1.0f, 0.5f, 0.2f, 0.5f));
    BR_CHECK(color_near(pm, 0.5f, 0.25f, 0.1f, 0.5f));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_vec();
    test_mat3();
    test_mat4_quat();
    test_shapes();
    test_color();
    return br_test_finish();
}
