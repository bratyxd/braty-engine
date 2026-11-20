# camera - Unity-style cameras for 2D and 3D

Header: `engine/include/braty/br_camera.h`. Source: `engine/src/camera/`. Test: `tests/test_camera.c`.
Example: `examples/camera_demo.c` (`--size WxH`, `--mode3d`).

## The model

- A camera is a 2D node of type `"camera"` holding a plain `br_camera` (edit it in place with
  `br_camera_get`; the pointer dies when cameras are created, so re-fetch).
  - **2D** (`br_camera_create`): the node's world position / rotation is the centre / roll of what
    it shows. Put it under the stage and move it like any node (or let `camera.follow` do it).
  - **3D** (`br_camera_create_3d(e, scene_root, &desc)`): the same data drives a `camera3d` node
    and a View3D the module owns. Place it with `br_camera_node3d` (a rig; the camera3d is its
    child so shake does not fight you); post / environment through `br_camera_view3d`.
- **Unity semantics:** `orthographic_size` is half the visible *vertical* extent,
  `field_of_view` is vertical; the horizontal extent follows the viewport aspect (`aspect`
  overrides). 2D: `pixels_per_unit` stage units per world unit, so a 2D camera shows
  `2 * orthographic_size * pixels_per_unit` stage units vertically (default size 360, ppu 1 =
  the 720-unit default stage). 2D cameras are always orthographic.
- **Fit policies** (`fit`, with `reference_aspect`, default 16/9): `FIT_HEIGHT` (default),
  `FIT_WIDTH`, `FIT_EXPAND` (the design rect always fits), `FIT_LETTERBOX` (exactly the design
  rect, the viewport shrinks; `br_camera_create_logical(e, 1280, 720)` = a fixed logical
  resolution, API_REVIEW item 17).
- **No camera = no change**: the stage is drawn Pixi-style in window coordinates. As soon as an
  enabled 2D camera exists, the stage is drawn only through cameras (each in its
  `viewport_rect`, ordered by `depth`, render-texture cameras first). Only 3D cameras: the stage
  stays a screen-space overlay drawn after them.
- **Layers:** `br_node_set_layer_bits(e, n, bits)`; 0 (default) inherits the parent's, the stage
  is layer 1. A camera draws a node when `bits & culling_mask`; children are still visited, so a
  UI container with bits 2 puts its whole subtree on layer 2.
