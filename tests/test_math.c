/* test_math: scalars, vectors, mat3 (Pixi semantics), mat4, quat, springs, 2D/3D shapes and
 * intersections, colour spaces, easing, PCG32, noise, splines. Reference values that are not
 * hand-computed come from tests/tools/math_reference.py. */
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

static int not_nan3(br_vec3 v)
{
    return v.x == v.x && v.y == v.y && v.z == v.z;
}

static int quat_same_rotation(br_quat a, br_quat b)
{
    return br_nearly_equal(fabsf(br_quat_dot(br_quat_normalize(a), br_quat_normalize(b))), 1.0f, 1e-4f);
}

static void test_scalar(void)
{
    BR_CHECK(br_minf(2, 3) == 2 && br_maxf(2, 3) == 3);
    BR_CHECK(br_clamp01f(-1) == 0 && br_clamp01f(2) == 1 && br_clamp01f(0.25f) == 0.25f);
    BR_CHECK(br_signf(-3) == -1 && br_signf(0) == 0 && br_signf(5) == 1);
    BR_CHECK(br_stepf(1, 0.5f) == 0 && br_stepf(1, 1) == 1);
    BR_CHECK(br_nearly_equal(br_fractf(2.25f), 0.25f, EPS) && br_nearly_equal(br_fractf(-0.25f), 0.75f, EPS));
    BR_CHECK(br_fractf(-1e-9f) < 1.0f);
    BR_CHECK(br_nearly_equal(br_wrapf(370, 0, 360), 10, EPS) && br_nearly_equal(br_wrapf(-10, 0, 360), 350, EPS));
    BR_CHECK(br_wrapf(5, 3, 3) == 3); /* empty range */
    BR_CHECK(br_inverse_lerpf(10, 20, 15) == 0.5f && br_inverse_lerpf(10, 20, 30) == 2.0f);
    BR_CHECK(br_inverse_lerpf(4, 4, 9) == 0.0f); /* a == b: no division by zero */
    BR_CHECK(br_nearly_equal(br_remapf(5, 0, 10, 100, 200), 150, EPS));
    BR_CHECK(br_remapf(5, 1, 1, 7, 9) == 7);
    BR_CHECK(br_smoothstepf(0, 1, -1) == 0 && br_smoothstepf(0, 1, 2) == 1 && br_smoothstepf(0, 1, 0.5f) == 0.5f);
    BR_CHECK(br_nearly_equal(br_smoothstepf(0, 1, 0.25f), 0.15625f, EPS));    /* 3t^2 - 2t^3 */
    BR_CHECK(br_nearly_equal(br_smootherstepf(0, 1, 0.25f), 0.103515625f, EPS)); /* 6t^5 - 15t^4 + 10t^3 */
    BR_CHECK(br_move_towardsf(0, 10, 3) == 3 && br_move_towardsf(0, 10, 30) == 10 && br_move_towardsf(0, -10, 3) == -3);
    BR_CHECK(br_nearly_equal_rel(1000.0f, 1000.5f, 1e-3f) && !br_nearly_equal_rel(1.0f, 1.5f, 1e-3f));
    BR_CHECK(br_nearly_equal_rel(0.0f, 1e-7f, 1e-6f)); /* near zero: absolute */

    /* smooth damp approaches, never overshoots, velocity settles */
    float x = 0.0f, vel = 0.0f;
    bool overshot = false;
    for (int i = 0; i < 300; i++) {
        x = br_smooth_dampf(x, 10.0f, &vel, 0.3f, 0.0f, 1.0f / 60.0f);
        overshot = overshot || x > 10.0f;
    }
    BR_CHECK(!overshot && br_nearly_equal(x, 10.0f, 1e-2f));
    x = 0.0f;
    vel = 0.0f;
    x = br_smooth_dampf(x, 100.0f, &vel, 1.0f, 5.0f, 0.1f); /* max speed caps the step */
    BR_CHECK(x > 0.0f && x <= 0.5f + EPS);

    BR_CHECK(br_nearly_equal(fabsf(br_angle_wrap(3.0f * BR_PI)), BR_PI, EPS));
    BR_CHECK(br_nearly_equal(br_angle_wrap(-BR_PI), BR_PI, EPS)); /* (-pi, pi] */
    BR_CHECK(br_nearly_equal(br_angle_wrap(BR_TAU + 0.5f), 0.5f, EPS));
    /* 350deg -> 10deg is +20deg, not -340deg */
    BR_CHECK(br_nearly_equal(br_angle_delta(350.0f * BR_DEG_TO_RAD, 10.0f * BR_DEG_TO_RAD), 20.0f * BR_DEG_TO_RAD, EPS));
    BR_CHECK(br_nearly_equal(br_angle_wrap(br_angle_lerp(350.0f * BR_DEG_TO_RAD, 10.0f * BR_DEG_TO_RAD, 0.5f)), 0.0f, EPS));
    BR_CHECK(br_nearly_equal(br_angle_move_towards(0.0f, 1.0f, 0.25f), 0.25f, EPS));
    BR_CHECK(br_nearly_equal(br_angle_move_towards(0.0f, 1.0f, 2.0f), 1.0f, EPS));
}

