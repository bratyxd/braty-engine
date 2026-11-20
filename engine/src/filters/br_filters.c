/* br_filters: the effects hook (filters, alpha/colour masks, cache_as_texture, advanced blend
 * modes), the render-target pool and per-node filter lists.
 *
 * Collect (inside the hook): the node's subtree is drawn into a pooled texture with
 * br_render_subtree_to_texture; filter passes are recorded as a list and replayed by one
 * own-passes callback (br_fx_run_job) in RENDER; then a quad (or a blend-shader draw) puts the
 * result back into the current target. Commands replay in order, so the callback runs after the
 * subtree render and before the quad. */
#include "filters/br_filters_internal.h"
#include "render/br_render_internal.h"

#include "filter.vert.h"
#include "filter_blend.frag.h"
#include "filter_blur.frag.h"
#include "filter_color_matrix.frag.h"
#include "filter_displacement.frag.h"
#include "filter_mask.frag.h"
#include "filter_noise.frag.h"
#include "textured.vert.h"

#define BR_POOL_MIN_SIZE 16
#define BR_POOL_IDLE_FRAMES 120

static br_texture br_fx_no_texture(void)
{
    br_texture t = { 0, 0 };
    return t;
}

static br_color br_fx_transparent(void)
{
    return br_color_make(0.0f, 0.0f, 0.0f, 0.0f);
}

/* ---- render-target pool */

uint32_t br_filters_pool_bucket(uint32_t pixels)
{
    uint32_t size = BR_POOL_MIN_SIZE;
    while (size < pixels && size < BR_FILTER_MAX_SIZE) {
        size *= 2;
    }
    return size;
}

br_texture br_filters_pool_acquire(br_engine *e, uint32_t width, uint32_t height)
{
    br_filters_state *fs = e->filters;
    if (fs == NULL) {
        return br_fx_no_texture();
    }
    uint32_t bw = br_filters_pool_bucket(width);
    uint32_t bh = br_filters_pool_bucket(height);
    for (uint32_t i = 0; i < fs->pool.count; i++) {
        br_pool_entry *p = br_array_at(&fs->pool, i);
        if (!p->in_use && p->width == bw && p->height == bh && br_texture_valid(e, p->texture)) {
            p->in_use = true;
            p->last_used = fs->frame;
            return p->texture;
        }
    }
    br_pool_entry entry;
    SDL_zero(entry);
    entry.texture = br_render_texture_create(e, (float)bw, (float)bh, 1.0f);
    if (entry.texture.generation == 0) {
        return br_fx_no_texture();
    }
    entry.width = bw;
    entry.height = bh;
    entry.in_use = true;
    entry.last_used = fs->frame;
    br_array_push(&fs->pool, &entry);
    return entry.texture;
}

void br_filters_pool_release_all(br_engine *e)
{
    br_filters_state *fs = e->filters;
    if (fs == NULL) {
        return;
    }
    for (uint32_t i = 0; i < fs->pool.count; i++) {
        br_pool_entry *p = br_array_at(&fs->pool, i);
        p->in_use = false;
    }
}

/* Destroys free entries; only those idle for more than max_idle frames unless max_idle is 0. */
static void br_fx_pool_trim_idle(br_engine *e, uint64_t max_idle)
{
    br_filters_state *fs = e->filters;
    uint32_t i = 0;
    while (i < fs->pool.count) {
        br_pool_entry *p = br_array_at(&fs->pool, i);
        bool idle = max_idle == 0 || fs->frame - p->last_used > max_idle;
        if (!p->in_use && idle) {
            br_texture_destroy(e, p->texture);
            br_array_remove_swap(&fs->pool, i);
        } else {
            i++;
        }
    }
}

void br_filters_pool_trim(br_engine *e)
{
    if (e->filters != NULL) {
        br_fx_pool_trim_idle(e, 0);
    }
}

uint32_t br_filters_pool_count(br_engine *e)
{
    return e->filters != NULL ? e->filters->pool.count : 0;
}

/* ---- per-node records */

static br_fx_record *br_fx_record_get(br_engine *e, br_node n)
{
    br_filters_state *fs = e->filters;
    if (fs == NULL || !br_node_valid(e, n) || n.index >= fs->records.count) {
        return NULL;
    }
    return *(br_fx_record **)br_array_at(&fs->records, n.index);
}

