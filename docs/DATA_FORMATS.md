# Braty data formats

Everything a designer tweaks is JSON. This file documents every format the engine reads:

1. [game.json](#1-gamejson) - window, asset root, start scene, ticker, input, audio, debug, profiler
2. [Scene files](#2-scene-files-scenejson) (`*.scene.json`) - node trees
3. [Prefabs](#3-prefabs-prefabjson) (`*.prefab.json`) - reusable node trees with overrides
4. [Values](#4-values-used-everywhere) shared by all of them: colours, vectors, textures, enums
5. [Formats owned by other modules](#5-formats-owned-by-other-modules) - asset manifests,
   spritesheets, bitmap fonts, input actions, glTF, play-test scripts
6. [Animation clips](#6-animation-clips-animjson) (`*.anim.json`)
7. [Animation state machines](#7-animation-state-machines-animgraphjson) (`*.animgraph.json`)

Every JSON example below is loaded by `tests/test_data_formats.c` (the `<!-- check: ... -->`
line before each one says how), so they stay valid.

Paths inside data files are asset paths: forward slashes, relative to the asset root,
case-sensitive. Errors never stop a load: each is logged as `file: json.path: reason`
(`level.scene.json: children[2].components.velocity.x: expected a number`), the bad value is
skipped (a bad node with its subtree), and the rest loads. Unknown keys (typos like
`"posiiton"`) log a warning at their JSON path; they are checked for game.json and for the
built-in node types and text styles, not for your own node types.

A working set of all three files: `examples/data/` (`game.json`, `demo.scene.json`,
`coin.prefab.json`), run by `examples/data_demo.c`.

---

## 1. game.json

Read in `br_main`, before an engine exists (the path is relative to the executable directory).
`br_game_config_main("game.json", argc, argv, init)` does it all: the engine applies the file
before `init` and loads `start_scene` after `init`. (The step-by-step form is
`br_game_config_load` / `_args` / `_app_desc` in `br_main` and `br_game_config_apply` in init.)
Every key is optional; the values below are the defaults unless noted.

<!-- check: game -->
```json
{
  "window": { "title": "My Game", "width": 1280, "height": 720,
              "fullscreen": false, "vsync": true, "resizable": true, "logical_size": [1280, 720] },
  "asset_root": "assets",
  "manifest": "manifest.json",
  "start_scene": "title",
  "scenes": { "title": "scenes/title.scene.json", "level1": "scenes/level1.scene.json" },
  "clear_color": "#1b1e2b",
  "ticker": { "max_fps": 0, "min_fps": 10, "speed": 1, "fixed_hz": 0 },
  "input": {
    "jump": { "keys": ["Space"], "buttons": ["a"], "threshold": 0.5, "context": "gameplay" },
    "left": { "keys": ["Left", "A"], "axes": ["-leftx"] }
  },
  "audio": {
    "master_volume": 1,
    "muted": false,
    "buses": { "music": 0.5, "sfx": { "volume": 1, "muted": false } },
    "sprites": { "sounds/sfx.wav": { "jump": [0.0, 0.25], "coin": [0.3, 0.2] } }
  },
  "debug": { "overlay": false, "toggle_key": "F1", "hot_reload": true },
  "profiler": { "hud": false, "panel": false },
  "save": { "org": "my-studio", "app": "My Game" },
  "game": { "lives": 3 }
}
```

Defaults: title `"braty"`, 1280 x 720, not fullscreen, vsync on, resizable, asset root = the
executable directory, no manifest, no start scene, the GPU module's clear colour, ticker 0 / 10 / 1,
overlay closed, F1, hot reload on in debug builds, no logical size (the example's
`logical_size` is not a default), profiler HUD and panel off.

| Key | Meaning |
|---|---|
| `window.*` | Window title and size (in `br_app_desc`), fullscreen / resizable (applied). |
| `window.logical_size` | `[width, height]`: a fixed design resolution. Applied as `br_camera_create_logical(e, w, h)`, a letterbox 2D camera under the stage: the stage is always w x h units, scaled to fit the window with bars, and pointer events map through it (`docs/agents/camera.md`). Absent = no camera, the stage is the window. |
| `window.vsync` | `true` / `false`, or `"on"`, `"off"`, `"mailbox"`, `"adaptive"` (`br_app_set_vsync`; unsupported modes fall back with a log line; SDL GPU has no adaptive mode, it becomes on). |
| `asset_root` | Directory of all other paths, relative to the executable directory. |
| `manifest` | Added with `br_assets_load_manifest` (aliases and bundles). |
| `start_scene` | A `"scenes"` key or a path; loaded as a `br_scene` (single) after `init` (`br_game_config_main`), or by `br_game_config_apply`, which returns the file's root. |
| `scenes` | Names for scene files: `br_scene_load(e, "level1", ...)`, `br_scene_switch(e, "level1", ...)` (`docs/agents/scenes.md`). |
| `clear_color` | Window clear colour, any colour string (section 4). |
| `ticker` | `max_fps`: FPS cap, 0 = uncapped, works with vsync off (`br_app_set_max_fps`); `min_fps`: dt clamp; `speed`: time scale; `fixed_hz`: rate of `BR_PHASE_FIXED_UPDATE`, 0 = once per frame (`br_app_set_fixed_timestep`). |
| `input` | Named actions, the `br_input_actions_from_json` format (section 5). |
| `audio` | Master volume/mute, named buses (a number = volume), sound sprites per file (`[start, duration]` seconds). |
| `debug.overlay` | Debug overlay open at start (`--debug` on the command line does the same). |
| `debug.toggle_key` | SDL key name toggling the overlay (`"F1"`, `"F12"`, `"Grave"`). |
| `debug.hot_reload` | Poll loaded scene files and reload them when they change (default: on in debug builds, off with `NDEBUG`). |
| `profiler.hud` / `profiler.panel` | Profiler corner readout / panel (F2) shown at start (`br_profiler_set_hud`, `br_profiler_set_open`). `false` leaves the command line (`--profiler-hud`, `--profiler`) in charge. |
| `save.org` / `save.app` | Save-data folder (`br_engine_set_save_name`): default org `"braty"` and the window title. Sanitised (characters a folder name cannot hold become `_`), so renaming or localising the title does not move saves once `save.app` is set. |
| `game` | Anything the game wants: `br_game_config_json(e)` returns the whole document. Other unknown keys log a warning. |

Command line (`br_game_config_args`): `--debug` opens the overlay, `--scene path` replaces
`start_scene`. The engine's own flags override game.json: `--vsync MODE`, `--max-fps N`,
`--assets DIR` (or `BRATY_ASSETS`; replaces `asset_root`, relative to the working directory).

Bad values are logged as `game.json: window.width: expected a positive number` and keep their
default; a syntax error keeps every default and makes `br_game_config_load` return false.

---

## 2. Scene files (`*.scene.json`)

One node object; children nest. `br_scene_load(e, path_or_name, mode)` loads it as a scene
(its own root, assets and lifetime, `docs/agents/scenes.md`); `br_scene_file_load(e, path,
parent)` builds it under any `parent` and returns its root. Either way the references the build
takes belong to the scene (or that root) and are released with it. The root may list `"assets"`
to load before anything else:

<!-- check: fields -->
```json
"assets": [ "atlas.json", "music",
            { "path": "big/boss.png", "load": "lazy" },
            { "bundle": "level1_sounds", "load": "preload" } ]
```

An entry is an asset path, a manifest bundle name (all its assets), or an object with `path` or
`bundle` and `load`: `preload` (default, loaded before the nodes) or `lazy` (a `br_scene` lists it
in its group and loads it on `br_asset_group_get`; a plain file load skips it). A `br_scene` also
collects every path its nodes, prefabs and components name (textures, spritesheets, fonts, glTF
`source`, `map`, `BR_FIELD_ASSET` fields such as sounds) and loads those first too.

<!-- check: scene -->
```json
{
  "type": "container",
  "label": "level1",
  "assets": ["atlas.json", "fonts/pixel.fnt"],
  "children": [
    { "type": "sprite", "label": "hero", "texture": "atlas.json#hero.png",
      "position": [100, 200], "anchor": [0.5, 0.5], "tint": "#ffcc00",
      "components": { "velocity": { "x": 40, "y": 0 }, "target": { "node": "door" } } },
    { "type": "graphics", "label": "door", "position": [600, 180],
      "commands": [["rect", 0, 0, 40, 80], ["fill", "saddlebrown"]] },
    { "prefab": "prefabs/coin.prefab.json", "position": [300, 200] }
  ]
}
```

### 2.1 Fields every node has (Container)

| Key | Value | Notes |
|---|---|---|
| `type` | node type name | default `"container"`; table 2.3 |
| `label` | string | used by `br_node_find_by_label` and by references |
| `position`, `scale`, `skew`, `pivot`, `origin` | vec2 | `scale: 2` = `[2, 2]` |
| `rotation` | radians | clockwise on screen |
| `angle` | degrees | alternative to `rotation` |
| `alpha` | 0..1 | |
| `tint` | colour | multiplied down the tree |
| `visible`, `renderable` | bool | |
| `z_index` | int | with the parent's `sortable_children: true` |
| `sortable_children`, `cullable`, `cullable_children`, `render_group`, `cache_as_texture` | bool | |
| `cull_area` | `[x, y, w, h]` | local rect used for culling |
| `blend` | name | `normal add multiply screen erase none normal_npm add_npm screen_npm` and the advanced modes `color color_burn color_dodge darken difference divide exclusion hard_light hard_mix lighten linear_burn linear_dodge linear_light luminosity negation overlay pin_light saturation soft_light subtract vivid_light` |
| `mask` | label or `{ "node": label, "kind": "stencil", "inverse": false }` | kind `scissor stencil alpha color`; the mask node is not drawn as content |
| `layer` | label | draw this node where the labelled render layer is (`br_node_layer_attach`) |
| `filters` | array of filters | section 2.4 |
| `filter_resolution` | number | `br_filters_set_resolution` |
| `filter_area` | `[x, y, w, h]` | `br_filters_set_area` |
| `event_mode` | name | `passive none auto static dynamic` |
| `interactive_children` | bool | |
| `cursor` | name | `none default text wait crosshair progress move not_allowed pointer ew_resize ns_resize nwse_resize nesw_resize` |
| `hit_area` | one of `{ "rect": [x,y,w,h] }`, `{ "circle": [x,y,r] }`, `{ "ellipse": [x,y,hw,hh] }`, `{ "rounded_rect": [x,y,w,h,r] }`, `{ "triangle": [x1,y1,x2,y2,x3,y3] }`, `{ "polygon": [x,y, ...] }` | |
| `on` | `{ "click": "handler_name", ... }` | handlers registered with `br_scene_file_add_handler`; events `pointerdown pointerup pointerupoutside pointermove pointerover pointerout pointerenter pointerleave tap click rightclick wheel globalpointermove` |
| `components` | `{ "name": { "field": value } }` | section 2.5 |
| `children` | array of nodes | |
| `assets` | array of paths / bundle names / `{ path or bundle, load }` | loaded before the node's other fields (`lazy` entries are not) |
| `prefab` | prefab path | the node is a prefab instance; other keys override it (section 3) |

Labels referenced by `mask`, `layer` and node component fields are resolved after the whole file
is built (so they may point forward), inside the loaded tree first, then the whole stage.

### 2.2 Type-specific fields

**sprite**

<!-- check: scene -->
```json
{ "type": "sprite", "texture": "atlas.json#hero.png", "anchor": [0.5, 1], "round_pixels": true,
  "width": 64, "height": 64 }
```
`anchor` defaults to the frame's pivot from the spritesheet. `width`/`height` set the scale.

**animated_sprite** (a sprite with frames). Frames from one of: a spritesheet animation, a GIF,
or a list (texture references or `{ "texture", "duration" }` in seconds).

<!-- check: scene -->
```json
{ "type": "animated_sprite", "animation": "atlas.json#walk", "fps": 12, "speed": 1, "loop": true,
  "playing": true, "frame": 0, "update_anchor": false, "anchor": [0.5, 0.5] }
{ "type": "animated_sprite", "gif": "fx/explosion.gif", "loop": false }
{ "type": "animated_sprite", "frames": ["a.png", { "texture": "b.png", "duration": 0.5 }], "fps": 10 }
```
Data-driven animations play unless `"playing": false`.

**nine_slice** (also `nine_slice_sprite`)

<!-- check: scene -->
```json
{ "type": "nine_slice", "texture": "atlas.json#panel.png", "borders": [8, 8, 8, 8],
  "width": 200, "height": 120, "anchor": [0, 0], "edge_fill": "tile", "center_fill": "stretch",
  "hide_center": false }
```
`borders` = left, top, right, bottom in texture units; without it a spritesheet frame's
`borders` are used. Fills: `stretch` or `tile`.

**tiling_sprite**

<!-- check: scene -->
```json
{ "type": "tiling_sprite", "texture": "bg/stars.png", "width": 1280, "height": 720, "anchor": [0, 0],
  "tile_position": [0, 0], "tile_scale": [1, 1], "tile_rotation": 0, "clamp_margin": 0.5 }
```

**graphics** - `commands` mirrors the `br_graphics_*` calls, one array per call:

<!-- check: scene -->
```json
{ "type": "graphics", "commands": [
    ["move_to", 0, 0], ["line_to", 100, 0], ["bezier_curve_to", 120, 0, 140, 20, 140, 40],
    ["quadratic_curve_to", 140, 80, 100, 80], ["arc", 50, 50, 20, 0, 3.14, false],
    ["arc_to", 10, 10, 50, 10, 8], ["arc_to_svg", 20, 10, 0, false, true, 80, 40], ["close_path"],
    ["svg_path", "M 0 0 L 10 10 Z"],
    ["rect", 0, 0, 100, 50], ["round_rect", 0, 0, 100, 50, 8], ["circle", 50, 50, 20],
    ["ellipse", 50, 50, 30, 20], ["poly", [0, 0, 50, 0, 25, 40], true],
    ["regular_poly", 50, 50, 30, 6, 0], ["round_poly", 50, 50, 30, 6, 5, 0],
    ["star", 50, 50, 5, 30, 15, 0], ["chamfer_rect", 0, 0, 80, 40, 6], ["fillet_rect", 0, 0, 80, 40, -6],
    ["fill", "#ff3355"],
    ["fill", { "linear": [0, 0, 1, 1], "space": "local", "stops": [[0, "white"], [1, "#3355ff"]] }],
    ["fill", { "radial": [0.5, 0.5, 0, 0.5], "stops": [[0, "yellow"], [1, "orange"]], "alpha": 0.8 }],
    ["fill", { "texture": "bricks.png", "matrix": [1, 0, 0, 1, 0, 0] }],
    ["stroke", { "width": 4, "color": "white", "alignment": 0.5, "cap": "round", "join": "miter",
                 "miter_limit": 10, "pixel_line": false, "dash": [10, 5], "dash_offset": 0 }],
    ["cut"],
    ["texture", "icon.png", "white", 0, 0, 32, 32],
    ["save"], ["translate", 10, 10], ["rotate", 0.5], ["scale", 2, 2], ["restore"],
    ["set_transform", 1, 0, 0, 1, 0, 0] ] }
```
A fill style is a colour or an object with `linear` `[x0,y0,x1,y1]` / `radial`
`[cx,cy,inner,outer]` + `stops` `[[offset, colour], ...]` + `space` (`local` = 0..1 over the
shape's box, `global`), or `texture` + `matrix` `[a,b,c,d,tx,ty]`; plus `color` (tint) and `alpha`.
A stroke style is a colour (1 unit wide) or an object with the fill-style keys plus `width`,
`alignment` (0 inside .. 1 outside), `cap` (`butt round square`), `join` (`miter bevel round`),
`miter_limit`, `pixel_line`, `dash`, `dash_offset`. Optional arguments end with `?` in the usage
the error message prints.

Shortcut for one SVG shape:

<!-- check: scene -->
```json
{ "type": "graphics", "svg": "M 0 40 L 60 40 L 60 15 L 110 55 Z", "fill": "#64e08a",
  "stroke": { "width": 3, "color": "#1f5f33" } }
```

**mesh** - the variant is chosen by its keys (`mesh_simple`, `mesh_plane`, `perspective_mesh`,
`mesh_rope` are accepted type names too):

<!-- check: scene -->
```json
{ "type": "mesh", "texture": "a.png", "positions": [0, 0, 100, 0, 0, 100], "uvs": [0, 0, 1, 0, 0, 1],
  "colors": ["red", "lime", "blue"], "indices": [0, 1, 2] }
{ "type": "mesh", "texture": "a.png", "plane": [10, 10] }
{ "type": "mesh", "texture": "a.png", "perspective": { "verts": [8, 8],
  "corners": [[0, 0], [200, 20], [200, 180], [0, 200]] } }
{ "type": "mesh", "texture": "rope.png", "rope": { "points": [0, 0, 50, 10, 100, 0], "texture_scale": 0, "width": 20 } }
```

**particle_container** - emitters are game code; data sets the texture, the dynamic property
groups and optional start particles:

<!-- check: scene -->
```json
{ "type": "particle_container", "texture": "atlas.json#spark.png",
  "dynamic": ["position", "rotation", "color"],
  "particles": [ { "position": [10, 20], "scale": 0.5, "anchor": [0.5, 0.5], "rotation": 0,
                   "tint": "orange", "alpha": 0.8, "texture": "atlas.json#spark.png" } ] }
```
Groups: `vertex position rotation uvs color`.

**text**, **bitmap_text**, **rich_text**

<!-- check: scene -->
```json
{ "type": "text", "text": "Hello", "anchor": [0.5, 0.5], "resolution": 2,
  "style": {
    "font": "fonts/Title.ttf", "font_family": "default", "font_size": 32, "bold": false, "italic": false,
    "fill": "white",
    "gradient": { "stops": [[0, "#ffffff"], [1, "#88aaff"]], "start": [0, 0], "end": [0, 1] },
    "stroke": { "width": 3, "color": "black" },
    "drop_shadow": { "color": "#00000080", "angle": 0.8, "distance": 4, "blur": 2 },
    "align": "center", "word_wrap": true, "wrap_width": 300, "break_words": false,
    "line_height": 0, "leading": 0, "letter_spacing": 1, "padding": 0,
    "white_space": "pre", "baseline": "top" } }
{ "type": "bitmap_text", "text": "SCORE 0", "font": "fonts/pixel.fnt", "size": 24 }
{ "type": "rich_text", "text": "<b>Bold</b>, <warn>warning</warn>, <color=#7fdcff>blue</color>",
  "style": { "font_size": 18, "fill": "white" },
  "tag_styles": { "warn": { "font_family": "", "font_size": 0, "color": "red", "bold": true,
                            "italic": false, "underline": false, "strike": false } } }
```
`font` loads a file: `.fnt`/`.xml` = bitmap font, others = TTF/OTF. `align`: `left center right
justify`; `white_space`: `pre normal pre_line`; `baseline`: `top alphabetic middle bottom`.

**3D views** (`"type": "view3d"`, read by `engine/src/data/br_node_json_view3d.c`): a 2D node
that renders a 3D scene into its rectangle. Container fields apply to the view itself; the 3D
content is the `scene` array.

<!-- check: scene -->
```json
{ "type": "view3d", "size": [640, 360], "auto_resize": false, "msaa": 4, "background": "#20242c",
  "environment": { "sky": "#9db8e8", "ground": "#5a4a3a", "intensity": 0.4 },
  "camera": { "position": [0, 2, 6], "target": [0, 1, 0], "fov": 50, "near": 0.1, "far": 200 },
  "scene": [
    { "type": "light", "kind": "directional", "color": "#fff0dc", "intensity": 2, "shadows": true,
      "position": [-4, 8, 4], "target": [0, 0, 0] },
    { "type": "mesh", "shape": "plane", "size": [20, 20], "cast_shadows": false,
      "material": { "model": "pbr", "color": "#707880", "roughness": 0.9 } },
    { "type": "gltf", "source": "models/Fox/Fox.glb", "animation": "Run", "scale": 0.02, "rotation": [0, 90, 0] },
    { "type": "group", "label": "props", "position": [2, 0, 0], "children": [] } ] }
```

| Key | Meaning |
|---|---|
| `size` | View size in stage units (default [640, 360]). |
| `auto_resize` | Follow the window size. |
| `msaa` | MSAA sample count (1 = off). |
| `background` | Clear colour of the view. |
| `environment` | Hemisphere ambient: `sky`, `ground` colours and `intensity` (defaults: grey-blue sky, dark brown ground, 0.4). |
| `camera` | The view's camera: `fov` (vertical, degrees), `near`, `far`, plus the 3D node keys below (`position`, `target`, ...). |
| `scene` | Array of 3D nodes. |

Every 3D node: `type` (`group` (default), `mesh`, `light`, `gltf`), `label`, `position`
[x, y, z], `rotation` (Euler degrees [x, y, z]), `scale` (number or [x, y, z]), `target`
(look at a point), `visible`, `children` (array of 3D nodes), `components` (3D components,
`{ "spin": { "speed": 2 } }`; node fields name 3D labels in the same view, resolved after the
whole view is built, so they may point forward).

| Type | Keys |
|---|---|
| `mesh` | `shape` and `size`: `cube` [x, y, z], `sphere` [r], `plane` [w, d], `cylinder` [r, h], `cone` [r, h], `torus` [R, r], `capsule` [r, length]; `cast_shadows`; `material`: `model` (`pbr` (default), `phong`, `flat`, `toon`), `color`, `metallic`, `roughness`, `emissive`, `emissive_strength`, `specular`, `shininess`, `double_sided`, `alpha` (`opaque`, `mask`, `blend`), `texture` (base colour). |
| `light` | `kind` (`directional` (default), `point`, `spot`), `color`, `intensity`, `range`, `shadows`, `shadow_distance`, `cascades` (1-4), `inner_angle` / `outer_angle` (degrees, spot). |
| `gltf` | `source` (required): a `.glb` / `.gltf` path, or one node of a kit with its subtree, `"models/kit.glb#Tree"` (its translation is reset so it lands where the node is placed); `animation` (clip name, or index for unnamed clips), `speed`, `loop`, `time` (the animator); `autoplay` (a clip started at once, `""` = none; default: the file's scene extras `"autoplay"`); `batch` (`false` keeps repeated static meshes as separate nodes); `camera` (`true` = the file's first camera becomes the view's camera, or a framing camera if it has none; a string = the camera on that glTF node); `default_light` (`true` adds a sun when the file has no lights). |

Colours are written as a colour picker shows them (sRGB) and converted to linear. 3D keys are
not checked for typos, and `br_scene_file_save` writes a view3d as it was loaded (live changes
to its 3D nodes are not saved). Each `mesh` creates its own geometry and material, freed when the
mesh node is destroyed (with its view). Example in use: `examples/gltf_demo.c` (the inset view).

### 2.3 Node type names

| JSON `type` | Node type | Created by |
|---|---|---|
| `container` | `BR_NODE_TYPE_CONTAINER` | `br_node_create` |
| `sprite` | `"sprite"` | `br_node_create` |
| `animated_sprite` | `"sprite"` + animation | `br_animated_sprite_create` |
| `nine_slice`, `nine_slice_sprite` | `"nine_slice"` | `br_node_create` |
| `tiling_sprite` | `"tiling_sprite"` | `br_tiling_sprite_create` |
| `graphics` | `"graphics"` | `br_node_create` |
| `mesh`, `mesh_simple`, `mesh_plane`, `perspective_mesh`, `mesh_rope` | `"mesh"` | the matching `br_mesh_*_create` |
| `particle_container` | `"particle_container"` | `br_node_create` |
| `text`, `bitmap_text`, `rich_text` | same names | `br_text_create` / `br_bitmap_text_create` / `br_rich_text_create` |
| any other registered type | that type | `br_node_create`, then its `from_json` |

Your own node types: register them, then `br_node_type_set_from_json(e, type_id, fn)`; inside
`fn` read keys with `br_data_get_float/vec2/color/texture/enum/...` (they report errors with the
JSON path).

### 2.4 Filters

A list of `{ "type": ..., params }`; params sit next to `type` or inside `"params"`. Every
filter takes `enabled` (default true) and `padding`.

<!-- check: fields -->
```json
"filters": [
  { "type": "alpha", "alpha": 0.5 },
  { "type": "blur", "params": { "strength": 8, "quality": 4, "kernel_size": 5, "blur_x": 8, "blur_y": 2 } },
  { "type": "color_matrix", "matrix": [1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0], "alpha": 1,
    "presets": ["sepia", { "name": "brightness", "value": 1.2 }, { "name": "tint", "color": "#ff8800" }] },
  { "type": "displacement", "map": "fx/noise.png", "scale": [20, 20], "map_offset": [0, 0] },
  { "type": "noise", "noise": 0.5, "seed": 0.3 }
]
```
Colour-matrix presets apply in order: `brightness hue contrast saturate greyscale night predator`
(with `value`), `tint` (with `color`), `black_and_white desaturate negative sepia technicolor
polaroid to_bgr kodachrome browni vintage lsd`. Custom filters need a compiled shader and are set
from code.

### 2.5 Components

`"components": { "name": { "field": value } }` fills a registered component
(`br_component_register`) through its `br_field_desc` list:

| Field type | JSON |
|---|---|
| `BR_FIELD_BOOL` | `true` / `false` |
| `BR_FIELD_INT` | whole number |
| `BR_FIELD_FLOAT` | number |
| `BR_FIELD_VEC2` | `[x, y]`, `{ "x", "y" }` or one number |
| `BR_FIELD_VEC3` | `[x, y, z]` |
| `BR_FIELD_COLOR` | colour (section 4) |
| `BR_FIELD_STRING` | string, at most 63 bytes |
| `BR_FIELD_ASSET` | asset path (a sound, a texture, a scene...), at most 63 bytes; not loaded automatically |
| `BR_FIELD_NODE` | label of another node, or `null` |

<!-- check: fields -->
```json
"components": {
  "enemy": { "alive": true, "hp": 3, "speed": 120.5, "home": [400, 300], "axis": [0, 1, 0],
             "flash": "#ff0000", "name": "grunt", "hit_sound": "sounds/hit.wav", "target": "hero" }
}
```
Components must be registered before the file is loaded. Fields left out keep their value
(zero for a new component). Saving writes every field of every component whose name has no
`.` (engine-internal components are named `module.name`).

**Engine components.** The animation module registers `animator` and `tween`, so scene files
and prefabs can start animation without code:

<!-- check: scene -->
```json
{ "type": "container", "label": "actors", "children": [
  { "type": "sprite", "label": "hero", "texture": "atlas.json#walk_0.png",
    "components": { "animator": { "clips": "animation/hero.anim.json", "play": "walk", "speed": 1, "loop": "loop" } },
    "children": [ { "type": "container", "label": "leg_l" },
                  { "type": "container", "label": "body", "children": [ { "type": "container", "label": "head" } ] } ] },
  { "type": "sprite", "label": "panel", "alpha": 0,
    "components": { "tween": { "property": "alpha", "to": 1, "duration": 0.4, "delay": 0.2, "ease": "out_quad" } } } ] }
```

`animator`: `clips` (an anim.json, section 6, loaded through the asset cache), `graph` (optional
animgraph.json, section 7; its states name clips of `clips`), `play` (optional clip), `loop`, `speed` (0 = 1). `tween`: `property`
(a property path, section 6), `to` / `from` (a number or `[x, y]`), `to_color` / `from_color`
(colour properties such as `tint`), `mode` (`"to"` default, `"from"`, `"from_to"`), `duration`,
`delay`, `ease` (easing name), `repeat` (-1 = forever), `yoyo`. Each row is turned into a real
animator / tween the first frame after it appears (2D nodes; one tween component per node).
Details: `docs/agents/animation.md`.

The camera module registers `camera.follow` (`target` node, `offset`, `damping`, `dead_zone`,
`use_bounds`, `offset3d`), `camera.shake` (`trauma`, `decay`, `max_offset`, `max_angle`,
`frequency`), `camera.orbit` (`target` [x, y, z], `distance`, `yaw`, `pitch`, `min_distance`,
`max_distance`, `rotate_speed`, `zoom_speed`) and `camera.anchor` (`camera` node, `anchor`,
`offset`, `safe_area`); they go on camera nodes made in code (`docs/agents/camera.md`; a camera
has no JSON node type yet, so a scene file can anchor a HUD node to a camera labelled in code).

### 2.6 Saving and hot reload

`br_scene_file_save(e, root, path)` writes the subtree: live container fields and components,
the type-specific keys as they were loaded (graphics commands, filters, masks and handlers cannot
be read back from a node), the live text of text nodes and the live geometry fields of sprites,
nine-slices and tiling sprites. Loading the result gives the same tree.

In debug builds every file loaded with `br_scene_file_load` is watched: system
`data.hot_reload` checks modification times every 30 frames and rebuilds a changed file into the
same root node (children recreated, root fields re-applied). A file that no longer parses keeps
the old scene. `br_scene_file_set_hot_reload(e, enabled, poll_frames)` changes this.

---

## 3. Prefabs (`*.prefab.json`)

A prefab is a scene file (one node object) meant to be instantiated many times:

<!-- check: prefab prefabs/coin.prefab.json -->
```json
{
  "type": "container",
  "label": "coin",
  "components": { "mover": { "velocity": [60, 0], "spin": 0 } },
  "children": [
    { "type": "graphics", "commands": [["circle", 0, 0, 18], ["fill", "gold"]] },
    { "type": "sprite", "texture": "atlas.json#gem.png", "anchor": [0.5, 0.5], "scale": 0.6 }
  ]
}
```

Instances, in a scene file (other keys are overrides):

<!-- check: scene -->
```json
{ "prefab": "prefabs/coin.prefab.json", "position": [400, 0], "tint": "#ffb0b0",
  "components": { "mover": { "velocity": [-90, 0] } },
  "children": [ { "type": "text", "text": "x2" } ] }
```

or from code: `br_prefab_instantiate(e, "prefabs/coin.prefab.json", parent, overrides_json)`.

Override rules: objects merge key by key, recursively (`components.mover.velocity` changes, `spin`
stays); `children` are appended to the prefab's children; anything else replaces. A prefab may
itself be `{ "prefab": ..., overrides }` (up to 16 levels). Parsed prefab files are cached
(`br_prefab_clear_cache`; hot reload clears it).

---

## 4. Values used everywhere

| Kind | Forms |
|---|---|
| colour | `"#rgb"`, `"#rgba"`, `"#rrggbb"`, `"#rrggbbaa"`, `"0xrrggbb"`, `"rgb(255, 128, 0)"`, `"rgba(255, 128, 0, 0.5)"`, CSS names (`"red"`, `"gold"`, `"transparent"`), a number `0xRRGGBB` (decimal in JSON), `[r, g, b]` / `[r, g, b, a]` in 0..1 |
| vec2 | `[x, y]`, `{ "x": 1, "y": 2 }`, or one number for both |
| rect | `[x, y, width, height]` |
| texture | `"images/hero.png"` (asset path or alias), `"atlas.json#hero.png"` (spritesheet frame), `"atlas#hero.png"` (frame of a sheet by its manifest alias), `"hero.png"` (a frame of any spritesheet already loaded, e.g. through `"assets"`) |
| angle | radians (`rotation`, `tile_rotation`, shadow `angle`), degrees only in `angle` |
| enum | lower-case names listed with each key |

---

## 5. Formats owned by other modules

These are read by other modules and only referenced from the files above.

**Asset manifest** (`br_assets_load_manifest`, the `manifest` key of game.json), PixiJS shape:

<!-- check: manifest -->
```json
{ "bundles": [
    { "name": "level1", "assets": [
      { "alias": "hero", "src": "images/hero@{1,2}x.{dds,png}" },
      { "alias": ["tiles", "t"], "src": ["atlas@2x.json", "atlas.json"], "type": "spritesheet" } ] },
    { "name": "level1_sounds", "assets": [ { "alias": "music", "src": "sounds/music.ogg" } ] } ] }
```
Details: `engine/include/braty/br_assets.h`, `docs/agents/assets.md`.

**Spritesheet** (TexturePacker JSON hash or array; `br_spritesheet.h`): `frames` with `frame`,
`rotated`, `trimmed`, `spriteSourceSize`, `sourceSize`, optional `anchor`/`pivot` and
`borders` `{ left, top, right, bottom }` (used by nine-slices), `animations`, `meta.image`.
Example: `examples/data/atlas.json`.

**Bitmap fonts**: AngelCode BMFont text or XML (`.fnt`, `.xml`); example
`examples/fonts/pixel5x7.fnt`.

**Input actions** (`br_input_actions_from_json`, the `input` key of game.json):

<!-- check: input -->
```json
{ "jump": { "keys": ["Space"], "mouse": [1], "buttons": ["a"], "axes": ["-lefty"] } }
```
`keys` are SDL scancode names, `buttons` SDL gamepad button names, `axes` SDL axis names with a
`+`/`-` direction, `threshold` the analog value that counts as pressed, `context` the input
context the action belongs to (`br_input_action_set_context`). Details: `docs/agents/input.md`.

**glTF 2.0** (`.gltf`, `.glb`; read with cgltf): models for `model3d` nodes and the `gltf` 3D
node of a `view3d`. Supported features and extensions: `docs/agents/gltf.md`.

A `.glb` / `.gltf` path also works **as a scene** and **as a prefab**, with no JSON around it:
`br_scene_load(e, "levels/level1.glb", BR_SCENE_SINGLE)` (or a game.json `"scenes"` entry /
`start_scene` naming it) builds a full-window `view3d` labelled after the file, with the file's
nodes, lights and first camera; the glTF scene extras `background`, `msaa`, `sky`, `ground` and
`ambient` set up the view, `autoplay` and `batch` apply to the instance.
`br_prefab_instantiate(e, "props/tree.glb", parent, overrides)` makes a 640 x 360 `view3d` holding
the model (overrides apply to the view). Either way, **node extras fill components**: register the
component, then a node's `"extras": { "crate.value": 10 }` (Blender custom property
`crate.value`) or `"extras": { "components": { "crate": { "value": 10 } } }` puts a `crate`
component on that node's 3D node (`br_component3d_get`, `_count`, `_at`, `_owner_at`). Name
suffixes and convention keys (`-col`, `-colonly`, `-trigger`, `-norender`, `collider`, `trigger`,
`no_render`, `visible`, `layer`, `cast_shadows`, `batch`) are listed in `docs/agents/gltf.md`;
`sample_projects/crate_rush/tools/build_assets.py` writes complete levels this way.

**Play-test scripts** (`--playtest file.json`): a JSON array of commands (`{"cmd": "step",
"frames": 60}`, `{"cmd": "expect", ...}`); the whole command list is in `docs/PLAYTESTING.md`.

---

## 6. Animation clips (`*.anim.json`)

Loaded with `br_anim_library_load(e, path)` (asset type `"anim"`) or by the `animator`
component (section 2.5). A track targets a property path `"<node path>:<property>"` relative to
the animator's node: `"body/arm_r:rotation"`, `":alpha"` (the node itself), `"#3:rotation"`
(glTF node 3 of a `model3d`), `"hp.value"` (component field). Properties: `position` `x` `y`
`scale` `rotation` `skew` `pivot` `alpha` `tint` `visible` `z_index` `sprite.texture`
`sprite.frame` (2D); `position` `rotation` `scale` `visible` `material.*` `morph_weights` (3D);
any float / int / bool / vec2 / vec3 / colour component field; more from
`br_anim_property_register` (`docs/agents/animation.md`).

<!-- check: anim animation/hero.anim.json -->
```json
{ "clips": [
  { "name": "walk", "loop": "loop", "duration": 1.0,
    "tracks": [
      { "target": "leg_l:rotation", "ease": "in_out_sine", "keys": [[0, 0.45], [0.5, -0.45], [1, 0.45]] },
      { "target": "body:position", "keys": [[0, [0, 0]], [0.5, [0, -6]], [1, [0, 0]]] },
      { "target": "body/head:tint", "interpolation": "step", "keys": [[0, "#ffd9b0"], [0.9, "#ffb090"]] },
      { "target": "body:y", "interpolation": "cubic",
        "keys": [ { "time": 0, "value": 0, "out": -20 }, { "time": 1, "value": 0, "in": 20 } ] },
      { "target": ":sprite.texture", "interpolation": "step", "keys": [[0, "atlas.json#walk_0.png"], [0.1, "atlas.json#walk_1.png"]] }
    ],
    "events": [ { "time": 0.0, "name": "step" }, { "time": 0.5, "name": "step" } ] } ] }
```

- A file is `{ "clips": [ ... ] }` or a single clip object (with `"tracks"`).
- Clip keys: `name`, `loop` (`loop` default, `once`, `ping_pong`, `clamp`), `duration` (optional:
  at least the last key time), `tracks`, `events` (`time` seconds, `name`).
- Track keys: `target` (a property path, see above), `interpolation` (`step`, `linear` default,
  `cubic`, `bezier`), `ease` (an easing name, LINEAR and BEZIER segments), `keys`.
- A key is `[time, value]` or `{ "time", "value", "in", "out" }`. `in` / `out` are tangents
  (`cubic`, default 0) or absolute handle values (`bezier`, default: the key's value).
- A value is a number, an array of 1-8 numbers, a colour string (`"#rrggbb"`, `"#rrggbbaa"`, CSS
  name: 4 floats, sRGB) or a texture reference (`"sheet.json#frame"`, a path, or a frame name of a
  loaded sheet) for `sprite.texture`. Every key of a track has the same number of values.
- Errors (`file: clips[0].tracks[2].keys[1]: reason`) skip that track or event; the rest loads.
- 3D rotations are quaternions `[x, y, z, w]`; material colours are sRGB.

---

## 7. Animation state machines (`*.animgraph.json`)

Loaded with `br_anim_graph_load(e, path)` (asset type `"animgraph"`) or by the `animator`
component's `graph`; `br_animator_set_graph` attaches it.

<!-- check: animgraph animation/boss.animgraph.json -->
```json
{ "parameters": { "speed": 0.0, "grounded": true, "jump": "trigger" },
  "entry": "idle",
  "states": [
    { "name": "idle", "clip": "Survey" },
    { "name": "move", "blend": { "parameter": "speed", "motions": [ { "clip": "Walk", "at": 0.3 }, { "clip": "Run", "at": 1.0 } ] } },
    { "name": "aim", "blend2d": { "x": "aim_x", "y": "aim_y", "motions": [ { "clip": "AimL", "at": [-1, 0] }, { "clip": "AimR", "at": [1, 0] } ] } },
    { "name": "jump", "clip": "Jump", "loop": "once", "speed": 1.2 } ],
  "transitions": [
    { "from": "idle", "to": "move", "when": [["speed", ">", 0.1]], "duration": 0.25 },
    { "from": "*", "to": "jump", "when": [["jump"], ["grounded", "==", true]], "duration": 0.1 },
    { "from": "jump", "to": "idle", "exit_time": 1.0, "duration": 0.2 } ] }
```

- `parameters`: a number (float), `true`/`false` (bool) or `"trigger"`, with its start value.
- `states`: `name` plus `clip` (a clip the animator has) or `blend` (1D: `parameter`, `motions`
  with `at` numbers) or `blend2d` (`x`, `y`, `motions` with `at` [x, y]); optional `speed`, `loop`.
- `transitions` are checked in order; the first whose conditions all hold wins. `from`: a state
  or `"*"` (any state but the target). `when`: `["param"]` (bool true / trigger set) or
  `["param", op, value]` with `>`, `<`, `>=`, `<=`, `==`, `!=` and a number or bool.
  `exit_time`: the current state's normalised time (0..1 within a loop; 1 = a once clip ended)
  must be reached. `duration`: cross-fade seconds (default 0.25).
- `entry`: the first state (default: the first in the list).
- The animator must have the clips before `br_animator_set_graph`; blend states become blend
  trees named after the state.
