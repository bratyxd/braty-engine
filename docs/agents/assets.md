# assets - loaders, ref-counted cache, resolver, bundles, background loading, spritesheets

Headers: `engine/include/braty/br_assets.h`, `br_spritesheet.h`. Source: `engine/src/assets/`.
Test: `tests/test_assets.c` (headless). Example: `examples/assets_demo.c`, data in
`examples/assets/` (regenerate with `python examples/assets/generate_assets.py`).

## The model

- A **loader** handles file extensions (`br_asset_loader_desc`). Several loaders may share an
  extension: the newest whose `test(bytes)` accepts the file wins (`.json` -> "spritesheet" if it
  has a top-level `"frames"` object or array and a `"meta"` object with an `"image"` string, else
  "json"). If the chosen loader fails, the next candidate whose test accepts the file gets it (a
  .json shaped like a sheet whose image is missing still loads as "json"). `br_asset_options.type`
  forces a loader by type name. Tests run on worker threads: keep them pure (the spritesheet test
  is a small scanner, not cJSON, whose error position is a global).
- The **cache** is keyed by the resolved path and **reference counted**: every load (sync, async,
  bundle) takes a reference, `br_assets_unload` drops one, the loader's `unload` runs at zero.
  `br_assets_get` takes none. `br_assets_unload_all` frees everything regardless.
- The **resolver** turns a name into that path: alias -> already-cached path -> `{a,b}` pattern /
  candidate list choice (closest `@Nx` to the preferred resolution, then preferred format) -> base path.
  A name may expand to at most 16 names (`"x{a,b}{a,b}{a,b}{a,b}"`); more is an error, found
  before expanding (`'...' expands to more than 16 names`).
- A loader that loads its own path while it runs (a multi-pack whose `related_multi_packs` lists
  itself, or a cycle through sub-assets) gets an error for the inner load: the cache entry
  exists, marked, while the loader runs.
- A failed async load drops its cache entry with every reference in it (waiters get NULL).
  Groups remember the serial number of the entry their reference went into, so another owner
  loading the same path afterwards is not mistaken for the group's reference.
- DDS / KTX: the mip count is clamped to the full chain of the size (a 4x4 texture has 3
  levels), and the bytes the header asks for are checked against the file size before anything
  is allocated.
- **Groups** (`br_asset_group.h`, `br_asset_group.c`): a named set holding one reference per
  asset, loaded sync or async (polled each frame, never called back after unload/destroy), with
  lazy entries and manifest bundles. Every `br_scene` owns one; guide: `docs/agents/scenes.md`.
- **Capture** (`br_assets_capture_begin/_end`, internal): references taken by `br_assets_load*`
  outside loaders are recorded; the data layer hands them to the scene or file root that loaded them.
- **Background loading**: worker threads read files and run `decode`; the main thread runs
  `load`/`finish` (GPU uploads) in the `assets.update` system (INPUT phase) within a per-frame
  budget, then calls completion callbacks. Nothing async happens outside `br_engine_step`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_assets_load(e, path)` / `_with(e, path, &opts)` | Load or cache hit, +1 reference. NULL on failure (logged). |
| `br_assets_unload(e, path)`, `br_assets_ref_count`, `br_assets_list` | Release, inspect (type, path, refs, bytes, loading). |
| `br_assets_report(e)`, `br_assets_owners(e, path, out, max)` | Log every asset with its owners; the owners of one (groups, scenes, `file:`, `bundle:`). |
| `br_asset_group_create/add/load/load_async/unload/destroy` | Load and release a set together (`br_asset_group.h`). |
| `br_assets_load_async(e, path, opts, fn, user)` | Worker-thread load, +1 reference now, `fn` from a later step. |
| `br_assets_load_manifest(e, "manifest.json")` | Pixi-shaped manifest: bundles + aliases. |
| `br_assets_load_bundle(e, name, progress_fn, user)` / `_unload_bundle`, `_bundle_failed` | Background bundle load with progress; how many failed. |
| `br_assets_background_load(e, paths, n)` | Prefetch with zero references. |
| `br_assets_set_preferences(e, 2.0f, "dds,png")`, `br_assets_set_base_path` | Resolver choices. |
| `br_texture_load(e, path)` | Typed texture load, never fails (magenta fallback). Works for png and dds/ktx. |
| `br_spritesheet_load`, `br_spritesheet_frame`, `br_spritesheet_animation`, `_frame_borders` | TexturePacker atlases. |

## Snippet (examples/assets_demo.c)

```c
static void on_bundle_progress(br_engine *e, const char *bundle, float progress, void *user)
{
    demo_state *s = user;
    (void)e;
    (void)bundle;
    s->progress = progress;
    s->loaded = progress >= 1.0f; /* the last call has progress 1, failures included */
}

