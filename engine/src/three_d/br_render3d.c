/* br_render3d: per-view collect (camera, culling, draw items, sorting, lights, shadow tiles) in
 * PRE_RENDER, and the RENDER callback that runs the shadow pass, the main HDR pass and post. */
#include "three_d/br_three_d_internal.h"

#include "mesh3d.frag.h"
#include "mesh3d.vert.h"
#include "mesh3d_deform.vert.h"
#include "post3d.vert.h"
#include "post3d_down.frag.h"
#include "post3d_tonemap.frag.h"
#include "post3d_up.frag.h"
#include "shadow3d.frag.h"

#define BR_HDR_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
#define BR_SHADOW_FORMAT SDL_GPU_TEXTUREFORMAT_R32_FLOAT

/* Fragment uniform b1 of mesh3d.frag.hlsl. */
typedef struct br_material3d_uniforms {
    float    base_color[4];
    float    emissive[4];
    float    pbr[4];
    float    specular[4];
    float    alpha[4];
    uint32_t flags[4];
    float    uv[BR_MATERIAL3D_SLOT_COUNT * 2][4];
} br_material3d_uniforms;

/* Vertex uniform b1 of mesh3d_deform.vert.hlsl. */
typedef struct br_deform3d_uniforms {
    float    morph_weights[BR_MESH3D_MAX_MORPH_WEIGHTS];
    uint32_t deform[4];
} br_deform3d_uniforms;

/* Fragment uniform b0 of shadow3d.frag.hlsl. */
typedef struct br_shadow_mask_uniforms {
    float mask[4];
    float uv[2][4];
} br_shadow_mask_uniforms;

/* ---- collect: submission API used by node types */

br_mat4 br_render3d_world(br_render3d_ctx *ctx, br_node3d n)
{
    br_three_d_state *s = ctx->e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return br_mat4_identity();
    }
    return s->nodes.world[n.index];
}

br_mat4 br_render3d_view_projection(br_render3d_ctx *ctx)
{
    return ctx->view->view_projection;
}

br_vec3 br_render3d_camera_position(br_render3d_ctx *ctx)
{
    return ctx->view->camera_position;
}

bool br_render3d_visible(br_render3d_ctx *ctx, br_aabb world_bounds)
{
    return br_frustum_test_aabb(&ctx->frustum, world_bounds);
}

br_node br_render3d_view(br_render3d_ctx *ctx)
{
    return ctx->view->node;
}

static float br_item_depth(const br_view3d_state *v, br_vec3 p)
{
    return br_vec3_dot(br_vec3_sub(p, v->camera_position), v->camera_forward);
}

void br_render3d_submit_with_parent(br_render3d_ctx *ctx, const br_draw3d *draw, const br_mat4 *parent)
{
    br_engine *e = ctx->e;
    br_view3d_state *v = ctx->view;
    br_gpu *g = br_engine_gpu(e);
    br_material3d_desc *mat = br_material3d_params(e, draw->material);
    br_geometry3d_data *geo = br_geometry3d_data_get(e, draw->geometry);
    if (g == NULL || mat == NULL || geo == NULL || draw->instances == NULL || draw->instance_count == 0) {
        return;
    }
    uint32_t passes = mat->alpha_mode == BR_ALPHA_BLEND ? BR_PASS3D_TRANSPARENT : BR_PASS3D_OPAQUE;
    if (!br_frustum_test_aabb(&ctx->frustum, draw->world_bounds)) {
        passes = 0;
    }
    if (draw->cast_shadows) {
        passes |= BR_PASS3D_SHADOW;
    }
    if (passes == 0) {
        return;
    }
    br_item3d item;
    SDL_zero(item);
    item.passes = passes;
    item.geometry = draw->geometry;
    item.material = draw->material;
    item.instance_count = draw->instance_count;
    br_instance3d *dst = br_gpu_transient_alloc_vertices(g, draw->instance_count * (uint32_t)sizeof(br_instance3d),
                                                         &item.instance_offset);
    if (dst == NULL) {
        return;
    }
    if (parent != NULL) {
        for (uint32_t i = 0; i < draw->instance_count; i++) {
            dst[i].model = br_mat4_mul(*parent, draw->instances[i].model);
            dst[i].color = draw->instances[i].color;
        }
    } else {
        SDL_memcpy(dst, draw->instances, sizeof(br_instance3d) * draw->instance_count);
    }
    if (draw->joint_matrices != NULL && draw->joint_count > 0) {
        item.joint_offset = v->joints.count;
        item.joint_count = draw->joint_count;
        br_array_push_n(&v->joints, draw->joint_matrices, draw->joint_count);
    }
    if (draw->morph_weights != NULL && geo->morph_target_count > 0) {
        uint32_t count = SDL_min(draw->morph_weight_count, geo->morph_target_count);
        count = SDL_min(count, (uint32_t)BR_MESH3D_MAX_MORPH_WEIGHTS);
        for (uint32_t i = 0; i < count; i++) {
            item.morph[i] = draw->morph_weights[i];
        }
        item.morph_count = count;
    }
    br_vec3 center = br_vec3_scale(br_vec3_add(draw->world_bounds.min, draw->world_bounds.max), 0.5f);
    item.depth = br_item_depth(v, center);
    if (passes & BR_PASS3D_SHADOW) {
        v->caster_bounds = br_aabb_union(v->caster_bounds, draw->world_bounds);
    }
    if (passes & (BR_PASS3D_OPAQUE | BR_PASS3D_TRANSPARENT)) {
        v->stats.instances += draw->instance_count;
    }
    br_array_push(&v->items, &item);
}

