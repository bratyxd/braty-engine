/* br_spline: Bezier / Catmull-Rom / Hermite curves, paths and arc-length tables. */
#include "braty/br_spline.h"

#include <SDL3/SDL_stdinc.h>

/* The curves are written once per dimension as weights; the vec2 / vec3 versions combine points. */

static void br_bezier_quadratic_weights(float t, float w[3])
{
    float u = 1.0f - t;
    w[0] = u * u;
    w[1] = 2.0f * u * t;
    w[2] = t * t;
}

static void br_bezier_quadratic_tangent_weights(float t, float w[3])
{
    w[0] = -2.0f * (1.0f - t);
    w[1] = 2.0f - 4.0f * t;
    w[2] = 2.0f * t;
}

static void br_bezier_cubic_weights(float t, float w[4])
{
    float u = 1.0f - t;
    w[0] = u * u * u;
    w[1] = 3.0f * u * u * t;
    w[2] = 3.0f * u * t * t;
    w[3] = t * t * t;
}

static void br_bezier_cubic_tangent_weights(float t, float w[4])
{
    float u = 1.0f - t;
    w[0] = -3.0f * u * u;
    w[1] = 3.0f * u * u - 6.0f * u * t;
    w[2] = 6.0f * u * t - 3.0f * t * t;
    w[3] = 3.0f * t * t;
}

static void br_catmull_rom_weights(float t, float w[4])
{
    /* 0.5 * (-t^3 + 2t^2 - t, 3t^3 - 5t^2 + 2, -3t^3 + 4t^2 + t, t^3 - t^2) */
    float t2 = t * t;
    float t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
    w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

static void br_catmull_rom_tangent_weights(float t, float w[4])
{
    float t2 = t * t;
    w[0] = 0.5f * (-3.0f * t2 + 4.0f * t - 1.0f);
    w[1] = 0.5f * (9.0f * t2 - 10.0f * t);
    w[2] = 0.5f * (-9.0f * t2 + 8.0f * t + 1.0f);
    w[3] = 0.5f * (3.0f * t2 - 2.0f * t);
}

static void br_hermite_weights(float t, float w[4])
{
    /* basis for p0, m0, p1, m1 */
    float t2 = t * t;
    float t3 = t2 * t;
    w[0] = 2.0f * t3 - 3.0f * t2 + 1.0f;
    w[1] = t3 - 2.0f * t2 + t;
    w[2] = -2.0f * t3 + 3.0f * t2;
    w[3] = t3 - t2;
}

static br_vec2 br_combine2(const br_vec2 *p, const float *w, int n)
{
    br_vec2 r = br_vec2_make(0.0f, 0.0f);
    for (int i = 0; i < n; i++) {
        r = br_vec2_add(r, br_vec2_scale(p[i], w[i]));
    }
    return r;
}

static br_vec3 br_combine3(const br_vec3 *p, const float *w, int n)
{
    br_vec3 r = br_vec3_make(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < n; i++) {
        r = br_vec3_add(r, br_vec3_scale(p[i], w[i]));
    }
    return r;
}

br_vec2 br_spline_bezier_quadratic2(br_vec2 p0, br_vec2 p1, br_vec2 p2, float t)
{
    br_vec2 p[3] = { p0, p1, p2 };
    float w[3];
    br_bezier_quadratic_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 3);
}

br_vec3 br_spline_bezier_quadratic3(br_vec3 p0, br_vec3 p1, br_vec3 p2, float t)
{
    br_vec3 p[3] = { p0, p1, p2 };
    float w[3];
    br_bezier_quadratic_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 3);
}

br_vec2 br_spline_bezier_quadratic_tangent2(br_vec2 p0, br_vec2 p1, br_vec2 p2, float t)
{
    br_vec2 p[3] = { p0, p1, p2 };
    float w[3];
    br_bezier_quadratic_tangent_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 3);
}

br_vec3 br_spline_bezier_quadratic_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, float t)
{
    br_vec3 p[3] = { p0, p1, p2 };
    float w[3];
    br_bezier_quadratic_tangent_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 3);
}

br_vec2 br_spline_bezier_cubic2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t)
{
    br_vec2 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_bezier_cubic_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 4);
}

br_vec3 br_spline_bezier_cubic3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t)
{
    br_vec3 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_bezier_cubic_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 4);
}

br_vec2 br_spline_bezier_cubic_tangent2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t)
{
    br_vec2 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_bezier_cubic_tangent_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 4);
}

br_vec3 br_spline_bezier_cubic_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t)
{
    br_vec3 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_bezier_cubic_tangent_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 4);
}

br_vec2 br_spline_catmull_rom2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t)
{
    br_vec2 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_catmull_rom_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 4);
}

br_vec3 br_spline_catmull_rom3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t)
{
    br_vec3 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_catmull_rom_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 4);
}

br_vec3 br_spline_catmull_rom_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t)
{
    br_vec3 p[4] = { p0, p1, p2, p3 };
    float w[4];
    br_catmull_rom_tangent_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 4);
}

