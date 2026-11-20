/* br_canvas3d: 2D subtrees in the 3D world. The "canvas3d" 3D node type captures its 2D
 * subtree's batcher output during the 3D collect (br_render_capture_subtree) and replays it as a
 * custom draw item inside the view's opaque or transparent pass; texture mode draws a render
 * texture of the subtree instead. 3D pointer events on the canvas become 2D events. */
#include "app/br_engine_internal.h"
#include "braty/br_arena.h"
#include "braty/br_camera3d.h"
#include "braty/br_canvas3d.h"
#include "braty/br_font.h"
#include "braty/br_material3d.h"
#include "render/br_render_internal.h"

#include "canvas3d.frag.h"
#include "canvas3d.vert.h"
#include "canvas3d_custom.vert.h"
#include "canvas3d_sdf.frag.h"

/* One canvas (heap: the 3D type pool moves, draw callbacks and listeners keep this pointer). */
typedef struct br_canvas3d_rec {
    br_canvas3d_desc desc;
    br_node3d        node;
    br_node          root;        /* detached 2D container */
    br_texture       target;      /* texture mode */
    br_mat4          world;       /* plane world matrix of the last collect (billboard applied) */
    bool             has_world;
    br_node          hovered;     /* 2D pointer state */
    br_node          pressed;
} br_canvas3d_rec;

typedef struct br_canvas3d_node {
    br_canvas3d_rec *rec;
} br_canvas3d_node;

/* Fragment b1 of the canvas shaders (canvas3d_common.hlsli). */
typedef struct br_canvas3d_uniforms {
    float tint[4];
    float params[4];
    float normal[4];
    float field[4];
    float mode[4];
} br_canvas3d_uniforms;

/* What one canvas draws this frame (frame arena: lives until the next frame). */
typedef struct br_canvas3d_draw {
    br_canvas3d_rec *rec;
    br_mat4          world;
    br_vec3          origin;
    br_vec3          normal;       /* plane normal (+Z of the plane) in world space */
    uint32_t         first_cmd;    /* captured commands in state->cmds */
    uint32_t         cmd_count;
    bool             texture_quad; /* texture mode: one quad below instead of commands */
    uint32_t         quad_vertices;
    uint32_t         quad_indices;
} br_canvas3d_draw;

typedef struct br_canvas3d_state {
    uint32_t type;
    br_array canvases;     /* br_canvas3d_rec * */
    br_array cmds;         /* br_draw_cmd captured this frame */
    uint64_t cmds_frame;
    br_font *sdf_font;
} br_canvas3d_state;

/* ---- pure maths */

br_canvas3d_desc br_canvas3d_default(float width, float height)
{
    br_canvas3d_desc d;
    SDL_zero(d);
    d.width = width;
    d.height = height;
    d.pixels_per_unit = 100.0f;
    d.pivot = br_vec2_make(0.5f, 0.5f);
    d.mode = BR_CANVAS3D_GEOMETRY;
    d.billboard = BR_CANVAS3D_BILLBOARD_NONE;
    d.double_sided = true;
    d.tint = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    d.resolution = 2.0f;
    return d;
}

static float br_canvas3d_ppu(const br_canvas3d_desc *d)
{
    return d->pixels_per_unit > 0.0f ? d->pixels_per_unit : 100.0f;
}

br_mat4 br_canvas3d_plane_matrix(const br_canvas3d_desc *d)
{
    float ppu = br_canvas3d_ppu(d);
    br_mat4 m = br_mat4_identity();
    m.m[0] = 1.0f / ppu;
    m.m[5] = -1.0f / ppu;   /* 2D y down -> 3D y up */
    m.m[12] = -d->pivot.x * d->width / ppu;
    m.m[13] = d->pivot.y * d->height / ppu;
    return m;
}

