/* br_particles: the particle container node type. One instanced draw per container: the
 * vertex shader (particles.vert.hlsl) expands each instance into a quad.
 *
 * Instance data is split into two buffers by br_particle_property:
 *   dynamic groups -> this frame's transient vertex buffer, packed every frame during collect;
 *   static groups  -> a buffer owned by the container, uploaded only when flagged dirty.
 * The draw itself is an in-pass render callback with its own pipeline. */
#include "app/br_engine_internal.h"
#include "braty/br_particles.h"

#include "particles.vert.h"
#include "textured.frag.h"

#define BR_PARTICLE_GROUP_COUNT 5

/* Bytes per particle of each group, in shader attribute order. */
static const uint32_t br_particle_group_flag[BR_PARTICLE_GROUP_COUNT] = {
    BR_PARTICLE_VERTEX, BR_PARTICLE_POSITION, BR_PARTICLE_ROTATION, BR_PARTICLE_UVS, BR_PARTICLE_COLOR
};
static const uint32_t br_particle_group_size[BR_PARTICLE_GROUP_COUNT] = { 16, 8, 4, 24, 4 };

/* Heap-allocated so its address is stable: it is the callback's user pointer. */
typedef struct br_particle_store {
    br_array       particles;       /* br_particle */
    br_texture     texture;
    uint32_t       dynamic;         /* br_particle_property mask */
    bool           static_dirty;
    SDL_GPUBuffer *static_buffer;
    uint32_t       static_capacity; /* bytes */
    /* filled by collect, read by the draw callback of the same frame */
    uint32_t       draw_count;
    uint32_t       dynamic_offset;  /* bytes into the transient vertex buffer */
    br_mat4        matrix;          /* container local -> current space */
    float          color[4];        /* container tint * alpha, premultiplied */
} br_particle_store;

typedef struct br_particle_node {
    br_particle_store *store;
} br_particle_node;

typedef struct br_particles_state {
    uint32_t type;
    br_array scratch;               /* uint8_t: packed static data before upload */
} br_particles_state;

/* Vertex uniforms of particles.vert.hlsl. */
typedef struct br_particle_uniforms {
    br_mat4 transform;
    float   color[4];
} br_particle_uniforms;

/* ---- packing */

/* Bytes per particle of the groups in `mask`. */
static uint32_t br_particle_stride(uint32_t mask)
{
    uint32_t stride = 0;
    for (int g = 0; g < BR_PARTICLE_GROUP_COUNT; g++) {
        if (mask & br_particle_group_flag[g]) {
            stride += br_particle_group_size[g];
        }
    }
    return stride;
}

/* Writes the groups in `mask` of every particle into dst (stride = br_particle_stride(mask)). */
static void br_particle_pack(br_engine *e, const br_particle_store *store, uint32_t mask, uint8_t *dst)
{
    const br_particle *p = store->particles.data;
    uint32_t count = store->particles.count;
    br_texture cached = { 0, 0 };
    br_texture_info info;
    br_texture_get_info(e, store->texture, &info);
    bool needs_info = (mask & (BR_PARTICLE_VERTEX | BR_PARTICLE_UVS)) != 0;
    for (uint32_t i = 0; i < count; i++) {
        br_texture t = p[i].texture.generation != 0 ? p[i].texture : store->texture;
        if (needs_info && (t.index != cached.index || t.generation != cached.generation)) {
            br_texture_get_info(e, t, &info); /* particles usually share a few frames: cheap */
            cached = t;
        }
        float f[6];
        if (mask & BR_PARTICLE_VERTEX) {
            float res = info.resolution;
            float w = info.frame.orig.width / res;
            float h = info.frame.orig.height / res;
            float x0 = -p[i].anchor_x * w;
            float y0 = -p[i].anchor_y * h;
            float x1 = x0 + w;
            float y1 = y0 + h;
            if (info.frame.trim.width > 0.0f && info.frame.trim.height > 0.0f) {
                x0 += info.frame.trim.x / res;
                y0 += info.frame.trim.y / res;
                x1 = x0 + info.frame.trim.width / res;
                y1 = y0 + info.frame.trim.height / res;
            }
            f[0] = x0 * p[i].scale_x;
            f[1] = y0 * p[i].scale_y;
            f[2] = x1 * p[i].scale_x;
            f[3] = y1 * p[i].scale_y;
            SDL_memcpy(dst, f, 16);
            dst += 16;
        }
        if (mask & BR_PARTICLE_POSITION) {
            f[0] = p[i].x;
            f[1] = p[i].y;
            SDL_memcpy(dst, f, 8);
            dst += 8;
        }
        if (mask & BR_PARTICLE_ROTATION) {
            SDL_memcpy(dst, &p[i].rotation, 4);
            dst += 4;
        }
        if (mask & BR_PARTICLE_UVS) {
            f[0] = info.uvs[0].x;
            f[1] = info.uvs[0].y;
            f[2] = info.uvs[1].x - info.uvs[0].x;
            f[3] = info.uvs[1].y - info.uvs[0].y;
            f[4] = info.uvs[3].x - info.uvs[0].x;
            f[5] = info.uvs[3].y - info.uvs[0].y;
            SDL_memcpy(dst, f, 24);
            dst += 24;
        }
        if (mask & BR_PARTICLE_COLOR) {
            float a = br_clampf(p[i].alpha, 0.0f, 1.0f);
            dst[0] = (uint8_t)((float)((p[i].tint >> 16) & 0xFFu) * a + 0.5f);
            dst[1] = (uint8_t)((float)((p[i].tint >> 8) & 0xFFu) * a + 0.5f);
            dst[2] = (uint8_t)((float)(p[i].tint & 0xFFu) * a + 0.5f);
            dst[3] = (uint8_t)(a * 255.0f + 0.5f);
            dst += 4;
        }
    }
}