static br_fx_record *br_fx_record_ensure(br_engine *e, br_node n)
{
    br_filters_state *fs = e->filters;
    if (fs == NULL || !br_node_valid(e, n)) {
        return NULL;
    }
    br_fx_record *rec = br_fx_record_get(e, n);
    if (rec != NULL) {
        return rec;
    }
    if (n.index >= fs->records.count && !br_array_resize(&fs->records, n.index + 1)) {
        return NULL;
    }
    rec = br_alloc(sizeof(br_fx_record));
    if (rec == NULL) {
        return NULL;
    }
    SDL_zerop(rec);
    rec->node = n;
    rec->resolution = 1.0f;
    *(br_fx_record **)br_array_at(&fs->records, n.index) = rec;
    return rec;
}

static void br_fx_record_free(br_engine *e, uint32_t index)
{
    br_filters_state *fs = e->filters;
    if (fs == NULL || index >= fs->records.count) {
        return;
    }
    br_fx_record **slot = br_array_at(&fs->records, index);
    if (*slot != NULL) {
        br_texture_destroy(e, (*slot)->cache);
        br_free(*slot);
        *slot = NULL;
    }
}

static void br_fx_on_node_destroy(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_fx_record_free(e, n.index);
}

bool br_filters_set(br_engine *e, br_node n, const br_filter *filters, uint32_t count)
{
    if (count > BR_FILTER_MAX_PER_NODE || (count > 0 && filters == NULL)) {
        br_log_error("br_filters_set: %u filters (max %u)", count, BR_FILTER_MAX_PER_NODE);
        return false;
    }
    br_fx_record *rec = count > 0 ? br_fx_record_ensure(e, n) : br_fx_record_get(e, n);
    if (rec == NULL) {
        return count == 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        rec->filters[i] = filters[i];
    }
    rec->count = count;
    br_node_set_filters(e, n, count > 0 ? rec : NULL);
    return true;
}

uint32_t br_filters_count(br_engine *e, br_node n)
{
    br_fx_record *rec = br_fx_record_get(e, n);
    return rec != NULL ? rec->count : 0;
}

br_filter *br_filters_get(br_engine *e, br_node n, uint32_t i)
{
    br_fx_record *rec = br_fx_record_get(e, n);
    return rec != NULL && i < rec->count ? &rec->filters[i] : NULL;
}

void br_filters_set_resolution(br_engine *e, br_node n, float resolution)
{
    br_fx_record *rec = br_fx_record_ensure(e, n);
    if (rec != NULL) {
        rec->resolution = resolution > 0.0f ? resolution : 1.0f;
    }
}

void br_filters_set_area(br_engine *e, br_node n, const br_rect *local_area)
{
    br_fx_record *rec = br_fx_record_ensure(e, n);
    if (rec != NULL) {
        rec->use_area = local_area != NULL;
        if (local_area != NULL) {
            rec->area = *local_area;
        }
    }
}

void br_cache_update_texture(br_engine *e, br_node n)
{
    br_fx_record *rec = br_fx_record_get(e, n);
    if (rec != NULL) {
        rec->cache_valid = false;
    }
}

/* ---- passes and jobs (recorded at collect) */

/* Starts a job whose passes are appended next. The job struct is reused across frames. */
static br_fx_job *br_fx_job_begin(br_filters_state *fs)
{
    if (fs->jobs_used == fs->jobs.count) {
        br_fx_job *job = br_alloc(sizeof(br_fx_job));
        if (job == NULL || br_array_push(&fs->jobs, &job) == NULL) {
            br_free(job);
            return NULL;
        }
    }
    br_fx_job *job = *(br_fx_job **)br_array_at(&fs->jobs, fs->jobs_used);
    fs->jobs_used += 1;
    job->fs = fs;
    job->first = fs->passes.count;
    job->count = 0;
    return job;
}

/* What the passes of one node share. */
typedef struct br_fx_chain {
    br_engine        *e;
    br_fx_job        *job;
    br_texture        current;      /* input of the next pass */
    br_texture        ping[2];      /* outputs alternate between these */
    uint32_t          next_ping;
    uint32_t          width;        /* texture size (bucket), all chain textures share it */
    uint32_t          height;
    float             scale;        /* texture pixels per world unit */
    br_filter_globals globals;
} br_fx_chain;

