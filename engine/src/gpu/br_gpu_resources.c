/* Buffers, transient streaming, textures, samplers, render targets, readback. */
#include "gpu/br_gpu_internal.h"

/* ---- one-off uploads: own command buffer, submitted at once.
 * SDL executes command buffers in submission order, so the data is in place before any
 * frame submitted later uses it. */

static SDL_GPUTransferBuffer *br_gpu_staging_create(br_gpu *g, SDL_GPUTransferBufferUsage usage, uint32_t size,
                                                    const void *data)
{
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = usage;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(g->device, &info);
    if (transfer == NULL) {
        br_log_error("cannot create transfer buffer (%u bytes): %s", size, SDL_GetError());
        return NULL;
    }
    if (data != NULL) {
        void *mapped = SDL_MapGPUTransferBuffer(g->device, transfer, false);
        if (mapped == NULL) {
            br_log_error("cannot map transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUTransferBuffer(g->device, transfer);
            return NULL;
        }
        SDL_memcpy(mapped, data, size);
        SDL_UnmapGPUTransferBuffer(g->device, transfer);
    }
    return transfer;
}

/* ---- buffers */

SDL_GPUBuffer *br_gpu_buffer_create(br_gpu *g, SDL_GPUBufferUsageFlags usage, uint32_t size, const void *data)
{
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = usage;
    info.size = size;
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g->device, &info);
    if (buffer == NULL) {
        br_log_error("cannot create GPU buffer (%u bytes): %s", size, SDL_GetError());
        return NULL;
    }
    if (data != NULL && !br_gpu_buffer_update(g, buffer, 0, data, size)) {
        SDL_ReleaseGPUBuffer(g->device, buffer);
        return NULL;
    }
    return buffer;
}