bool br_canvas3d_ray_to_local(const br_canvas3d_desc *d, br_mat4 world, br_ray3d ray, br_vec2 *local, float *t)
{
    bool ok = false;
    br_mat4 inverse = br_mat4_invert(world, &ok);
    if (!ok) {
        return false;
    }
    br_vec3 o = br_mat4_transform_point(inverse, ray.origin);
    br_vec3 dir = br_mat4_transform_dir(inverse, ray.direction);
    if (SDL_fabsf(dir.z) < 1e-8f) {
        return false;
    }
    float hit_t = -o.z / dir.z;
    if (hit_t < 0.0f) {
        return false;
    }
    float ppu = br_canvas3d_ppu(d);
    float x = (o.x + dir.x * hit_t) * ppu + d->pivot.x * d->width;
    float y = -(o.y + dir.y * hit_t) * ppu + d->pivot.y * d->height;
    if (x < 0.0f || y < 0.0f || x > d->width || y > d->height) {
        return false;
    }
    if (local != NULL) {
        *local = br_vec2_make(x, y);
    }
    if (t != NULL) {
        *t = hit_t;
    }
    return true;
}

static br_vec3 br_canvas3d_column(br_mat4 m, int c)
{
    return br_vec3_make(m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]);
}

br_mat4 br_canvas3d_billboard_matrix(br_canvas3d_billboard mode, br_mat4 w, br_mat4 camera)
{
    if (mode == BR_CANVAS3D_BILLBOARD_NONE) {
        return w;
    }
    br_vec3 pos = br_vec3_make(w.m[12], w.m[13], w.m[14]);
    br_vec3 scale = br_vec3_make(br_vec3_length(br_canvas3d_column(w, 0)), br_vec3_length(br_canvas3d_column(w, 1)),
                                 br_vec3_length(br_canvas3d_column(w, 2)));
    br_mat4 r = br_mat4_identity();
    if (mode == BR_CANVAS3D_BILLBOARD_FULL) {
        /* the camera's own axes: the plane's +Z points back at the camera */
        for (int c = 0; c < 3; c++) {
            br_vec3 axis = br_vec3_normalize(br_canvas3d_column(camera, c));
            r.m[c * 4] = axis.x;
            r.m[c * 4 + 1] = axis.y;
            r.m[c * 4 + 2] = axis.z;
        }
    } else {
        br_vec3 to_camera = br_vec3_make(camera.m[12] - pos.x, 0.0f, camera.m[14] - pos.z);
        if (br_vec3_length(to_camera) > 1e-6f) {
            r = br_mat4_rotation_y(SDL_atan2f(to_camera.x, to_camera.z));
        }
    }
    return br_mat4_mul(br_mat4_translation(pos), br_mat4_mul(r, br_mat4_scaling(scale)));
}

/* ---- the node type */

static br_canvas3d_rec *br_canvas3d_rec_get(br_engine *e, br_node3d n)
{
    if (e->canvas3d == NULL) {
        return NULL;
    }
    br_canvas3d_node *cn = br_node3d_data(e, n, e->canvas3d->type);
    return cn != NULL ? cn->rec : NULL;
}

static void br_canvas3d_on_create(br_engine *e, br_node3d n, void *data)
{
    br_canvas3d_node *cn = data;
    br_canvas3d_rec *c = br_alloc(sizeof(br_canvas3d_rec));
    cn->rec = c;
    if (c == NULL) {
        return;
    }
    c->desc = br_canvas3d_default(100.0f, 100.0f);
    c->node = n;
    c->root = br_node_create_container(e);
    br_array_push(&e->canvas3d->canvases, &c);
}

static void br_canvas3d_on_destroy(br_engine *e, br_node3d n, void *data)
{
    (void)n;
    br_canvas3d_node *cn = data;
    br_canvas3d_rec *c = cn->rec;
    if (c == NULL) {
        return;
    }
    br_canvas3d_state *s = e->canvas3d; /* NULL when the 3D module frees its nodes at shutdown */
    for (uint32_t i = 0; s != NULL && i < s->canvases.count; i++) {
        if (*(br_canvas3d_rec **)br_array_at(&s->canvases, i) == c) {
            br_array_remove_swap(&s->canvases, i);
            break;
        }
    }
    br_node_destroy(e, c->root);
    if (c->target.generation != 0) {
        br_texture_destroy(e, c->target);
    }
    br_free(c);
    cn->rec = NULL;
}

