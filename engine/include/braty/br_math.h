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
 * - Results are never NaN for finite inputs: zero vectors, degenerate triangles and parallel
 *   rays return documented fallbacks instead.
 * Also: br_ease.h (easing), br_random.h (PCG32), br_noise.h, br_spline.h.
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
/* True if a and b agree to a relative tolerance: |a - b| <= rel_eps * max(1, |a|, |b|). */
bool br_nearly_equal_rel(float a, float b, float rel_eps);
/* Smaller of a and b. */
float br_minf(float a, float b);
/* Larger of a and b. */
float br_maxf(float a, float b);
/* Clamps v to [0, 1] (HLSL saturate). */
float br_clamp01f(float v);
/* -1, 0 or +1. */
float br_signf(float v);
/* 0 if x < edge, else 1. */
float br_stepf(float edge, float x);
/* x - floor(x), always in [0, 1). */
float br_fractf(float x);
/* Wraps v into [lo, hi) (lo when the range is empty). */
float br_wrapf(float v, float lo, float hi);
/* t such that lerp(a, b, t) == v (0 when a == b). Not clamped. */
float br_inverse_lerpf(float a, float b, float v);
/* Maps v from [in_lo, in_hi] to [out_lo, out_hi]. Not clamped (out_lo when in_lo == in_hi). */
float br_remapf(float v, float in_lo, float in_hi, float out_lo, float out_hi);
/* Hermite 0..1 step between edge0 and edge1 (GLSL smoothstep). */
float br_smoothstepf(float edge0, float edge1, float x);
/* Ken Perlin's smoother 0..1 step (zero 1st and 2nd derivative at the edges). */
float br_smootherstepf(float edge0, float edge1, float x);
/* Moves current towards target by at most max_delta, never past it. */
float br_move_towardsf(float current, float target, float max_delta);
/* Unity SmoothDamp: critically damped follow. *velocity is state kept by the caller (start 0);
 * max_speed <= 0 means unlimited. Never overshoots the target. */
