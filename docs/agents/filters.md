# filters - filters, alpha/colour masks, cache_as_texture, advanced blend modes, extract

Headers: `engine/include/braty/br_filter.h` (filters, pool, cache, blend modes), `br_mask.h`
(alpha/colour masks), `br_extract.h` (generate texture, read pixels). Source: `engine/src/filters/`,
shaders `engine/shaders/filter*.hlsl`. Example: `examples/filters_demo.c` (every snippet below is
from it). Test: `tests/test_filters.c`.

## How it works

The module installs the render effects hook. For a node with filters, an alpha/colour mask, an
advanced blend mode or `cache_as_texture`, during collect it:

1. draws the subtree into a pooled render texture covering the node's bounds + filter padding,
   clipped to the current target and snapped to its pixels (`br_render_subtree_to_texture`);
2. draws the mask node (if any) into a second pooled texture over the same area;
3. records the passes (each filter = 1+ full-texture passes ping-ponging between two pooled
   textures, then the mask pass, then a copy of the target under the node for advanced blends)
   and pushes ONE own-passes callback that runs them in RENDER;
4. draws the result back as a quad with the node's blend mode (or with the blend shader).

Nested filters work (a filtered child is drawn into its parent's texture). Pooled textures are
power-of-two buckets, freed at the start of every frame, destroyed when idle 120 frames and all
destroyed when the window pixel size changes.

## Functions you actually need

| Function | What for |
|---|---|
| `br_filter_alpha/blur/color_matrix/displacement/noise/custom(...)` | Make a filter value (plain struct, edit fields freely). |
| `br_filters_set(e, n, filters, count)` | Give n a filter list (copied; count 0 removes). |
| `br_filters_get(e, n, i)` | Edit a filter in place (animate blur, matrix, uniforms). |
| `br_color_matrix_<preset>(m, ..., multiply)` | Pixi ColorMatrixFilter presets on `filter.matrix`. |
| `br_filters_set_resolution(e, n, r)` / `br_filters_set_area(e, n, &local_rect)` | Cheaper filters / fixed filter area. |
| `br_node_set_mask(e, n, mask, BR_MASK_ALPHA or BR_MASK_COLOR, inverse)` | Alpha / colour masks. |
| `br_node_set_cache_as_texture(e, n, true)`, `br_cache_update_texture(e, n)` | Render once, reuse. |
| `br_node_set_blend_mode(e, n, BR_BLEND_OVERLAY)` | Any of the 21 advanced modes (`br_blend_mode`, `br_node.h`). |
| `br_generate_texture(e, n, opts)`, `br_extract_pixels`, `br_extract_image_png` | Node -> texture -> pixels/PNG. |
| `br_filters_pool_acquire(e, w, h)` | A pooled render texture for your own effects (free next frame). |

## Snippets (from examples/filters_demo.c)

```c
/* two chained filters */
br_filter chain[2];
chain[0] = br_filter_blur(4.0f, 3, 7);
chain[1] = br_filter_color_matrix();
br_color_matrix_hue(chain[1].matrix, 90.0f, false);
br_filters_set(e, make_cell_card(e, d, 11), chain, 2);

/* alpha mask: any node (here a soft circle sprite, a child of the masked card) */
c = make_cell_card(e, d, 13);
br_node mask = add_sprite(e, c, d->soft_circle, 8.0f, 8.0f, 80.0f, 80.0f);
br_node_set_mask(e, c, mask, BR_MASK_ALPHA, false);

/* node -> texture (filled during this frame's render, usable from the next frame) */
d->generated_texture = br_generate_texture(e, d->first_card, NULL);
br_sprite_set_texture(e, d->generated, d->generated_texture);
br_sprite_set_size(e, d->generated, 120.0f, 120.0f);
```

## Write your own filter (recipe)

1. Write `engine/shaders/<name>.frag.hlsl`. Include `filter_common.hlsli` (gives `u_input` /
   `u_input_sampler` = t0/s0 and the input struct `FilterVaryings`), declare ONE cbuffer at
   `register(b0, space3)` that starts with the four globals, then your fields:

   ```hlsl
   #include "filter_common.hlsli"
   cbuffer Filter : register(b0, space3)
   {
       float4 u_input_size; float4 u_content; float4 u_area; float4 u_params; // fixed, in this order
       float4 u_wave;  // your uniforms: br_filter.uniforms starts here
       float4 u_tint;
   };
   float4 main(FilterVaryings input) : SV_Target0 { ... return premultiplied colour; }
   ```
   Extra textures go in `t1..t4` / `s1..s4` (space2) with `[[vk::combinedImageSampler]]`.
2. `python tools/compile_shaders.py engine/shaders/<name>.frag.hlsl`, include `<name>.frag.h`.
3. Make the filter with a C struct matching your fields (16-byte aligned rows):

```c
wave_uniforms wave = { { 6.0f, 2.0f, 3.0f, 0.0f }, { 0.7f, 1.0f, 1.0f, 1.0f } };
f = br_filter_custom(&br_shader_filter_wave_frag, &wave, sizeof(wave));
f.padding = 8.0f;
set_one_filter(e, make_cell_card(e, d, 17), f);
```
   (`engine/shaders/filter_wave.frag.hlsl` is this example.) Extra textures:
   `f.textures[0] = tex; f.texture_count = 1;`. To animate, write into
   `br_filters_get(e, n, i)->uniforms` each frame, or read `u_params.x` (elapsed seconds).

Fixed inputs of every pass: `input.uv` = input texture uv (content occupies uv
`0..u_content.zw`, the rest is transparent), `input.local` = 0..1 across the filter area,
`u_input_size` = (w, h, 1/w, 1/h) pixels, `u_area` = world rect, `u_params.y` = pixels per world
unit (convert world-unit strengths to pixels with it). Input is premultiplied; output must be too.
Blending is off: you write the final pixel. The vertex shader is the engine's (`filter.vert.hlsl`).

## Gotchas

- Filter strengths (blur, displacement scale, padding) are in world units; they do not scale
  with the node.
- Content outside the filter area is cut: give spreading filters padding (blur sets
  `padding = strength`; custom filters must set it themselves).
- Areas are clipped to the current target, so a blur at the screen edge does not see pixels
  beyond it (same as Pixi).
- Mask order: filters first, then the mask, then the blend mode.
- The mask node is not drawn as content; it may live outside the stage. The render module was
  changed so `br_render_subtree_to_texture` draws a mask node when it is the top node.
- `cache_as_texture` alone caches the content without the node's (and its ancestors') alpha and
  tint and redraws it with the node's current transform and colour: moving, fading and tinting
  are free; content changes need `br_cache_update_texture`. Combined with filters/masks/blend it
  only caches the content (colour baked in: a fade needs `br_cache_update_texture`) and
  re-renders it when the bounds move.
- Known limit: a cached node drawn by two views (cameras) of different zoom re-renders its cache
  twice per frame (the cache keys on pixel size). Inside a geometry-mode canvas3d no effect runs
  (one logged error; `docs/agents/canvas3d.md`).
- Advanced blend backdrops are copied from the target pixels under the node, viewport offset and
  view zoom included (`br_render_rect_to_pixels`); a rotated view copies the bounding box,
  which does not line up (`ponytail:` in br_filters.c).
- Advanced blend modes copy the target under the node every frame: one extra blit + texture each.
- The effects hook is one per engine; an app that calls `br_render_set_effects_hook` itself
  turns this module's filters off unless its hook chains the previous one (examples/sprites.c
  does, see render.md).
- `br_extract_*` block on the GPU; call them one frame after the content was drawn. Headless: false.
