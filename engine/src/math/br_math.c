/* Vectors, matrices, quaternions. See br_math.h for the conventions. */
#include "braty/br_math.h"

#include <SDL3/SDL_stdinc.h>

float br_clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

float br_lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

bool br_nearly_equal(float a, float b, float eps)
{
    return SDL_fabsf(a - b) <= eps;
}

/* ---- vec2 */

br_vec2 br_vec2_make(float x, float y)
{
    br_vec2 v = { x, y };
    return v;
}

br_vec2 br_vec2_add(br_vec2 a, br_vec2 b) { return br_vec2_make(a.x + b.x, a.y + b.y); }
br_vec2 br_vec2_sub(br_vec2 a, br_vec2 b) { return br_vec2_make(a.x - b.x, a.y - b.y); }
br_vec2 br_vec2_scale(br_vec2 v, float s) { return br_vec2_make(v.x * s, v.y * s); }
br_vec2 br_vec2_mul(br_vec2 a, br_vec2 b) { return br_vec2_make(a.x * b.x, a.y * b.y); }
float br_vec2_dot(br_vec2 a, br_vec2 b) { return a.x * b.x + a.y * b.y; }
float br_vec2_cross(br_vec2 a, br_vec2 b) { return a.x * b.y - a.y * b.x; }
float br_vec2_length(br_vec2 v) { return SDL_sqrtf(v.x * v.x + v.y * v.y); }
float br_vec2_distance(br_vec2 a, br_vec2 b) { return br_vec2_length(br_vec2_sub(a, b)); }
br_vec2 br_vec2_perp(br_vec2 v) { return br_vec2_make(-v.y, v.x); }

br_vec2 br_vec2_normalize(br_vec2 v)
{
    float len = br_vec2_length(v);
    if (len == 0.0f) {
        return br_vec2_make(0.0f, 0.0f);
    }
    return br_vec2_scale(v, 1.0f / len);
}

br_vec2 br_vec2_lerp(br_vec2 a, br_vec2 b, float t)
{
    return br_vec2_make(br_lerpf(a.x, b.x, t), br_lerpf(a.y, b.y, t));
}

br_vec2 br_vec2_rotate(br_vec2 v, float radians)
{
    float c = SDL_cosf(radians);
    float s = SDL_sinf(radians);
    return br_vec2_make(v.x * c - v.y * s, v.x * s + v.y * c);
}

/* ---- vec3 */

br_vec3 br_vec3_make(float x, float y, float z)
{
    br_vec3 v = { x, y, z };
    return v;
}