void br_render3d_submit(br_render3d_ctx *ctx, const br_draw3d *draw)
{
    br_render3d_submit_with_parent(ctx, draw, NULL);
}

void br_render3d_submit_custom(br_render3d_ctx *ctx, uint32_t passes, br_vec3 sort_point, br_draw3d_custom_fn fn,
                               void *user)
{
    if (fn == NULL || passes == 0) {
        return;
    }
    br_item3d item;
    SDL_zero(item);
    item.passes = passes;
    item.depth = br_item_depth(ctx->view, sort_point);
    item.custom_fn = fn;
    item.custom_user = user;
    br_array_push(&ctx->view->items, &item);
}

void br_render3d_add_light(br_render3d_ctx *ctx, const br_light3d *light, br_mat4 world)
{
    br_view3d_state *v = ctx->view;
    if (v->lights.count >= BR_LIGHT3D_MAX) {
        return; /* documented limit: extra lights are ignored */
    }
    br_light3d_entry entry;
    entry.light = *light;
    entry.world = world;
    br_array_push(&v->lights, &entry);
}

/* ---- collect: walk, sort, lights, shadows */

void br_view3d_compute_camera(br_engine *e, br_view3d_state *v)
{
    br_camera3d fallback = br_camera3d_perspective(60.0f * BR_DEG_TO_RAD, 0.1f, 500.0f);
    br_camera3d *cam = br_camera3d_get(e, v->camera);
    if (cam == NULL) {
        cam = &fallback;
    }
    br_mat4 world = br_node3d_world_matrix(e, v->camera);
    v->view = br_mat4_invert(world, NULL);
    float aspect = v->height > 0.0f ? v->width / v->height : 1.0f;
    v->projection = br_camera3d_projection(cam, aspect);
    v->view_projection = br_mat4_mul(v->projection, v->view);
    v->camera_position = br_vec3_make(world.m[12], world.m[13], world.m[14]);
    v->camera_forward = br_vec3_normalize(br_vec3_make(-world.m[8], -world.m[9], -world.m[10]));
}

/* Pre-order walk of root's subtree, skipping invisible subtrees, calling each type's collect. */
static void br_render3d_walk(br_engine *e, br_render3d_ctx *ctx)
{
    br_three_d_state *s = e->three_d;
    br_node3d_table *t = &s->nodes;
    br_view3d_state *v = ctx->view;
    uint32_t r = v->root.index;
    uint32_t i = r;
    while (i != BR_NONE3D) {
        bool descend = t->visible[i] != 0;
        if (descend && (t->layer[i] & v->culling_mask) != 0) {
            br_node3d_type_entry *type = br_array_at(&s->types, t->type[i] - 1);
            if (type != NULL && type->desc.collect != NULL) {
                br_node3d n = { i, t->generation[i] };
                void *data = type->desc.data_size > 0 ? br_array_at(&type->data, t->data_index[i]) : NULL;
                type->desc.collect(e, n, data, ctx);
            }
        }
        if (descend && t->first_child[i] != BR_NONE3D) {
            i = t->first_child[i];
            continue;
        }
        while (i != r && t->next_sibling[i] == BR_NONE3D) {
            i = t->parent[i];
        }
        i = (i == r) ? BR_NONE3D : t->next_sibling[i];
    }
}

/* Insertion sort of item indices by depth (ascending or descending). Lists are short and
 * nearly sorted from frame to frame. */
