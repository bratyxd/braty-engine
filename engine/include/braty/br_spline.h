/* br_spline.h - Bezier, Catmull-Rom and Hermite curves, multi-point paths, arc-length sampling.
 *
 *   br_vec3 pts[] = { ... };                                   // points the path passes through
 *   br_spline3 path = { BR_SPLINE_CATMULL_ROM, pts, 5 };
 *   float lengths[64];                                         // caller-owned table, nothing allocates
 *   br_spline_arc_table table;
 *   br_spline_arc_table_build(&table, path, lengths, 64);
 *   br_vec3 p = br_spline3_point(path, br_spline_arc_param(&table, distance));   // constant speed
 *
 * Curve parameters t run 0..1 and are clamped. Tangents are derivatives with respect to t
 * (not normalised).
 */
#ifndef BR_SPLINE_H
#define BR_SPLINE_H

#include "braty/br_math.h"

/* ---- single segments */

/* Quadratic Bezier (p1 = control point). */
br_vec2 br_spline_bezier_quadratic2(br_vec2 p0, br_vec2 p1, br_vec2 p2, float t);
/* Quadratic Bezier in 3D. */
br_vec3 br_spline_bezier_quadratic3(br_vec3 p0, br_vec3 p1, br_vec3 p2, float t);
/* Derivative of the quadratic Bezier. */
br_vec2 br_spline_bezier_quadratic_tangent2(br_vec2 p0, br_vec2 p1, br_vec2 p2, float t);
/* Derivative of the 3D quadratic Bezier. */
br_vec3 br_spline_bezier_quadratic_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, float t);
/* Cubic Bezier (p1, p2 = control points). */
br_vec2 br_spline_bezier_cubic2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t);
/* Cubic Bezier in 3D. */
br_vec3 br_spline_bezier_cubic3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t);
/* Derivative of the cubic Bezier. */
br_vec2 br_spline_bezier_cubic_tangent2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t);
/* Derivative of the 3D cubic Bezier. */
br_vec3 br_spline_bezier_cubic_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t);
/* Uniform Catmull-Rom between p1 (t = 0) and p2 (t = 1); p0 and p3 shape the ends. */
br_vec2 br_spline_catmull_rom2(br_vec2 p0, br_vec2 p1, br_vec2 p2, br_vec2 p3, float t);
/* Uniform Catmull-Rom in 3D. */
br_vec3 br_spline_catmull_rom3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t);
/* Derivative of the 3D Catmull-Rom segment. */
br_vec3 br_spline_catmull_rom_tangent3(br_vec3 p0, br_vec3 p1, br_vec3 p2, br_vec3 p3, float t);
/* Cubic Hermite: from p0 with tangent m0 to p1 with tangent m1 (glTF CUBICSPLINE shape). */
float br_spline_hermitef(float p0, float m0, float p1, float m1, float t);
/* Cubic Hermite in 2D. */
br_vec2 br_spline_hermite2(br_vec2 p0, br_vec2 m0, br_vec2 p1, br_vec2 m1, float t);
/* Cubic Hermite in 3D. */
br_vec3 br_spline_hermite3(br_vec3 p0, br_vec3 m0, br_vec3 p1, br_vec3 m1, float t);

/* ---- multi-segment paths (3D; use z = 0 for 2D) */

typedef enum br_spline_kind {
    BR_SPLINE_BEZIER,        /* 3n + 1 points: anchor, control, control, anchor, control, ... */
    BR_SPLINE_CATMULL_ROM    /* passes through every point; the end points are repeated as shapers */
} br_spline_kind;

/* A path over borrowed points (not copied, not owned). */
typedef struct br_spline3 {
    br_spline_kind kind;
    const br_vec3 *points;
    uint32_t       count;
} br_spline3;

/* Number of cubic segments in the path (0 if it has too few points). */
uint32_t br_spline3_segment_count(br_spline3 spline);
/* Point at t (0..1 over the whole path, segments get equal t ranges). (0,0,0) for an empty path,
 * the first point when there is less than one segment. */
br_vec3 br_spline3_point(br_spline3 spline, float t);
/* Derivative at t with respect to the whole-path t. */
br_vec3 br_spline3_tangent(br_spline3 spline, float t);

/* ---- arc length: maps a distance along the path to t, for constant-speed motion */

typedef struct br_spline_arc_table {
    float   *lengths;   /* caller storage: lengths[i] = path length up to t = i / (count - 1) */
    uint32_t count;
    float    total;     /* whole path length */
} br_spline_arc_table;

/* Samples the path at `count` evenly spaced t values into the caller's lengths array (no
 * allocation). More samples = more accurate distances (64 per few segments is plenty).
 * False if count < 2. */
bool br_spline_arc_table_build(br_spline_arc_table *table, br_spline3 spline, float *lengths, uint32_t count);
/* t at `distance` along the path (clamped to 0..total; binary search + linear interpolation). */
float br_spline_arc_param(const br_spline_arc_table *table, float distance);

#endif
