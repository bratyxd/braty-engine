/* br_assets.h - asset loading and cache (PixiJS Assets). Core here; Task 5 adds the resolver,
 * manifests, bundles, background loading and more loaders.
 *
 * A loader handles file extensions and turns file bytes into an asset pointer. Assets are
 * cached by path: loading the same path twice returns the same pointer. Paths are relative to
 * the asset root, forward slashes, case-sensitive. Aliases map a short name to a path.
 *
 *   br_texture t = br_texture_load(e, "images/bunny.png");   (texture loader, br_texture.h)
 *   void *a = br_assets_load(e, "data/level.json");           (generic; NULL on failure, logged)
 */
#ifndef BR_ASSETS_H
#define BR_ASSETS_H

#include "braty/br_core.h"

/* asset loaders: one per file extension */
typedef struct br_asset_loader_desc {
    const char *type_name;              /* "texture", "font", ... */
    const char *extensions;             /* "png,jpg,jpeg" (lower case, no dots) */
    void *(*load)(br_engine *e, const char *path, const void *bytes, size_t size);
    void  (*unload)(br_engine *e, void *asset);
} br_asset_loader_desc;

/* Registers a loader (strings are copied). A later loader for the same extension wins. */
void br_assets_register_loader(br_engine *e, const br_asset_loader_desc *desc);
/* Loads (or returns the cached) asset. NULL on failure, with path and reason logged. */
void *br_assets_load(br_engine *e, const char *path);
/* The cached asset, NULL if not loaded. */
void *br_assets_get(br_engine *e, const char *path);
/* Unloads a cached asset (calls the loader's unload). */
void  br_assets_unload(br_engine *e, const char *path);
/* Unloads every cached asset. */
void br_assets_unload_all(br_engine *e);
/* Makes `alias` resolve to `path` in load/get/unload (copied). */
void br_assets_add_alias(br_engine *e, const char *alias, const char *path);
/* Loader type name of a cached asset ("texture", ...), NULL if not loaded. */
const char *br_assets_type(br_engine *e, const char *path);
/* Number of cached assets. */
uint32_t br_assets_count(br_engine *e);
/* Registers the assets core (called by br_builtin). */
void br_assets_register(br_engine *e);

#endif