static void br_render3d_sort(br_array *order, const br_item3d *items, bool ascending)
{
    uint32_t *idx = order->data;
    for (uint32_t i = 1; i < order->count; i++) {
        uint32_t key = idx[i];
        float d = items[key].depth;
        uint32_t j = i;
        while (j > 0 && (ascending ? items[idx[j - 1]].depth > d : items[idx[j - 1]].depth < d)) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

static void br_vec4_store(float out[4], float x, float y, float z, float w)
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

/* Corners of the camera frustum slice between two view distances, in world space. */
static void br_render3d_slice_corners(br_engine *e, br_view3d_state *v, float d0, float d1, br_vec3 out[8])
{
    br_camera3d *cam = br_camera3d_get(e, v->camera);
    float aspect = v->height > 0.0f ? v->width / v->height : 1.0f;
    if (cam != NULL && cam->aspect > 0.0f) {
        aspect = cam->aspect;
    }
    br_mat4 world = br_mat4_invert(v->view, NULL);
    for (int k = 0; k < 2; k++) {
        float d = k == 0 ? d0 : d1;
        float h = (cam != NULL && cam->projection == BR_PROJECTION_ORTHOGRAPHIC) ? cam->ortho_size
                                                                                 : d * SDL_tanf((cam != NULL ? cam->fov_y : 1.047f) * 0.5f);
        float w = h * aspect;
        for (int c = 0; c < 4; c++) {
            br_vec3 p = br_vec3_make((c & 1) ? w : -w, (c & 2) ? h : -h, -d);
            out[k * 4 + c] = br_mat4_transform_point(world, p);
        }
    }
}

/* Light-space orthographic tile covering a bounding sphere, extended towards the light to
 * include every shadow caster. Returns the depth range (world units) for bias conversion. */
static float br_render3d_ortho_tile(br_view3d_state *v, uint32_t tile, br_vec3 dir, br_vec3 center, float radius)
{
    br_vec3 up = SDL_fabsf(dir.y) > 0.99f ? br_vec3_make(0.0f, 0.0f, 1.0f) : br_vec3_make(0.0f, 1.0f, 0.0f);
    br_mat4 light_view = br_mat4_look_at(br_vec3_make(0.0f, 0.0f, 0.0f), dir, up);
    br_vec3 c = br_mat4_transform_point(light_view, center);
    /* snap to whole texels so the shadow does not shimmer when the camera moves */
    float texel = 2.0f * radius / (float)v->shadow_tile_size;
    c.x = SDL_floorf(c.x / texel) * texel;
    c.y = SDL_floorf(c.y / texel) * texel;
    float z_near = c.z + radius; /* view space: -z is forward, so larger z = closer to the light */
    float z_far = c.z - radius;
    if (br_aabb_is_valid(v->caster_bounds)) {
        br_aabb cb = br_aabb_transform(v->caster_bounds, light_view);
        z_near = SDL_max(z_near, cb.max.z);
    }
    float near_d = -z_near - 1.0f;
    float far_d = -z_far + 1.0f;
    v->tile_view[tile] = light_view;
    v->tile_projection[tile] = br_mat4_ortho(c.x - radius, c.x + radius, c.y - radius, c.y + radius, near_d, far_d);
    return far_d - near_d;
}

static void br_render3d_tile_rect(br_view3d_state *v, uint32_t tile)
{
    br_vec4_store(v->frame.shadow_rect[tile], (float)(tile % 4) * 0.25f, (float)(tile / 4) * 0.5f, 0.25f, 0.5f);
    v->frame.shadow_matrix[tile] = br_mat4_mul(v->tile_projection[tile], v->tile_view[tile]);
    v->shadow_tiles |= 1u << tile;
}

/* Fills the light arrays and shadow tiles of the frame uniforms. */
static void br_render3d_setup_lights(br_engine *e, br_view3d_state *v)
{
    br_frame3d_uniforms *f = &v->frame;
    br_camera3d *cam = br_camera3d_get(e, v->camera);
    float cam_near = cam != NULL ? cam->near_z : 0.1f;
    float cam_far = cam != NULL ? cam->far_z : 500.0f;
    bool have_cascades = false;
    uint32_t spot_tiles = 0;
    v->shadow_tiles = 0;
    for (uint32_t i = 0; i < v->lights.count; i++) {
        const br_light3d_entry *le = br_array_at(&v->lights, i);
        const br_light3d *l = &le->light;
        br_vec3 pos = br_vec3_make(le->world.m[12], le->world.m[13], le->world.m[14]);
        br_vec3 dir = br_vec3_normalize(br_vec3_make(-le->world.m[8], -le->world.m[9], -le->world.m[10]));
        float tile = -1.0f;
        float bias = l->shadow_bias;
        if (l->cast_shadows && l->kind == BR_LIGHT3D_DIRECTIONAL && !have_cascades) {
            uint32_t count = SDL_clamp(l->cascade_count, 1u, (uint32_t)BR_SHADOW3D_MAX_CASCADES);
            float splits[BR_SHADOW3D_MAX_CASCADES];
            br_shadow3d_cascade_splits(cam_near, SDL_min(l->shadow_distance, cam_far), count, l->cascade_lambda, splits);
            float range0 = 1.0f;
            for (uint32_t c = 0; c < count; c++) {
                br_vec3 corners[8];
                br_render3d_slice_corners(e, v, c == 0 ? cam_near : splits[c - 1], splits[c], corners);
                br_vec3 center = br_vec3_make(0.0f, 0.0f, 0.0f);
                for (int k = 0; k < 8; k++) {
                    center = br_vec3_add(center, br_vec3_scale(corners[k], 0.125f));
                }
                float radius = 0.0f;
                for (int k = 0; k < 8; k++) {
                    radius = SDL_max(radius, br_vec3_length(br_vec3_sub(corners[k], center)));
                }
                radius = SDL_ceilf(radius * 16.0f) / 16.0f; /* stable size while the camera turns */
                float range = br_render3d_ortho_tile(v, c, dir, center, radius);
                if (c == 0) {
                    range0 = range;
                }
                br_render3d_tile_rect(v, c);
                f->cascade_far[c] = splits[c];
            }
            /* ponytail: one depth bias per light, converted with the first cascade's depth range;
             * per-tile biases would need another uniform array */
            bias = l->shadow_bias / range0;
            f->counts[1] = (float)count;
            tile = 0.0f;
            have_cascades = true;
        } else if (l->cast_shadows && l->kind == BR_LIGHT3D_SPOT && spot_tiles < BR_SHADOW3D_MAX_SPOT) {
            uint32_t t = BR_SHADOW3D_MAX_CASCADES + spot_tiles;
            float far_d = l->range > 0.0f ? l->range : 100.0f;
            br_vec3 up = SDL_fabsf(dir.y) > 0.99f ? br_vec3_make(0.0f, 0.0f, 1.0f) : br_vec3_make(0.0f, 1.0f, 0.0f);
            v->tile_view[t] = br_mat4_look_at(pos, br_vec3_add(pos, dir), up);
            float near_d = 0.25f;
            v->tile_projection[t] = br_mat4_perspective(SDL_min(l->outer_angle * 2.0f + 0.1f, 3.0f), 1.0f, near_d, far_d);
            br_render3d_tile_rect(v, t);
            /* ponytail: perspective depth is not linear; the world bias is converted with the
             * depth slope at half the range (too much close to the light, too little far away) */
            float mid = far_d * 0.5f;
            bias = l->shadow_bias * near_d * far_d / ((far_d - near_d) * mid * mid);
            tile = (float)t;
            spot_tiles++;
        }
        br_vec4_store(f->light_position[i], pos.x, pos.y, pos.z, (float)l->kind);
        br_vec4_store(f->light_direction[i], dir.x, dir.y, dir.z, l->range);
        br_vec4_store(f->light_color[i], l->color.r * l->intensity, l->color.g * l->intensity,
                      l->color.b * l->intensity, tile);
        br_vec4_store(f->light_params[i], SDL_cosf(l->outer_angle), SDL_cosf(l->inner_angle), bias, l->shadow_normal_bias);
    }
    f->counts[0] = (float)v->lights.count;
}

void br_render3d_collect(br_engine *e, br_view3d_state *v)
{
    br_array_clear(&v->items);
    br_array_clear(&v->opaque);
    br_array_clear(&v->transparent);
    br_array_clear(&v->shadow);
    br_array_clear(&v->joints);
    br_array_clear(&v->lights);
    SDL_zero(v->stats);
    SDL_zero(v->frame);
    v->caster_bounds = br_aabb_empty();

    br_three_d_update_world(e, v->root);
    br_view3d_compute_camera(e, v);
    br_render3d_ctx ctx;
    ctx.e = e;
    ctx.view = v;
    ctx.frustum = br_frustum_from_matrix(v->view_projection);
    br_render3d_walk(e, &ctx);

    const br_item3d *items = v->items.data;
    for (uint32_t i = 0; i < v->items.count; i++) {
        if (items[i].passes & BR_PASS3D_OPAQUE) {
            br_array_push(&v->opaque, &i);
        }
        if (items[i].passes & BR_PASS3D_TRANSPARENT) {
            br_array_push(&v->transparent, &i);
        }
        if (items[i].passes & BR_PASS3D_SHADOW) {
            br_array_push(&v->shadow, &i);
        }
    }
    br_render3d_sort(&v->opaque, items, true);
    br_render3d_sort(&v->transparent, items, false);

    br_frame3d_uniforms *f = &v->frame;
    br_vec4_store(f->camera_position, v->camera_position.x, v->camera_position.y, v->camera_position.z, 1.0f);
    br_vec4_store(f->camera_forward, v->camera_forward.x, v->camera_forward.y, v->camera_forward.z, 0.0f);
    br_vec4_store(f->sky, v->sky.r * v->ambient_intensity, v->sky.g * v->ambient_intensity, v->sky.b * v->ambient_intensity, 1.0f);
    br_vec4_store(f->ground, v->ground.r * v->ambient_intensity, v->ground.g * v->ambient_intensity,
                  v->ground.b * v->ambient_intensity, 1.0f);
    br_render3d_setup_lights(e, v);
    if (v->shadow.count == 0) {
        v->shadow_tiles = 0; /* nothing casts: skip the shadow pass, lights read "fully lit" */
        for (uint32_t i = 0; i < v->lights.count; i++) {
            f->light_color[i][3] = -1.0f;
        }
    }
    float atlas = (float)v->shadow_tile_size;
    f->counts[2] = 1.0f / (atlas * 4.0f);
    f->counts[3] = 1.0f / (atlas * 2.0f);

    v->stats.items_opaque = v->opaque.count;
    v->stats.items_transparent = v->transparent.count;
    v->stats.items_shadow = v->shadow.count;
    v->stats.lights = v->lights.count;
    for (uint32_t t = 0; t < BR_SHADOW3D_TILES; t++) {
        v->stats.shadow_tiles += (v->shadow_tiles >> t) & 1u;
    }
}

/* ---- RENDER: helpers */

static br_vertex_layout br_mesh3d_layout(void)
{
    br_vertex_layout l;
    SDL_zero(l);
    l.buffer_count = 2;
    l.strides[0] = sizeof(br_vertex3d);
    l.strides[1] = sizeof(br_instance3d);
    l.per_instance[1] = true;
    l.attribute_count = 13;
    l.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
    l.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 };
    l.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 24 };
    l.attributes[3] = (SDL_GPUVertexAttribute){ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 40 };
    l.attributes[4] = (SDL_GPUVertexAttribute){ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 48 };
    l.attributes[5] = (SDL_GPUVertexAttribute){ 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 56 };
    l.attributes[6] = (SDL_GPUVertexAttribute){ 6, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, 72 };
    l.attributes[7] = (SDL_GPUVertexAttribute){ 7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 80 };
    l.attributes[8] = (SDL_GPUVertexAttribute){ 8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 };
    l.attributes[9] = (SDL_GPUVertexAttribute){ 9, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16 };
    l.attributes[10] = (SDL_GPUVertexAttribute){ 10, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32 };
    l.attributes[11] = (SDL_GPUVertexAttribute){ 11, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48 };
    l.attributes[12] = (SDL_GPUVertexAttribute){ 12, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 64 };
    return l;
}

