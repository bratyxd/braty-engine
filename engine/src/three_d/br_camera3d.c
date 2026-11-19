/* br_camera3d: projection, AABB and NDC-ray maths (pure), shadow cascade splits, orbit camera. */
#include "three_d/br_three_d_internal.h"

#include "braty/br_input.h"

br_camera3d br_camera3d_perspective(float fov_y, float near_z, float far_z)
{
    br_camera3d c;
    SDL_zero(c);
    c.projection = BR_PROJECTION_PERSPECTIVE;
    c.fov_y = fov_y;
    c.ortho_size = 5.0f;
    c.near_z = near_z;
    c.far_z = far_z;
    return c;
}

br_camera3d br_camera3d_orthographic(float size, float near_z, float far_z)
{
    br_camera3d c = br_camera3d_perspective(60.0f * BR_DEG_TO_RAD, near_z, far_z);
    c.projection = BR_PROJECTION_ORTHOGRAPHIC;
    c.ortho_size = size;
    return c;
}

br_mat4 br_camera3d_projection(const br_camera3d *camera, float aspect)
{
    if (camera->aspect > 0.0f) {
        aspect = camera->aspect;
    }
    if (!(aspect > 0.0f)) {
        aspect = 1.0f;
    }
    if (camera->projection == BR_PROJECTION_ORTHOGRAPHIC) {
        /* Unity: size is half the vertical extent; horizontal follows the aspect */
        float h = camera->ortho_size;
        float w = h * aspect;
        return br_mat4_ortho(-w, w, -h, h, camera->near_z, camera->far_z);
    }
    return br_mat4_perspective(camera->fov_y, aspect, camera->near_z, camera->far_z);
}

/* ---- AABB */

br_aabb br_aabb_empty(void)
{
    br_aabb b;
    b.min = br_vec3_make(FLT_MAX, FLT_MAX, FLT_MAX);
    b.max = br_vec3_make(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    return b;
}

br_aabb br_aabb_extend(br_aabb b, br_vec3 p)
{
    b.min = br_vec3_make(SDL_min(b.min.x, p.x), SDL_min(b.min.y, p.y), SDL_min(b.min.z, p.z));
    b.max = br_vec3_make(SDL_max(b.max.x, p.x), SDL_max(b.max.y, p.y), SDL_max(b.max.z, p.z));
    return b;
}

br_aabb br_aabb_union(br_aabb a, br_aabb b)
{
    if (!br_aabb_is_valid(b)) {
        return a;
    }
    return br_aabb_extend(br_aabb_extend(a, b.min), b.max);
}

bool br_aabb_is_valid(br_aabb b)
{
    return b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

br_aabb br_aabb_transform(br_aabb box, br_mat4 m)
{
    br_aabb out = br_aabb_empty();
    if (!br_aabb_is_valid(box)) {
        return out;
    }
    for (int i = 0; i < 8; i++) {
        br_vec3 c = br_vec3_make((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                                 (i & 4) ? box.max.z : box.min.z);
        out = br_aabb_extend(out, br_mat4_transform_point(m, c));
    }
    return out;
}

/* ---- rays */

br_ray3d br_ray3d_from_ndc(br_mat4 inverse_view_projection, float ndc_x, float ndc_y)
{
    br_vec3 near_point = br_mat4_transform_point(inverse_view_projection, br_vec3_make(ndc_x, ndc_y, 0.0f));
    br_vec3 far_point = br_mat4_transform_point(inverse_view_projection, br_vec3_make(ndc_x, ndc_y, 1.0f));
    br_ray3d r;
    r.origin = near_point;
    r.direction = br_vec3_normalize(br_vec3_sub(far_point, near_point));
    return r;
}

/* ---- shadows */

void br_shadow3d_cascade_splits(float near_z, float far_z, uint32_t count, float lambda, float *out_far)
{
    if (count == 0) {
        return;
    }
    near_z = SDL_max(near_z, 1e-3f);
    for (uint32_t i = 1; i <= count; i++) {
        float f = (float)i / (float)count;
        float log_split = near_z * SDL_powf(far_z / near_z, f);
        float uniform_split = near_z + (far_z - near_z) * f;
        out_far[i - 1] = br_lerpf(uniform_split, log_split, lambda);
    }
    out_far[count - 1] = far_z;
}

/* ---- orbit camera */

br_orbit_camera br_orbit_camera_make(br_vec3 target, float distance, float yaw, float pitch)
{
    br_orbit_camera o;
    o.target = target;
    o.distance = distance;
    o.yaw = yaw;
    o.pitch = pitch;
    o.min_distance = 0.5f;
    o.max_distance = 500.0f;
    o.rotate_speed = 0.01f;
    o.zoom_speed = 0.1f;
    return o;
}

void br_orbit_camera_apply(br_engine *e, const br_orbit_camera *orbit, br_node3d camera)
{
    float cp = SDL_cosf(orbit->pitch);
    br_vec3 offset = br_vec3_make(SDL_sinf(orbit->yaw) * cp, SDL_sinf(orbit->pitch), SDL_cosf(orbit->yaw) * cp);
    br_node3d_set_position(e, camera, br_vec3_add(orbit->target, br_vec3_scale(offset, orbit->distance)));
    br_node3d_look_at(e, camera, orbit->target, br_vec3_make(0.0f, 1.0f, 0.0f));
}

void br_orbit_camera_update(br_engine *e, br_orbit_camera *orbit, br_node3d camera)
{
    if (br_input_mouse_down(e, SDL_BUTTON_LEFT)) {
        br_vec2 d = br_input_mouse_delta(e);
        orbit->yaw -= d.x * orbit->rotate_speed;
        orbit->pitch = br_clampf(orbit->pitch + d.y * orbit->rotate_speed, -1.5f, 1.5f);
    }
    br_vec2 wheel = br_input_mouse_wheel(e);
    if (wheel.y != 0.0f) {
        orbit->distance *= SDL_powf(1.0f - orbit->zoom_speed, wheel.y);
        orbit->distance = br_clampf(orbit->distance, orbit->min_distance, orbit->max_distance);
    }
    br_orbit_camera_apply(e, orbit, camera);
}