- **Screen** = window coordinates (top-left, y down), the space of `br_input`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_camera_default_2d/_3d`, `br_camera_create`, `br_camera_create_3d`, `br_camera_get` | Make and edit cameras. |
| `br_camera_screen_to_world`, `_world_to_screen`, `_screen_point_to_ray`, `_viewport_to_world` | Conversions (`z` = distance in front of a 3D camera). |
| `br_camera_get_view` / `br_camera_compute_view` + `br_camera_view_*` | The same maths, pure (tests, tools). |
| `br_node_set_layer_bits`, `culling_mask`, `depth`, `clear_flags`, `target_texture` | Split screen, minimap, UI over 3D. |
| `camera.follow` / `camera.shake` / `camera.orbit` / `camera.anchor` components (`br_camera_*_component`) | Helpers: data + one system each. |
| `br_camera_shake_add(e, cam, trauma)` | Shake (trauma squared, seeded from the engine RNG). |
| `br_camera_screen_to_stage`, `br_camera_stage_to_screen`, `br_camera_safe_area` | Pointer input with cameras, safe area. |

## Snippet (from examples/camera_demo.c, `make_follow_camera` and `build_ui`)

```c
br_camera c = br_camera_default_2d();
c.orthographic_size = 300.0f;                          /* 600 stage units tall at any window size */
c.viewport_rect = br_rect_make(viewport_x, 0.0f, 0.5f, 1.0f);
c.culling_mask = LAYER_WORLD;
c.background_color = hex(background);
br_node cam = br_camera_create(e, &c);
br_vec2 start = br_node_position(e, target);
br_node_set_position(e, cam, start.x, start.y);
br_node_add_child(e, br_stage_root(e), cam);
br_camera_follow *f = br_component_add(e, cam, br_camera_follow_component(e));
f->target = target;
f->damping = 0.15f;
f->dead_zone = br_vec2_make(20.0f, 20.0f);
f->bounds = br_rect_make(0.0f, 0.0f, (float)WORLD_W, (float)WORLD_H);
f->use_bounds = true;
```

A UI camera over everything, and a node anchored to its corner:

```c
br_camera c = br_camera_default_2d();          /* 720 UI units tall, centred on (640, 360) */
c.culling_mask = LAYER_UI;
c.depth = 10.0f;
c.clear_flags = BR_CAMERA_CLEAR_NOTHING;
s->ui_camera = br_camera_create(e, &c);
/* ... a panel node with br_node_set_layer_bits(e, n, LAYER_UI), then: */
br_camera_anchor *a = br_component_add(e, n, br_camera_anchor_component(e));
a->camera = s->ui_camera;
a->anchor = anchor;          /* (0,0) top-left .. (1,1) bottom-right of the viewport */
a->safe_area = true;
```

## Helpers

- `camera.follow`: 2D target node (+ offset), `damping` = seconds to close ~63% of the gap,
  `dead_zone` half size, `bounds` keep the visible rect inside. 3D: `target3d` + `offset3d`, the
  rig looks at the target. LATE_UPDATE 100.
- `camera.shake`: `trauma` 0..1 (decays by `decay` per second), offset/roll = trauma^2 *
  `max_offset` / `max_angle` * Perlin noise at `frequency`. Seed 0 is drawn from
  `br_random_engine` on first use: deterministic under `--deterministic`. LATE_UPDATE 200.
  `br_camera_shake_add` defaults: `max_offset` 12 stage units on a 2D camera, 0.3 world units on
  a 3D one; `max_angle` 0.05 rad.
- 3D `near_z <= 0` means 0.1 and `far_z <= near_z` (0 after `SDL_zero`) means `near_z + 1000`,
  in the view maths and in the camera3d the camera drives.
- `camera.orbit`: a `br_camera_orbit` (fill it with `br_camera_orbit_make`) driving a 3D camera's rig (left-drag, wheel). UPDATE 0.
- `camera.anchor`: puts a node at `anchor` of a 2D camera's viewport (or of the window when
  `camera` is zero), inside the safe area if asked, plus `offset`. PRE_RENDER -1050.
- `pixel_perfect` (2D): whole screen pixels per stage unit (zoom floored to an integer), camera
  position snapped to one screen pixel.

## Systems

`camera.views` (PRE_RENDER -1100) computes every camera's view, hands the stage views to the
renderer (`br_render_set_views`), sets the cull rect (`br_scene_set_cull_view`) and pushes 3D
settings into the camera3d nodes and Views3D.

## Gotchas

- Pointer events map every incoming window position through `br_camera_screen_to_stage`, and
  play-test `click` aims at `br_camera_stage_to_screen` of the node's bounds centre, so both work
  through zoomed / moved 2D cameras (`br_input_mouse_position` stays in window coordinates).
  `br_event` positions are stage coordinates.
- 3D cameras render through a View3D that is not on the stage: `br_node3d_on` listeners do not
  fire through them (the events hit test only walks the stage). Pick with
  `br_camera_screen_point_to_ray` + `br_view3d_raycast`, or use a View3D on the stage (as
  `canvas3d_demo` does).
- `background_color` is sRGB; for 3D cameras it is converted to the View3D's linear background.
  `CLEAR_DEPTH` and `CLEAR_NOTHING` both leave lower cameras visible (2D has no depth; a View3D
  always clears its own depth).
- A camera rendering into a texture shown by a sprite drawn by another camera: give them
  different layers, or the texture shows itself.
- Headless engines have no window: `br_camera_set_headless_screen(e, w, h)` lets tests and
  tools use cameras.
