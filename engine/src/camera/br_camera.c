/* br_camera: Unity-style cameras for 2D and 3D. Pure view maths, the "camera" node type, and
 * the systems that turn cameras into render views (br_render_set_views) every frame. */
#include "camera/br_camera_internal.h"

#include "braty/br_events.h"
#include "braty/br_input.h"
#include "braty/br_noise.h"
#include "braty/br_random.h"
#include "scene/br_scene_internal.h"

/* ---- defaults */

br_camera br_camera_default_2d(void)
{
    br_camera c;
    SDL_zero(c);
    c.projection = BR_CAMERA_ORTHOGRAPHIC;
    c.orthographic_size = 360.0f;
    c.field_of_view = 60.0f * BR_DEG_TO_RAD;
    c.near_z = 0.1f;
    c.far_z = 500.0f;
    c.viewport_rect = br_rect_make(0.0f, 0.0f, 1.0f, 1.0f);
    c.culling_mask = 0xFFFFFFFFu;
    c.clear_flags = BR_CAMERA_CLEAR_COLOR;
    c.background_color = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    c.pixels_per_unit = 1.0f;
    c.fit = BR_CAMERA_FIT_HEIGHT;
    c.reference_aspect = 16.0f / 9.0f;
    c.enabled = true;
    return c;
}

br_camera br_camera_default_3d(void)
{
    br_camera c = br_camera_default_2d();
    c.projection = BR_CAMERA_PERSPECTIVE;
    c.orthographic_size = 5.0f;
    return c;
}

/* ---- pure view maths */

br_vec2 br_camera_fit_extents(br_camera_fit fit, float base, float aspect, float reference_aspect)
{
    float ref = reference_aspect > 0.0f ? reference_aspect : 16.0f / 9.0f;
    float a = aspect > 0.0f ? aspect : ref;
    switch (fit) {
    case BR_CAMERA_FIT_WIDTH:
        return br_vec2_make(base * ref, base * ref / a);
    case BR_CAMERA_FIT_EXPAND:
        if (a >= ref) {
            return br_vec2_make(base * a, base);
        }
        return br_vec2_make(base * ref, base * ref / a);
    case BR_CAMERA_FIT_LETTERBOX:
        return br_vec2_make(base * ref, base);
    case BR_CAMERA_FIT_HEIGHT:
    default:
        return br_vec2_make(base * a, base);
    }
}

/* Maps clip space of a viewport into clip space of the whole screen. */
static br_mat4 br_camera_viewport_matrix(br_rect vp, br_vec2 screen)
{
    br_mat4 m = br_mat4_identity();
    m.m[0] = vp.width / screen.x;
    m.m[5] = vp.height / screen.y;
    m.m[12] = (2.0f * vp.x + vp.width) / screen.x - 1.0f;
    m.m[13] = 1.0f - (2.0f * vp.y + vp.height) / screen.y;
    return m;
}