/* ---- draw (RENDER phase, inside the open pass) */

static void br_particle_draw(br_engine *e, const br_render_callback_info *info, void *user)
{
    br_particle_store *store = user;
    br_gpu *g = br_engine_gpu(e);
    uint32_t static_mask = ~store->dynamic & 0x1Fu;
    uint32_t dynamic_stride = br_particle_stride(store->dynamic);
    uint32_t static_stride = br_particle_stride(static_mask);
    if (store->draw_count == 0 || (static_stride > 0 && store->static_buffer == NULL)) {
        return;
    }
    /* layout: dynamic groups in buffer 0 (if any), static groups in the next buffer */
    uint32_t dynamic_slot = 0;
    uint32_t static_slot = dynamic_stride > 0 ? 1 : 0;
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_particles_vert;
    desc.fragment_shader = &br_shader_textured_frag;
    desc.layout.buffer_count = (dynamic_stride > 0 ? 1u : 0u) + (static_stride > 0 ? 1u : 0u);
    desc.layout.strides[dynamic_slot] = dynamic_stride > 0 ? dynamic_stride : static_stride;
    desc.layout.strides[static_slot] = static_stride > 0 ? static_stride : dynamic_stride;
    desc.layout.per_instance[0] = true;
    desc.layout.per_instance[1] = true;
    uint32_t dynamic_offset = 0;
    uint32_t static_offset = 0;
    uint32_t location = 0;
    for (int grp = 0; grp < BR_PARTICLE_GROUP_COUNT; grp++) {
        bool dyn = (store->dynamic & br_particle_group_flag[grp]) != 0;
        uint32_t slot = dyn ? dynamic_slot : static_slot;
        uint32_t *offset = dyn ? &dynamic_offset : &static_offset;
        SDL_GPUVertexAttribute *a = &desc.layout.attributes[desc.layout.attribute_count];
        a->buffer_slot = slot;
        a->offset = *offset;
        a->location = location++;
        if (grp == 0) {
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        } else if (grp == 1) {
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        } else if (grp == 2) {
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        } else if (grp == 3) {
            /* uv origin + the two uv steps: two attributes */
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            desc.layout.attribute_count += 1;
            a = &desc.layout.attributes[desc.layout.attribute_count];
            a->buffer_slot = slot;
            a->offset = *offset + 8;
            a->location = location++;
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        } else {
            a->format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        }
        desc.layout.attribute_count += 1;
        *offset += br_particle_group_size[grp];
    }
    br_render_pipeline_setup(info, &desc);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    br_gpu_texture *tex = br_texture_gpu(e, store->texture);
    br_texture_info tinfo;
    br_texture_get_info(e, store->texture, &tinfo);
    if (tex == NULL) {
        tex = br_texture_gpu(e, br_texture_missing(e));
    }
    SDL_GPUTextureSamplerBinding binding;
    binding.texture = tex != NULL ? tex->handle : NULL;
    binding.sampler = br_gpu_sampler_get(g, &tinfo.style);
    if (pipeline == NULL || binding.texture == NULL || binding.sampler == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(info->pass, pipeline);
    /* ponytail: the callback info carries no scissor, so scissor masks do not clip particle
     * containers (stencil masks do); pass the scissor in br_render_callback_info to fix. */
    SDL_Rect full = { 0, 0, (int)info->target->width, (int)info->target->height };
    SDL_SetGPUScissor(info->pass, &full);
    if (desc.stencil_test) {
        SDL_SetGPUStencilReference(info->pass, info->stencil_ref);
    }
    br_particle_uniforms u;
    u.transform = br_mat4_mul(info->transform, store->matrix);
    SDL_memcpy(u.color, store->color, sizeof(u.color));
    SDL_PushGPUVertexUniformData(info->cmd, 0, &u, sizeof(u));
    SDL_GPUBufferBinding vbs[2];
    uint32_t vb_count = 0;
    if (dynamic_stride > 0) {
        vbs[vb_count].buffer = br_gpu_transient_vertex_buffer(g);
        vbs[vb_count].offset = store->dynamic_offset;
        vb_count++;
    }
    if (static_stride > 0) {
        vbs[vb_count].buffer = store->static_buffer;
        vbs[vb_count].offset = 0;
        vb_count++;
    }
    SDL_BindGPUVertexBuffers(info->pass, 0, vbs, vb_count);
    SDL_BindGPUFragmentSamplers(info->pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(info->pass, 6, store->draw_count, 0, 0);
}

/* ---- collect (PRE_RENDER) */

/* Uploads the static groups into the container's own buffer (only when dirty). */
static bool br_particle_upload_static(br_engine *e, br_particle_store *store, uint32_t static_mask)
{
    br_gpu *g = br_engine_gpu(e);
    uint32_t bytes = br_particle_stride(static_mask) * store->particles.count;
    br_array *scratch = &e->particles->scratch;
    if (!br_array_resize(scratch, bytes)) {
        return false;
    }
    br_particle_pack(e, store, static_mask, scratch->data);
    if (bytes > store->static_capacity) {
        br_gpu_buffer_destroy(g, store->static_buffer); /* SDL defers the release until the GPU is done */
        store->static_capacity = 0;
        store->static_buffer = br_gpu_buffer_create(g, SDL_GPU_BUFFERUSAGE_VERTEX, bytes, NULL);
        if (store->static_buffer == NULL) {
            return false;
        }
        store->static_capacity = bytes;
    }
    /* own command buffer, submitted now: ordered before this frame's draw */
    bool ok = br_gpu_buffer_update(g, store->static_buffer, 0, scratch->data, bytes);
    /* ponytail: scratch keeps its peak size (56 MB at 1M particles with everything static) */
    return ok;
}

static void br_particle_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_particle_node *pn = data;
    br_particle_store *store = pn->store;
    br_gpu *g = br_engine_gpu(e);
    uint32_t color = br_render_node_color(rc, n);
    uint32_t count = store->particles.count;
    store->draw_count = 0;
    if (g == NULL || count == 0 || color == 0) {
        return;
    }
    uint32_t static_mask = ~store->dynamic & 0x1Fu;
    if (store->static_dirty && br_particle_stride(static_mask) > 0) {
        if (!br_particle_upload_static(e, store, static_mask)) {
            return;
        }
    }
    store->static_dirty = false;
    uint32_t dynamic_stride = br_particle_stride(store->dynamic);
    if (dynamic_stride > 0) {
        uint8_t *dst = br_gpu_transient_alloc_vertices(g, dynamic_stride * count, &store->dynamic_offset);
        if (dst == NULL) {
            return;
        }
        br_particle_pack(e, store, store->dynamic, dst);
    }
    /* ponytail: one set of per-frame fields per container, so a container drawn twice in a
     * frame (render-to-texture + stage) uses the last matrix for both; per-push records fix it. */
    store->matrix = br_mat4_from_mat3(br_render_node_transform(rc, n));
    uint8_t bytes[4];
    SDL_memcpy(bytes, &color, 4);
    for (int i = 0; i < 4; i++) {
        store->color[i] = (float)bytes[i] / 255.0f;
    }
    store->draw_count = count;
    br_render_push_callback(rc, br_particle_draw, store, false);
}

/* AABB of the particle positions (frame sizes ignored: bounds are for culling and layout). */
static bool br_particle_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_particle_store *store = ((const br_particle_node *)data)->store;
    const br_particle *p = store->particles.data;
    if (store->particles.count == 0) {
        return false;
    }
    float x0 = p[0].x;
    float y0 = p[0].y;
    float x1 = x0;
    float y1 = y0;
    for (uint32_t i = 1; i < store->particles.count; i++) {
        x0 = SDL_min(x0, p[i].x);
        y0 = SDL_min(y0, p[i].y);
        x1 = SDL_max(x1, p[i].x);
        y1 = SDL_max(y1, p[i].y);
    }
    *out = br_rect_make(x0, y0, x1 - x0, y1 - y0);
    return true;
}

