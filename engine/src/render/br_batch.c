/* br_batch: the draw list. Pushes (collect) append commands and vertices; replay (RENDER)
 * turns them into SDL_GPU passes and draws. */
#include "render/br_render_internal.h"
#include "braty/br_profiler.h"
#include "gpu/br_gpu_internal.h"

#include "batch.frag.h"
#include "batch.vert.h"

/* ---- blend and stencil states */

SDL_GPUColorTargetBlendState br_render_blend_state(br_blend_mode blend)
{
    SDL_GPUColorTargetBlendState b;
    SDL_zero(b);
    b.enable_blend = true;
    b.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    switch (blend) {
    case BR_BLEND_ADD:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case BR_BLEND_MULTIPLY:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BR_BLEND_SCREEN:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        break;
    case BR_BLEND_ERASE:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        break;
    case BR_BLEND_NONE:
        b.enable_blend = false;
        break;
    case BR_BLEND_NORMAL_NPM:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BR_BLEND_ADD_NPM:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case BR_BLEND_SCREEN_NPM:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        break;
    case BR_BLEND_NORMAL:
    default:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
    return b;
}

/* Fills blend, formats and stencil of a pipeline for `state` on `rt`. False = skip the draw
 * (stencil write on a target without stencil). */
static bool br_pipeline_fit(br_pipeline_desc *desc, const br_draw_state *state, const br_render_target *rt)
{
    desc->color_format = rt->color_format;
    desc->depth_stencil_format = br_gpu_render_target_depth_format(rt);
    desc->blend = br_render_blend_state(state->blend);
    if (state->stencil == BR_STENCIL_OFF) {
        return true;
    }
    if (desc->depth_stencil_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        return state->stencil == BR_STENCIL_TEST; /* test: draw unmasked; writes: drop */
    }
    SDL_GPUStencilOpState op;
    SDL_zero(op);
    op.fail_op = SDL_GPU_STENCILOP_KEEP;
    op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    op.compare_op = SDL_GPU_COMPAREOP_EQUAL;
    op.pass_op = SDL_GPU_STENCILOP_KEEP;
    desc->stencil_test = true;
    desc->stencil_read_mask = 0xFF;
    desc->stencil_write_mask = 0;
    if (state->stencil != BR_STENCIL_TEST) {
        op.pass_op = state->stencil == BR_STENCIL_INCR ? SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP
                                                       : SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
        desc->stencil_write_mask = 0xFF;
        /* mask geometry only touches the stencil buffer; one blend state keeps pipelines few */
        desc->blend = br_render_blend_state(BR_BLEND_NORMAL);
        desc->blend.enable_color_write_mask = true;
        desc->blend.color_write_mask = 0;
    }
    desc->stencil_front = op;
    desc->stencil_back = op;
    return true;
}

void br_render_pipeline_setup(const br_render_callback_info *info, br_pipeline_desc *desc)
{
    br_draw_state state;
    SDL_zero(state);
    state.blend = info->blend;
    state.stencil = info->stencil_test ? BR_STENCIL_TEST : BR_STENCIL_OFF;
    br_pipeline_fit(desc, &state, info->target);
}

/* ---- transient streams */

/* The headless stream: same 16-byte alignment as br_gpu's, so offsets (and batch merging) match. */
static void *br_render_headless_alloc(br_array *stream, uint32_t size, uint32_t *offset)
{
    uint32_t start = (stream->count + 15u) & ~15u;
    if (!br_array_resize(stream, start + size)) {
        return NULL;
    }
    *offset = start;
    return (uint8_t *)stream->data + start;
}

void *br_render_transient_vertices(br_engine *e, uint32_t size, uint32_t *offset)
{
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        return br_gpu_transient_alloc_vertices(g, size, offset);
    }
    return br_render_headless_alloc(&e->render->headless_vertices, size, offset);
}

void *br_render_transient_indices(br_engine *e, uint32_t size, uint32_t *offset)
{
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        return br_gpu_transient_alloc_indices(g, size, offset);
    }
    return br_render_headless_alloc(&e->render->headless_indices, size, offset);
}

const uint8_t *br_render_transient_data(br_engine *e, bool indices)
{
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        return indices ? g->indices.cpu.data : g->vertices.cpu.data;
    }
    return indices ? e->render->headless_indices.data : e->render->headless_vertices.data;
}

/* ---- recording */

