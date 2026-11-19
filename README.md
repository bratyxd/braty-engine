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

Phase 1 (Tasks 1-12 of `docs/plans/2026-11-19-braty-engine.md`, glTF included) is implemented,
and Phase 2 (`docs/plans/2026-11-19-braty-engine-phase2.md`) Tasks 14-17 and 18b: memory
arenas, frame pacing, `--deterministic`, play-testing and golden images (14); input, math audit,
component queries (14c); cameras and 2D in 3D (15); scenes and asset groups (16); animation and
tweens (17); the profiler (18b). Task 18 (glTF as scene content) is in progress; Tasks 19-20 (a
second sample game, integration review) are open.

`ctest` runs 30 headless test programs, Star Courier's play-test script and 21 golden images;
20 examples and one sample game run on **Windows 11 + MinGW-w64 GCC with Direct3D 12 and
Vulkan** - the only tested platform. Linux, macOS, iOS and Android presets exist but have never
been built; Metal shaders are generated but have never run. Details: `docs/PLATFORMS.md`.

| Area | Modules | Status |
|---|---|---|
| Core, app loop, pacing | core, arena, math, app | Done (Task 1, Phase 2 Tasks 14, 14c) |
| 2D rendering | gpu, scene, render, graphics, text, sprites, mesh, particles, filters | Done (Tasks 1-8) |
| Input, events, audio | events (input), audio | Done (Task 7, Phase 2 Task 14c) |
| 3D | three_d, gltf, canvas3d | Done (Tasks 9, 11, Phase 2 Task 15); glTF scene content: Task 18, in progress |
| Cameras | camera | Done (Phase 2 Task 15) |
| Data, scenes, assets | data, scenes, assets | Done (Tasks 2, 5, 10, Phase 2 Task 16) |
| Animation | animation | Done (Phase 2 Task 17) |
| Tools | debug, playtest, profiler | Done (Task 10, Phase 2 Tasks 14, 18b) |
| Platforms | presets for Linux, macOS, iOS, Android, MSVC | Configured, untested |

## Quick start

Needs CMake >= 3.24, Ninja, GCC or Clang (MSVC untested) and SDL 3.4 (found with
`find_package`, or fetched and built automatically - slow the first time).

```sh
cmake --preset windows-gcc            # or: linux, macos (untested)
cmake --build --preset windows-gcc    # engine, tests, examples, sample game
ctest --preset windows-gcc            # unit tests, play-test script, golden images (docs/TESTING.md)

build/windows-gcc/sample_projects/star_courier/star_courier.exe          # play
build/windows-gcc/sample_projects/star_courier/star_courier.exe --playtest sample_projects/star_courier/playtest/round.json
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
```