/* Appends a shader pass reading chain->current; uniforms follow the globals in b0. */
static br_fx_pass *br_fx_add_pass(br_fx_chain *c, const br_shader_blob *shader, const void *uniforms,
                                  uint32_t uniform_size)
{
    br_filters_state *fs = c->e->filters;
    uint32_t slot = c->next_ping;
    if (c->ping[slot].generation == 0) {
        c->ping[slot] = br_filters_pool_acquire(c->e, c->width, c->height);
        if (c->ping[slot].generation == 0) {
            return NULL;
        }
    }
    br_fx_pass *p = br_array_push(&fs->passes, NULL);
    if (p == NULL) {
        return NULL;
    }
    p->kind = BR_FX_PASS_SHADER;
    p->shader = shader;
    p->input = c->current;
    p->output = c->ping[slot];
    if (uniform_size > BR_FILTER_MAX_UNIFORM_BYTES) {
        uniform_size = BR_FILTER_MAX_UNIFORM_BYTES;
    }
    SDL_memcpy(p->block, &c->globals, sizeof(br_filter_globals));
    if (uniforms != NULL && uniform_size > 0) {
        SDL_memcpy(p->block + sizeof(br_filter_globals), uniforms, uniform_size);
    }
    p->block_size = (uint32_t)sizeof(br_filter_globals) + (uniform_size + 15u) / 16u * 16u;
    c->current = p->output;
    c->next_ping = 1 - slot;
    c->job->count += 1;
    return p;
}

typedef struct br_fx_matrix_uniforms {
    float rows[4][4];
    float offset[4];
    float mix[4];
} br_fx_matrix_uniforms;

static void br_fx_add_color_matrix(br_fx_chain *c, const float m[20], float mix)
{
    br_fx_matrix_uniforms u;
    SDL_zero(u);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            u.rows[row][col] = m[row * 5 + col];
        }
        u.offset[row] = m[row * 5 + 4];
    }
    u.mix[0] = mix;
    br_fx_add_pass(c, &br_shader_filter_color_matrix_frag, &u, sizeof(u));
}

typedef struct br_fx_blur_uniforms {
    float weights[8][4];
    float step[4];
} br_fx_blur_uniforms;

static void br_fx_add_blur(br_fx_chain *c, const br_filter *f)
{
    br_fx_blur_uniforms u;
    SDL_zero(u);
    float weights[8];
    uint32_t count = br_filter_blur_kernel(f->kernel_size, weights);
    for (uint32_t i = 0; i < count; i++) {
        u.weights[i][0] = weights[i];
    }
    float taps = (float)(count - 1);
    u.step[2] = taps;
    int quality = f->quality > 0 ? f->quality : 1;
    /* q passes of radius r blur like one of r * sqrt(q): keep the total near `strength` */
    float per_pass = c->scale / SDL_sqrtf((float)quality) / taps;
    for (int q = 0; q < quality; q++) {
        if (f->blur_x > 0.0f) {
            u.step[0] = f->blur_x * per_pass / (float)c->width;
            u.step[1] = 0.0f;
            br_fx_add_pass(c, &br_shader_filter_blur_frag, &u, sizeof(u));
        }
        if (f->blur_y > 0.0f) {
            u.step[0] = 0.0f;
            u.step[1] = f->blur_y * per_pass / (float)c->height;
            br_fx_add_pass(c, &br_shader_filter_blur_frag, &u, sizeof(u));
        }
    }
}

static void br_fx_add_displacement(br_fx_chain *c, const br_filter *f)
{
    br_texture_info map;
    if (!br_texture_get_info(c->e, f->map, &map) || map.width <= 0.0f || map.height <= 0.0f) {
        return;
    }
    float u[8];
    SDL_zeroa(u);
    u[0] = f->scale.x * c->scale / (float)c->width;
    u[1] = f->scale.y * c->scale / (float)c->height;
    u[2] = c->globals.area[2] / map.width;
    u[3] = c->globals.area[3] / map.height;
    u[4] = f->map_offset.x / map.width;
    u[5] = f->map_offset.y / map.height;
    br_fx_pass *p = br_fx_add_pass(c, &br_shader_filter_displacement_frag, u, sizeof(u));
    if (p != NULL) {
        p->extra[0] = f->map;
        p->extra_count = 1;
    }
}