static SDL_GPUSampler *br_sampler(br_gpu *g, br_filter_mode filter, br_address_mode address)
{
    br_texture_style style;
    SDL_zero(style);
    style.filter = filter;
    style.address_mode = address;
    return br_gpu_sampler_get(g, &style);
}

/* Texture + sampler of a material slot; white when empty. */
static SDL_GPUTextureSamplerBinding br_slot_binding(br_engine *e, br_gpu *g, const br_material3d_texture *slot,
                                                    bool *present)
{
    SDL_GPUTextureSamplerBinding b;
    br_gpu_texture *tex = NULL;
    br_texture_info info;
    *present = false;
    if (slot->texture.generation != 0 && br_texture_valid(e, slot->texture)) {
        tex = br_texture_gpu(e, slot->texture);
    }
    if (tex != NULL) {
        *present = true;
        br_texture_get_info(e, slot->texture, &info);
        b.sampler = br_gpu_sampler_get(g, slot->has_sampler ? &slot->sampler : &info.style);
    } else {
        tex = br_texture_gpu(e, br_texture_white(e));
        b.sampler = br_sampler(g, BR_FILTER_LINEAR, BR_ADDRESS_CLAMP);
    }
    b.texture = tex != NULL ? tex->handle : NULL;
    return b;
}