static void test_vec_extras(void)
{
    br_vec2 a = br_vec2_make(3, 4);
    BR_CHECK(vec2_near(br_vec2_negate(a), -3, -4));
    BR_CHECK(vec2_near(br_vec2_div(a, br_vec2_make(2, 8)), 1.5f, 0.5f));
    BR_CHECK(vec2_near(br_vec2_min(a, br_vec2_make(5, 1)), 3, 1) && vec2_near(br_vec2_max(a, br_vec2_make(5, 1)), 5, 4));
    BR_CHECK(vec2_near(br_vec2_abs(br_vec2_make(-1, 2)), 1, 2) && vec2_near(br_vec2_floor(br_vec2_make(-0.5f, 1.5f)), -1, 1));
    BR_CHECK(br_vec2_length_sq(a) == 25 && br_vec2_distance_sq(a, br_vec2_make(0, 0)) == 25);
    BR_CHECK(vec2_near(br_vec2_reflect(br_vec2_make(1, 1), br_vec2_make(0, -1)), 1, -1));
    BR_CHECK(vec2_near(br_vec2_project(a, br_vec2_make(2, 0)), 3, 0));
    BR_CHECK(vec2_near(br_vec2_project(a, br_vec2_make(0, 0)), 0, 0));
    BR_CHECK(br_nearly_equal(br_vec2_angle(br_vec2_make(1, 0), br_vec2_make(0, 2)), BR_PI / 2.0f, EPS));
    BR_CHECK(br_vec2_angle(br_vec2_make(0, 0), a) == 0.0f);
    BR_CHECK(br_nearly_equal(br_vec2_signed_angle(br_vec2_make(1, 0), br_vec2_make(0, 1)), BR_PI / 2.0f, EPS));
    BR_CHECK(br_nearly_equal(br_vec2_signed_angle(br_vec2_make(1, 0), br_vec2_make(0, -1)), -BR_PI / 2.0f, EPS));
    BR_CHECK(vec2_near(br_vec2_from_angle(BR_PI / 2.0f), 0, 1));
    BR_CHECK(br_nearly_equal(br_vec2_to_angle(br_vec2_make(0, 1)), BR_PI / 2.0f, EPS));
    BR_CHECK(vec2_near(br_vec2_clamp_length(a, 1), 0.6f, 0.8f) && vec2_near(br_vec2_clamp_length(a, 10), 3, 4));
    BR_CHECK(vec2_near(br_vec2_clamp_length(br_vec2_make(0, 0), 1), 0, 0));
    BR_CHECK(vec2_near(br_vec2_move_towards(br_vec2_make(0, 0), a, 2.5f), 1.5f, 2.0f));
    BR_CHECK(vec2_near(br_vec2_move_towards(a, a, 1), 3, 4)); /* zero distance */
    BR_CHECK(br_vec2_nearly_equal(a, br_vec2_make(3.00001f, 4), EPS));

    br_vec3 v = br_vec3_make(1, 2, 2);
    BR_CHECK(vec3_near(br_vec3_negate(v), -1, -2, -2) && vec3_near(br_vec3_div(v, br_vec3_make(1, 2, 4)), 1, 1, 0.5f));
    BR_CHECK(vec3_near(br_vec3_min(v, br_vec3_make(0, 5, 2)), 0, 2, 2) && vec3_near(br_vec3_max(v, br_vec3_make(0, 5, 2)), 1, 5, 2));
    BR_CHECK(vec3_near(br_vec3_abs(br_vec3_make(-1, 0, 2)), 1, 0, 2));
    BR_CHECK(br_vec3_length_sq(v) == 9 && br_vec3_distance(v, br_vec3_make(0, 0, 0)) == 3);
    BR_CHECK(br_vec3_distance_sq(v, br_vec3_make(1, 2, 0)) == 4);
    BR_CHECK(vec3_near(br_vec3_reflect(br_vec3_make(1, -1, 0), br_vec3_make(0, 1, 0)), 1, 1, 0));
    /* refraction with eta 1 goes straight through; grazing glass->air is total internal reflection */
    BR_CHECK(vec3_near(br_vec3_refract(br_vec3_make(0, -1, 0), br_vec3_make(0, 1, 0), 1.0f), 0, -1, 0));
    br_vec3 grazing = br_vec3_normalize(br_vec3_make(1, -0.1f, 0));
    BR_CHECK(vec3_near(br_vec3_refract(grazing, br_vec3_make(0, 1, 0), 1.5f), 0, 0, 0));
    BR_CHECK(vec3_near(br_vec3_project(v, br_vec3_make(0, 0, 5)), 0, 0, 2) && vec3_near(br_vec3_project(v, br_vec3_make(0, 0, 0)), 0, 0, 0));
    BR_CHECK(vec3_near(br_vec3_project_on_plane(v, br_vec3_make(0, 1, 0)), 1, 0, 2));
    BR_CHECK(br_nearly_equal(br_vec3_angle(br_vec3_make(1, 0, 0), br_vec3_make(0, 0, 3)), BR_PI / 2.0f, EPS));
    BR_CHECK(br_vec3_angle(br_vec3_make(0, 0, 0), v) == 0.0f);
    BR_CHECK(vec3_near(br_vec3_clamp_length(v, 1.5f), 0.5f, 1, 1));
    BR_CHECK(vec3_near(br_vec3_move_towards(br_vec3_make(0, 0, 0), v, 1.5f), 0.5f, 1, 1));
    br_vec3 dirs[] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0.3f, -0.4f, 0.866f }, { 0, 0, 0 } };
    for (uint32_t i = 0; i < 6; i++) {
        br_vec3 p = br_vec3_any_perpendicular(dirs[i]);
        BR_CHECK(br_nearly_equal(br_vec3_length(p), 1.0f, EPS) && br_nearly_equal(br_vec3_dot(p, dirs[i]), 0.0f, EPS));
        if (i < 5) {
            br_vec3 n = br_vec3_normalize(dirs[i]), t, b;
            br_vec3_orthonormal_basis(n, &t, &b);
            br_vec3 c = br_vec3_cross(t, b);
            BR_CHECK(vec3_near(c, n.x, n.y, n.z) && br_nearly_equal(br_vec3_dot(t, b), 0.0f, EPS) &&
                     br_nearly_equal(br_vec3_length(t), 1.0f, EPS));
        }
    }
    /* slerp of directions keeps a constant angular speed; opposite vectors still give a unit-length result */
    br_vec3 mid = br_vec3_slerp(br_vec3_make(1, 0, 0), br_vec3_make(0, 2, 0), 0.5f);
    BR_CHECK(vec3_near(mid, 1.5f * 0.70710678f, 1.5f * 0.70710678f, 0));
    br_vec3 opp = br_vec3_slerp(br_vec3_make(1, 0, 0), br_vec3_make(-1, 0, 0), 0.5f);
    BR_CHECK(not_nan3(opp) && br_nearly_equal(br_vec3_length(opp), 1.0f, EPS) && br_nearly_equal(opp.x, 0.0f, EPS));
    BR_CHECK(vec3_near(br_vec3_slerp(br_vec3_make(0, 0, 0), br_vec3_make(2, 0, 0), 0.5f), 1, 0, 0));
    BR_CHECK(br_vec3_nearly_equal(v, br_vec3_make(1, 2, 2.00001f), EPS));

    br_vec4 q = br_vec4_make(1, 2, 3, 4);
    BR_CHECK(br_vec4_nearly_equal(br_vec4_sub(q, br_vec4_make(1, 1, 1, 1)), br_vec4_make(0, 1, 2, 3), 0));
    BR_CHECK(br_vec4_nearly_equal(br_vec4_mul(q, q), br_vec4_make(1, 4, 9, 16), 0));
    BR_CHECK(br_vec4_nearly_equal(br_vec4_negate(q), br_vec4_make(-1, -2, -3, -4), 0));
    BR_CHECK(br_vec4_nearly_equal(br_vec4_min(q, br_vec4_make(2, 2, 2, 2)), br_vec4_make(1, 2, 2, 2), 0));
    BR_CHECK(br_vec4_nearly_equal(br_vec4_max(q, br_vec4_make(2, 2, 2, 2)), br_vec4_make(2, 2, 3, 4), 0));
    BR_CHECK(br_vec4_length_sq(q) == 30 && br_nearly_equal(br_vec4_length(br_vec4_normalize(q)), 1.0f, EPS));
    BR_CHECK(br_vec4_nearly_equal(br_vec4_normalize(br_vec4_make(0, 0, 0, 0)), br_vec4_make(0, 0, 0, 0), 0));
}

