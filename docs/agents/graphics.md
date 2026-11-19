# graphics - vector shapes, fills, strokes, SVG paths (PixiJS Graphics / GraphicsContext)

Header: `engine/include/braty/br_graphics.h`. Source: `engine/src/graphics/`
(`br_graphics.c` = contexts, paths, shapes, styles, SVG parser, node type;
`br_graphics_build.c` = ear clipping, strokes, dashes, geometry cache).
Test: `tests/test_graphics.c`. Example: `examples/graphics_demo.c` (every feature in a grid).

## The model

- A `br_graphics_context` records instructions: build a path, then `fill` / `stroke` it.
  Shape calls (`rect`, `circle`, `star`, ...) add closed sub-paths; `move_to/line_to/...` build
  free paths. As in Pixi v8, fill/stroke keep the path (`rect` + `fill` + `stroke` paints the
  same rect twice); the next path call after them starts a fresh path.
- A `"graphics"` node draws one context. Many nodes may share one context: it is triangulated
  once per change and every node reuses the cached triangles with its own transform and tint.
- Contexts are reference counted: `br_graphics_context_create` gives you one reference,
  each node using it holds one. `br_graphics_context_destroy` drops yours.
- Output goes through the 2D batcher (`br_render_push_triangles`), so graphics batch with
  sprites. Gradients are baked into small textures (owned by the context), so they batch too.

## Functions you actually need

| Function | What for |
|---|---|
| `br_graphics_create(e, ctx)` | New graphics node; `ctx = NULL` gives it its own context. |
| `br_graphics_context_of(e, node)` | The node's context: draw into it. |
| `br_graphics_context_create(e)` / `_destroy(ctx)` | A shareable context. |
| `br_graphics_rect/round_rect/circle/ellipse/poly/regular_poly/round_poly/star/chamfer_rect/fillet_rect` | Shapes. |
| `br_graphics_move_to/line_to/bezier_curve_to/quadratic_curve_to/arc/arc_to/arc_to_svg/close_path` | Paths. |
| `br_graphics_svg_path(ctx, "M0 0 L ...")` | SVG path data (all commands). False + log on syntax errors. |
| `br_graphics_fill_color(ctx, c)`, `br_graphics_fill(ctx, &style)` | Fill the current path. |
| `br_graphics_stroke(ctx, &st)` with `st = br_graphics_stroke_style(width, c)` | Stroke it. |
| `br_graphics_cut(ctx)` | Current path becomes holes in the previous fill (and its stroke). |
| `br_graphics_style_linear/radial/pattern` + `br_graphics_style_add_stop` | Gradient and texture fills. |
| `br_graphics_save/restore/translate/rotate/scale/set_transform` | Transform stack. |
| `br_graphics_texture(ctx, tex, tint, x, y, w, h)` | Draw a texture as part of the graphics. |
| `br_graphics_clear(ctx)` | Start over. |

## Snippet (from examples/graphics_demo.c: one context shared by two nodes)

```c
br_graphics_context *shared = br_graphics_context_create(e);
br_graphics_star(shared, 0, 0, 5, 50, 20, 0);
br_graphics_fill_color(shared, hex(0xffffff));
br_graphics_circle(shared, 0, 0, 12);
br_graphics_fill_color(shared, hex(0xf72585));
br_node a = br_graphics_create(e, shared);
br_node b = br_graphics_create(e, shared);
br_graphics_context_destroy(shared); /* the nodes keep it alive */
```

Holes, gradient, dashed stroke (also from the example):

```c
br_graphics_rect(g, 8, 8, 120, 104);
br_graphics_fill_color(g, hex(0x80ed99));
br_graphics_circle(g, 46, 60, 24);
br_graphics_rect(g, 84, 24, 30, 30);
br_graphics_star(g, 100, 88, 5, 16, 7, 0);
br_graphics_cut(g);

br_fill_style lin = br_graphics_style_linear(BR_GRAPHICS_SPACE_LOCAL, 0, 0, 1, 0);
br_graphics_style_add_stop(&lin, 0.0f, hex(0xff006e));
br_graphics_style_add_stop(&lin, 0.5f, hex(0xffbe0b));
br_graphics_style_add_stop(&lin, 1.0f, hex(0x3a86ff));
br_graphics_round_rect(g, 8, 8, 120, 104, 12);
br_graphics_fill(g, &lin);

br_stroke_style dc = br_graphics_stroke_style(6.0f, hex(0xffbe0b));
dc.dash_count = 2;
dc.dash[0] = 10;
dc.dash[1] = 8;
dc.cap = BR_GRAPHICS_CAP_ROUND;
br_graphics_stroke(g, &dc);
```

## Styles

- `br_fill_style.kind`: colour, linear, radial, pattern. `color` multiplies gradients and
  patterns (white = unchanged); its alpha is the opacity.
- Gradient `space`: `LOCAL` (default) = 0..1 across the shape's bounding box, `GLOBAL` = context
  coordinates. Radial gradients are concentric (`inner_radius` -> offset 0, `outer_radius` -> 1).
- Pattern: `matrix` maps texture pixels to context space. The texture's address mode is switched
  to repeat; it must be a whole source texture, not an atlas frame.
- Stroke: `width`, `alignment` (0 inside, 0.5 centred, 1 outside; closed paths only), `cap`
  (butt/round/square), `join` (miter/bevel/round), `miter_limit` (default 10), `dash` / `dash_count`
  / `dash_offset`, `pixel_line` (exactly 1 pixel at any scale, drawn as GPU lines). Stroke `fill`
  can be any fill style (gradient strokes work).

## Scene JSON

None yet: the node type has no `from_json` (scene files are Task 10; it can be added with
`br_node_type_set_from_json` - e.g. an `"svg"` string plus fill/stroke).

## Gotchas

- Sub-paths of one fill are filled independently (overlaps are not subtracted). Use `cut` for holes;
  a hole belongs to the shape that contains its first point.
- Points are transformed by the transform stack when added, and curves are flattened then
  (tessellation density follows the context's scale, not the node's: a node scaled up a lot shows
  facets).
- `hit_test` (and `br_graphics_context_contains`) test the cached triangles, so holes and strokes
  count exactly as drawn; pixel lines are not hittable.
- Pixel lines are a custom draw (line list): they break the batch and ignore pattern/gradient
  fills (colour only).
- Geometry over 65535 vertices is split into several batches automatically.
- Gradient textures are owned by the context: `br_graphics_clear` and destroying the last reference
  free them. Identical gradients in one context share a texture.