static br_draw_state br_batch_state_of(const br_render_ctx *rc)
{
    br_draw_state s;
    s.blend = rc->blend;
    s.stencil = rc->stencil;
    s.stencil_ref = rc->stencil_ref;
    s.scissor_on = rc->scissor_on;
    s.scissor = rc->scissor;
    s.transform = rc->transform;
    return s;
}

static br_draw_cmd *br_batch_new_cmd(br_render_ctx *rc, br_draw_kind kind)
{
    if (rc->open_batch >= 0 && (kind == BR_DRAW_CUSTOM || kind == BR_DRAW_CALLBACK)) {
        br_profiler_count(rc->e, BR_PROFILER_BREAK_SHADER, 1);
    }
    br_draw_cmd *cmd = br_array_push(&rc->e->render->cmds, NULL);
    if (cmd != NULL) {
        cmd->kind = kind;
        cmd->state = br_batch_state_of(rc);
        cmd->serial = rc->serial;
    }
    rc->open_batch = -1;
    return cmd;
}

void br_batch_push_pass(br_render_ctx *rc, br_texture target, bool clear, br_color clear_color)
{
    br_draw_cmd *cmd = br_batch_new_cmd(rc, BR_DRAW_PASS);
    if (cmd != NULL) {
        cmd->target = target;
        cmd->clear = clear;
        cmd->clear_color = clear_color;
        rc->e->render->building.passes += 1;
    }
    br_render_ctx_changed(rc);
}

/* Returns the batch that takes `vertex_count` more vertices of `source` written at byte
 * `vertex_offset` of the transient vertex buffer, opening one if needed. */
static br_draw_cmd *br_batch_for(br_render_ctx *rc, br_texture source, uint32_t vertex_count, uint32_t vertex_offset,
                                 uint32_t *slot)
{
    br_render_state *r = rc->e->render;
    br_draw_cmd *b = rc->open_batch >= 0 ? (br_draw_cmd *)r->cmds.data + rc->open_batch : NULL;
    /* the vertices must directly follow the batch's: anything else allocating transient vertices
     * in between (custom draws, callbacks) starts a new batch */
    if (b != NULL && (b->serial != rc->serial || b->state.blend != rc->blend ||
                      b->vertex_count + vertex_count > BR_RENDER_MAX_BATCH_VERTICES ||
                      b->vertex_offset + b->vertex_count * (uint32_t)sizeof(br_batch_vertex) != vertex_offset)) {
        br_profiler_count(rc->e, b->state.blend != rc->blend ? BR_PROFILER_BREAK_BLEND : BR_PROFILER_BREAK_OTHER, 1);
        b = NULL;
    }
    if (b != NULL) {
        for (uint32_t i = 0; i < b->texture_count; i++) {
            if (b->textures[i].index == source.index && b->textures[i].generation == source.generation) {
                *slot = i;
                return b;
            }
        }
        if (b->texture_count < BR_RENDER_MAX_TEXTURES) {
            *slot = b->texture_count;
            b->textures[b->texture_count] = source;
            b->texture_count += 1;
            return b;
        }
        br_profiler_count(rc->e, BR_PROFILER_BREAK_TEXTURE, 1);
    }
    b = br_batch_new_cmd(rc, BR_DRAW_BATCH);
    if (b == NULL) {
        return NULL;
    }
    b->first_index = r->indices.count;
    b->vertex_offset = vertex_offset;
    b->textures[0] = source;
    b->texture_count = 1;
    *slot = 0;
    rc->open_batch = (int)(r->cmds.count - 1);
    r->building.batches += 1;
    r->building.draw_calls += 1;
    return b;
}

/* The source texture to bind for `texture`, remembering the last one (most pushes repeat it). */
static br_texture br_batch_source(br_render_ctx *rc, br_texture texture)
{
    if (texture.generation == 0 || texture.index != rc->last_texture.index ||
        texture.generation != rc->last_texture.generation) {
        rc->last_texture = texture;
        rc->last_source = br_texture_source_of(rc->e, texture);
    }
    return rc->last_source;
}

/* Reserves vertex_count vertices (straight in the transient vertex buffer, no copy later) and
 * index_count indices in a batch. *base = the first vertex's index within the batch. */
