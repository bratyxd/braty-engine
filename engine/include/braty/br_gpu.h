/* br_gpu.h - thin layer over the SDL3 GPU API.
 *
 * What it adds on top of SDL_GPU:
 * - shader blobs with one slot per SDL_GPUShaderFormat, picked at runtime (br_shader_blob);
 * - caches: one SDL_GPUShader per blob, one pipeline per br_pipeline_desc, one sampler per
 *   br_texture_style. Cached objects are owned by br_gpu: never release them yourself;
 * - textures and render targets with their size/format kept next to the SDL handle;
 * - transient per-frame vertex/index streaming (write on the CPU, one copy pass uploads it);
 * - the frame: an engine-owned offscreen "frame target" that systems render into; the app
 *   blits it to the swapchain at the end of the frame (the swapchain itself is write-only,
 *   so this is what makes screenshots and readback possible).
 *
 * Anything not wrapped here, call SDL directly through br_gpu_device(g). SDL types are used
 * as-is in this API (SDL_GPUTextureFormat, SDL_GPUColorTargetBlendState, ...).
 *
 * Frame flow (driven by br_app; you only write systems):
 *   PRE_RENDER systems : fill CPU data, br_gpu_transient_alloc_* for vertices/indices
 *   [app]              : acquire command buffer + swapchain texture. If there is none
 *                        (minimised, zero-size), RENDER and POST_RENDER are skipped.
 *   RENDER systems     : cmd = br_gpu_frame_cmd(g); br_gpu_transient_upload(g, cmd) (first call uploads);
 *                        pass = br_gpu_frame_begin_pass(g); bind, draw; SDL_EndGPURenderPass(pass)
 *   POST_RENDER systems: overlays (debug UI) on the same frame target
 *   [app]              : blit frame target -> swapchain, submit, optional screenshot
 *
 * Shader resource conventions (from SDL_CreateGPUShader; tools/compile_shaders.py follows them):
 *   vertex   : textures+samplers t[n]/s[n] space0, uniform buffers b[n] space1
 *   fragment : textures+samplers t[n]/s[n] space2, uniform buffers b[n] space3
 *   SPIR-V set = HLSL space. Vertex inputs use TEXCOORD0, TEXCOORD1, ... (location = index).
 *   The fragment input struct must equal the vertex output struct, SV_Position included
 *   (D3D12 links stages by register order; a mismatch fails pipeline creation).
 *   Pair each texture with a sampler and mark both [[vk::combinedImageSampler]].
 *   Uniforms: SDL_PushGPUVertexUniformData / SDL_PushGPUFragmentUniformData, slot = b index.
 *   Matrices: upload br_mat4 as-is (column-major) and use mul(matrix, vector) in HLSL.
 */
#ifndef BR_GPU_H
#define BR_GPU_H

#include "braty/br_core.h"
#include "braty/br_math.h"

#include <SDL3/SDL_gpu.h>

typedef struct br_gpu br_gpu;

/* ---- device */

/* Creates the GPU device for `window` and claims its swapchain. driver_name: "vulkan",
 * "direct3d12", "metal", or NULL for SDL's choice. Returns NULL on failure (logged). */
br_gpu *br_gpu_create(SDL_Window *window, const char *driver_name);
/* Waits for the GPU, releases every cached and owned object, destroys the device. */
void br_gpu_destroy(br_gpu *g);
/* The underlying SDL device, for anything this layer does not wrap. */
SDL_GPUDevice *br_gpu_device(br_gpu *g);
/* The driver in use: "vulkan", "direct3d12", "metal", ... */
const char *br_gpu_driver_name(br_gpu *g);
/* Pixel format of the swapchain and of the frame target. */
SDL_GPUTextureFormat br_gpu_swapchain_format(br_gpu *g);
/* Best supported depth+stencil format (D24S8 or D32S8). */
SDL_GPUTextureFormat br_gpu_depth_stencil_format(br_gpu *g);
/* Sets exactly this present mode. False (not logged) when the window does not support it.
 * Games use br_app_set_vsync, which calls this and keeps br_app_vsync in step. */
