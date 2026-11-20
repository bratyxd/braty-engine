# Braty Engine

A cross-platform, editor-less, **data-driven** 2D/3D game engine in **pure C11** on **SDL3 and
the SDL3 GPU API**, with the feature set of [PixiJS](https://pixijs.com) v8 (plus the
`@pixi/3d` beta) and none of the web's limits. A game is JSON data (game.json, scene files,
prefabs, manifests, spritesheets) plus plain C structs and the systems that iterate them.

- One static library, `braty`. Only SDL3 and the C standard library underneath.
- Every scene object is a handle into flat tables; behaviour is a list of systems run in
  phase order every frame. No per-object virtual methods.
- Shaders are HLSL compiled offline to SPIR-V, DXIL and MSL; the runtime never compiles source.
- Straightforward code by rule: no clever macros, no hidden globals (see `AGENTS.md`).

## Status

Phase 1 (Tasks 1-12 of `docs/plans/2026-11-19-braty-engine.md`) and Phase 2 Tasks 14-19
(`docs/plans/2026-11-19-braty-engine-phase2.md`) are implemented: memory arenas, frame pacing,
`--deterministic`, play-testing and golden images (14); input, math audit, component queries
(14c); cameras and 2D in 3D (15); scenes and asset groups (16); animation and tweens (17); the
profiler (18b); glTF as scene content (18); the second sample game, Crate Rush (19). Task 20
(integration review) is open. A full code review on 2026-11-20 (eight slices, every engine
file) and its fixes are summarised in `docs/REVIEW_2026-11-20.md`. Phase 3
(`docs/plans/2026-11-20-braty-engine-phase3.md`) made one API out of the duplicates (Task A) and
added what a shipped game was missing (Tasks B1-B6): component fields of any length, enums by
name, arrays, vec4 / quat / 3D node references; string tables and locales; font fallback chains;
a storage layer with `.brpack` packs and an install rule; a text-field node; glTF cameras as
`br_camera`; a headless draw-list harness. Both sample games use them (Task C).

`ctest` runs 41 test programs (headless; `test_gpu`, `test_debug` and one profiler case open a
window and skip without a GPU), every example once with `--headless --frames 5`, the two sample
games' play-test scripts (headless; Crate Rush's also from a `.brpack` with no loose files), the
pack round trip and 29 golden images (Direct3D 12; Vulkan differs by a few edge pixels in some of
them, `docs/TESTING.md`): 75 CTest entries. 27 examples and two sample games run on **Windows 11 + MinGW-w64 GCC with Direct3D 12 and Vulkan** (RTX 3060 Ti) -
the only tested platform, in Debug and with the `windows-gcc-release` preset. Linux, macOS, iOS,
Android and MSVC presets exist but have never been built; Metal shaders are generated but have
never run. Details: `docs/PLATFORMS.md`. What is not there yet: "Not implemented yet" below.