static br_batch_vertex *br_batch_alloc(br_render_ctx *rc, br_texture texture, uint32_t vertex_count,
                                       uint32_t index_count, uint32_t *slot, uint32_t *base, uint16_t **indices)
{
    br_render_state *r = rc->e->render;
    /* an even count makes every allocation a multiple of 48 bytes, so the transient buffer's
     * 16-byte alignment adds no gap and the next push stays contiguous with this batch */
    uint32_t padded = vertex_count + (vertex_count & 1u);
    uint32_t offset;
    br_batch_vertex *v = br_render_transient_vertices(rc->e, padded * (uint32_t)sizeof(br_batch_vertex), &offset);
    if (v == NULL) {
        return NULL;
    }
    br_draw_cmd *b = br_batch_for(rc, br_batch_source(rc, texture), padded, offset, slot);
    if (b == NULL || !br_array_reserve(&r->indices, r->indices.count + index_count)) {
        return NULL;
    }
    uint16_t *idx = (uint16_t *)r->indices.data + r->indices.count; /* filled by the caller: no zeroing */
    r->indices.count += index_count;
    *base = b->vertex_count;
    *indices = idx;
    b->vertex_count += padded;
    b->index_count += index_count;
    r->building.vertices += vertex_count;
    return v;
}

br_batch_vertex *br_batch_quad(br_render_ctx *rc, br_texture texture, uint32_t *slot)
{
    uint32_t base;
    uint16_t *idx;
    br_batch_vertex *v = br_batch_alloc(rc, texture, 4, 6, slot, &base, &idx);
    if (v == NULL) {
        return NULL;
    }
    idx[0] = (uint16_t)base;
    idx[1] = (uint16_t)(base + 1);
    idx[2] = (uint16_t)(base + 2);
    idx[3] = (uint16_t)base;
    idx[4] = (uint16_t)(base + 2);
    idx[5] = (uint16_t)(base + 3);
    return v;
}

void br_render_push_quad(br_render_ctx *rc, br_texture texture, const br_vertex2d v[4])
{
    uint32_t slot;
    br_batch_vertex *out = br_batch_quad(rc, texture, &slot);
    if (out == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        out[i].x = v[i].x;
        out[i].y = v[i].y;
        out[i].u = v[i].u;
        out[i].v = v[i].v;
        out[i].color = v[i].color;
        out[i].texture = slot;
    }
}

void br_render_push_triangles(br_render_ctx *rc, br_texture texture, const br_vertex2d *vertices,
                              uint32_t vertex_count, const uint16_t *indices, uint32_t index_count)
{
    if (vertex_count == 0 || index_count == 0) {
        return;
    }
    if (vertex_count > BR_RENDER_MAX_BATCH_VERTICES) {
        br_log_error("br_render_push_triangles: %u vertices (max %u per call; split it or use br_render_push_custom)",
                     vertex_count, BR_RENDER_MAX_BATCH_VERTICES);
        return;
    }
    for (uint32_t i = 0; i < index_count; i++) {
        if (indices[i] >= vertex_count) {
            br_log_error("br_render_push_triangles: index %u out of range", indices[i]);
            return;
        }
    }
    uint32_t slot;
    uint32_t base;
    uint16_t *idx;
    br_batch_vertex *out = br_batch_alloc(rc, texture, vertex_count, index_count, &slot, &base, &idx);
    if (out == NULL) {
        return;
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        out[i].x = vertices[i].x;
        out[i].y = vertices[i].y;
        out[i].u = vertices[i].u;
        out[i].v = vertices[i].v;
        out[i].color = vertices[i].color;
        out[i].texture = slot;
    }
    for (uint32_t i = 0; i < index_count; i++) {
        idx[i] = (uint16_t)(base + indices[i]);
    }
}

/* Copies bytes into this frame's transient vertex/index memory. */
static bool br_batch_copy_transient(br_engine *e, bool index_data, const void *data, uint32_t size, uint32_t *offset)
{
    void *dst = index_data ? br_render_transient_indices(e, size, offset) : br_render_transient_vertices(e, size, offset);
    if (dst == NULL) {
        return false;
    }
    SDL_memcpy(dst, data, size);
    return true;
}

static uint32_t br_batch_copy_uniforms(br_render_state *r, const void *data, uint32_t size)
{
    uint32_t offset = r->uniforms.count;
    if (size > 0 && data != NULL) {
        br_array_push_n(&r->uniforms, data, size);
    }
    return offset;
}