/* KHR_texture_transform rows: translation * rotation * scale. */
static void br_slot_uv_rows(const br_material3d_texture *slot, float row0[4], float row1[4])
{
    float sx = slot->scale.x != 0.0f ? slot->scale.x : 1.0f;
    float sy = slot->scale.y != 0.0f ? slot->scale.y : 1.0f;
    float c = SDL_cosf(slot->rotation);
    float s = SDL_sinf(slot->rotation);
    br_vec4_store(row0, c * sx, s * sy, slot->offset.x, slot->uv_set == 1 ? 1.0f : 0.0f);
    br_vec4_store(row1, -s * sx, c * sy, slot->offset.y, 0.0f);
}

static void br_material_uniforms(const br_material3d_desc *m, uint32_t texture_bits, br_material3d_uniforms *u)
{
    SDL_zero(*u);
    br_vec4_store(u->base_color, m->base_color.r, m->base_color.g, m->base_color.b, m->base_color.a);
    br_vec4_store(u->emissive, m->emissive.r * m->emissive_strength, m->emissive.g * m->emissive_strength,
                  m->emissive.b * m->emissive_strength, 0.0f);
    br_vec4_store(u->pbr, m->metallic, m->roughness, m->normal_scale, m->occlusion_strength);
    br_vec4_store(u->specular, m->specular.r, m->specular.g, m->specular.b, m->shininess);
    br_vec4_store(u->alpha, m->alpha_cutoff, m->double_sided ? 1.0f : 0.0f, m->receive_shadows ? 1.0f : 0.0f, 0.0f);
    u->flags[0] = (uint32_t)m->model;
    u->flags[1] = m->toon_bands;
    u->flags[2] = texture_bits;
    u->flags[3] = (uint32_t)m->alpha_mode;
    for (int s = 0; s < BR_MATERIAL3D_SLOT_COUNT; s++) {
        br_slot_uv_rows(&m->textures[s], u->uv[s * 2], u->uv[s * 2 + 1]);
    }
}

/* Binds geometry, instances, storage buffers and deform uniforms; returns the pipeline vertex
 * shader to use. */
static void br_bind_geometry(br_engine *e, br_view3d_state *v, SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                             const br_item3d *item, const br_geometry3d_data *geo)
{
    br_gpu *g = br_engine_gpu(e);
    SDL_GPUBufferBinding vbs[2];
    vbs[0].buffer = geo->vertex_buffer;
    vbs[0].offset = 0;
    vbs[1].buffer = br_gpu_transient_vertex_buffer(g);
    vbs[1].offset = item->instance_offset;
    SDL_BindGPUVertexBuffers(pass, 0, vbs, 2);
    SDL_GPUBufferBinding ib = { geo->index_buffer, 0 };
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    if (item->joint_count > 0 || item->morph_count > 0) {
        SDL_GPUBuffer *storage[2];
        storage[0] = item->joint_count > 0 && v->joint_buffer != NULL ? v->joint_buffer : e->three_d->dummy_storage;
        storage[1] = item->morph_count > 0 && geo->morph_buffer != NULL ? geo->morph_buffer : e->three_d->dummy_storage;
        SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 2);
        br_deform3d_uniforms du;
        SDL_zero(du);
        for (uint32_t i = 0; i < item->morph_count; i++) {
            du.morph_weights[i] = item->morph[i];
        }
        du.deform[0] = item->joint_offset;
        du.deform[1] = v->joint_buffer != NULL ? item->joint_count : 0;
        du.deform[2] = geo->morph_buffer != NULL ? item->morph_count : 0;
        du.deform[3] = geo->vertex_count;
        SDL_PushGPUVertexUniformData(cmd, 1, &du, sizeof(du));
    }
}

static bool br_item_deforms(const br_item3d *item)
{
    return item->joint_count > 0 || item->morph_count > 0;
}

