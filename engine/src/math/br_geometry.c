/* 3D primitives: planes, rays, spheres, boxes, oriented boxes, frustum planes, closest points.
 * Most algorithms follow Christer Ericson, "Real-Time Collision Detection" (2005). */
#include "braty/br_math.h"

#include <SDL3/SDL_stdinc.h>

#define BR_GEOMETRY_BIG 3.402823466e+38f /* FLT_MAX */

br_plane br_plane_from_point_normal(br_vec3 point, br_vec3 normal)
{
    br_plane p;
    p.normal = br_vec3_normalize(normal);
    p.d = -br_vec3_dot(p.normal, point);
    return p;
}

br_plane br_plane_from_points(br_vec3 a, br_vec3 b, br_vec3 c)
{
    return br_plane_from_point_normal(a, br_vec3_cross(br_vec3_sub(b, a), br_vec3_sub(c, a)));
}

float br_plane_distance(br_plane plane, br_vec3 p)
{
    return br_vec3_dot(plane.normal, p) + plane.d;
}

/* ---- rays */

bool br_ray_plane(br_vec3 origin, br_vec3 dir, br_plane plane, float *t)
{
    float denom = br_vec3_dot(plane.normal, dir);
    if (denom == 0.0f) {
        return false;
    }
    float hit = -br_plane_distance(plane, origin) / denom;
    if (hit < 0.0f) {
        return false;
    }
    if (t != NULL) {
        *t = hit;
    }
    return true;
}

bool br_ray_sphere(br_vec3 origin, br_vec3 dir, br_sphere sphere, float *t)
{
    br_vec3 m = br_vec3_sub(origin, sphere.center);
    float c = br_vec3_dot(m, m) - sphere.radius * sphere.radius;
    float hit = 0.0f;
    if (c > 0.0f) { /* origin outside */
        float a = br_vec3_dot(dir, dir);
        float b = br_vec3_dot(m, dir);
        if (b >= 0.0f || a == 0.0f) { /* pointing away, or no direction */
            return false;
        }
        float disc = b * b - a * c;
        if (disc < 0.0f) {
            return false;
        }
        hit = br_maxf(0.0f, (-b - SDL_sqrtf(disc)) / a);
    }
    if (t != NULL) {
        *t = hit;
    }
    return true;
}

bool br_ray_box(br_vec3 origin, br_vec3 dir, br_vec3 box_min, br_vec3 box_max, float *t)
{
    float o[3] = { origin.x, origin.y, origin.z };
    float d[3] = { dir.x, dir.y, dir.z };
    float lo[3] = { box_min.x, box_min.y, box_min.z };
    float hi[3] = { box_max.x, box_max.y, box_max.z };
    float t_min = 0.0f;
    float t_max = BR_GEOMETRY_BIG;
    for (int i = 0; i < 3; i++) {
        if (d[i] == 0.0f) { /* parallel to this slab: must already be inside it */
            if (o[i] < lo[i] || o[i] > hi[i]) {
                return false;
            }
            continue;
        }
        float t1 = (lo[i] - o[i]) / d[i];
        float t2 = (hi[i] - o[i]) / d[i];
        t_min = br_maxf(t_min, br_minf(t1, t2));
        t_max = br_minf(t_max, br_maxf(t1, t2));
        if (t_min > t_max) {
            return false;
        }
    }
    if (t != NULL) {
        *t = t_min;
    }
    return true;
}

bool br_ray_obb(br_vec3 origin, br_vec3 dir, br_obb box, float *t)
{
    /* into the box's frame: a rotation keeps the ray parameter */
    br_quat inv = br_quat_conjugate(br_quat_normalize(box.rotation));
    br_vec3 o = br_quat_rotate(inv, br_vec3_sub(origin, box.center));
    br_vec3 d = br_quat_rotate(inv, dir);
    return br_ray_box(o, d, br_vec3_negate(box.half_extents), box.half_extents, t);
}

bool br_ray_triangle(br_vec3 origin, br_vec3 dir, br_vec3 a, br_vec3 b, br_vec3 c, float *t, float *u, float *v)
{
    br_vec3 e1 = br_vec3_sub(b, a);
    br_vec3 e2 = br_vec3_sub(c, a);
    br_vec3 p = br_vec3_cross(dir, e2);
    float det = br_vec3_dot(e1, p);
    if (SDL_fabsf(det) < 1e-12f) { /* parallel or degenerate */
        return false;
    }
    float inv = 1.0f / det;
    br_vec3 s = br_vec3_sub(origin, a);
    float bu = br_vec3_dot(s, p) * inv;
    if (bu < 0.0f || bu > 1.0f) {
        return false;
    }
    br_vec3 q = br_vec3_cross(s, e1);
    float bv = br_vec3_dot(dir, q) * inv;
    if (bv < 0.0f || bu + bv > 1.0f) {
        return false;
    }
    float hit = br_vec3_dot(e2, q) * inv;
    if (hit < 0.0f) {
        return false;
    }
    if (t != NULL) {
        *t = hit;
    }
    if (u != NULL) {
        *u = bu;
    }
    if (v != NULL) {
        *v = bv;
    }
    return true;
}