static bool br_canvas3d_bounds(br_engine *e, br_node3d n, const void *data, br_aabb *out)
{
    (void)e;
    (void)n;
    const br_canvas3d_rec *c = ((const br_canvas3d_node *)data)->rec;
    if (c == NULL) {
        return false;
    }
    float ppu = br_canvas3d_ppu(&c->desc);
    float x0 = -c->desc.pivot.x * c->desc.width / ppu;
    float x1 = x0 + c->desc.width / ppu;
    float y1 = c->desc.pivot.y * c->desc.height / ppu;
    float y0 = y1 - c->desc.height / ppu;
    if (c->desc.billboard != BR_CANVAS3D_BILLBOARD_NONE) {
        /* any orientation: a cube around the plane's reach */
        float r = SDL_max(SDL_max(SDL_fabsf(x0), SDL_fabsf(x1)), SDL_max(SDL_fabsf(y0), SDL_fabsf(y1)));
        out->min = br_vec3_make(-r, -r, -r);
        out->max = br_vec3_make(r, r, r);
        return true;
    }
    out->min = br_vec3_make(x0, y0, -0.001f);
    out->max = br_vec3_make(x1, y1, 0.001f);
    return true;
}

/* ---- drawing (RENDER, inside the 3D pass) */

static bool br_canvas3d_blob_is(const br_shader_blob *blob, const char *name)
{
    return blob != NULL && blob->name != NULL && SDL_strcmp(blob->name, name) == 0;
}

static br_vertex_layout br_canvas3d_batch_layout(void)
{
    br_vertex_layout l;
    SDL_zero(l);
    l.buffer_count = 1;
    l.strides[0] = sizeof(br_batch_vertex);
    l.attribute_count = 4;
    l.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    l.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    l.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    l.attributes[3] = (SDL_GPUVertexAttribute){ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 20 };
    return l;
}

static bool br_canvas3d_bind_pipeline(br_engine *e, const br_draw3d_custom_info *info, const br_shader_blob *vs,
                                      const br_shader_blob *fs, br_vertex_layout layout)
{
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = vs;
    desc.fragment_shader = fs;
    desc.layout = layout;
    desc.cull_mode = SDL_GPU_CULLMODE_NONE; /* 2D winding is arbitrary: facing is decided per canvas */
    br_render3d_pipeline_setup(info, &desc);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(br_engine_gpu(e), &desc);
    if (pipeline == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(info->pass, pipeline);
    return true;
}

static void br_canvas3d_push_matrices(const br_draw3d_custom_info *info, br_mat4 world, br_mat4 transform)
{
    br_mat4 model = br_mat4_mul(world, transform);
    br_mat4 mvp = br_mat4_mul(info->view_projection, model);
    SDL_PushGPUVertexUniformData(info->cmd, 0, &mvp, sizeof(mvp));
    SDL_PushGPUVertexUniformData(info->cmd, 1, &model, sizeof(model));
}

/* A batch: vertices at vertex_offset, 16-bit indices in the transient index buffer. */
static void br_canvas3d_draw_batch(br_engine *e, const br_draw3d_custom_info *info, const br_canvas3d_draw *d,
                                   br_mat4 transform, const br_texture *textures, uint32_t texture_count,
                                   uint32_t vertex_offset, uint32_t index_base, uint32_t first_index,
                                   uint32_t index_count, const br_canvas3d_uniforms *u)
{
    br_gpu *g = br_engine_gpu(e);
    SDL_GPUTextureSamplerBinding bindings[BR_RENDER_MAX_TEXTURES];
    for (uint32_t i = 0; i < BR_RENDER_MAX_TEXTURES; i++) {
        br_texture t = i < texture_count ? textures[i] : br_texture_white(e);
        if (!br_texture_binding(e, t, &bindings[i])) {
            return;
        }
    }
    if (!br_canvas3d_bind_pipeline(e, info, &br_shader_canvas3d_vert, &br_shader_canvas3d_frag,
                                   br_canvas3d_batch_layout())) {
        return;
    }
    br_canvas3d_push_matrices(info, d->world, transform);
    SDL_PushGPUFragmentUniformData(info->cmd, 1, u, sizeof(*u));
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), vertex_offset };
    SDL_BindGPUVertexBuffers(info->pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { br_gpu_transient_index_buffer(g), index_base };
    SDL_BindGPUIndexBuffer(info->pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_BindGPUFragmentSamplers(info->pass, 0, bindings, BR_RENDER_MAX_TEXTURES);
    SDL_DrawGPUIndexedPrimitives(info->pass, index_count, 1, first_index, 0, 0);
}

/* SDF text (text_sdf.frag custom draws) with the canvas shading; other custom draws are skipped. */
static void br_canvas3d_draw_custom(br_engine *e, const br_draw3d_custom_info *info, const br_canvas3d_draw *d,
                                    const br_draw_cmd *cmd, br_canvas3d_uniforms u)
{
    br_render_state *r = e->render;
    br_gpu *g = br_engine_gpu(e);
    const br_custom_cmd *cc = br_array_at(&r->customs, cmd->custom);
    // ponytail: only SDF text is re-shaded for 3D; other custom 2D shaders (tiling sprites) need texture mode.
    if (!br_canvas3d_blob_is(cc->fragment_shader, "text_sdf.frag") || cc->texture_count < 1 || cc->instanced) {
        return;
    }
    if (cc->fragment_uniform_size >= 32) {
        const uint8_t *src = (const uint8_t *)r->uniforms.data + cc->fragment_uniform_offset;
        SDL_memcpy(u.field, src, 16);
        SDL_memcpy(u.mode, src + 16, 16);
    }
    SDL_GPUTextureSamplerBinding binding;
    if (!br_texture_binding(e, cc->textures[0], &binding)) {
        return;
    }
    if (!br_canvas3d_bind_pipeline(e, info, &br_shader_canvas3d_custom_vert, &br_shader_canvas3d_sdf_frag, cc->layout)) {
        return;
    }
    br_canvas3d_push_matrices(info, d->world, cmd->state.transform);
    SDL_PushGPUFragmentUniformData(info->cmd, 1, &u, sizeof(u));
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), cc->vertex_offset };
    SDL_BindGPUVertexBuffers(info->pass, 0, &vb, 1);
    SDL_BindGPUFragmentSamplers(info->pass, 0, &binding, 1);
    if (cc->indexed) {
        SDL_GPUBufferBinding ib = { br_gpu_transient_index_buffer(g), cc->index_offset };
        SDL_BindGPUIndexBuffer(info->pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(info->pass, cc->index_count, 1, 0, 0, 0);
    } else {
        SDL_DrawGPUPrimitives(info->pass, cc->vertex_count, 1, 0, 0);
    }
}