bool br_gpu_set_present_mode(br_gpu *g, SDL_GPUPresentMode mode);

/* ---- shaders */

/* One slot per SDL_GPUShaderFormat bit, so a console format is one more slot. */
typedef enum br_shader_format {
    BR_SHADER_FORMAT_PRIVATE,
    BR_SHADER_FORMAT_SPIRV,
    BR_SHADER_FORMAT_DXBC,
    BR_SHADER_FORMAT_DXIL,
    BR_SHADER_FORMAT_MSL,
    BR_SHADER_FORMAT_METALLIB,
    BR_SHADER_FORMAT_COUNT
} br_shader_format;

/* Compiled code for one format. size 0 = not available in this format. */
typedef struct br_shader_code {
    const uint8_t *code;
    size_t         size;
    const char    *entrypoint;
} br_shader_code;

/* A shader compiled offline for every backend (see tools/compile_shaders.py, which writes
 * these into engine/shaders/compiled/<name>.<stage>.h as `br_shader_<name>_<stage>`). */
typedef struct br_shader_blob {
    const char        *name;
    SDL_GPUShaderStage stage;
    uint32_t           num_samplers;
    uint32_t           num_storage_textures;
    uint32_t           num_storage_buffers;
    uint32_t           num_uniform_buffers;
    br_shader_code     formats[BR_SHADER_FORMAT_COUNT];
} br_shader_blob;

/* Returns the cached SDL shader for a blob (created on first use, in the device's format).
 * The blob must outlive the br_gpu (generated blobs are static, so they do). NULL on failure. */
SDL_GPUShader *br_gpu_shader_get(br_gpu *g, const br_shader_blob *blob);

/* ---- pipelines */

#define BR_MAX_VERTEX_BUFFERS 4
#define BR_MAX_VERTEX_ATTRIBUTES 16

/* Vertex input: buffers (stride, per-vertex or per-instance) and attributes. */
typedef struct br_vertex_layout {
    uint32_t               buffer_count;
    uint32_t               strides[BR_MAX_VERTEX_BUFFERS];
    bool                   per_instance[BR_MAX_VERTEX_BUFFERS];
    uint32_t               attribute_count;
    SDL_GPUVertexAttribute attributes[BR_MAX_VERTEX_ATTRIBUTES]; /* location, buffer_slot, format, offset */
} br_vertex_layout;

/* Everything that selects a graphics pipeline. Zero-initialise, then fill.
 * Zero means: triangle list, no culling, counter-clockwise front, blending off (writes all
 * channels), 1 sample, no depth/stencil target. color_format must be set. */
typedef struct br_pipeline_desc {
    const br_shader_blob        *vertex_shader;
    const br_shader_blob        *fragment_shader;
    br_vertex_layout             layout;
    SDL_GPUPrimitiveType         primitive;
    SDL_GPUCullMode              cull_mode;
    SDL_GPUFrontFace             front_face;
    SDL_GPUColorTargetBlendState blend;
    SDL_GPUTextureFormat         color_format;
    SDL_GPUTextureFormat         depth_stencil_format; /* SDL_GPU_TEXTUREFORMAT_INVALID = none */
    SDL_GPUSampleCount           sample_count;
    bool                         depth_test;
    bool                         depth_write;
    SDL_GPUCompareOp             depth_compare;
    bool                         stencil_test;
    SDL_GPUStencilOpState        stencil_front;
    SDL_GPUStencilOpState        stencil_back;
    uint8_t                      stencil_read_mask;
    uint8_t                      stencil_write_mask;
} br_pipeline_desc;

/* Returns the cached pipeline for `desc`, creating it on first use. NULL on failure. */
SDL_GPUGraphicsPipeline *br_gpu_pipeline_get(br_gpu *g, const br_pipeline_desc *desc);
/* Number of pipelines in the cache (for stats). */
uint32_t br_gpu_pipeline_count(br_gpu *g);
/* Blend state for premultiplied-alpha "normal" blending (src = ONE, dst = ONE_MINUS_SRC_ALPHA). */
SDL_GPUColorTargetBlendState br_gpu_blend_premultiplied(void);

