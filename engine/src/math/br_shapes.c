/* 2D shapes: containment, bounds, intersections. Semantics follow PixiJS. */
#include "braty/br_math.h"

#include <SDL3/SDL_stdinc.h>

br_rect br_rect_make(float x, float y, float width, float height)
{
    br_rect r = { x, y, width, height };
    return r;
}

bool br_rect_contains(br_rect r, float x, float y)
{
    if (r.width <= 0.0f || r.height <= 0.0f) {
        return false;
    }
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

bool br_rect_intersects(br_rect a, br_rect b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

br_rect br_rect_intersection(br_rect a, br_rect b)
{
    float x0 = br_maxf(a.x, b.x);
    float y0 = br_maxf(a.y, b.y);
    float x1 = br_minf(a.x + a.width, b.x + b.width);
    float y1 = br_minf(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) {
        return br_rect_make(x0, y0, 0.0f, 0.0f);
    }
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

br_rect br_rect_union(br_rect a, br_rect b)
{
    float x0 = br_minf(a.x, b.x);
    float y0 = br_minf(a.y, b.y);
    float x1 = br_maxf(a.x + a.width, b.x + b.width);
    float y1 = br_maxf(a.y + a.height, b.y + b.height);
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

br_rect br_rect_pad(br_rect r, float pad)
{
    return br_rect_make(r.x - pad, r.y - pad, r.width + 2.0f * pad, r.height + 2.0f * pad);
}

br_rect br_rect_transform_bounds(br_rect r, br_mat3 m)
{
    br_vec2 p0 = br_mat3_apply(m, br_vec2_make(r.x, r.y));
    br_vec2 p1 = br_mat3_apply(m, br_vec2_make(r.x + r.width, r.y));
    br_vec2 p2 = br_mat3_apply(m, br_vec2_make(r.x + r.width, r.y + r.height));
    br_vec2 p3 = br_mat3_apply(m, br_vec2_make(r.x, r.y + r.height));
    float x0 = br_minf(br_minf(p0.x, p1.x), br_minf(p2.x, p3.x));
    float y0 = br_minf(br_minf(p0.y, p1.y), br_minf(p2.y, p3.y));
    float x1 = br_maxf(br_maxf(p0.x, p1.x), br_maxf(p2.x, p3.x));
    float y1 = br_maxf(br_maxf(p0.y, p1.y), br_maxf(p2.y, p3.y));
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool br_rect_is_empty(br_rect r)
{
    return r.width <= 0.0f || r.height <= 0.0f;
}

bool br_circle_contains(br_circle c, float x, float y)
{
    if (c.radius <= 0.0f) {
        return false;
    }
    float dx = x - c.x;
    float dy = y - c.y;
    return dx * dx + dy * dy <= c.radius * c.radius;
}

br_rect br_circle_bounds(br_circle c)
{
    return br_rect_make(c.x - c.radius, c.y - c.radius, c.radius * 2.0f, c.radius * 2.0f);
}

bool br_ellipse_contains(br_ellipse e, float x, float y)
{
    if (e.half_width <= 0.0f || e.half_height <= 0.0f) {
        return false;
    }
    float nx = (x - e.x) / e.half_width;
    float ny = (y - e.y) / e.half_height;
    return nx * nx + ny * ny <= 1.0f;
}

br_rect br_ellipse_bounds(br_ellipse e)
{
    return br_rect_make(e.x - e.half_width, e.y - e.half_height, e.half_width * 2.0f, e.half_height * 2.0f);
}

bool br_rounded_rect_contains(br_rounded_rect r, float x, float y)
{
    if (r.width <= 0.0f || r.height <= 0.0f) {
        return false;
    }
    if (x < r.x || x > r.x + r.width || y < r.y || y > r.y + r.height) {
        return false;
    }
    float radius = br_clampf(r.radius, 0.0f, br_minf(r.width, r.height) * 0.5f);
    /* inside the cross formed by the two inner rects: no corner test needed */
    if ((y >= r.y + radius && y <= r.y + r.height - radius) ||
        (x >= r.x + radius && x <= r.x + r.width - radius)) {
        return true;
    }
    /* in a corner square: test against that corner's circle */
    float cx = x < r.x + radius ? r.x + radius : r.x + r.width - radius;
    float cy = y < r.y + radius ? r.y + radius : r.y + r.height - radius;
    float dx = x - cx;
    float dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

br_rect br_rounded_rect_bounds(br_rounded_rect r)
{
    return br_rect_make(r.x, r.y, r.width, r.height);
}

bool br_triangle_contains(br_triangle t, float x, float y)
{
    /* a zero-area triangle has all cross products 0 and would "contain" its whole line */
    float area2 = (t.x2 - t.x1) * (t.y3 - t.y1) - (t.x3 - t.x1) * (t.y2 - t.y1);
    if (area2 == 0.0f) {
        return false;
    }
    /* same sign of all three edge cross products = inside, for either winding */
    float d1 = (x - t.x2) * (t.y1 - t.y2) - (t.x1 - t.x2) * (y - t.y2);
    float d2 = (x - t.x3) * (t.y2 - t.y3) - (t.x2 - t.x3) * (y - t.y3);
    float d3 = (x - t.x1) * (t.y3 - t.y1) - (t.x3 - t.x1) * (y - t.y1);
    bool has_neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    bool has_pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(has_neg && has_pos);
}

br_rect br_triangle_bounds(br_triangle t)
{
    float x0 = br_minf(t.x1, br_minf(t.x2, t.x3));
    float y0 = br_minf(t.y1, br_minf(t.y2, t.y3));
    float x1 = br_maxf(t.x1, br_maxf(t.x2, t.x3));
    float y1 = br_maxf(t.y1, br_maxf(t.y2, t.y3));
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool br_polygon_contains(br_polygon p, float x, float y)
{
    bool inside = false;
    for (uint32_t i = 0, j = p.count - 1; i < p.count; j = i, i++) {
        br_vec2 a = p.points[i];
        br_vec2 b = p.points[j];
        bool crosses = (a.y > y) != (b.y > y);
        if (crosses && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

br_rect br_polygon_bounds(br_polygon p)
{
    if (p.count == 0) {
        return br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float x0 = p.points[0].x, y0 = p.points[0].y, x1 = x0, y1 = y0;
    for (uint32_t i = 1; i < p.count; i++) {
        x0 = br_minf(x0, p.points[i].x);
        y0 = br_minf(y0, p.points[i].y);
        x1 = br_maxf(x1, p.points[i].x);
        y1 = br_maxf(y1, p.points[i].y);
    }
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

float br_polygon_signed_area(br_polygon p)
{
    float sum = 0.0f;
    for (uint32_t i = 0, j = p.count - 1; i < p.count; j = i, i++) {
        sum += br_vec2_cross(p.points[j], p.points[i]);
    }
    return sum * 0.5f;
}

bool br_line_intersect(br_vec2 a0, br_vec2 a1, br_vec2 b0, br_vec2 b1, br_vec2 *out)
{
    br_vec2 r = br_vec2_sub(a1, a0);
    br_vec2 s = br_vec2_sub(b1, b0);
    float denom = br_vec2_cross(r, s);
    if (denom == 0.0f) {
        return false;
    }
    float t = br_vec2_cross(br_vec2_sub(b0, a0), s) / denom;
    if (out != NULL) {
        *out = br_vec2_add(a0, br_vec2_scale(r, t));
    }
    return true;
}

bool br_segment_intersect(br_vec2 a0, br_vec2 a1, br_vec2 b0, br_vec2 b1, br_vec2 *out)
{
    br_vec2 r = br_vec2_sub(a1, a0);
    br_vec2 s = br_vec2_sub(b1, b0);
    float denom = br_vec2_cross(r, s);
    if (denom == 0.0f) {
        return false; // ponytail: collinear overlapping segments report no intersection; add an overlap test if a caller needs it.
    }
    br_vec2 ab = br_vec2_sub(b0, a0);
    float t = br_vec2_cross(ab, s) / denom;
    float u = br_vec2_cross(ab, r) / denom;
    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) {
        return false;
    }
    if (out != NULL) {
        *out = br_vec2_add(a0, br_vec2_scale(r, t));
    }
    return true;
}

float br_segment_distance(br_vec2 p, br_vec2 a, br_vec2 b)
{
    br_vec2 ab = br_vec2_sub(b, a);
    float len2 = br_vec2_dot(ab, ab);
    float t = len2 > 0.0f ? br_clampf(br_vec2_dot(br_vec2_sub(p, a), ab) / len2, 0.0f, 1.0f) : 0.0f;
    return br_vec2_distance(p, br_vec2_add(a, br_vec2_scale(ab, t)));
}

/* ---- separating-axis tests */

/* Projects the polygon's points on axis into [*lo, *hi]. */
static void br_sat_project(br_polygon p, br_vec2 axis, float *lo, float *hi)
{
    *lo = br_vec2_dot(p.points[0], axis);
    *hi = *lo;
    for (uint32_t i = 1; i < p.count; i++) {
        float d = br_vec2_dot(p.points[i], axis);
        *lo = br_minf(*lo, d);
        *hi = br_maxf(*hi, d);
    }
}

static br_vec2 br_sat_centroid(br_polygon p)
{
    br_vec2 sum = br_vec2_make(0.0f, 0.0f);
    for (uint32_t i = 0; i < p.count; i++) {
        sum = br_vec2_add(sum, p.points[i]);
    }
    return br_vec2_scale(sum, 1.0f / (float)p.count);
}

/* Tests one axis; keeps the smallest overlap in *best / *best_axis. False if it separates. */
static bool br_sat_axis(br_vec2 axis, float a_lo, float a_hi, float b_lo, float b_hi, float *best, br_vec2 *best_axis)
{
    float overlap = br_minf(a_hi, b_hi) - br_maxf(a_lo, b_lo);
    if (overlap <= 0.0f) {
        return false;
    }
    if (overlap < *best) {
        *best = overlap;
        *best_axis = axis;
    }
    return true;
}

/* Tests every edge normal of `edges` against polygons a and b. */
static bool br_sat_edges(br_polygon edges, br_polygon a, br_polygon b, float *best, br_vec2 *best_axis)
{
    for (uint32_t i = 0, j = edges.count - 1; i < edges.count; j = i, i++) {
        br_vec2 axis = br_vec2_normalize(br_vec2_perp(br_vec2_sub(edges.points[i], edges.points[j])));
        if (br_vec2_length_sq(axis) == 0.0f) {
            continue; /* repeated point */
        }
        float a_lo, a_hi, b_lo, b_hi;
        br_sat_project(a, axis, &a_lo, &a_hi);
        br_sat_project(b, axis, &b_lo, &b_hi);
        if (!br_sat_axis(axis, a_lo, a_hi, b_lo, b_hi, best, best_axis)) {
            return false;
        }
    }
    return true;
}

bool br_sat_polygon_polygon(br_polygon a, br_polygon b, br_vec2 *mtv)
{
    if (a.count == 0 || b.count == 0) {
        return false;
    }
    float best = 3.402823466e+38f;
    br_vec2 axis = br_vec2_make(0.0f, 0.0f);
    if (!br_sat_edges(a, a, b, &best, &axis) || !br_sat_edges(b, a, b, &best, &axis)) {
        return false;
    }
    if (mtv != NULL) {
        if (br_vec2_dot(br_vec2_sub(br_sat_centroid(a), br_sat_centroid(b)), axis) < 0.0f) {
            axis = br_vec2_negate(axis); /* push a away from b */
        }
        *mtv = br_vec2_scale(axis, best);
    }
    return true;
}

bool br_sat_circle_polygon(br_circle c, br_polygon p, br_vec2 *mtv)
{
    if (p.count == 0) {
        return false;
    }
    br_vec2 center = br_vec2_make(c.x, c.y);
    float best = 3.402823466e+38f;
    br_vec2 best_axis = br_vec2_make(0.0f, 0.0f);
    /* candidate axes: every edge normal, plus the axis from the nearest vertex to the centre */
    br_vec2 nearest = p.points[0];
    for (uint32_t i = 1; i < p.count; i++) {
        if (br_vec2_distance_sq(p.points[i], center) < br_vec2_distance_sq(nearest, center)) {
            nearest = p.points[i];
        }
    }
    uint32_t axis_count = p.count + 1;
    for (uint32_t k = 0; k < axis_count; k++) {
        br_vec2 axis;
        if (k < p.count) {
            uint32_t j = k == 0 ? p.count - 1 : k - 1;
            axis = br_vec2_perp(br_vec2_sub(p.points[k], p.points[j]));
        } else {
            axis = br_vec2_sub(center, nearest);
        }
        axis = br_vec2_normalize(axis);
        if (br_vec2_length_sq(axis) == 0.0f) {
            continue;
        }
        float p_lo, p_hi;
        br_sat_project(p, axis, &p_lo, &p_hi);
        float d = br_vec2_dot(center, axis);
        if (!br_sat_axis(axis, d - c.radius, d + c.radius, p_lo, p_hi, &best, &best_axis)) {
            return false;
        }
    }
    if (best_axis.x == 0.0f && best_axis.y == 0.0f) {
        return false; /* no usable axis (all points equal and centred on them) */
    }
    if (mtv != NULL) {
        if (br_vec2_dot(br_vec2_sub(center, br_sat_centroid(p)), best_axis) < 0.0f) {
            best_axis = br_vec2_negate(best_axis);
        }
        *mtv = br_vec2_scale(best_axis, best);
    }
    return true;
}

void br_obb2d_corners(br_vec2 center, br_vec2 half_extents, float rotation, br_vec2 out[4])
{
    br_vec2 local[4] = { { -half_extents.x, -half_extents.y }, { half_extents.x, -half_extents.y },
                         { half_extents.x, half_extents.y }, { -half_extents.x, half_extents.y } };
    for (int i = 0; i < 4; i++) {
        out[i] = br_vec2_add(center, br_vec2_rotate(local[i], rotation));
    }
}