static void br_canvas3d_draw_fn(br_engine *e, const br_draw3d_custom_info *info, void *user)
{
    const br_canvas3d_draw *d = user;
    br_canvas3d_state *s = e->canvas3d;
    const br_canvas3d_desc *desc = &d->rec->desc;
    if (info->pass_kind == BR_PASS3D_SHADOW) {
        return;
    }
    br_vec3 n = d->normal;
    if (br_vec3_dot(br_vec3_sub(info->camera_position, d->origin), n) < 0.0f) {
        if (!desc->double_sided) {
            return; /* the back of a single-sided canvas */
        }
        n = br_vec3_scale(n, -1.0f);
    }
    br_canvas3d_uniforms u;
    SDL_zero(u);
    br_color tint = br_color_to_linear(desc->tint);
    u.tint[0] = tint.r;
    u.tint[1] = tint.g;
    u.tint[2] = tint.b;
    u.tint[3] = desc->tint.a;
    u.params[0] = desc->lit ? 1.0f : 0.0f;
    u.params[1] = desc->alpha_cutoff;
    u.normal[0] = n.x;
    u.normal[1] = n.y;
    u.normal[2] = n.z;
    if (d->texture_quad) {
        br_mat4 plane = br_canvas3d_plane_matrix(desc);
        br_canvas3d_draw_batch(e, info, d, plane, &d->rec->target, 1, d->quad_vertices, d->quad_indices, 0, 6, &u);
        return;
    }
    br_render_state *r = e->render;
    for (uint32_t k = 0; k < d->cmd_count; k++) {
        const br_draw_cmd *cmd = br_array_at(&s->cmds, d->first_cmd + k);
        if (cmd->kind == BR_DRAW_BATCH) {
            br_canvas3d_draw_batch(e, info, d, cmd->state.transform, cmd->textures, cmd->texture_count,
                                   cmd->vertex_offset, r->index_base, cmd->first_index, cmd->index_count, &u);
        } else if (cmd->kind == BR_DRAW_CUSTOM) {
            br_canvas3d_draw_custom(e, info, d, cmd, u);
        }
    }
}

/* ---- collect (PRE_RENDER, from the view's 3D walk) */