void br_render_push_custom(br_render_ctx *rc, const br_custom_draw *draw)
{
    br_engine *e = rc->e;
    br_render_state *r = e->render;
    if (draw->vertex_shader == NULL || draw->fragment_shader == NULL || draw->layout.buffer_count == 0 ||
        draw->layout.strides[0] == 0 || draw->vertices == NULL || draw->vertex_bytes == 0 ||
        draw->texture_count > BR_RENDER_MAX_TEXTURES) {
        br_log_error("br_render_push_custom: incomplete draw (shaders, layout, vertices)");
        return;
    }
    br_custom_cmd cc;
    SDL_zero(cc);
    cc.vertex_shader = draw->vertex_shader;
    cc.fragment_shader = draw->fragment_shader;
    cc.layout = draw->layout;
    cc.primitive = draw->primitive;
    cc.vertex_count = draw->vertex_bytes / draw->layout.strides[0];
    if (!br_batch_copy_transient(e, false, draw->vertices, draw->vertex_bytes, &cc.vertex_offset)) {
        return;
    }
    cc.instance_count = draw->instance_count > 0 ? draw->instance_count : 1;
    if (draw->instances != NULL && draw->instance_bytes > 0) {
        cc.instanced = true;
        if (!br_batch_copy_transient(e, false, draw->instances, draw->instance_bytes, &cc.instance_offset)) {
            return;
        }
    }
    if (draw->indices != NULL && draw->index_count > 0) {
        cc.indexed = true;
        cc.index_count = draw->index_count;
        if (!br_batch_copy_transient(e, true, draw->indices, draw->index_count * 4, &cc.index_offset)) {
            return;
        }
    }
    cc.texture_count = draw->texture_count;
    for (uint32_t i = 0; i < draw->texture_count; i++) {
        cc.textures[i] = br_texture_source_of(e, draw->textures[i]);
    }
    cc.vertex_uniform_size = draw->vertex_uniforms != NULL ? draw->vertex_uniform_size : 0;
    cc.vertex_uniform_offset = br_batch_copy_uniforms(r, draw->vertex_uniforms, cc.vertex_uniform_size);
    cc.fragment_uniform_size = draw->fragment_uniforms != NULL ? draw->fragment_uniform_size : 0;
    cc.fragment_uniform_offset = br_batch_copy_uniforms(r, draw->fragment_uniforms, cc.fragment_uniform_size);
    br_draw_cmd *cmd = br_batch_new_cmd(rc, BR_DRAW_CUSTOM);
    if (cmd == NULL || br_array_push(&r->customs, &cc) == NULL) {
        return;
    }
    cmd->custom = r->customs.count - 1;
    r->building.draw_calls += 1;
    br_profiler_count(e, BR_PROFILER_TRIANGLES, (int64_t)(cc.indexed ? cc.index_count : cc.vertex_count) / 3 * cc.instance_count);
    r->building.vertices += cc.vertex_count * cc.instance_count;
}

void br_render_push_callback(br_render_ctx *rc, br_render_callback_fn fn, void *user, bool own_passes)
{
    if (fn == NULL) {
        return;
    }
    br_draw_cmd *cmd = br_batch_new_cmd(rc, BR_DRAW_CALLBACK);
    if (cmd != NULL) {
        cmd->fn = fn;
        cmd->user = user;
        cmd->own_passes = own_passes;
    }
}

void br_batch_reset(br_engine *e)
{
    br_render_state *r = e->render;
    br_array_clear(&r->cmds);
    br_array_clear(&r->customs);
    br_array_clear(&r->indices);
    br_array_clear(&r->uniforms);
    br_array_clear(&r->headless_vertices);
    br_array_clear(&r->headless_indices);
    SDL_zero(r->building);
    r->collected = false;
}

void br_batch_finish(br_engine *e)
{
    br_render_state *r = e->render;
    if (r->indices.count > 0) {
        br_batch_copy_transient(e, true, r->indices.data, r->indices.count * (uint32_t)sizeof(uint16_t),
                                &r->index_base);
    }
    r->stats = r->building;
    br_profiler_count(e, BR_PROFILER_TRIANGLES, r->indices.count / 3);
}

/* ---- replay */

typedef struct br_replay {
    br_engine               *e;
    br_gpu                  *g;
    SDL_GPUCommandBuffer    *cmd;
    SDL_GPURenderPass       *pass;
    br_render_target        *rt;
    br_texture               target;
    SDL_GPUGraphicsPipeline *bound;
} br_replay;

