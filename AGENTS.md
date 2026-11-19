# AGENTS.md - working on (or with) the Braty Engine

Braty is a cross-platform, editor-less, data-driven 2D/3D game engine in **pure C11** on
**SDL3 and the SDL3 GPU API**, with the feature set of PixiJS v8 (plus `@pixi/3d`). One static
library, `braty`. All scene data lives in flat tables inside a `br_engine`; behaviour is a list of
systems (plain functions) run in phase order every frame.

The full plan and feature map: `docs/plans/2026-11-19-braty-engine.md`.

## Build, test, look at it

```sh
cmake --preset windows-gcc && cmake --build --preset windows-gcc
ctest --preset windows-gcc
build/windows-gcc/hello_quad.exe --frames 3 --screenshot out.png [--gpu-driver vulkan|direct3d12]
```

Every app built on `br_app` takes `--frames N --screenshot out.png` (fixed 1/60 s steps, exits 0).
**Open the PNG before claiming a visual feature works.** Details: `docs/BUILDING.md`.

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
- **Nodes** (Task 2, `br_node.h`): handles `br_node { index, generation }` into a node table
  (hierarchy, transform, alpha, tint, visibility...). Each node has a type; per-type data lives in
  a dense pool. Stale handles are detected by generation and ignored.
- **Components** (Task 2): user structs in dense arrays attached to nodes, described field by
  field (`br_component_desc`) so scene files can fill them. Systems iterate
  `br_component_count` / `br_component_at` / `br_component_owner_at`.
- **Assets** (Task 2/5): loaders registered per file extension, cache keyed by path.

The contracts are fixed in the plan under "Core contracts".

## How to add ...

- **A system**: write `static void my_system(br_engine *e, float dt, void *user)`, register it in
  your init with `br_system_add`. Keep state in a struct passed as `user`; free it with
  `br_engine_on_shutdown`. See `docs/agents/app.md`.
- **A node type** (Task 2 API): fill a `br_node_type_desc` (name = JSON "type", data_size,
  callbacks for create/destroy/bounds/render/hit_test/from_json) and call
  `br_node_type_register` from your module's `br_<module>_register(e)`. See `docs/agents/scene.md`.
- **A component** (Task 2 API): a plain struct + a `br_field_desc` array + `br_component_register`.
- **An asset loader** (Task 2 API): fill `br_asset_loader_desc` (type name, extensions, load,
  unload) and call `br_assets_register_loader`. See `docs/agents/assets.md`.
- **A shader**: `engine/shaders/<name>.<vert|frag>.hlsl`, run `python tools/compile_shaders.py`,
  include `<name>.<stage>.h`, use `br_shader_<name>_<stage>` in a `br_pipeline_desc`.
  Rules in `docs/BUILDING.md` and `docs/agents/gpu.md`.
- **Module state**: one pointer per module in `struct br_engine`
  (`engine/src/app/br_engine_internal.h`); allocate in `br_<module>_register`, free on shutdown.
- **A test**: `tests/test_<module>.c` with a plain `main` and `BR_CHECK(cond)` from
  `tests/br_test.h`, return `br_test_finish()`. CMake picks it up automatically.
- **An example**: `examples/<name>.c` including `braty/braty_main.h`. Picked up automatically.

## Module guides

Read the guide, then the module's public header (headers are the API reference).

| Module | Guide | Status |
|---|---|---|
| core | `docs/agents/core.md` | done (Task 1) |
| math | `docs/agents/math.md` | done (Task 1) |
| gpu | `docs/agents/gpu.md` | done (Task 1) |
| app | `docs/agents/app.md` | done (Task 1) |
| scene | `docs/agents/scene.md` | Task 2 |
| render | `docs/agents/render.md` | Task 2 |
| assets | `docs/agents/assets.md` | Tasks 2, 5 |
| graphics | `docs/agents/graphics.md` | Task 3 |
| text | `docs/agents/text.md` | Task 4 |
| sprites | `docs/agents/sprites.md` | Task 6 |
| mesh | `docs/agents/mesh.md` | Task 6 |
| particles | `docs/agents/particles.md` | Task 6 |
| events | `docs/agents/events.md` | Task 7 |
| audio | `docs/agents/audio.md` | Task 7 |
| filters | `docs/agents/filters.md` | Task 8 |
| three_d | `docs/agents/three_d.md` | Task 9 |
| gltf | `docs/agents/gltf.md` | Task 11 |
| data | `docs/agents/data.md` | Task 10 |
| debug | `docs/agents/debug.md` | Task 10 |

Guides marked with a task number do not exist until that task lands.

## Layout

```
engine/include/braty/   public headers (br_core.h, br_math.h, br_gpu.h, br_app.h, braty_main.h, ...)
engine/src/<module>/    sources; private headers *_internal.h
engine/shaders/         HLSL sources + compiled/ generated headers (committed)
third_party/            stb, cgltf, cJSON, microui (+ licences, see third_party/README.md)
tools/                  compile_shaders.py
tests/  examples/  docs/
```