br_camera_view br_camera_compute_view(const br_camera *c, br_mat4 world, bool is_3d, br_vec2 screen_size,
                                      float pixel_density)
{
    br_camera_view v;
    SDL_zero(v);
    v.is_3d = is_3d;
    v.screen_size = br_vec2_make(SDL_max(screen_size.x, 1.0f), SDL_max(screen_size.y, 1.0f));
    br_rect r = c->viewport_rect;
    br_rect vp = br_rect_make(r.x * v.screen_size.x, r.y * v.screen_size.y, SDL_max(r.width * v.screen_size.x, 1.0f),
                              SDL_max(r.height * v.screen_size.y, 1.0f));
    float ref = c->reference_aspect > 0.0f ? c->reference_aspect : 16.0f / 9.0f;
    if (c->fit == BR_CAMERA_FIT_LETTERBOX) {
        float a = vp.width / vp.height;
        if (a > ref) {
            float w = vp.height * ref;
            vp.x += (vp.width - w) * 0.5f;
            vp.width = w;
        } else {
            float h = vp.width / ref;
            vp.y += (vp.height - h) * 0.5f;
            vp.height = h;
        }
    }
    v.viewport = vp;
    float aspect = c->aspect > 0.0f ? c->aspect : vp.width / vp.height;
    bool perspective = is_3d && c->projection == BR_CAMERA_PERSPECTIVE;
    float base;
    if (perspective) {
        base = SDL_tanf(c->field_of_view * 0.5f);
    } else {
        base = c->orthographic_size * (is_3d ? 1.0f : (c->pixels_per_unit > 0.0f ? c->pixels_per_unit : 1.0f));
    }
    br_vec2 half = br_camera_fit_extents(c->fit, base, aspect, ref);
    v.position = br_vec3_make(world.m[12], world.m[13], world.m[14]);

    if (!is_3d) {
        /* 2D: stage -> screen = to viewport centre * zoom * unrotate * (p - position) */
        float angle = SDL_atan2f(world.m[1], world.m[0]);
        br_vec2 pos = br_vec2_make(world.m[12], world.m[13]);
        if (c->pixel_perfect && pixel_density > 0.0f) {
            float zoom_px = vp.height * pixel_density / (2.0f * half.y);
            float z = SDL_max(1.0f, SDL_floorf(zoom_px + 0.001f));
            half = br_vec2_make(vp.width * pixel_density / (2.0f * z), vp.height * pixel_density / (2.0f * z));
            pos.x = SDL_roundf(pos.x * z) / z;
            pos.y = SDL_roundf(pos.y * z) / z;
            v.position = br_vec3_make(pos.x, pos.y, 0.0f);
        }
        v.half_width = half.x;
        v.half_height = half.y;
        br_mat3 m = br_mat3_translation(vp.x + vp.width * 0.5f, vp.y + vp.height * 0.5f);
        m = br_mat3_mul(m, br_mat3_scaling(vp.width / (2.0f * half.x), vp.height / (2.0f * half.y)));
        m = br_mat3_mul(m, br_mat3_rotation(-angle));
        m = br_mat3_mul(m, br_mat3_translation(-pos.x, -pos.y));
        v.screen_from_world = m;
        v.world_from_screen = br_mat3_invert(m, NULL);
        v.visible = br_rect_transform_bounds(vp, v.world_from_screen);
        br_mat4 screen = br_mat4_ortho(0.0f, v.screen_size.x, v.screen_size.y, 0.0f, -1.0f, 1.0f);
        v.clip_from_world = br_mat4_mul(screen, br_mat4_from_mat3(m));
        v.world_from_clip = br_mat4_invert(v.clip_from_world, NULL);
        v.view = br_mat4_from_mat3(br_mat3_invert(br_mat3_mul(br_mat3_translation(pos.x, pos.y), br_mat3_rotation(angle)), NULL));
        v.projection = br_mat4_ortho(-half.x, half.x, half.y, -half.y, -1.0f, 1.0f);
        v.forward = br_vec3_make(0.0f, 0.0f, 1.0f);
        return v;
    }

    v.half_width = half.x;
    v.half_height = half.y;
    v.view = br_mat4_invert(world, NULL);
    float near_z = c->near_z > 0.0f ? c->near_z : 0.1f;
    float far_z = c->far_z > near_z ? c->far_z : near_z + 1000.0f;
    if (perspective) {
        v.projection = br_mat4_perspective(2.0f * SDL_atanf(half.y), half.x / half.y, near_z, far_z);
    } else {
        v.projection = br_mat4_ortho(-half.x, half.x, -half.y, half.y, near_z, far_z);
    }
    v.clip_from_world = br_mat4_mul(br_camera_viewport_matrix(vp, v.screen_size), br_mat4_mul(v.projection, v.view));
    v.world_from_clip = br_mat4_invert(v.clip_from_world, NULL);
    v.forward = br_vec3_normalize(br_vec3_make(-world.m[8], -world.m[9], -world.m[10]));
    v.visible = vp;
    return v;
}

br_ray3d br_camera_view_screen_to_ray(const br_camera_view *v, br_vec2 screen)
{
    br_ray3d ray;
    if (!v->is_3d) {
        br_vec2 p = br_mat3_apply(v->world_from_screen, screen);
        ray.origin = br_vec3_make(p.x, p.y, 0.0f);
        ray.direction = br_vec3_make(0.0f, 0.0f, 1.0f);
        return ray;
    }
    float x = 2.0f * screen.x / v->screen_size.x - 1.0f;
    float y = 1.0f - 2.0f * screen.y / v->screen_size.y;
    return br_ray3d_from_ndc(v->world_from_clip, x, y);
}

br_vec3 br_camera_view_screen_to_world(const br_camera_view *v, br_vec3 screen)
{
    br_ray3d ray = br_camera_view_screen_to_ray(v, br_vec2_make(screen.x, screen.y));
    if (!v->is_3d) {
        return ray.origin;
    }
    /* the point of the ray at distance screen.z along the camera's forward axis */
    float along = br_vec3_dot(ray.direction, v->forward);
    float start = br_vec3_dot(br_vec3_sub(ray.origin, v->position), v->forward);
    float t = along != 0.0f ? (screen.z - start) / along : 0.0f;
    return br_vec3_add(ray.origin, br_vec3_scale(ray.direction, t));
}

br_vec3 br_camera_view_world_to_screen(const br_camera_view *v, br_vec3 world)
{
    if (!v->is_3d) {
        br_vec2 p = br_mat3_apply(v->screen_from_world, br_vec2_make(world.x, world.y));
        return br_vec3_make(p.x, p.y, 0.0f);
    }
    br_vec4 clip = br_mat4_transform_vec4(v->clip_from_world, br_vec4_make(world.x, world.y, world.z, 1.0f));
    float w = SDL_fabsf(clip.w) > 1e-8f ? clip.w : 1e-8f;
    float sx = (clip.x / w * 0.5f + 0.5f) * v->screen_size.x;
    float sy = (0.5f - clip.y / w * 0.5f) * v->screen_size.y;
    return br_vec3_make(sx, sy, br_vec3_dot(br_vec3_sub(world, v->position), v->forward));
}

br_vec3 br_camera_view_viewport_to_world(const br_camera_view *v, br_vec3 viewport)
{
    br_vec3 screen = br_vec3_make(v->viewport.x + viewport.x * v->viewport.width,
                                  v->viewport.y + viewport.y * v->viewport.height, viewport.z);
    return br_camera_view_screen_to_world(v, screen);
}

