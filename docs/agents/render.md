# render - batcher, draw list, textures, masks, render textures, extension points

Headers: `engine/include/braty/br_render.h`, `br_texture.h`. Source: `engine/src/render/`,
shaders `engine/shaders/batch.*.hlsl`. Example: `examples/sprites.c` (every snippet below is from it).

## How a frame is drawn

1. PRE_RENDER `render.collect` (priority 1000, after transforms/sort/cull): render-to-texture
   jobs, then the stage, then overlays. Each visible node's type `render` callback gets a
   `br_render_ctx` and pushes geometry. Output: a CPU draw list (passes, batches, custom draws,
   callbacks) and vertices in the GPU transient buffers.
2. RENDER `render.submit` (priority 0): `br_gpu_transient_upload`, then replays the list.

Batches break on: blend mode, space (render group / `br_render_set_space`), scissor, stencil
state, a 9th texture, 65535 vertices, a pass switch, a custom draw or a callback.

## Functions you actually need

| Function | What for |
|---|---|
| `br_texture_load(e, path)` | PNG/JPG/... (missing file -> logged + magenta fallback). |
| `br_texture_create_rgba8`, `br_texture_create_frame`, `br_texture_white` | Textures from code, atlas frames. |
| `br_render_texture_create(e, w, h, res)`, `br_render_to_texture(e, node, rt)` | Pixi RenderTexture. |
| `br_node_set_mask(e, n, mask, BR_MASK_SCISSOR or BR_MASK_STENCIL, inverse)` | Masks. |
| `br_render_get_stats(e, &stats)` | Draw calls, batches, vertices, passes of the last frame. |
| In a render callback: `br_render_node_transform`, `br_render_node_color`, `br_render_texture_info`, `br_render_push_quad/triangles/custom/callback` | Drawing. |

## Extension point 1: push quads/triangles from a node type's render callback

Vertex positions are in the current *space*: map local points with `br_render_node_transform`.
Colours are premultiplied RGBA8 (`br_render_node_color(rc, n)` = world tint * world alpha).
The sprite type (`engine/src/scene/br_sprite.c`) is the reference:

```c
const br_texture_info *info = br_render_texture_info(rc, sp->texture); /* cached per frame */
br_mat3 m = br_render_node_transform(rc, n);
uint32_t color = br_render_node_color(rc, n);
br_vertex2d v[4];
for (int i = 0; i < 4; i++) {
    br_vec2 p = br_mat3_apply(m, corners[i]);
    v[i] = (br_vertex2d){ p.x, p.y, info->uvs[i].x, info->uvs[i].y, color };
}
br_render_push_quad(rc, sp->texture, v);
```

Untextured geometry (graphics): use `br_texture_white(e)` and vertex colours.
`br_render_push_triangles` takes up to 65535 vertices with 16-bit indices per call.

## Extension point 2: custom shaders (`br_render_push_custom`)

Data is copied at push time; masks, scissor, blend and the target are applied by the engine.
Uniform slots: vertex `b0` = engine transform (`ortho * space`), vertex `b1` = your
`vertex_uniforms`, fragment `b0` = your `fragment_uniforms`; texture i = `t[i]/s[i]`. 32-bit
indices, optional instance buffer (slot 1).

```c
br_custom_draw draw;
SDL_zero(draw);
draw.vertex_shader = &br_shader_textured_vert; /* its b0 is the engine-pushed transform */
draw.fragment_shader = &br_shader_textured_frag;
draw.layout.buffer_count = 1;
draw.layout.strides[0] = sizeof(br_vertex2d);
draw.layout.attribute_count = 3;
draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
draw.vertices = vertices;
draw.vertex_bytes = sizeof(vertices);
draw.indices = indices;
draw.index_count = BR_ARRAY_COUNT(indices);
draw.textures[0] = w->texture;
draw.texture_count = 1;
br_render_push_custom(rc, &draw);
```

## Extension point 3: callback commands (`br_render_push_callback`) - 3D, filter passes

`own_passes = false`: your function runs inside the open pass (`info->pass`); build pipelines with
`br_render_pipeline_setup(info, &desc)` so formats, blend and stencil test match.
`own_passes = true`: the engine ends its pass, you run any passes on `info->cmd` (3D scene into an
HDR target, ping-pong blurs), the engine reopens its target with contents kept.
Allocate transient vertices during collect (in the render callback), never in the callback.
Scissor masks: `info->scissor_on` / `info->scissor` (target pixels, clamped). In-pass callbacks
start with that scissor already set; if you call `SDL_SetGPUScissor` yourself, pass `&info->scissor`.

