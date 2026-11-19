/* br_math.h - vectors, matrices, quaternions, 2D shapes, colour.
 *
 * Conventions
 * - 2D: x right, y DOWN (screen space, like PixiJS). Angles in radians, positive = clockwise
 *   on screen.
 * - br_mat3 is a 2D affine matrix with PixiJS Matrix semantics:
 *       | a  c  tx |
 *       | b  d  ty |       x' = a*x + c*y + tx,  y' = b*x + d*y + ty
 * - br_mat4 is column-major (m[column * 4 + row]), vectors are columns: v' = M * v.
 *   Clip space follows SDL_GPU: x,y in [-1,1] with y UP, depth z in [0,1].
 *   3D helpers are right-handed (camera looks down -Z).
 * - All functions take and return values; nothing allocates except br_polygon users.
 */
#ifndef BR_MATH_H
#define BR_MATH_H

#include <stdbool.h>
#include <stdint.h>

#define BR_PI 3.14159265358979323846f
#define BR_TAU 6.28318530717958647692f
#define BR_DEG_TO_RAD (BR_PI / 180.0f)
#define BR_RAD_TO_DEG (180.0f / BR_PI)

typedef struct br_vec2 { float x, y; } br_vec2;
typedef struct br_vec3 { float x, y, z; } br_vec3;
typedef struct br_vec4 { float x, y, z, w; } br_vec4;
typedef struct br_mat3 { float a, b, c, d, tx, ty; } br_mat3;
typedef struct br_mat4 { float m[16]; } br_mat4;
typedef struct br_quat { float x, y, z, w; } br_quat;

/* ---- scalar helpers */

/* Clamps v to [lo, hi]. */
float br_clampf(float v, float lo, float hi);
/* Linear interpolation a + (b - a) * t. */
float br_lerpf(float a, float b, float t);
/* True if |a - b| <= eps. */
bool br_nearly_equal(float a, float b, float eps);

/* ---- br_vec2 */

/* Makes a vec2. */
br_vec2 br_vec2_make(float x, float y);
/* a + b */
br_vec2 br_vec2_add(br_vec2 a, br_vec2 b);
/* a - b */
br_vec2 br_vec2_sub(br_vec2 a, br_vec2 b);
/* v * s */
br_vec2 br_vec2_scale(br_vec2 v, float s);
/* Component-wise a * b. */
br_vec2 br_vec2_mul(br_vec2 a, br_vec2 b);
/* Dot product. */
float br_vec2_dot(br_vec2 a, br_vec2 b);
/* 2D cross product (z of the 3D cross): a.x*b.y - a.y*b.x. */
float br_vec2_cross(br_vec2 a, br_vec2 b);
/* Length. */
float br_vec2_length(br_vec2 v);
/* Distance between two points. */
float br_vec2_distance(br_vec2 a, br_vec2 b);
/* Unit vector, or (0,0) for a zero vector. */
br_vec2 br_vec2_normalize(br_vec2 v);
/* Linear interpolation. */
br_vec2 br_vec2_lerp(br_vec2 a, br_vec2 b, float t);
/* Rotates by `radians` (clockwise on a y-down screen). */
br_vec2 br_vec2_rotate(br_vec2 v, float radians);
/* Perpendicular (-y, x). */
br_vec2 br_vec2_perp(br_vec2 v);

/* ---- br_vec3 */

/* Makes a vec3. */
br_vec3 br_vec3_make(float x, float y, float z);
/* a + b */
br_vec3 br_vec3_add(br_vec3 a, br_vec3 b);
/* a - b */
br_vec3 br_vec3_sub(br_vec3 a, br_vec3 b);
/* v * s */
br_vec3 br_vec3_scale(br_vec3 v, float s);
/* Component-wise a * b. */
br_vec3 br_vec3_mul(br_vec3 a, br_vec3 b);
/* Dot product. */
float br_vec3_dot(br_vec3 a, br_vec3 b);
/* Cross product. */
br_vec3 br_vec3_cross(br_vec3 a, br_vec3 b);
/* Length. */
float br_vec3_length(br_vec3 v);
/* Unit vector, or (0,0,0) for a zero vector. */
br_vec3 br_vec3_normalize(br_vec3 v);
/* Linear interpolation. */
br_vec3 br_vec3_lerp(br_vec3 a, br_vec3 b, float t);

/* ---- br_vec4 */

/* Makes a vec4. */
br_vec4 br_vec4_make(float x, float y, float z, float w);
/* a + b */
br_vec4 br_vec4_add(br_vec4 a, br_vec4 b);
/* v * s */
br_vec4 br_vec4_scale(br_vec4 v, float s);
/* Dot product. */
float br_vec4_dot(br_vec4 a, br_vec4 b);
/* Linear interpolation. */
br_vec4 br_vec4_lerp(br_vec4 a, br_vec4 b, float t);

/* ---- br_mat3 (2D affine, PixiJS Matrix) */