static void start_loading(br_engine *e, demo_state *s)
{
    br_assets_set_preferences(e, 1.0f, "dds,png");  /* "checker.{png,dds}" resolves to checker.dds */
    br_assets_load_manifest(e, "manifest.json");     /* bundles + aliases */
    br_assets_set_frame_budget(e, 0.0f);             /* demo only: one asset per frame */
    br_assets_load_bundle(e, "game", on_bundle_progress, s);
}

/* once s->loaded: */
    br_spritesheet *sheet = br_assets_get(e, "atlas");       /* loaded by the bundle */
    br_node root = br_stage_root(e);
    add_sprite(e, root, br_spritesheet_frame(e, sheet, "gem.png"), 340.0f, 60.0f);
    const br_texture *walk = NULL;
    uint32_t walk_count = br_spritesheet_animation(e, sheet, "walk", &walk);
    br_texture missing = br_texture_load(e, "missing");      /* magenta fallback, logged */
```

Manifest (`examples/assets/manifest.json`):

```json
{ "bundles": [ { "name": "game", "assets": [
    { "alias": "atlas", "src": "atlas.json" },
    { "alias": "checker", "src": "checker.{png,dds}" },
    { "alias": ["hero", "h"], "src": ["hero@2x.png", "hero.png"], "type": "texture" } ] } ] }
```

## Writing a loader

From `engine/src/assets/br_assets_loaders.c`:

```c
br_asset_loader_desc desc;
SDL_zero(desc);                 /* every optional callback NULL */
desc.type_name = "json";
desc.extensions = "json";
desc.load = br_json_asset_load; /* (e, path, bytes, size) -> heap pointer, NULL = failure */
desc.unload = br_json_asset_unload;
br_assets_register_loader(e, &desc);
```

Optional: `test` (claim by content, pure), `decode` + `finish` (background split: `decode` on a
worker - parse bytes only, no engine/GPU, only `br_alloc/br_free/br_log_*`; `finish` on the main
thread consumes the decoded data and returns the asset), `byte_size` (for `br_assets_list`).
A loader that loads other assets uses `br_assets_load`/`br_assets_unload` (spritesheet: its image).

## Gotchas

- Paths are relative to the asset root, forward slashes. An alias target is used as given (no base path).
  Every read goes through `br_file_read_all`, so assets come from loose files or mounted `.brpack`
  packs alike, async loads included (the worker reads packs; see `docs/agents/core.md`).
- Failed loads are not cached; `br_texture_load` of a missing file retries (and logs) every call.
- Async callbacks get the resolved path; `br_assets_unload(e, that_path)` works.
- Unloading the last reference of an asset that is still loading cancels it (callbacks get NULL).
- JSON (and spritesheets) parse on the main thread: cJSON keeps its error position in a global.
- The spritesheet image is loaded synchronously inside the sheet's load (ponytail: no job dependencies yet).
- Linked multi-packs (`meta.related_multi_packs`) are followed only from the sheet loaded first.
- Compressed textures (dds/ktx): uploaded as-is if `SDL_GPUTextureSupportsFormat`, else a logged
  failure. They are not premultiplied on upload (author them premultiplied); sRGB variants use the
  UNORM format like PNGs do; 2D only (no cube/array/volume); ETC1/2 has no SDL_GPU format; KTX2/Basis deferred.
- Headless engines get texture records with sizes and no pixels, so tests can load atlases and DDS files.
- References taken while a scene file or prefab builds belong to that build (capture): they are
  released with the `br_scene` or the file's root node, not leaked.
- Shutdown joins the workers, finishes queued results and frees everything before the render module.