/* Texture mode: the quad's 4 batch vertices and 6 indices in this frame's transient buffers. */
static bool br_canvas3d_texture_quad(br_engine *e, br_canvas3d_rec *c, br_canvas3d_draw *d)
{
    br_gpu *g = br_engine_gpu(e);
    br_texture_info info;
    if (c->target.generation == 0 || br_texture_render_target(e, c->target) == NULL ||
        !br_texture_get_info(e, c->target, &info)) {
        return false;
    }
    br_batch_vertex *v = br_gpu_transient_alloc_vertices(g, 4 * (uint32_t)sizeof(br_batch_vertex), &d->quad_vertices);
    if (v == NULL) {
        return false;
    }
    float w = c->desc.width;
    float h = c->desc.height;
    br_vec2 corners[4] = { { 0.0f, 0.0f }, { w, 0.0f }, { w, h }, { 0.0f, h } };
    for (int i = 0; i < 4; i++) {
        v[i].x = corners[i].x;
        v[i].y = corners[i].y;
        v[i].u = info.uvs[i].x;
        v[i].v = info.uvs[i].y;
        v[i].color = 0xFFFFFFFFu;
        v[i].texture = 0;
    }
    uint16_t *idx = br_gpu_transient_alloc_indices(g, 6 * (uint32_t)sizeof(uint16_t), &d->quad_indices);
    if (idx == NULL) {
        return false;
    }
    const uint16_t quad[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_memcpy(idx, quad, sizeof(quad));
    d->texture_quad = true;
    return true;
}

static void br_canvas3d_collect(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx)
{
    br_canvas3d_state *s = e->canvas3d;
    br_canvas3d_rec *c = ((const br_canvas3d_node *)data)->rec;
    if (c == NULL || c->desc.width <= 0.0f || c->desc.height <= 0.0f) {
        return;
    }
    uint64_t frame = br_ticker_frame(e);
    if (s->cmds_frame != frame) {
        br_array_clear(&s->cmds);
        s->cmds_frame = frame;
    }
    br_mat4 node_world = br_render3d_world(ctx, n);
    br_mat4 camera = br_node3d_world_transform(e, br_view3d_camera(e, br_render3d_view(ctx)));
    br_canvas3d_draw *d = br_frame_alloc(e, sizeof(br_canvas3d_draw));
    if (d == NULL) {
        return;
    }
    d->rec = c;
    d->world = br_canvas3d_billboard_matrix(c->desc.billboard, node_world, camera);
    c->world = d->world;
    c->has_world = true;

    /* visibility: the plane's corners against the frustum */
    br_mat4 plane = br_canvas3d_plane_matrix(&c->desc);
    br_mat4 to_world = br_mat4_mul(d->world, plane);
    br_aabb box = br_aabb_empty();
    for (int i = 0; i < 4; i++) {
        br_vec3 p = br_vec3_make((i & 1) ? c->desc.width : 0.0f, (i & 2) ? c->desc.height : 0.0f, 0.0f);
        box = br_aabb_extend(box, br_mat4_transform_point(to_world, p));
    }
    if (!br_render3d_visible(ctx, box)) {
        return;
    }
    d->origin = br_vec3_scale(br_vec3_add(box.min, box.max), 0.5f);
    d->normal = br_vec3_normalize(br_canvas3d_column(d->world, 2));

    if (c->desc.mode == BR_CANVAS3D_TEXTURE) {
        if (!br_canvas3d_texture_quad(e, c, d)) {
            return;
        }
    } else {
        d->first_cmd = s->cmds.count;
        br_rect area = br_rect_make(0.0f, 0.0f, c->desc.width, c->desc.height);
        float ratio = c->desc.resolution > 0.0f ? c->desc.resolution : 2.0f;
        br_render_capture_subtree(e, c->root, plane, area, ratio, &s->cmds);
        d->cmd_count = s->cmds.count - d->first_cmd;
        if (d->cmd_count == 0) {
            return;
        }
    }
    uint32_t pass = c->desc.alpha_cutoff > 0.0f ? BR_PASS3D_OPAQUE : BR_PASS3D_TRANSPARENT;
    br_render3d_submit_custom(ctx, pass, d->origin, br_canvas3d_draw_fn, d);
}

/* ---- picking and pointer events */

/* The plane's world matrix: as last drawn (billboards), or the node's before the first draw. */
static br_mat4 br_canvas3d_plane_world(br_engine *e, const br_canvas3d_rec *c)
{
    return c->has_world ? c->world : br_node3d_world_transform(e, c->node);
}

static bool br_canvas3d_pick(br_engine *e, br_node3d n, const void *data, br_ray3d local_ray, br_pick3d_hit *hit)
{
    br_canvas3d_rec *c = ((const br_canvas3d_node *)data)->rec;
    if (c == NULL) {
        return false;
    }
    br_mat4 node_world = br_node3d_world_transform(e, n);
    br_mat4 plane_world = c->has_world ? c->world : node_world; /* = br_canvas3d_plane_world */
    br_ray3d world_ray;
    world_ray.origin = br_mat4_transform_point(node_world, local_ray.origin);
    world_ray.direction = br_mat4_transform_dir(node_world, local_ray.direction);
    float t = 0.0f;
    if (!br_canvas3d_ray_to_local(&c->desc, plane_world, world_ray, NULL, &t) || t >= hit->distance) {
        return false;
    }
    br_mat4 inverse = br_mat4_invert(node_world, NULL);
    br_vec3 p = br_vec3_add(world_ray.origin, br_vec3_scale(world_ray.direction, t));
    hit->distance = t;
    hit->point = br_mat4_transform_point(inverse, p);
    hit->normal = br_mat4_transform_dir(inverse, br_canvas3d_column(plane_world, 2));
    return true;
}

/* Deepest visible node of the subtree whose shape contains p (root coordinates). Recurses per
 * level, so it stops where the render walk stops drawing (BR_RENDER_MAX_DEPTH). */
#define BR_CANVAS3D_MAX_HIT_DEPTH 2048

static br_node br_canvas3d_shape_hit(br_engine *e, br_node n, br_vec2 p, uint32_t depth)
{
    br_node none = { 0, 0 };
    if (depth >= BR_CANVAS3D_MAX_HIT_DEPTH || !br_node_visible(e, n) || br_node_event_mode(e, n) == BR_EVENT_MODE_NONE) {
        return none;
    }
    for (uint32_t i = br_node_child_count(e, n); i > 0; i--) {
        br_node child = br_node_child_at(e, n, i - 1);
        if (!br_node_valid(e, child)) {
            continue;
        }
        br_node hit = br_canvas3d_shape_hit(e, child, p, depth + 1);
        if (br_node_valid(e, hit)) {
            return hit;
        }
    }
    br_vec2 local = br_node_to_local(e, n, p);
    br_hit_area area = br_node_hit_area(e, n);
    bool inside = area.type != BR_HIT_AREA_NONE ? br_hit_area_contains(&area, local) : br_node_type_hit_test(e, n, local);
    return inside ? n : none;
}

/* The interactive node (STATIC / DYNAMIC) at p: the shape hit or its nearest interactive ancestor. */
static br_node br_canvas3d_target(br_engine *e, br_canvas3d_rec *c, br_vec2 p)
{
    // ponytail: no z-index / render-layer order and no masks in canvas hit tests; add them if UIs in 3D need them.
    br_node n = br_canvas3d_shape_hit(e, c->root, p, 0);
    while (br_node_valid(e, n)) {
        br_event_mode mode = br_node_event_mode(e, n);
        if (mode == BR_EVENT_MODE_STATIC || mode == BR_EVENT_MODE_DYNAMIC) {
            return n;
        }
        n = br_node_parent(e, n);
    }
    br_node none = { 0, 0 };
    return none;
}

static void br_canvas3d_dispatch(br_engine *e, br_event_type type, br_node target, br_vec2 p, const br_event *source)
{
    if (!br_node_valid(e, target)) {
        return;
    }
    br_event ev;
    SDL_zero(ev);
    ev.type = type;
    ev.target = target;
    ev.global = p; /* the root is detached at the origin: root coordinates are its global space */
    ev.pointer_type = source->pointer_type;
    ev.pointer_id = source->pointer_id;
    ev.button = source->button;
    ev.pressure = source->pressure;
    ev.wheel = source->wheel;
    ev.timestamp_ns = source->timestamp_ns;
    br_events_dispatch(e, &ev);
}

static void br_canvas3d_event(br_engine *e, br_event3d *ev, void *user)
{
    br_canvas3d_rec *c = user;
    br_event_type type = ev->source->type;
    br_node none = { 0, 0 };
    br_node target = none;
    br_vec2 p = br_vec2_make(0.0f, 0.0f);
    if (type != BR_EVENT_POINTER_OUT) {
        /* the hit point is on the plane: map it straight into root coordinates */
        bool ok = false;
        br_mat4 inverse = br_mat4_invert(br_mat4_mul(br_canvas3d_plane_world(e, c), br_canvas3d_plane_matrix(&c->desc)), &ok);
        if (!ok) {
            return;
        }
        br_vec3 q = br_mat4_transform_point(inverse, ev->hit.point);
        p = br_vec2_make(q.x, q.y);
        target = br_canvas3d_target(e, c, p);
    }
    if (type == BR_EVENT_POINTER_MOVE || type == BR_EVENT_POINTER_OVER || type == BR_EVENT_POINTER_OUT) {
        if (!br_node_equal(target, c->hovered)) {
            br_canvas3d_dispatch(e, BR_EVENT_POINTER_OUT, c->hovered, p, ev->source);
            br_canvas3d_dispatch(e, BR_EVENT_POINTER_OVER, target, p, ev->source);
            c->hovered = target;
        }
        if (type == BR_EVENT_POINTER_MOVE) {
            br_canvas3d_dispatch(e, BR_EVENT_POINTER_MOVE, target, p, ev->source);
        }
    } else if (type == BR_EVENT_POINTER_DOWN) {
        c->pressed = target;
        br_canvas3d_dispatch(e, type, target, p, ev->source);
    } else if (type == BR_EVENT_CLICK || type == BR_EVENT_TAP || type == BR_EVENT_RIGHT_CLICK) {
        if (br_node_equal(target, c->pressed)) {
            br_canvas3d_dispatch(e, type, target, p, ev->source);
        }
        c->pressed = none;
    } else {
        br_canvas3d_dispatch(e, type, target, p, ev->source);
    }
}

/* ---- texture mode: render the subtrees before the stage */

static void br_canvas3d_texture_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_canvas3d_state *s = user;
    if (br_engine_gpu(e) == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->canvases.count; i++) {
        br_canvas3d_rec *c = *(br_canvas3d_rec **)br_array_at(&s->canvases, i);
        if (c->desc.mode != BR_CANVAS3D_TEXTURE || c->desc.width <= 0.0f || c->desc.height <= 0.0f) {
            continue;
        }
        float res = c->desc.resolution > 0.0f ? c->desc.resolution : 2.0f;
        br_texture_info info = { 0 };
        if (c->target.generation != 0 && br_texture_get_info(e, c->target, &info) && info.resolution != res) {
            br_texture_destroy(e, c->target);
            c->target.generation = 0;
        }
        if (c->target.generation == 0) {
            c->target = br_render_texture_create(e, c->desc.width, c->desc.height, res);
        } else if (info.width != c->desc.width || info.height != c->desc.height) {
            br_render_texture_resize(e, c->target, c->desc.width, c->desc.height);
        }
        br_render_to_texture(e, c->root, c->target);
    }
}

