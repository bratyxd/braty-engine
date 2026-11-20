# API review: Star Courier (Phase 1, Task 12) and Crate Rush (Phase 2, Task 19)

Star Courier (`sample_projects/star_courier/`) is the first program that uses the whole engine
the intended way: game.json, a manifest with a bundle, three scene files, nine prefabs, a
spritesheet, sound sprites, input actions, ten components with field lists, eight systems.
Everything below was hit while writing it. Each item names where it lives, what happened, what
the game does instead, and a concrete fix. Items fixed later say **Resolved** with the task,
the functions and the test.

Severity: **bug** = behaves differently from its documentation; **gap** = something a game
needs that is missing; **friction** = works, but awkward or easy to get wrong.

## 0. Fixed in this task

- **Unknown keys in data files were ignored silently** (`engine/src/data/br_scene_file.c`,
  `br_data_apply_node`; `br_game_config.c`, `br_game_config_parse`). A typo such as `"posiiton"`
  loaded without a word. Now every node object of a built-in type, every text `style` object,
  and game.json (top level, `window`, `ticker`, `debug`, `audio`, each `input` action) log a
  warning `file: json.path: unknown key (ignored; a typo?)`. Not counted as errors. Test:
  `tests/test_scene_file.c`, `test_unknown_keys`. Contract change: unknown **top-level**
  game.json keys now warn; game data belongs under `"game"` (DATA_FORMATS.md updated). Custom
  node types (and `view3d`) are not checked: the loader cannot know their keys.

## 1. Boot: `br_main`, `br_app_desc`, game.json

1. **gap - `init` gets no user pointer and no command line** (`br_app.h`, `br_app_desc.init`,
   `br_app_desc.shutdown`). `br_main` runs before an engine exists, so anything it parses
   (`--autoplay`, the game.json config) can only reach `init` through a file-scope static.
   The sample keeps exactly one (`static sc_boot s_boot` in `src/main.c`); `examples/data_demo.c`
   does the same. **Fix:** add `void *user` to `br_app_desc` and `void *br_engine_app_user(e)`,
   plus `br_engine_args(e, &argc)` returning argv.
   **Resolved (Task 14):** `br_app_desc.user`, `br_engine_app_user(e)`, `br_engine_args(e, &argc)`.
   Test: `tests/test_app.c`, `test_user_and_args`.
2. **friction - the `br_game_config` must outlive `br_main`** (`br_game_config_app_desc` returns
   pointers into it; `br_game_config_apply` runs later in init). Every game needs the static
   above. **Fix:** `br_app_desc br_game_config_main(const char *path, int argc, char **argv,
   bool (*init)(br_engine *))`: the engine keeps the parsed config itself (`e->data`) and
   applies it before calling `init`; `br_main` becomes one line.
   **Resolved (Task 14):** `br_game_config_main(path, argc, argv, init)` returns a desc whose
   `config` the engine owns: settings are applied before `init`, `start_scene` is loaded after
   it (so it can use components registered in `init`), then the config is freed. Star Courier
   uses it. Test: `tests/test_app.c`, `test_game_config_main`.
3. **friction - `start_scene` assumes one scene** (`br_game_config_apply`). A game with a state
   machine (title / play / game over) loads scenes itself; the sample removed `start_scene`
   and lists its scenes under `game.scenes` (read with `br_game_config_json`). **Fix:** Phase 2
   Task 16 (`br_scene_load` by name); until then document the pattern (GETTING_STARTED does).
   **Resolved (Task 16):** top-level `"scenes": { "title": "scenes/title.scene.json" }` in
   game.json; `br_scene_load(e, "title", BR_SCENE_SINGLE)` / `br_scene_switch(e, "play", ...)`
   load by name; `start_scene` may be a name and loads as a `br_scene`. Test: `tests/test_scene.c`,
   `test_game_config_keys`. Star Courier now lists its screens there and switches with
   `br_scene_load(e, "play", BR_SCENE_SINGLE)` (Phase 2 integration).