/* ---- overlap tests */

bool br_sphere_sphere(br_sphere a, br_sphere b)
{
    float r = a.radius + b.radius;
    return br_vec3_distance_sq(a.center, b.center) <= r * r;
}

bool br_sphere_box(br_sphere s, br_vec3 box_min, br_vec3 box_max)
{
    br_vec3 p = br_closest_point_box(s.center, box_min, box_max);
    return br_vec3_distance_sq(p, s.center) <= s.radius * s.radius;
}

bool br_box_box(br_vec3 a_min, br_vec3 a_max, br_vec3 b_min, br_vec3 b_max)
{
    return a_min.x <= b_max.x && b_min.x <= a_max.x && a_min.y <= b_max.y && b_min.y <= a_max.y &&
           a_min.z <= b_max.z && b_min.z <= a_max.z;
}

/* The three unit axes of an oriented box. */
static void br_obb_axes(br_obb box, br_vec3 axes[3])
{
    br_quat q = br_quat_normalize(box.rotation);
    axes[0] = br_quat_rotate(q, br_vec3_make(1.0f, 0.0f, 0.0f));
    axes[1] = br_quat_rotate(q, br_vec3_make(0.0f, 1.0f, 0.0f));
    axes[2] = br_quat_rotate(q, br_vec3_make(0.0f, 0.0f, 1.0f));
}

bool br_obb_obb(br_obb a, br_obb b)
{
    /* Ericson 4.4.1: 3 axes of a, 3 of b, 9 cross products, all expressed in a's frame */
    br_vec3 aa[3];
    br_vec3 ba[3];
    br_obb_axes(a, aa);
    br_obb_axes(b, ba);
    float ea[3] = { a.half_extents.x, a.half_extents.y, a.half_extents.z };
    float eb[3] = { b.half_extents.x, b.half_extents.y, b.half_extents.z };
    float r[3][3];
    float abs_r[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = br_vec3_dot(aa[i], ba[j]);
            abs_r[i][j] = SDL_fabsf(r[i][j]) + 1e-6f; /* epsilon: parallel edges make a zero cross product */
        }
    }
    br_vec3 d = br_vec3_sub(b.center, a.center);
    float t[3] = { br_vec3_dot(d, aa[0]), br_vec3_dot(d, aa[1]), br_vec3_dot(d, aa[2]) };
    for (int i = 0; i < 3; i++) {
        float rb = eb[0] * abs_r[i][0] + eb[1] * abs_r[i][1] + eb[2] * abs_r[i][2];
        if (SDL_fabsf(t[i]) > ea[i] + rb) {
            return false;
        }
    }
    for (int j = 0; j < 3; j++) {
        float ra = ea[0] * abs_r[0][j] + ea[1] * abs_r[1][j] + ea[2] * abs_r[2][j];
        float proj = t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j];
        if (SDL_fabsf(proj) > ra + eb[j]) {
            return false;
        }
    }
    for (int i = 0; i < 3; i++) {
        int i1 = (i + 1) % 3;
        int i2 = (i + 2) % 3;
        for (int j = 0; j < 3; j++) {
            int j1 = (j + 1) % 3;
            int j2 = (j + 2) % 3;
            /* axis = a_i x b_j */
            float ra = ea[i1] * abs_r[i2][j] + ea[i2] * abs_r[i1][j];
            float rb = eb[j1] * abs_r[i][j2] + eb[j2] * abs_r[i][j1];
            float proj = t[i2] * r[i1][j] - t[i1] * r[i2][j];
            if (SDL_fabsf(proj) > ra + rb) {
                return false;
            }
        }
    }
    return true;
}

/* ---- frustum planes */

