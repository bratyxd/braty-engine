# canvas3d - 2D content in the 3D world

Header: `engine/include/braty/br_canvas3d.h`. Source: `engine/src/canvas3d/`, shaders
`engine/shaders/canvas3d*.hlsl(i)`. Test: `tests/test_canvas3d.c`. Example: `examples/canvas3d_demo.c`.

## The model

- `canvas3d` is a 3D node type (registered through `br_node3d_type_register` like `mesh3d`). Each
  canvas owns a detached 2D container, `br_canvas3d_root(e, n)`: put sprites, nine-slice,
  graphics, text, buttons under it as on the stage.
- **Geometry mode** (default): during the 3D collect the subtree is walked by the normal 2D
  renderer, its commands are captured (`br_render_capture_subtree`, engine-internal) and
  replayed as one custom draw item in the view's **transparent** pass (back to front with the
  other transparents), or in the **opaque** pass with depth write when `alpha_cutoff > 0`
  (alpha-tested). Depth tested, sRGB -> linear, `tint`, optional `lit` (view ambient + lights,
  Lambert, no shadows). Batches and SDF text are drawn; other custom 2D draws (tiling sprites)
  and filters/masks need texture mode.
- **Texture mode**: the subtree is rendered into a render texture every frame
  (`resolution` pixels per 2D unit) and that texture is drawn on the plane. Filters and masks
  work (put them on children of the root: the root's own effects are skipped).
- **Plane:** 2D x right / y down -> local +X / -Y, the canvas faces +Z; `pixels_per_unit` 2D
  units per world unit; `pivot` (0..1 of `width` x `height`) sits on the node.
  `billboard`: `NONE`, `FULL` (camera's rotation), `Y_AXIS` (turns about world Y).
  `double_sided = false` hides the back.
- **Events:** a 3D pick on the canvas is mapped onto the plane, hit-tested against the subtree
  (event modes, hit areas, type hit tests) and dispatched as a normal 2D event
  (`br_events_dispatch`), so `br_node_on(button, BR_EVENT_CLICK, ...)` inside a canvas works.

## Functions you actually need

| Function | What for |
|---|---|
| `br_canvas3d_default(w, h)`, `br_canvas3d_create(e, &desc)`, `br_canvas3d_get` | A canvas; edit settings in place. |
| `br_canvas3d_root(e, n)`, `br_canvas3d_content(e, n)` | Its 2D container / first child. |
| `br_sprite3d_create`, `br_text3d_create`, `br_nine_slice3d_create` | A canvas with one child, sized to it. |
| `br_canvas3d_sdf_font(e)` | The default font as SDF: text sharp at any distance. |
| `br_canvas3d_plane_matrix`, `br_canvas3d_ray_to_local`, `br_canvas3d_billboard_matrix` | The maths (pure). |

## Snippet (from examples/canvas3d_demo.c, `build_scene`)

```c
br_node3d plate = br_nine_slice3d_create(e, make_panel_texture(e), 12, 12, 12, 12, 240.0f, 64.0f, 200.0f);
br_canvas3d_get(e, plate)->billboard = BR_CANVAS3D_BILLBOARD_FULL;
br_text_style name = sdf_style(e, 34.0f, 0xF1FAEE);
add_text(e, br_canvas3d_root(e, plate), "Hero  Lv 7", &name, 120.0f, 32.0f);
br_node3d_set_position(e, plate, br_vec3_make(-3.2f, 1.45f, 0.5f));
br_node3d_add_child(e, s->root, plate);
```

(`sdf_style` = `br_text_style_default()` with `font = br_canvas3d_sdf_font(e)`; `add_text` =
`br_text_create` + anchor 0.5 + position + `br_node_add_child`.) A button in the world is a
STATIC container under a canvas root with `br_node_on(button, BR_EVENT_CLICK, fn, user)`; the
demo clicks it with `br_input_inject_pointer_*` at `br_view3d_world_to_screen` of the canvas.

## Gotchas

- Use an SDF font for text in geometry mode (`br_text3d_create` does when `style.font` is NULL
  and no family is set); coverage glyphs are rasterised at `resolution` and blur up close.
- Do not transform the root container: 2D event coordinates assume it sits at the origin.
- Billboards are computed per view at collect time; picking uses the last drawn orientation.
- Canvases do not cast shadows; lit canvases get no shadows either.
- Colours and the tint are sRGB like all 2D content; the view's tone map still applies (ACES by
  default), so pure UI colours look slightly different than on the stage.
- Hit tests inside a canvas ignore z-index, render layers and masks (`ponytail:` in the source).
