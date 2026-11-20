/* br_asset_group: named sets of assets loaded and released together, reference capture, and the
 * owner lists br_assets_report prints.
 *
 * A group holds at most one reference per entry. Async loads are polled (assets.update calls
 * br_asset_groups_poll every frame) instead of registering waiter callbacks, so unloading or
 * destroying a group while its loads are in flight leaves nothing behind that could call into it. */
#include "assets/br_assets_internal.h"

#define BR_GROUP_PATH_MAX 1024

static br_group_entry *br_group_entry_at(const br_asset_group *g, uint32_t i)
{
    return br_array_at(&g->entries, i);
}

static br_group_entry *br_group_find(const br_asset_group *g, const char *path)
{
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        if (SDL_strcmp(entry->path, path) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* True if the group holds a reference on the entry's path: it took one, and the cache entry it
 * went into is still there (a failed async load drops its entry with every reference in it). */
static bool br_group_holds(br_engine *e, const br_group_entry *entry)
{
    return (entry->state == BR_GROUP_LOADING || entry->state == BR_GROUP_LOADED) &&
           br_assets_serial(e->assets, entry->path) == entry->serial;
}

br_asset_group *br_asset_group_create(br_engine *e, const char *name)
{
    br_asset_group *g = br_alloc(sizeof(br_asset_group));
    if (g == NULL) {
        return NULL;
    }
    SDL_zerop(g);
    g->engine = e;
    g->name = br_strdup(name != NULL ? name : "group");
    br_array_init(&g->entries, sizeof(br_group_entry));
    br_array_push(&e->assets->groups, &g);
    return g;
}

static void br_group_free(br_asset_group *g)
{
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        br_free(entry->path);
        br_free(entry->type);
    }
    br_array_free(&g->entries);
    br_free(g->name);
    br_free(g);
}

void br_asset_group_destroy(br_asset_group *g)
{
    if (g == NULL || g->dead) {
        return;
    }
    br_asset_group_unload(g);
    br_assets_state *a = g->engine->assets;
    if (a->polling) {
        g->dead = true; /* still in the list being polled: freed when polling ends */
        return;
    }
    for (uint32_t i = 0; i < a->groups.count; i++) {
        if (*(br_asset_group **)br_array_at(&a->groups, i) == g) {
            br_array_remove_ordered(&a->groups, i);
            break;
        }
    }
    br_group_free(g);
}

static br_group_entry *br_group_add_resolved(br_asset_group *g, const char *path, const char *type, bool lazy)
{
    br_group_entry *found = br_group_find(g, path);
    if (found != NULL) {
        found->lazy = found->lazy && lazy; /* preload wins */
        return found;
    }
    br_group_entry entry;
    SDL_zero(entry);
    entry.path = br_strdup(path);
    entry.type = type != NULL ? br_strdup(type) : NULL;
    entry.lazy = lazy;
    entry.state = BR_GROUP_IDLE;
    return br_array_push(&g->entries, &entry);
}

static bool br_group_add_name(br_asset_group *g, const char *name, bool lazy)
{
    char path[BR_GROUP_PATH_MAX];
    if (g == NULL || name == NULL || name[0] == '\0' ||
        !br_assets_resolve_internal(g->engine->assets, name, path, sizeof(path))) {
        br_log_error("asset group '%s': cannot resolve '%s'", g != NULL ? g->name : "?", name != NULL ? name : "");
        return false;
    }
    return br_group_add_resolved(g, path, NULL, lazy) != NULL;
}

bool br_asset_group_add(br_asset_group *g, const char *path)
{
    return br_group_add_name(g, path, false);
}

bool br_asset_group_add_lazy(br_asset_group *g, const char *path)
{
    return br_group_add_name(g, path, true);
}

bool br_asset_group_add_manifest_bundle(br_asset_group *g, const char *bundle)
{
    if (g == NULL) {
        return false;
    }
    br_engine *e = g->engine;
    uint32_t count = br_assets_bundle_size(e, bundle);
    if (count == 0) {
        br_log_error("asset group '%s': unknown or empty bundle '%s'", g->name, bundle != NULL ? bundle : "");
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        br_bundle_asset *asset = NULL;
        const char *path = br_assets_bundle_path(e, bundle, i);
        /* the bundle's forced loader type comes along (manifest "type") */
        for (uint32_t b = 0; b < e->assets->bundles.count && asset == NULL; b++) {
            br_bundle *candidate = br_array_at(&e->assets->bundles, b);
            if (SDL_strcmp(candidate->name, bundle) == 0) {
                asset = br_array_at(&candidate->assets, i);
            }
        }
        br_group_add_resolved(g, path, asset != NULL ? asset->type : NULL, false);
    }
    return true;
}

/* Loads one entry synchronously (the group holds a reference after it, unless it failed). */
static void br_group_load_entry(br_asset_group *g, br_group_entry *entry)
{
    br_engine *e = g->engine;
    if (entry->state == BR_GROUP_LOADED) {
        return;
    }
    br_asset_options options;
    SDL_zero(options);
    options.type = entry->type;
    /* our async reference counts only while the entry it went into is the cached one */
    bool had_reference = entry->state == BR_GROUP_LOADING && br_assets_serial(e->assets, entry->path) == entry->serial;
    void *asset = br_assets_load_resolved(e, entry->path, &options);
    if (asset == NULL) {
        /* a failed sync load of an entry that was loading cancels nothing: our async reference
         * is still in the cache entry, if that entry survived */
        bool still = had_reference && br_assets_serial(e->assets, entry->path) == entry->serial;
        entry->state = still ? BR_GROUP_LOADING : BR_GROUP_FAILED;
        return;
    }
    if (had_reference) {
        br_assets_unload_resolved(e, entry->path); /* keep one reference, not two */
    }
    entry->serial = br_assets_serial(e->assets, entry->path);
    entry->state = BR_GROUP_LOADED;
}

bool br_asset_group_load(br_asset_group *g)
{
    if (g == NULL) {
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        if (!entry->lazy) {
            br_group_load_entry(g, entry);
            ok = ok && entry->state == BR_GROUP_LOADED;
        }
    }
    /* an async load in flight is finished by this one, not cancelled: its on_done still runs once,
     * from the next poll (callbacks always come from engine steps). Nothing is left to request. */
    g->issue_next = g->entries.count;
    return ok;
}

/* Requests one entry in the background (reference taken now). */
static void br_group_request(br_asset_group *g, br_group_entry *entry)
{
    if (entry->lazy || br_group_holds(g->engine, entry)) {
        return;
    }
    br_asset_options options;
    SDL_zero(options);
    options.type = entry->type;
    br_assets_load_async_resolved(g->engine, entry->path, &options, NULL, NULL);
    /* without worker threads the load already happened (or failed: then nothing is cached) */
    entry->serial = br_assets_serial(g->engine->assets, entry->path);
    entry->state = entry->serial != 0 ? BR_GROUP_LOADING : BR_GROUP_FAILED;
}

void br_asset_group_load_async(br_asset_group *g, br_asset_group_progress_fn on_progress,
                               br_asset_group_done_fn on_done, void *user)
{
    if (g == NULL) {
        return;
    }
    g->async = true;
    g->on_progress = on_progress;
    g->on_done = on_done;
    g->user = user;
    g->issue_next = 0;
    g->reported = 0;
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        if (entry->state == BR_GROUP_FAILED) {
            entry->state = BR_GROUP_IDLE; /* try failed ones again */
        }
    }
    if (br_app_deterministic(g->engine)) {
        return; /* one request per frame from the poll, in order: progress is reproducible */
    }
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_request(g, br_group_entry_at(g, i));
    }
    g->issue_next = g->entries.count;
}