```c
/* in the node's render callback (PRE_RENDER) */
br_vertex2d *v = br_gpu_transient_alloc_vertices(br_engine_gpu(e), 3 * sizeof(br_vertex2d), &s->triangle_offset);
...
br_render_push_callback(rc, triangle_draw, s, false);   /* draws in the current pass */
br_render_push_callback(rc, clear_target_draw, s, true); /* runs its own pass */

/* replayed in RENDER */
static void clear_target_draw(br_engine *e, const br_render_callback_info *info, void *user)
{
    br_render_target *rt = br_texture_render_target(e, s->callback_target);
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(br_engine_gpu(e), info->cmd, rt, true, c);
    if (pass != NULL) {
        SDL_EndGPURenderPass(pass);
    }
}
```

## Extension point 4: render a subtree offscreen and draw the result

`br_render_subtree_to_texture(rc, n, target, world_area, clear, clear_color)` - from a render
callback, the effects hook or an overlay. It switches the draw list to the render texture
(`br_render_texture_create`, has stencil), draws `n` and its subtree (ignoring n's own effects,
so no recursion) mapping `world_area` onto the whole texture, then switches back. Then push a quad
with that texture. Outside the render phase use `br_render_to_texture(e, node, rt)`.

## Extension point 5: the effects hook (Task 8: filters, alpha/colour masks, cache, advanced blend)

`br_render_set_effects_hook(e, fn, user)` - one per engine. The walk calls `fn` instead of the
normal draw for every node where `br_node_needs_effects` is true (filters set, cache_as_texture,
`BR_MASK_ALPHA/COLOR`, blend `>= BR_BLEND_ADVANCED_FIRST`). Return true if you drew it.

```c
static bool demo_effects(br_engine *e, br_node n, br_render_ctx *rc, void *user)
{
    demo_state *s = user;
    if (!br_node_cache_as_texture(e, n)) {
        return false; /* not ours: draw normally */
    }
    br_rect b = br_node_get_bounds(e, n);
    if (br_rect_is_empty(b) || !br_render_texture_resize(e, s->effect_target, b.width, b.height)) {
        return true;
    }
    br_render_subtree_to_texture(rc, n, s->effect_target, b, true, br_color_make(0, 0, 0, 0));
    br_mat3 old = br_render_set_space(rc, br_mat3_identity()); /* b is in world coordinates */
    /* ... a quad over b with uvs 0..1 ... */
    br_render_push_quad(rc, s->effect_target, v);
    br_render_set_space(rc, old);
    return true;
}
```

## Extension point 6: overlays (debug UI)

`br_render_add_overlay(e, fn, user)`: `fn(e, rc, user)` runs after the stage every frame, in
window coordinates, same push functions. Overlays added later draw on top.

## Masks, layers, groups (built in)

- `BR_MASK_SCISSOR`: the mask node's world AABB clips (nested masks intersect).
- `BR_MASK_STENCIL`: the mask node's geometry (quads/triangles it pushes; sprite = its rectangle,
  alpha ignored) clips; nested and `inverse` supported (up to 255 levels).
- Render layer: `br_node_layer_attach(e, layer, n)` draws n where `layer` is in the order.
- Render group: children drawn with the group matrix on the GPU.

## Gotchas

- Textures are premultiplied on upload; blend modes assume premultiplied colour.
- Shaders: add `engine/shaders/<name>.vert/.frag.hlsl`, run
  `python tools/compile_shaders.py engine/shaders/<name>.vert.hlsl ...` (pass your files: the
  no-argument form rebuilds every header), include `<name>.<stage>.h` in your `.c`. No shared file
  changes. Fragment input must equal vertex output (SV_Position included).
- `br_render_pack_color(c, alpha)` premultiplies; do not premultiply twice.
- Callback `user` pointers must live until RENDER of the same frame.
- Headless engines skip collect entirely (no GPU).
- Stats for 100k rotating sprites (examples/sprites.c, RTX 3060 Ti): 31 batches, ~22-30 ms/frame
  in a Release build; per-node trig for rotation and the walk dominate.