4. **gap - assets cannot be pointed at the source tree.** game.json and `asset_root` resolve
   against the executable directory (`br_game_config_load`, `br_engine_create`). The sample
   copies `data/` next to the executable after each build (its `CMakeLists.txt`), which makes
   it run from any working directory, but hot reload then watches the *copies*: editing
   `sample_projects/star_courier/data/...` does nothing until the next build. **Fix:** an
   `--assets <dir>` option in `br_game_config_args` (and `BRATY_ASSETS` environment variable)
   that overrides `asset_root`, so `star_courier --assets ../../sample_projects/star_courier/data/assets`
   hot-reloads the sources.
   **Resolved (Task 14):** every `br_app` executable takes `--assets <dir>` (relative to the
   working directory) and the `BRATY_ASSETS` environment variable, both overriding
   `br_app_desc.asset_root` / game.json's `asset_root`:
   `star_courier --assets sample_projects/star_courier/data/assets` hot-reloads the sources.
   Test: `tests/test_app.c`, `test_assets_override`.

## 2. Scene graph and data

5. **bug - `br_scene_node_count` counts destroyed nodes** (`br_node.h` says "number of live
   nodes"; `engine/src/scene/br_node.c` returns the pool's `alive_count`, which drops only at the
   flush). Right after switching from the play scene the game logged 57 nodes, a few frames
   later 14. **Fix:** decrement a separate counter in `br_node_destroy`, or document "including
   nodes destroyed this frame".
   **Resolved (Task 16):** the count excludes nodes waiting for the flush. Test: `tests/test_node.c`.
6. **gap - scene loads leak asset references** (`br_data.c`, `br_data_texture` and the `assets`
   preload; existing `ponytail:`). Measured: `sprites/atlas.json` has 2 references after the
   first scene and 9 after three rounds (+2 or +3 per scene load; never released; the asset
   count stays 6, so nothing is reloaded or lost). `br_assets_unload` can therefore never free
   a scene's atlas. **Fix:** Phase 2 Task 16 asset groups; interim: record the paths a load
   referenced (`br_scene_file_collect_assets` already finds them) on the root and drop them in
   a `br_scene_on_node_destroy` listener.
   **Resolved (Task 16):** the outermost load of a scene file / prefab captures every reference
   it takes; a `br_scene`'s asset group or the subtree root owns them and releases them after the
   node flush. Measured: the soak's atlas count stays at 3 at every round start (bundle + old and
   new scene in the switch frame) and 2 at shutdown. Tests: `tests/test_scene.c`
   (`test_scene_file_roots`, `test_load_unload_baseline`).
7. **friction - instantiating a prefab at a position** (`br_prefab.h`). Overrides are a `cJSON`;
   a spawner wants "this prefab, here, with this velocity". The sample instantiates, then sets
   the position, then fetches the component (`systems.c`, `sc_spawn_debris`). **Fix:**
   `br_prefab_instantiate_at(e, path, parent, br_vec2 position)`; the rest stays code.
   **Resolved (Task 16):** `br_prefab_instantiate_at`. Test: `tests/test_scene.c`. Star Courier's
   crates and debris use it.
8. **friction - component pointers die on any add** (`br_component_get/at`). In a system that
   holds a `sc_pilot *` and then calls something that may instantiate a prefab (scene switch,
   spawn), the pointer silently dangles. The sample was audited by hand (`sc_rules_system`
   returns right after `sc_enter_state`). **Fix:** in debug builds keep a per-component
   "array version" bumped on growth and let `br_component_at` / `_get` callers assert it with a
   cheap `br_component_version(e, id)`; or document a "re-fetch after any create" rule in
   scene.md.
   **Resolved (Task 16, documented):** the re-fetch rule is in scene.md and data.md.
9. **friction - texture references are long.** `"sprites/atlas.json#ship.png"` appears ~25
   times in the sample's data. Manifest aliases *do* work (`"atlas#crate.png"` resolved with
   no extra reference, verified), but no doc says so. **Fix:** say it in DATA_FORMATS.md
   section 4 and data.md.
   **Resolved (Task 16):** both say it.

## 3. Input and scripted play

10. **friction - injection is key-level only** (`br_input.h`, `br_input_inject_*`). The
    autoplay bot must know that action `start` is bound to Return, send key-down, remember to
    send key-up next frame (`main.c`, `sc_bot_tap`), and there is no way to inject a gamepad
    button or axis. **Fix:** `br_input_inject_action(e, "start", value, frames)` and
    `br_input_inject_gamepad_button/axis`; Phase 2 Task 14 (playtest) builds on them.
    **Resolved (Task 14c):** `br_input_inject_action(e, action, value, frames)` holds any action
    whatever it is bound to (respects contexts); `br_input_inject_gamepad_button`,
    `br_input_inject_gamepad_axis` and `br_input_inject_gamepad_disconnect` drive pads (an empty
    slot gets an injected pad); `br_input_gamepad_attach_virtual(e, type)` adds an SDL virtual pad
    of a chosen family. Test: `tests/test_input.c`. The play-test `action` and `gamepad` commands
    use them (Task 14d; `tests/test_playtest.c`, `test_action`, `test_gamepad`).