static void br_fx_add_filter(br_fx_chain *c, const br_filter *f)
{
    switch (f->type) {
    case BR_FILTER_ALPHA: {
        float m[20];
        br_color_matrix_identity(m);
        m[18] = f->alpha;
        br_fx_add_color_matrix(c, m, 1.0f);
        break;
    }
    case BR_FILTER_BLUR:
        br_fx_add_blur(c, f);
        break;
    case BR_FILTER_COLOR_MATRIX:
        br_fx_add_color_matrix(c, f->matrix, f->alpha);
        break;
    case BR_FILTER_DISPLACEMENT:
        br_fx_add_displacement(c, f);
        break;
    case BR_FILTER_NOISE: {
        float u[4] = { f->noise, f->seed, 0.0f, 0.0f };
        br_fx_add_pass(c, &br_shader_filter_noise_frag, u, sizeof(u));
        break;
    }
    case BR_FILTER_CUSTOM: {
        if (f->shader == NULL || f->shader->stage != SDL_GPU_SHADERSTAGE_FRAGMENT) {
            br_log_error("custom filter without a fragment shader skipped");
            break;
        }
        br_fx_pass *p = br_fx_add_pass(c, f->shader, f->uniforms, f->uniform_size);
        if (p != NULL) {
            p->extra_count = SDL_min(f->texture_count, (uint32_t)BR_FILTER_MAX_TEXTURES);
            for (uint32_t i = 0; i < p->extra_count; i++) {
                p->extra[i] = f->textures[i];
            }
        }
        break;
    }
    }
}

/* ---- replay (RENDER) */

/* Binding for t, or the white texture when t has no GPU pixels. */
static SDL_GPUTextureSamplerBinding br_fx_binding(br_engine *e, br_texture t)
{
    SDL_GPUTextureSamplerBinding b;
    SDL_zero(b);
    br_gpu_texture *gpu = br_texture_gpu(e, t);
    br_texture_info info;
    if (gpu == NULL || !br_texture_get_info(e, t, &info)) {
        t = br_texture_white(e);
        gpu = br_texture_gpu(e, t);
        br_texture_get_info(e, t, &info);
    }
    b.texture = gpu != NULL ? gpu->handle : NULL;
    b.sampler = br_gpu_sampler_get(br_engine_gpu(e), &info.style);
    return b;
}

static void br_fx_run_shader_pass(br_engine *e, SDL_GPUCommandBuffer *cmd, const br_fx_pass *p)
{
    br_gpu *g = br_engine_gpu(e);
    br_render_target *rt = br_texture_render_target(e, p->output);
    if (rt == NULL) {
        return;
    }
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_filter_vert;
    desc.fragment_shader = p->shader;
    desc.color_format = rt->color_format;
    desc.depth_stencil_format = br_gpu_render_target_depth_format(rt);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    if (pipeline == NULL) {
        return;
    }
    SDL_GPUTextureSamplerBinding bindings[1 + BR_FILTER_MAX_TEXTURES];
    uint32_t sampler_count = SDL_min(p->shader->num_samplers, (uint32_t)(1 + BR_FILTER_MAX_TEXTURES));
    for (uint32_t i = 0; i < sampler_count; i++) {
        br_texture t = i == 0 ? p->input : (i - 1 < p->extra_count ? p->extra[i - 1] : br_texture_white(e));
        bindings[i] = br_fx_binding(e, t);
        if (bindings[i].texture == NULL || bindings[i].sampler == NULL) {
            return;
        }
    }
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(g, cmd, rt, true, br_fx_transparent());
    if (pass == NULL) {
        return;
    }
    SDL_Rect scissor = { 0, 0, (int)rt->width, (int)rt->height };
    SDL_SetGPUScissor(pass, &scissor);
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (sampler_count > 0) {
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, sampler_count);
    }
    SDL_PushGPUVertexUniformData(cmd, 0, p->block, sizeof(br_filter_globals));
    SDL_PushGPUFragmentUniformData(cmd, 0, p->block, p->block_size);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/* Copies copy_src of the current target into the output's top-left copy_w x copy_h pixels. */
static void br_fx_run_copy_pass(br_engine *e, const br_render_callback_info *info, const br_fx_pass *p)
{
    br_render_target *src = info->target;
    br_render_target *dst = br_texture_render_target(e, p->output);
    if (src == NULL || dst == NULL || src->color == NULL || dst->color == NULL || p->copy_src.w <= 0 ||
        p->copy_src.h <= 0) {
        return;
    }
    /* clamp to the target (it can be smaller than at collect after a resize) */
    SDL_Rect bounds = { 0, 0, (int)src->width, (int)src->height };
    SDL_Rect clamped;
    if (!SDL_GetRectIntersection(&p->copy_src, &bounds, &clamped)) {
        return;
    }
    float sx = (float)p->copy_w / (float)p->copy_src.w;
    float sy = (float)p->copy_h / (float)p->copy_src.h;
    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture = src->color->handle;
    blit.source.x = (Uint32)clamped.x;
    blit.source.y = (Uint32)clamped.y;
    blit.source.w = (Uint32)clamped.w;
    blit.source.h = (Uint32)clamped.h;
    blit.destination.texture = dst->color->handle;
    blit.destination.x = (Uint32)((float)(clamped.x - p->copy_src.x) * sx);
    blit.destination.y = (Uint32)((float)(clamped.y - p->copy_src.y) * sy);
    blit.destination.w = (Uint32)SDL_max(1.0f, (float)clamped.w * sx);
    blit.destination.h = (Uint32)SDL_max(1.0f, (float)clamped.h * sy);
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.0f };
    bool same_size = blit.source.w == blit.destination.w && blit.source.h == blit.destination.h;
    blit.filter = same_size ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(info->cmd, &blit);
}