/* ---- API */

uint32_t br_canvas3d_type(br_engine *e)
{
    return e->canvas3d != NULL ? e->canvas3d->type : 0;
}

br_node3d br_canvas3d_create(br_engine *e, const br_canvas3d_desc *desc)
{
    br_node3d n = br_node3d_create(e, br_canvas3d_type(e));
    br_canvas3d_rec *c = br_canvas3d_rec_get(e, n);
    if (c == NULL) {
        br_node3d none = { 0, 0 };
        br_node3d_destroy(e, n);
        return none;
    }
    if (desc != NULL) {
        c->desc = *desc;
    }
    br_event_type types[] = { BR_EVENT_POINTER_DOWN, BR_EVENT_POINTER_UP, BR_EVENT_POINTER_MOVE, BR_EVENT_POINTER_OVER,
                              BR_EVENT_POINTER_OUT, BR_EVENT_CLICK, BR_EVENT_RIGHT_CLICK, BR_EVENT_TAP, BR_EVENT_WHEEL };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(types); i++) {
        br_node3d_on(e, n, types[i], br_canvas3d_event, c);
    }
    return n;
}

br_canvas3d_desc *br_canvas3d_get(br_engine *e, br_node3d n)
{
    br_canvas3d_rec *c = br_canvas3d_rec_get(e, n);
    return c != NULL ? &c->desc : NULL;
}