| Area | What | Modules | Status |
|---|---|---|---|
| Core, app loop | handles + dense tables, systems in phases, vsync modes, FPS cap, fixed timestep with render interpolation, `--deterministic` | core, app, math | Done |
| Memory | linear / stack / pool / scratch arenas, per-frame arena, arenas listed in the profiler | arena | Done |
| 2D | sprites, nine-slice, tiling, graphics, text (TTF, bitmap, SDF, rich; font fallback chains and a missing-glyph box), meshes, particles, filters, masks, blend modes | gpu, scene, render, graphics, text, sprites, mesh, particles, filters | Done |
| Localisation | `strings.<locale>.json` tables, system locale with fallbacks, `{name}` placeholders, zero / one / other plurals, `"text_key"` nodes that follow the locale | loc | Done (Phase 3 B1) |
| Text entry | `text_field` node: caret, selection, clipboard, undo, max length, password, multi-line, IME composition; game actions do not see typed keys | text_field | Done (Phase 3 B6) |
| Files, shipping | one storage layer (asset root, packs, executable folder, working directory, user storage), `.brpack` packs (`tools/pack_assets.py`, game.json `"packs"`, `--pack`), `braty_install_game` + `cmake --install`, start-up error dialog, release `log.txt` | core, data | Done (Phase 3 B3) |
| 3D | View3D, PBR / Phong / flat / toon materials, cascaded shadows, HDR + bloom + tone mapping, picking, instancing, skinning, morph targets | three_d | Done |
| glTF-first | a `.glb` is a scene, a prefab, a scene-file node or a kit (`kit.glb#Node`); node extras become components; collider / trigger / no-render name conventions; automatic batching | gltf, data | Done |
| 2D in 3D | canvas3d: sprites, nine-slice, SDF text and buttons in the world, billboards | canvas3d | Done |
| Cameras | Unity-style 2D / 3D cameras (vertical extent fixed), fit policies, safe-area anchors, follow / shake / orbit, several cameras with layers | camera | Done |
| Scenes, assets | single / additive / async scenes, fade transitions with a loading scene, persistent nodes, asset groups with reference counts, manifests and bundles | scenes, assets, data | Done |
| Animation | one clip format for 2D and 3D, animators with layers, blend trees, `*.animgraph.json` state machines, events, root motion; tweens with named easings and sequences | animation | Done |
| Input | actions and composites in data, contexts, rebinding with conflict checks, gamepads (Xbox / PlayStation / Switch families, virtual pads), button glyphs and `{action}` prompts | events | Done |
| Audio | wav / ogg, streaming, buses, sound sprites | audio | Done |
| Play-testing | scripts, interactive stdin, record / replay, `expect` / `wait_until`, state queries, `profile` | playtest | Done |
| Profiler | CPU scopes, GPU estimate, counters, bottleneck verdict, F2 panel, HUD, Chrome trace | profiler | Done |
| Tests | 41 test programs, headless runs of every example, play-test scripts, pack round trip, 29 golden images (`docs/TESTING.md`) | tests, tools | Done |
| Platforms | presets for Linux, macOS, iOS, Android, MSVC; a `-release` preset for each | - | Configured, untested (only `windows-gcc` / `windows-gcc-release` built) |

## Quick start

Needs CMake >= 3.24, Ninja, GCC or Clang (MSVC untested) and SDL 3.4 (found with
`find_package`, or fetched and built automatically - slow the first time).

```sh
cmake --preset windows-gcc            # or: linux, macos (untested)
cmake --build --preset windows-gcc    # engine, tests, examples, sample game
ctest --preset windows-gcc            # unit tests, play-test script, golden images (docs/TESTING.md)

build/windows-gcc/sample_projects/crate_rush/crate_rush.exe              # play the 3D sample
build/windows-gcc/sample_projects/star_courier/star_courier.exe          # play the 2D sample
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --playtest sample_projects/crate_rush/playtest/round.json
build/windows-gcc/hello_quad.exe --frames 3 --screenshot out.png --gpu-driver vulkan
```

Every app takes `--frames N` (fixed 1/60 s steps, then exit 0), `--screenshot out.png`,
`--gpu-driver direct3d12|vulkan|metal`, `--vsync off|on|mailbox|adaptive`, `--max-fps N`,
`--deterministic`, `--headless`, `--assets DIR` and the play-test flags below. Building SDL
yourself, presets and shaders: `docs/BUILDING.md`. Making a game: `docs/GETTING_STARTED.md`.
Tests: `docs/TESTING.md`. AI agents start at `AGENTS.md`.

## Play-testing a game

Any Braty game can be played by a script or by an agent: press keys and input actions, click
nodes by label, step frames, take screenshots, read and set node properties, component fields
and game state, and `expect` values (`docs/PLAYTESTING.md`, module `br_playtest`).

