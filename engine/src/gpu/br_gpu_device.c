/* Device creation, swapchain, and the per-frame acquire / blit / submit. */
#include "gpu/br_gpu_internal.h"

static SDL_GPUTextureFormat br_gpu_pick_depth_format(SDL_GPUDevice *device)
{
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D, usage)) {
        return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    }
    return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

br_gpu *br_gpu_create(SDL_Window *window, const char *driver_name)
{
    /* The formats tools/compile_shaders.py produces. */
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    bool debug_mode = false;
#ifndef NDEBUG
    debug_mode = true; /* validation when the driver has it; harmless otherwise */
#endif
    SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, debug_mode, driver_name);
    if (device == NULL) {
        br_log_error("cannot create GPU device (driver '%s'): %s", driver_name ? driver_name : "default",
                     SDL_GetError());
        return NULL;
    }
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        br_log_error("cannot claim window for GPU: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        return NULL;
    }
    br_gpu *g = br_alloc(sizeof(br_gpu));
    if (g == NULL) {
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        return NULL;
    }
    g->device = device;
    g->window = window;
    g->swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    g->depth_stencil_format = br_gpu_pick_depth_format(device);
    br_array_init(&g->shaders, sizeof(br_shader_entry));
    br_array_init(&g->pipelines, sizeof(br_pipeline_entry));
    br_array_init(&g->samplers, sizeof(br_sampler_entry));
    br_array_init(&g->vertices.cpu, 1);
    br_array_init(&g->indices.cpu, 1);
    g->vertices.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    g->indices.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    g->clear_color = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    br_log_info("GPU driver: %s", SDL_GetGPUDeviceDriver(device));
    return g;
}

void br_gpu_destroy(br_gpu *g)
{
    if (g == NULL) {
        return;
    }
    SDL_WaitForGPUIdle(g->device);
    if (g->frame_fence != NULL) {
        SDL_ReleaseGPUFence(g->device, g->frame_fence);
    }
    br_gpu_render_target_destroy(g, g->frame_target);
    br_gpu_release_transient(g);
    br_gpu_release_caches(g);
    SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
    SDL_DestroyGPUDevice(g->device);
    br_free(g);
}

SDL_GPUDevice *br_gpu_device(br_gpu *g)
{
    return g->device;
}

const char *br_gpu_driver_name(br_gpu *g)
{
    return SDL_GetGPUDeviceDriver(g->device);
}

SDL_GPUTextureFormat br_gpu_swapchain_format(br_gpu *g)
{
    return g->swapchain_format;
}

SDL_GPUTextureFormat br_gpu_depth_stencil_format(br_gpu *g)
{
    return g->depth_stencil_format;
}

bool br_gpu_set_present_mode(br_gpu *g, SDL_GPUPresentMode mode)
{
    if (!SDL_WindowSupportsGPUPresentMode(g->device, g->window, mode)) {
        return false;
    }
    if (!SDL_SetGPUSwapchainParameters(g->device, g->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
        br_log_error("cannot set present mode: %s", SDL_GetError());
        return false;
    }
    return true;
}

/* ---- frame */

bool br_gpu_frame_begin(br_gpu *g)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (cmd == NULL) {
        br_log_error("cannot acquire command buffer: %s", SDL_GetError());
        return false;
    }
    SDL_GPUTexture *swapchain = NULL;
    Uint32 width = 0;
    Uint32 height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g->window, &swapchain, &width, &height)) {
        /* nothing acquired, so cancelling is allowed */
        br_log_error("cannot acquire swapchain texture: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    if (swapchain == NULL || width == 0 || height == 0) {
        /* Minimised or zero-size window: not an error. Submit the empty buffer and skip
         * rendering; no GPU call may touch a NULL swapchain texture. */
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    if (g->frame_target == NULL) {
        g->frame_target = br_gpu_render_target_create(g, width, height, g->swapchain_format,
                                                      SDL_GPU_SAMPLECOUNT_1, true);
    } else {
        br_gpu_render_target_resize(g, g->frame_target, width, height);
    }
    if (g->frame_target == NULL || g->frame_target->color == NULL) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    g->cmd = cmd;
    g->swapchain = swapchain;
    g->frame_cleared = false;
    return true;
}

void br_gpu_frame_end(br_gpu *g)
{
    if (g->cmd == NULL) {
        return;
    }
    if (!g->frame_cleared) {
        SDL_GPURenderPass *pass = br_gpu_frame_begin_pass(g);
        if (pass != NULL) {
            SDL_EndGPURenderPass(pass);
        }
    }
    br_render_target *rt = g->frame_target;
    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture = rt->color->handle;
    blit.source.w = rt->width;
    blit.source.h = rt->height;
    blit.destination.texture = g->swapchain;
    blit.destination.w = rt->width;
    blit.destination.h = rt->height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(g->cmd, &blit);
    g->stats.submit_ns = SDL_GetTicksNS();
    bool submitted;
    if (g->want_fence) {
        /* the profiler polls this fence to estimate GPU time (nobody else waits on it) */
        if (g->frame_fence != NULL) {
            SDL_ReleaseGPUFence(g->device, g->frame_fence);
        }
        g->frame_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g->cmd);
        submitted = g->frame_fence != NULL;
    } else {
        submitted = SDL_SubmitGPUCommandBuffer(g->cmd);
    }
    if (!submitted) {
        br_log_error("cannot submit frame: %s", SDL_GetError());
    }
    g->cmd = NULL;
    g->swapchain = NULL;
    g->frames_presented += 1;
}

SDL_GPUCommandBuffer *br_gpu_frame_cmd(br_gpu *g)
{
    return g->cmd;
}

br_render_target *br_gpu_frame_target(br_gpu *g)
{
    return g->frame_target;
}

SDL_GPURenderPass *br_gpu_frame_begin_pass(br_gpu *g)
{
    if (g->cmd == NULL) {
        return NULL;
    }
    bool clear = !g->frame_cleared;
    g->frame_cleared = true;
    return br_gpu_render_target_begin(g, g->cmd, g->frame_target, clear, g->clear_color);
}

void br_gpu_set_clear_color(br_gpu *g, br_color color)
{
    g->clear_color = color;
}

uint64_t br_gpu_frames_presented(br_gpu *g)
{
    return g->frames_presented;
}