11. **friction - "run before input latches" is a magic number.** Injected events must arrive
    before `input.begin_frame` (INPUT, priority -2000000) to count this frame; the sample uses
    -3000000. **Fix:** public constants (`BR_PRIORITY_INPUT_LATCH`, `BR_PRIORITY_TRANSFORM`, ...)
    in `br_app.h` for every engine system that games order against.
    **Resolved (Task 14c):** `BR_INPUT_PRIORITY_LATCH` in `br_input.h` (the priority of
    `input.begin_frame`); inject from a system at `BR_INPUT_PRIORITY_LATCH - 1000`. Test:
    `tests/test_input.c`. The other engine priorities are still plain numbers (listed per module
    in `docs/agents/`).
12. **gap - rumble needs a slot, actions are slot-less** (`br_input_gamepad_rumble`). Actions
    read every pad, so the game cannot tell which pad to rumble; the sample rumbles slot 0.
    **Fix:** `br_input_action_last_gamepad(e, action)` or `br_input_gamepad_rumble_all`.
    **Resolved (Task 14c):** `br_input_action_last_gamepad(e, action)` returns the slot that last
    drove the action (-1 if none), so `br_input_gamepad_rumble(e, slot, ...)` rumbles the right
    pad. Test: `tests/test_input.c`.

## 4. Systems and game flow

13. **gap - no way to pause a group of systems.** Pause, the title screen and game over all
    need "gameplay systems off, UI systems on". `br_system_set_enabled` works one name at a
    time, `br_ticker_set_speed(0)` freezes the UI too; every gameplay system in the sample
    checks `g->state` / `g->paused` itself. **Fix:** an optional group name in `br_system_add`
    (or a `br_system_set_group`) and `br_system_group_set_enabled(e, "gameplay", false)`;
    Phase 2 Task 16's `br_system_add_for_scene` is the same mechanism.
    **Resolved (Task 14):** `br_system_set_group(e, name, "gameplay")` and
    `br_system_group_set_enabled(e, "gameplay", false)`; a system runs when it and its group are
    both enabled, and systems joining a disabled group start disabled. Test: `tests/test_app.c`,
    `test_system_groups`. (Star Courier still checks its state itself; switching it over is optional.)
14. **friction - bundle progress cannot report failure** (`br_asset_progress_fn`). The last
    call has progress 1 "failures included", so the game learns about a missing music file only
    when `br_sound_load` logs. **Fix:** add `uint32_t failed` to the callback (or
    `br_assets_bundle_failed(e, name)`).
    **Resolved (Task 16):** `br_assets_bundle_failed(e, name)`; asset groups report `failed` in
    their done callback. Test: `tests/test_asset_group.c`.

## 5. Rendering features

15. **gap - no particle emitter** (`br_particles.h`). Every game has to write one. `br_particle`
    has no velocity/age, so the sample keeps parallel arrays (age, velocity, colour) of a fixed
    256 slots inside its `emitter` component (`game.h`, `sc_emitter`, ~5 KB per emitter) and
    pre-allocates all particles with alpha 0, because `br_particle_container_remove` is ordered
    (O(n) per removal). **Fix:** an engine `particles.emitter` component with JSON (rate, burst,
    life, speed, spread, scale and colour over life, follow node) updated by one system, and a
    swap-remove (`br_particle_container_remove_swap`).
16. **friction - filters are addressed by index** (`br_filters_get(e, n, i)`). The hit flash
    toggles two filters declared (disabled) in `play.scene.json`; the code relies on the blur
    being first and the colour matrix second. Reordering the JSON breaks the game silently.
    **Fix:** an optional `"name"` per filter in JSON and `br_filters_find(e, n, "flash_blur")`.
