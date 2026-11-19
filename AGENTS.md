# AGENTS.md - working on (or with) the Braty Engine

Braty is a cross-platform, editor-less, data-driven 2D/3D game engine in **pure C11** on
**SDL3 and the SDL3 GPU API**, with the feature set of PixiJS v8 (plus `@pixi/3d`). One static
library, `braty`. All scene data lives in flat tables inside a `br_engine`; behaviour is a list of
systems (plain functions) run in phase order every frame.

The full plan and feature map: `docs/plans/2026-11-19-braty-engine.md`.

## Build, test, look at it

```sh
cmake --preset windows-gcc [-DCMAKE_PREFIX_PATH=<SDL3 prefix>]   # default prefix: .deps/sdl3
cmake --build --preset windows-gcc                                  # engine, tests, examples, sample
ctest --preset windows-gcc                                          # headless tests
build/windows-gcc/hello_quad.exe --frames 3 --screenshot build/out.png [--gpu-driver vulkan|direct3d12]
build/windows-gcc/sample_projects/star_courier/star_courier.exe --autoplay --frames 700 --screenshot build/game.png
```

Every app built on `br_app` takes `--frames N --screenshot out.png` (fixed 1/60 s steps, exits 0).
**Open the PNG before claiming a visual feature works**, on both `direct3d12` and `vulkan`.
Examples find their data at `../../examples` relative to the executable, so they work from any
working directory but only when built into the default `build/<preset>/`; the sample game
copies its data next to its executable and works from any build directory. Star Courier's
`--autoplay` plays itself deterministically: `--frames 60` = title, `700` = gameplay,
`1300` = game over; `--frames 3600` is the 60-second soak (it logs node/texture/asset counts at
every game over). Details: `docs/BUILDING.md`, other platforms: `docs/PLATFORMS.md`.

## How to make a game with Braty

Read `docs/GETTING_STARTED.md` (a new game in eleven compiling steps), then copy the shape of
`sample_projects/star_courier/`:

- `data/game.json` - window, asset root, manifest, input actions, audio buses and sound
  sprites; game-specific settings under `"game"`. Unknown keys are warned about.
- `data/assets/` - scene files (one per screen), prefabs (spawned things), a TexturePacker
  spritesheet, a manifest with a bundle, and `tuning.json` (every gameplay number, loaded as a
  component so errors carry JSON paths and edits hot-reload).
- `src/game.h` - component structs + the ONE game-state struct handed to systems as `user`.
- `src/components.c` - a `br_field_desc` list per component (`br_component_register`).
- `src/systems.c` - systems over component arrays (input -> intent, state machine, spawner,
  movement, collision -> events, rules, fx, hud).
- `src/main.c` - `br_main` with `br_game_config_load` / `_args` / `_app_desc`, init registers
  components and systems, then `br_game_config_apply`.
- `CMakeLists.txt` - `add_subdirectory(<braty>)`, link `braty::braty`, copy `data/` next to the
  executable.

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

## The data-driven model

- **Systems** (`br_app.h`, available now): `br_system_add(e, name, phase, priority, fn, user)`.
  Phases: INPUT, UPDATE, LATE_UPDATE, PRE_RENDER (always run) then RENDER, POST_RENDER (only when
  a frame can be drawn). Engine work (transforms, culling, rendering) is systems too.
- **Nodes** (`br_node.h`): handles `br_node { index, generation }` into a node table
  (hierarchy, transform, alpha, tint, visibility...). Each node has a type; per-type data lives in
  a dense pool. Stale handles are detected by generation and ignored.
- **Components** (`br_node.h`): user structs in dense arrays attached to nodes, described field by
  field (`br_component_desc`) so scene files can fill them. Systems iterate
  `br_component_count` / `br_component_at` / `br_component_owner_at`.
- **Assets** (`br_assets.h`): loaders registered per file extension, cache keyed by path.

The contracts are fixed in the plan under "Core contracts".

## How to add ...

- **A system**: write `static void my_system(br_engine *e, float dt, void *user)`, register it in
  your init with `br_system_add`. Keep state in a struct passed as `user`; free it with
  `br_engine_on_shutdown`. See `docs/agents/app.md`.