static void test_mat4_extras(void)
{
    br_quat r = br_quat_from_euler(0.3f, -0.7f, 1.1f);
    br_mat4 m = br_mat4_from_trs(br_vec3_make(1, 2, 3), r, br_vec3_make(2, 3, 4));
    BR_CHECK(br_nearly_equal(br_mat4_determinant(m), 24.0f, 1e-3f)); /* scale product */
    BR_CHECK(br_mat4_determinant(br_mat4_scaling(br_vec3_make(1, 0, 1))) == 0.0f);
    bool ok = false;
    br_mat4 inv = br_mat4_invert_affine(m, &ok);
    BR_CHECK(ok && br_mat4_nearly_equal(inv, br_mat4_invert(m, NULL), EPS));
    br_mat4_invert_affine(br_mat4_scaling(br_vec3_make(0, 1, 1)), &ok);
    BR_CHECK(!ok);
    br_mat4 rigid = br_mat4_from_trs(br_vec3_make(-4, 5, 6), r, br_vec3_make(1, 1, 1));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_invert_rigid(rigid), br_mat4_invert(rigid, NULL), EPS));

    /* axis rotations agree with the general axis-angle rotation */
    BR_CHECK(br_mat4_nearly_equal(br_mat4_rotation_x(0.8f), br_mat4_rotation(br_vec3_make(1, 0, 0), 0.8f), EPS));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_rotation_y(0.8f), br_mat4_rotation(br_vec3_make(0, 1, 0), 0.8f), EPS));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_rotation_z(0.8f), br_mat4_rotation(br_vec3_make(0, 0, 1), 0.8f), EPS));
    BR_CHECK(vec3_near(br_mat4_transform_dir(br_mat4_rotation_x(BR_PI / 2), br_vec3_make(0, 1, 0)), 0, 0, 1));

    /* normal matrix keeps normals perpendicular under non-uniform scale */
    br_mat4 squash = br_mat4_scaling(br_vec3_make(2, 1, 1));
    br_vec3 tangent = br_mat4_transform_dir(squash, br_vec3_make(1, -1, 0));
    br_vec3 normal = br_mat4_transform_dir(br_mat4_normal_matrix(squash), br_vec3_make(1, 1, 0));
    BR_CHECK(br_nearly_equal(br_vec3_dot(tangent, normal), 0.0f, EPS));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_normal_matrix(br_mat4_scaling(br_vec3_make(0, 0, 0))), br_mat4_identity(), 0));

    /* decompose recovers TRS, including a mirror */
    br_vec3 t, s;
    br_quat q;
    BR_CHECK(br_mat4_decompose(m, &t, &q, &s));
    BR_CHECK(vec3_near(t, 1, 2, 3) && vec3_near(s, 2, 3, 4) && quat_same_rotation(q, r));
    br_mat4 mirrored = br_mat4_from_trs(br_vec3_make(0, 0, 0), r, br_vec3_make(-2, 3, 4));
    BR_CHECK(br_mat4_decompose(mirrored, &t, &q, &s));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_from_trs(t, q, s), mirrored, EPS));
    BR_CHECK(!br_mat4_decompose(br_mat4_scaling(br_vec3_make(1, 0, 1)), &t, &q, &s));

    BR_CHECK(br_mat4_nearly_equal(br_mat4_look_to(br_vec3_make(1, 2, 3), br_vec3_make(0, 0, -2), br_vec3_make(0, 1, 0)),
                                  br_mat4_look_at(br_vec3_make(1, 2, 3), br_vec3_make(1, 2, 1), br_vec3_make(0, 1, 0)), EPS));

    /* a symmetric frustum is the plain perspective; an off-centre one shifts the centre */
    float n = 0.5f, f = 50.0f, fov = 1.0f, aspect = 1.5f;
    float top = n * tanf(fov * 0.5f);
    BR_CHECK(br_mat4_nearly_equal(br_mat4_frustum(-top * aspect, top * aspect, -top, top, n, f),
                                  br_mat4_perspective(fov, aspect, n, f), EPS));
    br_mat4 off = br_mat4_frustum(0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 10.0f);
    BR_CHECK(vec3_near(br_mat4_transform_point(off, br_vec3_make(0.5f, 0.5f, -1.0f)), 0, 0, 0));

    br_mat4 inf = br_mat4_perspective_infinite(fov, aspect, n);
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(inf, br_vec3_make(0, 0, -n)).z, 0.0f, EPS));
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(inf, br_vec3_make(0, 0, -1e6f)).z, 1.0f, 1e-3f));
    br_mat4 rev = br_mat4_perspective_reversed(fov, aspect, n, f);
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(rev, br_vec3_make(0, 0, -n)).z, 1.0f, EPS));
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(rev, br_vec3_make(0, 0, -f)).z, 0.0f, EPS));
    br_mat4 rev_inf = br_mat4_perspective_reversed_infinite(fov, aspect, n);
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(rev_inf, br_vec3_make(0, 0, -n)).z, 1.0f, EPS));
    BR_CHECK(br_nearly_equal(br_mat4_transform_point(rev_inf, br_vec3_make(0, 0, -1e6f)).z, 0.0f, 1e-3f));
}