br_node br_canvas3d_root(br_engine *e, br_node3d n)
{
    br_canvas3d_rec *c = br_canvas3d_rec_get(e, n);
    br_node none = { 0, 0 };
    return c != NULL ? c->root : none;
}

br_node br_canvas3d_content(br_engine *e, br_node3d n)
{
    br_canvas3d_rec *c = br_canvas3d_rec_get(e, n);
    br_node none = { 0, 0 };
    if (c == NULL || br_node_child_count(e, c->root) == 0) {
        return none;
    }
    return br_node_child_at(e, c->root, 0);
}

static br_node3d br_canvas3d_with_child(br_engine *e, br_node child, float width, float height, float ppu)
{
    br_canvas3d_desc d = br_canvas3d_default(width, height);
    d.pixels_per_unit = ppu > 0.0f ? ppu : 100.0f;
    br_node3d n = br_canvas3d_create(e, &d);
    br_canvas3d_rec *c = br_canvas3d_rec_get(e, n);
    if (c == NULL) {
        br_node_destroy(e, child);
        return n;
    }
    br_node_add_child(e, c->root, child);
    return n;
}

br_node3d br_sprite3d_create(br_engine *e, br_texture texture, float pixels_per_unit)
{
    br_texture_info info;
    if (!br_texture_get_info(e, texture, &info)) {
        info.width = 1.0f;
        info.height = 1.0f;
    }
    return br_canvas3d_with_child(e, br_sprite_create(e, texture), info.width, info.height, pixels_per_unit);
}