static void br_fx_run_job(br_engine *e, const br_render_callback_info *info, void *user)
{
    br_fx_job *job = user;
    br_filters_state *fs = job->fs;
    for (uint32_t i = 0; i < job->count; i++) {
        const br_fx_pass *p = br_array_at(&fs->passes, job->first + i);
        if (p->kind == BR_FX_PASS_COPY) {
            br_fx_run_copy_pass(e, info, p);
        } else {
            br_fx_run_shader_pass(e, info->cmd, p);
        }
    }
}

/* ---- drawing results back */

/* Quad over world rect r showing uv 0..u, 0..v of t, in world space, with a packed colour. */
static void br_fx_quad(br_vertex2d v[4], const br_vec2 corners[4], float u, float vv, uint32_t color)
{
    const float us[4] = { 0.0f, u, u, 0.0f };
    const float vs[4] = { 0.0f, 0.0f, vv, vv };
    for (int i = 0; i < 4; i++) {
        v[i].x = corners[i].x;
        v[i].y = corners[i].y;
        v[i].u = us[i];
        v[i].v = vs[i];
        v[i].color = color;
    }
}

static void br_fx_rect_corners(br_rect r, br_vec2 out[4])
{
    out[0] = br_vec2_make(r.x, r.y);
    out[1] = br_vec2_make(r.x + r.width, r.y);
    out[2] = br_vec2_make(r.x + r.width, r.y + r.height);
    out[3] = br_vec2_make(r.x, r.y + r.height);
}

/* Draws source over the target with an advanced blend shader (backdrop already copied). */
static void br_fx_draw_blended(br_render_ctx *rc, br_texture source, br_texture backdrop, int mode,
                               const br_vertex2d v[4])
{
    static const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    float uniforms[4] = { (float)(mode - BR_BLEND_ADVANCED_FIRST), 0.0f, 0.0f, 0.0f };
    br_custom_draw draw;
    SDL_zero(draw);
    draw.vertex_shader = &br_shader_textured_vert;
    draw.fragment_shader = &br_shader_filter_blend_frag;
    draw.layout.buffer_count = 1;
    draw.layout.strides[0] = sizeof(br_vertex2d);
    draw.layout.attribute_count = 3;
    draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    draw.vertices = v;
    draw.vertex_bytes = 4 * sizeof(br_vertex2d);
    draw.indices = indices;
    draw.index_count = 6;
    draw.textures[0] = source;
    draw.textures[1] = backdrop;
    draw.texture_count = 2;
    draw.fragment_uniforms = uniforms;
    draw.fragment_uniform_size = sizeof(uniforms);
    br_blend_mode old = br_render_set_blend(rc, BR_BLEND_NONE); /* the shader writes the composite */
    br_render_push_custom(rc, &draw);
    br_render_set_blend(rc, old);
}

/* Makes sure the cache texture is at least width x height pixels (bucket sized). */
static bool br_fx_cache_texture(br_engine *e, br_fx_record *rec, uint32_t width, uint32_t height)
{
    if (!br_texture_valid(e, rec->cache)) {
        rec->cache = br_render_texture_create(e, (float)width, (float)height, 1.0f);
        return rec->cache.generation != 0;
    }
    br_texture_info info;
    br_texture_get_info(e, rec->cache, &info);
    if (info.source_width == width && info.source_height == height) {
        return true;
    }
    return br_render_texture_resize(e, rec->cache, (float)width, (float)height);
}

/* cache_as_texture without other effects: render once without the node's colour, then draw
 * with the current transform and colour (fades and tints need no re-render, like Pixi). */
