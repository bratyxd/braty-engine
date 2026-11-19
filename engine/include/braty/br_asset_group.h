/* br_asset_group.h - a named set of assets loaded and released together.
 *
 * The asset API has three levels, all reference counted (br_assets.h):
 *   single  br_assets_load(e, path) / br_assets_unload(e, path)          (+ br_assets_load_async)
 *   group   br_asset_group_create / _add / _load or _load_async / _unload (this header)
 *   scene   every br_scene owns an implicit group (br_scene.h): loading the scene loads what its
 *           file references, unloading it releases them.
 *
 * A group holds at most one reference per asset. Two groups (or a group and your own
 * br_assets_load) that share an asset keep it alive until the last one lets go.
 *
 *   br_asset_group *g = br_asset_group_create(e, "level1");
 *   br_asset_group_add(g, "sprites/atlas.json");
 *   br_asset_group_add_manifest_bundle(g, "music");
 *   br_asset_group_load_async(g, on_progress, on_done, state);   (or br_asset_group_load(g))
 *   ...
 *   br_asset_group_destroy(g);                                     (unloads, frees)
 *
 * Callbacks run from br_engine_step (system assets.update); destroying or unloading a group
 * stops its callbacks at once, also from inside one of them.
 */
#ifndef BR_ASSET_GROUP_H
#define BR_ASSET_GROUP_H

#include "braty/br_app.h"
#include "braty/br_assets.h"

typedef struct br_asset_group br_asset_group;

/* Called after each asset of an async group load finishes (failures count as finished). */
typedef void (*br_asset_group_progress_fn)(br_engine *e, br_asset_group *group, float progress, void *user);
/* Called once when an async group load is complete; `failed` assets could not be loaded. */
typedef void (*br_asset_group_done_fn)(br_engine *e, br_asset_group *group, uint32_t failed, void *user);

/* Creates an empty group (name copied, shown by br_assets_report). Freed by _destroy or at shutdown. */
br_asset_group *br_asset_group_create(br_engine *e, const char *name);
/* Unloads the group's assets and frees it. NULL is ignored. */
void br_asset_group_destroy(br_asset_group *g);
/* Adds an asset loaded by _load / _load_async (path or alias; duplicates ignored). False if unresolvable. */
bool br_asset_group_add(br_asset_group *g, const char *path);
/* Adds an asset that is only loaded on first br_asset_group_get (and released with the group). */
bool br_asset_group_add_lazy(br_asset_group *g, const char *path);
/* Adds every asset of a manifest bundle (br_assets_load_manifest). False if the bundle is unknown. */
bool br_asset_group_add_manifest_bundle(br_asset_group *g, const char *bundle);
/* Loads every non-lazy asset now (one reference each). True if all of them loaded. */
bool br_asset_group_load(br_asset_group *g);
/* Starts loading in the background; callbacks (may be NULL) run from later engine steps.
 * In deterministic mode assets load one per frame, in the order they were added. */
void br_asset_group_load_async(br_asset_group *g, br_asset_group_progress_fn on_progress,
                               br_asset_group_done_fn on_done, void *user);
/* Drops every reference the group holds (loads in flight are cancelled when nobody else wants them). */
void br_asset_group_unload(br_asset_group *g);
/* Finished share of the non-lazy assets, 0..1 (1 for an empty group). */
float br_asset_group_progress(const br_asset_group *g);
/* True when every non-lazy asset finished loading (failed ones included) and no load is running. */
bool br_asset_group_is_loaded(const br_asset_group *g);
/* Assets that failed in the last load. */
uint32_t br_asset_group_failed(const br_asset_group *g);
/* The asset (loading a lazy one now, the group keeps the reference). NULL if not in the group or failed. */
void *br_asset_group_get(br_asset_group *g, const char *path);
/* The name given at creation. */
const char *br_asset_group_name(const br_asset_group *g);
/* Number of assets listed in the group. */
uint32_t br_asset_group_count(const br_asset_group *g);
/* Resolved path of asset i, NULL if out of range. */
const char *br_asset_group_path(const br_asset_group *g, uint32_t i);

#endif