/* ---- pure helpers */

static float br_camera_follow_axis(float camera, float target, float dead_zone)
{
    float d = target - camera;
    if (SDL_fabsf(d) <= dead_zone) {
        return camera;
    }
    return d > 0.0f ? target - dead_zone : target + dead_zone;
}

br_vec2 br_camera_follow_step(br_vec2 camera, br_vec2 target, br_vec2 dead_zone, float damping, float dt)
{
    br_vec2 desired = br_vec2_make(br_camera_follow_axis(camera.x, target.x, dead_zone.x),
                                   br_camera_follow_axis(camera.y, target.y, dead_zone.y));
    float k = damping > 0.0f ? 1.0f - SDL_expf(-dt / damping) : 1.0f;
    return br_vec2_make(camera.x + (desired.x - camera.x) * k, camera.y + (desired.y - camera.y) * k);
}

static float br_camera_clamp_axis(float c, float half, float lo, float size)
{
    if (size <= 2.0f * half) {
        return lo + size * 0.5f;
    }
    return br_clampf(c, lo + half, lo + size - half);
}

br_vec2 br_camera_clamp_to_bounds(br_vec2 camera, br_vec2 half, br_rect bounds)
{
    return br_vec2_make(br_camera_clamp_axis(camera.x, half.x, bounds.x, bounds.width),
                        br_camera_clamp_axis(camera.y, half.y, bounds.y, bounds.height));
}

br_vec3 br_camera_shake_sample(const br_camera_shake *s)
{
    float t = br_clampf(s->trauma, 0.0f, 1.0f);
    float k = t * t;
    float x = s->time * (s->frequency > 0.0f ? s->frequency : 15.0f);
    return br_vec3_make(k * s->max_offset * br_noise_perlin2(x, 0.5f, s->seed),
                        k * s->max_offset * br_noise_perlin2(x, 0.5f, s->seed + 1u),
                        k * s->max_angle * br_noise_perlin2(x, 0.5f, s->seed + 2u));
}

br_vec2 br_camera_anchor_point(br_rect area, br_rect safe, br_vec2 anchor, bool use_safe)
{
    br_rect r = area;
    if (use_safe) {
        br_rect i = br_rect_intersection(area, safe);
        if (!br_rect_is_empty(i)) {
            r = i;
        }
    }
    return br_vec2_make(r.x + anchor.x * r.width, r.y + anchor.y * r.height);
}

/* ---- the node type */

static br_camera_data *br_camera_data_get(br_engine *e, br_node cam)
{
    if (e->camera == NULL) {
        return NULL;
    }
    return br_node_data(e, cam, e->camera->type);
}

static void br_camera_on_create(br_engine *e, br_node n, void *data)
{
    br_camera_data *d = data;
    d->camera = br_camera_default_2d();
    br_array_push(&e->camera->cameras, &n);
}

static void br_camera_on_destroy(br_engine *e, br_node n, void *data)
{
    br_camera_data *d = data;
    br_camera_state *s = e->camera;
    for (uint32_t i = 0; i < s->cameras.count; i++) {
        if (br_node_equal(*(br_node *)br_array_at(&s->cameras, i), n)) {
            br_array_remove_ordered(&s->cameras, i);
            break;
        }
    }
    if (d->is_3d) {
        br_node3d_destroy(e, d->rig);
        br_node_destroy(e, d->view3d);
    }
}

uint32_t br_camera_type(br_engine *e)
{
    return e->camera != NULL ? e->camera->type : 0;
}

br_node br_camera_create(br_engine *e, const br_camera *desc)
{
    br_node n = br_node_create(e, br_camera_type(e));
    br_camera_data *d = br_camera_data_get(e, n);
    if (d != NULL && desc != NULL) {
        d->camera = *desc;
    }
    return n;
}

br_node br_camera_create_3d(br_engine *e, br_node3d scene_root, const br_camera *desc)
{
    br_node none = { 0, 0 };
    if (!br_node3d_valid(e, scene_root)) {
        br_log_error("br_camera_create_3d: invalid scene root");
        return none;
    }
    br_node view = br_view3d_create(e, 16.0f, 16.0f);
    if (!br_node_valid(e, view)) {
        return none;
    }
    br_node n = br_node_create(e, br_camera_type(e));
    br_camera_data *d = br_camera_data_get(e, n);
    if (d == NULL) {
        br_node_destroy(e, view);
        return none;
    }
    d->camera = desc != NULL ? *desc : br_camera_default_3d();
    d->is_3d = true;
    d->view3d = view;
    d->rig = br_node3d_create_container(e);
    d->node3d = br_camera3d_create(e, NULL);
    br_node3d_set_label(e, d->rig, "camera_rig");
    br_node3d_add_child(e, d->rig, d->node3d);
    br_node3d_add_child(e, scene_root, d->rig);
    br_view3d_set_root(e, view, scene_root);
    br_view3d_set_camera(e, view, d->node3d);
    return n;
}