float br_smooth_dampf(float current, float target, float *velocity, float smooth_time, float max_speed, float dt);
/* Wraps an angle into (-pi, pi]. */
float br_angle_wrap(float radians);
/* Shortest signed difference to - from, in (-pi, pi]. */
float br_angle_delta(float from, float to);
/* Interpolates angles along the shortest arc. */
float br_angle_lerp(float from, float to, float t);
/* Moves an angle towards target along the shortest arc by at most max_delta. */
float br_angle_move_towards(float current, float target, float max_delta);

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
/* -v */
br_vec2 br_vec2_negate(br_vec2 v);
/* Component-wise a / b (no zero check). */
br_vec2 br_vec2_div(br_vec2 a, br_vec2 b);
/* Component-wise minimum. */
br_vec2 br_vec2_min(br_vec2 a, br_vec2 b);
/* Component-wise maximum. */
br_vec2 br_vec2_max(br_vec2 a, br_vec2 b);
/* Component-wise absolute value. */
br_vec2 br_vec2_abs(br_vec2 v);
/* Component-wise floor. */
br_vec2 br_vec2_floor(br_vec2 v);
/* Squared length. */
float br_vec2_length_sq(br_vec2 v);
/* Squared distance. */
float br_vec2_distance_sq(br_vec2 a, br_vec2 b);
/* Reflects v off a surface with unit normal n. */
br_vec2 br_vec2_reflect(br_vec2 v, br_vec2 n);
/* Projection of v onto `onto` ((0,0) when onto is zero). */
br_vec2 br_vec2_project(br_vec2 v, br_vec2 onto);
/* Unsigned angle between two vectors in [0, pi] (0 if either is zero). */
float br_vec2_angle(br_vec2 a, br_vec2 b);
/* Signed angle that rotates a onto b, in (-pi, pi] (positive = clockwise on a y-down screen). */
float br_vec2_signed_angle(br_vec2 a, br_vec2 b);
/* Unit vector at `radians` ((1,0) at 0, (0,1) at pi/2). */
br_vec2 br_vec2_from_angle(float radians);
/* Angle of v (atan2(y, x)). */
float br_vec2_to_angle(br_vec2 v);
/* v shortened to at most max_length. */
br_vec2 br_vec2_clamp_length(br_vec2 v, float max_length);
/* Moves current towards target by at most max_distance. */
br_vec2 br_vec2_move_towards(br_vec2 current, br_vec2 target, float max_distance);
/* True if both components are within eps. */
bool br_vec2_nearly_equal(br_vec2 a, br_vec2 b, float eps);

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
/* -v */
br_vec3 br_vec3_negate(br_vec3 v);
/* Component-wise a / b (no zero check). */
br_vec3 br_vec3_div(br_vec3 a, br_vec3 b);
/* Component-wise minimum. */
br_vec3 br_vec3_min(br_vec3 a, br_vec3 b);
/* Component-wise maximum. */
br_vec3 br_vec3_max(br_vec3 a, br_vec3 b);
/* Component-wise absolute value. */
br_vec3 br_vec3_abs(br_vec3 v);
/* Squared length. */
float br_vec3_length_sq(br_vec3 v);
/* Distance between two points. */
float br_vec3_distance(br_vec3 a, br_vec3 b);
/* Squared distance. */
float br_vec3_distance_sq(br_vec3 a, br_vec3 b);
/* Reflects v off a surface with unit normal n. */
br_vec3 br_vec3_reflect(br_vec3 v, br_vec3 n);
/* Refracts unit direction v through unit normal n with ratio eta = n1/n2; (0,0,0) on total internal reflection. */
br_vec3 br_vec3_refract(br_vec3 v, br_vec3 n, float eta);
/* Projection of v onto `onto` ((0,0,0) when onto is zero). */
br_vec3 br_vec3_project(br_vec3 v, br_vec3 onto);
/* v minus its component along unit normal n (projection onto the plane). */
br_vec3 br_vec3_project_on_plane(br_vec3 v, br_vec3 n);
/* Unsigned angle between two vectors in [0, pi] (0 if either is zero). */
float br_vec3_angle(br_vec3 a, br_vec3 b);
/* v shortened to at most max_length. */
br_vec3 br_vec3_clamp_length(br_vec3 v, float max_length);
/* Moves current towards target by at most max_distance. */
br_vec3 br_vec3_move_towards(br_vec3 current, br_vec3 target, float max_distance);
/* A unit vector perpendicular to v ((1,0,0) for a zero vector). */
br_vec3 br_vec3_any_perpendicular(br_vec3 v);
/* Completes unit n to a right-handed orthonormal basis: t x b = n. */
void br_vec3_orthonormal_basis(br_vec3 n, br_vec3 *t, br_vec3 *b);
/* Spherical interpolation of directions (lengths are interpolated linearly). */
br_vec3 br_vec3_slerp(br_vec3 a, br_vec3 b, float t);
/* True if all components are within eps. */
bool br_vec3_nearly_equal(br_vec3 a, br_vec3 b, float eps);

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
/* a - b */
br_vec4 br_vec4_sub(br_vec4 a, br_vec4 b);
/* Component-wise a * b. */
br_vec4 br_vec4_mul(br_vec4 a, br_vec4 b);
/* -v */
br_vec4 br_vec4_negate(br_vec4 v);
/* Component-wise minimum. */
br_vec4 br_vec4_min(br_vec4 a, br_vec4 b);
/* Component-wise maximum. */
br_vec4 br_vec4_max(br_vec4 a, br_vec4 b);
/* Squared length. */
float br_vec4_length_sq(br_vec4 v);
/* Length. */
float br_vec4_length(br_vec4 v);
/* Unit vector, or (0,0,0,0) for a zero vector. */
br_vec4 br_vec4_normalize(br_vec4 v);
/* True if all components are within eps. */
bool br_vec4_nearly_equal(br_vec4 a, br_vec4 b, float eps);

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
/* Determinant. */
float br_mat4_determinant(br_mat4 m);
/* Inverse of an affine matrix (last row 0,0,0,1): cheaper than br_mat4_invert. */
br_mat4 br_mat4_invert_affine(br_mat4 m, bool *ok);
/* Inverse of rotation + translation only (no scale): transposes the rotation. */
br_mat4 br_mat4_invert_rigid(br_mat4 m);
/* Rotation about +X (right-handed: +Y turns towards +Z). */
br_mat4 br_mat4_rotation_x(float radians);
/* Rotation about +Y (+Z turns towards +X). */
br_mat4 br_mat4_rotation_y(float radians);
/* Rotation about +Z (+X turns towards +Y). */
br_mat4 br_mat4_rotation_z(float radians);
/* Inverse-transpose of the upper 3x3 (for transforming normals), in a mat4 with no translation.
 * Identity for a singular matrix. */