static bool br_fx_draw_cached(br_engine *e, br_node n, br_fx_record *rec, br_render_ctx *rc)
{
    float scale = br_render_pixel_ratio(rc);
    br_mat3 world = br_node_world_transform(e, n);
    if (!rec->cache_valid || rec->cache_scale != scale || !br_texture_valid(e, rec->cache)) {
        br_rect b = br_node_get_bounds(e, n);
        if (br_rect_is_empty(b)) {
            return true;
        }
        /* snap to the pixel grid so the cached copy is not resampled */
        float x0 = SDL_floorf(b.x * scale);
        float y0 = SDL_floorf(b.y * scale);
        float x1 = SDL_ceilf((b.x + b.width) * scale);
        float y1 = SDL_ceilf((b.y + b.height) * scale);
        uint32_t tw = (uint32_t)SDL_min(x1 - x0, (float)BR_FILTER_MAX_SIZE);
        uint32_t th = (uint32_t)SDL_min(y1 - y0, (float)BR_FILTER_MAX_SIZE);
        uint32_t w = br_filters_pool_bucket(tw);
        uint32_t h = br_filters_pool_bucket(th);
        if (!br_fx_cache_texture(e, rec, w, h)) {
            return true;
        }
        b = br_rect_make(x0 / scale, y0 / scale, (float)tw / scale, (float)th / scale);
        br_rect area = br_rect_make(b.x, b.y, (float)w / scale, (float)h / scale);
        br_render_subtree_to_texture_ex(rc, n, rec->cache, area, true, br_fx_transparent(), false);
        rec->cache_area = b;
        rec->cache_world = world;
        rec->cache_scale = scale;
        rec->cache_u = (float)tw / (float)w;
        rec->cache_v = (float)th / (float)h;
        rec->cache_valid = true;
    }
    /* the cache was drawn with cache_world; map it to where the node is now */
    br_mat3 delta = br_mat3_mul(world, br_mat3_invert(rec->cache_world, NULL));
    br_vec2 corners[4];
    br_fx_rect_corners(rec->cache_area, corners);
    for (int i = 0; i < 4; i++) {
        corners[i] = br_mat3_apply(delta, corners[i]);
    }
    br_vertex2d v[4];
    br_fx_quad(v, corners, rec->cache_u, rec->cache_v, br_render_node_color(rc, n));
    br_mat3 old = br_render_set_space(rc, br_mat3_identity());
    br_render_push_quad(rc, rec->cache, v);
    br_render_set_space(rc, old);
    return true;
}

/* ---- the effects hook */