br_node br_camera_create_logical(br_engine *e, float width, float height)
{
    br_camera c = br_camera_default_2d();
    c.orthographic_size = height * 0.5f;
    c.fit = BR_CAMERA_FIT_LETTERBOX;
    c.reference_aspect = height > 0.0f ? width / height : 16.0f / 9.0f;
    br_node n = br_camera_create(e, &c);
    br_node_set_position(e, n, br_vec2_make(width * 0.5f, height * 0.5f));
    br_node_add_child(e, br_stage_root(e), n);
    return n;
}

br_camera *br_camera_get(br_engine *e, br_node cam)
{
    br_camera_data *d = br_camera_data_get(e, cam);
    return d != NULL ? &d->camera : NULL;
}

bool br_camera_is_3d(br_engine *e, br_node cam)
{
    br_camera_data *d = br_camera_data_get(e, cam);
    return d != NULL && d->is_3d;
}

br_node3d br_camera_node3d(br_engine *e, br_node cam)
{
    br_node3d none = { 0, 0 };
    br_camera_data *d = br_camera_data_get(e, cam);
    return d != NULL && d->is_3d ? d->rig : none;
}

br_node br_camera_view3d(br_engine *e, br_node cam)
{
    br_node none = { 0, 0 };
    br_camera_data *d = br_camera_data_get(e, cam);
    return d != NULL && d->is_3d ? d->view3d : none;
}

/* ---- views on the current window */

/* Screen size (units) a camera renders to, and pixels per unit. False if nothing to draw on. */
static bool br_camera_screen_of(br_engine *e, const br_camera *c, br_vec2 *size, float *density)
{
    if (c->target_texture.generation != 0) {
        br_texture_info info;
        if (!br_texture_get_info(e, c->target_texture, &info) || info.width <= 0.0f || info.height <= 0.0f) {
            return false;
        }
        *size = br_vec2_make(info.width, info.height);
        *density = info.resolution > 0.0f ? info.resolution : 1.0f;
        return true;
    }
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    if ((w <= 0 || h <= 0) && e->camera->headless_size.x > 0.0f && e->camera->headless_size.y > 0.0f) {
        *size = e->camera->headless_size;
        *density = 1.0f;
        return true;
    }
    if (w <= 0 || h <= 0) {
        return false;
    }
    *size = br_vec2_make((float)w, (float)h);
    *density = br_engine_pixel_density(e);
    return true;
}

void br_camera_set_headless_screen(br_engine *e, float width, float height)
{
    if (e->camera != NULL) {
        e->camera->headless_size = br_vec2_make(width, height);
    }
}

/* World matrix the view is computed from (shake included for 2D; 3D shake sits in the rig). */
static br_mat4 br_camera_world(br_engine *e, br_node cam, const br_camera_data *d)
{
    if (d->is_3d) {
        return br_node3d_world_transform(e, d->node3d);
    }
    br_mat3 m = br_node_world_transform(e, cam);
    br_vec2 pos = br_vec2_make(m.tx + d->shake.x, m.ty + d->shake.y);
    float angle = SDL_atan2f(m.b, m.a) + d->shake.z;
    return br_mat4_from_mat3(br_mat3_mul(br_mat3_translation(pos.x, pos.y), br_mat3_rotation(angle)));
}

static br_camera_view br_camera_view_now(br_engine *e, br_node cam, const br_camera_data *d)
{
    br_vec2 size = br_vec2_make(1.0f, 1.0f);
    float density = 1.0f;
    br_camera_screen_of(e, &d->camera, &size, &density);
    return br_camera_compute_view(&d->camera, br_camera_world(e, cam, d), d->is_3d, size, density);
}

bool br_camera_get_view(br_engine *e, br_node cam, br_camera_view *out)
{
    br_camera_data *d = br_camera_data_get(e, cam);
    if (d == NULL) {
        SDL_zero(*out);
        return false;
    }
    *out = br_camera_view_now(e, cam, d);
    return true;
}

br_vec3 br_camera_screen_to_world(br_engine *e, br_node cam, br_vec3 screen)
{
    br_camera_view v;
    br_camera_get_view(e, cam, &v);
    return br_camera_view_screen_to_world(&v, screen);
}

br_vec3 br_camera_world_to_screen(br_engine *e, br_node cam, br_vec3 world)
{
    br_camera_view v;
    br_camera_get_view(e, cam, &v);
    return br_camera_view_world_to_screen(&v, world);
}

br_ray3d br_camera_screen_point_to_ray(br_engine *e, br_node cam, br_vec2 screen)
{
    br_camera_view v;
    br_camera_get_view(e, cam, &v);
    return br_camera_view_screen_to_ray(&v, screen);
}

br_vec3 br_camera_viewport_to_world(br_engine *e, br_node cam, br_vec3 viewport)
{
    br_camera_view v;
    br_camera_get_view(e, cam, &v);
    return br_camera_view_viewport_to_world(&v, viewport);
}

br_rect br_camera_safe_area(br_engine *e)
{
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    br_rect full = br_rect_make(0.0f, 0.0f, (float)w, (float)h);
    SDL_Window *window = br_engine_window(e);
    SDL_Rect safe;
    if (window == NULL || !SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 || safe.h <= 0) {
        return full;
    }
    return br_rect_make((float)safe.x, (float)safe.y, (float)safe.w, (float)safe.h);
}