br_mat4 br_mat4_normal_matrix(br_mat4 m);
/* Splits an affine TRS matrix into translation, rotation, scale. A negative determinant flips
 * scale.x. False (identity rotation, zero scale) if a scale axis is zero. Shear is lost. */
bool br_mat4_decompose(br_mat4 m, br_vec3 *translation, br_quat *rotation, br_vec3 *scale);
/* Right-handed view matrix at eye looking along direction. */
br_mat4 br_mat4_look_to(br_vec3 eye, br_vec3 direction, br_vec3 up);
/* Off-centre perspective frustum (glFrustum shape, extents on the near plane), right-handed,
 * depth [near, far] -> [0, 1]. */
br_mat4 br_mat4_frustum(float left, float right, float bottom, float top, float near_z, float far_z);
/* Perspective with the far plane at infinity: depth near -> 0, infinity -> 1. */
br_mat4 br_mat4_perspective_infinite(float fov_y, float aspect, float near_z);
/* Reversed-Z perspective: depth near -> 1, far -> 0 (use depth compare GREATER, clear to 0). */
br_mat4 br_mat4_perspective_reversed(float fov_y, float aspect, float near_z, float far_z);
/* Reversed-Z with the far plane at infinity: near -> 1, infinity -> 0. */
br_mat4 br_mat4_perspective_reversed_infinite(float fov_y, float aspect, float near_z);

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
/* Makes a quaternion from raw components (not normalised). */
br_quat br_quat_make(float x, float y, float z, float w);
/* Length. */
float br_quat_length(br_quat q);
/* Inverse of any non-zero quaternion (identity for zero). */
br_quat br_quat_inverse(br_quat q);
/* Normalised linear interpolation along the short path (cheaper than slerp, not constant speed). */
br_quat br_quat_nlerp(br_quat a, br_quat b, float t);
/* Angle in radians [0, pi] between two unit rotations. */
float br_quat_angle(br_quat a, br_quat b);
/* Axis (unit) and angle [0, pi] of a unit quaternion; axis (1,0,0) and angle 0 for identity. */
void br_quat_to_axis_angle(br_quat q, br_vec3 *axis, float *radians);
/* Inverse of br_quat_from_euler (X, then Y, then Z). y is in [-pi/2, pi/2]; at the poles z is 0. */
void br_quat_to_euler(br_quat q, float *x, float *y, float *z);
/* Rotation part of a matrix (scale is removed first). */
br_quat br_quat_from_mat4(br_mat4 m);
/* Shortest-arc rotation turning direction `from` into direction `to` (any lengths; identity if zero). */
br_quat br_quat_from_to(br_vec3 from, br_vec3 to);
/* Rotation whose -Z axis points along forward and whose +Y is as close to up as possible
 * (the engine's right-handed camera convention, like br_mat4_look_at). Identity for zero forward. */