static void test_quat_extras(void)
{
    br_quat q = br_quat_from_axis_angle(br_vec3_make(0, 1, 0), 1.2f);
    BR_CHECK(br_nearly_equal(br_quat_length(q), 1.0f, EPS));
    br_quat scaled = br_quat_make(q.x * 3, q.y * 3, q.z * 3, q.w * 3);
    br_quat back = br_quat_mul(scaled, br_quat_inverse(scaled));
    BR_CHECK(quat_same_rotation(back, br_quat_identity()) && br_nearly_equal(back.w, 1.0f, EPS));
    BR_CHECK(br_quat_inverse(br_quat_make(0, 0, 0, 0)).w == 1.0f);

    br_quat a = br_quat_identity();
    BR_CHECK(br_nearly_equal(br_quat_angle(a, q), 1.2f, EPS));
    BR_CHECK(quat_same_rotation(br_quat_nlerp(a, q, 0.0f), a) && quat_same_rotation(br_quat_nlerp(a, q, 1.0f), q));
    /* nlerp takes the short way: -q is the same rotation */
    br_quat neg = br_quat_make(-q.x, -q.y, -q.z, -q.w);
    BR_CHECK(quat_same_rotation(br_quat_nlerp(a, neg, 0.5f), br_quat_from_axis_angle(br_vec3_make(0, 1, 0), 0.6f)));

    br_vec3 axis;
    float angle;
    br_quat_to_axis_angle(q, &axis, &angle);
    BR_CHECK(vec3_near(axis, 0, 1, 0) && br_nearly_equal(angle, 1.2f, EPS));
    br_quat_to_axis_angle(neg, &axis, &angle);
    BR_CHECK(vec3_near(axis, 0, 1, 0) && br_nearly_equal(angle, 1.2f, EPS));
    br_quat_to_axis_angle(br_quat_identity(), &axis, &angle);
    BR_CHECK(vec3_near(axis, 1, 0, 0) && angle == 0.0f);

    float x, y, z;
    br_quat_to_euler(br_quat_from_euler(0.3f, -0.7f, 1.1f), &x, &y, &z);
    BR_CHECK(br_nearly_equal(x, 0.3f, EPS) && br_nearly_equal(y, -0.7f, EPS) && br_nearly_equal(z, 1.1f, EPS));
    br_quat pole = br_quat_from_euler(0.4f, BR_PI / 2.0f, 0.0f); /* gimbal lock still round-trips the rotation */
    br_quat_to_euler(pole, &x, &y, &z);
    BR_CHECK(br_nearly_equal(y, BR_PI / 2.0f, 1e-3f) && quat_same_rotation(br_quat_from_euler(x, y, z), pole));

    BR_CHECK(quat_same_rotation(br_quat_from_mat4(br_mat4_from_trs(br_vec3_make(5, 5, 5), q, br_vec3_make(3, 3, 3))), q));

    br_quat ft = br_quat_from_to(br_vec3_make(2, 0, 0), br_vec3_make(0, 0, 5));
    BR_CHECK(vec3_near(br_quat_rotate(ft, br_vec3_make(1, 0, 0)), 0, 0, 1));
    br_quat flip = br_quat_from_to(br_vec3_make(1, 0, 0), br_vec3_make(-1, 0, 0));
    BR_CHECK(vec3_near(br_quat_rotate(flip, br_vec3_make(1, 0, 0)), -1, 0, 0));
    BR_CHECK(quat_same_rotation(br_quat_from_to(br_vec3_make(0, 0, 0), br_vec3_make(1, 0, 0)), br_quat_identity()));

    /* look rotation: -Z goes to forward, +Y stays up; matches br_mat4_look_at's camera */
    br_vec3 fwd = br_vec3_normalize(br_vec3_make(1, 0, -1));
    br_quat look = br_quat_look_rotation(fwd, br_vec3_make(0, 1, 0));
    BR_CHECK(vec3_near(br_quat_rotate(look, br_vec3_make(0, 0, -1)), fwd.x, fwd.y, fwd.z));
    BR_CHECK(vec3_near(br_quat_rotate(look, br_vec3_make(0, 1, 0)), 0, 1, 0));
    br_mat4 view = br_mat4_look_at(br_vec3_make(0, 0, 0), fwd, br_vec3_make(0, 1, 0));
    BR_CHECK(br_mat4_nearly_equal(br_mat4_invert(view, NULL), br_mat4_from_quat(look), EPS));
    br_quat up_fwd = br_quat_look_rotation(br_vec3_make(0, 1, 0), br_vec3_make(0, 1, 0)); /* up parallel */
    BR_CHECK(vec3_near(br_quat_rotate(up_fwd, br_vec3_make(0, 0, -1)), 0, 1, 0));
    BR_CHECK(quat_same_rotation(br_quat_look_rotation(br_vec3_make(0, 0, 0), br_vec3_make(0, 1, 0)), br_quat_identity()));

    br_quat step = br_quat_rotate_towards(a, q, 0.2f);
    BR_CHECK(br_nearly_equal(br_quat_angle(a, step), 0.2f, EPS));
    BR_CHECK(quat_same_rotation(br_quat_rotate_towards(a, q, 5.0f), q));
}