/* ---- pointer mapping */

/* True if a camera with this mask draws n (bits inherited from the nearest ancestor that has
 * some; the stage is layer 1). */
static bool br_camera_draws(br_engine *e, br_node n, uint32_t mask)
{
    while (br_node_valid(e, n)) {
        uint32_t bits = br_node_layer_bits(e, n);
        if (bits != 0) {
            return (bits & mask) != 0;
        }
        n = br_node_parent(e, n);
    }
    return (mask & 1u) != 0;
}

br_vec2 br_camera_screen_to_stage(br_engine *e, br_vec2 screen)
{
    br_camera_state *s = e->camera;
    if (s == NULL || s->stage_count == 0) {
        return screen;
    }
    bool have_fallback = false;
    br_vec2 fallback = screen;
    for (uint32_t k = s->stage_count; k > 0; k--) { /* top-most first */
        const br_camera_slot *slot = br_array_at(&s->slots, s->stage_first + k - 1);
        if (!br_rect_contains(slot->view.viewport, screen.x, screen.y)) {
            continue;
        }
        br_vec2 p = br_mat3_apply(slot->view.world_from_screen, screen);
        br_node hit = br_events_hit_test(e, p);
        if (br_node_valid(e, hit) && br_camera_draws(e, hit, slot->culling_mask)) {
            return p;
        }
        if (!have_fallback) {
            have_fallback = true;
            fallback = p;
        }
    }
    return fallback;
}

br_vec2 br_camera_stage_to_screen(br_engine *e, br_node n, br_vec2 stage)
{
    br_camera_state *s = e->camera;
    if (s == NULL || s->stage_count == 0) {
        return stage;
    }
    for (uint32_t k = s->stage_count; k > 0; k--) {
        const br_camera_slot *slot = br_array_at(&s->slots, s->stage_first + k - 1);
        if (br_node_valid(e, n) && !br_camera_draws(e, n, slot->culling_mask)) {
            continue;
        }
        br_vec2 p = br_mat3_apply(slot->view.screen_from_world, stage);
        if (br_rect_contains(slot->view.viewport, p.x, p.y)) {
            return p;
        }
    }
    return stage;
}

/* ---- systems */

static void br_camera_follow_system(br_engine *e, float dt, void *user)
{
    br_camera_state *s = user;
    uint32_t count = br_component_count(e, s->follow_id);
    for (uint32_t i = 0; i < count; i++) {
        br_camera_follow *f = br_component_at(e, s->follow_id, i);
        br_node cam = br_component_owner_at(e, s->follow_id, i);
        br_camera_data *d = br_camera_data_get(e, cam);
        if (d == NULL) {
            continue;
        }
        if (d->is_3d) {
            if (!br_node3d_valid(e, f->target3d)) {
                continue;
            }
            br_vec3 target = br_node3d_world_position(e, f->target3d);
            br_vec3 goal = br_vec3_add(target, f->offset3d);
            br_vec3 pos = br_node3d_position(e, d->rig);
            float k = f->damping > 0.0f ? 1.0f - SDL_expf(-dt / f->damping) : 1.0f;
            pos = br_vec3_add(pos, br_vec3_scale(br_vec3_sub(goal, pos), k));
            br_node3d_set_position(e, d->rig, pos);
            br_node3d_look_at(e, d->rig, target, br_vec3_make(0.0f, 1.0f, 0.0f));
            continue;
        }
        if (!br_node_valid(e, f->target)) {
            continue;
        }
        br_vec2 target = br_vec2_add(br_node_to_global(e, f->target, br_vec2_make(0.0f, 0.0f)), f->offset);
        br_mat3 world = br_node_world_transform(e, cam);
        br_vec2 pos = br_camera_follow_step(br_vec2_make(world.tx, world.ty), target, f->dead_zone, f->damping, dt);
        if (f->use_bounds) {
            br_camera_view v = br_camera_view_now(e, cam, d);
            /* the visible box's half size (rotation included) */
            pos = br_camera_clamp_to_bounds(pos, br_vec2_make(v.visible.width * 0.5f, v.visible.height * 0.5f), f->bounds);
        }
        br_node parent = br_node_parent(e, cam);
        br_vec2 local = br_node_valid(e, parent) ? br_node_to_local(e, parent, pos) : pos;
        br_node_set_position(e, cam, local);
    }
}

static void br_camera_shake_system(br_engine *e, float dt, void *user)
{
    br_camera_state *s = user;
    uint32_t count = br_component_count(e, s->shake_id);
    for (uint32_t i = 0; i < count; i++) {
        br_camera_shake *sh = br_component_at(e, s->shake_id, i);
        br_node cam = br_component_owner_at(e, s->shake_id, i);
        br_camera_data *d = br_camera_data_get(e, cam);
        if (d == NULL) {
            continue;
        }
        if (sh->seed == 0) {
            sh->seed = br_random_u32(br_random_engine(e)) | 1u; /* seeded engine RNG: deterministic runs */
        }
        sh->time += dt;
        d->shake = br_camera_shake_sample(sh);
        sh->trauma = SDL_max(0.0f, sh->trauma - (sh->decay > 0.0f ? sh->decay : 1.0f) * dt);
        if (d->is_3d) {
            br_node3d_set_position(e, d->node3d, br_vec3_make(d->shake.x, d->shake.y, 0.0f));
            br_node3d_set_euler(e, d->node3d, br_vec3_make(0.0f, 0.0f, d->shake.z));
        }
    }
}

