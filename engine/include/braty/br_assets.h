/* br_assets.h - asset loading and cache (PixiJS Assets: Cache, Loader, Resolver, manifests,
 * bundles, background loading).
 *
 * A loader handles file extensions and turns file bytes into an asset pointer. Assets are
 * cached by path and reference counted: every load (sync, async, bundle) takes one reference,
 * every br_assets_unload drops one, and the asset is freed when the count reaches zero.
 * Paths are relative to the asset root, forward slashes, case-sensitive. Aliases map a short
 * name to a path.
 *
 *   br_texture t = br_texture_load(e, "images/bunny.png");   (texture loader, br_texture.h)
 *   void *a = br_assets_load(e, "data/level.json");           (generic; NULL on failure, logged)
 *   br_assets_load_async(e, "big.dds", NULL, on_done, user);  (finished inside br_engine_step)
 *
 * Built-in loaders: "json" (cJSON *), "text" (char *, txt/csv), "binary" (br_asset_blob *, bin),
 * "spritesheet" (br_spritesheet *, TexturePacker .json, see br_spritesheet.h), and "texture"
 * for dds/ktx compressed textures (br_texture *, same as the png loader of br_texture.h).
 * Audio files (wav/ogg) are registered by the audio module.
 */
#ifndef BR_ASSETS_H
#define BR_ASSETS_H

#include "braty/br_core.h"

/* ---- loaders */

/* One loader per kind of file. Zero-initialise, then fill; only type_name, extensions and
 * load (or decode + finish) are required. */
typedef struct br_asset_loader_desc {
    const char *type_name;              /* "texture", "font", ... */
    const char *extensions;             /* "png,jpg,jpeg" (lower case, no dots) */
    void *(*load)(br_engine *e, const char *path, const void *bytes, size_t size);
    void  (*unload)(br_engine *e, void *asset);
    /* Optional: claims a file by its content when several loaders share an extension
     * ("spritesheet" vs "json"). Must be a pure function (it may run on a worker thread).
     * NULL = accepts every file with a matching extension. */
    bool  (*test)(const char *path, const void *bytes, size_t size);
    /* Optional background split (set both or neither). decode runs on a worker thread and may
     * only parse bytes and call br_alloc/br_free/br_log_*: no engine state, no GPU. finish runs
     * on the main thread, consumes (frees) `decoded` and returns the asset, or NULL on failure.
     * Loads without a split read the file on a worker and call load on the main thread.
     * Synchronous loads use load when set, else decode + finish. */
    void *(*decode)(br_engine *e, const char *path, const void *bytes, size_t size);
    void *(*finish)(br_engine *e, const char *path, void *decoded);
    /* Optional: approximate memory an asset holds, GPU memory included. NULL = file size. */
    size_t (*byte_size)(br_engine *e, const void *asset);
} br_asset_loader_desc;

/* Registers a loader (strings are copied). A later loader for the same extension wins. */
void br_assets_register_loader(br_engine *e, const br_asset_loader_desc *desc);

/* ---- loading and the cache */

/* Per-load options (PixiJS search params / asset data). Zero = defaults. */
typedef struct br_asset_options {
    const char *type;        /* use the loader with this type name ("spritesheet"), not the extension */
    float       resolution;  /* textures: pixels per logical unit; 0 = from "@2x" in the file name, else 1 */
} br_asset_options;

/* Loads (or returns the cached) asset and takes a reference. NULL on failure (path and reason logged). */
void *br_assets_load(br_engine *e, const char *path);
/* br_assets_load with options (NULL = defaults). Options only matter for the first load. */
void *br_assets_load_with(br_engine *e, const char *path, const br_asset_options *options);
/* The cached asset, NULL if not loaded (or still loading). Takes no reference. */
void *br_assets_get(br_engine *e, const char *path);
/* Drops one reference; at zero the loader's unload frees the asset (GPU memory included). */
void  br_assets_unload(br_engine *e, const char *path);
/* Frees every cached asset whatever its reference count. */
void br_assets_unload_all(br_engine *e);
/* Makes `alias` resolve to `path` in load/get/unload (copied). */
void br_assets_add_alias(br_engine *e, const char *alias, const char *path);
/* Loader type name of a cached asset ("texture", ...), NULL if not loaded. */
const char *br_assets_type(br_engine *e, const char *path);
/* Number of cached assets (loaded or loading). */
uint32_t br_assets_count(br_engine *e);
/* References held on an asset (0 if not cached; background-loaded assets start at 0). */
uint32_t br_assets_ref_count(br_engine *e, const char *path);

/* ---- cache inspection */

/* One cached asset. Strings point into the cache: valid until the next load or unload. */
typedef struct br_asset_info {
    const char *path;        /* resolved path (the cache key) */
    const char *type;        /* loader type name; NULL while still loading */
    uint32_t    ref_count;
    size_t      byte_size;   /* approximate, see br_asset_loader_desc.byte_size */
    bool        loading;     /* queued or decoding on a worker */
} br_asset_info;

