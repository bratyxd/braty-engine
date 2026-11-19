/* br_node_types3d: the built-in 3D node types, registered through the public 3D type registry
 * exactly like another module would: mesh3d, batched_mesh3d, light3d, camera3d. */
#include "three_d/br_three_d_internal.h"

/* ---- shared picking: ray (geometry space) vs bounds, then triangles */

static bool br_pick_geometry(br_engine *e, br_geometry3d geometry, br_ray3d ray, br_pick3d_hit *hit)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, geometry);
    float t_box = 0.0f;
    if (d == NULL || !br_ray3d_aabb(ray, d->bounds, &t_box) || t_box > hit->distance) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i + 2 < d->index_count; i += 3) {
        br_vec3 a = d->vertices[d->indices[i]].position;
        br_vec3 b = d->vertices[d->indices[i + 1]].position;
        br_vec3 c = d->vertices[d->indices[i + 2]].position;
        float t = 0.0f;
        if (br_ray3d_triangle(ray, a, b, c, &t, NULL, NULL) && t < hit->distance) {
            hit->distance = t;
            hit->point = br_vec3_add(ray.origin, br_vec3_scale(ray.direction, t));
            hit->normal = br_vec3_normalize(br_vec3_cross(br_vec3_sub(b, a), br_vec3_sub(c, a)));
            hit->triangle = i / 3;
            found = true;
        }
    }
    return found;
}

/* ---- mesh3d */

static void br_mesh3d_on_create(br_engine *e, br_node3d n, void *data)
{
    (void)e;
    (void)n;
    br_mesh3d *m = data;
    m->color = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    m->cast_shadows = true;
}

static bool br_mesh3d_bounds(br_engine *e, br_node3d n, const void *data, br_aabb *out)
{
    (void)n;
    const br_mesh3d *m = data;
    br_geometry3d_data *d = br_geometry3d_data_get(e, m->geometry);
    if (d == NULL) {
        return false;
    }
    *out = d->bounds;
    return true;
}

static void br_mesh3d_collect(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx)
{
    const br_mesh3d *m = data;
    br_geometry3d_data *d = br_geometry3d_data_get(e, m->geometry);
    if (d == NULL || !br_material3d_valid(e, m->material)) {
        return;
    }
    br_instance3d instance;
    instance.model = br_render3d_world(ctx, n);
    instance.color = m->color;
    br_draw3d draw;
    SDL_zero(draw);
    draw.geometry = m->geometry;
    draw.material = m->material;
    draw.instances = &instance;
    draw.instance_count = 1;
    draw.world_bounds = br_aabb_transform(d->bounds, instance.model);
    draw.cast_shadows = m->cast_shadows;
    if (m->joint_matrices != NULL && m->joint_count > 0) {
        draw.joint_matrices = m->joint_matrices;
        draw.joint_count = m->joint_count;
        /* ponytail: skinned bounds = bind pose grown 2x around its centre; joints that move
         * vertices further get culled early. Task 11 can pass real joint bounds. */
        br_vec3 c = br_vec3_scale(br_vec3_add(draw.world_bounds.min, draw.world_bounds.max), 0.5f);
        br_vec3 h = br_vec3_sub(draw.world_bounds.max, c);
        draw.world_bounds.min = br_vec3_sub(c, br_vec3_scale(h, 2.0f));
        draw.world_bounds.max = br_vec3_add(c, br_vec3_scale(h, 2.0f));
    }
    if (m->morph_weight_count > 0 && d->morph_target_count > 0) {
        draw.morph_weights = m->morph_weights;
        draw.morph_weight_count = m->morph_weight_count;
    }
    br_render3d_submit(ctx, &draw);
}

static bool br_mesh3d_pick(br_engine *e, br_node3d n, const void *data, br_ray3d ray, br_pick3d_hit *hit)
{
    (void)n;
    const br_mesh3d *m = data;
    return br_pick_geometry(e, m->geometry, ray, hit);
}

uint32_t br_mesh3d_type(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->mesh_type : 0;
}

br_node3d br_mesh3d_create(br_engine *e, br_geometry3d geometry, br_material3d material)
{
    br_node3d n = br_node3d_create(e, br_mesh3d_type(e));
    br_mesh3d *m = br_mesh3d_get(e, n);
    if (m != NULL) {
        m->geometry = geometry;
        m->material = material;
    }
    return n;
}

br_mesh3d *br_mesh3d_get(br_engine *e, br_node3d n)
{
    return br_node3d_data(e, n, br_mesh3d_type(e));
}

/* ---- batched_mesh3d */

static void br_batched_on_create(br_engine *e, br_node3d n, void *data)
{
    (void)e;
    (void)n;
    br_batched_mesh3d_data *b = data;
    b->store = br_alloc(sizeof(br_batched3d));
    if (b->store != NULL) {
        br_array_init(&b->store->instances, sizeof(br_instance3d));
        b->store->bounds = br_aabb_empty();
        b->store->cast_shadows = true;
    }
}