17. **gap - no fixed logical resolution.** The game is laid out for 1280x720; with a resizable
    window everything would sit in the top-left corner. The sample sets `resizable: false` and
    hard-codes 1280x720 for spawn edges and wrapping (headless `br_engine_window_size` is 0x0).
    **Fix:** `window.logical_size` in game.json: the stage gets a letterbox scale + offset and
    `br_engine_logical_size(e)` returns the design size (also for input mapping).
    **Resolved (Task 15 + Phase 2 integration):** `br_camera_create_logical(e, w, h)` is a
    letterbox camera (the stage is always w x h units); game.json `"window": {"logical_size":
    [w, h]}` creates it, and pointer events and play-test `click` map through cameras
    (`br_camera_screen_to_stage` / `_stage_to_screen`). No `br_engine_logical_size`: with the
    camera the stage *is* the design size. Tests: `tests/test_scene.c`
    (`test_game_config_profiler_logical`), `tests/test_camera.c` (`test_pointer_events`),
    `tests/test_playtest.c` (`test_click_with_camera`). Star Courier keeps its fixed window.
18. **friction - scaled text blurs and JSON cannot ask for SDF.** UI pulses scale text up to
    1.5x; coverage glyphs blur (known text gotcha). SDF fonts are code-only
    (`br_font_options.sdf`), there is no `style` key for it. **Fix:** `"sdf": true` in the JSON
    text style (data module creates/caches an SDF variant of the family).