float br_spline_hermitef(float p0, float m0, float p1, float m1, float t)
{
    float w[4];
    br_hermite_weights(br_clamp01f(t), w);
    return w[0] * p0 + w[1] * m0 + w[2] * p1 + w[3] * m1;
}

br_vec2 br_spline_hermite2(br_vec2 p0, br_vec2 m0, br_vec2 p1, br_vec2 m1, float t)
{
    br_vec2 p[4] = { p0, m0, p1, m1 };
    float w[4];
    br_hermite_weights(br_clamp01f(t), w);
    return br_combine2(p, w, 4);
}

br_vec3 br_spline_hermite3(br_vec3 p0, br_vec3 m0, br_vec3 p1, br_vec3 m1, float t)
{
    br_vec3 p[4] = { p0, m0, p1, m1 };
    float w[4];
    br_hermite_weights(br_clamp01f(t), w);
    return br_combine3(p, w, 4);
}

/* ---- paths */

uint32_t br_spline3_segment_count(br_spline3 spline)
{
    if (spline.count < 2) {
        return 0;
    }
    return spline.kind == BR_SPLINE_BEZIER ? (spline.count - 1) / 3 : spline.count - 1;
}

/* The four points of segment `seg` (Catmull-Rom repeats the end points as outer shapers). */
static void br_spline3_segment(br_spline3 spline, uint32_t seg, br_vec3 out[4])
{
    if (spline.kind == BR_SPLINE_BEZIER) {
        for (uint32_t i = 0; i < 4; i++) {
            out[i] = spline.points[seg * 3 + i];
        }
        return;
    }
    uint32_t last = spline.count - 1;
    out[0] = spline.points[seg == 0 ? 0 : seg - 1];
    out[1] = spline.points[seg];
    out[2] = spline.points[seg + 1];
    out[3] = spline.points[seg + 2 > last ? last : seg + 2];
}

/* Splits a whole-path t into a segment index and a local t. */
static uint32_t br_spline3_locate(uint32_t segments, float t, float *local)
{
    float u = br_clamp01f(t) * (float)segments;
    uint32_t seg = (uint32_t)u;
    if (seg >= segments) {
        seg = segments - 1;
    }
    *local = u - (float)seg;
    return seg;
}

br_vec3 br_spline3_point(br_spline3 spline, float t)
{
    uint32_t segments = br_spline3_segment_count(spline);
    if (segments == 0) {
        return spline.count > 0 ? spline.points[0] : br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    float local;
    br_vec3 p[4];
    br_spline3_segment(spline, br_spline3_locate(segments, t, &local), p);
    if (spline.kind == BR_SPLINE_BEZIER) {
        return br_spline_bezier_cubic3(p[0], p[1], p[2], p[3], local);
    }
    return br_spline_catmull_rom3(p[0], p[1], p[2], p[3], local);
}

br_vec3 br_spline3_tangent(br_spline3 spline, float t)
{
    uint32_t segments = br_spline3_segment_count(spline);
    if (segments == 0) {
        return br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    float local;
    br_vec3 p[4];
    br_spline3_segment(spline, br_spline3_locate(segments, t, &local), p);
    br_vec3 d = spline.kind == BR_SPLINE_BEZIER ? br_spline_bezier_cubic_tangent3(p[0], p[1], p[2], p[3], local)
                                                : br_spline_catmull_rom_tangent3(p[0], p[1], p[2], p[3], local);
    return br_vec3_scale(d, (float)segments); /* chain rule: local t moves `segments` times faster */
}

/* ---- arc length */

bool br_spline_arc_table_build(br_spline_arc_table *table, br_spline3 spline, float *lengths, uint32_t count)
{
    if (count < 2 || lengths == NULL) {
        return false;
    }
    table->lengths = lengths;
    table->count = count;
    br_vec3 prev = br_spline3_point(spline, 0.0f);
    lengths[0] = 0.0f;
    for (uint32_t i = 1; i < count; i++) {
        br_vec3 p = br_spline3_point(spline, (float)i / (float)(count - 1));
        lengths[i] = lengths[i - 1] + br_vec3_distance(prev, p); /* chord sum: slightly short on tight bends */
        prev = p;
    }
    table->total = lengths[count - 1];
    return true;
}

float br_spline_arc_param(const br_spline_arc_table *table, float distance)
{
    if (table->count < 2 || table->total <= 0.0f) {
        return 0.0f;
    }
    float d = br_clampf(distance, 0.0f, table->total);
    /* largest i with lengths[i] <= d */
    uint32_t lo = 0;
    uint32_t hi = table->count - 1;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (table->lengths[mid] <= d) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    float span = table->lengths[hi] - table->lengths[lo];
    float frac = span > 0.0f ? (d - table->lengths[lo]) / span : 0.0f;
    return ((float)lo + frac) / (float)(table->count - 1);
}