static br_draw3d_custom_info br_custom_info(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, br_pass3d kind,
                                            br_mat4 view, br_mat4 projection, br_vec3 camera, br_render_target *rt)
{
    br_draw3d_custom_info info;
    info.cmd = cmd;
    info.pass = pass;
    info.pass_kind = kind;
    info.view = view;
    info.projection = projection;
    info.view_projection = br_mat4_mul(projection, view);
    info.camera_position = camera;
    info.color_format = rt->color_format;
    info.depth_format = br_gpu_render_target_depth_format(rt);
    info.sample_count = rt->sample_count;
    return info;
}

void br_render3d_pipeline_setup(const br_draw3d_custom_info *info, br_pipeline_desc *desc)
{
    desc->color_format = info->color_format;
    desc->depth_stencil_format = info->depth_format;
    desc->sample_count = info->sample_count;
    desc->depth_test = true;
    desc->depth_compare = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    desc->depth_write = info->pass_kind != BR_PASS3D_TRANSPARENT;
    if (info->pass_kind == BR_PASS3D_TRANSPARENT) {
        desc->blend = br_gpu_blend_premultiplied();
    } else {
        SDL_zero(desc->blend);
    }
}

/* ---- RENDER: shadow pass */

static void br_render3d_shadow_pass(br_engine *e, br_view3d_state *v, SDL_GPUCommandBuffer *cmd)
{
    br_gpu *g = br_engine_gpu(e);
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(g, cmd, v->shadow_atlas, true, br_color_make(1.0f, 1.0f, 1.0f, 1.0f));
    if (pass == NULL) {
        return;
    }
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.layout = br_mesh3d_layout();
    desc.fragment_shader = &br_shader_shadow3d_frag;
    desc.color_format = BR_SHADOW_FORMAT;
    desc.depth_stencil_format = br_gpu_render_target_depth_format(v->shadow_atlas);
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    desc.cull_mode = SDL_GPU_CULLMODE_NONE; /* both faces: no light leaks through thin/open meshes */
    const br_item3d *items = v->items.data;
    float tile = (float)v->shadow_atlas_tile;
    for (uint32_t t = 0; t < BR_SHADOW3D_TILES; t++) {
        if ((v->shadow_tiles & (1u << t)) == 0) {
            continue;
        }
        SDL_GPUViewport vp = { (float)(t % 4) * tile, (float)(t / 4) * tile, tile, tile, 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &vp);
        SDL_Rect sc = { (int)vp.x, (int)vp.y, (int)tile, (int)tile };
        SDL_SetGPUScissor(pass, &sc);
        br_mat4 vp_matrix = br_mat4_mul(v->tile_projection[t], v->tile_view[t]);
        SDL_PushGPUVertexUniformData(cmd, 0, &vp_matrix, sizeof(vp_matrix));
        for (uint32_t k = 0; k < v->shadow.count; k++) {
            const br_item3d *item = &items[*(uint32_t *)br_array_at(&v->shadow, k)];
            if (item->custom_fn != NULL) {
                br_draw3d_custom_info info = br_custom_info(cmd, pass, BR_PASS3D_SHADOW, v->tile_view[t],
                                                            v->tile_projection[t], v->camera_position, v->shadow_atlas);
                item->custom_fn(e, &info, item->custom_user);
                SDL_PushGPUVertexUniformData(cmd, 0, &vp_matrix, sizeof(vp_matrix));
                continue;
            }
            br_geometry3d_data *geo = br_geometry3d_data_get(e, item->geometry);
            br_material3d_desc *mat = br_material3d_params(e, item->material);
            if (geo == NULL || mat == NULL || geo->vertex_buffer == NULL) {
                continue;
            }
            desc.vertex_shader = br_item_deforms(item) ? &br_shader_mesh3d_deform_vert : &br_shader_mesh3d_vert;
            SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
            if (pipeline == NULL) {
                continue;
            }
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            br_bind_geometry(e, v, pass, cmd, item, geo);
            bool present = false;
            SDL_GPUTextureSamplerBinding base = br_slot_binding(e, g, &mat->textures[BR_MATERIAL3D_BASE_COLOR], &present);
            SDL_BindGPUFragmentSamplers(pass, 0, &base, 1);
            br_shadow_mask_uniforms mu;
            SDL_zero(mu);
            if (mat->alpha_mode == BR_ALPHA_MASK) {
                br_vec4_store(mu.mask, mat->alpha_cutoff, mat->base_color.a, present ? 1.0f : 0.0f,
                              mat->textures[0].uv_set == 1 ? 1.0f : 0.0f);
            }
            br_slot_uv_rows(&mat->textures[0], mu.uv[0], mu.uv[1]);
            SDL_PushGPUFragmentUniformData(cmd, 0, &mu, sizeof(mu));
            SDL_DrawGPUIndexedPrimitives(pass, geo->index_count, item->instance_count, 0, 0, 0);
            v->stats.draw_calls++;
        }
    }
    SDL_EndGPURenderPass(pass);
}

/* ---- RENDER: main pass */