br_camera_orbit br_camera_orbit_make(br_vec3 target, float distance, float yaw, float pitch)
{
    br_camera_orbit o;
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

/* Left-drag turns, the wheel zooms, then the rig sits on the orbit looking at its target. */
static void br_camera_orbit_update(br_engine *e, br_camera_orbit *orbit, br_node3d camera)
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
    float cp = SDL_cosf(orbit->pitch);
    br_vec3 offset = br_vec3_make(SDL_sinf(orbit->yaw) * cp, SDL_sinf(orbit->pitch), SDL_cosf(orbit->yaw) * cp);
    br_node3d_set_position(e, camera, br_vec3_add(orbit->target, br_vec3_scale(offset, orbit->distance)));
    br_node3d_look_at(e, camera, orbit->target, br_vec3_make(0.0f, 1.0f, 0.0f));
}

static void br_camera_orbit_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_camera_state *s = user;
    uint32_t count = br_component_count(e, s->orbit_id);
    for (uint32_t i = 0; i < count; i++) {
        br_camera_orbit *o = br_component_at(e, s->orbit_id, i);
        br_camera_data *d = br_camera_data_get(e, br_component_owner_at(e, s->orbit_id, i));
        if (d != NULL && d->is_3d) {
            br_camera_orbit_update(e, o, d->rig);
        }
    }
}

void br_camera_shake_add(br_engine *e, br_node cam, float trauma)
{
    br_camera_data *d = br_camera_data_get(e, cam);
    if (d == NULL) {
        return;
    }
    br_camera_shake *sh = br_component_get(e, cam, e->camera->shake_id);
    if (sh == NULL) {
        sh = br_component_add(e, cam, e->camera->shake_id);
        if (sh == NULL) {
            return;
        }
        sh->decay = 1.0f;
        sh->max_offset = d->is_3d ? 0.3f : 12.0f; /* world units on a 3D camera, stage units in 2D */
        sh->max_angle = 0.05f;
        sh->frequency = 15.0f;
    }
    sh->trauma = br_clampf(sh->trauma + trauma, 0.0f, 1.0f);
}

