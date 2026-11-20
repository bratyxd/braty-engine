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

/* Adjugate of a 4x4 matrix (cofactor expansion, the classic MESA gluInvertMatrix layout). */
static void br_mat4_adjugate(const float *m, float inv[16])
{
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

}

br_mat4 br_mat4_invert(br_mat4 a, bool *ok)
{
    const float *m = a.m;
    float inv[16];
    br_mat4_adjugate(m, inv);
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
    if (right == left || top == bottom || far_z == near_z) {
        return r; /* zero extent (a 0x0 headless window): identity instead of inf/NaN */
    }
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
    float tan_half = SDL_tanf(fov_y * 0.5f);
    if (tan_half == 0.0f || aspect == 0.0f || far_z == near_z) {
        return br_mat4_identity(); /* degenerate: identity instead of inf/NaN */
    }
    float f = 1.0f / tan_half;
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

br_quat br_quat_make(float x, float y, float z, float w)
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

/* ---- scalar helpers */

bool br_nearly_equal_rel(float a, float b, float rel_eps)
{
    float scale = br_maxf(1.0f, br_maxf(SDL_fabsf(a), SDL_fabsf(b)));
    return SDL_fabsf(a - b) <= rel_eps * scale;
}

float br_minf(float a, float b) { return a < b ? a : b; }
float br_maxf(float a, float b) { return a > b ? a : b; }
float br_clamp01f(float v) { return br_clampf(v, 0.0f, 1.0f); }
float br_stepf(float edge, float x) { return x < edge ? 0.0f : 1.0f; }

float br_signf(float v)
{
    if (v > 0.0f) {
        return 1.0f;
    }
    return v < 0.0f ? -1.0f : 0.0f;
}

float br_fractf(float x)
{
    float f = x - SDL_floorf(x);
    return f < 1.0f ? f : 0.0f; /* a tiny negative x rounds up to exactly 1 */
}

float br_wrapf(float v, float lo, float hi)
{
    float range = hi - lo;
    if (!(range > 0.0f)) {
        return lo;
    }
    float r = SDL_fmodf(v - lo, range);
    if (r < 0.0f) {
        r += range;
    }
    float out = lo + r;
    return out < hi ? out : lo;
}

float br_inverse_lerpf(float a, float b, float v)
{
    return a == b ? 0.0f : (v - a) / (b - a);
}

float br_remapf(float v, float in_lo, float in_hi, float out_lo, float out_hi)
{
    return br_lerpf(out_lo, out_hi, br_inverse_lerpf(in_lo, in_hi, v));
}

float br_smoothstepf(float edge0, float edge1, float x)
{
    float t = br_clamp01f(br_inverse_lerpf(edge0, edge1, x));
    return t * t * (3.0f - 2.0f * t);
}

float br_smootherstepf(float edge0, float edge1, float x)
{
    float t = br_clamp01f(br_inverse_lerpf(edge0, edge1, x));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float br_move_towardsf(float current, float target, float max_delta)
{
    float d = target - current;
    if (SDL_fabsf(d) <= max_delta) {
        return target;
    }
    return current + br_signf(d) * max_delta;
}

float br_smooth_dampf(float current, float target, float *velocity, float smooth_time, float max_speed, float dt)
{
    /* Game Programming Gems 4, chapter 1.10: the approximation Unity's SmoothDamp uses */
    smooth_time = br_maxf(0.0001f, smooth_time);
    float omega = 2.0f / smooth_time;
    float x = omega * dt;
    float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    float original_target = target;
    if (max_speed > 0.0f) {
        float max_change = max_speed * smooth_time;
        change = br_clampf(change, -max_change, max_change);
    }
    target = current - change;
    float temp = (*velocity + omega * change) * dt;
    *velocity = (*velocity - omega * temp) * decay;
    float out = target + (change + temp) * decay;
    if ((original_target - current > 0.0f) == (out > original_target)) { /* overshot: stop on the target */
        out = original_target;
        *velocity = 0.0f;
    }
    return out;
}

float br_angle_wrap(float radians)
{
    float a = SDL_fmodf(radians, BR_TAU);
    if (a <= -BR_PI) {
        a += BR_TAU;
    } else if (a > BR_PI) {
        a -= BR_TAU;
    }
    return a;
}

float br_angle_delta(float from, float to)
{
    return br_angle_wrap(to - from);
}

float br_angle_lerp(float from, float to, float t)
{
    return from + br_angle_delta(from, to) * t;
}

float br_angle_move_towards(float current, float target, float max_delta)
{
    float d = br_angle_delta(current, target);
    if (SDL_fabsf(d) <= max_delta) {
        return current + d;
    }
    return current + br_signf(d) * max_delta;
}

/* ---- vec2 extras */

br_vec2 br_vec2_negate(br_vec2 v) { return br_vec2_make(-v.x, -v.y); }
br_vec2 br_vec2_div(br_vec2 a, br_vec2 b) { return br_vec2_make(a.x / b.x, a.y / b.y); }
br_vec2 br_vec2_min(br_vec2 a, br_vec2 b) { return br_vec2_make(br_minf(a.x, b.x), br_minf(a.y, b.y)); }
br_vec2 br_vec2_max(br_vec2 a, br_vec2 b) { return br_vec2_make(br_maxf(a.x, b.x), br_maxf(a.y, b.y)); }
br_vec2 br_vec2_abs(br_vec2 v) { return br_vec2_make(SDL_fabsf(v.x), SDL_fabsf(v.y)); }
br_vec2 br_vec2_floor(br_vec2 v) { return br_vec2_make(SDL_floorf(v.x), SDL_floorf(v.y)); }
float br_vec2_length_sq(br_vec2 v) { return v.x * v.x + v.y * v.y; }
float br_vec2_distance_sq(br_vec2 a, br_vec2 b) { return br_vec2_length_sq(br_vec2_sub(a, b)); }
br_vec2 br_vec2_from_angle(float radians) { return br_vec2_make(SDL_cosf(radians), SDL_sinf(radians)); }
float br_vec2_to_angle(br_vec2 v) { return SDL_atan2f(v.y, v.x); }

br_vec2 br_vec2_reflect(br_vec2 v, br_vec2 n)
{
    return br_vec2_sub(v, br_vec2_scale(n, 2.0f * br_vec2_dot(v, n)));
}

br_vec2 br_vec2_project(br_vec2 v, br_vec2 onto)
{
    float len2 = br_vec2_length_sq(onto);
    return len2 > 0.0f ? br_vec2_scale(onto, br_vec2_dot(v, onto) / len2) : br_vec2_make(0.0f, 0.0f);
}

float br_vec2_angle(br_vec2 a, br_vec2 b)
{
    float len = br_vec2_length(a) * br_vec2_length(b);
    return len > 0.0f ? SDL_acosf(br_clampf(br_vec2_dot(a, b) / len, -1.0f, 1.0f)) : 0.0f;
}

float br_vec2_signed_angle(br_vec2 a, br_vec2 b)
{
    return SDL_atan2f(br_vec2_cross(a, b), br_vec2_dot(a, b));
}

br_vec2 br_vec2_clamp_length(br_vec2 v, float max_length)
{
    float len = br_vec2_length(v);
    return len > max_length && len > 0.0f ? br_vec2_scale(v, max_length / len) : v;
}

br_vec2 br_vec2_move_towards(br_vec2 current, br_vec2 target, float max_distance)
{
    br_vec2 d = br_vec2_sub(target, current);
    float len = br_vec2_length(d);
    if (len <= max_distance || len == 0.0f) {
        return target;
    }
    return br_vec2_add(current, br_vec2_scale(d, max_distance / len));
}

bool br_vec2_nearly_equal(br_vec2 a, br_vec2 b, float eps)
{
    return br_nearly_equal(a.x, b.x, eps) && br_nearly_equal(a.y, b.y, eps);
}

/* ---- vec3 extras */

br_vec3 br_vec3_negate(br_vec3 v) { return br_vec3_make(-v.x, -v.y, -v.z); }
br_vec3 br_vec3_div(br_vec3 a, br_vec3 b) { return br_vec3_make(a.x / b.x, a.y / b.y, a.z / b.z); }
br_vec3 br_vec3_abs(br_vec3 v) { return br_vec3_make(SDL_fabsf(v.x), SDL_fabsf(v.y), SDL_fabsf(v.z)); }
float br_vec3_length_sq(br_vec3 v) { return br_vec3_dot(v, v); }
float br_vec3_distance(br_vec3 a, br_vec3 b) { return br_vec3_length(br_vec3_sub(a, b)); }
float br_vec3_distance_sq(br_vec3 a, br_vec3 b) { return br_vec3_length_sq(br_vec3_sub(a, b)); }

br_vec3 br_vec3_min(br_vec3 a, br_vec3 b)
{
    return br_vec3_make(br_minf(a.x, b.x), br_minf(a.y, b.y), br_minf(a.z, b.z));
}

br_vec3 br_vec3_max(br_vec3 a, br_vec3 b)
{
    return br_vec3_make(br_maxf(a.x, b.x), br_maxf(a.y, b.y), br_maxf(a.z, b.z));
}

br_vec3 br_vec3_reflect(br_vec3 v, br_vec3 n)
{
    return br_vec3_sub(v, br_vec3_scale(n, 2.0f * br_vec3_dot(v, n)));
}

br_vec3 br_vec3_refract(br_vec3 v, br_vec3 n, float eta)
{
    float d = br_vec3_dot(n, v);
    float k = 1.0f - eta * eta * (1.0f - d * d);
    if (k < 0.0f) {
        return br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    return br_vec3_sub(br_vec3_scale(v, eta), br_vec3_scale(n, eta * d + SDL_sqrtf(k)));
}

br_vec3 br_vec3_project(br_vec3 v, br_vec3 onto)
{
    float len2 = br_vec3_length_sq(onto);
    return len2 > 0.0f ? br_vec3_scale(onto, br_vec3_dot(v, onto) / len2) : br_vec3_make(0.0f, 0.0f, 0.0f);
}

br_vec3 br_vec3_project_on_plane(br_vec3 v, br_vec3 n)
{
    return br_vec3_sub(v, br_vec3_scale(n, br_vec3_dot(v, n)));
}

float br_vec3_angle(br_vec3 a, br_vec3 b)
{
    float len = br_vec3_length(a) * br_vec3_length(b);
    return len > 0.0f ? SDL_acosf(br_clampf(br_vec3_dot(a, b) / len, -1.0f, 1.0f)) : 0.0f;
}

br_vec3 br_vec3_clamp_length(br_vec3 v, float max_length)
{
    float len = br_vec3_length(v);
    return len > max_length && len > 0.0f ? br_vec3_scale(v, max_length / len) : v;
}

br_vec3 br_vec3_move_towards(br_vec3 current, br_vec3 target, float max_distance)
{
    br_vec3 d = br_vec3_sub(target, current);
    float len = br_vec3_length(d);
    if (len <= max_distance || len == 0.0f) {
        return target;
    }
    return br_vec3_add(current, br_vec3_scale(d, max_distance / len));
}

br_vec3 br_vec3_any_perpendicular(br_vec3 v)
{
    /* cross with the axis v is least aligned with */
    br_vec3 a = br_vec3_abs(v);
    br_vec3 axis = br_vec3_make(1.0f, 0.0f, 0.0f);
    if (a.y < a.x && a.y <= a.z) {
        axis = br_vec3_make(0.0f, 1.0f, 0.0f);
    } else if (a.z < a.x && a.z < a.y) {
        axis = br_vec3_make(0.0f, 0.0f, 1.0f);
    }
    br_vec3 p = br_vec3_normalize(br_vec3_cross(v, axis));
    return br_vec3_length_sq(p) > 0.0f ? p : br_vec3_make(1.0f, 0.0f, 0.0f);
}

void br_vec3_orthonormal_basis(br_vec3 n, br_vec3 *t, br_vec3 *b)
{
    /* Duff et al. 2017, "Building an Orthonormal Basis, Revisited" */
    float sign = SDL_copysignf(1.0f, n.z);
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;
    *t = br_vec3_make(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    *b = br_vec3_make(bb, sign + n.y * n.y * a, -n.y);
}

br_vec3 br_vec3_slerp(br_vec3 a, br_vec3 b, float t)
{
    float la = br_vec3_length(a);
    float lb = br_vec3_length(b);
    if (la == 0.0f || lb == 0.0f) {
        return br_vec3_lerp(a, b, t);
    }
    br_vec3 ua = br_vec3_scale(a, 1.0f / la);
    br_vec3 ub = br_vec3_scale(b, 1.0f / lb);
    float d = br_clampf(br_vec3_dot(ua, ub), -1.0f, 1.0f);
    float theta = SDL_acosf(d);
    if (theta < 1e-5f) {
        return br_vec3_lerp(a, b, t);
    }
    /* w: unit direction in the plane of a and b, perpendicular to a (any one if they are opposite) */
    br_vec3 w = br_vec3_normalize(br_vec3_sub(ub, br_vec3_scale(ua, d)));
    if (br_vec3_length_sq(w) == 0.0f) {
        w = br_vec3_any_perpendicular(ua);
    }
    br_vec3 dir = br_vec3_add(br_vec3_scale(ua, SDL_cosf(theta * t)), br_vec3_scale(w, SDL_sinf(theta * t)));
    return br_vec3_scale(dir, br_lerpf(la, lb, t));
}

bool br_vec3_nearly_equal(br_vec3 a, br_vec3 b, float eps)
{
    return br_nearly_equal(a.x, b.x, eps) && br_nearly_equal(a.y, b.y, eps) && br_nearly_equal(a.z, b.z, eps);
}

/* ---- vec4 extras */

br_vec4 br_vec4_sub(br_vec4 a, br_vec4 b) { return br_vec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
br_vec4 br_vec4_mul(br_vec4 a, br_vec4 b) { return br_vec4_make(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); }
br_vec4 br_vec4_negate(br_vec4 v) { return br_vec4_make(-v.x, -v.y, -v.z, -v.w); }
float br_vec4_length_sq(br_vec4 v) { return br_vec4_dot(v, v); }
float br_vec4_length(br_vec4 v) { return SDL_sqrtf(br_vec4_dot(v, v)); }

br_vec4 br_vec4_min(br_vec4 a, br_vec4 b)
{
    return br_vec4_make(br_minf(a.x, b.x), br_minf(a.y, b.y), br_minf(a.z, b.z), br_minf(a.w, b.w));
}

br_vec4 br_vec4_max(br_vec4 a, br_vec4 b)
{
    return br_vec4_make(br_maxf(a.x, b.x), br_maxf(a.y, b.y), br_maxf(a.z, b.z), br_maxf(a.w, b.w));
}

br_vec4 br_vec4_normalize(br_vec4 v)
{
    float len = br_vec4_length(v);
    return len > 0.0f ? br_vec4_scale(v, 1.0f / len) : br_vec4_make(0.0f, 0.0f, 0.0f, 0.0f);
}

bool br_vec4_nearly_equal(br_vec4 a, br_vec4 b, float eps)
{
    return br_nearly_equal(a.x, b.x, eps) && br_nearly_equal(a.y, b.y, eps) && br_nearly_equal(a.z, b.z, eps) &&
           br_nearly_equal(a.w, b.w, eps);
}

/* ---- mat4 extras */

float br_mat4_determinant(br_mat4 a)
{
    float inv[16];
    br_mat4_adjugate(a.m, inv);
    return a.m[0] * inv[0] + a.m[1] * inv[4] + a.m[2] * inv[8] + a.m[3] * inv[12];
}

/* Inverse of the upper 3x3 of a column-major mat4, written column-major into out[9]. */
static bool br_mat4_invert3x3(const float *m, float out[9])
{
    /* rows of the inverse are c1 x c2, c2 x c0, c0 x c1 divided by the determinant */
    br_vec3 c0 = br_vec3_make(m[0], m[1], m[2]);
    br_vec3 c1 = br_vec3_make(m[4], m[5], m[6]);
    br_vec3 c2 = br_vec3_make(m[8], m[9], m[10]);
    br_vec3 r0 = br_vec3_cross(c1, c2);
    br_vec3 r1 = br_vec3_cross(c2, c0);
    br_vec3 r2 = br_vec3_cross(c0, c1);
    float det = br_vec3_dot(c0, r0);
    if (det == 0.0f) {
        return false;
    }
    float inv = 1.0f / det;
    out[0] = r0.x * inv;
    out[3] = r0.y * inv;
    out[6] = r0.z * inv;
    out[1] = r1.x * inv;
    out[4] = r1.y * inv;
    out[7] = r1.z * inv;
    out[2] = r2.x * inv;
    out[5] = r2.y * inv;
    out[8] = r2.z * inv;
    return true;
}

br_mat4 br_mat4_invert_affine(br_mat4 a, bool *ok)
{
    float r[9];
    bool good = br_mat4_invert3x3(a.m, r);
    if (ok != NULL) {
        *ok = good;
    }
    if (!good) {
        return br_mat4_identity();
    }
    br_mat4 out = br_mat4_identity();
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            out.m[col * 4 + row] = r[col * 3 + row];
        }
    }
    for (int row = 0; row < 3; row++) {
        out.m[12 + row] = -(r[row] * a.m[12] + r[3 + row] * a.m[13] + r[6 + row] * a.m[14]);
    }
    return out;
}

br_mat4 br_mat4_invert_rigid(br_mat4 a)
{
    br_mat4 out = br_mat4_identity();
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            out.m[col * 4 + row] = a.m[row * 4 + col];
        }
    }
    for (int row = 0; row < 3; row++) {
        out.m[12 + row] = -(out.m[row] * a.m[12] + out.m[4 + row] * a.m[13] + out.m[8 + row] * a.m[14]);
    }
    return out;
}

br_mat4 br_mat4_rotation_x(float radians)
{
    float c = SDL_cosf(radians);
    float s = SDL_sinf(radians);
    br_mat4 r = br_mat4_identity();
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

br_mat4 br_mat4_rotation_y(float radians)
{
    float c = SDL_cosf(radians);
    float s = SDL_sinf(radians);
    br_mat4 r = br_mat4_identity();
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

br_mat4 br_mat4_rotation_z(float radians)
{
    float c = SDL_cosf(radians);
    float s = SDL_sinf(radians);
    br_mat4 r = br_mat4_identity();
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

br_mat4 br_mat4_normal_matrix(br_mat4 a)
{
    float r[9];
    br_mat4 out = br_mat4_identity();
    if (!br_mat4_invert3x3(a.m, r)) {
        return out;
    }
    for (int col = 0; col < 3; col++) { /* transposed while copying */
        for (int row = 0; row < 3; row++) {
            out.m[col * 4 + row] = r[row * 3 + col];
        }
    }
    return out;
}

/* Quaternion of a pure rotation given as three orthonormal columns (Shepperd's method). */
static br_quat br_quat_from_columns(br_vec3 c0, br_vec3 c1, br_vec3 c2)
{
    float m00 = c0.x, m10 = c0.y, m20 = c0.z;
    float m01 = c1.x, m11 = c1.y, m21 = c1.z;
    float m02 = c2.x, m12 = c2.y, m22 = c2.z;
    float trace = m00 + m11 + m22;
    br_quat q;
    if (trace > 0.0f) {
        float s = SDL_sqrtf(trace + 1.0f) * 2.0f;
        q = br_quat_make((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
    } else if (m00 > m11 && m00 > m22) {
        float s = SDL_sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q = br_quat_make(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    } else if (m11 > m22) {
        float s = SDL_sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q = br_quat_make((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
    } else {
        float s = SDL_sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q = br_quat_make((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
    }
    return br_quat_normalize(q);
}

bool br_mat4_decompose(br_mat4 m, br_vec3 *translation, br_quat *rotation, br_vec3 *scale)
{
    br_vec3 c0 = br_vec3_make(m.m[0], m.m[1], m.m[2]);
    br_vec3 c1 = br_vec3_make(m.m[4], m.m[5], m.m[6]);
    br_vec3 c2 = br_vec3_make(m.m[8], m.m[9], m.m[10]);
    *translation = br_vec3_make(m.m[12], m.m[13], m.m[14]);
    br_vec3 s = br_vec3_make(br_vec3_length(c0), br_vec3_length(c1), br_vec3_length(c2));
    if (s.x == 0.0f || s.y == 0.0f || s.z == 0.0f) {
        *rotation = br_quat_identity();
        *scale = br_vec3_make(0.0f, 0.0f, 0.0f);
        return false;
    }
    if (br_vec3_dot(c0, br_vec3_cross(c1, c2)) < 0.0f) { /* mirrored: put the flip on x */
        s.x = -s.x;
    }
    *scale = s;
    *rotation = br_quat_from_columns(br_vec3_scale(c0, 1.0f / s.x), br_vec3_scale(c1, 1.0f / s.y),
                                     br_vec3_scale(c2, 1.0f / s.z));
    return true;
}

br_mat4 br_mat4_look_to(br_vec3 eye, br_vec3 direction, br_vec3 up)
{
    return br_mat4_look_at(eye, br_vec3_add(eye, direction), up);
}

br_mat4 br_mat4_frustum(float left, float right, float bottom, float top, float near_z, float far_z)
{
    if (right == left || top == bottom || far_z == near_z) {
        return br_mat4_identity(); /* degenerate: identity instead of inf/NaN */
    }
    br_mat4 r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = 0.0f;
    }
    r.m[0] = 2.0f * near_z / (right - left);
    r.m[5] = 2.0f * near_z / (top - bottom);
    r.m[8] = (right + left) / (right - left);
    r.m[9] = (top + bottom) / (top - bottom);
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = near_z * far_z / (near_z - far_z);
    return r;
}

br_mat4 br_mat4_perspective_infinite(float fov_y, float aspect, float near_z)
{
    br_mat4 r = br_mat4_perspective(fov_y, aspect, near_z, 2.0f * near_z);
    r.m[10] = -1.0f;   /* limit of far / (near - far) */
    r.m[14] = -near_z; /* limit of near * far / (near - far) */
    return r;
}

br_mat4 br_mat4_perspective_reversed(float fov_y, float aspect, float near_z, float far_z)
{
    /* depth' = 1 - depth, i.e. clip z' = w - z */
    br_mat4 r = br_mat4_perspective(fov_y, aspect, near_z, far_z);
    r.m[10] = near_z / (far_z - near_z);
    r.m[14] = near_z * far_z / (far_z - near_z);
    return r;
}

br_mat4 br_mat4_perspective_reversed_infinite(float fov_y, float aspect, float near_z)
{
    br_mat4 r = br_mat4_perspective(fov_y, aspect, near_z, 2.0f * near_z);
    r.m[10] = 0.0f;
    r.m[14] = near_z;
    return r;
}

/* ---- quat extras */

float br_quat_length(br_quat q) { return SDL_sqrtf(br_quat_dot(q, q)); }

br_quat br_quat_inverse(br_quat q)
{
    float len2 = br_quat_dot(q, q);
    if (len2 == 0.0f) {
        return br_quat_identity();
    }
    return br_quat_make(-q.x / len2, -q.y / len2, -q.z / len2, q.w / len2);
}

br_quat br_quat_nlerp(br_quat a, br_quat b, float t)
{
    if (br_quat_dot(a, b) < 0.0f) {
        b = br_quat_make(-b.x, -b.y, -b.z, -b.w);
    }
    return br_quat_normalize(br_quat_make(br_lerpf(a.x, b.x, t), br_lerpf(a.y, b.y, t), br_lerpf(a.z, b.z, t),
                                          br_lerpf(a.w, b.w, t)));
}

float br_quat_angle(br_quat a, br_quat b)
{
    float d = br_minf(SDL_fabsf(br_quat_dot(a, b)), 1.0f);
    return 2.0f * SDL_acosf(d);
}

void br_quat_to_axis_angle(br_quat q, br_vec3 *axis, float *radians)
{
    q = br_quat_normalize(q);
    if (q.w < 0.0f) { /* same rotation; keeps the angle in [0, pi] */
        q = br_quat_make(-q.x, -q.y, -q.z, -q.w);
    }
    float s = SDL_sqrtf(br_maxf(0.0f, 1.0f - q.w * q.w));
    if (s < 1e-6f) {
        *axis = br_vec3_make(1.0f, 0.0f, 0.0f);
        *radians = 0.0f;
        return;
    }
    *radians = 2.0f * SDL_acosf(br_minf(q.w, 1.0f));
    *axis = br_vec3_make(q.x / s, q.y / s, q.z / s);
}

void br_quat_to_euler(br_quat q, float *x, float *y, float *z)
{
    /* from_euler builds R = Rz * Ry * Rx; read the angles back from the matrix entries */
    q = br_quat_normalize(q);
    float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float r10 = 2.0f * (q.x * q.y + q.w * q.z);
    float r20 = 2.0f * (q.x * q.z - q.w * q.y);
    float r21 = 2.0f * (q.y * q.z + q.w * q.x);
    float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    float r12 = 2.0f * (q.y * q.z - q.w * q.x);
    *y = SDL_asinf(br_clampf(-r20, -1.0f, 1.0f));
    if (SDL_fabsf(r20) < 0.99999f) {
        *x = SDL_atan2f(r21, r22);
        *z = SDL_atan2f(r10, r00);
    } else { /* gimbal lock: only one of x, z is defined; put it all on x */
        *x = SDL_atan2f(-r12, r11);
        *z = 0.0f;
    }
}

br_quat br_quat_from_mat4(br_mat4 m)
{
    br_vec3 t;
    br_vec3 s;
    br_quat r;
    return br_mat4_decompose(m, &t, &r, &s) ? r : br_quat_identity();
}

br_quat br_quat_from_to(br_vec3 from, br_vec3 to)
{
    br_vec3 f = br_vec3_normalize(from);
    br_vec3 t = br_vec3_normalize(to);
    if (br_vec3_length_sq(f) == 0.0f || br_vec3_length_sq(t) == 0.0f) {
        return br_quat_identity();
    }
    float d = br_vec3_dot(f, t);
    if (d < -0.999999f) { /* opposite: half a turn about any perpendicular axis */
        return br_quat_from_axis_angle(br_vec3_any_perpendicular(f), BR_PI);
    }
    br_vec3 c = br_vec3_cross(f, t);
    return br_quat_normalize(br_quat_make(c.x, c.y, c.z, 1.0f + d));
}

br_quat br_quat_look_rotation(br_vec3 forward, br_vec3 up)
{
    br_vec3 f = br_vec3_normalize(forward);
    if (br_vec3_length_sq(f) == 0.0f) {
        return br_quat_identity();
    }
    br_vec3 z = br_vec3_negate(f); /* the camera looks down its -Z */
    br_vec3 x = br_vec3_normalize(br_vec3_cross(up, z));
    if (br_vec3_length_sq(x) == 0.0f) { /* up parallel to forward */
        x = br_vec3_any_perpendicular(z);
    }
    br_vec3 y = br_vec3_cross(z, x);
    return br_quat_from_columns(x, y, z);
}

br_quat br_quat_rotate_towards(br_quat from, br_quat to, float max_radians)
{
    float angle = br_quat_angle(from, to);
    if (angle <= max_radians || angle == 0.0f) {
        return to;
    }
    return br_quat_slerp(from, to, max_radians / angle);
}

/* ---- springs */

void br_spring_step(float *value, float *velocity, float target, float frequency, float damping_ratio, float dt)
{
    if (dt <= 0.0f) {
        return;
    }
    // ponytail: implicit Euler is stable for any dt but adds a little damping at large dt; switch to the
    // analytic damped-oscillator solution if exact bounce timing ever matters.
    float omega = BR_TAU * frequency;
    float k = omega * omega;
    float c = 2.0f * damping_ratio * omega;
    float v = (*velocity - dt * k * (*value - target)) / (1.0f + dt * c + dt * dt * k);
    *velocity = v;
    *value += dt * v;
}

void br_spring_step_vec2(br_vec2 *value, br_vec2 *velocity, br_vec2 target, float frequency, float damping_ratio, float dt)
{
    br_spring_step(&value->x, &velocity->x, target.x, frequency, damping_ratio, dt);
    br_spring_step(&value->y, &velocity->y, target.y, frequency, damping_ratio, dt);
}

void br_spring_step_vec3(br_vec3 *value, br_vec3 *velocity, br_vec3 target, float frequency, float damping_ratio, float dt)
{
    br_spring_step(&value->x, &velocity->x, target.x, frequency, damping_ratio, dt);
    br_spring_step(&value->y, &velocity->y, target.y, frequency, damping_ratio, dt);
    br_spring_step(&value->z, &velocity->z, target.z, frequency, damping_ratio, dt);
}