static void br_render3d_draw_item(br_engine *e, br_view3d_state *v, SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                                  const br_item3d *item, br_pass3d kind)
{
    br_gpu *g = br_engine_gpu(e);
    if (item->custom_fn != NULL) {
        br_draw3d_custom_info info = br_custom_info(cmd, pass, kind, v->view, v->projection, v->camera_position, v->hdr);
        item->custom_fn(e, &info, item->custom_user);
        /* the custom draw may have pushed its own uniforms into the slots we use */
        SDL_PushGPUVertexUniformData(cmd, 0, &v->view_projection, sizeof(v->view_projection));
        SDL_PushGPUFragmentUniformData(cmd, 0, &v->frame, sizeof(v->frame));
        return;
    }
    br_geometry3d_data *geo = br_geometry3d_data_get(e, item->geometry);
    br_material3d_desc *mat = br_material3d_params(e, item->material);
    if (geo == NULL || mat == NULL || geo->vertex_buffer == NULL) {
        return;
    }
    bool custom = mat->model == BR_MATERIAL3D_CUSTOM;
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = br_item_deforms(item) ? &br_shader_mesh3d_deform_vert : &br_shader_mesh3d_vert;
    desc.fragment_shader = custom ? mat->custom_fragment_shader : &br_shader_mesh3d_frag;
    desc.layout = br_mesh3d_layout();
    desc.cull_mode = mat->double_sided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
    desc.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    br_draw3d_custom_info target = br_custom_info(cmd, pass, kind, v->view, v->projection, v->camera_position, v->hdr);
    br_render3d_pipeline_setup(&target, &desc);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    if (pipeline == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    br_bind_geometry(e, v, pass, cmd, item, geo);
    if (custom) {
        SDL_GPUTextureSamplerBinding bindings[BR_MATERIAL3D_CUSTOM_TEXTURES];
        for (uint32_t i = 0; i < mat->custom_texture_count; i++) {
            br_material3d_texture slot;
            SDL_zero(slot);
            slot.texture = mat->custom_textures[i];
            bool present = false;
            bindings[i] = br_slot_binding(e, g, &slot, &present);
        }
        if (mat->custom_texture_count > 0) {
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, mat->custom_texture_count);
        }
        if (mat->custom_uniform_size > 0) {
            SDL_PushGPUFragmentUniformData(cmd, 1, mat->custom_uniforms, mat->custom_uniform_size);
        }
    } else {
        SDL_GPUTextureSamplerBinding bindings[BR_MATERIAL3D_SLOT_COUNT + 1];
        uint32_t bits = 0;
        for (int s = 0; s < BR_MATERIAL3D_SLOT_COUNT; s++) {
            bool present = false;
            bindings[s] = br_slot_binding(e, g, &mat->textures[s], &present);
            bits |= present ? (1u << s) : 0u;
        }
        br_render_target *atlas = v->shadow_tiles != 0 ? v->shadow_atlas : NULL;
        br_gpu_texture *shadow = atlas != NULL ? atlas->color : br_texture_gpu(e, br_texture_white(e));
        bindings[BR_MATERIAL3D_SLOT_COUNT].texture = shadow != NULL ? shadow->handle : NULL;
        bindings[BR_MATERIAL3D_SLOT_COUNT].sampler = br_sampler(g, BR_FILTER_NEAREST, BR_ADDRESS_CLAMP);
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, BR_MATERIAL3D_SLOT_COUNT + 1);
        br_material3d_uniforms mu;
        br_material_uniforms(mat, bits, &mu);
        SDL_PushGPUFragmentUniformData(cmd, 1, &mu, sizeof(mu));
    }
    SDL_DrawGPUIndexedPrimitives(pass, geo->index_count, item->instance_count, 0, 0, 0);
    v->stats.draw_calls++;
}

static void br_render3d_main_pass(br_engine *e, br_view3d_state *v, SDL_GPUCommandBuffer *cmd)
{
    br_gpu *g = br_engine_gpu(e);
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(g, cmd, v->hdr, true, v->background);
    if (pass == NULL) {
        return;
    }
    SDL_PushGPUVertexUniformData(cmd, 0, &v->view_projection, sizeof(v->view_projection));
    SDL_PushGPUFragmentUniformData(cmd, 0, &v->frame, sizeof(v->frame));
    const br_item3d *items = v->items.data;
    for (uint32_t k = 0; k < v->opaque.count; k++) {
        br_render3d_draw_item(e, v, pass, cmd, &items[*(uint32_t *)br_array_at(&v->opaque, k)], BR_PASS3D_OPAQUE);
    }
    for (uint32_t k = 0; k < v->transparent.count; k++) {
        br_render3d_draw_item(e, v, pass, cmd, &items[*(uint32_t *)br_array_at(&v->transparent, k)],
                              BR_PASS3D_TRANSPARENT);
    }
    SDL_EndGPURenderPass(pass);
}

/* ---- RENDER: post */