bool br_gpu_buffer_update(br_gpu *g, SDL_GPUBuffer *buffer, uint32_t offset, const void *data, uint32_t size)
{
    SDL_GPUTransferBuffer *transfer = br_gpu_staging_create(g, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, data);
    if (transfer == NULL) {
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (cmd == NULL) {
        br_log_error("cannot acquire command buffer: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(g->device, transfer);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { transfer, 0 };
    SDL_GPUBufferRegion dst = { buffer, offset, size };
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    g->stats.uploads += 1;
    g->stats.upload_bytes += size;
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g->device, transfer); /* SDL defers the release until the GPU is done */
    return ok;
}

void br_gpu_buffer_destroy(br_gpu *g, SDL_GPUBuffer *buffer)
{
    if (buffer != NULL) {
        SDL_ReleaseGPUBuffer(g->device, buffer);
    }
}

/* ---- transient streaming */

static void *br_transient_alloc(br_gpu *g, br_transient_stream *s, uint32_t size, uint32_t *offset)
{
    if (g->transient_uploaded) {
        /* the bytes would never reach the GPU: the draw would read stale data */
        if (!g->transient_late_logged) {
            br_log_error("br_gpu_transient_alloc_*: called after this frame's upload (allocate in PRE_RENDER)");
            g->transient_late_logged = true;
        }
        return NULL;
    }
    uint32_t start = (s->cpu.count + 15u) & ~15u;
    if (!br_array_resize(&s->cpu, start + size)) {
        return NULL;
    }
    *offset = start;
    return (uint8_t *)s->cpu.data + start;
}

void *br_gpu_transient_alloc_vertices(br_gpu *g, uint32_t size, uint32_t *offset)
{
    return br_transient_alloc(g, &g->vertices, size, offset);
}

void *br_gpu_transient_alloc_indices(br_gpu *g, uint32_t size, uint32_t *offset)
{
    return br_transient_alloc(g, &g->indices, size, offset);
}

/* Grows the GPU and transfer buffers to hold `needed` bytes. */
static bool br_transient_reserve(br_gpu *g, br_transient_stream *s, uint32_t needed)
{
    if (needed <= s->capacity) {
        return true;
    }
    /* uint64: doubling past 2^31 must not wrap (it would loop forever) */
    uint64_t grown = s->capacity > 0 ? s->capacity : 64 * 1024;
    while (grown < needed) {
        grown *= 2;
    }
    uint32_t capacity = grown > UINT32_MAX ? UINT32_MAX : (uint32_t)grown;
    if (s->buffer != NULL) {
        SDL_ReleaseGPUBuffer(g->device, s->buffer);
    }
    if (s->transfer != NULL) {
        SDL_ReleaseGPUTransferBuffer(g->device, s->transfer);
    }
    s->capacity = 0;
    s->transfer = br_gpu_staging_create(g, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, capacity, NULL);
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = s->usage;
    info.size = capacity;
    s->buffer = SDL_CreateGPUBuffer(g->device, &info);
    if (s->transfer == NULL || s->buffer == NULL) {
        br_log_error("cannot grow transient buffer to %u bytes: %s", capacity, SDL_GetError());
        return false;
    }
    s->capacity = capacity;
    return true;
}

static bool br_transient_upload_stream(br_gpu *g, br_transient_stream *s, SDL_GPUCopyPass *copy)
{
    if (s->cpu.count == 0) {
        return true;
    }
    if (!br_transient_reserve(g, s, s->cpu.count)) {
        return false;
    }
    /* cycle = true: the GPU may still read last frame's data, so SDL hands us fresh memory */
    void *mapped = SDL_MapGPUTransferBuffer(g->device, s->transfer, true);
    if (mapped == NULL) {
        br_log_error("cannot map transient buffer: %s", SDL_GetError());
        return false;
    }
    SDL_memcpy(mapped, s->cpu.data, s->cpu.count);
    SDL_UnmapGPUTransferBuffer(g->device, s->transfer);
    SDL_GPUTransferBufferLocation src = { s->transfer, 0 };
    SDL_GPUBufferRegion dst = { s->buffer, 0, s->cpu.count };
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    g->stats.uploads += 1;
    g->stats.upload_bytes += s->cpu.count;
    return true;
}

bool br_gpu_transient_upload(br_gpu *g, SDL_GPUCommandBuffer *cmd)
{
    /* several RENDER systems may call this (the 2D renderer does too): only the first uploads */
    if (g->transient_uploaded) {
        return true;
    }
    g->transient_uploaded = true;
    if (g->vertices.cpu.count == 0 && g->indices.cpu.count == 0) {
        return true;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    bool ok = br_transient_upload_stream(g, &g->vertices, copy);
    ok = br_transient_upload_stream(g, &g->indices, copy) && ok;
    SDL_EndGPUCopyPass(copy);
    return ok;
}

SDL_GPUBuffer *br_gpu_transient_vertex_buffer(br_gpu *g)
{
    return g->vertices.buffer;
}

SDL_GPUBuffer *br_gpu_transient_index_buffer(br_gpu *g)
{
    return g->indices.buffer;
}

void br_gpu_transient_reset(br_gpu *g)
{
    g->transient_uploaded = false;
    g->transient_late_logged = false;
    br_array_clear(&g->vertices.cpu);
    br_array_clear(&g->indices.cpu);
}

static void br_transient_release(br_gpu *g, br_transient_stream *s)
{
    if (s->buffer != NULL) {
        SDL_ReleaseGPUBuffer(g->device, s->buffer);
    }
    if (s->transfer != NULL) {
        SDL_ReleaseGPUTransferBuffer(g->device, s->transfer);
    }
    br_array_free(&s->cpu);
    s->buffer = NULL;
    s->transfer = NULL;
    s->capacity = 0;
}

void br_gpu_release_transient(br_gpu *g)
{
    br_transient_release(g, &g->vertices);
    br_transient_release(g, &g->indices);
}

/* ---- textures */

static uint32_t br_mip_count(uint32_t width, uint32_t height)
{
    uint32_t size = width > height ? width : height;
    uint32_t levels = 1;
    while (size > 1) {
        size /= 2;
        levels++;
    }
    return levels;
}

static br_gpu_texture *br_gpu_texture_create_ex(br_gpu *g, uint32_t width, uint32_t height,
                                                SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                                uint32_t mip_levels, SDL_GPUSampleCount samples)
{
    if (width == 0 || height == 0) {
        br_log_error("cannot create a %ux%u texture", width, height);
        return NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = mip_levels;
    info.sample_count = samples;
    SDL_GPUTexture *handle = SDL_CreateGPUTexture(g->device, &info);
    if (handle == NULL) {
        br_log_error("cannot create %ux%u texture: %s", width, height, SDL_GetError());
        return NULL;
    }
    br_gpu_texture *t = br_alloc(sizeof(br_gpu_texture));
    if (t == NULL) {
        SDL_ReleaseGPUTexture(g->device, handle);
        return NULL;
    }
    t->handle = handle;
    t->width = width;
    t->height = height;
    t->mip_levels = mip_levels;
    t->format = format;
    t->usage = usage;
    t->sample_count = samples;
    return t;
}

br_gpu_texture *br_gpu_texture_create(br_gpu *g, uint32_t width, uint32_t height, SDL_GPUTextureFormat format,
                                      SDL_GPUTextureUsageFlags usage, bool mipmaps)
{
    uint32_t levels = 1;
    if (mipmaps) {
        levels = br_mip_count(width, height);
        usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; /* SDL_GenerateMipmapsForGPUTexture renders into each level */
    }
    return br_gpu_texture_create_ex(g, width, height, format, usage, levels, SDL_GPU_SAMPLECOUNT_1);
}

br_gpu_texture *br_gpu_texture_create_rgba8(br_gpu *g, uint32_t width, uint32_t height, const void *pixels,
                                            bool mipmaps)
{
    br_gpu_texture *t = br_gpu_texture_create(g, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                              SDL_GPU_TEXTUREUSAGE_SAMPLER, mipmaps);
    if (t != NULL && pixels != NULL && !br_gpu_texture_upload(g, t, 0, 0, width, height, pixels)) {
        br_gpu_texture_destroy(g, t);
        return NULL;
    }
    return t;
}

bool br_gpu_texture_upload(br_gpu *g, br_gpu_texture *t, uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, const void *texels)
{
    /* SDL only validates regions in debug mode; out of range is undefined behaviour on the GPU */
    if (width == 0 || height == 0 || x >= t->width || width > t->width - x || y >= t->height ||
        height > t->height - y) {
        br_log_error("texture upload: region %u,%u %ux%u is outside the %ux%u texture", x, y, width, height,
                     t->width, t->height);
        return false;
    }
    uint32_t size = SDL_CalculateGPUTextureFormatSize(t->format, width, height, 1);
    SDL_GPUTransferBuffer *transfer = br_gpu_staging_create(g, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, texels);
    if (transfer == NULL) {
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (cmd == NULL) {
        br_log_error("cannot acquire command buffer: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(g->device, transfer);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src;
    SDL_zero(src);
    src.transfer_buffer = transfer;
    src.pixels_per_row = width;
    src.rows_per_layer = height;
    SDL_GPUTextureRegion dst;
    SDL_zero(dst);
    dst.texture = t->handle;
    dst.x = x;
    dst.y = y;
    dst.w = width;
    dst.h = height;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    g->stats.uploads += 1;
    g->stats.upload_bytes += size;
    if (t->mip_levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t->handle);
    }
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    return ok;
}

bool br_gpu_texture_read(br_gpu *g, br_gpu_texture *t, uint8_t **rgba)
{
    *rgba = NULL;
    if (g->cmd != NULL) {
        br_log_warn("texture readback inside a frame: this frame's draws are not submitted yet, so they are not in it");
    }
    bool swizzle;
    if (t->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM || t->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB) {
        swizzle = false;
    } else if (t->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
               t->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
        swizzle = true;
    } else {
        // ponytail: readback handles 8-bit RGBA/BGRA only; add a conversion per format when HDR targets need it.
        br_log_error("texture readback: unsupported format %d", (int)t->format);
        return false;
    }
    uint32_t size = t->width * t->height * 4;
    SDL_GPUTransferBuffer *transfer = br_gpu_staging_create(g, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, NULL);
    if (transfer == NULL) {
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (cmd == NULL) {
        br_log_error("cannot acquire command buffer: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(g->device, transfer);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src;
    SDL_zero(src);
    src.texture = t->handle;
    src.w = t->width;
    src.h = t->height;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst;
    SDL_zero(dst);
    dst.transfer_buffer = transfer;
    dst.pixels_per_row = t->width;
    dst.rows_per_layer = t->height;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == NULL) {
        br_log_error("cannot submit readback: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(g->device, transfer);
        return false;
    }
    SDL_WaitForGPUFences(g->device, true, &fence, 1);
    SDL_ReleaseGPUFence(g->device, fence);

    bool ok = false;
    uint8_t *out = br_alloc(size);
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(g->device, transfer, false);
    if (out != NULL && mapped != NULL) {
        for (uint32_t i = 0; i < size; i += 4) {
            out[i + 0] = swizzle ? mapped[i + 2] : mapped[i + 0];
            out[i + 1] = mapped[i + 1];
            out[i + 2] = swizzle ? mapped[i + 0] : mapped[i + 2];
            out[i + 3] = mapped[i + 3];
        }
        ok = true;
    }
    if (mapped != NULL) {
        SDL_UnmapGPUTransferBuffer(g->device, transfer);
    } else {
        br_log_error("cannot map readback buffer: %s", SDL_GetError());
    }
    SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    if (!ok) {
        br_free(out);
        return false;
    }
    *rgba = out;
    return true;
}

void br_gpu_texture_destroy(br_gpu *g, br_gpu_texture *t)
{
    if (t == NULL) {
        return;
    }
    SDL_ReleaseGPUTexture(g->device, t->handle);
    br_free(t);
}

/* ---- samplers */

static bool br_style_equal(const br_texture_style *a, const br_texture_style *b)
{
    return a->filter == b->filter && a->address_mode == b->address_mode && a->mipmaps == b->mipmaps &&
           a->max_anisotropy == b->max_anisotropy;
}

static SDL_GPUSamplerAddressMode br_address_to_sdl(br_address_mode mode)
{
    if (mode == BR_ADDRESS_REPEAT) {
        return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
    if (mode == BR_ADDRESS_MIRROR) {
        return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

SDL_GPUSampler *br_gpu_sampler_get(br_gpu *g, const br_texture_style *style)
{
    for (uint32_t i = 0; i < g->samplers.count; i++) {
        br_sampler_entry *entry = br_array_at(&g->samplers, i);
        if (br_style_equal(&entry->style, style)) {
            return entry->sampler;
        }
    }
    SDL_GPUFilter filter = style->filter == BR_FILTER_NEAREST ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_GPUSamplerCreateInfo info;
    SDL_zero(info);
    info.min_filter = filter;
    info.mag_filter = filter;
    info.mipmap_mode = style->filter == BR_FILTER_NEAREST ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST
                                                          : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = br_address_to_sdl(style->address_mode);
    info.address_mode_v = info.address_mode_u;
    info.address_mode_w = info.address_mode_u;
    info.enable_anisotropy = style->max_anisotropy > 1.0f;
    info.max_anisotropy = style->max_anisotropy > 1.0f ? style->max_anisotropy : 1.0f;
    info.min_lod = 0.0f;
    info.max_lod = style->mipmaps ? 1000.0f : 0.0f;
    SDL_GPUSampler *sampler = SDL_CreateGPUSampler(g->device, &info);
    if (sampler == NULL) {
        br_log_error("cannot create sampler: %s", SDL_GetError());
        return NULL;
    }
    br_sampler_entry entry = { *style, sampler };
    br_array_push(&g->samplers, &entry);
    return sampler;
}

/* ---- render targets */

static void br_render_target_release_textures(br_gpu *g, br_render_target *rt)
{
    br_gpu_texture_destroy(g, rt->color);
    br_gpu_texture_destroy(g, rt->msaa_color);
    br_gpu_texture_destroy(g, rt->depth_stencil);
    rt->color = NULL;
    rt->msaa_color = NULL;
    rt->depth_stencil = NULL;
}

static bool br_render_target_create_textures(br_gpu *g, br_render_target *rt, bool depth_stencil)
{
    rt->color = br_gpu_texture_create_ex(g, rt->width, rt->height, rt->color_format,
                                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1,
                                         SDL_GPU_SAMPLECOUNT_1);
    bool ok = rt->color != NULL;
    if (ok && rt->sample_count != SDL_GPU_SAMPLECOUNT_1) {
        rt->msaa_color = br_gpu_texture_create_ex(g, rt->width, rt->height, rt->color_format,
                                                  SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, rt->sample_count);
        ok = rt->msaa_color != NULL;
    }
    if (ok && depth_stencil) {
        rt->depth_stencil = br_gpu_texture_create_ex(g, rt->width, rt->height, g->depth_stencil_format,
                                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1,
                                                     rt->sample_count);
        ok = rt->depth_stencil != NULL;
    }
    if (!ok) {
        br_render_target_release_textures(g, rt);
    }
    return ok;
}

br_render_target *br_gpu_render_target_create(br_gpu *g, uint32_t width, uint32_t height,
                                              SDL_GPUTextureFormat color_format,
                                              SDL_GPUSampleCount sample_count, bool depth_stencil)
{
    br_render_target *rt = br_alloc(sizeof(br_render_target));
    if (rt == NULL) {
        return NULL;
    }
    rt->width = width;
    rt->height = height;
    rt->color_format = color_format;
    rt->sample_count = sample_count;
    if (!br_render_target_create_textures(g, rt, depth_stencil)) {
        br_free(rt);
        return NULL;
    }
    return rt;
}

bool br_gpu_render_target_resize(br_gpu *g, br_render_target *rt, uint32_t width, uint32_t height)
{
    if (rt->width == width && rt->height == height && rt->color != NULL) {
        return true;
    }
    bool depth_stencil = rt->depth_stencil != NULL;
    br_render_target_release_textures(g, rt); /* SDL keeps them alive while in-flight frames use them */
    rt->width = width;
    rt->height = height;
    return br_render_target_create_textures(g, rt, depth_stencil);
}

void br_gpu_render_target_destroy(br_gpu *g, br_render_target *rt)
{
    if (rt == NULL) {
        return;
    }
    br_render_target_release_textures(g, rt);
    br_free(rt);
}

SDL_GPUTextureFormat br_gpu_render_target_depth_format(const br_render_target *rt)
{
    return rt->depth_stencil != NULL ? rt->depth_stencil->format : SDL_GPU_TEXTUREFORMAT_INVALID;
}

SDL_GPURenderPass *br_gpu_render_target_begin(br_gpu *g, SDL_GPUCommandBuffer *cmd, br_render_target *rt,
                                              bool clear, br_color clear_color)
{
    g->stats.target_switches += 1;
    if (rt == NULL || rt->color == NULL) {
        br_log_error("render pass on a missing render target");
        return NULL;
    }
    SDL_GPUColorTargetInfo color;
    SDL_zero(color);
    color.clear_color.r = clear_color.r;
    color.clear_color.g = clear_color.g;
    color.clear_color.b = clear_color.b;
    color.clear_color.a = clear_color.a;
    color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    /* cycling on clear lets SDL hand out fresh memory instead of waiting on last frame */
    color.cycle = clear;
    if (rt->msaa_color != NULL) {
        color.texture = rt->msaa_color->handle;
        color.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
        color.resolve_texture = rt->color->handle;
        color.cycle_resolve_texture = clear;
    } else {
        color.texture = rt->color->handle;
        color.store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth;
    SDL_zero(depth);
    if (rt->depth_stencil != NULL) {
        depth.texture = rt->depth_stencil->handle;
        depth.clear_depth = 1.0f;
        depth.clear_stencil = 0;
        depth.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth.stencil_load_op = depth.load_op;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
        depth.cycle = clear;
    }
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, rt->depth_stencil != NULL ? &depth : NULL);
    if (pass == NULL) {
        br_log_error("cannot begin render pass: %s", SDL_GetError());
        return NULL;
    }
    SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)rt->width, (float)rt->height, 0.0f, 1.0f };
    SDL_SetGPUViewport(pass, &viewport);
    return pass;
}