/* Cameras in draw order: render-texture cameras first, then by depth (stable). */
static void br_camera_sort(br_array *slots)
{
    br_camera_slot *a = slots->data;
    for (uint32_t i = 1; i < slots->count; i++) {
        br_camera_slot key = a[i];
        uint32_t j = i;
        while (j > 0 && (a[j - 1].on_texture < key.on_texture ||
                         (a[j - 1].on_texture == key.on_texture && a[j - 1].depth > key.depth))) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

/* 3D camera: push the settings into its camera3d node and View3D. */
static void br_camera_drive_3d(br_engine *e, br_camera_data *d, const br_camera_view *v)
{
    br_camera3d *c3 = br_camera3d_get(e, d->node3d);
    if (c3 != NULL) {
        bool perspective = d->camera.projection == BR_CAMERA_PERSPECTIVE;
        c3->projection = perspective ? BR_PROJECTION_PERSPECTIVE : BR_PROJECTION_ORTHOGRAPHIC;
        c3->fov_y = 2.0f * SDL_atanf(v->half_height);
        c3->ortho_size = v->half_height;
        /* the same guards as br_camera_compute_view, so drawing and picking agree */
        c3->near_z = d->camera.near_z > 0.0f ? d->camera.near_z : 0.1f;
        c3->far_z = d->camera.far_z > c3->near_z ? d->camera.far_z : c3->near_z + 1000.0f;
        c3->aspect = v->half_width / v->half_height;
    }
    br_view3d_set_size(e, d->view3d, v->viewport.width, v->viewport.height);
    br_view3d_set_culling_mask(e, d->view3d, d->camera.culling_mask);
    br_color bg = br_color_make(0.0f, 0.0f, 0.0f, 0.0f);
    if (d->camera.clear_flags == BR_CAMERA_CLEAR_COLOR) {
        bg = br_color_to_linear(d->camera.background_color);
    }
    br_view3d_set_background(e, d->view3d, bg);
}

/* PRE_RENDER: every camera's view -> render views, cull rect, 3D settings. */
static void br_camera_views_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_camera_state *s = user;
    br_array_clear(&s->slots);
    s->stage_first = 0;
    s->stage_count = 0;
    for (uint32_t i = 0; i < s->cameras.count; i++) {
        br_node cam = *(br_node *)br_array_at(&s->cameras, i);
        br_camera_data *d = br_camera_data_get(e, cam);
        br_vec2 size;
        float density;
        if (d == NULL || !d->camera.enabled || !br_camera_screen_of(e, &d->camera, &size, &density)) {
            continue;
        }
        br_camera_slot slot;
        SDL_zero(slot);
        slot.camera = cam;
        slot.is_3d = d->is_3d;
        slot.on_texture = d->camera.target_texture.generation != 0;
        slot.depth = d->camera.depth;
        slot.culling_mask = d->camera.culling_mask;
        slot.view = br_camera_compute_view(&d->camera, br_camera_world(e, cam, d), d->is_3d, size, density);
        br_array_push(&s->slots, &slot);
    }
    if (s->slots.count == 0) {
        if (s->had_views) {
            br_render_set_views(e, NULL, 0);
            br_scene_set_cull_view(e->scene, NULL);
            s->had_views = false;
        }
        return;
    }
    br_camera_sort(&s->slots);

    br_array_clear(&s->views);
    bool any_2d = false;
    bool have_cull = false;
    br_rect cull = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint32_t i = 0; i < s->slots.count; i++) {
        br_camera_slot *slot = br_array_at(&s->slots, i);
        br_camera_data *d = br_camera_data_get(e, slot->camera);
        const br_camera_view *v = &slot->view;
        br_render_view rv;
        SDL_zero(rv);
        rv.target = d->camera.target_texture;
        rv.viewport = v->viewport;
        br_mat4 screen = br_mat4_ortho(0.0f, v->screen_size.x, v->screen_size.y, 0.0f, -1.0f, 1.0f);
        if (slot->is_3d) {
            br_camera_drive_3d(e, d, v);
            rv.root = d->view3d;
            rv.projection = br_mat4_mul(screen, br_mat4_translation(br_vec3_make(v->viewport.x, v->viewport.y, 0.0f)));
            rv.area = br_rect_make(0.0f, 0.0f, v->viewport.width, v->viewport.height);
            rv.culling_mask = 0xFFFFFFFFu;
        } else {
            rv.projection = v->clip_from_world;
            rv.area = v->visible;
            rv.culling_mask = d->camera.culling_mask;
            rv.clear = d->camera.clear_flags == BR_CAMERA_CLEAR_COLOR;
            rv.clear_color = d->camera.background_color;
            cull = have_cull ? br_rect_union(cull, v->visible) : v->visible;
            have_cull = true;
            any_2d = any_2d || !slot->on_texture;
        }
        br_array_push(&s->views, &rv);
    }
    /* screen 2D cameras for pointer mapping: keep them in their own contiguous run of slots */
    uint32_t base = s->slots.count;
    for (uint32_t i = 0; i < base; i++) {
        br_camera_slot slot = *(br_camera_slot *)br_array_at(&s->slots, i);
        if (!slot.is_3d && !slot.on_texture) {
            br_array_push(&s->slots, &slot);
        }
    }
    s->stage_first = base;
    s->stage_count = s->slots.count - base;

    if (!any_2d) {
        /* only 3D cameras: the stage stays a screen-space overlay on top of them */
        int w = 0;
        int h = 0;
        br_engine_window_size(e, &w, &h);
        br_render_view rv;
        SDL_zero(rv);
        rv.projection = br_mat4_ortho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f);
        rv.area = br_rect_make(0.0f, 0.0f, (float)w, (float)h);
        rv.viewport = rv.area;
        rv.culling_mask = 0xFFFFFFFFu;
        br_array_push(&s->views, &rv);
        cull = have_cull ? br_rect_union(cull, rv.area) : rv.area;
        have_cull = true;
    }
    br_render_set_views(e, s->views.data, s->views.count);
    br_scene_set_cull_view(e->scene, have_cull ? &cull : NULL);
    s->had_views = true;
}

/* PRE_RENDER, after the views: UI nodes anchored to viewports / the safe area. */
static void br_camera_anchor_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_camera_state *s = user;
    uint32_t count = br_component_count(e, s->anchor_id);
    if (count == 0) {
        return;
    }
    br_rect safe = br_camera_safe_area(e);
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    for (uint32_t i = 0; i < count; i++) {
        br_camera_anchor *a = br_component_at(e, s->anchor_id, i);
        br_node n = br_component_owner_at(e, s->anchor_id, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        br_vec2 stage;
        br_camera_data *d = br_camera_data_get(e, a->camera);
        if (d != NULL && !d->is_3d) {
            br_camera_view v = br_camera_view_now(e, a->camera, d);
            br_vec2 p = br_camera_anchor_point(v.viewport, safe, a->anchor, a->safe_area);
            stage = br_mat3_apply(v.world_from_screen, p);
        } else {
            stage = br_camera_anchor_point(br_rect_make(0.0f, 0.0f, (float)w, (float)h), safe, a->anchor, a->safe_area);
        }
        stage = br_vec2_add(stage, a->offset);
        br_node parent = br_node_parent(e, n);
        br_vec2 local = br_node_valid(e, parent) ? br_node_to_local(e, parent, stage) : stage;
        br_node_set_position(e, n, local);
    }
}

/* ---- module */

uint32_t br_camera_follow_component(br_engine *e)
{
    return e->camera != NULL ? e->camera->follow_id : 0;
}

uint32_t br_camera_shake_component(br_engine *e)
{
    return e->camera != NULL ? e->camera->shake_id : 0;
}

uint32_t br_camera_orbit_component(br_engine *e)
{
    return e->camera != NULL ? e->camera->orbit_id : 0;
}

uint32_t br_camera_anchor_component(br_engine *e)
{
    return e->camera != NULL ? e->camera->anchor_id : 0;
}