static void br_replay_begin(br_replay *rp, br_texture target, bool clear, br_color clear_color)
{
    if (rp->pass != NULL) {
        SDL_EndGPURenderPass(rp->pass);
        rp->pass = NULL;
    }
    rp->bound = NULL;
    rp->target = target;
    if (target.generation == 0) {
        rp->rt = br_gpu_frame_target(rp->g);
        rp->pass = br_gpu_frame_begin_pass(rp->g); /* the frame's first pass clears to the clear colour */
    } else {
        rp->rt = br_texture_render_target(rp->e, target);
        if (rp->rt != NULL) {
            rp->pass = br_gpu_render_target_begin(rp->g, rp->cmd, rp->rt, clear, clear_color);
        }
    }
}

/* Binds pipeline, scissor, stencil reference and the transform uniform for a draw. */
static bool br_replay_prepare(br_replay *rp, br_pipeline_desc *desc, const br_draw_state *state)
{
    if (!br_pipeline_fit(desc, state, rp->rt)) {
        return false;
    }
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(rp->g, desc);
    if (pipeline == NULL) {
        return false;
    }
    if (pipeline != rp->bound) {
        br_gpu_bind_pipeline(br_engine_gpu(rp->e), rp->pass, pipeline);
        rp->bound = pipeline;
        br_profiler_count(rp->e, BR_PROFILER_PIPELINE_SWITCHES, 1);
    }
    SDL_Rect scissor = { 0, 0, (int)rp->rt->width, (int)rp->rt->height };
    if (state->scissor_on) {
        /* clamp: the window may have been resized between collect and submit */
        SDL_Rect clamped;
        if (!SDL_GetRectIntersection(&state->scissor, &scissor, &clamped)) {
            return false;
        }
        scissor = clamped;
    }
    SDL_SetGPUScissor(rp->pass, &scissor);
    if (desc->stencil_test) {
        SDL_SetGPUStencilReference(rp->pass, state->stencil_ref);
    }
    SDL_PushGPUVertexUniformData(rp->cmd, 0, &state->transform, sizeof(br_mat4));
    return true;
}