static void test_springs(void)
{
    /* critically damped: reaches the target without overshoot; under-damped overshoots */
    float x = 0.0f, v = 0.0f, peak = 0.0f;
    for (int i = 0; i < 240; i++) {
        br_spring_step(&x, &v, 1.0f, 2.0f, 1.0f, 1.0f / 60.0f);
        peak = br_maxf(peak, x);
    }
    BR_CHECK(peak <= 1.0f && br_nearly_equal(x, 1.0f, 1e-3f));
    x = 0.0f;
    v = 0.0f;
    peak = 0.0f;
    for (int i = 0; i < 240; i++) {
        br_spring_step(&x, &v, 1.0f, 2.0f, 0.2f, 1.0f / 60.0f);
        peak = br_maxf(peak, x);
    }
    BR_CHECK(peak > 1.2f);
    /* a huge dt stays finite and bounded */
    x = 0.0f;
    v = 0.0f;
    br_spring_step(&x, &v, 1.0f, 50.0f, 0.1f, 10.0f);
    BR_CHECK(x == x && x >= 0.0f && x <= 1.0f);
    br_vec3 p = br_vec3_make(0, 0, 0), pv = br_vec3_make(0, 0, 0);
    br_vec2 p2 = br_vec2_make(0, 0), pv2 = br_vec2_make(0, 0);
    for (int i = 0; i < 600; i++) {
        br_spring_step_vec3(&p, &pv, br_vec3_make(1, 2, 3), 1.0f, 1.0f, 1.0f / 60.0f);
        br_spring_step_vec2(&p2, &pv2, br_vec2_make(-1, 4), 1.0f, 1.0f, 1.0f / 60.0f);
    }
    BR_CHECK(br_vec3_nearly_equal(p, br_vec3_make(1, 2, 3), 1e-2f) && br_vec2_nearly_equal(p2, br_vec2_make(-1, 4), 1e-2f));
}