/* Identity matrix. */
br_mat3 br_mat3_identity(void);
/* Translation matrix. */
br_mat3 br_mat3_translation(float x, float y);
/* Scale matrix. */
br_mat3 br_mat3_scaling(float sx, float sy);
/* Rotation matrix. */
br_mat3 br_mat3_rotation(float radians);
/* Returns m * other: apply `other` first, then `m` (Pixi m.append(other)). */
br_mat3 br_mat3_mul(br_mat3 m, br_mat3 other);
/* Returns m translated by (x, y) after its existing transform (Pixi m.translate). */
br_mat3 br_mat3_translate(br_mat3 m, float x, float y);
/* Returns m scaled after its existing transform (Pixi m.scale). */
br_mat3 br_mat3_scale(br_mat3 m, float sx, float sy);
/* Returns m rotated after its existing transform (Pixi m.rotate). */
br_mat3 br_mat3_rotate(br_mat3 m, float radians);
/* Inverse. A singular matrix returns identity and false in *ok (ok may be NULL). */
br_mat3 br_mat3_invert(br_mat3 m, bool *ok);
/* Transforms a point. */
br_vec2 br_mat3_apply(br_mat3 m, br_vec2 p);
/* Transforms a point by the inverse matrix. */
br_vec2 br_mat3_apply_inverse(br_mat3 m, br_vec2 p);
/* Builds a node transform like Pixi Matrix.setTransform: translate(x,y) * rotate/skew * scale * translate(-pivot). */
br_mat3 br_mat3_from_transform(float x, float y, float pivot_x, float pivot_y, float scale_x,
                               float scale_y, float rotation, float skew_x, float skew_y);
/* Splits into position, scale, rotation, skew (Pixi Matrix.decompose; pivot is assumed 0). */
void br_mat3_decompose(br_mat3 m, br_vec2 *position, br_vec2 *scale, float *rotation, br_vec2 *skew);
/* True if all six entries are within eps. */
bool br_mat3_nearly_equal(br_mat3 a, br_mat3 b, float eps);

/* ---- br_mat4 (column-major) */

/* Identity matrix. */
br_mat4 br_mat4_identity(void);
/* Returns a * b (apply b first). */
br_mat4 br_mat4_mul(br_mat4 a, br_mat4 b);
/* Translation matrix. */
br_mat4 br_mat4_translation(br_vec3 t);
/* Scale matrix. */
br_mat4 br_mat4_scaling(br_vec3 s);
/* Rotation matrix about a unit axis. */
br_mat4 br_mat4_rotation(br_vec3 axis, float radians);
/* Rotation matrix from a unit quaternion. */
br_mat4 br_mat4_from_quat(br_quat q);
/* translation * rotation * scale. */
br_mat4 br_mat4_from_trs(br_vec3 t, br_quat r, br_vec3 s);
/* Embeds a 2D affine br_mat3 (z untouched). */
br_mat4 br_mat4_from_mat3(br_mat3 m);
/* Transpose. */
br_mat4 br_mat4_transpose(br_mat4 m);
/* Inverse. A singular matrix returns identity and false in *ok (ok may be NULL). */
br_mat4 br_mat4_invert(br_mat4 m, bool *ok);
/* Transforms a point (w = 1, divides by w). */
br_vec3 br_mat4_transform_point(br_mat4 m, br_vec3 p);
/* Transforms a direction (w = 0). */
br_vec3 br_mat4_transform_dir(br_mat4 m, br_vec3 d);
/* Transforms a vec4. */
br_vec4 br_mat4_transform_vec4(br_mat4 m, br_vec4 v);
/* Orthographic projection, depth [near, far] -> [0, 1]. For 2D screen space use
 * br_mat4_ortho(0, width, height, 0, -1, 1) so (0,0) is the top-left pixel. */
br_mat4 br_mat4_ortho(float left, float right, float bottom, float top, float near_z, float far_z);
/* Right-handed perspective projection, vertical fov in radians, depth -> [0, 1]. */
br_mat4 br_mat4_perspective(float fov_y, float aspect, float near_z, float far_z);
/* Right-handed view matrix looking from eye to target. */
br_mat4 br_mat4_look_at(br_vec3 eye, br_vec3 target, br_vec3 up);
/* True if all entries are within eps. */
bool br_mat4_nearly_equal(br_mat4 a, br_mat4 b, float eps);

/* ---- br_quat */

/* Identity rotation. */
br_quat br_quat_identity(void);
/* Rotation about a unit axis. */
br_quat br_quat_from_axis_angle(br_vec3 axis, float radians);
/* Rotation from Euler angles applied in order X, then Y, then Z (radians). */
br_quat br_quat_from_euler(float x, float y, float z);
/* Returns a * b (rotate by b first, then a). */
br_quat br_quat_mul(br_quat a, br_quat b);
/* Unit quaternion (identity for zero). */
br_quat br_quat_normalize(br_quat q);
/* Inverse of a unit quaternion. */
br_quat br_quat_conjugate(br_quat q);
/* Shortest-path spherical interpolation. */
br_quat br_quat_slerp(br_quat a, br_quat b, float t);
/* Rotates a vector. */
br_vec3 br_quat_rotate(br_quat q, br_vec3 v);
/* Dot product. */
float br_quat_dot(br_quat a, br_quat b);

