# Braty Engine Phase 2 Implementation Plan

> **For agentic workers:** read `AGENTS.md`, then `docs/plans/2026-11-19-braty-engine.md`
> (its Global Constraints apply here unchanged), then this file. Your task's "Owns" list is the
> only set of files you may create or edit, plus `docs/agents/<module>.md` for modules you own.

**Goal:** Turn the PixiJS-shaped engine of Phase 1 into a 3D-first engine with excellent 2D:
regression tests, memory arenas, vsync + FPS cap, Unity-style cameras, 2D content in the 3D
world, first-class scenes with asset groups, one animation system for 2D and 3D, glTF as the
primary 3D format, and a second sample game that uses all of it.

**Architecture:** Phase 1 keeps two node tables (2D nodes with `br_mat3`, 3D nodes with
`br_mat4` inside a `br_view3d`). Phase 2 does not merge them; it adds the bridges and the
ownership layer on top: a `br_scene` owns nodes of both kinds plus an asset group, a
`br_canvas3d` puts a 2D subtree into the 3D world, cameras drive both, and one animation system
writes into both tables.

**Tech stack:** unchanged (C11, SDL 3.4.20 GPU, CMake/Ninja). Kenney CC0 assets for the sample.

**Spec:** the section "Spec" below (user's follow-up request).

**Starts after:** Phase 1 Task 13 (integration review) is complete.

---

## Spec

User request, condensed, with the decision taken for each:

1. **Tests so iteration does not break things.** Unit tests per module already exist; add
   golden-image regression tests for everything visual and a single command that runs all of it.
2. **glTF is the first-class citizen for 3D.** Scenes reference `.glb` files directly, a glTF is
   a prefab, glTF `extras` (Blender custom properties) become engine components, glTF animations
   land in the unified animation system.
3. **3D engine with excellent 2D, incl. 9-slice and text/fonts in the 3D world.** `br_canvas3d`:
   any 2D subtree (sprites, nine-slice, text, graphics, UI) placed in 3D space, depth-tested,
   lit or unlit, optionally billboarded; SDF text stays sharp at any distance.
4. **Vsync control and capped FPS control.** Two independent settings, changeable at runtime and
   from `game.json`.
5. **Unity-like camera.** Vertical extent is what you set (orthographic size, or vertical FOV);
   horizontal extent follows the screen aspect ratio.
6. **Scene graph system.** Scenes become first-class objects: load, unload, additive load,
   switch with transition; each owns its nodes.
7. **Intuitive asset load/unload, single and grouped, "maybe via scenes?"** Answer: both. Any
   asset can be loaded/unloaded alone; `br_asset_group` loads/unloads a set; every scene has an
   implicit group so loading a scene loads what it needs and unloading it releases them.
   Reference counts make shared assets safe.
8. **Memory arenas of different types, definable in code.**
9. **Intuitive, performant animation for 2D and 3D.** One clip format, one animator, plus
   one-line tweens.
10. **Second sample game using the new features**, Kenney assets allowed.

## Global Constraints (additions to Phase 1's)

- **Every behaviour change ships with a test that fails without it.** Logic -> `tests/test_*.c`.
  Visual -> a golden image (Task 14). `AGENTS.md` states this as a rule.
- **No C++ dependencies**, so Draco, meshopt and Basis decoders stay deferred. Say so in docs.
- **Hot paths are loops over dense arrays.** Animation sampling, transform update and canvas
  batching must not allocate per frame; per-frame scratch memory comes from the frame arena.
- Kenney assets are CC0; keep each pack's `License.txt` next to the files.

## Review Focus

1. **Unloading a scene while its assets are still loading in the background:** load is
   cancelled or completes then releases; no leak, no use-after-free, no callback into a dead
   scene. Task 16 test.
2. **An asset shared by two scenes/groups is unloaded by one:** stays alive until the last
   reference goes. Task 16 test.
3. **Aspect-ratio extremes (21:9, 9:16 portrait, resize mid-frame):** vertical extent constant,
   horizontal follows; UI anchored to screen edges stays on screen. Task 15 golden images at
   three aspect ratios.
4. **Arena overflow and misuse:** allocation beyond capacity returns NULL and logs (or grows, by
   arena type); freeing a stack marker out of order is detected in debug builds. Task 14 test.
5. **Animation targeting a destroyed node / missing property:** binding is dropped quietly once
   with one warning, the animator keeps playing other tracks. Task 17 test.

---

## Tasks

Order: 14 -> {15, 16, 17 in parallel} -> 18 -> 19 -> 20.

### Task 14: Arenas, frame pacing, regression-test harness

**Owns:** `engine/src/core/br_arena*.c`, `engine/include/braty/br_arena.h`, frame-pacing code in
`engine/src/app/**` and `engine/src/gpu/**`, `tools/golden.py`, `tests/golden/**`,
`tests/test_arena.c`, `tests/test_pacing.c`, CMake test wiring, `docs/TESTING.md`.

- [ ] `br_allocator`: a plain struct `{ alloc, realloc, free, user }` of function pointers so
      engine containers (`br_array`, pools, maps) can be created on a chosen allocator; default
      is the heap.
- [ ] Arena types, each a plain struct with create/destroy and a `br_allocator` view:
      - `br_arena_linear` (bump; reset all at once; fixed capacity or growing by chained blocks),
      - `br_arena_stack` (bump with markers; `push_marker` / `pop_to_marker`, order checked in debug),
      - `br_arena_pool` (fixed-size blocks with a free list),
      - `br_arena_frame` (engine-owned linear arena reset every frame: `br_frame_alloc(e, size)`),
      - `br_arena_scratch` (temporary scope: begin/end pair on a thread-local-free, explicitly
        passed linear arena).
      All support alignment, report used/peak/capacity, and can be backed by caller memory
      (`create_in(buffer, size)`) for consoles with fixed budgets. Named arenas show up in the
      debug overlay's memory panel.
- [ ] Vsync: `br_app_set_vsync(e, BR_VSYNC_OFF | BR_VSYNC_ON | BR_VSYNC_ADAPTIVE | BR_VSYNC_MAILBOX)`
      mapped to SDL GPU present modes with fallback when unsupported (query + log).
      FPS cap: `br_app_set_max_fps(e, fps)` (0 = uncapped), independent of vsync, implemented with
      a precise sleep + short spin so it also works with vsync off. Fixed-timestep option for
      simulation systems (`br_app_set_fixed_timestep(e, hz)` adds `BR_PHASE_FIXED_UPDATE`).
      All three readable from `game.json` and switchable at runtime.
- [ ] Golden-image harness: `tools/golden.py run|update` runs each example with fixed seed,
      fixed `--frames`, fixed time step (`--deterministic` flag in `br_app`: constant dt, seeded
      RNG) and compares the PNG with `tests/golden/<example>.png` within a per-pixel tolerance;
      writes a diff image on failure. Registered in CTest with label `golden`; skipped (not
      failed) when no GPU device can be created. Commit goldens for every existing example.
- [ ] `docs/TESTING.md` + `AGENTS.md` rule: how to run all tests, how to add a unit test, how to
      add/update a golden, when an update is legitimate.
- [ ] Tests: each arena type (alignment, overflow, reset, marker order, pool reuse); pacing
      (measured frame interval within 2% of cap over 120 frames, headless).

### Task 15: Cameras and 2D in the 3D world

**Owns:** `engine/src/camera/**`, `engine/src/canvas3d/**`, `engine/include/braty/br_camera.h`,
`br_canvas3d.h`, their shaders, `tests/test_camera.c`, `examples/camera_demo.c`,
`examples/canvas3d_demo.c`, goldens for both.

- [ ] `br_camera` (usable as a 2D node and as a 3D node), Unity semantics:
      - orthographic: `orthographic_size` = half the visible **vertical** world extent;
        horizontal = size * aspect. 2D scenes get a `pixels_per_unit` setting.
      - perspective: `field_of_view` is vertical; horizontal follows aspect.
      - `near`, `far`, `viewport_rect` (normalised), `depth` (render order between cameras),
        `culling_mask` + per-node `layer`, `clear_flags` (colour / depth only / nothing),
        `background_color`, `target_texture`, `aspect` override.
      - `br_camera_screen_to_world`, `world_to_screen`, `screen_point_to_ray`, `viewport_to_world`.
      - Several cameras per scene (split screen, minimap, UI camera over 3D camera).
      - Phase 1's Pixi-style "stage in pixels" stays as the default when no camera exists, so
        existing examples are unchanged.
- [ ] Camera helpers as data + systems: follow (damping, dead zone, bounds), shake, orbit
      (Phase 1's orbit camera moves onto this), pixel-perfect snap for 2D.
- [ ] Screen-fit policies for 2D/UI layered on top: `expand`, `fit_height` (the Unity default
      above), `fit_width`, `letterbox`, and anchors to screen edges / safe area
      (`SDL_GetWindowSafeArea`) for UI nodes.
- [ ] `br_canvas3d` (3D node type): hosts a 2D subtree in the 3D world.
      - Mode `geometry` (default): the subtree's batched triangles are drawn in the 3D pass with
        the canvas' world matrix, depth test on, optional depth write, so sprites, nine-slice,
        graphics and text are crisp at any distance and sort with 3D transparents.
      - Mode `texture`: subtree rendered to a render texture shown on a quad (needed when the
        subtree uses filters/masks).
      - `pixels_per_unit`, pivot, `billboard` (none / full / y-axis), `lit` (uses scene lights)
        or unlit, double-sided, tint/alpha, receives pointer events through 3D picking mapped
        back into the 2D subtree (buttons in the world work).
      - Convenience creators: `br_sprite3d_create`, `br_text3d_create`, `br_nine_slice3d_create`
        (a canvas with one child).
      - Text in 3D uses SDF/MSDF fonts (generated from the TTF at load) so it stays sharp.
- [ ] Tests: ortho/perspective extents at 16:9, 21:9, 9:16 (Review Focus 3); screen<->world
      round trips; culling mask; canvas ray -> 2D local point mapping.

### Task 16: Scenes and asset groups

**Owns:** `engine/src/scenes/**`, `engine/include/braty/br_scene.h`, `br_asset_group.h`, additions
inside `engine/src/assets/**` and `engine/src/data/**` needed for them, `tests/test_scene.c`,
`tests/test_asset_group.c`, `examples/scenes_demo.c` + its data, golden.

- [ ] Asset API made obvious, three levels:
      - single: `br_assets_load(e, path)` / `br_assets_unload(e, path)` (+ async variants);
      - group: `br_asset_group_create(e, name)`, `_add(group, path)`, `_add_manifest_bundle`,
        `_load(group)`, `_load_async(group, on_progress, on_done)`, `_unload(group)`,
        `_progress(group)`, `_is_loaded(group)`;
      - scene: implicit group, see below.
      Reference counted throughout (Review Focus 2); `br_assets_report(e)` lists every live
      asset with ref count, size and owning groups (also shown in the debug overlay).
- [ ] `br_scene` first-class object: `br_scene_load(e, "levels/one.scene.json", mode)` with
      `BR_SCENE_SINGLE` (unloads others) or `BR_SCENE_ADDITIVE`; `br_scene_load_async` with
      progress; `br_scene_unload`; `br_scene_set_active`; `br_scene_find(e, name)`;
      `br_scene_root_2d(scene)` / `br_scene_root_3d(scene)`; nodes created while a scene is
      active belong to it; `br_node_set_persistent` (survives scene switches, Unity's
      DontDestroyOnLoad).
- [ ] Scene file gains: `"assets": [paths or bundle names]` (explicit) and automatic
      collection of every asset path referenced by its nodes/components; both feed the scene's
      implicit group. `"preload"` vs `"lazy"` per entry.
- [ ] Scene lifecycle callbacks as systems-friendly events (`on_loaded`, `on_unloading`), and
      transitions (`br_scene_switch(e, path, BR_TRANSITION_FADE, seconds)`), with an optional
      loading scene shown while the next one loads in the background.
- [ ] Systems can be scoped to a scene (`br_system_add_for_scene`) so they stop with it.
- [ ] Tests: load/unload leaves zero live nodes/assets; shared asset survives one unload;
      unload during async load (Review Focus 1); additive scenes; persistent nodes.

### Task 17: Animation system (2D and 3D)

**Owns:** `engine/src/animation/**`, `engine/include/braty/br_animation.h`, `br_tween.h`,
takes over `engine/src/three_d/br_anim3d*.c`, `tests/test_animation.c`, `tests/test_tween.c`,
`examples/animation_demo.c` + data, golden, `docs/agents/animation.md`.

- [ ] `br_anim_clip`: tracks of keyframes stored as flat arrays (times[], values[], per-track
      interpolation: step / linear / cubic spline / bezier with easing). A track targets a
      *property path*: `"<node label path>:<property>"`, where property is a built-in
      (`position`, `rotation`, `scale`, `alpha`, `tint`, `visible`, `sprite.frame`,
      `sprite.texture`, `material.base_color`, `morph_weights`, ...) or `component.field` via
      `br_field_desc`. Works identically on 2D nodes, 3D nodes and canvas3d content.
- [ ] Clip sources: `*.anim.json` (documented in `docs/DATA_FORMATS.md`), glTF animations
      (converted on load into the same clip structure; Phase 1's 3D animator is replaced by
      this), and spritesheet animations (frame tracks), so `br_animated_sprite` becomes a thin
      convenience over a clip.
- [ ] `br_animator` component: `br_animator_play(e, node, "run")`, `_cross_fade(node, "idle", 0.2)`,
      speed, loop modes (once / loop / ping-pong / clamp), layers with weights and masks
      (upper body / lower body), additive layers, 1D/2D blend trees by parameter, animation
      events (named markers in the clip -> callback row or system-readable event queue),
      root motion extraction option.
- [ ] Optional state machine as data (`*.animgraph.json`): states, transitions with conditions
      on float/bool/trigger parameters, exit time, blend duration.
- [ ] `br_tween`: `br_tween_to(e, node, "position", value, seconds, BR_EASE_OUT_CUBIC)`, `_from`,
      `_from_to`, delay, repeat, yoyo, on_complete, sequences and parallel groups
      (`br_tween_sequence_*`), all standard easings, kill by node/tag. Tweens live in one dense
      array and are advanced by one system.
- [ ] Performance: bind tracks to targets once per (animator, clip) into index arrays; cache the
      last key index per track; sample all active animators in one system over dense arrays,
      split across worker threads above a threshold; skinning matrices written straight into
      the GPU buffer; no per-frame allocation. Benchmark in the example: 1,000 skinned
      characters and 50,000 tweened sprites, report ms/frame.
- [ ] Tests: interpolation maths, loop modes, cross-fade weights, event firing exactly once,
      binding to destroyed node (Review Focus 5), tween sequence ordering.

### Task 18: glTF as first-class scene content

**Owns:** `engine/src/three_d/br_gltf*.c`, `engine/include/braty/br_gltf.h`, glTF parts of
`engine/src/data/**`, `tests/test_gltf_scene.c`, `examples/gltf_scene_demo.c` + data, golden.

- [ ] A `.glb`/`.gltf` is directly usable as: a scene (`br_scene_load(e, "level.glb", ...)`),
      a prefab (`br_prefab_instantiate`), and a node in a scene file
      (`{ "type": "gltf", "source": "models/tree.glb" }`).
- [ ] glTF `extras` -> components: a node's extras object `{ "components": { "health": {...} } }`
      (or flat custom properties matched by registered component/field names) is applied through
      `br_field_desc`, so levels authored in Blender carry gameplay data with no engine editor.
      Name conventions for colliders/spawn points/triggers documented.
- [ ] Imports cameras (as `br_camera`), lights, all animations (as `br_anim_clip`, Task 17),
      skins, morph targets; instancing of repeated meshes (`EXT_mesh_gpu_instancing` and
      automatic batching of identical mesh+material into `br_batched_mesh3d`).
- [ ] More material extensions where the shader already supports them cheaply:
      `KHR_materials_emissive_strength`, `_unlit`, `KHR_texture_transform`,
      `KHR_materials_transmission` (alpha approximation) — list unsupported ones in the load log.
- [ ] Sub-asset addressing: `"models/kit.glb#Tree_01"` loads one node/mesh from a kit file;
      glTF textures/materials share the asset cache and ref counts (Task 16).
- [ ] Draco / meshopt / Basis-KTX2 compressed glTF: detected, clear error naming the unsupported
      extension (no C decoders; deferred).
- [ ] Tests: extras -> component values, sub-asset lookup, instance batching count, a fixture
      `.gltf` written by hand in `tests/data/`.

### Task 19: Second sample game, docs

**Owns:** `sample_projects/<name>/**`, updates to `README.md`, `AGENTS.md`, `docs/GETTING_STARTED.md`.

- [ ] A small original 3D game built on Kenney CC0 kits (GLB models, UI pack, fonts, audio),
      downloaded by a script into the sample's `assets/` with licences kept. Working title
      `crate_rush`: third-person character on small themed islands collecting crates against a
      timer; menu scene -> level scenes -> results.
- [ ] Must use, visibly: glTF levels with `extras` components, skinned character with
      animator state machine (idle/run/jump, cross-fades), tweens for pickups and UI, Unity-style
      perspective camera with follow + shake, a second UI camera, world-space canvas3d (9-slice
      name plates / signs, floating score text), screen-space 9-slice UI anchored to the safe
      area, scene switching with asset groups and a loading scene, a frame arena and a
      level-lifetime arena, vsync + FPS-cap options in a settings menu, keyboard + gamepad.
- [ ] C code is components + systems only; everything else is data files.
- [ ] Golden-image test of the menu and of a deterministic gameplay frame.
- [ ] Docs updated; `AGENTS.md` gains "how to make a game with Braty" walkthrough pointing at
      both samples.

### Task 20: Integration review (orchestrator)

- [ ] Merge, clean build, all tests + goldens, run both sample games, review against
      constraints, update README's deferred list.