br_quat br_quat_look_rotation(br_vec3 forward, br_vec3 up);
/* Rotates from towards to by at most max_radians (never past it). */
br_quat br_quat_rotate_towards(br_quat from, br_quat to, float max_radians);

/* ---- springs (stable for any dt; state = value + velocity kept by the caller) */

/* Damped spring towards target: frequency in Hz, damping_ratio 1 = critical (no overshoot),
 * < 1 bouncy, > 1 sluggish. Updates *value and *velocity. */
void br_spring_step(float *value, float *velocity, float target, float frequency, float damping_ratio, float dt);
/* br_spring_step for a vec2. */
void br_spring_step_vec2(br_vec2 *value, br_vec2 *velocity, br_vec2 target, float frequency, float damping_ratio, float dt);
/* br_spring_step for a vec3. */
void br_spring_step_vec3(br_vec3 *value, br_vec3 *velocity, br_vec3 target, float frequency, float damping_ratio, float dt);

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

/* ---- 2D separating-axis tests (convex shapes; polygons may wind either way) */

/* True if convex polygons a and b overlap. *mtv (may be NULL) = smallest push that moves a out of b. */
bool br_sat_polygon_polygon(br_polygon a, br_polygon b, br_vec2 *mtv);
/* True if the circle overlaps convex polygon p. *mtv (may be NULL) moves the circle out of p. */
bool br_sat_circle_polygon(br_circle c, br_polygon p, br_vec2 *mtv);
/* Corners of a rotated rectangle (2D oriented box), ready for the SAT functions. */
void br_obb2d_corners(br_vec2 center, br_vec2 half_extents, float rotation, br_vec2 out[4]);

/* ---- 3D primitives and intersections
 *
 * Boxes are passed as (box_min, box_max) and rays as (origin, dir) so these work with
 * br_aabb / br_ray3d (br_view3d.h) without depending on them. Ray results are the ray parameter
 * t (point = origin + dir * t); t is in world units only if dir has unit length. Rays start at
 * t = 0: hits behind the origin are misses, an origin inside a solid gives t = 0.
 */

typedef struct br_plane  { br_vec3 normal; float d; } br_plane;   /* dot(normal, p) + d = 0 */
typedef struct br_sphere { br_vec3 center; float radius; } br_sphere;
typedef struct br_obb    { br_vec3 center; br_vec3 half_extents; br_quat rotation; } br_obb;

/* Plane through point with normal (normalised; a zero normal gives a zero plane). */
br_plane br_plane_from_point_normal(br_vec3 point, br_vec3 normal);
/* Plane through a, b, c; normal = (b-a) x (c-a) normalised (counter-clockwise when facing it).
 * Collinear points give a zero plane. */
br_plane br_plane_from_points(br_vec3 a, br_vec3 b, br_vec3 c);
/* Signed distance from p (positive on the normal side). Exact for a unit normal. */
float br_plane_distance(br_plane plane, br_vec3 p);

/* Ray vs plane (either side). False if parallel or behind the origin. */
bool br_ray_plane(br_vec3 origin, br_vec3 dir, br_plane plane, float *t);
/* Ray vs sphere: entry t (0 if the origin is inside). */
bool br_ray_sphere(br_vec3 origin, br_vec3 dir, br_sphere sphere, float *t);
/* Ray vs axis-aligned box (slab test): entry t (0 if the origin is inside). */
bool br_ray_box(br_vec3 origin, br_vec3 dir, br_vec3 box_min, br_vec3 box_max, float *t);
/* Ray vs oriented box: entry t (0 if the origin is inside). */
bool br_ray_obb(br_vec3 origin, br_vec3 dir, br_obb box, float *t);
/* Ray vs triangle, both sides (Moller-Trumbore). u, v = barycentrics of b and c (may be NULL).
 * Degenerate triangles never hit. */