/* ---- 2D shapes (PixiJS Rectangle, Circle, Ellipse, RoundedRectangle, Triangle, Polygon) */

typedef struct br_rect         { float x, y, width, height; } br_rect;
typedef struct br_circle       { float x, y, radius; } br_circle;
typedef struct br_ellipse      { float x, y, half_width, half_height; } br_ellipse;
typedef struct br_rounded_rect { float x, y, width, height, radius; } br_rounded_rect;
typedef struct br_triangle     { float x1, y1, x2, y2, x3, y3; } br_triangle;
/* Points are borrowed, not owned. Always treated as closed for contains/bounds. */
typedef struct br_polygon      { const br_vec2 *points; uint32_t count; } br_polygon;

/* Makes a rect. */
br_rect br_rect_make(float x, float y, float width, float height);
/* True if (x,y) is inside (left/top edges inclusive, right/bottom exclusive, like Pixi). */
bool br_rect_contains(br_rect r, float x, float y);
/* True if the rects overlap with non-zero area. */
bool br_rect_intersects(br_rect a, br_rect b);
/* Overlap of two rects (zero-size rect if none). */
br_rect br_rect_intersection(br_rect a, br_rect b);
/* Smallest rect containing both. */
br_rect br_rect_union(br_rect a, br_rect b);
/* Grows by `pad` on every side. */
br_rect br_rect_pad(br_rect r, float pad);
/* Axis-aligned bounds of the rect's four corners after transform `m`. */
br_rect br_rect_transform_bounds(br_rect r, br_mat3 m);
/* True if width or height is <= 0. */
bool br_rect_is_empty(br_rect r);

/* True if (x,y) is inside the circle. */
bool br_circle_contains(br_circle c, float x, float y);
/* Bounding rect. */
br_rect br_circle_bounds(br_circle c);
/* True if (x,y) is inside the ellipse. */
bool br_ellipse_contains(br_ellipse e, float x, float y);
/* Bounding rect. */
br_rect br_ellipse_bounds(br_ellipse e);
/* True if (x,y) is inside the rounded rect. */
bool br_rounded_rect_contains(br_rounded_rect r, float x, float y);
/* Bounding rect. */
br_rect br_rounded_rect_bounds(br_rounded_rect r);
/* True if (x,y) is inside the triangle (either winding). */
bool br_triangle_contains(br_triangle t, float x, float y);
/* Bounding rect. */
br_rect br_triangle_bounds(br_triangle t);
/* True if (x,y) is inside the polygon (even-odd rule). */
bool br_polygon_contains(br_polygon p, float x, float y);
/* Bounding rect (zero rect for an empty polygon). */
br_rect br_polygon_bounds(br_polygon p);
/* Signed area (positive for clockwise on a y-down screen). */
float br_polygon_signed_area(br_polygon p);

/* Intersection of segments a0-a1 and b0-b1. Writes the point to *out (may be NULL). */
bool br_segment_intersect(br_vec2 a0, br_vec2 a1, br_vec2 b0, br_vec2 b1, br_vec2 *out);
/* Intersection of the infinite lines through a0-a1 and b0-b1. False if parallel. */
bool br_line_intersect(br_vec2 a0, br_vec2 a1, br_vec2 b0, br_vec2 b1, br_vec2 *out);
/* Distance from p to the segment a-b. */
float br_segment_distance(br_vec2 p, br_vec2 a, br_vec2 b);

/* ---- br_color: straight (not premultiplied) RGBA, floats in [0, 1] */

typedef struct br_color { float r, g, b, a; } br_color;

/* Makes a colour from floats. */
br_color br_color_make(float r, float g, float b, float a);
/* From 0xRRGGBB and an alpha. */
br_color br_color_from_hex(uint32_t rgb, float alpha);
/* From 0xRRGGBBAA. */
br_color br_color_from_hex_rgba(uint32_t rgba);
/* To 0xRRGGBB (alpha dropped). */
uint32_t br_color_to_hex(br_color c);
/* Packs into 4 bytes R,G,B,A in memory order (for UBYTE4_NORM vertex colours). */
uint32_t br_color_to_rgba8(br_color c);
/* From hue (degrees), saturation, lightness (0..1). */
br_color br_color_from_hsl(float h, float s, float l, float alpha);
/* To hue (degrees), saturation, lightness (0..1). */
void br_color_to_hsl(br_color c, float *h, float *s, float *l);
/* Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa", "0xrrggbb", "rgb(r,g,b)", "rgba(r,g,b,a)" or a CSS name. */
bool br_color_parse(const char *text, br_color *out);
/* rgb * a, alpha kept. */
br_color br_color_premultiply(br_color c);
/* Component-wise multiply (tinting). */
br_color br_color_multiply(br_color a, br_color b);
/* Linear interpolation. */
br_color br_color_lerp(br_color a, br_color b, float t);

#endif