void br_frustum_planes(br_mat4 vp, br_plane out[6])
{
    /* clip space: -w <= x <= w, -w <= y <= w, 0 <= z <= w; row i of M dotted with (p, 1) */
    br_vec4 row[4];
    for (int i = 0; i < 4; i++) {
        row[i] = br_vec4_make(vp.m[i], vp.m[4 + i], vp.m[8 + i], vp.m[12 + i]);
    }
    br_vec4 p[6];
    p[0] = br_vec4_add(row[3], row[0]);
    p[1] = br_vec4_sub(row[3], row[0]);
    p[2] = br_vec4_add(row[3], row[1]);
    p[3] = br_vec4_sub(row[3], row[1]);
    p[4] = row[2];
    p[5] = br_vec4_sub(row[3], row[2]);
    for (int i = 0; i < 6; i++) {
        br_vec3 n = br_vec3_make(p[i].x, p[i].y, p[i].z);
        float len = br_vec3_length(n);
        float inv = len > 0.0f ? 1.0f / len : 0.0f;
        out[i].normal = br_vec3_scale(n, inv);
        out[i].d = p[i].w * inv;
    }
}

bool br_frustum_planes_sphere(const br_plane planes[6], br_sphere s)
{
    for (int i = 0; i < 6; i++) {
        if (br_plane_distance(planes[i], s.center) < -s.radius) {
            return false;
        }
    }
    return true;
}

bool br_frustum_planes_box(const br_plane planes[6], br_vec3 box_min, br_vec3 box_max)
{
    for (int i = 0; i < 6; i++) {
        /* the corner furthest along the normal: if even that is outside, the whole box is */
        br_vec3 n = planes[i].normal;
        br_vec3 corner = br_vec3_make(n.x >= 0.0f ? box_max.x : box_min.x, n.y >= 0.0f ? box_max.y : box_min.y,
                                      n.z >= 0.0f ? box_max.z : box_min.z);
        if (br_plane_distance(planes[i], corner) < 0.0f) {
            return false;
        }
    }
    return true;
}

/* ---- closest points */

br_vec3 br_closest_point_segment(br_vec3 p, br_vec3 a, br_vec3 b)
{
    br_vec3 ab = br_vec3_sub(b, a);
    float len2 = br_vec3_dot(ab, ab);
    float t = len2 > 0.0f ? br_clamp01f(br_vec3_dot(br_vec3_sub(p, a), ab) / len2) : 0.0f;
    return br_vec3_add(a, br_vec3_scale(ab, t));
}

br_vec3 br_closest_point_ray(br_vec3 p, br_vec3 origin, br_vec3 dir)
{
    float len2 = br_vec3_dot(dir, dir);
    float t = len2 > 0.0f ? br_maxf(0.0f, br_vec3_dot(br_vec3_sub(p, origin), dir) / len2) : 0.0f;
    return br_vec3_add(origin, br_vec3_scale(dir, t));
}

br_vec3 br_closest_point_plane(br_vec3 p, br_plane plane)
{
    return br_vec3_sub(p, br_vec3_scale(plane.normal, br_plane_distance(plane, p)));
}

br_vec3 br_closest_point_box(br_vec3 p, br_vec3 box_min, br_vec3 box_max)
{
    return br_vec3_make(br_clampf(p.x, box_min.x, box_max.x), br_clampf(p.y, box_min.y, box_max.y),
                        br_clampf(p.z, box_min.z, box_max.z));
}

br_vec3 br_closest_point_obb(br_vec3 p, br_obb box)
{
    br_quat q = br_quat_normalize(box.rotation);
    br_vec3 local = br_quat_rotate(br_quat_conjugate(q), br_vec3_sub(p, box.center));
    local = br_closest_point_box(local, br_vec3_negate(box.half_extents), box.half_extents);
    return br_vec3_add(box.center, br_quat_rotate(q, local));
}

br_vec3 br_closest_point_triangle(br_vec3 p, br_vec3 a, br_vec3 b, br_vec3 c)
{
    br_vec3 ab = br_vec3_sub(b, a);
    br_vec3 ac = br_vec3_sub(c, a);
    if (br_vec3_length_sq(br_vec3_cross(ab, ac)) == 0.0f) {
        /* degenerate: closest of the three edges */
        br_vec3 best = br_closest_point_segment(p, a, b);
        br_vec3 q = br_closest_point_segment(p, b, c);
        if (br_vec3_distance_sq(p, q) < br_vec3_distance_sq(p, best)) {
            best = q;
        }
        q = br_closest_point_segment(p, c, a);
        if (br_vec3_distance_sq(p, q) < br_vec3_distance_sq(p, best)) {
            best = q;
        }
        return best;
    }
    /* Ericson 5.1.5: find the Voronoi region of p */
    br_vec3 ap = br_vec3_sub(p, a);
    float d1 = br_vec3_dot(ab, ap);
    float d2 = br_vec3_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    br_vec3 bp = br_vec3_sub(p, b);
    float d3 = br_vec3_dot(ab, bp);
    float d4 = br_vec3_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return br_vec3_add(a, br_vec3_scale(ab, d1 / (d1 - d3)));
    }
    br_vec3 cp = br_vec3_sub(p, c);
    float d5 = br_vec3_dot(ab, cp);
    float d6 = br_vec3_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return br_vec3_add(a, br_vec3_scale(ac, d2 / (d2 - d6)));
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return br_vec3_add(b, br_vec3_scale(br_vec3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }
    float denom = 1.0f / (va + vb + vc);
    return br_vec3_add(a, br_vec3_add(br_vec3_scale(ab, vb * denom), br_vec3_scale(ac, vc * denom)));
}