void br_asset_group_unload(br_asset_group *g)
{
    if (g == NULL) {
        return;
    }
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        if (br_group_holds(g->engine, entry)) {
            br_assets_unload_resolved(g->engine, entry->path);
        }
        entry->state = BR_GROUP_IDLE;
    }
    g->async = false;
    g->on_progress = NULL;
    g->on_done = NULL;
}

/* Finished (loaded or failed) and total non-lazy entries. */
static void br_group_counts(const br_asset_group *g, uint32_t *finished, uint32_t *total, uint32_t *failed)
{
    *finished = 0;
    *total = 0;
    *failed = 0;
    for (uint32_t i = 0; i < g->entries.count; i++) {
        const br_group_entry *entry = br_group_entry_at(g, i);
        if (entry->lazy) {
            continue;
        }
        (*total)++;
        if (entry->state == BR_GROUP_LOADED || entry->state == BR_GROUP_FAILED) {
            (*finished)++;
        }
        if (entry->state == BR_GROUP_FAILED) {
            (*failed)++;
        }
    }
}

float br_asset_group_progress(const br_asset_group *g)
{
    uint32_t finished, total, failed;
    if (g == NULL) {
        return 0.0f;
    }
    br_group_counts(g, &finished, &total, &failed);
    return total > 0 ? (float)finished / (float)total : 1.0f;
}