br_font *br_canvas3d_sdf_font(br_engine *e)
{
    br_canvas3d_state *s = e->canvas3d;
    if (s != NULL && s->sdf_font == NULL) {
        size_t size = 0;
        const void *data = br_font_default_data(&size);
        br_font_options opts;
        SDL_zero(opts);
        opts.sdf = true;
        s->sdf_font = br_font_create(e, data, size, &opts);
    }
    return s != NULL ? s->sdf_font : NULL;
}

br_node3d br_text3d_create(br_engine *e, const char *utf8, const br_text_style *style, float pixels_per_unit)
{
    br_text_style st = style != NULL ? *style : br_text_style_default();
    if (st.font == NULL && st.font_family[0] == '\0') {
        st.font = br_canvas3d_sdf_font(e);
    }
    br_text_metrics m = br_text_measure(e, utf8, &st);
    return br_canvas3d_with_child(e, br_text_create(e, utf8, &st), SDL_max(m.width, 1.0f), SDL_max(m.height, 1.0f),
                                  pixels_per_unit);
}

br_node3d br_nine_slice3d_create(br_engine *e, br_texture texture, float left, float top, float right, float bottom,
                                 float width, float height, float pixels_per_unit)
{
    br_node ns = br_nine_slice_create(e, texture, left, top, right, bottom);
    br_nine_slice_set_size(e, ns, width, height);
    return br_canvas3d_with_child(e, ns, width, height, pixels_per_unit);
}

/* ---- module */

static void br_canvas3d_shutdown(br_engine *e, void *user)
{
    br_canvas3d_state *s = user;
    if (s->sdf_font != NULL) {
        br_font_destroy(e, s->sdf_font);
    }
    br_array_free(&s->canvases);
    br_array_free(&s->cmds);
    br_free(s);
    e->canvas3d = NULL;
}

void br_canvas3d_register(br_engine *e)
{
    br_canvas3d_state *s = br_alloc(sizeof(br_canvas3d_state));
    if (s == NULL) {
        return;
    }
    e->canvas3d = s;
    br_array_init(&s->canvases, sizeof(br_canvas3d_rec *));
    br_array_init(&s->cmds, sizeof(br_draw_cmd));
    s->cmds_frame = UINT64_MAX;
    br_node3d_type_desc d;
    SDL_zero(d);
    d.name = "canvas3d";
    d.data_size = sizeof(br_canvas3d_node);
    d.on_create = br_canvas3d_on_create;
    d.on_destroy = br_canvas3d_on_destroy;
    d.local_bounds = br_canvas3d_bounds;
    d.collect = br_canvas3d_collect;
    d.pick = br_canvas3d_pick;
    s->type = br_node3d_type_register(e, &d);
    br_engine_on_shutdown(e, br_canvas3d_shutdown, s);
    br_system_add(e, "canvas3d.textures", BR_PHASE_PRE_RENDER, 900, br_canvas3d_texture_system, s);
}