static bool br_fx_hook(br_engine *e, br_node n, br_render_ctx *rc, void *user)
{
    br_filters_state *fs = user;
    br_fx_record *rec = br_fx_record_get(e, n);
    uint32_t active = 0;
    float padding = 0.0f;
    for (uint32_t i = 0; rec != NULL && i < rec->count; i++) {
        if (rec->filters[i].enabled) {
            active += 1;
            padding += SDL_max(rec->filters[i].padding, 0.0f);
        }
    }
    br_mask_kind mask_kind = BR_MASK_NONE;
    bool mask_inverse = false;
    br_node mask = br_node_mask(e, n, &mask_kind, &mask_inverse);
    bool has_mask = (mask_kind == BR_MASK_ALPHA || mask_kind == BR_MASK_COLOR) && br_node_valid(e, mask);
    int blend = br_node_blend_mode(e, n);
    bool advanced = blend >= BR_BLEND_ADVANCED_FIRST && blend < BR_BLEND_ADVANCED_END;
    bool cached = br_node_cache_as_texture(e, n);
    if (active == 0 && !has_mask && !advanced && !cached) {
        return false;
    }
    if (cached) {
        rec = br_fx_record_ensure(e, n);
        if (rec == NULL) {
            return false;
        }
        if (active == 0 && !has_mask && !advanced) {
            return br_fx_draw_cached(e, n, rec, rc);
        }
    }

    /* filter area in world units, snapped to the target's pixels */
    float ratio = br_render_pixel_ratio(rc);
    br_rect target = br_render_area(rc);
    br_rect b;
    if (rec != NULL && rec->use_area) {
        b = br_rect_transform_bounds(rec->area, br_node_world_transform(e, n));
    } else {
        b = br_node_get_bounds(e, n);
    }
    if (br_rect_is_empty(b)) {
        return true;
    }
    b = br_rect_pad(b, padding);
    if (!cached) {
        b = br_rect_intersection(b, target); /* off-target parts cost pixels and show nothing */
        if (br_rect_is_empty(b)) {
            return true;
        }
    }
    float x0 = SDL_floorf((b.x - target.x) * ratio);
    float y0 = SDL_floorf((b.y - target.y) * ratio);
    float x1 = SDL_ceilf((b.x + b.width - target.x) * ratio);
    float y1 = SDL_ceilf((b.y + b.height - target.y) * ratio);
    b = br_rect_make(target.x + x0 / ratio, target.y + y0 / ratio, (x1 - x0) / ratio, (y1 - y0) / ratio);
    float scale = ratio * (rec != NULL ? rec->resolution : 1.0f);
    float max_scale = (float)BR_FILTER_MAX_SIZE / SDL_max(b.width, b.height);
    // ponytail: huge filter areas are rendered at a lower resolution (BR_FILTER_MAX_SIZE); tile them if that shows.
    scale = SDL_min(scale, max_scale);
    uint32_t tw = (uint32_t)SDL_max(1.0f, SDL_ceilf(b.width * scale));
    uint32_t th = (uint32_t)SDL_max(1.0f, SDL_ceilf(b.height * scale));
    uint32_t w = br_filters_pool_bucket(tw);
    uint32_t h = br_filters_pool_bucket(th);
    br_rect area = br_rect_make(b.x, b.y, (float)w / scale, (float)h / scale);

    /* 1. content: the subtree (or its cache) */
    br_texture content;
    if (cached) {
        // ponytail: a cached node that also has filters/masks/advanced blend caches only its content (with its colour baked in) and re-renders it when its bounds change; caching the filtered result and applying the colour at draw time is the upgrade.
        bool same = rec->cache_valid && rec->cache_area.x == b.x && rec->cache_area.y == b.y &&
                    rec->cache_area.width == b.width && rec->cache_area.height == b.height &&
                    rec->cache_scale == scale;
        if (!same || !br_texture_valid(e, rec->cache)) {
            if (!br_fx_cache_texture(e, rec, w, h)) {
                return true;
            }
            br_render_subtree_to_texture(rc, n, rec->cache, area, true, br_fx_transparent());
            rec->cache_area = b;
            rec->cache_world = br_node_world_transform(e, n);
            rec->cache_scale = scale;
            rec->cache_u = (float)tw / (float)w;
            rec->cache_v = (float)th / (float)h;
            rec->cache_valid = true;
        }
        content = rec->cache;
    } else {
        content = br_filters_pool_acquire(e, w, h);
        if (content.generation == 0) {
            return true;
        }
        br_render_subtree_to_texture(rc, n, content, area, true, br_fx_transparent());
    }

    /* 2. the mask over the same area */
    br_texture mask_texture = br_fx_no_texture();
    if (has_mask) {
        mask_texture = br_filters_pool_acquire(e, w, h);
        if (mask_texture.generation != 0) {
            br_render_subtree_to_texture(rc, mask, mask_texture, area, true, br_fx_transparent());
        }
    }

    /* 3. passes: filters, then the mask, then the blend backdrop copy */
    br_fx_job *job = br_fx_job_begin(fs);
    if (job == NULL) {
        return true;
    }
    br_fx_chain chain;
    SDL_zero(chain);
    chain.e = e;
    chain.job = job;
    chain.current = content;
    chain.width = w;
    chain.height = h;
    chain.scale = scale;
    chain.globals.input_size[0] = (float)w;
    chain.globals.input_size[1] = (float)h;
    chain.globals.input_size[2] = 1.0f / (float)w;
    chain.globals.input_size[3] = 1.0f / (float)h;
    chain.globals.content[2] = (float)tw / (float)w;
    chain.globals.content[3] = (float)th / (float)h;
    chain.globals.area[0] = b.x;
    chain.globals.area[1] = b.y;
    chain.globals.area[2] = b.width;
    chain.globals.area[3] = b.height;
    chain.globals.params[0] = (float)br_ticker_elapsed(e);
    chain.globals.params[1] = scale;
    for (uint32_t i = 0; rec != NULL && i < rec->count; i++) {
        if (rec->filters[i].enabled) {
            br_fx_add_filter(&chain, &rec->filters[i]);
        }
    }
    if (mask_texture.generation != 0) {
        float u[4] = { mask_kind == BR_MASK_COLOR ? 1.0f : 0.0f, mask_inverse ? 1.0f : 0.0f, 0.0f, 0.0f };
        br_fx_pass *p = br_fx_add_pass(&chain, &br_shader_filter_mask_frag, u, sizeof(u));
        if (p != NULL) {
            p->extra[0] = mask_texture;
            p->extra_count = 1;
        }
    }
    br_texture backdrop = br_fx_no_texture();
    if (advanced) {
        backdrop = br_filters_pool_acquire(e, w, h);
        br_fx_pass *p = backdrop.generation != 0 ? br_array_push(&fs->passes, NULL) : NULL;
        if (p != NULL) {
            p->kind = BR_FX_PASS_COPY;
            p->output = backdrop;
            /* target pixels under b, viewport offset and view zoom included */
            // ponytail: a rotated view copies the bounding box, which does not line up with the rotated quad; sample the backdrop in screen space if rotated cameras need advanced blends.
            br_rect px = br_render_rect_to_pixels(rc, b);
            p->copy_src.x = (int)px.x;
            p->copy_src.y = (int)px.y;
            p->copy_src.w = (int)px.width;
            p->copy_src.h = (int)px.height;
            p->copy_w = tw;
            p->copy_h = th;
            job->count += 1;
        }
    }
    if (job->count > 0) {
        br_render_push_callback(rc, br_fx_run_job, job, true);
    }

    /* 4. put the result back */
    br_vec2 corners[4];
    br_fx_rect_corners(b, corners);
    br_vertex2d v[4];
    br_fx_quad(v, corners, (float)tw / (float)w, (float)th / (float)h, 0xFFFFFFFFu);
    br_mat3 old = br_render_set_space(rc, br_mat3_identity()); /* b is in world coordinates */
    if (advanced && backdrop.generation != 0) {
        br_fx_draw_blended(rc, chain.current, backdrop, blend, v);
    } else {
        br_render_push_quad(rc, chain.current, v);
    }
    br_render_set_space(rc, old);
    return true;
}

