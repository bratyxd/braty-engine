# sprites - nine-slice, tiling and animated sprites (+ GIF)

Header: `engine/include/braty/br_sprites.h`. Source: `engine/src/sprites/`, shader
`engine/shaders/tiling.frag.hlsl`. Example: `examples/sprites_demo.c`. Test: `tests/test_sprites.c`.

## What it does

- **Nine-slice** (`"nine_slice"` node type, PixiJS NineSliceSprite): four independent borders,
  any width/height, anchor; edges and centre each `BR_NINE_SLICE_STRETCH` or `_TILE`; optional
  hidden centre; atlas frames (trimmed and rotated). If the size is smaller than two opposite
  borders, both shrink proportionally (15 + 5 into 10 -> 7.5 + 2.5). Tint/alpha come from the node.
- **Tiling sprite** (`"tiling_sprite"`, PixiJS TilingSprite): tile position/scale/rotation, clamp
  margin. Wraps inside the texture frame in a shader, so atlas frames tile without REPEAT.
- **Animated sprite**: an ordinary `"sprite"` node plus the internal `"sprites.animated"`
  component. One system (`sprites.animated`, UPDATE 1000) advances every animation by iterating
  the dense component array. Frames carry their own duration in seconds; speed (negative =
  backwards), loop, play/stop/goto, callbacks on_complete / on_frame_change / on_loop,
  update-anchor. Every `br_sprite_*` call works on it.
- **GIF**: the `gif` loader (asset type `"frames"`) decodes with stb_image into a
  `br_frame_set` (one texture per frame, delays in seconds).

## Functions you actually need

| Function | What for |
|---|---|
| `br_nine_slice_create(e, tex, l, t, r, b)` | New nine-slice, sized like the texture. |
| `br_nine_slice_get(e, n)` | Edit fields in place: size, anchor, fills, hide_center. |
| `br_nine_slice_set_size` / `_set_borders` | Common setters. |
| `br_nine_slice_geometry(ns, info, out)` | The quads it draws (tests, custom drawing). |
| `br_tiling_sprite_create(e, tex, w, h)`, `br_tiling_sprite_get` | Tiling; edit tile_position etc. in place. |
| `br_animated_sprite_create(e, frames, n)` / `_create_uniform(e, textures, n, fps)` | Animated sprite (stopped). |
| `br_animated_sprite_play/stop/goto_and_play/goto_and_stop/set_speed/set_loop` | Playback. |
| `br_animated_sprite_set_callbacks(e, n, on_complete, on_frame_change, on_loop, user)` | Events. |
| `br_frame_set_load(e, "anim.gif")` | GIF -> frames (cached asset). |

## Snippets (from examples/sprites_demo.c)

```c
br_node n = br_nine_slice_create(e, s->atlas_panel, 12, 12, 12, 12);
br_nine_slice *atlas_ns = br_nine_slice_get(e, n);
atlas_ns->width = 200;
atlas_ns->height = 110;
atlas_ns->anchor = br_vec2_make(0.5f, 0.5f);
atlas_ns->edge_fill = BR_NINE_SLICE_TILE;
br_node_set_tint(e, n, br_color_from_hex(0xA0FFA0, 1.0f));
br_node_set_position(e, n, 1080, 80);
br_node_add_child(e, br_scene_root(e), n);
```

```c
const br_frame_set *gif = br_frame_set_load(e, "sprites_demo.gif");
if (gif != NULL) {
    br_node g = br_animated_sprite_create(e, gif->frames, gif->count);
    br_node_set_position(e, g, 340, 380);
    br_node_set_scale(e, g, 1.5f, 1.5f);
    br_node_add_child(e, br_scene_root(e), g);
    br_animated_sprite_play(e, g);
    br_log_info("sprites_demo: GIF %ux%u, %u frames, first delay %.2f s", gif->width, gif->height,
                gif->count, (double)gif->frames[0].duration);
}
```

## Gotchas

- Borders are in texture units of the *untrimmed* frame; trimmed padding produces no geometry.
- Tile fill repeats the source cell at the border scale; the last tile is cut. More than 256
  tiles per axis in one cell falls back to stretching (`ponytail:` in br_nine_slice.c).
- `br_nine_slice_get` / `br_tiling_sprite_get` pointers die when nodes of that type are created
  or freed: re-fetch, do not keep them.
- Tiling repeats the stored frame (trim padding is not repeated). One custom draw per tiling sprite.
- Animated sprites copy the frame list; the textures must outlive the sprite. A GIF's textures
  belong to its asset (`br_assets_unload(e, path)` frees them).
- Registering the `gif` loader means `br_texture_load("x.gif")` returns the magenta missing
  texture: load GIFs with `br_frame_set_load`.
- Callbacks run inside the system; destroying the sprite there is fine (deferred).
- Frames with duration <= 0 are skipped; GIF delays under 20 ms become 100 ms (browser rule).
- Animated sprites are created stopped: call `br_animated_sprite_play`.