br_vec3 br_vec3_add(br_vec3 a, br_vec3 b) { return br_vec3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
br_vec3 br_vec3_sub(br_vec3 a, br_vec3 b) { return br_vec3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
br_vec3 br_vec3_scale(br_vec3 v, float s) { return br_vec3_make(v.x * s, v.y * s, v.z * s); }
br_vec3 br_vec3_mul(br_vec3 a, br_vec3 b) { return br_vec3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
float br_vec3_dot(br_vec3 a, br_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float br_vec3_length(br_vec3 v) { return SDL_sqrtf(br_vec3_dot(v, v)); }

br_vec3 br_vec3_cross(br_vec3 a, br_vec3 b)
{
    return br_vec3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

br_vec3 br_vec3_normalize(br_vec3 v)
{
    float len = br_vec3_length(v);
    if (len == 0.0f) {
        return br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    return br_vec3_scale(v, 1.0f / len);
}

br_vec3 br_vec3_lerp(br_vec3 a, br_vec3 b, float t)
{
    return br_vec3_make(br_lerpf(a.x, b.x, t), br_lerpf(a.y, b.y, t), br_lerpf(a.z, b.z, t));
}

/* ---- vec4 */

br_vec4 br_vec4_make(float x, float y, float z, float w)
{
    br_vec4 v = { x, y, z, w };
    return v;
}

br_vec4 br_vec4_add(br_vec4 a, br_vec4 b) { return br_vec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
br_vec4 br_vec4_scale(br_vec4 v, float s) { return br_vec4_make(v.x * s, v.y * s, v.z * s, v.w * s); }
float br_vec4_dot(br_vec4 a, br_vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

br_vec4 br_vec4_lerp(br_vec4 a, br_vec4 b, float t)
{
    return br_vec4_make(br_lerpf(a.x, b.x, t), br_lerpf(a.y, b.y, t), br_lerpf(a.z, b.z, t),
                        br_lerpf(a.w, b.w, t));
}

/* ---- mat3 */

static br_mat3 br_mat3_make(float a, float b, float c, float d, float tx, float ty)
{
    br_mat3 m = { a, b, c, d, tx, ty };
    return m;
}

br_mat3 br_mat3_identity(void) { return br_mat3_make(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f); }
br_mat3 br_mat3_translation(float x, float y) { return br_mat3_make(1.0f, 0.0f, 0.0f, 1.0f, x, y); }
br_mat3 br_mat3_scaling(float sx, float sy) { return br_mat3_make(sx, 0.0f, 0.0f, sy, 0.0f, 0.0f); }

br_mat3 br_mat3_rotation(float radians)
{
    float c = SDL_cosf(radians);
    float s = SDL_sinf(radians);
    return br_mat3_make(c, s, -s, c, 0.0f, 0.0f);
}

br_mat3 br_mat3_mul(br_mat3 m, br_mat3 o)
{
    return br_mat3_make(m.a * o.a + m.c * o.b,
                        m.b * o.a + m.d * o.b,
                        m.a * o.c + m.c * o.d,
                        m.b * o.c + m.d * o.d,
                        m.a * o.tx + m.c * o.ty + m.tx,
                        m.b * o.tx + m.d * o.ty + m.ty);
}

br_mat3 br_mat3_translate(br_mat3 m, float x, float y)
{
    return br_mat3_mul(br_mat3_translation(x, y), m);
}

br_mat3 br_mat3_scale(br_mat3 m, float sx, float sy)
{
    return br_mat3_mul(br_mat3_scaling(sx, sy), m);
}

br_mat3 br_mat3_rotate(br_mat3 m, float radians)
{
    return br_mat3_mul(br_mat3_rotation(radians), m);
}

br_mat3 br_mat3_invert(br_mat3 m, bool *ok)
{
    float det = m.a * m.d - m.b * m.c;
    if (det == 0.0f) {
        if (ok != NULL) {
            *ok = false;
        }
        return br_mat3_identity();
    }
    if (ok != NULL) {
        *ok = true;
    }
    float inv = 1.0f / det;
    return br_mat3_make(m.d * inv,
                        -m.b * inv,
                        -m.c * inv,
                        m.a * inv,
                        (m.c * m.ty - m.d * m.tx) * inv,
                        (m.b * m.tx - m.a * m.ty) * inv);
}

br_vec2 br_mat3_apply(br_mat3 m, br_vec2 p)
{
    return br_vec2_make(m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty);
}

br_vec2 br_mat3_apply_inverse(br_mat3 m, br_vec2 p)
{
    return br_mat3_apply(br_mat3_invert(m, NULL), p);
}

br_mat3 br_mat3_from_transform(float x, float y, float pivot_x, float pivot_y, float scale_x,
                               float scale_y, float rotation, float skew_x, float skew_y)
{
    br_mat3 m;
    m.a = SDL_cosf(rotation + skew_y) * scale_x;
    m.b = SDL_sinf(rotation + skew_y) * scale_x;
    m.c = -SDL_sinf(rotation - skew_x) * scale_y;
    m.d = SDL_cosf(rotation - skew_x) * scale_y;
    m.tx = x - (pivot_x * m.a + pivot_y * m.c);
    m.ty = y - (pivot_x * m.b + pivot_y * m.d);
    return m;
}

void br_mat3_decompose(br_mat3 m, br_vec2 *position, br_vec2 *scale, float *rotation, br_vec2 *skew)
{
    float skew_x = -SDL_atan2f(-m.c, m.d);
    float skew_y = SDL_atan2f(m.b, m.a);
    float delta = SDL_fabsf(skew_x + skew_y);
    /* equal skews (or opposite by pi) mean a pure rotation */
    if (delta < 0.00001f || SDL_fabsf(BR_TAU - delta) < 0.00001f) {
        *rotation = skew_y;
        *skew = br_vec2_make(0.0f, 0.0f);
    } else {
        *rotation = 0.0f;
        *skew = br_vec2_make(skew_x, skew_y);
    }
    *scale = br_vec2_make(SDL_sqrtf(m.a * m.a + m.b * m.b), SDL_sqrtf(m.c * m.c + m.d * m.d));
    *position = br_vec2_make(m.tx, m.ty);
}

bool br_mat3_nearly_equal(br_mat3 a, br_mat3 b, float eps)
{
    return br_nearly_equal(a.a, b.a, eps) && br_nearly_equal(a.b, b.b, eps) &&
           br_nearly_equal(a.c, b.c, eps) && br_nearly_equal(a.d, b.d, eps) &&
           br_nearly_equal(a.tx, b.tx, eps) && br_nearly_equal(a.ty, b.ty, eps);
}

/* ---- mat4 */

br_mat4 br_mat4_identity(void)
{
    br_mat4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}

br_mat4 br_mat4_mul(br_mat4 a, br_mat4 b)
{
    br_mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

br_mat4 br_mat4_translation(br_vec3 t)
{
    br_mat4 r = br_mat4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

br_mat4 br_mat4_scaling(br_vec3 s)
{
    br_mat4 r = br_mat4_identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

br_mat4 br_mat4_rotation(br_vec3 axis, float radians)
{
    return br_mat4_from_quat(br_quat_from_axis_angle(axis, radians));
}

br_mat4 br_mat4_from_quat(br_quat q)
{
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    br_mat4 r = br_mat4_identity();
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

br_mat4 br_mat4_from_trs(br_vec3 t, br_quat r, br_vec3 s)
{
    br_mat4 m = br_mat4_from_quat(r);
    for (int row = 0; row < 3; row++) {
        m.m[0 + row] *= s.x;
        m.m[4 + row] *= s.y;
        m.m[8 + row] *= s.z;
    }
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

br_mat4 br_mat4_from_mat3(br_mat3 a)
{
    br_mat4 r = br_mat4_identity();
    r.m[0] = a.a;
    r.m[1] = a.b;
    r.m[4] = a.c;
    r.m[5] = a.d;
    r.m[12] = a.tx;
    r.m[13] = a.ty;
    return r;
}

br_mat4 br_mat4_transpose(br_mat4 a)
{
    br_mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r.m[col * 4 + row] = a.m[row * 4 + col];
        }
    }
    return r;
}

br_mat4 br_mat4_invert(br_mat4 a, bool *ok)
{
    /* cofactor expansion (the classic MESA gluInvertMatrix layout) */
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        if (ok != NULL) {
            *ok = false;
        }
        return br_mat4_identity();
    }
    if (ok != NULL) {
        *ok = true;
    }
    br_mat4 r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = inv[i] / det;
    }
    return r;
}

br_vec4 br_mat4_transform_vec4(br_mat4 a, br_vec4 v)
{
    const float *m = a.m;
    return br_vec4_make(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
                        m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
                        m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                        m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w);
}

br_vec3 br_mat4_transform_point(br_mat4 m, br_vec3 p)
{
    br_vec4 r = br_mat4_transform_vec4(m, br_vec4_make(p.x, p.y, p.z, 1.0f));
    if (r.w != 0.0f && r.w != 1.0f) {
        return br_vec3_make(r.x / r.w, r.y / r.w, r.z / r.w);
    }
    return br_vec3_make(r.x, r.y, r.z);
}

br_vec3 br_mat4_transform_dir(br_mat4 m, br_vec3 d)
{
    br_vec4 r = br_mat4_transform_vec4(m, br_vec4_make(d.x, d.y, d.z, 0.0f));
    return br_vec3_make(r.x, r.y, r.z);
}

br_mat4 br_mat4_ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
    br_mat4 r = br_mat4_identity();
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -1.0f / (far_z - near_z);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -near_z / (far_z - near_z);
    return r;
}

br_mat4 br_mat4_perspective(float fov_y, float aspect, float near_z, float far_z)
{
    float f = 1.0f / SDL_tanf(fov_y * 0.5f);
    br_mat4 r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = 0.0f;
    }
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = near_z * far_z / (near_z - far_z);
    return r;
}

br_mat4 br_mat4_look_at(br_vec3 eye, br_vec3 target, br_vec3 up)
{
    br_vec3 f = br_vec3_normalize(br_vec3_sub(target, eye));
    br_vec3 s = br_vec3_normalize(br_vec3_cross(f, up));
    br_vec3 u = br_vec3_cross(s, f);
    br_mat4 r = br_mat4_identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -br_vec3_dot(s, eye);
    r.m[13] = -br_vec3_dot(u, eye);
    r.m[14] = br_vec3_dot(f, eye);
    return r;
}

bool br_mat4_nearly_equal(br_mat4 a, br_mat4 b, float eps)
{
    for (int i = 0; i < 16; i++) {
        if (!br_nearly_equal(a.m[i], b.m[i], eps)) {
            return false;
        }
    }
    return true;
}

/* ---- quat */

static br_quat br_quat_make(float x, float y, float z, float w)
{
    br_quat q = { x, y, z, w };
    return q;
}

br_quat br_quat_identity(void) { return br_quat_make(0.0f, 0.0f, 0.0f, 1.0f); }
float br_quat_dot(br_quat a, br_quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
br_quat br_quat_conjugate(br_quat q) { return br_quat_make(-q.x, -q.y, -q.z, q.w); }

br_quat br_quat_from_axis_angle(br_vec3 axis, float radians)
{
    float s = SDL_sinf(radians * 0.5f);
    return br_quat_make(axis.x * s, axis.y * s, axis.z * s, SDL_cosf(radians * 0.5f));
}

br_quat br_quat_from_euler(float x, float y, float z)
{
    br_quat qx = br_quat_from_axis_angle(br_vec3_make(1.0f, 0.0f, 0.0f), x);
    br_quat qy = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), y);
    br_quat qz = br_quat_from_axis_angle(br_vec3_make(0.0f, 0.0f, 1.0f), z);
    return br_quat_mul(qz, br_quat_mul(qy, qx));
}

br_quat br_quat_mul(br_quat a, br_quat b)
{
    return br_quat_make(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

br_quat br_quat_normalize(br_quat q)
{
    float len = SDL_sqrtf(br_quat_dot(q, q));
    if (len == 0.0f) {
        return br_quat_identity();
    }
    float inv = 1.0f / len;
    return br_quat_make(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

br_quat br_quat_slerp(br_quat a, br_quat b, float t)
{
    float cos_theta = br_quat_dot(a, b);
    if (cos_theta < 0.0f) { /* take the short way round */
        b = br_quat_make(-b.x, -b.y, -b.z, -b.w);
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) { /* nearly parallel: lerp avoids dividing by ~0 */
        br_quat r = br_quat_make(br_lerpf(a.x, b.x, t), br_lerpf(a.y, b.y, t),
                                 br_lerpf(a.z, b.z, t), br_lerpf(a.w, b.w, t));
        return br_quat_normalize(r);
    }
    float theta = SDL_acosf(cos_theta);
    float sin_theta = SDL_sinf(theta);
    float wa = SDL_sinf((1.0f - t) * theta) / sin_theta;
    float wb = SDL_sinf(t * theta) / sin_theta;
    return br_quat_make(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                        a.w * wa + b.w * wb);
}

br_vec3 br_quat_rotate(br_quat q, br_vec3 v)
{
    /* v' = v + 2w(u x v) + 2(u x (u x v)), u = q.xyz */
    br_vec3 u = br_vec3_make(q.x, q.y, q.z);
    br_vec3 uv = br_vec3_cross(u, v);
    br_vec3 uuv = br_vec3_cross(u, uv);
    return br_vec3_add(v, br_vec3_add(br_vec3_scale(uv, 2.0f * q.w), br_vec3_scale(uuv, 2.0f)));
}