/* Fills up to `max` entries of `out` (any order). Returns the number of cached assets. */
uint32_t br_assets_list(br_engine *e, br_asset_info *out, uint32_t max);
/* Who holds references on an asset: names of asset groups ("scene:level1", your groups,
 * "file:<scene path>" for scene files loaded with br_scene_file_load) and loaded bundles
 * ("bundle:game"). Fills up to `max` names (valid until the owner goes away); returns the total.
 * References beyond the owners come from direct br_assets_load calls or from loaders. */
uint32_t br_assets_owners(br_engine *e, const char *path, const char **out, uint32_t max);
/* Logs every live asset with type, reference count, size and owners (one line each). */
void br_assets_report(br_engine *e);

/* ---- resolver (aliases, base path, "{a,b}" patterns, resolution and format preferences) */

/* Prefix for relative, non-alias paths ("" by default), e.g. "packs/hd". */
void br_assets_set_base_path(br_engine *e, const char *base_path);
/* What the resolver picks from "hero@{1,2}x.{dds,png}" or a manifest src list: the candidate
 * whose "@Nx" is closest to `resolution` (no suffix = 1), then the first format in `formats`
 * ("dds,png"; NULL or "" = no preference). Defaults: 1, no preference. */
void br_assets_set_preferences(br_engine *e, float resolution, const char *formats);
/* Resolves a name to the path that load/get/unload use: alias, cached path, pattern choice,
 * base path. False if the result does not fit `out`, or a pattern gives more than 16 names. */
bool br_assets_resolve(br_engine *e, const char *name, char *out, size_t out_size);
/* The "@Nx" resolution in a file name ("hero@2x.png" -> 2), 1 if none. */
float br_assets_resolution_of(const char *path);

/* ---- background loading (worker threads; results are finished inside br_engine_step) */

/* Called on the main thread when an async load ends: asset is NULL on failure. */
typedef void (*br_asset_done_fn)(br_engine *e, const char *path, void *asset, void *user);

/* Starts loading on a worker thread and takes a reference now. fn (may be NULL) runs from a
 * later br_engine_step, also when the asset was already cached. options may be NULL. */
void br_assets_load_async(br_engine *e, const char *path, const br_asset_options *options,
                          br_asset_done_fn fn, void *user);
/* Prefetches assets at low priority into the cache without taking references (PixiJS
 * backgroundLoad): a later br_assets_load takes the first reference. */
void br_assets_background_load(br_engine *e, const char *const *paths, uint32_t count);
/* Number of assets still loading. */
uint32_t br_assets_pending(br_engine *e);
/* Main-thread time per frame for finishing loads (GPU uploads), at least one asset per frame.
 * Default 4 ms. */
void br_assets_set_frame_budget(br_engine *e, float milliseconds);

/* ---- manifests and bundles (PixiJS manifest JSON)
 *
 *   { "bundles": [ { "name": "level1", "assets": [
 *       { "alias": "hero", "src": "images/hero@{1,2}x.{dds,png}" },
 *       { "alias": ["tiles", "t"], "src": ["atlas@2x.json", "atlas.json"], "type": "spritesheet" } ] } ] }
 *
 * "assets" may also be an object { "alias": "src", ... }. Each asset's src is resolved when the
 * manifest is added (current preferences) and its aliases point at the result.
 */

/* Adds the bundles and aliases of a manifest held in memory. False on bad JSON (logged). */
bool br_assets_add_manifest(br_engine *e, const char *json, size_t size);
/* Reads a manifest file and adds it. */
bool br_assets_load_manifest(br_engine *e, const char *path);

/* Called after each asset of a bundle finishes (failures count as finished); the last call has progress 1. */
typedef void (*br_asset_progress_fn)(br_engine *e, const char *bundle, float progress, void *user);

/* Loads every asset of a bundle in the background (one reference each). False if unknown. */
bool br_assets_load_bundle(br_engine *e, const char *bundle, br_asset_progress_fn fn, void *user);
/* Drops the references br_assets_load_bundle took. */
void br_assets_unload_bundle(br_engine *e, const char *bundle);
/* Number of assets in a bundle (0 if unknown). */
uint32_t br_assets_bundle_size(br_engine *e, const char *bundle);
/* Resolved path of asset `index` of a bundle, NULL if out of range. */
const char *br_assets_bundle_path(br_engine *e, const char *bundle, uint32_t index);
/* Assets of a bundle that are not in the cache: after the progress callback reached 1, the
 * ones that failed to load. */
uint32_t br_assets_bundle_failed(br_engine *e, const char *bundle);

/* ---- loader results of the built-in json/text/binary loaders */

/* What the "binary" loader returns. */
typedef struct br_asset_blob {
    size_t   size;
    uint8_t *data;
} br_asset_blob;

/* Registers the assets core and its built-in loaders (called by br_builtin). */
void br_assets_register(br_engine *e);

#endif
