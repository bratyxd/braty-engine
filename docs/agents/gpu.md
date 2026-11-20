# gpu - thin layer over the SDL3 GPU API

Header: `engine/include/braty/br_gpu.h` (read its top comment: frame flow and shader rules).
Source: `engine/src/gpu/`. Example: `examples/hello_quad.c`. Tests: `tests/test_gpu.c` (hidden
window; skips when no GPU device can be created): transient alignment and late allocs, upload
bounds, RGBA/BGRA readback, sampler cache.

It does not hide SDL_GPU: SDL types are used directly, and `br_gpu_device(g)` gives you the
`SDL_GPUDevice` for anything not wrapped. What it adds: shader blobs per backend, caches
(shader per blob, pipeline per `br_pipeline_desc`, sampler per `br_texture_style`), textures and
render targets that remember their size/format, per-frame transient vertex/index streaming,
readback, and the frame target.

## Functions you actually need

| Function | What for |
|---|---|
| `br_engine_gpu(e)` | Get the `br_gpu` (NULL when headless: every example runs `--headless` in CTest, so guard GPU calls made outside RENDER; `br_gpu_set_clear_color` and `br_gpu_pipeline_count` accept NULL). |
| `br_gpu_pipeline_get(g, &desc)` | Cached pipeline for shaders + layout + blend + formats. |
| `br_gpu_sampler_get(g, &style)` | Cached sampler (filter, address mode, mips, anisotropy). |
| `br_gpu_texture_create_rgba8(g, w, h, pixels, mipmaps)` | Upload an image. |
| `br_gpu_transient_alloc_vertices/indices(g, size, &offset)` | Per-frame geometry (PRE_RENDER). |
| `br_gpu_transient_upload(g, cmd)` | One copy pass before render passes (first call per frame uploads, later calls no-op). |
| `br_gpu_frame_cmd(g)`, `br_gpu_frame_target(g)`, `br_gpu_frame_begin_pass(g)` | Draw the frame (RENDER). |
| `br_gpu_render_target_create/begin` | Offscreen targets (render textures, filters, 3D). |
| `br_gpu_texture_read(g, tex, &rgba)` | Blocking readback (extract, screenshots). |

## Snippet (from examples/hello_quad.c, RENDER system)

```c
br_gpu *g = br_engine_gpu(e);
SDL_GPUCommandBuffer *cmd = br_gpu_frame_cmd(g);
br_render_target *target = br_gpu_frame_target(g);

br_pipeline_desc desc;
SDL_zero(desc);
desc.vertex_shader = &br_shader_textured_vert;          /* #include "textured.vert.h" */
desc.fragment_shader = &br_shader_textured_frag;
desc.layout.buffer_count = 1;
desc.layout.strides[0] = sizeof(quad_vertex);
desc.layout.attribute_count = 3;
desc.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
desc.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
desc.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
desc.blend = br_gpu_blend_premultiplied();
desc.color_format = target->color_format;
desc.depth_stencil_format = br_gpu_render_target_depth_format(target);
SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);

br_gpu_transient_upload(g, cmd);                       /* before any render pass */
SDL_GPURenderPass *pass = br_gpu_frame_begin_pass(g);  /* first pass of the frame clears */
SDL_BindGPUGraphicsPipeline(pass, pipeline);
SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), s->vertex_offset };
SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
/* ... index buffer, SDL_BindGPUFragmentSamplers, SDL_PushGPUVertexUniformData(cmd, 0, ...) */
SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
SDL_EndGPURenderPass(pass);
```

## Adding a shader

1. Write `engine/shaders/<name>.vert.hlsl` and `<name>.frag.hlsl` (rules in `docs/BUILDING.md`).
2. `python tools/compile_shaders.py` -> `engine/shaders/compiled/<name>.<stage>.h`.
3. `#include "<name>.vert.h"` in the one `.c` that uses it and reference `br_shader_<name>_vert`.
4. Commit the `.hlsl` and the generated `.h`.

## Gotchas

- The frame target has depth+stencil: pipelines drawing into it must set
  `depth_stencil_format = br_gpu_render_target_depth_format(target)`.
- Call `br_gpu_transient_upload` in RENDER before any render pass; only the first call of a frame uploads, so allocate everything in PRE_RENDER (copy passes
  cannot run inside render passes). Transient pointers die at the next alloc; write at once.
  An alloc after that upload returns NULL and logs (once per frame): its bytes would never reach
  the GPU.
- `br_gpu_buffer_update`, `br_gpu_texture_upload` and `br_gpu_texture_read` submit their own
  command buffer at once. Use them outside RENDER / POST_RENDER: inside, an upload lands before
  every command of the current frame (also draws recorded earlier), and a readback does not see
  this frame's draws (it logs a warning). `br_gpu_texture_upload` refuses a region outside the
  texture (SDL checks it only in debug mode).
- Vsync is the app's: `br_app_set_vsync` (there is no GPU-level vsync call; `br_gpu_set_present_mode`
  bypasses `br_app_vsync`).
- `br_gpu_frame_*` return NULL outside RENDER/POST_RENDER, and RENDER does not run at all when
  the window is minimised. Never cache the command buffer or the frame target across frames
  (the target is recreated on resize).
- The frame target is in pixels; lay out in window units with `br_engine_window_size` and an
  ortho matrix, so high-DPI just works.
- Cached shaders/pipelines/samplers belong to `br_gpu`: never release them.
- Fragment input struct must equal the vertex output struct (`SV_Position` included).
- `--gpu-driver vulkan|direct3d12|metal` picks the backend; both D3D12 and Vulkan are verified.