static void br_batched_on_destroy(br_engine *e, br_node3d n, void *data)
{
    (void)e;
    (void)n;
    br_batched_mesh3d_data *b = data;
    if (b->store != NULL) {
        br_array_free(&b->store->instances);
        br_free(b->store);
        b->store = NULL;
    }
}

static bool br_batched_bounds(br_engine *e, br_node3d n, const void *data, br_aabb *out)
{
    (void)e;
    (void)n;
    const br_batched_mesh3d_data *b = data;
    if (b->store == NULL || !br_aabb_is_valid(b->store->bounds)) {
        return false;
    }
    *out = b->store->bounds;
    return true;
}

static void br_batched_collect(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx)
{
    const br_batched_mesh3d_data *b = data;
    if (b->store == NULL || b->store->instances.count == 0 || !br_geometry3d_valid(e, b->geometry) ||
        !br_material3d_valid(e, b->material)) {
        return;
    }
    br_mat4 world = br_render3d_world(ctx, n);
    br_draw3d draw;
    SDL_zero(draw);
    draw.geometry = b->geometry;
    draw.material = b->material;
    draw.instances = b->store->instances.data;
    draw.instance_count = b->store->instances.count;
    draw.world_bounds = br_aabb_transform(b->store->bounds, world);
    draw.cast_shadows = b->store->cast_shadows;
    /* ponytail: culled as one box, no per-instance culling; split big fields into several
     * batched meshes (or add per-instance culling here) when fill rate matters */
    br_render3d_submit_with_parent(ctx, &draw, &world);
}

static bool br_batched_pick(br_engine *e, br_node3d n, const void *data, br_ray3d ray, br_pick3d_hit *hit)
{
    (void)n;
    const br_batched_mesh3d_data *b = data;
    br_geometry3d_data *d = br_geometry3d_data_get(e, b->geometry);
    if (b->store == NULL || d == NULL) {
        return false;
    }
    float t = 0.0f;
    if (!br_ray3d_aabb(ray, b->store->bounds, &t) || t > hit->distance) {
        return false;
    }
    bool found = false;
    const br_instance3d *inst = b->store->instances.data;
    for (uint32_t i = 0; i < b->store->instances.count; i++) {
        if (!br_ray3d_aabb(ray, br_aabb_transform(d->bounds, inst[i].model), &t) || t > hit->distance) {
            continue;
        }
        bool ok = false;
        br_mat4 inverse = br_mat4_invert(inst[i].model, &ok);
        if (!ok) {
            continue;
        }
        br_ray3d local;
        local.origin = br_mat4_transform_point(inverse, ray.origin);
        local.direction = br_mat4_transform_dir(inverse, ray.direction); /* not normalised: same t */
        br_pick3d_hit h = *hit;
        if (br_pick_geometry(e, b->geometry, local, &h)) {
            h.point = br_mat4_transform_point(inst[i].model, h.point);
            h.normal = br_vec3_normalize(br_mat4_transform_dir(inst[i].model, h.normal));
            h.instance = i;
            *hit = h;
            found = true;
        }
    }
    return found;
}

static br_batched3d *br_batched_store(br_engine *e, br_node3d n)
{
    br_batched_mesh3d_data *b = br_node3d_data(e, n, br_batched_mesh3d_type(e));
    return b != NULL ? b->store : NULL;
}

uint32_t br_batched_mesh3d_type(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->batched_type : 0;
}

br_node3d br_batched_mesh3d_create(br_engine *e, br_geometry3d geometry, br_material3d material)
{
    br_node3d n = br_node3d_create(e, br_batched_mesh3d_type(e));
    br_batched_mesh3d_data *b = br_node3d_data(e, n, br_batched_mesh3d_type(e));
    if (b != NULL) {
        b->geometry = geometry;
        b->material = material;
    }
    return n;
}

uint32_t br_batched_mesh3d_add(br_engine *e, br_node3d n, br_mat4 transform, br_color color)
{
    br_batched3d *store = br_batched_store(e, n);
    br_batched_mesh3d_data *b = br_node3d_data(e, n, br_batched_mesh3d_type(e));
    if (store == NULL) {
        return UINT32_MAX;
    }
    br_instance3d inst = { transform, color };
    if (br_array_push(&store->instances, &inst) == NULL) {
        return UINT32_MAX;
    }
    store->bounds = br_aabb_union(store->bounds, br_aabb_transform(br_geometry3d_bounds(e, b->geometry), transform));
    return store->instances.count - 1;
}

br_instance3d *br_batched_mesh3d_instances(br_engine *e, br_node3d n, uint32_t *count)
{
    br_batched3d *store = br_batched_store(e, n);
    if (count != NULL) {
        *count = store != NULL ? store->instances.count : 0;
    }
    return store != NULL ? store->instances.data : NULL;
}