static void test_sat2d(void)
{
    br_vec2 sq_a[4] = { { 0, 0 }, { 2, 0 }, { 2, 2 }, { 0, 2 } };
    br_vec2 sq_b[4] = { { 1.5f, 0.5f }, { 3.5f, 0.5f }, { 3.5f, 1.5f }, { 1.5f, 1.5f } };
    br_polygon a = { sq_a, 4 };
    br_polygon b = { sq_b, 4 };
    br_vec2 mtv;
    BR_CHECK(br_sat_polygon_polygon(a, b, &mtv));
    BR_CHECK(vec2_near(mtv, -0.5f, 0.0f)); /* a is pushed left, out of b */
    br_vec2 far_pts[4] = { { 5, 5 }, { 6, 5 }, { 6, 6 }, { 5, 6 } };
    br_polygon far_poly = { far_pts, 4 };
    BR_CHECK(!br_sat_polygon_polygon(a, far_poly, NULL));
    /* rotated box (diamond) near a corner: the corner gap separates them */
    br_vec2 diamond[4];
    br_obb2d_corners(br_vec2_make(3.2f, 3.2f), br_vec2_make(1, 1), BR_PI / 4.0f, diamond);
    BR_CHECK(vec2_near(diamond[0], 3.2f, 3.2f - 1.41421356f));
    br_polygon d = { diamond, 4 };
    BR_CHECK(!br_sat_polygon_polygon(a, d, NULL)); /* AABBs overlap, shapes do not */

    br_circle c = { 3.0f, 1.0f, 1.5f };
    BR_CHECK(br_sat_circle_polygon(c, a, &mtv));
    BR_CHECK(vec2_near(mtv, 0.5f, 0.0f));
    br_circle corner = { 2.8f, 2.8f, 1.0f }; /* AABB touches, corner distance 1.13 > 1 */
    BR_CHECK(!br_sat_circle_polygon(corner, a, NULL));
    br_polygon empty = { sq_a, 0 };
    BR_CHECK(!br_sat_polygon_polygon(a, empty, NULL) && !br_sat_circle_polygon(c, empty, NULL));
}