- **A node type**: fill a `br_node_type_desc` (name = JSON "type", data_size,
  callbacks for create/destroy/bounds/render/hit_test/from_json) and call
  `br_node_type_register` from your module's `br_<module>_register(e)`. See `docs/agents/scene.md`.
- **A component**: a plain struct + a `br_field_desc` array + `br_component_register`.
- **An asset loader**: fill `br_asset_loader_desc` (type name, extensions, load,
  unload) and call `br_assets_register_loader`. See `docs/agents/assets.md`.
- **A shader**: `engine/shaders/<name>.<vert|frag>.hlsl`, run `python tools/compile_shaders.py`,
  include `<name>.<stage>.h`, use `br_shader_<name>_<stage>` in a `br_pipeline_desc`.
  Rules in `docs/BUILDING.md` and `docs/agents/gpu.md`.
- **Data a game reads** (scenes, prefabs, game.json, components): `docs/DATA_FORMATS.md`.
- **Module state**: one pointer per module in `struct br_engine`
  (`engine/src/app/br_engine_internal.h`); allocate in `br_<module>_register`, free on shutdown.
- **A module (Tasks 3-11)**: your public header and an empty `br_<module>_register(e)` already
  exist, are included by `braty/braty.h` and called from `engine/src/app/br_builtin.c`. Fill them;
  add `.c` files in your `engine/src/<module>/` directory. Never edit `braty.h`, `br_builtin.c`,
  `br_engine_internal.h` or CMake. Drawing from a node type, offscreen rendering, callback
  commands and the effects hook: `docs/agents/render.md`.
- **A test**: `tests/test_<module>.c` with a plain `main` and `BR_CHECK(cond)` from
  `tests/br_test.h`, return `br_test_finish()`. CMake picks it up automatically.
- **An example**: `examples/<name>.c` including `braty/braty_main.h`. Picked up automatically.
- **A sample game**: `sample_projects/<name>/` with its own `CMakeLists.txt` (copy Star
  Courier's), plus one `add_subdirectory` line under `BRATY_BUILD_SAMPLES` in the root CMakeLists.

## Module guides

Read the guide, then the module's public header (headers are the API reference).

| Module | Guide | Status |
|---|---|---|
| core | `docs/agents/core.md` | done (Task 1) |
| math | `docs/agents/math.md` | done (Task 1) |
| gpu | `docs/agents/gpu.md` | done (Task 1) |
| app | `docs/agents/app.md` | done (Task 1) |
| scene | `docs/agents/scene.md` | done (Task 2) |
| render | `docs/agents/render.md` | done (Task 2) |
| assets | `docs/agents/assets.md` | done (Tasks 2, 5) |
| graphics | `docs/agents/graphics.md` | done (Task 3) |
| text | `docs/agents/text.md` | done (Task 4) |
| sprites | `docs/agents/sprites.md` | done (Task 6) |
| mesh | `docs/agents/mesh.md` | done (Task 6) |
| particles | `docs/agents/particles.md` | done (Task 6) |
| events | `docs/agents/events.md` | done (Task 7) |
| audio | `docs/agents/audio.md` | done (Task 7) |
| filters | `docs/agents/filters.md` | done (Task 8) |
| three_d | `docs/agents/three_d.md` | done (Task 9) |
| gltf | `docs/agents/gltf.md` | in progress (Task 11) |
| data | `docs/agents/data.md` | done (Task 10) |
| debug | `docs/agents/debug.md` | done (Task 10) |

Guides marked "in progress" do not exist until that task lands. Phase 2 (arenas, play-testing
API, cameras, scenes and asset groups, animation, profiler): `docs/plans/2026-11-19-braty-engine-phase2.md`.

## Layout

```
engine/include/braty/   public headers; braty.h includes them all, braty_main.h = braty.h + entry point
engine/src/<module>/    sources; private headers *_internal.h
engine/shaders/         HLSL sources + compiled/ generated headers (committed)
third_party/            stb, cgltf, cJSON, microui (+ licences, see third_party/README.md)
tools/                  compile_shaders.py
sample_projects/        star_courier (sample game; builds standalone or from the root project)
tests/  examples/  docs/   docs: BUILDING, GETTING_STARTED, PLATFORMS, DATA_FORMATS, API_REVIEW, agents/
```