static void br_particle_on_create(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_particle_node *pn = data;
    pn->store = br_alloc(sizeof(br_particle_store));
    if (pn->store != NULL) {
        br_array_init(&pn->store->particles, sizeof(br_particle));
        pn->store->dynamic = BR_PARTICLE_POSITION;
    }
}

static void br_particle_on_destroy(br_engine *e, br_node n, void *data)
{
    (void)n;
    br_particle_node *pn = data;
    if (pn->store != NULL) {
        br_gpu *g = br_engine_gpu(e);
        if (g != NULL) {
            br_gpu_buffer_destroy(g, pn->store->static_buffer);
        }
        br_array_free(&pn->store->particles);
        br_free(pn->store);
        pn->store = NULL;
    }
}

/* ---- API */

static br_particle_store *br_particle_store_get(br_engine *e, br_node n)
{
    br_particle_node *pn = br_node_data(e, n, e->particles->type);
    return pn != NULL ? pn->store : NULL;
}

uint32_t br_particle_container_type(br_engine *e)
{
    return e->particles->type;
}

br_node br_particle_container_create(br_engine *e, br_texture texture, uint32_t dynamic_properties)
{
    br_node n = br_node_create(e, e->particles->type);
    br_particle_store *store = br_particle_store_get(e, n);
    if (store == NULL) {
        br_node_destroy(e, n);
        br_node none = { 0, 0 };
        return none;
    }
    store->texture = texture;
    store->dynamic = dynamic_properties & 0x1Fu;
    store->static_dirty = true;
    return n;
}