float br_closest_points_segments(br_vec3 p1, br_vec3 q1, br_vec3 p2, br_vec3 q2, br_vec3 *c1, br_vec3 *c2)
{
    /* Ericson 5.1.9, with the degenerate (point) cases */
    br_vec3 d1 = br_vec3_sub(q1, p1);
    br_vec3 d2 = br_vec3_sub(q2, p2);
    br_vec3 r = br_vec3_sub(p1, p2);
    float a = br_vec3_dot(d1, d1);
    float e = br_vec3_dot(d2, d2);
    float f = br_vec3_dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a == 0.0f && e == 0.0f) {
        s = 0.0f;
        t = 0.0f;
    } else if (a == 0.0f) {
        t = br_clamp01f(f / e);
    } else {
        float c = br_vec3_dot(d1, r);
        if (e == 0.0f) {
            s = br_clamp01f(-c / a);
        } else {
            float b = br_vec3_dot(d1, d2);
            float denom = a * e - b * b;
            s = denom != 0.0f ? br_clamp01f((b * f - c * e) / denom) : 0.0f; /* parallel: any s works */
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = br_clamp01f(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = br_clamp01f((b - c) / a);
            }
        }
    }
    br_vec3 x1 = br_vec3_add(p1, br_vec3_scale(d1, s));
    br_vec3 x2 = br_vec3_add(p2, br_vec3_scale(d2, t));
    if (c1 != NULL) {
        *c1 = x1;
    }
    if (c2 != NULL) {
        *c2 = x2;
    }
    return br_vec3_distance_sq(x1, x2);
}

bool br_barycentric(br_vec3 p, br_vec3 a, br_vec3 b, br_vec3 c, br_vec3 *out)
{
    br_vec3 v0 = br_vec3_sub(b, a);
    br_vec3 v1 = br_vec3_sub(c, a);
    br_vec3 v2 = br_vec3_sub(p, a);
    float d00 = br_vec3_dot(v0, v0);
    float d01 = br_vec3_dot(v0, v1);
    float d11 = br_vec3_dot(v1, v1);
    float d20 = br_vec3_dot(v2, v0);
    float d21 = br_vec3_dot(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    if (denom == 0.0f) {
        *out = br_vec3_make(1.0f, 0.0f, 0.0f);
        return false;
    }
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    *out = br_vec3_make(1.0f - v - w, v, w);
    return true;
}

/* ---- the same on br_aabb / br_ray3d */

bool br_ray3d_plane(br_ray3d ray, br_plane plane, float *t)
{
    return br_ray_plane(ray.origin, ray.direction, plane, t);
}

bool br_ray3d_sphere(br_ray3d ray, br_sphere sphere, float *t)
{
    return br_ray_sphere(ray.origin, ray.direction, sphere, t);
}

bool br_ray3d_box(br_ray3d ray, br_aabb box, float *t)
{
    return br_ray_box(ray.origin, ray.direction, box.min, box.max, t);
}

bool br_ray3d_obb(br_ray3d ray, br_obb box, float *t)
{
    return br_ray_obb(ray.origin, ray.direction, box, t);
}

bool br_sphere_aabb(br_sphere s, br_aabb box)
{
    return br_sphere_box(s, box.min, box.max);
}

bool br_aabb_aabb(br_aabb a, br_aabb b)
{
    return br_box_box(a.min, a.max, b.min, b.max);
}

bool br_frustum_planes_aabb(const br_plane planes[6], br_aabb box)
{
    return br_frustum_planes_box(planes, box.min, box.max);
}

br_vec3 br_closest_point_aabb(br_vec3 p, br_aabb box)
{
    return br_closest_point_box(p, box.min, box.max);
}

br_vec3 br_closest_point_ray3d(br_vec3 p, br_ray3d ray)
{
    return br_closest_point_ray(p, ray.origin, ray.direction);
}
