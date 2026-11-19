/* 2D shapes: containment, bounds, intersections. Semantics follow PixiJS. */
#include "braty/br_math.h"

#include <SDL3/SDL_stdinc.h>

static float br_minf(float a, float b) { return a < b ? a : b; }
static float br_maxf(float a, float b) { return a > b ? a : b; }

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