bool br_asset_group_is_loaded(const br_asset_group *g)
{
    uint32_t finished, total, failed;
    if (g == NULL || (g->async && g->issue_next < g->entries.count)) {
        return false; /* deterministic async: entries not requested yet may still be LOADED from before */
    }
    br_group_counts(g, &finished, &total, &failed);
    return finished == total;
}

uint32_t br_asset_group_failed(const br_asset_group *g)
{
    uint32_t finished, total, failed;
    if (g == NULL) {
        return 0;
    }
    br_group_counts(g, &finished, &total, &failed);
    return failed;
}

void *br_asset_group_get(br_asset_group *g, const char *name)
{
    char path[BR_GROUP_PATH_MAX];
    if (g == NULL || name == NULL || !br_assets_resolve_internal(g->engine->assets, name, path, sizeof(path))) {
        return NULL;
    }
    br_group_entry *entry = br_group_find(g, path);
    if (entry == NULL) {
        return NULL;
    }
    if (entry->state != BR_GROUP_LOADED) {
        br_group_load_entry(g, entry);
    }
    return entry->state == BR_GROUP_LOADED ? br_assets_get(g->engine, path) : NULL;
}

const char *br_asset_group_name(const br_asset_group *g)
{
    return g != NULL ? g->name : "";
}

uint32_t br_asset_group_count(const br_asset_group *g)
{
    return g != NULL ? g->entries.count : 0;
}

const char *br_asset_group_path(const br_asset_group *g, uint32_t i)
{
    const br_group_entry *entry = g != NULL ? br_array_at(&g->entries, i) : NULL;
    return entry != NULL ? entry->path : NULL;
}

void br_asset_group_adopt(br_asset_group *g, const char *path)
{
    br_group_entry *entry = br_group_add_resolved(g, path, NULL, false);
    if (entry == NULL) {
        br_assets_unload_resolved(g->engine, path);
        return;
    }
    if (br_group_holds(g->engine, entry)) {
        br_assets_unload_resolved(g->engine, path); /* the group already holds its one reference */
        return;
    }
    entry->serial = br_assets_serial(g->engine->assets, path);
    entry->state = BR_GROUP_LOADED;
}

/* ---- polling (assets.update, every frame) */

/* Updates entry states from the cache; true if the async load is complete. */
static bool br_group_update(br_asset_group *g)
{
    br_engine *e = g->engine;
    if (g->issue_next < g->entries.count) {
        /* deterministic mode: one request per frame */
        br_group_request(g, br_group_entry_at(g, g->issue_next));
        g->issue_next++;
    }
    for (uint32_t i = 0; i < g->entries.count; i++) {
        br_group_entry *entry = br_group_entry_at(g, i);
        if (entry->state != BR_GROUP_LOADING) {
            continue;
        }
        /* by serial, not by count: after a failure dropped our entry, another owner may have
         * loaded the path again, and that reference is not ours */
        if (br_assets_serial(e->assets, entry->path) != entry->serial) {
            entry->state = BR_GROUP_FAILED; /* the failed load was dropped from the cache */
        } else if (br_assets_get(e, entry->path) != NULL) {
            entry->state = BR_GROUP_LOADED;
        }
    }
    return g->issue_next >= g->entries.count;
}

