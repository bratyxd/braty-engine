# animation - clips, animators, blend trees, state machines, tweens (2D and 3D)

Headers: `engine/include/braty/br_animation.h` (clips, property registry, animators, graphs,
events), `br_tween.h` (tweens, sequences). Source: `engine/src/animation/`; glTF clips play
through the same animator (`br_animator_add_model_clips`). Tests: `tests/test_animation.c`, `tests/test_tween.c`. Example:
`examples/animation_demo.c` + `examples/animation/` (`hero.anim.json`, `props.anim.json`,
`fox.animgraph.json`); flags `--bench-foxes N`, `--bench-tweens N` (use with `--vsync off`).

## The model

- **One clip format.** A `br_anim_clip` is a name, a duration, a default loop mode, markers
  (named events) and tracks. A track is flat data: `times[]`, `values[]`, `components` floats per
  key, an interpolation (`STEP`, `LINEAR` (slerp for quaternions), `CUBIC` = glTF Hermite with
  in/out tangents, `BEZIER` = in/out handles as absolute values) and an easing (`br_ease_type`)
  that shapes each LINEAR/BEZIER segment.
- **A track targets a property path** `"<node path>:<property>"`, relative to the animator's
  node: `"body/arm_r:rotation"`, `":alpha"` / `"alpha"` (the node itself), `"#3:rotation"` (glTF
  node 3 of a `model3d` instance), `"hp.value"` (component `hp`, field `value`, 2D nodes). Each
  path segment matches a direct child's label first, then any descendant's.
- **Properties come from a registry** of `(name, kind, type, get, set)` rows. Built in:
  - 2D nodes: `position` `x` `y` `scale` `rotation` `skew` `pivot` `alpha` `tint` `visible`
    `z_index`, `sprite.texture` (texture values; flipbooks), `sprite.frame` (animated sprites).
  - 3D nodes: `position` `rotation` (quaternion x y z w) `scale` `visible`;
    `material.base_color` `material.emissive` (sRGB like every data file; the material keeps
    linear) `material.roughness` `material.metallic` `material.alpha` (the node's mesh, or its
    first mesh child - glTF node groups); `morph_weights` (up to 8; every mesh child).
  - Any registered component field of type float, int, bool, vec2, vec3, colour.
  Other modules add rows with `br_anim_property_register` (see below) without touching this module.
