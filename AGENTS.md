# AGENTS.md - working on (or with) the Braty Engine

Braty is a cross-platform, editor-less, data-driven 3D-first game engine with excellent 2D, in
**pure C11** on **SDL3 and the SDL3 GPU API**: the feature set of PixiJS v8 (plus `@pixi/3d`),
glTF as the main 3D format (a `.glb` is a scene, a prefab or a kit; node extras become
components), Unity-style cameras and scenes, one animation system for 2D and 3D, arenas, a
profiler and scriptable play-testing. One static library, `braty`. All scene data lives in flat
tables inside a `br_engine`; behaviour is a list of systems (plain functions) run in phase order
every frame. Games are JSON data + component structs + systems.

Plans and feature maps: `docs/plans/2026-11-19-braty-engine.md` (Phase 1),
`docs/plans/2026-11-19-braty-engine-phase2.md` (Phase 2). What works and what does not:
`README.md` (Status, Limits).

## Build, test, look at it

```sh
cmake --preset windows-gcc [-DCMAKE_PREFIX_PATH=<SDL3 prefix>]   # default prefix: .deps/sdl3
cmake --build --preset windows-gcc                                  # engine, tests, examples, both samples
ctest --preset windows-gcc                                          # unit + play-test + golden images
build/windows-gcc/hello_quad.exe --frames 3 --screenshot build/out.png [--gpu-driver vulkan|direct3d12]
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --level level1 --frames 120 --screenshot build/crate.png
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --headless --playtest sample_projects/crate_rush/playtest/round.json
build/windows-gcc/sample_projects/star_courier/star_courier.exe --playtest sample_projects/star_courier/playtest/round.json
python tools/golden.py run --only crate_rush_play                   # one golden image (docs/TESTING.md)
python tools/check_getting_started.py                               # builds and runs every step of the guide
cmake --preset windows-gcc-release && cmake --build --preset windows-gcc-release   # optimised, profiler off (docs/BUILDING.md)
```