/* ---- buffers */

/* Creates a GPU buffer, uploading `data` if non-NULL. Free with br_gpu_buffer_destroy. */
SDL_GPUBuffer *br_gpu_buffer_create(br_gpu *g, SDL_GPUBufferUsageFlags usage, uint32_t size, const void *data);
/* Uploads bytes into an existing buffer (own command buffer, submitted at once). Call outside
 * RENDER / POST_RENDER: inside, the data lands before ALL of this frame's commands (draws
 * recorded earlier in the frame see it too). */
bool br_gpu_buffer_update(br_gpu *g, SDL_GPUBuffer *buffer, uint32_t offset, const void *data, uint32_t size);
/* Releases a buffer from br_gpu_buffer_create. NULL is fine. */
void br_gpu_buffer_destroy(br_gpu *g, SDL_GPUBuffer *buffer);

/* ---- transient per-frame streaming (reset by the app at the start of every frame)
 *
 * Offsets are 16-byte aligned. Returned pointers stay valid until the next alloc call, so
 * write the data right away. Bind with br_gpu_transient_vertex_buffer + the returned offset.
 */

/* Reserves `size` bytes of vertex data for this frame. Returns a CPU pointer to fill. NULL (logged
 * once per frame) after this frame's br_gpu_transient_upload: allocate in PRE_RENDER. */
void *br_gpu_transient_alloc_vertices(br_gpu *g, uint32_t size, uint32_t *offset);
/* Reserves `size` bytes of index data for this frame. Same rules as the vertex stream. */
void *br_gpu_transient_alloc_indices(br_gpu *g, uint32_t size, uint32_t *offset);
/* Uploads this frame's transient data in one copy pass. Call in RENDER before any render pass;
 * the first call of a frame uploads, later calls do nothing (allocate everything in PRE_RENDER). */
bool br_gpu_transient_upload(br_gpu *g, SDL_GPUCommandBuffer *cmd);
/* GPU buffer holding this frame's transient vertices (valid after br_gpu_transient_upload). */
SDL_GPUBuffer *br_gpu_transient_vertex_buffer(br_gpu *g);
/* GPU buffer holding this frame's transient indices (valid after br_gpu_transient_upload). */
SDL_GPUBuffer *br_gpu_transient_index_buffer(br_gpu *g);

/* ---- textures and samplers */

typedef enum br_filter_mode { BR_FILTER_LINEAR, BR_FILTER_NEAREST } br_filter_mode;
typedef enum br_address_mode { BR_ADDRESS_CLAMP, BR_ADDRESS_REPEAT, BR_ADDRESS_MIRROR } br_address_mode;

/* How a texture is sampled (PixiJS TextureStyle). Zero = linear, clamp, no mips, no aniso. */
typedef struct br_texture_style {
    br_filter_mode  filter;          /* min/mag filter */
    br_address_mode address_mode;    /* u, v and w */
    bool            mipmaps;         /* sample mip levels (texture must have them) */
    float           max_anisotropy;  /* <= 1 disables anisotropic filtering */
} br_texture_style;

/* A GPU texture plus the facts you usually need about it. */
typedef struct br_gpu_texture {
    SDL_GPUTexture          *handle;
    uint32_t                 width;
    uint32_t                 height;
    uint32_t                 mip_levels;
    SDL_GPUTextureFormat     format;
    SDL_GPUTextureUsageFlags usage;
    SDL_GPUSampleCount       sample_count;
} br_gpu_texture;

/* Creates an empty 2D texture. mipmaps = full mip chain (adds COLOR_TARGET usage, needed for generation). */
br_gpu_texture *br_gpu_texture_create(br_gpu *g, uint32_t width, uint32_t height, SDL_GPUTextureFormat format,
                                      SDL_GPUTextureUsageFlags usage, bool mipmaps);