static void br_replay_batch(br_replay *rp, const br_draw_cmd *c)
{
    br_render_state *r = rp->e->render;
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_batch_vert;
    desc.fragment_shader = &br_shader_batch_frag;
    desc.layout.buffer_count = 1;
    desc.layout.strides[0] = sizeof(br_batch_vertex);
    desc.layout.attribute_count = 4;
    desc.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    desc.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    desc.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    desc.layout.attributes[3] = (SDL_GPUVertexAttribute){ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 20 };

    SDL_GPUTextureSamplerBinding bindings[BR_RENDER_MAX_TEXTURES];
    for (uint32_t i = 0; i < BR_RENDER_MAX_TEXTURES; i++) {
        /* unused slots get the white texture: every declared sampler must be bound */
        br_texture t = i < c->texture_count ? c->textures[i] : br_texture_white(rp->e);
        if (!br_texture_binding(rp->e, t, &bindings[i])) {
            return;
        }
    }
    if (!br_replay_prepare(rp, &desc, &c->state)) {
        return;
    }
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(rp->g), c->vertex_offset };
    SDL_BindGPUVertexBuffers(rp->pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { br_gpu_transient_index_buffer(rp->g), r->index_base };
    SDL_BindGPUIndexBuffer(rp->pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_BindGPUFragmentSamplers(rp->pass, 0, bindings, BR_RENDER_MAX_TEXTURES);
    br_gpu_draw_indexed(br_engine_gpu(rp->e), rp->pass, c->index_count, 1, c->first_index, 0, 0);
}

static void br_replay_custom(br_replay *rp, const br_draw_cmd *c)
{
    br_render_state *r = rp->e->render;
    const br_custom_cmd *cc = br_array_at(&r->customs, c->custom);
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = cc->vertex_shader;
    desc.fragment_shader = cc->fragment_shader;
    desc.layout = cc->layout;
    desc.primitive = cc->primitive;
    SDL_GPUTextureSamplerBinding bindings[BR_RENDER_MAX_TEXTURES];
    for (uint32_t i = 0; i < cc->texture_count; i++) {
        if (!br_texture_binding(rp->e, cc->textures[i], &bindings[i])) {
            return;
        }
    }
    if (!br_replay_prepare(rp, &desc, &c->state)) {
        return;
    }
    SDL_GPUBufferBinding vbs[2];
    vbs[0].buffer = br_gpu_transient_vertex_buffer(rp->g);
    vbs[0].offset = cc->vertex_offset;
    vbs[1].buffer = vbs[0].buffer;
    vbs[1].offset = cc->instance_offset;
    SDL_BindGPUVertexBuffers(rp->pass, 0, vbs, cc->instanced ? 2 : 1);
    if (cc->texture_count > 0) {
        SDL_BindGPUFragmentSamplers(rp->pass, 0, bindings, cc->texture_count);
    }
    const uint8_t *uniforms = r->uniforms.data;
    if (cc->vertex_uniform_size > 0) {
        SDL_PushGPUVertexUniformData(rp->cmd, 1, uniforms + cc->vertex_uniform_offset, cc->vertex_uniform_size);
    }
    if (cc->fragment_uniform_size > 0) {
        SDL_PushGPUFragmentUniformData(rp->cmd, 0, uniforms + cc->fragment_uniform_offset, cc->fragment_uniform_size);
    }
    if (cc->indexed) {
        SDL_GPUBufferBinding ib = { br_gpu_transient_index_buffer(rp->g), cc->index_offset };
        SDL_BindGPUIndexBuffer(rp->pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        br_gpu_draw_indexed(br_engine_gpu(rp->e), rp->pass, cc->index_count, cc->instance_count, 0, 0, 0);
    } else {
        br_gpu_draw(br_engine_gpu(rp->e), rp->pass, cc->vertex_count, cc->instance_count, 0, 0);
    }
}

static void br_replay_callback(br_replay *rp, const br_draw_cmd *c)
{
    br_render_callback_info info;
    SDL_zero(info);
    info.cmd = rp->cmd;
    info.target = rp->rt;
    info.transform = c->state.transform;
    info.blend = c->state.blend;
    info.stencil_test = c->state.stencil == BR_STENCIL_TEST;
    info.stencil_ref = c->state.stencil_ref;
    SDL_Rect full = { 0, 0, (int)rp->rt->width, (int)rp->rt->height };
    info.scissor = full;
    if (c->state.scissor_on) {
        /* clamp like batches do; an empty clip leaves nothing for an in-pass callback to draw */
        info.scissor_on = true;
        if (!SDL_GetRectIntersection(&c->state.scissor, &full, &info.scissor)) {
            SDL_zero(info.scissor);
            if (!c->own_passes) {
                return;
            }
        }
    }
    if (c->own_passes) {
        if (rp->pass != NULL) {
            SDL_EndGPURenderPass(rp->pass);
            rp->pass = NULL;
        }
        c->fn(rp->e, &info, c->user);
        br_replay_begin(rp, rp->target, false, br_color_make(0, 0, 0, 0)); /* reopen, keeping contents */
    } else {
        info.pass = rp->pass;
        SDL_SetGPUScissor(rp->pass, &info.scissor);
        c->fn(rp->e, &info, c->user);
        rp->bound = NULL; /* the callback may have bound its own pipeline */
    }
}

void br_batch_replay(br_engine *e)
{
    br_render_state *r = e->render;
    br_replay rp;
    SDL_zero(rp);
    rp.e = e;
    rp.g = br_engine_gpu(e);
    rp.cmd = rp.g != NULL ? br_gpu_frame_cmd(rp.g) : NULL;
    if (rp.cmd == NULL || !br_gpu_transient_upload(rp.g, rp.cmd)) {
        return;
    }
    for (uint32_t i = 0; i < r->cmds.count; i++) {
        const br_draw_cmd *c = br_array_at(&r->cmds, i);
        if (c->kind == BR_DRAW_PASS) {
            br_replay_begin(&rp, c->target, c->clear, c->clear_color);
            continue;
        }
        if (rp.rt == NULL || (rp.pass == NULL && !(c->kind == BR_DRAW_CALLBACK && c->own_passes))) {
            continue;
        }
        if (c->kind == BR_DRAW_BATCH) {
            br_replay_batch(&rp, c);
        } else if (c->kind == BR_DRAW_CUSTOM) {
            br_replay_custom(&rp, c);
        } else {
            br_replay_callback(&rp, c);
        }
    }
    if (rp.pass != NULL) {
        SDL_EndGPURenderPass(rp.pass);
    }
}