/* ---- systems and lifetime */

/* PRE_RENDER, before render.collect: free last frame's textures and passes, trim the pool. */
static void br_fx_frame_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_filters_state *fs = user;
    fs->frame += 1;
    br_filters_pool_release_all(e);
    br_array_clear(&fs->passes);
    fs->jobs_used = 0;
    int pw, ph;
    br_engine_pixel_size(e, &pw, &ph);
    if (pw != fs->pixel_w || ph != fs->pixel_h) {
        /* sizes are buckets of the old window: start over (Review Focus 5) */
        br_filters_pool_trim(e);
        fs->pixel_w = pw;
        fs->pixel_h = ph;
    } else {
        br_fx_pool_trim_idle(e, BR_POOL_IDLE_FRAMES);
    }
    /* caches whose node turned cache_as_texture off */
    for (uint32_t i = 0; i < fs->records.count; i++) {
        br_fx_record *rec = *(br_fx_record **)br_array_at(&fs->records, i);
        if (rec == NULL || rec->cache.generation == 0) {
            continue;
        }
        if (!br_node_cache_as_texture(e, rec->node)) {
            br_texture_destroy(e, rec->cache);
            rec->cache = br_fx_no_texture();
            rec->cache_valid = false;
        }
    }
}

static void br_fx_shutdown(br_engine *e, void *user)
{
    br_filters_state *fs = user;
    for (uint32_t i = 0; i < fs->records.count; i++) {
        br_fx_record_free(e, i);
    }
    br_filters_pool_trim(e);
    for (uint32_t i = 0; i < fs->jobs.count; i++) {
        br_free(*(br_fx_job **)br_array_at(&fs->jobs, i));
    }
    br_array_free(&fs->pool);
    br_array_free(&fs->records);
    br_array_free(&fs->passes);
    br_array_free(&fs->jobs);
    br_array_free(&fs->generate_jobs);
    br_free(fs);
    e->filters = NULL;
}

void br_filters_register(br_engine *e)
{
    br_filters_state *fs = br_alloc(sizeof(br_filters_state));
    if (fs == NULL) {
        return;
    }
    SDL_zerop(fs);
    e->filters = fs;
    br_array_init(&fs->pool, sizeof(br_pool_entry));
    br_array_init(&fs->records, sizeof(br_fx_record *));
    br_array_init(&fs->passes, sizeof(br_fx_pass));
    br_array_init(&fs->jobs, sizeof(br_fx_job *));
    br_array_init(&fs->generate_jobs, sizeof(br_generate_job));
    br_engine_on_shutdown(e, br_fx_shutdown, fs);
    br_scene_on_node_destroy(e, br_fx_on_node_destroy, NULL);
    br_render_set_effects_hook(e, br_fx_hook, fs);
    br_system_add(e, "filters.frame", BR_PHASE_PRE_RENDER, 900, br_fx_frame_system, fs);
    br_extract_init(e);
}