static void test_geometry3d(void)
{
    br_vec3 o = br_vec3_make(0, 0, 10);
    br_vec3 down = br_vec3_make(0, 0, -1);
    float t = -1.0f;

    br_plane ground = br_plane_from_point_normal(br_vec3_make(0, 0, 2), br_vec3_make(0, 0, 5));
    BR_CHECK(vec3_near(ground.normal, 0, 0, 1) && br_nearly_equal(ground.d, -2.0f, EPS));
    br_plane tri_plane = br_plane_from_points(br_vec3_make(0, 0, 2), br_vec3_make(1, 0, 2), br_vec3_make(0, 1, 2));
    BR_CHECK(vec3_near(tri_plane.normal, 0, 0, 1) && br_nearly_equal(tri_plane.d, -2.0f, EPS));
    br_plane line = br_plane_from_points(br_vec3_make(0, 0, 0), br_vec3_make(1, 1, 1), br_vec3_make(2, 2, 2));
    BR_CHECK(vec3_near(line.normal, 0, 0, 0) && not_nan3(line.normal));
    BR_CHECK(br_nearly_equal(br_plane_distance(ground, br_vec3_make(4, 4, 7)), 5.0f, EPS));

    BR_CHECK(br_ray_plane(o, down, ground, &t) && br_nearly_equal(t, 8.0f, EPS));
    BR_CHECK(!br_ray_plane(o, br_vec3_make(1, 0, 0), ground, NULL)); /* parallel */
    BR_CHECK(!br_ray_plane(o, br_vec3_make(0, 0, 1), ground, NULL)); /* behind */
    BR_CHECK(!br_ray_plane(o, down, line, NULL));                    /* zero plane */

    br_sphere s = { { 0, 0, 0 }, 2 };
    BR_CHECK(br_ray_sphere(o, down, s, &t) && br_nearly_equal(t, 8.0f, EPS));
    BR_CHECK(br_ray_sphere(o, br_vec3_make(0, 0, -4), s, &t) && br_nearly_equal(t, 2.0f, EPS)); /* t in dir units */
    BR_CHECK(br_ray_sphere(br_vec3_make(0, 0, 1), down, s, &t) && t == 0.0f);                  /* inside */
    BR_CHECK(!br_ray_sphere(o, br_vec3_make(0, 0, 1), s, NULL));
    BR_CHECK(!br_ray_sphere(br_vec3_make(3, 0, 10), down, s, NULL));
    BR_CHECK(!br_ray_sphere(o, br_vec3_make(0, 0, 0), s, NULL)); /* zero direction */

    br_vec3 bmin = br_vec3_make(-1, -1, -1), bmax = br_vec3_make(1, 1, 1);
    BR_CHECK(br_ray_box(o, down, bmin, bmax, &t) && br_nearly_equal(t, 9.0f, EPS));
    BR_CHECK(br_ray_box(br_vec3_make(0, 0, 0), down, bmin, bmax, &t) && t == 0.0f);
    BR_CHECK(!br_ray_box(br_vec3_make(2, 0, 10), down, bmin, bmax, NULL)); /* parallel to x slab, outside */
    BR_CHECK(!br_ray_box(o, br_vec3_make(0, 0, 1), bmin, bmax, NULL));
    BR_CHECK(br_ray_box(br_vec3_make(-5, -5, 0), br_vec3_make(1, 1, 0), bmin, bmax, &t) && br_nearly_equal(t, 4.0f, EPS));

    br_obb ob = { { 0, 0, 0 }, { 2, 0.5f, 0.5f }, br_quat_from_axis_angle(br_vec3_make(0, 0, 1), BR_PI / 2.0f) };
    /* the long axis now points along y: a ray down y hits at 10 - 2 = 8 */
    BR_CHECK(br_ray_obb(br_vec3_make(0, 10, 0), br_vec3_make(0, -1, 0), ob, &t) && br_nearly_equal(t, 8.0f, EPS));
    BR_CHECK(!br_ray_obb(br_vec3_make(1.5f, 10, 0), br_vec3_make(0, -1, 0), ob, NULL));

    br_vec3 ta = br_vec3_make(-1, -1, 0), tb = br_vec3_make(1, -1, 0), tc = br_vec3_make(0, 1, 0);
    float u, v;
    BR_CHECK(br_ray_triangle(o, down, ta, tb, tc, &t, &u, &v) && br_nearly_equal(t, 10.0f, EPS));
    BR_CHECK(br_nearly_equal(u, 0.25f, EPS) && br_nearly_equal(v, 0.5f, EPS));
    BR_CHECK(br_ray_triangle(br_vec3_make(0, 0, -10), br_vec3_make(0, 0, 1), ta, tb, tc, &t, NULL, NULL)); /* back side */
    BR_CHECK(!br_ray_triangle(br_vec3_make(5, 0, 10), down, ta, tb, tc, NULL, NULL, NULL));
    BR_CHECK(!br_ray_triangle(o, br_vec3_make(1, 0, 0), ta, tb, tc, NULL, NULL, NULL));                   /* parallel */
    BR_CHECK(!br_ray_triangle(o, down, ta, tb, br_vec3_make(3, -1, 0), NULL, NULL, NULL));                /* degenerate */

    br_sphere s2 = { { 3.5f, 0, 0 }, 1.5f };
    BR_CHECK(br_sphere_sphere(s, s2));
    s2.center.x = 3.6f;
    BR_CHECK(!br_sphere_sphere(s, s2));
    BR_CHECK(br_sphere_box(s2, bmin, br_vec3_make(2.2f, 1, 1)) && !br_sphere_box(s2, bmin, bmax));
    BR_CHECK(br_box_box(bmin, bmax, br_vec3_make(1, 1, 1), br_vec3_make(2, 2, 2)));
    BR_CHECK(!br_box_box(bmin, bmax, br_vec3_make(1.1f, 0, 0), br_vec3_make(2, 2, 2)));

    br_obb ob2 = { { 2.2f, 0, 0 }, { 0.5f, 0.5f, 0.5f }, br_quat_identity() };
    BR_CHECK(!br_obb_obb(ob, ob2)); /* ob is thin along x after its turn */
    ob2.center.x = 0.9f;
    BR_CHECK(br_obb_obb(ob, ob2));
    /* edge-edge separation: two cubes rotated 45 degrees about different axes, just apart */
    br_obb e1 = { { 0, 0, 0 }, { 1, 1, 1 }, br_quat_from_axis_angle(br_vec3_make(0, 0, 1), BR_PI / 4.0f) };
    br_obb e2 = { { 2.9f, 0, 0 }, { 1, 1, 1 }, br_quat_from_axis_angle(br_vec3_make(0, 1, 0), BR_PI / 4.0f) };
    BR_CHECK(!br_obb_obb(e1, e2)); /* 1.414 + 1.414 = 2.83 < 2.9 */
    e2.center.x = 2.7f;
    BR_CHECK(br_obb_obb(e1, e2));

    br_plane planes[6];
    br_mat4 vp = br_mat4_mul(br_mat4_perspective(1.0f, 1.0f, 0.1f, 100.0f),
                             br_mat4_look_at(br_vec3_make(0, 0, 5), br_vec3_make(0, 0, 0), br_vec3_make(0, 1, 0)));
    br_frustum_planes(vp, planes);
    for (int i = 0; i < 6; i++) {
        BR_CHECK(br_nearly_equal(br_vec3_length(planes[i].normal), 1.0f, EPS));
    }
    BR_CHECK(br_nearly_equal(br_plane_distance(planes[4], br_vec3_make(0, 0, 4.9f)), 0.0f, EPS)); /* near plane */
    br_sphere at_origin = { { 0, 0, 0 }, 1 };
    br_sphere behind = { { 0, 0, 10 }, 1 };
    br_sphere side = { { 50, 0, 0 }, 1 };
    BR_CHECK(br_frustum_planes_sphere(planes, at_origin) && !br_frustum_planes_sphere(planes, behind) &&
             !br_frustum_planes_sphere(planes, side));
    BR_CHECK(br_frustum_planes_box(planes, bmin, bmax));
    BR_CHECK(!br_frustum_planes_box(planes, br_vec3_make(-1, -1, 8), br_vec3_make(1, 1, 9)));

    BR_CHECK(vec3_near(br_closest_point_segment(br_vec3_make(5, 3, 0), br_vec3_make(0, 0, 0), br_vec3_make(2, 0, 0)), 2, 0, 0));
    BR_CHECK(vec3_near(br_closest_point_segment(br_vec3_make(5, 3, 0), br_vec3_make(1, 1, 1), br_vec3_make(1, 1, 1)), 1, 1, 1));
    BR_CHECK(vec3_near(br_closest_point_ray(br_vec3_make(-5, 3, 0), br_vec3_make(0, 0, 0), br_vec3_make(1, 0, 0)), 0, 0, 0));
    BR_CHECK(vec3_near(br_closest_point_ray(br_vec3_make(5, 3, 0), br_vec3_make(0, 0, 0), br_vec3_make(2, 0, 0)), 5, 0, 0));
    BR_CHECK(vec3_near(br_closest_point_plane(br_vec3_make(4, 4, 7), ground), 4, 4, 2));
    BR_CHECK(vec3_near(br_closest_point_box(br_vec3_make(3, 0.5f, -4), bmin, bmax), 1, 0.5f, -1));
    BR_CHECK(vec3_near(br_closest_point_obb(br_vec3_make(0, 5, 0), ob), 0, 2, 0));
    BR_CHECK(vec3_near(br_closest_point_triangle(br_vec3_make(0, 0, 3), ta, tb, tc), 0, 0, 0));   /* face */
    BR_CHECK(vec3_near(br_closest_point_triangle(br_vec3_make(-3, -3, 0), ta, tb, tc), -1, -1, 0)); /* vertex */
    BR_CHECK(vec3_near(br_closest_point_triangle(br_vec3_make(0, -3, 1), ta, tb, tc), 0, -1, 0));  /* edge */
    br_vec3 deg = br_closest_point_triangle(br_vec3_make(1, 2, 0), br_vec3_make(0, 0, 0), br_vec3_make(2, 0, 0),
                                            br_vec3_make(4, 0, 0));
    BR_CHECK(vec3_near(deg, 1, 0, 0));

    br_vec3 c1, c2;
    float d2 = br_closest_points_segments(br_vec3_make(0, 0, 0), br_vec3_make(2, 0, 0), br_vec3_make(1, 1, -1),
                                          br_vec3_make(1, 1, 1), &c1, &c2);
    BR_CHECK(br_nearly_equal(d2, 1.0f, EPS) && vec3_near(c1, 1, 0, 0) && vec3_near(c2, 1, 1, 0));
    d2 = br_closest_points_segments(br_vec3_make(0, 0, 0), br_vec3_make(1, 0, 0), br_vec3_make(0, 2, 0),
                                    br_vec3_make(1, 2, 0), &c1, &c2); /* parallel */
    BR_CHECK(br_nearly_equal(d2, 4.0f, EPS) && not_nan3(c1) && not_nan3(c2));
    d2 = br_closest_points_segments(br_vec3_make(3, 0, 0), br_vec3_make(3, 0, 0), br_vec3_make(0, 0, 0),
                                    br_vec3_make(0, 0, 0), NULL, NULL); /* two points */
    BR_CHECK(br_nearly_equal(d2, 9.0f, EPS));

    br_vec3 bary;
    BR_CHECK(br_barycentric(br_vec3_make(0, 0, 0), ta, tb, tc, &bary));
    BR_CHECK(vec3_near(bary, 0.25f, 0.25f, 0.5f));
    BR_CHECK(!br_barycentric(br_vec3_make(0, 0, 0), ta, tb, br_vec3_make(3, -1, 0), &bary) && vec3_near(bary, 1, 0, 0));
}