Every app built on `br_app` takes `--frames N --screenshot out.png` (fixed 1/60 s steps, exits 0),
plus `--vsync`, `--max-fps`, `--deterministic`, `--headless`, `--assets DIR` and the play-test
flags (`docs/agents/app.md`). **Open the PNG before claiming a visual feature works**, on both
`direct3d12` and `vulkan`. How to test, and the rule below: `docs/TESTING.md`.
Examples and tests read their data from the source tree (`BRATY_EXAMPLES_DIR` /
`BRATY_SOURCE_DIR`, set by CMake), so they work from any working directory and any build directory; the sample games copy their data next to the
executable (`--assets <dir>` or `BRATY_ASSETS` points any app at another asset root). Each sample
has `playtest/round.json` (CTest runs it headless) and `playtest/soak.json` (run by hand with a
window: Crate Rush's cycles all levels and checks counts return to baseline). Details:
`docs/BUILDING.md`, other platforms: `docs/PLATFORMS.md`.

## Play-testing a game

To play a game yourself (look at a screenshot, decide, send the next input) use
`tools/playtest.py`; it runs the game with `--playtest-stdin`, where frames advance only when you
say so. From the repository root:

```sh
python tools/playtest.py build/windows-gcc/sample_projects/star_courier/star_courier.exe "step 60" "action start" "step 30" "screenshot build/playtest_shot.png" "get ship:position" quit
python tools/playtest.py build/windows-gcc/sample_projects/crate_rush/crate_rush.exe "step 30" "action confirm" "step 120" "key W down" "step 30" "key W up" "screenshot build/crate_shot.png" "get */player:position" "get game:crates" quit
```

Then open the PNG. Commands: `step N`, `key Space`, `action start`, `click
<label>`, `get <label>:<property>` / `<label>:<component>.<field>` / `game:<field>` /
`engine:scene`, `set <path> <json>`, `dump`, `profile [N]` (then `profile:frame_ms_p95`...),
`screenshot FILE`, `quit`; scripts (`--playtest file.json`) add `expect` and
`wait_until` and exit 1 on failure; `--record file.json` turns a played session into a script.
Everything, with the Python class: `docs/PLAYTESTING.md`.

## Profiling

`--profiler-hud` shows FPS, CPU ms, GPU ms (estimated from fences) and the verdict (CPU update /
CPU render submit / GPU / vsync / FPS cap) in a corner; F2 opens the panel; `--profile
build/trace.json` writes a Chrome trace + `build/trace.json.txt`. Measure with `--vsync off`,
outside play-test mode (play-tests are deterministic: the HUD shows placeholder times, but the
`profile` command's numbers are real):

```sh
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --level level3 --vsync off --profiler-hud --frames 600 --screenshot build/hud.png
build/windows-gcc/sample_projects/star_courier/star_courier.exe --vsync off --frames 600 --profile build/trace.json
```

Your own code: `br_profile_begin(e, "name")` / `br_profile_end(e)`. `docs/PROFILING.md`.

## How to make a game with Braty

1. **Learn the pattern:** `docs/GETTING_STARTED.md` - a 2D game in eleven steps (the first two only write files), then a
   3D track (a `.glb` level as the scene, components from glTF extras, a system over 3D
   components, a following camera, a world-space label). `tools/check_getting_started.py` builds
   every step, so the code there is known to compile.
2. **Copy a sample's shape.** Both samples have the same layout:
   - `data/game.json` - window, asset root, manifest, scene names (`"scenes"`, `.glb` paths
     allowed), input actions (with contexts), audio buses; game settings under `"game"`.
   - `data/assets/` - scene files (one per screen), prefabs, atlases, a manifest bundle, and
     `tuning.json` (every gameplay number, loaded as a component: errors carry JSON paths, edits
     hot-reload).
   - `src/game.h` - component structs + the ONE game-state struct handed to systems as `user`.
   - `src/components.c` - a `br_field_desc` list per component (`br_component_register`).
   - systems in `src/*.c`, each file a group (input, rules, movement, UI...).
   - `src/main.c` - `br_main` is `return br_game_config_main("game.json", argc, argv, init);`;
     init registers components and systems and exposes game state (`br_playtest_expose`).
   - `playtest/round.json` registered with CTest; `CMakeLists.txt` = `add_subdirectory(<braty>)`,
     link `braty::braty`, copy `data/` next to the executable.
3. **2D: `sample_projects/star_courier/`** - scene files switched by a state machine, prefabs
   spawned at runtime, particles, a filter flash, sound sprites, a recorded play-test.
4. **3D: `sample_projects/crate_rush/`** - read `src/flow.c` first (states; a `.glb` level
   becomes a scene, its scene extras a `level` component, a `br_camera` follows the player),
   then `src/player.c` (fixed-step movement + box collision from `-colonly` nodes, animator
   parameters for `*.animgraph.json`, footsteps from animation events), `src/world.c` (crates,
   hazards, signs as 3D components from extras; canvas3d plates and popups; tweens) and
   `src/ui.c` (buttons, focus, prompts, settings, HUD; `br_query`). Levels come from
   `sample_projects/crate_rush/tools/build_assets.py` (what a Blender export would contain);
   Kenney assets from `sample_projects/crate_rush/tools/fetch_assets.py`.
5. **Test it like the samples:** a `round.json` with `expect` (state, score, positions,
   `engine:scene`), a soak that checks counts return to baseline, a golden image of a
   deterministic frame (`tools/golden.py`), and look at screenshots on both GPU drivers.

Known rough edges (and their workarounds) are in `docs/API_REVIEW.md`.

## Code rules (hard requirements)

- C11 only. No C++, no compiler extensions, no `_Generic`, no X-macros, no token pasting, no
  macros that generate functions or types, no `setjmp`. Only trivial constant macros plus
  `BR_ARRAY_COUNT(a)` and `BR_CHECK` (tests).
- Straightforward code: plain structs, plain functions, plain `for` loops. If a reader must stop
  and decode a line, rewrite it.
- Zero warnings with `-Wall -Wextra -Wpedantic` (and `/W4` on MSVC) in engine code.
- Naming: functions `br_<module>_<verb>[_<noun>]`, types `br_<name>`, enum values
  `BR_<MODULE>_<VALUE>`, files `br_<module>.h/.c`. Public headers in `engine/include/braty/`,
  private headers next to their `.c` with suffix `_internal.h`.
- No hidden global state: every function takes `br_engine *e` (or a sub-object such as `br_gpu *`).
- Errors: return `bool` (true = ok) or a handle/pointer (0/NULL = failure) and call
  `br_log_error(...)` with the reason. Never abort on bad user data.
- Only SDL3 and C standard math/string functions. No `<windows.h>`, no POSIX, no `fopen`
  (use `br_file_*`). No platform `#ifdef`s.
- Shaders are compiled offline (`tools/compile_shaders.py`); the runtime never compiles source.
- Comments explain why, briefly. Every public function has a one-line doc comment.
- Deliberate shortcuts get a `// ponytail:` comment naming the limit and the upgrade path.
- Git: small commits, imperative subject prefixed by module (`gpu: add readback`), `git add`
  specific paths only.
- **Every behaviour change ships with a test that fails without it.** Logic -> `tests/test_*.c`;
  game flow -> a play-test script; anything visible -> a golden image (`tools/golden.py`). Update
  a golden only after looking at the new image and the diff (`docs/TESTING.md`).

## The data-driven model

- **Systems** (`br_app.h`): `br_system_add(e, name, phase, priority, fn, user)`. Phases: INPUT,
  FIXED_UPDATE (0..N times per frame at `ticker.fixed_hz`; `br_app_fixed_alpha` interpolates),
  UPDATE, LATE_UPDATE, PRE_RENDER (always run) then RENDER, POST_RENDER (only when a frame can be
  drawn). Groups (`br_system_set_group`, `br_system_group_set_enabled`) pause sets of systems.
  Engine work (transforms, animation, cameras, culling, rendering) is systems too.
- **Nodes** (`br_node.h`): handles `br_node { index, generation }` into the 2D node table
  (hierarchy, transform, alpha, tint, visibility, layer bits). Each node has a type; per-type data
  lives in a dense pool. Stale handles are detected by generation and ignored.
- **3D nodes** (`br_view3d.h`): `br_node3d` handles into a second table (TRS, world matrix,
  layers), drawn by a View3D (a 2D node) or a 3D `br_camera`. glTF instances, meshes, lights,
  cameras and canvas3d are 3D node types.
- **Components** (`br_node.h`, `br_gltf.h`): user structs described field by field
  (`br_component_desc`) so scene files, prefabs and glTF extras can fill them. One registry; 2D
  rows (`br_component_count/at/owner_at`, `br_query` for several at once) and 3D rows
  (`br_component3d_count/at/owner_at`).
- **Scenes** (`br_scene.h`): a scene file or a `.glb`, loaded single / additive / async or
  switched with a fade and a loading scene; it owns its nodes and an asset group.
- **Assets** (`br_assets.h`, `br_asset_group.h`): loaders per file extension, a cache keyed by
  path, reference counts; groups and manifest bundles load sets.

The contracts are fixed in the plan under "Core contracts".

## How to add ...

- **A system**: write `static void my_system(br_engine *e, float dt, void *user)`, register it in
  your init with `br_system_add`. Keep state in a struct passed as `user`; free it with
  `br_engine_on_shutdown`. See `docs/agents/app.md`.
- **A node type**: fill a `br_node_type_desc` (name = JSON "type", data_size,
  callbacks for create/destroy/bounds/render/hit_test/from_json) and call
  `br_node_type_register` from your module's `br_<module>_register(e)`. See `docs/agents/scene.md`.
- **A 3D node type**: fill a `br_node3d_type_desc` (name, data_size, on_create / on_destroy,
  local_bounds, `collect` submitting `br_render3d_submit` / `_submit_custom` draws, `pick`,
  from_json) and call `br_node3d_type_register`. See `docs/agents/three_d.md` (the axes example)
  and `engine/src/canvas3d/br_canvas3d.c` (a custom-draw type).
- **A component**: a plain struct + a `br_field_desc` array + `br_component_register`; it can then
  be filled by scene files (`"components"`), prefabs and glTF node extras (`"crate.value": 10`).
- **An asset loader**: fill `br_asset_loader_desc` (type name, extensions, load,
  unload) and call `br_assets_register_loader`. See `docs/agents/assets.md`.
- **A shader**: `engine/shaders/<name>.<vert|frag>.hlsl`, run `python tools/compile_shaders.py`,
  include `<name>.<stage>.h`, use `br_shader_<name>_<stage>` in a `br_pipeline_desc`.
  Rules in `docs/BUILDING.md` and `docs/agents/gpu.md`.
- **An animation property** (so clips and tweens can drive it): a `br_anim_property_desc`
  (name, 2D or 3D target kind, value type, get / set) and `br_anim_property_register`; then
  `"node:my.prop"` works in `*.anim.json`, tweens and the `tween` component. See
  `docs/agents/animation.md`. Component fields need nothing: `"comp.field"` paths already work.
- **A play-test command**: a row in the `br_pt_commands` table (name, typed argument string,
  one-line doc) and a branch in `br_pt_exec` (`engine/src/playtest/br_playtest.c`); arguments are
  validated from the row, `help` lists it. Document it in `docs/PLAYTESTING.md` and test it in
  `tests/test_playtest.c`.
- **Data a game reads** (scenes, prefabs, game.json, components): `docs/DATA_FORMATS.md`.
- **Module state**: one pointer per module in `struct br_engine`
  (`engine/src/app/br_engine_internal.h`); allocate in `br_<module>_register`, free on shutdown.
- **An engine module**: a public header in `engine/include/braty/` (included from `braty.h`),
  sources in `engine/src/<module>/`, a `br_<module>_register(e)` called from
  `engine/src/app/br_builtin.c` that allocates the module's state pointer in `struct br_engine`
  and adds its systems, and `docs/agents/<module>.md`. CMake globs sources, no list to edit.
  Drawing from a node type, offscreen rendering, callback commands and the effects hook:
  `docs/agents/render.md`.
- **A test**: `tests/test_<module>.c` with a plain `main` and `BR_CHECK(cond)` from
  `tests/br_test.h`, return `br_test_finish()`. CMake picks it up automatically. A golden image:
  a line in `tools/golden.py`'s `CASES`, then `python tools/golden.py update --only <name>`.
- **An example**: `examples/<name>.c` including `braty/braty_main.h`. Picked up automatically.
- **A sample game**: `sample_projects/<name>/` with its own `CMakeLists.txt` (copy Crate Rush's
  or Star Courier's), plus one `add_subdirectory` line under `BRATY_BUILD_SAMPLES` in the root
  CMakeLists; its golden images are lines in `tools/golden.py`'s `CASES`.

## Module guides

Read the guide, then the module's public header (headers are the API reference).

| Module | Guide | Status |
|---|---|---|
| core | `docs/agents/core.md` | done (Task 1) |
| arena | `docs/agents/arena.md` | done (Phase 2 Task 14) |
| math | `docs/agents/math.md` | done (Task 1; audit, geometry, easing, random, noise, splines: Phase 2 Task 14c) |
| gpu | `docs/agents/gpu.md` | done (Task 1) |
| app | `docs/agents/app.md` | done (Task 1; pacing, `--deterministic`, groups: Phase 2 Task 14) |
| scene | `docs/agents/scene.md` | done (Task 2; component queries: Phase 2 Task 14c) |
| render | `docs/agents/render.md` | done (Task 2) |
| assets | `docs/agents/assets.md` | done (Tasks 2, 5, 16) |
| graphics | `docs/agents/graphics.md` | done (Task 3) |
| text | `docs/agents/text.md` | done (Task 4) |
| sprites | `docs/agents/sprites.md` | done (Task 6) |
| mesh | `docs/agents/mesh.md` | done (Task 6) |
| particles | `docs/agents/particles.md` | done (Task 6) |
| events | `docs/agents/events.md` | done (Task 7; pointer positions through cameras: Phase 2 integration) |
| input | `docs/agents/input.md` | done (Task 7; gamepads, actions, contexts, rebinding, glyphs: Phase 2 Task 14c) |
| audio | `docs/agents/audio.md` | done (Task 7) |
| filters | `docs/agents/filters.md` | done (Task 8) |
| three_d | `docs/agents/three_d.md` | done (Task 9) |
| gltf | `docs/agents/gltf.md` | done (Task 11; glTF as scene content - `.glb` as scene / prefab / kit, extras -> components, conventions, batching: Phase 2 Task 18) |
| data | `docs/agents/data.md` | done (Task 10) |
| debug | `docs/agents/debug.md` | done (Task 10) |
| playtest | `docs/agents/playtest.md` | done (Phase 2 Task 14; actions, gamepads, 3D paths: 14c/14d; `profile`, `engine:scene`: integration) |
| camera | `docs/agents/camera.md` | done (Phase 2 Task 15) |
| canvas3d | `docs/agents/canvas3d.md` | done (Phase 2 Task 15) |
| scenes | `docs/agents/scenes.md` | done (Phase 2 Task 16) |
| animation | `docs/agents/animation.md` | done (Phase 2 Task 17) |
| profiler | `docs/agents/profiler.md` | done (Phase 2 Task 18b) |

Phase 2: `docs/plans/2026-11-19-braty-engine-phase2.md`. Done: Tasks 14 (arenas, pacing,
play-testing), 14c (input, math, queries), 15 (cameras, canvas3d), 16 (scenes, asset groups), 17
(animation, tweens), 18b (profiler), 18 (glTF scene content), 19 (Crate Rush, docs). Open: Task 20
(integration review).

## Layout

```
engine/include/braty/   public headers; braty.h includes them all, braty_main.h = braty.h + entry point
engine/src/<module>/    sources; private headers *_internal.h
engine/shaders/         HLSL sources + compiled/ generated headers (committed)
third_party/            stb, cgltf, cJSON, microui (+ licences, see third_party/README.md)
tools/                  compile_shaders.py, playtest.py, golden.py, check_getting_started.py
sample_projects/        star_courier (2D) and crate_rush (3D); each builds standalone or from the root project
tests/  examples/  docs/   docs: BUILDING, GETTING_STARTED, TESTING, PLAYTESTING, PLATFORMS, DATA_FORMATS,
                           API_REVIEW, agents/; tests/golden/ holds the reference images
```