```sh
python tools/playtest.py build/windows-gcc/sample_projects/star_courier/star_courier.exe "step 60" "action start" "step 30" "screenshot build/playtest_shot.png" "get ship:position" quit
python tools/playtest.py build/windows-gcc/sample_projects/crate_rush/crate_rush.exe "step 30" "action confirm" "step 120" "key W down" "step 30" "key W up" "screenshot build/crate_shot.png" "get */player:position" "get game:crates" quit
```

prints one JSON reply per command (`{"ok": true, "frame": 90, "value": [400, 400]}` for the last
`get`; 3D nodes are `view/label`, `*` = any view). `--playtest script.json` runs a script and exits 1 if an `expect` fails;
`--playtest-stdin` is the interactive protocol `tools/playtest.py` speaks; `--record
session.json` turns a played session into a script that replays exactly under
`--deterministic` - add `expect` lines and it is a regression test.

## Profiling

Press **F2** in any app for the profiler panel, or run with `--profiler-hud` for a corner
readout (FPS, CPU ms, GPU ms and what limits the frame rate: CPU update, CPU render submit,
GPU, vsync or the FPS cap). `--profile build/trace.json` writes a Chrome trace (open it in
<https://ui.perfetto.dev>) and `build/trace.json.txt`, a text summary. Measure with
`--vsync off`:

```sh
build/windows-gcc/sample_projects/star_courier/star_courier.exe --vsync off --frames 600 --profile build/trace.json
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --level level3 --vsync off --profiler-hud --frames 600 --screenshot build/hud.png
```

Crate Rush, Release build with the profiler on (`-DCMAKE_BUILD_TYPE=Release`; the release
presets turn the profiler off), vsync off, level 3 (RTX 3060 Ti, D3D12): ~2,000 FPS, CPU 0.18 ms,
GPU 0.43 ms (estimated), "GPU bound", top scope `render.submit`. The GPU number is an estimate
from fence polling, not a timer query (`docs/PROFILING.md`).

Play-tests read the same numbers: `{"cmd": "profile", "frames": 300}` and `expect` on
`profile:frame_ms_p95`. game.json can open the HUD or panel (`"profiler": {"hud": true}`).
Everything the numbers mean: `docs/PROFILING.md`.

## Sample games

Both build from the root project or on their own (`cmake -S sample_projects/<name> -B
build/<name> -G Ninja -DCMAKE_PREFIX_PATH=<SDL3>`), copy their `data/` next to the executable,
and ship a `playtest/round.json` that CTest runs headless. What writing them taught us about the
API: `docs/API_REVIEW.md` (Star Courier: items 1-22, Crate Rush: 23-41; the full review: 42-52; the Phase 3 port: 53-62;
renamed and removed functions: its migration table).

### Crate Rush (3D)

`sample_projects/crate_rush/`: a worker in a hard hat runs around three small islands (Meadow
Isle, Frost Ridge, Sawmill Bay) collecting every crate before the clock runs out; spikes and saws
cost time and knock you back, falling in the water costs more. Menu with a 3D diorama, level
select with best scores, loading screen, pause, settings (language, vsync, FPS cap, volume,
profiler HUD, jump rebinding; saved), results with stars and a name field. English and German
(every text from `data/assets/strings/`). Keyboard, mouse, Xbox and PlayStation pads (prompts
follow the device). It is the Phase 2 showcase:

- levels are `.glb` files loaded directly as scenes; their nodes carry `spawn_point`, `crate`,
  `hazard` and `sign` components in extras, `-colonly` collider boxes, a camera, a sun, and level
  settings in the scene extras. `sample_projects/crate_rush/tools/build_assets.py` composes them from Kenney kit pieces the
  way a Blender export would; the menu diorama uses kit parts as `models/kit/kit.glb#crate`;
- a skinned character driven by `animation/character.animgraph.json` (idle, walk / sprint blend,
  jump, fall), footstep sounds from animation events, movement and box collision on a 60 Hz
  fixed timestep with render interpolation (level arena + frame arena);
- the level's own camera as a `br_camera` (`camera_as_br_camera`) with follow and shake, a second (2D) camera for every screen and
  the HUD anchored to the safe area; canvas3d name plates that react to the player and "+10"
  popups; tweens with named easings for pops, counters and menus;
- scenes and asset groups: shared assets in a persistent group, per-level assets owned by the
  level scene; `playtest/soak.json` checks node / 3D node / asset / animator / tween / sound counts
  return to baseline over nine level runs and profiles against a frame budget;
- shipping: `cmake --install` gives exe + game.json + `assets.brpack` (`braty_install_game`);
  the pause stops the ticker (unscaled UI tweens) and holds the gameplay sound bus.

```sh
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe                      # menu
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --level level2        # straight into a level
build/windows-gcc/sample_projects/crate_rush/crate_rush.exe --playtest sample_projects/crate_rush/playtest/soak.json --vsync off
```

Art, UI, fonts, prompts and sounds are Kenney CC0 packs, fetched by
`sample_projects/crate_rush/tools/fetch_assets.py` (URLs and checksums in
`sample_projects/crate_rush/tools/assets.lock.json`).

### Star Courier (2D)

`sample_projects/star_courier/`: steer a courier ship, pick up cargo crates, deliver them to a
station that moves after every delivery, dodge debris that gets faster and denser every three
deliveries. Title -> play (pause with P / Esc / Start) -> game over -> restart. Keyboard,
gamepad, mouse and touch. Everything is data: `game.json` (window, input actions, sound sprites),
a manifest bundle (atlas, starfield, music), three scenes, nine prefabs, and `tuning.json` with
every gameplay number, and `strings/strings.en.json` with every text. C is ten component structs
with field lists and eight systems. All art and
sound are generated by `tools/make_assets.py` (stdlib Python). It builds from the root project
or on its own. `playtest/round.json` plays a round and checks score, lives and state;
`playtest/soak.json` is the 60-second soak.

## PixiJS feature map

Done = implemented and shown in an example. Differs = same capability, different shape.

| PixiJS | Braty | Status |
|---|---|---|
| Application, Ticker, ResizePlugin | `br_app` (SDL main callbacks), systems + phases, `br_ticker_*`, `br_app_set_vsync` / `_max_fps` / `_fixed_timestep` | Done. Differs: the "ticker" is the system list (INPUT, FIXED_UPDATE, UPDATE, LATE_UPDATE, PRE_RENDER, RENDER, POST_RENDER); window resize is SDL's. |
| Point, Matrix, shapes, Color | `br_math.h` (`br_vec2/3/4`, `br_mat3` = Pixi Matrix, `br_mat4`, `br_quat`, rect/circle/ellipse/rounded rect/triangle/polygon, `br_color` with CSS names) | Done |
| Renderer, Texture, TextureSource/Style, RenderTexture, batching | `br_gpu`, `br_texture`, `br_render` (8 textures per batch, 65535-vertex splits) | Done. Differs: SDL_GPU only (Vulkan, D3D12, Metal); no WebGL/WebGPU/Canvas renderer. |
| Container, render groups, render layers, zIndex, Culler, bounds | `br_node_*`, `br_node_set_render_group`, `br_node_layer_attach`, `br_node_set_cullable` | Done. Differs: nodes are `{index, generation}` handles; destroy is deferred to the next frame. |
| Sprite, NineSliceSprite, TilingSprite, AnimatedSprite, GifSprite | `br_sprite`, `br_nine_slice`, `br_tiling_sprite`, `br_animated_sprite`, `br_frame_set_load` | Done |
| Mesh, MeshSimple, MeshRope, MeshPlane, PerspectiveMesh | `br_mesh*` | Done |
| ParticleContainer, Particle | `br_particle_container_*` (instanced, 1M particles) | Done. Differs: no emitter (games write one; see API_REVIEW 15). |
| (no Pixi equivalent) Unity-style cameras | `br_camera` (2D / 3D, viewports, culling masks, render-texture cameras, fit policies, logical resolution, follow / shake / orbit / anchor), `br_canvas3d` (2D UI, sprites and text in the 3D world, billboards) | Done (Phase 2 Task 15) |
| (no Pixi equivalent) scenes | `br_scene_load` / `_switch` (single, additive, async, fade transitions, persistent nodes, scene-scoped systems), `br_asset_group` | Done (Phase 2 Task 16) |
| Animation (Pixi has none built in; GSAP-style tweens) | `br_animator` (2D + 3D clips, layers, blend trees, state machines, events, root motion), `*.anim.json`, `*.animgraph.json`, `br_tween` (+ sequences) | Done (Phase 2 Task 17) |
| Graphics, GraphicsContext, fills, gradients, patterns, strokes, dashes, SVG paths | `br_graphics_*` | Done. Deferred: SVG *file* import (only path data). |
| Text, TextStyle, BitmapText, BitmapFont (+install), HTMLText, SplitText | `br_text`, `br_bitmap_font`, `br_rich_text` (tags + tag styles), `br_split_text`; SDF/MSDF; font fallback chains | Done. Differs: HTMLText is tagged rich text. Missing: complex shaping (Arabic, Indic, ligatures), bidi, colour emoji. |
| (no Pixi equivalent) localisation, text input | `br_loc` (string tables, plurals, `"text_key"`), `br_text_field` (editable text node) | Done (Phase 3) |
| Assets, resolver, manifests, bundles, background loading, Spritesheet | `br_assets_*` (ref-counted cache, worker threads), `br_spritesheet` (TexturePacker hash/array, multipack, 9-slice borders) | Done |
| Compressed textures | DDS (BC1-7), KTX 1 (BCn/ASTC/ETC2 when the GPU supports them) | Done. Deferred: KTX2 / Basis transcoding. |
| EventSystem, federated pointer events, hit areas, cursors | `br_events` (`br_node_on`, capture/target/bubble), `br_input` (keyboard, mouse, touch, pen, gamepads with rumble, actions, injection) | Done. Differs: polled input and named actions (data) next to the event system. |
| @pixi/sound | `br_audio` (wav, ogg, streaming, instances, buses, sound sprites) | Done. Missing: @pixi/sound filters (reverb, distortion, EQ). |
| Filters: Alpha, Blur, ColorMatrix (all presets), Displacement, Noise, custom | `br_filter_*`, custom = your HLSL fragment shader | Done |
| Advanced blend modes (21) | `br_node_set_blend_mode(e, n, BR_BLEND_OVERLAY)` ... | Done |
| Masks (scissor, stencil, alpha, colour), cacheAsTexture | `br_node_set_mask`, `br_node_set_cache_as_texture` | Done |
| Extract, GenerateTexture | `br_extract_pixels`, `br_extract_image_png`, `br_generate_texture` | Done |
| @pixi/3d: View3D, Mesh3D, geometries, PBR/Phong/Flat/Toon materials, lights, cascaded shadows, HDR + tone mapping, post, picking, instancing, skin/morph slots | `br_view3d`, `br_mesh3d`, `br_material3d`, `br_light3d`, `br_camera3d`, `br_post3d` | Done (Task 9). Missing: environment maps / IBL, point-light shadows. |
| @pixi/3d: glTF, skinning playback, animation | `br_gltf`, `br_anim3d`; a `.glb` as scene / prefab / kit (`kit.glb#Node`), extras -> components | Done (Tasks 11, 18). Deferred: Draco / meshopt compression. |
| Extensions | node-type, asset-loader, component and system registries | Done |
| (no Pixi equivalent) | scene files, prefabs, game.json, hot reload (`br_scene_file`, `br_prefab`, `br_game_config`) | Done (view3d content is load-and-save-as-loaded). |
| (dev tools) | `br_debug`: microui overlay (stats incl. heap and arenas, systems, scene tree, inspector, log), F1 | Done |
| (no Pixi equivalent) | memory arenas (`br_arena`: linear, stack, pool, scratch, per-frame), allocator-aware containers, heap statistics | Done (Phase 2 Task 14) |
| (testing) | `br_playtest` (scripts, interactive stdin, record/replay, queries, actions, gamepads, `profile`), `tools/golden.py` golden images | Done (Phase 2 Tasks 14, 14c) |
| (dev tools) | `br_profiler`: CPU scopes, GPU estimate, counters, bottleneck verdict, F2 panel, HUD, Chrome trace | Done (Phase 2 Task 18b) |

**Web-only, not ported:** AccessibilitySystem, DOMContainer, HTMLSource, the Canvas2D renderer,
DOMAdapter / WebWorkerAdapter, crossOrigin, WorkerManager.

**Deferred** (not started): video textures (SDL has no codec), KTX2 / Basis transcoding,
Draco / meshopt glTF compression (no C decoders; compressed files fail with the extension's name),
point-light shadows, environment reflections (IBL), shadows from canvas3d content, a web target,
SVG file import beyond path data, GPU compute culling.

**Not implemented yet** (found by the 2026-11-20 review; the rest of that list was done in Phase 3):

- **Bidi and complex shaping:** Arabic, Hebrew and Indic scripts lay out as isolated code points
  left to right; no ligatures, no colour emoji.
- **Window modes at runtime:** fullscreen only from game.json at start; no API to switch
  fullscreen / borderless / resolution / display while running (call SDL on `br_engine_window`).
- **Platform packaging:** packs and `cmake --install` exist, but loose files are read with
  `SDL_IOFromFile` (not SDL title storage) and there is no app-bundle / APK copy step.
- **Windows subsystem:** executables are console programs (a console window opens next to the
  game; add `WIN32_EXECUTABLE` to your target, `docs/BUILDING.md`).
- **Aspect-ratio goldens:** one 16:9 golden per case; nothing checks 21:9 or 9:16 automatically.

## Limits

- **Platforms:** tested only on Windows 11 with MinGW-w64 GCC, D3D12 and Vulkan, one GPU (RTX
  3060 Ti). Linux, macOS, iOS, Android and MSVC have presets and CI jobs (MSVC, Linux, macOS)
  that have never run.
- **Golden images are Direct3D 12 images:** Vulkan and other GPUs differ by a few pixels and
  would need goldens of their own (`docs/TESTING.md`).
- **GPU time is an estimate:** SDL_GPU has no timer queries; the profiler polls fences (~0.5 ms
  resolution on Windows) and with vsync on the number includes display waits.
- **Real controllers untested:** gamepad families, glyphs and rebinding are verified with SDL
  virtual pads only; rumble, LEDs and touchpads on real hardware are not (`docs/agents/input.md`).
- **Runtime window resizing** is handled by the cameras (verified by starting at 16:9, 21:9 and
  9:16); the play-test `window` command resizes a running game, but the automated tests use it
  only headless (the screen size cameras see), not on a live window.
- **Hostile data is bounded, not unlimited** (each limit logs and refuses or clamps): the 2D
  render walk and hit tests stop at 2048 levels (deeper nodes are neither drawn nor hit); event
  propagation paths stop at 64 ancestors; prefabs nest at most 16 deep and scene-file
  `"children"` 256 levels; a `{a,b}` asset pattern expands to at most 16 names; a concave
  graphics fill has at most 8192 points, a sub-path 65536, polygons and stars 4096 sides (1024
  from data files); text sizes, strokes and blurs are capped at 4096 (glyphs
  rasterise at most at 1024 px, effects grow at most 64 px); glTF accessors hold at most 2^24
  elements (2^20 without a buffer view) and 2^20 GPU instances per node; animation graphs hold
  1024 states and 4096 transitions, clips 100000 keys per track; render textures at most 16384
  px a side (`BR_RENDER_TEXTURE_MAX_SIZE`). `docs/DATA_FORMATS.md` lists the value ranges.
- **Physics:** none in the engine. Crate Rush does box collision itself (`src/player.c`).
- **Localisation:** plural forms are zero / one / other only (no ICU: "few" / "many" languages use
  `other`); no bidirectional text and no shaping; locale names are at most 31 bytes (letters,
  digits, `-`, `_`); string tables are not hot-reloaded. Glyphs missing from a font come from
  its fallback chain (up to 8 fonts), else a missing-glyph box.
- **Packs:** no compression; paths are case-sensitive (loose files follow the OS) and 1..255
  bytes; loose files win in debug builds, packs in release builds.
- **3D lights:** 16 per view (`BR_LIGHT3D_MAX`); beyond that directional lights are kept, then
  the point / spot lights with the largest `intensity / (1 + distance^2)` to the camera.
- **Text field:** caret and Backspace step over code points, not grapheme clusters (a combining
  accent or an emoji sequence takes several presses); on a canvas3d it needs texture mode and the
  IME candidate window is placed in canvas coordinates (wrong place on screen); there is no
  engine focus ring (menus call `br_text_field_focus`).
- **Start-up errors and the log:** an interactive run that fails to start shows an SDL message
  box with the last error (never in `--headless`, `--frames`, `--deterministic` or play-test
  runs; no test sees the dialog). Release builds mirror the log into `log.txt` in user storage,
  written at shutdown and on a failed start (first 1 MB), so a crash loses it.
- Open API rough edges and their proposed fixes: `docs/API_REVIEW.md`.

## Deliberate shortcuts (`ponytail:` comments)

Each is a known ceiling with its upgrade path written next to it in the code
(`grep -rn "ponytail:" engine`). The ones worth knowing:

| Module | Shortcut |
|---|---|
| app | Sleeps a fixed 16 ms while minimised or paused (inactive) instead of waiting for the restore event. |
| assets | A spritesheet's image loads synchronously inside the sheet load (also for async loads); texture loaders are assumed to return `br_texture *`. |
| core | Save data busy-waits until SDL storage is ready (instant on desktop; consoles may want a callback); heap statistics are process-wide counters under one spinlock; no double-free check in `br_free` (pool arenas do check). |
| animation | 2D blend trees use inverse-distance weights; a step of more than ~8 loop passes skips the extra markers; on layers 1-3 an interrupted fade drops the older fading motion (the base layer fades from a snapshot of the pose on screen). |
| audio | A deterministic run with a device plays on the wall clock (without one, sounds advance by game time). |
| canvas3d | Only SDF text is re-shaded for 3D (other custom 2D shaders need texture mode); canvas hit tests ignore z-index, render layers and masks. |
| data | Masks, filters, hit areas and handlers are saved as loaded, not read back; edited prefab files reload only when a scene using them changes. |
| debug | Overlay glyphs are rasterised at 1 pixel per unit (soft on HiDPI); log capture and the input event filter are process-wide (one engine per process; injected input skips the filter). |
| events | Event paths stop at 64 ancestors; hit tests walk the ancestor chain per node; headless touch coordinates pass through unscaled; the hover ticker replays the stored stage point (a camera moving under a still mouse is not re-mapped). |
| filters | Rotated atlas frames are returned as stored by extract; huge filter areas drop resolution at 4096 px; a cached node with filters re-renders when its bounds change. |
| gpu | Pipeline/sampler caches are linear arrays; readback handles 8-bit RGBA/BGRA only. |
| graphics | Ear clipping is about O(n^2) (so concave fills over 8192 points are refused), hole bridging O(ring * edges), hit tests scan cached triangles linearly. |
| math | Collinear overlapping segments report no intersection; the spring helper's implicit Euler damps a little at large dt. |
| particles | Scratch memory keeps its peak size; a container drawn twice in a frame uses the last matrix for both. |
| playtest | Every mouse motion is recorded; the log hook is process-wide; a node property read builds the node's whole JSON. |
| profiler | A single frame longer than 4.29 s is clamped; the GPU estimate polls fences every 0.1 ms. |
| sprites | Nine-slice tile fill falls back to stretching beyond 256 tiles per axis. |
| text | Kerning pairs only for sets up to 256 glyphs; a full 4096 atlas stops caching new glyphs (no eviction); diagonal gradients sampled at glyph corners. |
| three_d / gltf | Skinned bounds = bind pose grown 2x; batched meshes culled as one box; batching only inside one instance (kit parts placed separately stay separate draws); one depth bias per light; per-triangle tangents (not MikkTSpace); smooth generated normals; points / lines skipped; one sampler filter / wrap mode; transmission as alpha; light units unscaled; infinite far planes become 1000 units. |
| data (glTF) | Every collider is a box from the mesh bounds; extras are parsed per instance; a view using a glTF camera reloads through its scene. |
| crate_rush | Box-only collision, no slopes or steps; a linear scan of the level's colliders. |
| loc | String tables are not hot-reloaded (an edited table is read again once no locale holds it). |
| text_field | Caret stops are rebuilt every frame (O(text length) per field); editing steps over code points, not graphemes. |
| core (files) | Loose files are opened with `SDL_IOFromFile`, not SDL title storage; the log file keeps the first 1 MB of a run. |

## Repository layout

```
engine/include/braty/   public headers (the API reference); braty.h includes them all
engine/src/<module>/    sources; private headers *_internal.h
engine/shaders/         HLSL + compiled/ headers (committed; tools/compile_shaders.py)
engine/assets/fonts/    the embedded default font (Noto Sans) + licence
third_party/            stb, cgltf, cJSON, microui (vendored, with licences)
examples/               one program per module (+ generated demo data)
sample_projects/        star_courier (2D) and crate_rush (3D) sample games (build standalone too)
tests/                  headless CTest programs (tests/test_<module>.c), golden/ (reference images)
tools/                  compile_shaders.py, playtest.py (drive a game), golden.py (image tests),
                        check_getting_started.py (builds every step of the guide),
                        pack_assets.py (an asset folder -> .brpack), install_game.cmake
docs/                   BUILDING, GETTING_STARTED, TESTING, PLAYTESTING, PLATFORMS, DATA_FORMATS, API_REVIEW,
                        agents/<module>.md (module guides), plans/
.github/workflows/      CI (Windows MinGW + release, MSVC, Linux, macOS; never run)
```

## Licences

- Engine code: no licence file has been chosen yet; until the owner adds one, all rights are
  reserved by the owner.
- Third-party code (`third_party/`, vendored unmodified, licence files next to each):
  stb_image, stb_image_write, stb_truetype, stb_rect_pack, stb_vorbis (MIT or public domain),
  cgltf (MIT), cJSON (MIT), microui (MIT). SDL3 (zlib) is a build dependency, not vendored.
- Fonts: Noto Sans Regular, SIL Open Font License 1.1 (`engine/assets/fonts/OFL.txt`), embedded
  as the default font.
- Example data: generated by scripts in `examples/`, except `examples/audio/gong.ogg` (CC0,
  `examples/audio/LICENSE-gong.txt`) and the glTF test models in `examples/models/`: unmodified
  Khronos glTF-Sample-Assets, CC0 1.0 or CC-BY 4.0 with attribution per model in
  `examples/models/LICENSES.md` (the Fox: model CC0, rigging and animation CC-BY 4.0).
- Star Courier: all art and sound are original, generated by
  `sample_projects/star_courier/tools/make_assets.py`.
- Crate Rush: Kenney (www.kenney.nl) packs Mini Characters, Platformer Kit, UI Pack (incl. the
  Kenney Future fonts and UI sounds), Input Prompts, Impact Sounds, Interface Sounds and Music
  Jingles, all **CC0 1.0**; each pack's `License.txt` sits next to its files (and next to the
  atlases and level files built from them). The levels, atlases and kit file are derived by
  `sample_projects/crate_rush/tools/build_assets.py`.