bool br_ray_triangle(br_vec3 origin, br_vec3 dir, br_vec3 a, br_vec3 b, br_vec3 c, float *t, float *u, float *v);

/* True if the spheres touch or overlap. */
bool br_sphere_sphere(br_sphere a, br_sphere b);
/* True if the sphere touches or overlaps the box. */
bool br_sphere_box(br_sphere s, br_vec3 box_min, br_vec3 box_max);
/* True if the boxes touch or overlap. */
bool br_box_box(br_vec3 a_min, br_vec3 a_max, br_vec3 b_min, br_vec3 b_max);
/* True if the oriented boxes overlap (separating axis test, 15 axes). */
bool br_obb_obb(br_obb a, br_obb b);

/* Six normalised planes (left, right, bottom, top, near, far) of a view-projection matrix with
 * depth 0..1, normals pointing inside. */
void br_frustum_planes(br_mat4 view_projection, br_plane out[6]);
/* True if the sphere is at least partly inside the planes. */
bool br_frustum_planes_sphere(const br_plane planes[6], br_sphere s);
/* True if the box is at least partly inside the planes (conservative: may say true near corners). */
bool br_frustum_planes_box(const br_plane planes[6], br_vec3 box_min, br_vec3 box_max);

/* Closest point to p on segment a-b. */
br_vec3 br_closest_point_segment(br_vec3 p, br_vec3 a, br_vec3 b);
/* Closest point to p on the ray (the origin when p is behind it). */
br_vec3 br_closest_point_ray(br_vec3 p, br_vec3 origin, br_vec3 dir);
/* Closest point to p on the plane (unit normal expected). */
br_vec3 br_closest_point_plane(br_vec3 p, br_plane plane);
/* Closest point to p in or on the box (p itself when inside). */
br_vec3 br_closest_point_box(br_vec3 p, br_vec3 box_min, br_vec3 box_max);
/* Closest point to p in or on the oriented box. */
br_vec3 br_closest_point_obb(br_vec3 p, br_obb box);
/* Closest point to p on triangle a, b, c (degenerate triangles too). */
br_vec3 br_closest_point_triangle(br_vec3 p, br_vec3 a, br_vec3 b, br_vec3 c);
/* Closest points c1 on segment p1-q1 and c2 on p2-q2 (either may be NULL); returns the squared distance. */
float br_closest_points_segments(br_vec3 p1, br_vec3 q1, br_vec3 p2, br_vec3 q2, br_vec3 *c1, br_vec3 *c2);
/* Barycentric weights (wa, wb, wc) of p in triangle a, b, c; false (and (1,0,0)) if degenerate. */
bool br_barycentric(br_vec3 p, br_vec3 a, br_vec3 b, br_vec3 c, br_vec3 *out);

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
/* sRGB-encoded channel (0..1) to linear light. */
float br_srgb_to_linear(float c);
/* Linear light channel to sRGB encoding. */
float br_linear_to_srgb(float c);
/* sRGB colour to linear (alpha unchanged). */
br_color br_color_to_linear(br_color c);
/* Linear colour to sRGB (alpha unchanged). */
br_color br_color_to_srgb(br_color c);
/* From hue (degrees), saturation, value (0..1). */
br_color br_color_from_hsv(float h, float s, float v, float alpha);
/* To hue (degrees, 0..360), saturation, value (0..1). */
void br_color_to_hsv(br_color c, float *h, float *s, float *v);
/* sRGB colour to OKLab (x = L, y = a, z = b), via linear sRGB. Alpha dropped. */
br_vec3 br_color_to_oklab(br_color c);
/* OKLab (x = L, y = a, z = b) to an sRGB colour (not clamped). */
br_color br_color_from_oklab(br_vec3 lab, float alpha);
/* Perceptual interpolation: lerp in OKLab, result back in sRGB; alpha lerped linearly. */
br_color br_color_lerp_oklab(br_color a, br_color b, float t);

#endif