19. **friction - sound sprites live outside bundles** (`br_game_config.c`,
    `br_config_apply_audio`). `audio.sprites` loads its sounds synchronously in
    `br_game_config_apply`, so a sound with sprites cannot be in a background bundle. **Fix:**
    accept Pixi's manifest `data` field (`{ "alias": "sfx", "src": "sounds/sfx.wav", "data": {
    "sprites": { ... } } }`) and apply sprites when the bundle loads it.

## 6. Tooling and diagnostics

20. **gap - no heap statistics.** `SDL_GetNumAllocations()` returns -1 with the prebuilt SDL
    (allocation counting compiled out) and `br_alloc` (`engine/src/core/br_memory.c`) keeps no
    counters, so the soak test can compare node/texture/asset counts but not heap use.
    **Fix:** count allocations and bytes in `br_alloc/br_realloc/br_free` (a 16-byte size header
    in debug builds), expose `br_memory_stats(e)` and show it in the debug Stats panel; Phase 2
    Task 14 (arenas) adds per-arena numbers on top.
    **Resolved (Task 14):** `br_alloc/br_realloc/br_free` keep a size header and spinlock-guarded
    counters; `br_memory_get_stats(&s)` (process-wide: live blocks/bytes, total allocations, peak)
    is shown in the Stats panel with every tracked arena. `br_file_read_all` now returns a
    `br_alloc` block and audio frees SDL-converted samples with `SDL_free`, so no pointer crosses
    allocators. Test: `tests/test_arena.c` (`test_heap_stats`).
21. **friction - the debug frame graph is red at 60 Hz** (`engine/src/debug/br_debug_panels.c`,
    fixed `ms > 17.0f`). With vsync at 60 Hz, ordinary 16.6-17.2 ms frames show as red bars.
    **Fix:** compare against the display's refresh period (`SDL_GetCurrentDisplayMode`) + 10%.
    **Resolved (Task 14):** `br_app_frame_budget_ms(e)` (FPS cap, or the display refresh with
    vsync on, whichever is slower; 1000/60 otherwise) and the graph turns red above it + 10%.
    Test: `tests/test_pacing.c`, `test_frame_budget`.
22. **friction - examples only work from the default build directory** (`examples/*_demo.c`
    set `asset_root` / game.json to `../../examples...` relative to the executable). A build in
    any other directory (`cmake -B out/x`) cannot find the data. Fixed for the sample by the
    post-build copy (see 4); the root `CMakeLists.txt` could copy `examples/` data the same way
    (or pass the source path as a compile definition).
    **Resolved (Task 14):** CMake passes `BRATY_EXAMPLES_DIR` (the source `examples/` path) to
    every example, which uses it as asset root / game.json path; `data_demo` also moved to
    `br_game_config_main`. Verified by running data, text, assets, glTF and audio demos from
    `%TEMP%` (screenshots written; the golden-image run covers them from then on).

## What worked well (keep it)

- Components with field lists + prefabs + label references: the ship's `cargo_icon`, the
  station's `halo` and the thrust emitter's `follow` are wired in data and resolved for free.
- Tuning as a component loaded from a tiny scene file (`assets/tuning.json`): every number has
  a JSON path in its error messages and hot-reloads in debug builds.
- Data-declared, disabled filters toggled by a system: no allocation, no effects pass while
  off (verified: disabled filters skip the offscreen path).
- `br_text_set_text` skips unchanged text, so HUD systems can set every frame.
- Determinism: fixed-step `--frames` + a seeded game RNG gave identical D3D12 and Vulkan
  screenshots of the same frame.

---

# Phase 2 review: Crate Rush (Task 19)

Crate Rush (`sample_projects/crate_rush/`) is the first 3D game: three `.glb` levels loaded as
scenes with extras components, a skinned character driven by an animation graph, fixed-step
movement against the levels' `-colonly` boxes, a `br_camera` with follow and shake, canvas3d name
plates and popups, a HUD and five UI scenes through a persistent 2D UI camera, settings with live
vsync / FPS cap / volume / profiler HUD and jump rebinding, a loading scene, two play-test scripts
and two golden images. No engine code was changed for it: every item below is worked around in
the game (the workaround is named) and left here with a proposed fix.

## 7. glTF scenes and cameras

23. **gap - a `.glb` scene's camera cannot follow anything** (`br_gltf.h`: imported cameras are
    `camera3d` nodes; `br_camera_create_3d` builds its own View3D). The game wants the level's
    framing plus `camera.follow` / `camera.shake`. It creates a `br_camera` over
    `br_scene_root_3d`, copies the file camera's FOV / near / far and pose, sets the follow offset
    to `eye - look`, and **hides the `.glb`'s own view** (`br_node_set_visible`), or the level is
    rendered twice (`src/flow.c`, `cr_level_setup`). **Fix:** `br_camera_adopt_view3d(e, view,
    desc)` turning an existing View3D (with its environment, post and MSAA) into a `br_camera`
    whose rig starts at the view's current camera; and `br_gltf_options.camera_as_br_camera` so a
    scene load does it, labelling the camera node after the glTF camera.
24. **gap - View3D settings are write-only** (`br_view3d.h`: `br_view3d_set_environment`,
    `_set_background`, `_set_msaa` have no getters). Because of 23 the camera's view needs the
    level's sky / ground / ambient / background, which only the scene extras have, so the game
    parses `model->scene_extras` again (`cr_extras_color`). **Fix:** `br_view3d_environment(e,
    view, &sky, &ground, &intensity)`, `br_view3d_background`, `br_view3d_msaa`, and a
    `br_view3d_copy_settings(e, from, to)`.
25. **gap - scene extras cannot carry components** (`engine/src/data/br_gltf_extras.c`,
    `br_data_gltf_apply_extras` reads node extras only). Level settings (title, time limit,
    footstep sounds, kill height) are a `"level"` object in the scene extras; the game reaches
    them with `br_model3d_model(...)->scene_extras`, `cJSON_Parse` and
    `br_scene_file_apply_component` on the view node. **Fix:** apply registered component keys
    of the scene extras (both the nested and the flat form) to the instance root as 3D
    components, exactly like node extras.
26. **gap - glTF clips cannot get events from data** (`br_animation.h`: markers come from
    `*.anim.json` clips only; `br_animator_add_model_clips` shares the model's clips read-only).
    Footsteps need "step" markers on `walk` and `sprint`, so the game copies the clips with
    `br_anim_clip_from_gltf`, adds markers from tuning.json and owns the copies - and must remove
    the animator before freeing them at shutdown (`src/main.c`, `cr_make_clips`,
    `cr_shutdown`). **Fix:** an `"events"` table in `*.animgraph.json` (`{ "clip": "sprint",
    "events": [[0.11, "step"], [0.36, "step"]] }`) applied when the graph binds, or
    `br_animator_add_marker(e, a, clip, time, name)` that stores per-animator markers.
27. **gap - `br_query` is 2D only** (`br_query.h`). The settings rows use it (button + setting),
    but every gameplay system walks a 3D component array and calls `br_component3d_get` for the
    others (`src/world.c`, `src/player.c`). **Fix:** `br_query3d_begin(e, ids, count)` with the
    same deferral rules over the 3D rows.
28. **friction - resizing a canvas3d to its content is manual** (`br_nine_slice3d_create` fixes
    the size). A sign whose text changes must resize the nine-slice, the canvas desc
    (`width`, `height`, which the pivot uses) and re-centre the text (`src/world.c`,
    `cr_sign_layout`). **Fix:** `br_canvas3d_fit_content(e, n, padding)` measuring the root's
    bounds; `br_text3d_create` could do it on every text change.
29. **friction - SDF fonts are still code-only** (item 18 is open). World text uses Kenney
    Future as SDF; the game reads the TTF with `br_file_read_all` and calls `br_font_create`
    with `sdf = true`, and destroys it itself. **Fix:** as in 18, plus `br_asset_options` font
    fields (`sdf`, `sdf_size`) so `br_assets_load_with` and manifests can ask for it.

## 8. Scenes, assets, audio

30. **friction - a scene that is one asset has binary progress** (`br_scene_progress` counts
    assets). A level is a single `.glb`, so the loading bar jumps from empty to full; the game
    eases the bar toward the target (`src/ui.c`, `cr_loading_system`). **Fix:** weight group
    progress by file size and report decode progress of large assets (the glTF worker knows
    which buffers and images are done).
31. **friction - `br_scene_switching` stays true through the fade-in.** Menus that ignore input
    while a switch runs (to avoid clicking a screen that is going away) also drop the first
    presses on the new screen. **Fix:** `br_scene_switch_phase(e)` (out / loading / in), or make
    `br_scene_switching` false once the target is active and only the fade-in remains.
32. **friction - playing a sound by path takes a reference each time** (`br_sound_load` is a
    cached `br_assets_load`). A game that names sounds in data must use `br_assets_get` (no
    reference) and keep the sounds alive with a group (`src/flow.c`, `cr_play`). **Fix:**
    `br_audio_play_path(e, path, desc)` that plays a loaded sound without touching references
    (and logs once if it is not loaded).
33. **friction - per-level assets are added after the scene loaded.** The footsteps of a level's
    theme are named in its scene extras, so they cannot be in the scene's group before the `.glb`
    is parsed; the game adds them to `br_scene_assets(level)` on activation and loads them
    synchronously. It works (they are released with the level; the soak checks it) but they miss
    the loading screen. **Fix:** let scene extras list `"assets"` like a scene file's root, read
    when the model is decoded, before the scene is built.
34. **friction - deterministic runs with an audio device play voices on the wall clock**
    (`ponytail:` in audio). The soak checks that sound instances return to zero on the menu;
    with a window frames run uncapped, so the check needs a long `wait_until` timeout. **Fix:**
    under `--deterministic`, advance voices by game time even with a device open (and feed the
    device the mixed result), so counts are frame-exact.

## 9. Input and play-testing

35. **friction - an action that becomes live while held stays idle** (`br_input.h`, contexts).
    By design (closing a menu with Space does not jump), but a script that pushes the stick in the
    frame the game switches to the "gameplay" context sees no movement until release;
    `round.json` waits 5 frames after the level starts. **Fix:** keep the rule for buttons, but
    let axis bindings (sticks) be live at once; document the frame in `docs/PLAYTESTING.md`.
36. **bug - play-test paths cannot name components with a dot**
    (`engine/src/playtest/br_playtest_query.c`): `level_camera:camera.follow.offset3d` fails with
    "too many parts", so engine components (`camera.follow`, `camera.anchor`, `gltf.collider`)
    are unreachable. The game added a `camera_distance` tuning value to adjust the camera from a
    script instead. **Fix:** after the colon, match the longest registered component name, then
    the field, then an optional element.
37. **friction - 2D labels are global in play-test paths.** `jump_row/value:text` is rejected
    ("no view3d labelled jump_row"); every settings row has a child labelled `value`, so a script
    can only read the first. **Fix:** accept `parent/child` paths for 2D nodes the way 3D paths
    already work.
38. **gap - an agent cannot resize the window.** Checking 21:9 and 9:16 meant copying the build
    output and editing its game.json. **Fix:** a `--window WxH` flag in `br_app` and a play-test
    command `{"cmd": "window", "width": 2520, "height": 1080}` (`SDL_SetWindowSize`, then wait a
    frame), so aspect ratios and resizes can be golden-tested.
39. **friction - golden bases count `wait` frames only** (`tools/golden.py`, `script_for`). A
    script with `wait_until` cannot be a golden base, so Crate Rush keeps a second, wait-only
    input script (`playtest/golden_play.json`). **Fix:** let a base script contain a
    `{"cmd": "screenshot", "golden": true}` marker and take the golden there.
40. **friction - `tools/playtest.py` short commands accept anything after the key name.**
    `"key Up frames=40"` became a key command with action `"frames=40"` (an error reply, not a
    40-frame hold). **Fix:** parse `key NAME [tap|down|up] [FRAMES]` and reject unknown words
    before starting the game.
41. **friction - `br_animation_stats` counts an animator only once it has been sampled.** On the
    first frame of a level the count is 0 although `br_animator_add` ran; the soak checks it a few
    frames later. **Fix:** report live animators (pool size) and sampled animators separately.

## What worked well in Phase 2 (keep it)

- A level as a `.glb` with extras is a complete level format: spawn, crates, hazards with motion
  parameters, signs with text, `-colonly` collider empties and settings, all filled through
  `br_field_desc` with no level-loading code. The build script writes what Blender would.
- The `gltf.collider` convention plus a dense AABB array in a linear arena was all the physics
  the game needed (11-20 boxes per level; the candidate list comes from the frame arena).
- `*.animgraph.json` with a blend tree (walk / sprint by speed) and a jump trigger needed no code
  beyond setting four parameters; cross-fades and "jump from any state" just work.
- Tweens with easing names from tuning.json (pickup pops, popups, score counters through the
  component field `counter.value`, focus scaling) and the `tween` component in scene files for
  every screen's entrance.
- Glyphs: one atlas named per the `br_input_glyph` scheme, and `{jump}` in sign text and HUD
  strings switch between Space, A and Cross as the device changes.
- Scenes + groups: the soak shows node, 3D node, asset, animator, tween and sound counts back at
  their baseline after nine level runs; `br_assets_report` on every menu visit lists only the
  shared group and the menu's own assets.
- Play-testing with `wait_until` on `*/player:position.x`: the round script walks to waypoints
  instead of counting frames, so it survives tuning changes.

# Full review 2026-11-20

The whole-engine review (`docs/plans/2026-11-20-engine-review.md`, report R8) read the public
headers as one API. These are its API findings, ranked: worth fixing first at the top. None is
implemented yet; each has a recommended resolution. Items 36, 23-25 and 38 above are ranked by the
same review as the most valuable open ones from the earlier lists.

42. **friction - two ways to do one thing.** (a) `br_animator3d_*` (`br_anim3d.h`) is a legacy
    shim over the unified `br_animator`, still public and still used by scene JSON
    (`br_node_json_view3d.c`, `"animation"`), although Phase 2 Task 17 says it is replaced.
    (b) `br_orbit_camera_*` (`br_camera3d.h`) next to the `camera.orbit` component.
    (c) `br_ticker_set_max_fps` next to `br_app_set_max_fps` (`br_app.h`).
    (d) `br_color_srgb_to_linear` (`br_material3d.h`) next to `br_color_to_linear` (`br_math.h`).
    (e) `br_engine_random` / `_random_float` / `_set_seed` (`br_app.h`) next to
    `br_random_engine(e)` + `br_random_*` (`br_random.h`). Every duplicate doubles what an agent
    reads and must choose between. **Fix:** delete (c)-(e) now (one caller each); mark (a) and (b)
    deprecated in their headers, move scene JSON `"animation"` to `br_animator`, delete them in
    the next phase. Rank 1: cheap, and the cost grows with every game written.
43. **friction - component fields are too narrow for data-driven games** (`br_node.h`,
    `br_field_type`). Types are bool / int / float / vec2 / vec3 / color / string / asset / node.
    Strings are `char[BR_FIELD_STRING_SIZE]` = 64, so 63 UTF-8 bytes: Crate Rush's sign `text`
    does not survive a German or Russian translation. No arrays (waypoints, loot tables, dialogue
    lines), no enum by name, no uint, vec4 / quat or `br_node3d` field. Anything beyond is parsed by
    hand from cJSON, the opposite of "data + systems". **Fix:** `BR_FIELD_TEXT` (a heap or interned
    string freed with the row), `BR_FIELD_ENUM` with a name table, `BR_FIELD_VEC4` / `_QUAT`, a
    `BR_FIELD_NODE3D`, and a fixed-capacity array field (count + inline elements of one type).
    Rank 2: blocks localisation and every list-shaped design.
44. **friction - 2D and 3D node APIs disagree** (`br_node.h` vs `br_view3d.h`). Position takes
    `(x, y)` in 2D and a `br_vec3` in 3D; children by index in 2D (`br_node_child_at`) vs
    first-child / next-sibling in 3D; `br_node_find_by_label(e, root, label, deep)` excludes root,
    `br_node3d_find_by_label(e, root, label)` includes it and is always deep; layer bits are
    "0 = inherit" in 2D (`br_node_set_layer_bits`) and "default 1, no inherit" in 3D
    (`br_node3d_set_layer`); counts are `br_scene_node_count` vs `br_node3d_count`; destroy hooks
    `br_scene_on_node_destroy` vs `br_node3d_on_destroy`. 3D lacks reparent-keeping-world,
    add-child-at, world-to-local and an Euler getter. 3D components are declared in `br_gltf.h`
    although they have nothing to do with glTF. An agent writing 3D code by analogy with 2D gets
    each of these wrong once. **Fix:** add the missing 3D functions; give find-by-label the same
    root / deep rule in both; rename the odd ones keeping the old names as one-line deprecated
    wrappers for one phase; move the `br_component3d_*` block to `br_view3d.h`. Rank 3.
45. **friction - "scene" means two things** (`br_node.h`, `br_scene.h`). `br_scene_root(e)` is
    the stage (every node), `br_scene_root_2d(e, scene)` one loaded scene's root,
    `br_scene_node_count` counts all nodes; modules "scene", "scenes", "scene_file" and "data"
    (`br_data_register` lives in `br_scene_file.h`) are four names for overlapping things.
    **Fix:** `br_stage(e)`, `br_node_count(e)`, `br_node_on_destroy(e, ...)` with the old names
    kept as deprecated one-liners; "scene" then only means a loaded scene (`br_scene`). Rank 4.
46. **gap - pausing a game is incomplete** (`br_app.h`, `br_tween.h`, `br_animation.h`). System
    groups pause game systems, but tweens, animators, particles and audio are engine systems that
    keep running; `br_ticker_set_speed(0)` freezes the UI too, and there is no unscaled delta.
    Crate Rush pauses its animator by hand. **Fix:** an unscaled delta for UI and an
    `unscaled` flag (or a time group) on tweens and animators. Rank 5.
47. **friction - two mutation styles.** 2D nodes, audio and input use setters; `br_camera_get`,
    `br_light3d_get`, `br_canvas3d_get`, `br_material3d_params`, `br_view3d_post` and
    `br_animator3d_get` return a mutable struct pointer whose lifetime differs per module. An
    agent cannot tell whether an edit needs a "dirty" call or whether the pointer survives the
    next create. **Fix:** state the rule once in each header with the same wording ("valid until
    the next create or destroy of this type; edits apply next frame"); long term one style per
    kind of object. Rank 6: doc-only first step.
48. **friction - two RNG and two colour helpers** are items 42 (d) and (e); listed here so a
    search for "random" or "linear" finds them.
49. **friction - getters are named three ways**: bare noun (`br_node_alpha`), `_get_`
    (`br_text_get_text`, `br_node_get_bounds`, `br_view3d_get_stats`) and `_get` returning a
    pointer (`br_camera_get`). **Fix:** bare noun for values, `_get` only for pointers into a
    table; rename with deprecated wrappers. Rank 7.
50. **friction - `br_node_set_blend_mode` takes `int`** because the advanced modes live in
    `br_filter.h`. **Fix:** one `br_blend_mode` enum in `br_node.h`. Rank 8.
51. **friction - path bases differ:** game.json and the asset root are relative to the
    executable, `--assets` / `--screenshot` / `--record` / `--profile` to the working directory,
    `br_file_*` to the asset root. **Fix:** document the three in `br_app.h` in one table; long
    term route all file access through one storage layer (title storage for data, user storage
    for saves). Rank 9.
52. **gap - audio has no volume getters and no global pause** (`br_audio.h`). Settings screens
    keep their own copy of bus volumes. **Fix:** `br_audio_bus_volume`, `br_audio_master_volume`,
    `br_audio_set_paused`. Rank 10.
