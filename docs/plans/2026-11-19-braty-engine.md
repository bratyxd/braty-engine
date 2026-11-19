# Braty Engine Implementation Plan

> **For agentic workers:** each task below is implemented by one agent. Read this whole
> document first, then `docs/BUILDING.md` and the public headers in `engine/include/braty/`
> (once they exist). Your task's "Owns" list is the only set of files you may create or edit.

**Goal:** A cross-platform, editor-less, data-driven game engine in pure C on SDL3 + the SDL3 GPU
API, with the feature set of PixiJS v8 (plus the `@pixi/3d` beta) and none of the web's limits.

**Architecture:** One static library (`braty`). All scene data lives in flat tables inside a
`br_engine` (nodes, per-type data pools, user components); behaviour is a list of systems
(plain functions) that the engine runs in phase order every frame. Rendering is two-phase:
systems walk the scene and fill a CPU draw list, then one copy pass uploads and render passes
replay the list through SDL_GPU.

**Tech stack:** C11, CMake >= 3.24 + Ninja, SDL 3.4.20 (static), HLSL shaders compiled offline
with `dxc` + `spirv-cross` to SPIR-V / DXIL / MSL, stb (image, image_write, truetype, rect_pack,
vorbis), cgltf, cJSON, microui.

**Spec:** the section "Spec" below (the user's request plus the PixiJS feature inventory).

---

## Spec

User request, condensed:

- Cross-platform game engine, **no editor**, **fully data driven**: "data and systems that iterate over it".
- SDL3 and the SDL3 GPU API. **C only, no C++.**
- Feature-wise a copy of PixiJS (including the new 3D support), with web restrictions removed.
- TypeScript API translated to C conventions.
- **No voodoo / black magic / clever syntax.** Straightforward code.
- In-game debug screens may use nuklear or microui (we use microui).
- Git repo with meaningful commits.
- `sample_projects/` with a simple game.
- One code base must publish to all platforms, including consoles once SDL3 supports them.

### PixiJS -> Braty feature map

| PixiJS | Braty module | Task |
|---|---|---|
| Application, TickerPlugin, ResizePlugin | `br_app`, `br_ticker` (systems + phases) | 1, 2 |
| Maths (Point, Matrix, shapes, helpers), Color | `br_math`, `br_shapes`, `br_color` | 1 |
| Renderer, Texture, TextureSource, TextureStyle, RenderTexture, pipelines, batching | `br_gpu`, `br_texture`, `br_render`, `br_batch` | 1, 2 |
| Container, scene graph, render groups, render layers, zIndex, Culler, Bounds | `br_node`, `br_scene`, `br_bounds`, `br_cull` | 2 |
| Sprite | `br_sprite` | 2 |
| NineSliceSprite, TilingSprite, AnimatedSprite, GifSprite | `br_sprite_nine`, `br_sprite_tiling`, `br_sprite_anim` | 6 |
| Mesh, MeshSimple, MeshRope, MeshPlane, PerspectiveMesh | `br_mesh` | 6 |
| ParticleContainer, Particle | `br_particles` | 6 |
| Graphics, GraphicsContext, GraphicsPath, FillGradient, FillPattern, strokes, dashes, SVG paths | `br_graphics` | 3 |
| Text, TextStyle, BitmapText, BitmapFont, HTMLText, SplitText | `br_text`, `br_font`, `br_bitmap_font`, `br_rich_text` | 4 |
| Assets, Cache, Loader, Resolver, manifests, bundles, background loader, Spritesheet, compressed textures | `br_assets`, `br_spritesheet` | 2 (core), 5 |
| EventSystem, EventBoundary, Federated*Event | `br_events`, `br_input` | 7 |
| @pixi/sound | `br_audio` | 7 |
| Filters (Alpha, Blur, ColorMatrix, Displacement, Noise, custom), advanced blend modes | `br_filter`, `br_blend` | 8 |
| Masks (Alpha, Color, Scissor, Stencil), cacheAsTexture, ExtractSystem, GenerateTextureSystem | `br_mask`, `br_cache`, `br_extract` | 2 (scissor/stencil plumbing), 8 |
| @pixi/3d: View3D, Mesh3D, geometries, Material3D (PBR/Phong/Flat/Toon), lights, cascaded shadows, environment, OrbitCamera, BatchedMesh3D, HDR + tone mapping, post-processing, picking | `br_view3d`, `br_mesh3d`, `br_material3d`, `br_light3d`, `br_camera3d`, `br_post3d` | 9 |
| @pixi/3d: glTF, skinning, morph targets, animation | `br_gltf`, `br_anim3d` | 11 |
| Extensions registry | node-type registry, asset-loader registry, component registry, system list | 2 |
| (no Pixi equivalent) scene files, prefabs, game config | `br_scene_file`, `br_prefab`, `br_game_config` | 10 |
| (dev tools) | `br_debug` (microui overlay) | 10 |

**Web-only, intentionally not ported:** AccessibilitySystem (DOM screen readers), DOMContainer,
HTMLSource, Canvas2D renderer, DOMAdapter/WebWorkerAdapter, crossOrigin, WorkerManager.
HTMLText is replaced by tagged rich text rendered by the engine.

**Deferred (write a `// ponytail:` comment where the hook would go, list in README):** video
textures (SDL has no codec), Basis/KTX2 transcoding, Draco/meshopt glTF compression, SVG *file*
import beyond path data, GPU compute culling.

**Web restrictions removed:** direct file system and save-data access, real threads for
background loading, raw keyboard/mouse/gamepad/touch/pen input, multiple gamepads with rumble,
no texture-size or audio-autoplay limits, synchronous loading allowed, fullscreen/vsync/display
control, native binaries.

---

## Global Constraints

These apply to every task.

**Language and style**
- C11 only. No C++, no compiler extensions, no `_Generic`, no X-macros, no macros that generate
  functions or types, no token pasting, no `setjmp`, no function-like macros beyond trivial
  constants and `BR_ARRAY_COUNT(a)` / `BR_CHECK` in tests.
- Straightforward code: plain structs, plain functions, plain `for` loops. If a reader needs to
  stop and decode a line, rewrite it.
- Must compile clean with `-Wall -Wextra -Wpedantic` (GCC/Clang) and `/W4` (MSVC) for engine code.
- Naming: functions `br_<module>_<verb>[_<noun>]`, types `br_<name>` (typedef'd structs),
  enum values `BR_<MODULE>_<VALUE>`, files `br_<module>.h/.c`. Public headers in
  `engine/include/braty/`, private headers next to their `.c` file with suffix `_internal.h`.
- No hidden global state. Every function takes `br_engine *e` (or a sub-object) explicitly.
- Errors: functions return `bool` (true = ok) or a handle/pointer (zero/NULL = failure) and call
  `br_log_error(...)` with the reason. Never abort on bad user data (missing file, bad JSON).
- Comments explain *why*, briefly. Every public function has a one-line doc comment.

**Portability (console-ready)**
- Engine and game code call only SDL3 and the C standard math/string functions. No `<windows.h>`,
  no POSIX, no `fopen`. Files go through `br_file_*` (wrapping `SDL_IOStream` / `SDL_Storage`).
- Entry point uses SDL main callbacks (`SDL_MAIN_USE_CALLBACKS`). No blocking main loop.
- No platform `#ifdef` outside `engine/src/core/br_platform.c`.
- Shaders are compiled offline for every backend; the runtime never compiles shader source.
  `br_shader_blob` has one slot per `SDL_GPUShaderFormat`, so a console format is one more slot.
- Asset paths are forward-slash, relative to the asset root, case-sensitive.
- No reliance on `sizeof(long)`, endianness-specific casts, or unaligned reads.

**Data-driven rule**
- State is data in tables; behaviour is systems. A node type or game feature is: a data struct in
  a dense pool + functions that iterate that pool. No per-object virtual methods beyond the
  node-type descriptor table defined in Task 2.

**Build layout**
- `CMakeLists.txt` globs `engine/src/**/*.c`, turns every `tests/test_*.c` into a CTest
  executable and every `examples/*.c` into an executable. Adding a file needs no CMake edit.
- SDL3: `find_package(SDL3 CONFIG)` first, `FetchContent` of tag `release-3.4.20` as fallback.
  A prebuilt copy lives at `D:/personal/engines/b1/.deps/sdl3` (gitignored); pass
  `-DCMAKE_PREFIX_PATH=D:/personal/engines/b1/.deps/sdl3` so you do not rebuild SDL.
- Compiled shaders are committed (`engine/shaders/compiled/<name>.<stage>.h`), one generated
  header per shader, so building needs no shader toolchain.

**Verification**
- Every executable built on `br_app` accepts `--frames N --screenshot out.png` (run N frames,
  write the final frame as PNG, exit 0). Use it and look at the PNG before claiming a visual
  feature works.
- Logic gets a `tests/test_<module>.c` (plain `main`, `BR_CHECK(cond)` from `tests/br_test.h`).
- Each module ships `examples/<module>_demo.c` exercising its features on screen.
- Before each commit: full build with zero warnings in your files, `ctest` green.

**Agent-friendliness (user requirement)**
- `AGENTS.md` at the repo root is the entry point for any AI agent working on the engine or
  making a game with it: what the engine is, how to build/test/screenshot, the code rules above,
  the data-driven model (nodes, components, systems), "how to add a node type / component /
  system / asset loader / shader", and an index of module guides. `CLAUDE.md` contains only
  `@AGENTS.md`. Task 1 creates both, with the index listing every `docs/agents/<module>.md`
  named below; Task 12 does the final pass.
- Every task writes `docs/agents/<module>.md` for each module it owns (file names = module
  directory names: `core`, `math`, `gpu`, `app`, `scene`, `render`, `assets`, `graphics`, `text`,
  `sprites`, `mesh`, `particles`, `events`, `audio`, `filters`, `three_d`, `gltf`, `data`,
  `debug`). Each is short and task-oriented: what the module does, the 5-10 functions you
  actually need, one minimal working code snippet (compiled as part of the module's example so
  it cannot rot), the scene-JSON form if any, and gotchas. These files are in each task's
  "Owns" list implicitly.
- Public headers are the API reference: one-line doc comment per function, grouped by topic,
  so an agent can learn a module by reading a single header.

**Git**
- Small meaningful commits, imperative subject, prefixed by module (`graphics: add dashed strokes`).
- `git add` only files you own. Never `git add -A`. Never rewrite history.

## Review Focus

Things no single task's happy path exercises, most likely to bite first:

1. **Destroying a node while systems/events iterate** (e.g. inside a click handler or a system
   callback): destruction is deferred to end of frame; stale handles fail generation checks and
   are ignored, never crash. Test in Task 2 (`test_node.c`) and Task 7 (`test_events.c`).
2. **Window minimised / zero-size / swapchain not acquired:** frame is skipped cleanly, no GPU
   calls with a null swapchain texture, systems still update. Task 1.
3. **Missing or corrupt asset files** (bad PNG, truncated JSON, absent font): load returns
   failure, logs path and reason, a 2x2 magenta fallback texture is used for sprites. Task 2, 5.
4. **Very large batches** (> 65535 vertices, > 8 textures in a frame): batches split, index type
   stays valid. Task 2 (`examples/sprites.c` with 100k sprites).
5. **Resize and DPI change mid-frame:** render targets for filters/masks/3D are recreated at the
   new size; logical vs pixel size (`resolution`) stays consistent. Tasks 2, 8, 9.

---

## Directory layout

```
CMakeLists.txt  CMakePresets.json  README.md  .gitignore
docs/BUILDING.md  docs/plans/
engine/include/braty/        public headers, braty.h umbrella, braty_main.h entry point
engine/src/core/             memory, log, array, pool, hash, string, file, platform
engine/src/math/             vec/mat/quat, shapes, color
engine/src/app/              app, ticker, window, systems
engine/src/gpu/              device, shader, pipeline cache, buffers, textures, render targets
engine/src/render/           draw list, batcher, frame, masks, blend
engine/src/scene/            node table, node types, components, transform, bounds, cull, sprite
engine/src/assets/  graphics/  text/  sprites/  mesh/  particles/
engine/src/events/  audio/  filters/  three_d/  data/  debug/
engine/shaders/              *.vert.hlsl *.frag.hlsl + compiled/
third_party/                 stb, cgltf, cJSON, microui (+ licences)
tools/compile_shaders.py
tests/  examples/  sample_projects/
```

## Core contracts (fixed by Tasks 1 and 2; later tasks build on these)

```c
/* handles: index + generation; generation 0 = invalid */
typedef struct br_node    { uint32_t index; uint32_t generation; } br_node;
typedef struct br_texture { uint32_t index; uint32_t generation; } br_texture;

/* node types: one descriptor per kind of scene object, registered at engine init */
typedef struct br_node_type_desc {
    const char *name;                 /* "sprite", "graphics", ... also the JSON "type" */
    size_t      data_size;            /* per-node data stored in a dense pool */
    void (*on_create)(br_engine *e, br_node n, void *data);
    void (*on_destroy)(br_engine *e, br_node n, void *data);
    bool (*local_bounds)(br_engine *e, br_node n, const void *data, br_rect *out);
    void (*render)(br_engine *e, br_node n, const void *data, br_render_ctx *rc);
    bool (*hit_test)(br_engine *e, br_node n, const void *data, br_vec2 local_point);
    bool (*from_json)(br_engine *e, br_node n, void *data, const cJSON *json);
} br_node_type_desc;
uint32_t br_node_type_register(br_engine *e, const br_node_type_desc *desc);
br_node  br_node_create(br_engine *e, uint32_t type_id);
void    *br_node_data(br_engine *e, br_node n, uint32_t type_id);  /* NULL if wrong type/stale */

/* user components: dense arrays, described field by field so scene files can fill them */
typedef struct br_field_desc { const char *name; br_field_type type; size_t offset; } br_field_desc;
typedef struct br_component_desc {
    const char *name; size_t size; const br_field_desc *fields; uint32_t field_count;
} br_component_desc;
uint32_t br_component_register(br_engine *e, const br_component_desc *desc);
void    *br_component_add(br_engine *e, br_node n, uint32_t component_id);
void    *br_component_get(br_engine *e, br_node n, uint32_t component_id);
void     br_component_remove(br_engine *e, br_node n, uint32_t component_id);
uint32_t br_component_count(br_engine *e, uint32_t component_id);
void    *br_component_at(br_engine *e, uint32_t component_id, uint32_t i);      /* i-th data */
br_node  br_component_owner_at(br_engine *e, uint32_t component_id, uint32_t i);

/* systems: everything that runs per frame, engine-internal work included */
typedef enum br_phase { BR_PHASE_INPUT, BR_PHASE_UPDATE, BR_PHASE_LATE_UPDATE,
                        BR_PHASE_PRE_RENDER, BR_PHASE_RENDER, BR_PHASE_POST_RENDER } br_phase;
typedef void (*br_system_fn)(br_engine *e, float dt_seconds, void *user);
void br_system_add(br_engine *e, const char *name, br_phase phase, int priority,
                   br_system_fn fn, void *user);
void br_system_set_enabled(br_engine *e, const char *name, bool enabled);

/* asset loaders: one per file extension */
typedef struct br_asset_loader_desc {
    const char *type_name;              /* "texture", "font", ... */
    const char *extensions;             /* "png,jpg,jpeg" */
    void *(*load)(br_engine *e, const char *path, const void *bytes, size_t size);
    void  (*unload)(br_engine *e, void *asset);
} br_asset_loader_desc;
void  br_assets_register_loader(br_engine *e, const br_asset_loader_desc *desc);
void *br_assets_load(br_engine *e, const char *path);   /* cached; NULL on failure */
void *br_assets_get(br_engine *e, const char *path);
void  br_assets_unload(br_engine *e, const char *path);

/* game entry point (braty_main.h, included once by the game) */
typedef struct br_app_desc {
    const char *title; int width; int height; const char *asset_root;
    bool (*init)(br_engine *e);      /* register components, systems, load first scene */
    void (*shutdown)(br_engine *e);
} br_app_desc;
br_app_desc br_main(int argc, char **argv);   /* the only function a game must define */
```

Task 2 also creates, for every module of Tasks 3-11, a stub public header (include guard only)
listed in `braty.h`, and a stub `.c` with an empty `void br_<module>_register(br_engine *e)`
called from `engine/src/app/br_builtin.c`. Later tasks fill their stubs; nobody edits
`braty.h` or `br_builtin.c` again.

---

## Tasks

Order: 1 -> 2 -> {3,4,5,6,7,8 in parallel} -> {9,10 in parallel} -> {11,12 in parallel} -> 13.
Parallel tasks run in separate git worktrees and are merged by the orchestrator.

### Task 1: Foundation (build, core, math, GPU, app loop)

**Owns:** `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `docs/BUILDING.md`, `third_party/**`,
`tools/**`, `engine/src/core/**`, `engine/src/math/**`, `engine/src/gpu/**`, `engine/src/app/**`,
matching headers, `engine/shaders/**`, `tests/br_test.h`, `tests/test_core.c`, `tests/test_math.c`,
`examples/hello_quad.c`.

- [ ] CMake project per "Build layout". Build SDL 3.4.20 once and install to `.deps/sdl3`.
      Presets: `windows-gcc`, `windows-msvc`, `linux`, `macos`, `ios`, `android` (the last four
      configured but untested here; say so in `docs/BUILDING.md`).
- [ ] Vendor into `third_party/` with licence files: stb_image, stb_image_write, stb_truetype,
      stb_rect_pack, stb_vorbis, cgltf, cJSON, microui. One `.c` per library compiling it.
- [ ] `core`: `br_alloc/br_realloc/br_free`, `br_log_*`, growable array (`br_array` of bytes with
      element size, plain functions), generational pool, string-keyed hash map, small string
      helpers, `br_file_read_all(e, path, &bytes, &size)` over `SDL_IOStream`, save-data access
      over `SDL_Storage`.
- [ ] `math`: `br_vec2/3/4`, `br_mat3` (2D affine, Pixi `Matrix` semantics), `br_mat4`, `br_quat`,
      shapes (`br_rect`, `br_circle`, `br_ellipse`, `br_rounded_rect`, `br_triangle`, `br_polygon`)
      with `contains` and bounds, segment/line intersection, `br_color` (rgba float, hex, HSL,
      CSS-style name/`#rrggbb` string parse, premultiply).
- [ ] `gpu`: device + swapchain setup, `br_shader_blob` selection by `SDL_GetGPUShaderFormats`,
      pipeline cache keyed by (shaders, vertex layout, blend, stencil, target format, samples),
      textures + samplers (`br_texture_style`: filter, address mode, mipmaps, anisotropy), render
      targets, transient per-frame vertex/index/uniform upload, texture readback.
- [ ] `tools/compile_shaders.py`: HLSL -> SPIR-V and DXIL (`dxc`), SPIR-V -> MSL (`spirv-cross`),
      emits one header per shader with three byte arrays. Follow SDL_GPU's resource-set
      conventions (vertex: textures space0 / uniforms space1; fragment: textures space2 /
      uniforms space3). MSL output cannot be tested on this machine; say so in BUILDING.md.
- [ ] `app`: `braty_main.h` with the SDL callbacks, `br_engine` create/destroy, system list with
      phases and priorities, ticker (delta, elapsed, `max_fps`, `min_fps` clamp, time scale),
      resize handling, `--frames/--screenshot`, skip-frame on missing swapchain (Review Focus 2).
- [ ] `examples/hello_quad.c`: window, clears, draws one textured quad through raw `br_gpu`.
- [ ] Verify: `ctest` green; `hello_quad --frames 3 --screenshot` PNG shows the quad.

### Task 2: Scene graph and 2D render core

**Owns:** `engine/src/scene/**`, `engine/src/render/**`, `engine/src/assets/br_assets.c`,
`engine/src/app/br_builtin.c`, `engine/include/braty/braty.h`, all stub files,
`tests/test_node.c`, `tests/test_components.c`, `tests/test_transform.c`, `examples/sprites.c`.

- [ ] Node table (struct of arrays in `br_engine`): hierarchy links, position, scale, rotation,
      skew, pivot, origin, local/world `br_mat3`, alpha, tint, visible, renderable, z_index,
      sortable_children, blend mode, label, cull flags, world alpha/tint, dirty flags.
      Full Pixi `Container` API as `br_node_*` functions (add/remove/swap/reparent child, child
      index, get by label, to_global/to_local, destroy with children, deferred destroy).
- [ ] Node-type registry, user components, systems exactly as in "Core contracts".
- [ ] Engine systems registered in phases: transform update (dirty only), z-sort, bounds, culling
      (`cullable`, `cull_area`), render collect, frame submit.
- [ ] Draw list + batcher: quads and arbitrary triangles, 8 textures per batch, splits on
      texture overflow / blend change / 65535 vertices; commands for begin/end pass on any
      render target, scissor, stencil reference, and a callback command (used by 3D and filters).
- [ ] Textures: `br_texture` = source + frame + orig + trim + rotate + default anchor; magenta
      fallback; texture loader (png/jpg/bmp/tga via stb_image) registered with assets core.
      Render textures and `br_render_to_texture(e, node, target)`.
- [ ] Blend modes: normal, add, multiply, screen, erase, none, plus premultiplied variants.
- [ ] Render groups (`is_render_group`) and render layers (draw order decoupled from hierarchy).
- [ ] Mask plumbing: scissor mask (axis-aligned) and stencil mask (any node as mask, nested,
      inverse). Alpha/colour masks come in Task 8.
- [ ] `br_sprite`: texture, anchor, tint, width/height setters, round pixels.
- [ ] Assets core per contract, cache keyed by path, alias support.
- [ ] Stubs + `br_builtin.c` for Tasks 3-11.
- [ ] Verify: tests green including deferred-destroy (Review Focus 1); `sprites --frames 60
      --screenshot` with 100k sprites renders and reports batch count (Review Focus 4).

### Task 3: Graphics

**Owns:** `engine/src/graphics/**`, `engine/include/braty/br_graphics.h`, graphics shaders,
`tests/test_graphics.c`, `examples/graphics_demo.c`.

- [ ] `br_graphics_context` (shareable between nodes) with the Pixi v8 API: `move_to`, `line_to`,
      `bezier_curve_to`, `quadratic_curve_to`, `arc`, `arc_to`, `arc_to_svg`, `close_path`, `rect`,
      `round_rect`, `circle`, `ellipse`, `poly`, `regular_poly`, `round_poly`, `star`,
      `chamfer_rect`, `fillet_rect`, `svg_path(string)`, `fill`, `stroke`, `cut` (holes), `clear`,
      transform stack (`save/restore/translate/rotate/scale/set_transform`), `texture`.
- [ ] Fill styles: colour+alpha, linear and radial gradient (`br_fill_gradient`, colour stops,
      local/global space), texture pattern (`br_fill_pattern`) with matrix.
- [ ] Stroke styles: width, colour, alignment, cap (butt/round/square), join (miter/bevel/round),
      miter limit, dashed lines, 1-pixel lines.
- [ ] Triangulation: ear clipping with holes for polygons, dedicated builders for rect / circle /
      rounded rect, adaptive curve subdivision. Output goes through the Task 2 batcher.
- [ ] Bounds and `hit_test` (point in filled/stroked shape) for the node type.
- [ ] Tests: triangle counts and area for known shapes, polygon-with-hole area, svg path parse.

### Task 4: Text

**Owns:** `engine/src/text/**`, `engine/include/braty/br_text.h`, `br_font.h`, `br_bitmap_font.h`,
`br_rich_text.h`, text shaders, `engine/assets/fonts/**`, `tests/test_text.c`, `examples/text_demo.c`.

- [ ] TTF/OTF fonts via stb_truetype with a dynamic glyph atlas (stb_rect_pack), UTF-8, kerning,
      atlas growth, per-size caches. One OFL-licensed default font embedded (licence included).
- [ ] `br_text_style`: font family/size/style/weight, fill (colour or gradient), stroke, drop
      shadow, align, word wrap + width, break words, line height, letter spacing, leading,
      padding, white-space modes, text baseline.
- [ ] `br_text` node: text, style, anchor, resolution; measured bounds (`br_text_measure`).
- [ ] `br_bitmap_font`: AngelCode `.fnt` text + XML, multi-page, kerning, SDF/MSDF shader;
      `br_bitmap_text` node; `br_bitmap_font_install` (generate a bitmap font from a TTF + style).
- [ ] `br_rich_text` (HTMLText replacement): tags `<b> <i> <u> <s> <br> <color=#> <size=> <font=>`
      and named tag styles (Pixi `tagStyles`).
- [ ] `br_split_text`: split into per-line / word / character nodes.
- [ ] Loaders registered for `ttf,otf,fnt,xml`.
- [ ] Tests: UTF-8 decode, wrap positions, measure against known advances, `.fnt` parse, tag parse.

### Task 5: Assets

**Owns:** `engine/src/assets/**` (takes over `br_assets.c`), `engine/include/braty/br_assets.h`,
`br_spritesheet.h`, `tests/test_assets.c`, `examples/assets_demo.c`, `examples/assets/**`.

- [ ] Resolver: aliases, base path, `@2x`-style resolution suffix and format preference lists
      (`{png,dds}`), search params replaced by per-load options struct.
- [ ] Manifests and bundles (Pixi manifest JSON shape), `br_assets_load_bundle`,
      `br_assets_unload_bundle`, progress callback.
- [ ] Background loading on worker threads (`SDL_Thread`): file IO + decode off-thread, GPU
      upload on main thread at a per-frame time budget; `br_assets_load_async` with completion
      callback; `br_assets_background_load`.
- [ ] Loaders: json, text, binary, spritesheet (TexturePacker JSON hash + array, trims, rotation,
      animations, multi-pack, 9-slice borders), DDS (BC1-7) and KTX (uncompressed + BCn/ASTC/ETC2
      passed straight to the GPU when `SDL_GPUTextureSupportsFormat`), wav/ogg hand-off to audio.
- [ ] Reference counting and `br_assets_unload` freeing GPU resources; cache inspection API.
- [ ] Failure behaviour per Review Focus 3.
- [ ] Tests (no GPU): resolver, manifest parse, spritesheet parse, corrupt-file handling.

### Task 6: Sprite variants, Mesh, Particles

**Owns:** `engine/src/sprites/**`, `engine/src/mesh/**`, `engine/src/particles/**`, their headers and
shaders, `tests/test_sprites.c`, `examples/sprites_demo.c`, `examples/mesh_demo.c`,
`examples/particles_demo.c`.

- [ ] `br_nine_slice_sprite` (left/top/right/bottom, width/height).
- [ ] `br_tiling_sprite` (tile position/scale/rotation, clamp margin, any texture incl. atlas frames).
- [ ] `br_animated_sprite` (frames with per-frame durations, speed, loop, play/stop/goto,
      on_complete / on_frame_change / on_loop callbacks, update-anchor) + GIF decode into frames.
- [ ] `br_mesh` (geometry: positions, uvs, indices, optional colours; texture; custom shader +
      uniforms), `br_mesh_simple`, `br_mesh_rope`, `br_mesh_plane`, `br_perspective_mesh`.
- [ ] `br_particle_container`: flat array of `br_particle` (position, scale, rotation, anchor,
      tint, alpha, texture frame), dynamic/static property flags, instanced draw, 1M particles.
- [ ] Tests: nine-slice vertex layout, animation frame advance timing, rope geometry.

### Task 7: Events, input, audio

**Owns:** `engine/src/events/**`, `engine/src/audio/**`, `engine/include/braty/br_events.h`,
`br_input.h`, `br_audio.h`, `tests/test_events.c`, `examples/events_demo.c`, `examples/audio_demo.c`.

- [ ] `br_input`: keyboard (down/pressed/released by scancode), mouse, multi-touch, pen,
      gamepads (buttons, axes, rumble, hot-plug), text input. Fed from `SDL_AppEvent`.
- [ ] Federated events: `event_mode` (none/passive/auto/static/dynamic), `hit_area`,
      `interactive_children`, cursor; pointerdown/up/upoutside/move/over/out/enter/leave/tap/
      click/rightclick/wheel + global move; capture, target and bubble phases, `stop_propagation`;
      listeners as `(type, fn, user)` rows in a table per node; `br_node_on/off/once`.
- [ ] Hit testing through the scene in reverse draw order, respecting masks and node `hit_test`.
- [ ] Listener-safe node destruction (Review Focus 1).
- [ ] `br_audio`: wav + ogg (stb_vorbis), streaming for long files, mixer over `SDL_AudioStream`
      with instances (volume, pan, speed, loop, pause, seek), groups/buses, global mute/volume,
      sound sprites, completion callback.
- [ ] Tests (no window): hit-test ordering, propagation order, listener removal during dispatch.

### Task 8: Filters, masks, effects

**Owns:** `engine/src/filters/**`, `engine/include/braty/br_filter.h`, `br_mask.h`, `br_extract.h`,
filter shaders, `tests/test_filters.c`, `examples/filters_demo.c`.

- [ ] Filter pipeline: per-node filter list, filter area, padding, resolution, pooled render
      targets, ping-pong passes, blend back into parent target; nested filters.
- [ ] Built-in: Alpha, Blur (separable, strength/quality/kernel size), ColorMatrix (all Pixi
      presets: brightness, contrast, saturate, hue, greyscale, sepia, negative, ...), Displacement,
      Noise. Custom filters: user shader blob + uniform struct + extra textures.
- [ ] Advanced blend modes (the 21 from `pixi.js/advanced-blend-modes`) via a backdrop copy.
- [ ] Alpha mask (sprite/any node as mask through an offscreen target) and colour mask.
- [ ] `cache_as_texture` (render subtree once, redraw on `update_cache_texture`).
- [ ] Extract: `br_extract_pixels`, `br_extract_image_png`, `br_generate_texture(node, options)`.
- [ ] Target recreation on resize (Review Focus 5).
- [ ] Tests: colour-matrix maths, kernel weights; demo screenshot showing every built-in filter.

### Task 9: 3D core

**Owns:** `engine/src/three_d/**` except `br_gltf*.c` / `br_anim3d*.c`, headers `br_view3d.h`,
`br_mesh3d.h`, `br_material3d.h`, `br_light3d.h`, `br_camera3d.h`, `br_post3d.h`, 3D shaders,
`tests/test_three_d.c`, `examples/view3d_demo.c`.

- [ ] `br_view3d` node type: sits in the 2D scene like a sprite, owns a 3D node table
      (`br_node3d`: TRS with quaternion, hierarchy, world `br_mat4`), camera, environment
      (sky/ground hemisphere colours), auto-resize; rendered to an HDR target then composited, so
      2D filters and masks work on it.
- [ ] Cameras: perspective and orthographic, `look_at`, screen<->world ray; `br_orbit_camera`.
- [ ] Geometry: vertex layout (position, normal, tangent, uv0, uv1, colour, joints, weights),
      builders for cube, sphere, plane, cylinder, cone, torus, capsule; tangents; AABB.
- [ ] Materials: PBR metallic-roughness (base colour, metallic, roughness, normal, occlusion,
      emissive, alpha modes, double-sided), Phong, Flat (unlit), Toon; custom material = shader
      blobs + uniform block + textures.
- [ ] Lights: directional, point, spot, hemisphere ambient; directional cascaded shadow maps
      (1-4 cascades, PCF), spot shadows.
- [ ] `br_batched_mesh3d` (instancing: one geometry+material, N transforms/colours).
- [ ] Frustum culling on CPU; opaque front-to-back, transparent back-to-front sorting.
- [ ] HDR + tone mapping (ACES, Reinhard, linear), post chain with bloom and custom passes, MSAA.
- [ ] Picking: ray vs AABB then triangles; pointer events on 3D nodes via the Task 7 event types
      (if Task 7's header is not merged yet, expose `br_view3d_pick` and leave event wiring to
      Task 11).
- [ ] 2D-in-3D: any render texture (so any 2D container) usable as a material texture;
      `br_view3d_world_to_screen` for 2D elements following 3D ones.
- [ ] Tests: matrix/projection maths, ray-triangle, frustum-AABB, cascade split distances.

### Task 10: Data layer and debug overlay

**Owns:** `engine/src/data/**`, `engine/src/debug/**`, `engine/include/braty/br_scene_file.h`,
`br_prefab.h`, `br_game_config.h`, `br_debug.h`, `docs/DATA_FORMATS.md`, `tests/test_scene_file.c`,
`examples/data_demo.c`, `examples/data/**`.

- [ ] Scene files (`*.scene.json`): tree of `{ "type", "label", transform fields, type-specific
      fields, "components": { name: {fields} }, "children": [] }`. Uses each node type's
      `from_json`; implement `from_json` for every built-in node type from Tasks 2-9 inside
      `engine/src/data/br_node_json_<type>.c` (so those tasks' files are not edited).
- [ ] Component fields filled from JSON through `br_field_desc` (types: bool, int, float, vec2,
      vec3, colour, string, asset path, node reference by label).
- [ ] Prefabs: a scene file instantiated N times under a parent with overrides.
- [ ] `game.json`: window (title, size, fullscreen, vsync, resizable), asset root, manifest,
      start scene, ticker limits, clear colour. `br_app_desc` can be filled from it.
- [ ] Scene save (`br_scene_file_save`) for round-tripping and debugging; hot reload of scene
      files in debug builds via file modification time.
- [ ] Debug overlay (microui rendered through the batcher, toggle F1): FPS/frame time graph,
      draw calls, batches, node/texture/asset counts, scene tree with live property view, system
      list with per-system timings and enable toggles, log console.
- [ ] `docs/DATA_FORMATS.md` documenting every file format.
- [ ] Tests: load -> save -> load equality, bad JSON handling, component field parsing.

### Task 11: glTF and 3D animation

**Owns:** `engine/src/three_d/br_gltf*.c`, `br_anim3d*.c`, headers `br_gltf.h`, `br_anim3d.h`,
skinning/morph shader variants, `tests/test_gltf.c`, `examples/gltf_demo.c`, `examples/models/**`.

- [ ] `.gltf` / `.glb` loader via cgltf registered with assets: scenes, node hierarchy, meshes,
      PBR materials, textures + samplers, cameras, KHR_lights_punctual, KHR_materials_emissive_
      strength, KHR_materials_unlit, KHR_texture_transform.
- [ ] `br_view3d_from_model(e, model)` (Pixi `View3D.from`), `br_model_instantiate` into a view.
- [ ] Skinning (joint matrices in a uniform/storage buffer), morph targets, animation clips with
      linear/step/cubic-spline sampling, `br_animator3d` (play, cross-fade, speed, loop, events).
- [ ] 3D pointer-event wiring if Task 9 left it open.
- [ ] Tests: sampler interpolation, joint matrix maths on a two-bone fixture built in code.

### Task 12: Sample game, docs, packaging

**Owns:** `sample_projects/**`, `README.md`, `docs/GETTING_STARTED.md`, `docs/PLATFORMS.md`,
`.github/workflows/build.yml`.

- [ ] `sample_projects/star_courier/`: a small original arcade game (steer a ship, collect
      cargo, dodge debris, score + lives + restart). Everything authored as data: `game.json`,
      manifest, scenes, prefabs, spritesheet, bitmap font, sounds. C code is only component
      structs + systems (`movement`, `spawner`, `collision`, `score`, `hud`) iterating them.
      Uses sprites, graphics, text, particles, a filter, audio, keyboard + gamepad + touch.
      Art and sound generated by a script in `sample_projects/star_courier/tools/` (original work).
- [ ] Builds standalone with its own `CMakeLists.txt` consuming the engine via `add_subdirectory`.
- [ ] `README.md` (what it is, feature table vs PixiJS, deferred list), `GETTING_STARTED.md`,
      `PLATFORMS.md` (per-platform build + what a console port needs: SDL port, shader format slot).
- [ ] CI workflow building Windows/Linux/macOS and running `ctest`.
- [ ] Verify: game runs, screenshot of gameplay looks right, 60 s soak without leaks or errors.

### Task 13: Integration review (orchestrator)

- [ ] Merge all branches, full clean build, `ctest`, run every example with `--screenshot`.
- [ ] Review against Global Constraints (grep for banned constructs, platform headers, `fopen`).
- [ ] Fix or file what is found; update README's deferred list to match reality.