/* One full-target triangle from `src` into `dst` with a post fragment shader. */
static void br_post_blit(br_engine *e, SDL_GPUCommandBuffer *cmd, br_render_target *dst, bool clear,
                         const br_shader_blob *fragment, SDL_GPUColorTargetBlendState blend,
                         const br_gpu_texture *src, const br_gpu_texture *src2, const void *uniforms, uint32_t uniform_size)
{
    br_gpu *g = br_engine_gpu(e);
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_post3d_vert;
    desc.fragment_shader = fragment;
    desc.blend = blend;
    desc.color_format = dst->color_format;
    desc.depth_stencil_format = br_gpu_render_target_depth_format(dst);
    desc.sample_count = dst->sample_count;
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    if (pipeline == NULL) {
        return;
    }
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(g, cmd, dst, clear, br_color_make(0.0f, 0.0f, 0.0f, 0.0f));
    if (pass == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_GPUSampler *linear = br_sampler(g, BR_FILTER_LINEAR, BR_ADDRESS_CLAMP);
    SDL_GPUTextureSamplerBinding b[2];
    b[0].texture = src->handle;
    b[0].sampler = linear;
    uint32_t count = 1;
    if (src2 != NULL) {
        b[1].texture = src2->handle;
        b[1].sampler = linear;
        count = 2;
    }
    SDL_BindGPUFragmentSamplers(pass, 0, b, count);
    if (uniform_size > 0) {
        SDL_PushGPUFragmentUniformData(cmd, 0, uniforms, uniform_size);
    }
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void br_render3d_post(br_engine *e, br_view3d_state *v, SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUColorTargetBlendState no_blend;
    SDL_zero(no_blend);
    br_gpu_texture *image = v->hdr->color;
    br_gpu_texture *bloom = NULL;
    if (v->post.bloom && v->bloom_count > 0) {
        for (uint32_t i = 0; i < v->bloom_count; i++) {
            br_gpu_texture *src = i == 0 ? image : v->bloom[i - 1]->color;
            float params[4] = { 1.0f / (float)src->width, 1.0f / (float)src->height, v->post.bloom_threshold,
                                i == 0 ? 1.0f : 0.0f };
            br_post_blit(e, cmd, v->bloom[i], true, &br_shader_post3d_down_frag, no_blend, src, NULL, params, sizeof(params));
        }
        SDL_GPUColorTargetBlendState add;
        SDL_zero(add);
        add.enable_blend = true;
        add.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        add.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        add.color_blend_op = SDL_GPU_BLENDOP_ADD;
        add.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        add.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        add.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        for (uint32_t i = v->bloom_count - 1; i > 0; i--) {
            br_gpu_texture *src = v->bloom[i]->color;
            float params[4] = { 1.0f / (float)src->width, 1.0f / (float)src->height, 0.0f, 0.0f };
            br_post_blit(e, cmd, v->bloom[i - 1], false, &br_shader_post3d_up_frag, add, src, NULL, params, sizeof(params));
        }
        bloom = v->bloom[0]->color;
    }
    uint32_t done = 0;
    for (uint32_t p = 0; p < v->pass_count; p++) {
        const br_post3d_pass *pp = &v->passes[p];
        if (!pp->enabled || pp->fragment_shader == NULL || v->post_temp[done % 2] == NULL) {
            continue;
        }
        br_render_target *dst = v->post_temp[done % 2];
        br_post_blit(e, cmd, dst, true, pp->fragment_shader, no_blend, image, NULL, pp->uniforms, pp->uniform_size);
        image = dst->color;
        done++;
    }
    br_render_target *out = br_texture_render_target(e, v->output);
    if (out == NULL) {
        return;
    }
    float params[4] = { v->post.exposure, (float)v->post.tonemap, bloom != NULL ? v->post.bloom_intensity : 0.0f, 0.0f };
    br_post_blit(e, cmd, out, true, &br_shader_post3d_tonemap_frag, no_blend, image, bloom != NULL ? bloom : image,
                 params, sizeof(params));
}

/* ---- RENDER: the callback */

static bool br_render3d_upload_joints(br_engine *e, br_view3d_state *v, SDL_GPUCommandBuffer *cmd)
{
    br_gpu *g = br_engine_gpu(e);
    SDL_GPUDevice *device = br_gpu_device(g);
    uint32_t count = v->joints.count;
    uint32_t bytes = count * (uint32_t)sizeof(br_mat4);
    if (count > v->joint_capacity) {
        br_gpu_buffer_destroy(g, v->joint_buffer);
        if (v->joint_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, v->joint_transfer);
        }
        uint32_t capacity = SDL_max(count, 64u) * 2;
        v->joint_buffer = br_gpu_buffer_create(g, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                               capacity * (uint32_t)sizeof(br_mat4), NULL);
        SDL_GPUTransferBufferCreateInfo info;
        SDL_zero(info);
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = capacity * (uint32_t)sizeof(br_mat4);
        v->joint_transfer = SDL_CreateGPUTransferBuffer(device, &info);
        v->joint_capacity = (v->joint_buffer != NULL && v->joint_transfer != NULL) ? capacity : 0;
        if (v->joint_capacity == 0) {
            br_log_error("cannot create the 3D joint buffer: %s", SDL_GetError());
            return false;
        }
    }
    void *dst = SDL_MapGPUTransferBuffer(device, v->joint_transfer, true);
    if (dst == NULL) {
        return false;
    }
    SDL_memcpy(dst, v->joints.data, bytes);
    SDL_UnmapGPUTransferBuffer(device, v->joint_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { v->joint_transfer, 0 };
    SDL_GPUBufferRegion region = { v->joint_buffer, 0, bytes };
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

void br_render3d_draw(br_engine *e, const br_render_callback_info *info, void *user)
{
    br_view3d_state *v = user;
    if (v->hdr == NULL || br_texture_render_target(e, v->output) == NULL) {
        return;
    }
    if (v->joints.count > 0 && !br_render3d_upload_joints(e, v, info->cmd)) {
        v->joint_capacity = 0;
    }
    if (v->shadow_tiles != 0 && v->shadow_atlas != NULL) {
        br_render3d_shadow_pass(e, v, info->cmd);
    }
    br_render3d_main_pass(e, v, info->cmd);
    br_render3d_post(e, v, info->cmd);
}