void br_asset_groups_poll(br_engine *e)
{
    br_assets_state *a = e->assets;
    a->polling = true;
    /* groups created by callbacks are appended and polled in the same pass: harmless */
    for (uint32_t i = 0; i < a->groups.count; i++) {
        br_asset_group *g = *(br_asset_group **)br_array_at(&a->groups, i);
        if (g->dead || !g->async) {
            continue;
        }
        bool issued = br_group_update(g);
        uint32_t finished, total, failed;
        br_group_counts(g, &finished, &total, &failed);
        if (finished != g->reported && g->on_progress != NULL) {
            g->reported = finished;
            g->on_progress(e, g, total > 0 ? (float)finished / (float)total : 1.0f, g->user);
        }
        g->reported = finished;
        if (!g->dead && g->async && issued && finished == total) {
            g->async = false;
            if (g->on_done != NULL) {
                g->on_done(e, g, failed, g->user);
            }
        }
    }
    a->polling = false;
    for (uint32_t i = a->groups.count; i > 0; i--) {
        br_asset_group *g = *(br_asset_group **)br_array_at(&a->groups, i - 1);
        if (g->dead) {
            br_array_remove_ordered(&a->groups, i - 1);
            br_group_free(g);
        }
    }
}

void br_asset_groups_free(br_engine *e)
{
    br_assets_state *a = e->assets;
    while (a->groups.count > 0) {
        br_asset_group_destroy(*(br_asset_group **)br_array_at(&a->groups, a->groups.count - 1));
    }
    br_array_free(&a->groups);
}

uint32_t br_asset_groups_owners(br_engine *e, const char *path, const char **out, uint32_t max)
{
    br_assets_state *a = e->assets;
    uint32_t n = 0;
    // ponytail: linear scan of every group and bundle per asset; a debug/report path only.
    for (uint32_t i = 0; i < a->groups.count; i++) {
        br_asset_group *g = *(br_asset_group **)br_array_at(&a->groups, i);
        br_group_entry *entry = g->dead ? NULL : br_group_find(g, path);
        if (entry != NULL && br_group_holds(g->engine, entry)) {
            if (out != NULL && n < max) {
                out[n] = g->name;
            }
            n++;
        }
    }
    for (uint32_t i = 0; i < a->bundles.count; i++) {
        br_bundle *b = br_array_at(&a->bundles, i);
        if (!b->loaded) {
            continue;
        }
        for (uint32_t j = 0; j < b->assets.count; j++) {
            br_bundle_asset *asset = br_array_at(&b->assets, j);
            if (SDL_strcmp(asset->path, path) == 0) {
                if (out != NULL && n < max) {
                    out[n] = b->owner;
                }
                n++;
                break;
            }
        }
    }
    return n;
}

/* ---- capture */

uint32_t br_assets_capture_begin(br_engine *e)
{
    e->assets->capture_depth++;
    return e->assets->captured.count;
}

uint32_t br_assets_capture_count(br_engine *e, uint32_t mark)
{
    uint32_t count = e->assets->captured.count;
    return count > mark ? count - mark : 0;
}

void br_assets_capture_end(br_engine *e, uint32_t mark, br_asset_group *owner)
{
    br_assets_state *a = e->assets;
    if (a->capture_depth > 0) {
        a->capture_depth--;
    }
    /* take the records off the list first: adopting may unload, which looks at the list */
    uint32_t count = br_assets_capture_count(e, mark);
    char **paths = count > 0 ? br_alloc(sizeof(char *) * count) : NULL;
    for (uint32_t i = 0; i < count && paths != NULL; i++) {
        paths[i] = *(char **)br_array_at(&a->captured, mark + i);
    }
    if (count > 0 && paths == NULL) {
        return; /* out of memory: the references stay (logged by br_alloc) */
    }
    while (a->captured.count > mark) {
        br_array_pop(&a->captured);
    }
    uint32_t depth = a->capture_depth;
    a->capture_depth = 0; /* adopt/unload below are not recorded by an outer capture */
    for (uint32_t i = 0; i < count; i++) {
        if (owner != NULL) {
            br_asset_group_adopt(owner, paths[i]);
        } else {
            br_assets_unload_resolved(e, paths[i]);
        }
        br_free(paths[i]);
    }
    a->capture_depth = depth;
    br_free(paths);
}