br_particle *br_particle_container_add(br_engine *e, br_node n, uint32_t count)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store == NULL || count == 0) {
        return NULL;
    }
    uint32_t first = store->particles.count;
    br_particle *p = br_array_push_n(&store->particles, NULL, count);
    if (p == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        p[i].scale_x = 1.0f;
        p[i].scale_y = 1.0f;
        p[i].tint = 0xFFFFFFu;
        p[i].alpha = 1.0f;
    }
    store->static_dirty = true;
    return br_array_at(&store->particles, first);
}

void br_particle_container_remove(br_engine *e, br_node n, uint32_t first, uint32_t count)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store == NULL || first >= store->particles.count) {
        return;
    }
    uint32_t total = store->particles.count;
    if (count > total - first) {
        count = total - first;
    }
    br_particle *p = store->particles.data;
    SDL_memmove(&p[first], &p[first + count], sizeof(br_particle) * (total - first - count));
    br_array_resize(&store->particles, total - count);
    store->static_dirty = true;
}

void br_particle_container_clear(br_engine *e, br_node n)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store != NULL) {
        br_array_clear(&store->particles);
        store->static_dirty = true;
    }
}

br_particle *br_particle_container_particles(br_engine *e, br_node n, uint32_t *count)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (count != NULL) {
        *count = store != NULL ? store->particles.count : 0;
    }
    return store != NULL ? store->particles.data : NULL;
}

void br_particle_container_update(br_engine *e, br_node n)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store != NULL) {
        store->static_dirty = true;
    }
}

void br_particle_container_set_dynamic(br_engine *e, br_node n, uint32_t dynamic_properties)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store != NULL) {
        store->dynamic = dynamic_properties & 0x1Fu;
        store->static_dirty = true;
    }
}

void br_particle_container_set_texture(br_engine *e, br_node n, br_texture texture)
{
    br_particle_store *store = br_particle_store_get(e, n);
    if (store != NULL) {
        store->texture = texture;
        store->static_dirty = true;
    }
}

/* ---- module */

static void br_particles_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_array_free(&e->particles->scratch);
    br_free(e->particles);
    e->particles = NULL;
}

void br_particles_register(br_engine *e)
{
    e->particles = br_alloc(sizeof(br_particles_state));
    if (e->particles == NULL) {
        return;
    }
    br_array_init(&e->particles->scratch, 1);
    br_engine_on_shutdown(e, br_particles_shutdown, NULL);
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "particle_container";
    desc.data_size = sizeof(br_particle_node);
    desc.on_create = br_particle_on_create;
    desc.on_destroy = br_particle_on_destroy;
    desc.local_bounds = br_particle_bounds;
    desc.render = br_particle_render;
    e->particles->type = br_node_type_register(e, &desc);
}