void br_batched_mesh3d_update(br_engine *e, br_node3d n)
{
    br_batched3d *store = br_batched_store(e, n);
    br_batched_mesh3d_data *b = br_node3d_data(e, n, br_batched_mesh3d_type(e));
    if (store == NULL) {
        return;
    }
    br_aabb local = br_geometry3d_bounds(e, b->geometry);
    store->bounds = br_aabb_empty();
    const br_instance3d *inst = store->instances.data;
    for (uint32_t i = 0; i < store->instances.count; i++) {
        store->bounds = br_aabb_union(store->bounds, br_aabb_transform(local, inst[i].model));
    }
}

void br_batched_mesh3d_clear(br_engine *e, br_node3d n)
{
    br_batched3d *store = br_batched_store(e, n);
    if (store != NULL) {
        br_array_clear(&store->instances);
        store->bounds = br_aabb_empty();
    }
}

void br_batched_mesh3d_set_cast_shadows(br_engine *e, br_node3d n, bool cast)
{
    br_batched3d *store = br_batched_store(e, n);
    if (store != NULL) {
        store->cast_shadows = cast;
    }
}

/* ---- light3d */

br_light3d br_light3d_default(br_light3d_kind kind)
{
    br_light3d l;
    SDL_zero(l);
    l.kind = kind;
    l.color = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    l.intensity = kind == BR_LIGHT3D_DIRECTIONAL ? 1.0f : 3.0f;
    l.range = kind == BR_LIGHT3D_DIRECTIONAL ? 0.0f : 20.0f;
    l.inner_angle = 20.0f * BR_DEG_TO_RAD;
    l.outer_angle = 30.0f * BR_DEG_TO_RAD;
    l.shadow_bias = kind == BR_LIGHT3D_DIRECTIONAL ? 0.002f : 0.0005f;
    l.shadow_normal_bias = 0.03f;
    l.cascade_count = 3;
    l.shadow_distance = 60.0f;
    l.cascade_lambda = 0.75f;
    return l;
}

static void br_light3d_collect(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx)
{
    (void)e;
    br_render3d_add_light(ctx, data, br_render3d_world(ctx, n));
}

uint32_t br_light3d_type(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->light_type : 0;
}

br_node3d br_light3d_create(br_engine *e, const br_light3d *light)
{
    br_node3d n = br_node3d_create(e, br_light3d_type(e));
    br_light3d *l = br_light3d_get(e, n);
    if (l != NULL) {
        *l = light != NULL ? *light : br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    }
    return n;
}

br_light3d *br_light3d_get(br_engine *e, br_node3d n)
{
    return br_node3d_data(e, n, br_light3d_type(e));
}

/* ---- camera3d */

static void br_camera3d_on_create(br_engine *e, br_node3d n, void *data)
{
    (void)e;
    (void)n;
    *(br_camera3d *)data = br_camera3d_perspective(60.0f * BR_DEG_TO_RAD, 0.1f, 500.0f);
}

uint32_t br_camera3d_type(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->camera_type : 0;
}

br_node3d br_camera3d_create(br_engine *e, const br_camera3d *camera)
{
    br_node3d n = br_node3d_create(e, br_camera3d_type(e));
    br_camera3d *c = br_camera3d_get(e, n);
    if (c != NULL && camera != NULL) {
        *c = *camera;
    }
    return n;
}

br_camera3d *br_camera3d_get(br_engine *e, br_node3d n)
{
    return br_node3d_data(e, n, br_camera3d_type(e));
}

/* ---- registration */

void br_node_types3d_register(br_engine *e)
{
    br_three_d_state *s = e->three_d;
    br_node3d_type_desc d;

    SDL_zero(d);
    d.name = "mesh3d";
    d.data_size = sizeof(br_mesh3d);
    d.on_create = br_mesh3d_on_create;
    d.local_bounds = br_mesh3d_bounds;
    d.collect = br_mesh3d_collect;
    d.pick = br_mesh3d_pick;
    s->mesh_type = br_node3d_type_register(e, &d);

    SDL_zero(d);
    d.name = "batched_mesh3d";
    d.data_size = sizeof(br_batched_mesh3d_data);
    d.on_create = br_batched_on_create;
    d.on_destroy = br_batched_on_destroy;
    d.local_bounds = br_batched_bounds;
    d.collect = br_batched_collect;
    d.pick = br_batched_pick;
    s->batched_type = br_node3d_type_register(e, &d);

    SDL_zero(d);
    d.name = "light3d";
    d.data_size = sizeof(br_light3d);
    d.collect = br_light3d_collect;
    s->light_type = br_node3d_type_register(e, &d);

    SDL_zero(d);
    d.name = "camera3d";
    d.data_size = sizeof(br_camera3d);
    d.on_create = br_camera3d_on_create;
    s->camera_type = br_node3d_type_register(e, &d);
}
