/* br_gpu_internal.h - br_gpu state and the frame calls used by br_app. Engine-private. */
#ifndef BR_GPU_INTERNAL_H
#define BR_GPU_INTERNAL_H

#include "braty/br_gpu.h"

#include <SDL3/SDL.h>

typedef struct br_shader_entry {
    const br_shader_blob *blob;
    SDL_GPUShader        *shader;
} br_shader_entry;

typedef struct br_pipeline_entry {
    br_pipeline_desc         desc;
    SDL_GPUGraphicsPipeline *pipeline;
} br_pipeline_entry;

typedef struct br_sampler_entry {
    br_texture_style style;
    SDL_GPUSampler  *sampler;
} br_sampler_entry;

/* CPU bytes collected during a frame, uploaded to `buffer` in one copy pass. */
typedef struct br_transient_stream {
    br_array                cpu;       /* uint8_t */
    SDL_GPUBuffer          *buffer;
    SDL_GPUTransferBuffer  *transfer;
    uint32_t                capacity;  /* bytes allocated in buffer and transfer */
    SDL_GPUBufferUsageFlags usage;
} br_transient_stream;

/* Per-frame numbers for the profiler (read and zeroed by br_profiler_frame_end). */
typedef struct br_gpu_frame_stats {
    uint64_t submit_ns;        /* SDL_GetTicksNS just before the frame's submit, 0 = none */
    uint32_t uploads;          /* copy-pass uploads (textures, buffers, transient geometry) */
    uint64_t upload_bytes;
    uint32_t target_switches;  /* render passes begun */
} br_gpu_frame_stats;

struct br_gpu {
    SDL_GPUDevice       *device;
    SDL_Window          *window;
    SDL_GPUTextureFormat swapchain_format;
    SDL_GPUTextureFormat depth_stencil_format;

    // ponytail: caches are linear arrays; fine for the tens of entries a 2D/3D game uses, hash them if profiles say so.
    br_array shaders;    /* br_shader_entry */
    br_array pipelines;  /* br_pipeline_entry */
    br_array samplers;   /* br_sampler_entry */

    /* draw statistics: counted this frame, and the totals of the last finished frame */
    uint32_t draw_calls, set_passes, last_draw_calls, last_set_passes;
    SDL_GPURenderPass       *bound_pass;      /* pass and pipeline of the last bind */
    SDL_GPUGraphicsPipeline *bound_pipeline;

    br_transient_stream vertices;
    br_transient_stream indices;
    bool                transient_uploaded;  /* this frame's data is on the GPU (later calls no-op) */
    bool                transient_late_logged; /* an alloc after the upload was logged this frame */

    /* current frame */
    SDL_GPUCommandBuffer *cmd;            /* non-NULL only between frame_begin and frame_end */
    SDL_GPUCommandBuffer *present_cmd;    /* holds the swapchain texture; only the final blit */
    SDL_GPUTexture       *swapchain;
    br_render_target     *frame_target;
    bool                  frame_cleared;
    br_color              clear_color;
    uint64_t              frames_presented;

    /* profiler */
    br_gpu_frame_stats stats;
    bool               want_fence;   /* submit frames with a fence (the profiler's GPU estimate) */
    SDL_GPUFence      *frame_fence;  /* the last frame's rendering fence until the profiler takes it */
    SDL_GPUFence      *present_fence; /* the last frame's swapchain blit + present, same owner */
};

/* Clears the transient streams. Called by the engine before any system of a frame runs. */
void br_gpu_transient_reset(br_gpu *g);
/* Acquires the command buffer and swapchain texture. False = no swapchain (minimised,
 * zero-size, error): nothing is recorded and RENDER phases must be skipped. */
bool br_gpu_frame_begin(br_gpu *g);
/* Clears the frame target if nobody drew, blits it to the swapchain and submits. */
void br_gpu_frame_end(br_gpu *g);
/* Releases cached shaders/pipelines/samplers (called by br_gpu_destroy). */
void br_gpu_release_caches(br_gpu *g);
/* Releases the transient GPU buffers (called by br_gpu_destroy). */
void br_gpu_release_transient(br_gpu *g);

#endif