prints one JSON reply per command (`{"ok": true, "frame": 90, "value": [400, 400]}` for the last
`get`). `--playtest script.json` runs a script and exits 1 if an `expect` fails;
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
```

Play-tests read the same numbers: `{"cmd": "profile", "frames": 300}` and `expect` on
`profile:frame_ms_p95`. game.json can open the HUD or panel (`"profiler": {"hud": true}`).
Everything the numbers mean: `docs/PROFILING.md`.

## The sample game: Star Courier

`sample_projects/star_courier/`: steer a courier ship, pick up cargo crates, deliver them to a
station that moves after every delivery, dodge debris that gets faster and denser every three
deliveries. Title -> play (pause with P / Esc / Start) -> game over -> restart. Keyboard,
gamepad, mouse and touch. Everything is data: `game.json` (window, input actions, sound sprites),
a manifest bundle (atlas, starfield, music), three scenes, nine prefabs, and `tuning.json` with
every gameplay number. C is ten component structs with field lists and eight systems. All art and
sound are generated by `tools/make_assets.py` (stdlib Python). It builds from the root project
or on its own (`cmake -S sample_projects/star_courier -B build/sc -DCMAKE_PREFIX_PATH=<SDL3>`).
`playtest/round.json` plays a round and checks score, lives and state (CTest runs it headless);
`playtest/soak.json` is the 60-second soak.
What writing it taught us about the API: `docs/API_REVIEW.md`.

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
| Text, TextStyle, BitmapText, BitmapFont (+install), HTMLText, SplitText | `br_text`, `br_bitmap_font`, `br_rich_text` (tags + tag styles), `br_split_text`; SDF/MSDF | Done. Differs: HTMLText is tagged rich text. Missing: complex shaping (Arabic, Indic, ligatures), bidi, colour emoji. |
| Assets, resolver, manifests, bundles, background loading, Spritesheet | `br_assets_*` (ref-counted cache, worker threads), `br_spritesheet` (TexturePacker hash/array, multipack, 9-slice borders) | Done |
| Compressed textures | DDS (BC1-7), KTX 1 (BCn/ASTC/ETC2 when the GPU supports them) | Done. Deferred: KTX2 / Basis transcoding. |
| EventSystem, federated pointer events, hit areas, cursors | `br_events` (`br_node_on`, capture/target/bubble), `br_input` (keyboard, mouse, touch, pen, gamepads with rumble, actions, injection) | Done. Differs: polled input and named actions (data) next to the event system. |
| @pixi/sound | `br_audio` (wav, ogg, streaming, instances, buses, sound sprites) | Done. Missing: @pixi/sound filters (reverb, distortion, EQ). |
| Filters: Alpha, Blur, ColorMatrix (all presets), Displacement, Noise, custom | `br_filter_*`, custom = your HLSL fragment shader | Done |
| Advanced blend modes (21) | `br_node_set_blend_mode(e, n, BR_BLEND_OVERLAY)` ... | Done |
| Masks (scissor, stencil, alpha, colour), cacheAsTexture | `br_node_set_mask`, `br_node_set_cache_as_texture` | Done |
| Extract, GenerateTexture | `br_extract_pixels`, `br_extract_image_png`, `br_generate_texture` | Done |
| @pixi/3d: View3D, Mesh3D, geometries, PBR/Phong/Flat/Toon materials, lights, cascaded shadows, HDR + tone mapping, post, picking, instancing, skin/morph slots | `br_view3d`, `br_mesh3d`, `br_material3d`, `br_light3d`, `br_camera3d`, `br_post3d` | Done (Task 9). Missing: environment maps / IBL, point-light shadows. |
| @pixi/3d: glTF, skinning playback, animation | `br_gltf`, `br_anim3d` | Done (Task 11). Deferred: Draco / meshopt compression. |
| Extensions | node-type, asset-loader, component and system registries | Done |
| (no Pixi equivalent) | scene files, prefabs, game.json, hot reload (`br_scene_file`, `br_prefab`, `br_game_config`) | Done (view3d content is load-and-save-as-loaded). |
| (dev tools) | `br_debug`: microui overlay (stats incl. heap and arenas, systems, scene tree, inspector, log), F1 | Done |
| (no Pixi equivalent) | memory arenas (`br_arena`: linear, stack, pool, scratch, per-frame), allocator-aware containers, heap statistics | Done (Phase 2 Task 14) |
| (testing) | `br_playtest` (scripts, interactive stdin, record/replay, queries, actions, gamepads, `profile`), `tools/golden.py` golden images | Done (Phase 2 Tasks 14, 14c) |
| (dev tools) | `br_profiler`: CPU scopes, GPU estimate, counters, bottleneck verdict, F2 panel, HUD, Chrome trace | Done (Phase 2 Task 18b) |

**Web-only, not ported:** AccessibilitySystem, DOMContainer, HTMLSource, the Canvas2D renderer,
DOMAdapter / WebWorkerAdapter, crossOrigin, WorkerManager.

**Deferred** (not started): video textures (SDL has no codec), KTX2/Basis, Draco/meshopt glTF
compression, SVG file import beyond path data, GPU compute culling.

## Deliberate shortcuts (`ponytail:` comments)

Each is a known ceiling with its upgrade path written next to it in the code
(`grep -rn "ponytail:" engine`). The ones worth knowing:

| Module | Shortcut |
|---|---|
| app | Sleeps a fixed 16 ms while minimised instead of waiting for the restore event. |
| assets | A spritesheet's image loads synchronously inside the sheet load (also for async loads); texture loaders are assumed to return `br_texture *`. |
| core | Save data busy-waits until SDL storage is ready (instant on desktop; consoles may want a callback); heap statistics are process-wide counters under one spinlock. |
| animation | 2D blend trees use inverse-distance weights; a step of more than ~8 loop passes skips the extra markers; root motion reports nothing on the frame a loop wraps. |
| audio | A deterministic run with a device plays on the wall clock (without one, sounds advance by game time). |
| canvas3d | Only SDF text is re-shaded for 3D (other custom 2D shaders need texture mode); canvas hit tests ignore z-index, render layers and masks. |
| data | Masks, filters, hit areas and handlers are saved as loaded, not read back; edited prefab files reload only when a scene using them changes. |
| debug | Overlay glyphs are rasterised at 1 pixel per unit (soft on HiDPI); log capture is process-wide (one engine per process). |
| events | Event paths stop at 64 ancestors; hit tests walk the ancestor chain per node; headless touch coordinates pass through unscaled; the hover ticker replays the stored stage point (a camera moving under a still mouse is not re-mapped). |
| filters | Rotated atlas frames are returned as stored by extract; huge filter areas drop resolution at 4096 px; a cached node with filters re-renders when its bounds change. |
| gpu | Pipeline/sampler caches are linear arrays; readback handles 8-bit RGBA/BGRA only. |
| graphics | Ear clipping is O(n^2) per ear, hole bridging O(ring * edges), hit tests scan cached triangles linearly. |
| math | Collinear overlapping segments report no intersection. |
| particles | Scratch memory keeps its peak size; a container drawn twice in a frame uses the last matrix for both. |
| playtest | Every mouse motion is recorded; the log hook is process-wide; a node property read builds the node's whole JSON. |
| profiler | A single frame longer than 4.29 s is clamped; the GPU estimate polls fences every 0.1 ms. |
| sprites | Nine-slice tile fill falls back to stretching beyond 256 tiles per axis. |
| text | Kerning pairs only for sets up to 256 glyphs; a full 4096 atlas stops caching new glyphs (no eviction); diagonal gradients sampled at glyph corners. |
| three_d | Skinned bounds = bind pose grown 2x; batched meshes culled as one box; one depth bias per light (from the first cascade). |

## Repository layout

```
engine/include/braty/   public headers (the API reference); braty.h includes them all
engine/src/<module>/    sources; private headers *_internal.h
engine/shaders/         HLSL + compiled/ headers (committed; tools/compile_shaders.py)
engine/assets/fonts/    the embedded default font (Noto Sans) + licence
third_party/            stb, cgltf, cJSON, microui (vendored, with licences)
examples/               one program per module (+ generated demo data)
sample_projects/        star_courier: the sample game (builds standalone too)
tests/                  headless CTest programs (tests/test_<module>.c), golden/ (reference images)
tools/                  compile_shaders.py, playtest.py (drive a game), golden.py (image tests)
docs/                   BUILDING, GETTING_STARTED, TESTING, PLAYTESTING, PLATFORMS, DATA_FORMATS, API_REVIEW,
                        agents/<module>.md (module guides), plans/
.github/workflows/      CI (Windows, Linux, macOS; not yet run)
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
  `examples/audio/LICENSE-gong.txt`).
- Star Courier: all art and sound are original, generated by
  `sample_projects/star_courier/tools/make_assets.py`.
