# math - vectors, matrices, quaternions, shapes, colour

Header: `engine/include/braty/br_math.h`. Source: `engine/src/math/`. Test: `tests/test_math.c`.

All value types, all functions take and return by value. Nothing allocates.

## Conventions (read these first)

- 2D is screen space: x right, **y down**, radians, positive rotation = clockwise on screen.
- `br_mat3` is PixiJS `Matrix` (`a b c d tx ty`). `br_mat3_from_transform` is Pixi
  `setTransform` (position, pivot, scale, rotation, skew). `br_mat3_mul(m, o)` applies `o` first.
- `br_mat4` is column-major (`m[col * 4 + row]`), `v' = M * v`, upload as-is to HLSL and use
  `mul(matrix, vector)`. Clip space is SDL_GPU's: y up, depth 0..1. 3D is right-handed.
- For 2D drawing in window units: `br_mat4_ortho(0, width, height, 0, -1, 1)`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_mat3_from_transform(x, y, px, py, sx, sy, rot, skx, sky)` | Node local transform. |
| `br_mat3_mul`, `br_mat3_invert`, `br_mat3_apply` | Parent * local, to-local, point transform. |
| `br_mat4_ortho`, `br_mat4_perspective`, `br_mat4_look_at` | Projection / view. |
| `br_mat4_from_trs`, `br_quat_from_axis_angle`, `br_quat_slerp` | 3D transforms. |
| `br_rect_contains`, `br_polygon_contains`, ... `_bounds` | Hit areas and bounds (Pixi shapes). |
| `br_segment_intersect`, `br_line_intersect` | Geometry helpers. |
| `br_color_parse("#ff8800")`, `br_color_from_hex(0xff8800, 1)` | Colours from data files. |
| `br_color_to_rgba8`, `br_color_premultiply` | Vertex colours, blending. |

## Snippet (from tests/test_math.c)

```c
br_mat3 t = br_mat3_from_transform(100.0f, 50.0f, 8.0f, 4.0f, 2.0f, 3.0f, 0.7f, 0.0f, 0.0f);
br_vec2 world = br_mat3_apply(t, br_vec2_make(8.0f, 4.0f));   /* pivot lands on (100, 50) */

br_color c;
if (br_color_parse("CornflowerBlue", &c)) {          /* also #rgb, #rrggbbaa, 0x..., rgb(), rgba() */
    uint32_t vertex_color = br_color_to_rgba8(c);
}
```

## Gotchas

- `br_rect_contains` is right/bottom exclusive (Pixi).
- `br_polygon` borrows its points; it does not own or copy them.
- `br_color` is straight alpha; premultiply before blending with the premultiplied blend state.
- Singular matrices invert to identity and report `ok = false`.