- **Clip sources:** `*.anim.json` libraries (asset type `"anim"`), glTF clips
  (`br_animator_add_model_clips` converts once per model and *borrows* the model's key arrays),
  and flipbooks (`br_anim_clip_from_frames` / `_from_spritesheet`: one STEP track on
  `sprite.texture`, so any sprite - an animated sprite included - can be driven by a clip).
- **Animators** (`br_animator` handles) sit on a 2D or 3D node, in one dense pool. The system
  `animation.update` (LATE_UPDATE -100) runs state machines, advances clocks, samples, blends and
  writes every animator; `gltf.animate` (LATE_UPDATE 0) then turns joint transforms into skinning
  matrices straight in the skin slots. Each clip is bound to an animator once (tracks ->
  slot indices, rest values read then); every track remembers its last key segment. Nothing is
  allocated per frame (the legacy callback list uses `br_frame_alloc`).
- **Layers:** layer 0 is the base; layers 1-3 have a weight, override or additive (adds the change
  from the clip's first key), and optional node masks (a node and its descendants). Each layer
  plays one motion (clip or blend tree), cross-fades from the previous one and has a queue.
- **Blend trees** are motions too: 1D (clips at thresholds of one float parameter, lerp between
  neighbours) and 2D (clips at points in a plane, inverse-distance weights). Children run on one
  normalised clock; weights always sum to 1.
- **State machines** (`*.animgraph.json`, asset type `"animgraph"`) run on the base layer: states
  (a clip or a blend tree, speed, loop), transitions from a state or any state (`"*"`) with
  conditions on float / bool / trigger parameters, exit time and blend duration. A running
  transition is not interrupted; triggers are consumed by the transition that used them. A
  parameter's type comes from the graph's `parameters`, else from the first setter (so an
  undeclared trigger set with `br_animator_set_trigger` is consumed too); a setter of another type
  warns once. Names (states, parameters) are at most 31 bytes; longer ones are reported.
- **Events:** markers reached this frame are in `br_animation_events` (read them in any system
  after LATE_UPDATE -100) and go to the animator's callback after all animators were sampled.
  Names stay valid until the next update, even if a callback destroys the node (clips and asset
  references of removed animators are freed at the start of the next update). A marker fires exactly once per pass - across loop wraps, ping-pong turns and big steps (more
  than 8 passes in one step skip the extra passes' markers).
- **Root motion:** `br_animator_set_root_motion(e, a, "Hips")` stops writing that node's
  horizontal position and reports the track's own movement (sample at the new time minus sample
  at the old one, split at loop wraps, weighted like the cross-fade / blend tree) with
  `br_animator_root_motion`; apply it to the node itself or to a parent. Base layer only.
- **Fades start from the pose on screen.** A base-layer fade started while another runs, or after
  a ONCE clip finished, fades out of a frozen snapshot of what was shown. `br_animator_cross_fade`
  to the motion already playing or fading in does nothing, so "ask for the state you want every
  frame" works.
- **Tweens** live in one dense array advanced by `animation.tweens` (LATE_UPDATE -200, so an
  animator wins over a tween on the same property). A `to` tween reads its start value when it
  starts (after its delay). Sequences are a plain time cursor that turns "after" / "with" into
  start delays, so a sequence costs nothing per frame.
- **Destroyed nodes / missing properties** (Review Focus 5): a track whose node or property
  does not resolve when the clip is bound is dropped with one warning; a node destroyed later is
  noticed (targets are re-checked only after some node died), its tracks are dropped with one
  warning, and the animator keeps playing the rest. Animators and tweens on a destroyed node go
  away with it.
- **Single-threaded on purpose.** Setters write the shared node tables (dirty flags, parent
  lists), so a parallel-for would need a sample phase and a write phase; at 1,000 foxes the
  sampling share is ~3 ms and the gain did not justify that. Results are deterministic: the same
  script gives byte-identical frames (checked on D3D12 and in `test_animation`).

## Functions you actually need

| Function | What for |
|---|---|
| `br_animator_add(e, br_anim_target_2d(n))` / `_3d(n)`, `br_animator_get` | An animator on a node (one per node). |
| `br_animator_add_library(e, a, br_anim_library_load(e, "x.anim.json"))` | Clips from a file. |
| `br_animator_add_model_clips(e, a, model)` | A glTF model's clips (animator on the `model3d` node). |
| `br_animator_add_clip(e, a, clip)`, `br_anim_clip_from_frames(name, frames, n)` | Your own / flipbook clips (you own them; they must outlive the animator). |
| `br_animator_rebind(e, a)` | Re-resolve dropped tracks after attaching nodes (a weapon) under the animator. |
| `br_animator_play`, `_cross_fade(e, a, "run", 0.2f)`, `_queue`, `_stop` | Base-layer playback. |
| `br_animator_set_speed`, `_set_paused`, `_set_loop(e, a, layer, BR_ANIM_PING_PONG)`, `_set_time` | Clock control. |
| `br_animator_set_layer(e, a, 1, weight, additive)`, `_layer_add_mask`, `_play_layer` | Layers. |
| `br_animator_set_float/_bool/_trigger`, `_add_blend1d`, `_add_blend2d` | Parameters, blend trees. |
| `br_animator_set_graph(e, a, br_anim_graph_load(e, "x.animgraph.json"))`, `br_animator_state` | State machine. |
| `br_animation_events(e, &n)`, `br_animator_set_event_callback` | Markers. |
| `br_tween_to(e, target, "position", to, seconds, ease)`, `_to_f`, `_from`, `_from_to` | One-line tweens. |
| `br_tween_set_delay/_repeat(count, yoyo)/_on_complete/_tag`, `br_tween_kill*` | Tween options. |
| `br_tween_sequence_begin/_add/_join/_wait/_call` | Sequences and parallel groups. |
| `br_anim_get/_set(e, root, "arm:rotation", &value)`, `br_anim_property_register` | Property paths, new properties. |

## Snippets (from examples/animation_demo.c)

A 2D rig from `hero.anim.json`, plus a second layer masked to the right arm (two-layer blending;
the demo moves the layer weight every frame with `br_animator_set_layer`):

```c
const br_anim_library *lib = br_anim_library_load(e, "animation/hero.anim.json");
br_animator a = br_animator_add(e, br_anim_target_2d(s->hero));
br_animator_add_library(e, a, lib);
br_animator_play(e, a, "walk");                           /* base layer: the whole body */
br_animator_set_layer(e, a, 1, 0.0f, false);              /* layer 1 overrides ... */
br_animator_layer_add_mask(e, a, 1, "body/arm_r");        /* ... only the right arm */
br_animator_play_layer(e, a, 1, "wave", 0.0f);
br_animator_set_event_callback(e, a, on_hero_event, s);   /* "step" markers */
```

The Fox with a state machine (`fox.animgraph.json`):

```c
br_animator a = br_animator_add(e, br_anim_target_3d(f));
br_animator_add_model_clips(e, a, fox);   /* Survey, Walk, Run (keys shared with the model) */
br_animator_set_graph(e, a, graph);       /* idle -> walk -> run by "speed" */
/* every frame: br_animator_set_float(e, a, "speed", speed); */
```

A UI panel brought in by a tween sequence:

```c
br_anim_target panel = br_anim_target_2d(s->panel);
float slide_to[2] = { 950.0f, 30.0f };
float pop_to[2] = { 1.0f, 1.0f };
br_tween_sequence seq = br_tween_sequence_begin(1);
br_tween_sequence_wait(&seq, 0.2f);
br_tween_sequence_add(e, &seq, br_tween_to(e, panel, "position", slide_to, 0.6f, BR_EASE_OUT_BACK));
br_tween_sequence_join(e, &seq, br_tween_to_f(e, panel, "alpha", 1.0f, 0.4f, BR_EASE_OUT_QUAD));
br_tween_sequence_add(e, &seq, br_tween_to(e, br_anim_target_2d(s->panel_text), "scale", pop_to, 0.5f,
                                           BR_EASE_OUT_ELASTIC));
br_tween_sequence_call(e, &seq, on_panel_ready, s);
```

## Adding a property from another module

```c
static void get_glow(br_engine *e, br_anim_target t, br_anim_value *out) { out->f[0] = my_glow(e, br_anim_target_node(t)); }
static void set_glow(br_engine *e, br_anim_target t, const br_anim_value *v) { my_set_glow(e, br_anim_target_node(t), v->f[0]); }
br_anim_property_desc d = { "mymodule.glow", BR_ANIM_TARGET_2D, BR_ANIM_FLOAT, get_glow, set_glow };
br_anim_property_register(e, &d);   /* now "panel:mymodule.glow" works in clips, tweens and anim.json */
```

Call it from your module's register function (it works before `br_animation_register` too) or
from a game's init. Setters must ignore nodes they do not apply to. A row with the same kind and
name replaces the old one, so a module can also override a built-in property.

## Scene JSON (components; no data-layer code)

The module registers two components, so scene files and prefabs can start animation:

```json
{ "type": "container", "label": "hero", "children": [ ... ],
  "components": { "animator": { "clips": "animation/hero.anim.json", "play": "walk", "speed": 1, "loop": "loop" } } },
{ "type": "container", "label": "boss",
  "components": { "animator": { "clips": "boss.anim.json", "graph": "boss.animgraph.json" } } },
{ "type": "sprite", "label": "panel", "alpha": 0,
  "components": { "tween": { "property": "alpha", "to": 1, "duration": 0.4, "delay": 0.2, "ease": "out_quad" } } }
```

`animator`: `clips` (an anim.json, loaded through the asset cache), `graph` (optional
animgraph.json), `play` (optional clip), `loop`, `speed` (0 = 1). The animator holds one reference
to each asset and releases it when it is removed. `tween`: `property` (a property path), `to` /
`from` (a number or `[x, y]`), `to3` / `from3` (`[x, y, z]`, vec3 properties such as a 3D
`position`), `to_color` / `from_color` (colour properties such as `tint`), `mode` (`"to"`
default, `"from"`, `"from_to"`), `duration`, `delay`, `ease` (easing name), `repeat` (-1 =
forever), `yoyo`; rotations and weights cannot be tweened from a component. Each row is turned
into a real animator / tween the first frame after it appears, on 2D and on 3D nodes, so glTF
node extras work too: `{ "animator.clips": "door.anim.json", "animator.play": "open" }` (one
tween component per node).

## `*.anim.json` (also docs/DATA_FORMATS.md, section 6)

```json
{ "clips": [
  { "name": "walk", "loop": "loop", "duration": 1.0,
    "tracks": [
      { "target": "leg_l:rotation", "ease": "in_out_sine", "keys": [[0, 0.45], [0.5, -0.45], [1, 0.45]] },
      { "target": "body:position", "keys": [[0, [0, 0]], [0.5, [0, -6]], [1, [0, 0]]] },
      { "target": "body/head:tint", "interpolation": "step", "keys": [[0, "#ffd9b0"], [0.9, "#ffb090"]] },
      { "target": "body:y", "interpolation": "cubic",
        "keys": [ { "time": 0, "value": 0, "out": -20 }, { "time": 1, "value": 0, "in": 20 } ] },
      { "target": ":sprite.texture", "interpolation": "step", "keys": [[0, "hero.json#walk_0.png"], [0.1, "hero.json#walk_1.png"]] }
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
  Numbers must be finite; a track has at most 100,000 keys.
- 3D rotations are quaternions `[x, y, z, w]`; material colours are sRGB.

## `*.animgraph.json` (also docs/DATA_FORMATS.md, section 7)

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
  trees named after the state (a clip with the same name does not replace the blend; a warning
  says `br_animator_play` by that name plays the clip).
- Bad values are reported with their JSON path: wrong types, non-finite numbers, names over 31
  bytes (clip names over 63), more than 1,024 states or 4,096 transitions. A bad state or
  transition is skipped; a graph without a valid entry state fails to load.

## Numbers (RTX 3060 Ti, Release, 1280x720, vsync off; ms per frame)

| Case | D3D12: frame / animation (share) | Vulkan: frame / animation (share) |
|---|---|---|
| `animation_demo --bench-foxes 1000` (1,005 animators with state machines) | 7.14 / 3.95 (55%): animators 2.90, skinning 1.05 | 8.54 / 4.23 (50%): animators 3.13, skinning 1.11 |
| `animation_demo --bench-tweens 50000` (50,000 sprites, one tween each) | 5.81 / 2.69 (46%) | 5.84 / 2.80 (48%) |
| `animation_demo --bench-foxes 200` | 1.22 / 0.60 (50%) | 1.25 / 0.61 (49%) |
| `gltf_demo --foxes 200` (Phase 1 API, 209 animators; `animation.update` + `gltf.animate`) | 1.26 / 0.469-0.472 | 1.27 / 0.466-0.474 |

The last row against the Phase 1 animator (same machine, runs interleaved): 0.466-0.472 ms on
D3D12, 0.460-0.469 ms on Vulkan, so no regression. "animation" = `animation.update` +
`animation.tweens` + `gltf.animate` (skinning).

## Gotchas

- Clips you build (`br_anim_clip_create`, `_from_frames`, `_from_gltf`) are yours: free them
  after removing the animators that use them. Library clips belong to the asset; model clips are
  freed with the last animator that uses them (the model must outlive the animator: put the
  animator on the model3d node).
- Rest values are read when a clip is first bound. A cross-fade blends properties the new clip
  does not animate towards that rest value (as the Phase 1 animator did).
- On layers 1-3 a fade started while another runs still drops the older fading motion
  (`ponytail:`; the base layer fades from a snapshot).
- `ONCE` stops writing after the end (the last pose stays); `CLAMP` keeps writing it, so other
  code cannot move that property.
- Material properties write the material, which glTF instances share: pulse one instance's
  colour by giving it its own material.
- `sprite.texture` / `bool` / `int` values are not blended: the heavier contribution wins (a
  cross-fade switches at the half).
- Blend tree markers come from the heaviest child; 2D trees use inverse-distance weights
  (`ponytail:` in br_animator.c), not gradient bands.
- Root motion and markers over more than 8 loop passes in one step skip the extra passes.
- Tweens animate up to 8 floats, not textures. One `"tween"` component per node.
