# API review from the first real game (Star Courier, Task 12)

Star Courier (`sample_projects/star_courier/`) is the first program that uses the whole engine
the intended way: game.json, a manifest with a bundle, three scene files, nine prefabs, a
spritesheet, sound sprites, input actions, ten components with field lists, eight systems.
Everything below was hit while writing it. Each item names where it lives, what happened, what
the game does instead, and a concrete fix. Nothing here was fixed in the engine except item 0.

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
   `test_game_config_keys`.
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
   **Resolved (Task 16):** `br_prefab_instantiate_at`. Test: `tests/test_scene.c`.
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
11. **friction - "run before input latches" is a magic number.** Injected events must arrive
    before `input.begin_frame` (INPUT, priority -2000000) to count this frame; the sample uses
    -3000000. **Fix:** public constants (`BR_PRIORITY_INPUT_LATCH`, `BR_PRIORITY_TRANSFORM`, ...)
    in `br_app.h` for every engine system that games order against.
12. **gap - rumble needs a slot, actions are slot-less** (`br_input_gamepad_rumble`). Actions
    read every pad, so the game cannot tell which pad to rumble; the sample rumbles slot 0.
    **Fix:** `br_input_action_last_gamepad(e, action)` or `br_input_gamepad_rumble_all`.

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