static void br_camera_shutdown(br_engine *e, void *user)
{
    br_camera_state *s = user;
    br_array_free(&s->cameras);
    br_array_free(&s->slots);
    br_array_free(&s->views);
    br_free(s);
    e->camera = NULL;
}

static void br_camera_register_components(br_engine *e, br_camera_state *s)
{
    static const br_field_desc follow_fields[] = {
        { "target", BR_FIELD_NODE, offsetof(br_camera_follow, target) },
        { "offset", BR_FIELD_VEC2, offsetof(br_camera_follow, offset) },
        { "damping", BR_FIELD_FLOAT, offsetof(br_camera_follow, damping) },
        { "dead_zone", BR_FIELD_VEC2, offsetof(br_camera_follow, dead_zone) },
        { "use_bounds", BR_FIELD_BOOL, offsetof(br_camera_follow, use_bounds) },
        { "offset3d", BR_FIELD_VEC3, offsetof(br_camera_follow, offset3d) },
    };
    static const br_field_desc shake_fields[] = {
        { "trauma", BR_FIELD_FLOAT, offsetof(br_camera_shake, trauma) },
        { "decay", BR_FIELD_FLOAT, offsetof(br_camera_shake, decay) },
        { "max_offset", BR_FIELD_FLOAT, offsetof(br_camera_shake, max_offset) },
        { "max_angle", BR_FIELD_FLOAT, offsetof(br_camera_shake, max_angle) },
        { "frequency", BR_FIELD_FLOAT, offsetof(br_camera_shake, frequency) },
    };
    static const br_field_desc orbit_fields[] = {
        { "target", BR_FIELD_VEC3, offsetof(br_camera_orbit, target) },
        { "distance", BR_FIELD_FLOAT, offsetof(br_camera_orbit, distance) },
        { "yaw", BR_FIELD_FLOAT, offsetof(br_camera_orbit, yaw) },
        { "pitch", BR_FIELD_FLOAT, offsetof(br_camera_orbit, pitch) },
        { "min_distance", BR_FIELD_FLOAT, offsetof(br_camera_orbit, min_distance) },
        { "max_distance", BR_FIELD_FLOAT, offsetof(br_camera_orbit, max_distance) },
        { "rotate_speed", BR_FIELD_FLOAT, offsetof(br_camera_orbit, rotate_speed) },
        { "zoom_speed", BR_FIELD_FLOAT, offsetof(br_camera_orbit, zoom_speed) },
    };
    static const br_field_desc anchor_fields[] = {
        { "camera", BR_FIELD_NODE, offsetof(br_camera_anchor, camera) },
        { "anchor", BR_FIELD_VEC2, offsetof(br_camera_anchor, anchor) },
        { "offset", BR_FIELD_VEC2, offsetof(br_camera_anchor, offset) },
        { "safe_area", BR_FIELD_BOOL, offsetof(br_camera_anchor, safe_area) },
    };
    br_component_desc c;
    c = (br_component_desc){ "camera.follow", sizeof(br_camera_follow), follow_fields, BR_ARRAY_COUNT(follow_fields) };
    s->follow_id = br_component_register(e, &c);
    c = (br_component_desc){ "camera.shake", sizeof(br_camera_shake), shake_fields, BR_ARRAY_COUNT(shake_fields) };
    s->shake_id = br_component_register(e, &c);
    c = (br_component_desc){ "camera.orbit", sizeof(br_camera_orbit), orbit_fields, BR_ARRAY_COUNT(orbit_fields) };
    s->orbit_id = br_component_register(e, &c);
    c = (br_component_desc){ "camera.anchor", sizeof(br_camera_anchor), anchor_fields, BR_ARRAY_COUNT(anchor_fields) };
    s->anchor_id = br_component_register(e, &c);
}

void br_camera_register(br_engine *e)
{
    br_camera_state *s = br_alloc(sizeof(br_camera_state));
    if (s == NULL) {
        return;
    }
    e->camera = s;
    br_array_init(&s->cameras, sizeof(br_node));
    br_array_init(&s->slots, sizeof(br_camera_slot));
    br_array_init(&s->views, sizeof(br_render_view));
    br_node_type_desc d;
    SDL_zero(d);
    d.name = "camera";
    d.data_size = sizeof(br_camera_data);
    d.on_create = br_camera_on_create;
    d.on_destroy = br_camera_on_destroy;
    s->type = br_node_type_register(e, &d);
    br_camera_register_components(e, s);
    br_engine_on_shutdown(e, br_camera_shutdown, s);
    br_system_add(e, "camera.orbit", BR_PHASE_UPDATE, 0, br_camera_orbit_system, s);
    br_system_add(e, "camera.follow", BR_PHASE_LATE_UPDATE, 100, br_camera_follow_system, s);
    br_system_add(e, "camera.shake", BR_PHASE_LATE_UPDATE, 200, br_camera_shake_system, s);
    br_system_add(e, "camera.views", BR_PHASE_PRE_RENDER, -1100, br_camera_views_system, s);
    br_system_add(e, "camera.anchors", BR_PHASE_PRE_RENDER, -1050, br_camera_anchor_system, s);
}