/* Creates a sampleable RGBA8 texture from tightly packed pixels (may be NULL), generating mips if asked. */
br_gpu_texture *br_gpu_texture_create_rgba8(br_gpu *g, uint32_t width, uint32_t height, const void *pixels,
                                            bool mipmaps);
/* Uploads tightly packed texels (in the texture's format) into a region of mip 0, then regenerates
 * mips. False (logged) when the region is empty or not inside the texture. Own command buffer,
 * submitted at once (same ordering note as br_gpu_buffer_update). */
bool br_gpu_texture_upload(br_gpu *g, br_gpu_texture *t, uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, const void *texels);
/* Reads mip 0 back as tightly packed RGBA8 (BGRA formats are swizzled). Blocks until the GPU is done.
 * Supports 8-bit RGBA/BGRA formats only. Free *rgba with br_free. Inside RENDER / POST_RENDER it
 * does not see this frame's draws (they are submitted at frame end) and logs a warning. */
bool br_gpu_texture_read(br_gpu *g, br_gpu_texture *t, uint8_t **rgba);
/* Releases a texture. NULL is fine. */
void br_gpu_texture_destroy(br_gpu *g, br_gpu_texture *t);
/* Returns the cached sampler for a style. */
SDL_GPUSampler *br_gpu_sampler_get(br_gpu *g, const br_texture_style *style);

/* ---- render targets */

/* A colour target with optional MSAA and depth/stencil. `color` is always the sampleable
 * single-sample result (MSAA is resolved into it when a pass ends). */
typedef struct br_render_target {
    br_gpu_texture      *color;          /* COLOR_TARGET | SAMPLER */
    br_gpu_texture      *msaa_color;     /* NULL unless sample_count > 1 */
    br_gpu_texture      *depth_stencil;  /* NULL unless requested */
    uint32_t             width;
    uint32_t             height;
    SDL_GPUTextureFormat color_format;
    SDL_GPUSampleCount   sample_count;
} br_render_target;

/* Creates a render target. Returns NULL on failure. */
br_render_target *br_gpu_render_target_create(br_gpu *g, uint32_t width, uint32_t height,
                                              SDL_GPUTextureFormat color_format,
                                              SDL_GPUSampleCount sample_count, bool depth_stencil);
/* Recreates the textures at a new size (contents are lost). No-op if the size is unchanged. */
bool br_gpu_render_target_resize(br_gpu *g, br_render_target *rt, uint32_t width, uint32_t height);
/* Releases a render target. NULL is fine. */
void br_gpu_render_target_destroy(br_gpu *g, br_render_target *rt);
/* Depth/stencil format of a target, or SDL_GPU_TEXTUREFORMAT_INVALID (for br_pipeline_desc). */
SDL_GPUTextureFormat br_gpu_render_target_depth_format(const br_render_target *rt);
/* Begins a render pass on `rt`. clear = clear colour/depth(1)/stencil(0), else load the contents.
 * Sets a full-target viewport. End it with SDL_EndGPURenderPass. NULL on failure. */
SDL_GPURenderPass *br_gpu_render_target_begin(br_gpu *g, SDL_GPUCommandBuffer *cmd, br_render_target *rt,
                                              bool clear, br_color clear_color);

/* ---- the frame (valid in RENDER and POST_RENDER systems only) */

/* Command buffer of the current frame, NULL outside RENDER/POST_RENDER. */
SDL_GPUCommandBuffer *br_gpu_frame_cmd(br_gpu *g);
/* Window-sized target (swapchain format, with depth/stencil) that the frame renders into. */
br_render_target *br_gpu_frame_target(br_gpu *g);
/* Begins a render pass on the frame target: the first pass of a frame clears it to the clear
 * colour, later passes load it. End with SDL_EndGPURenderPass. NULL outside RENDER phases. */
SDL_GPURenderPass *br_gpu_frame_begin_pass(br_gpu *g);
/* Colour the frame target is cleared to (default opaque black). */
void br_gpu_set_clear_color(br_gpu *g, br_color color);
/* Number of frames presented so far. */
uint64_t br_gpu_frames_presented(br_gpu *g);

#endif