static void test_color_spaces(void)
{
    /* reference values: tests/tools/math_reference.py */
    BR_CHECK(br_nearly_equal(br_srgb_to_linear(0.5f), 0.2140411f, 1e-5f));
    BR_CHECK(br_nearly_equal(br_linear_to_srgb(0.5f), 0.7353570f, 1e-5f));
    BR_CHECK(br_srgb_to_linear(0.0f) == 0.0f && br_nearly_equal(br_srgb_to_linear(1.0f), 1.0f, 1e-6f));
    br_color c = br_color_from_hex(0x6495ed, 0.5f);
    br_color round = br_color_to_srgb(br_color_to_linear(c));
    BR_CHECK(color_near(round, c.r, c.g, c.b, 0.5f));

    float h, s, v;
    br_color_to_hsv(c, &h, &s, &v);
    BR_CHECK(br_nearly_equal(h, 218.5401f, 1e-2f) && br_nearly_equal(s, 0.578059f, 1e-4f) && br_nearly_equal(v, 0.929412f, 1e-4f));
    BR_CHECK(br_color_to_hex(br_color_from_hsv(h, s, v, 1.0f)) == 0x6495ed);
    BR_CHECK(color_near(br_color_from_hsv(120.0f, 1.0f, 1.0f, 1.0f), 0, 1, 0, 1));
    BR_CHECK(color_near(br_color_from_hsv(-120.0f, 1.0f, 1.0f, 1.0f), 0, 0, 1, 1)); /* hue wraps */
    br_color_to_hsv(br_color_make(0, 0, 0, 1), &h, &s, &v);
    BR_CHECK(h == 0.0f && s == 0.0f && v == 0.0f);

    br_vec3 lab = br_color_to_oklab(br_color_make(1, 0, 0, 1));
    BR_CHECK(vec3_near(lab, 0.627955f, 0.224863f, 0.125846f));
    lab = br_color_to_oklab(br_color_make(1, 1, 1, 1));
    BR_CHECK(vec3_near(lab, 1.0f, 0.0f, 0.0f));
    lab = br_color_to_oklab(c);
    BR_CHECK(vec3_near(lab, 0.674622f, -0.021289f, -0.139745f));
    BR_CHECK(color_near(br_color_from_oklab(lab, 0.5f), c.r, c.g, c.b, 0.5f));
    br_color mid = br_color_lerp_oklab(br_color_make(1, 0, 0, 1), br_color_make(0, 0, 1, 0), 0.5f);
    BR_CHECK(color_near(mid, 0.550441f, 0.325621f, 0.636501f, 0.5f));
    br_color ends = br_color_lerp_oklab(br_color_make(1, 0, 0, 1), br_color_make(0, 0, 1, 0), 1.0f);
    BR_CHECK(color_near(ends, 0, 0, 1, 0));
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
    test_scalar();
    test_vec_extras();
    test_mat4_extras();
    test_quat_extras();
    test_springs();
    test_sat2d();
    test_geometry3d();
    test_color_spaces();
    return br_test_finish();
}
