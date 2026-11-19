/* br_assets: loader registry (by extension), reference-counted cache keyed by resolved path,
 * aliases, and background loading.
 *
 * Threading (kept deliberately simple): one mutex protects a job queue and a result queue.
 * Worker threads take a job, read the file and run the loader's decode (pure parsing) without
 * holding the lock, then push a result. Everything else - the cache, waiters, loaders' load
 * and finish, GPU uploads - happens on the main thread in the "assets.update" system. */
#include "assets/br_assets_internal.h"
#include "braty/br_texture.h"

#define BR_ASSET_PATH_MAX 1024

/* ---- loader registry */

void br_assets_register_loader(br_engine *e, const br_asset_loader_desc *desc)
{
    if (desc == NULL || desc->type_name == NULL || desc->extensions == NULL) {
        br_log_error("br_assets_register_loader: type_name and extensions are required");
        return;
    }
    if ((desc->decode == NULL) != (desc->finish == NULL)) {
        br_log_error("br_assets_register_loader '%s': set both decode and finish, or neither", desc->type_name);
        return;
    }
    if (desc->load == NULL && desc->decode == NULL) {
        br_log_error("br_assets_register_loader '%s': load (or decode + finish) is required", desc->type_name);
        return;
    }
    br_loader_entry *l = br_alloc(sizeof(br_loader_entry));
    if (l == NULL) {
        return;
    }
    l->type_name = br_strdup(desc->type_name);
    l->extensions = br_strdup(desc->extensions);
    l->load = desc->load;
    l->unload = desc->unload;
    l->test = desc->test;
    l->decode = desc->decode;
    l->finish = desc->finish;
    l->byte_size = desc->byte_size;
    br_array_push(&e->assets->loaders, &l);
}

/* True if `ext` (any case) is one of the comma-separated lower-case `list`. */
static bool br_extension_in_list(const char *list, const char *ext)
{
    size_t ext_len = SDL_strlen(ext);
    const char *p = list;
    while (*p != 0) {
        const char *end = SDL_strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : SDL_strlen(p);
        if (len == ext_len && SDL_strncasecmp(p, ext, len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return false;
}

/* Loaders that may handle `path`, newest first: the ones of type `type` if given, else the
 * ones listing the file's extension. */
static uint32_t br_assets_candidates(br_assets_state *a, const char *path, const char *type,
                                     const br_loader_entry **out)
{
    const char *ext = br_path_extension(path);
    uint32_t n = 0;
    for (uint32_t i = a->loaders.count; i > 0 && n < BR_ASSET_MAX_CANDIDATES; i--) {
        const br_loader_entry *l = *(br_loader_entry **)br_array_at(&a->loaders, i - 1);
        bool match = type != NULL ? br_str_equal(l->type_name, type) : br_extension_in_list(l->extensions, ext);
        if (match) {
            out[n] = l;
            n++;
        }
    }
    return n;
}

static void br_assets_log_no_loader(const char *path, const char *type)
{
    if (type != NULL) {
        br_log_error("asset '%s': no loader of type '%s'", path, type);
    } else {
        br_log_error("asset '%s': no loader for extension '%s'", path, br_path_extension(path));
    }
}

/* The first candidate whose test accepts the bytes. Thread-safe (tests are pure). */
static const br_loader_entry *br_assets_pick(const br_loader_entry *const *candidates, uint32_t count, bool forced,
                                             const char *path, const void *bytes, size_t size)
{
    for (uint32_t i = 0; i < count; i++) {
        const br_loader_entry *l = candidates[i];
        if (forced || l->test == NULL || l->test(path, bytes, size)) {
            return l;
        }
    }
    br_log_error("asset '%s': no loader accepts the file contents", path);
    return NULL;
}

/* ---- cache helpers (main thread) */

static br_asset_entry *br_assets_entry(br_assets_state *a, const char *path)
{
    return br_map_get(&a->cache, path);
}

static float br_assets_option_resolution(const char *path, const br_asset_options *options)
{
    if (options != NULL && options->resolution > 0.0f) {
        return options->resolution;
    }
    return br_assets_resolution_of(path);
}

/* What every freshly loaded asset gets, whichever way it was loaded. */
static void br_assets_apply_resolution(br_engine *e, const br_loader_entry *l, void *asset, float resolution)
{
    // ponytail: the core knows that "texture" loaders return br_texture *; if a texture loader ever
    // returns something else, move this into the loaders (they would read the resolution themselves).
    if (resolution != 1.0f && br_str_equal(l->type_name, "texture")) {
        br_texture_set_resolution(e, *(br_texture *)asset, resolution);
    }
}

/* Removes an entry and frees its asset (a loading entry is just forgotten: its result is
 * dropped when it arrives). */
static void br_assets_free_entry(br_engine *e, const char *path)
{
    br_assets_state *a = e->assets;
    br_asset_entry *found = br_assets_entry(a, path);
    if (found == NULL) {
        return;
    }
    br_asset_entry entry = *found;
    char key[BR_ASSET_PATH_MAX];
    br_str_copy(key, sizeof(key), path); /* path may point at the key we are removing */
    br_map_remove(&a->cache, key);
    if (!entry.loading && entry.loader != NULL && entry.loader->unload != NULL) {
        entry.loader->unload(e, entry.asset);
    }
}

/* Runs a loader on the main thread. */
static void *br_assets_run_loader(br_engine *e, const br_loader_entry *l, const char *path, const void *bytes,
                                  size_t size)
{
    if (l->load != NULL) {
        return l->load(e, path, bytes, size);
    }
    void *decoded = l->decode(e, path, bytes, size);
    if (decoded == NULL) {
        return NULL;
    }
    return l->finish(e, path, decoded);
}

/* ---- synchronous loading */

void *br_assets_load_resolved(br_engine *e, const char *path, const br_asset_options *options)
{
    br_assets_state *a = e->assets;
    br_asset_entry *entry = br_assets_entry(a, path);
    if (entry != NULL && !entry->loading) {
        entry->refs++;
        return entry->asset;
    }
    /* Not cached, or still loading in the background: load now (the async result is dropped). */
    const char *type = options != NULL ? options->type : NULL;
    const br_loader_entry *candidates[BR_ASSET_MAX_CANDIDATES];
    uint32_t count = br_assets_candidates(a, path, type, candidates);
    if (count == 0) {
        br_assets_log_no_loader(path, type);
        return NULL;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (!br_file_read_all(e, path, &bytes, &size)) {
        br_log_error("asset '%s': cannot read the file", path);
        return NULL;
    }
    const br_loader_entry *loader = br_assets_pick(candidates, count, type != NULL, path, bytes, size);
    void *asset = loader != NULL ? br_assets_run_loader(e, loader, path, bytes, size) : NULL;
    br_free(bytes);
    if (asset == NULL) {
        if (loader != NULL) {
            br_log_error("asset '%s': the %s loader failed", path, loader->type_name);
        }
        return NULL;
    }
    float resolution = br_assets_option_resolution(path, options);
    br_assets_apply_resolution(e, loader, asset, resolution);
    /* The loader may have loaded other assets (the map moved): look the entry up again. */
    entry = br_assets_entry(a, path);
    if (entry == NULL) {
        br_asset_entry fresh;
        SDL_zero(fresh);
        entry = br_map_set(&a->cache, path, &fresh);
        if (entry == NULL) {
            if (loader->unload != NULL) {
                loader->unload(e, asset);
            }
            return NULL;
        }
    }
    entry->loader = loader;
    entry->asset = asset;
    entry->file_size = size;
    entry->resolution = resolution;
    entry->loading = false;
    entry->refs++;
    return asset;
}

void *br_assets_load_with(br_engine *e, const char *name, const br_asset_options *options)
{
    if (name == NULL || name[0] == 0) {
        br_log_error("br_assets_load: empty path");
        return NULL;
    }
    char path[BR_ASSET_PATH_MAX];
    if (!br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return NULL;
    }
    return br_assets_load_resolved(e, path, options);
}

void *br_assets_load(br_engine *e, const char *path)
{
    return br_assets_load_with(e, path, NULL);
}

void *br_assets_get(br_engine *e, const char *name)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return NULL;
    }
    br_asset_entry *entry = br_assets_entry(e->assets, path);
    return entry != NULL && !entry->loading ? entry->asset : NULL;
}

const char *br_assets_type(br_engine *e, const char *name)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return NULL;
    }
    br_asset_entry *entry = br_assets_entry(e->assets, path);
    return entry != NULL && entry->loader != NULL ? entry->loader->type_name : NULL;
}

uint32_t br_assets_ref_count(br_engine *e, const char *name)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return 0;
    }
    br_asset_entry *entry = br_assets_entry(e->assets, path);
    return entry != NULL ? entry->refs : 0;
}

void br_assets_unload_resolved(br_engine *e, const char *path)
{
    br_asset_entry *entry = br_assets_entry(e->assets, path);
    if (entry == NULL) {
        return;
    }
    if (entry->refs > 1) {
        entry->refs--;
        return;
    }
    br_assets_free_entry(e, path);
}

void br_assets_unload(br_engine *e, const char *name)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return;
    }
    br_assets_unload_resolved(e, path);
}

void br_assets_unload_all(br_engine *e)
{
    br_assets_state *a = e->assets;
    /* Restart the walk after every free: an unload may unload other assets (spritesheet image). */
    while (a->cache.count > 0) {
        uint32_t slot = 0;
        const char *key = NULL;
        void *value = NULL;
        if (!br_map_next(&a->cache, &slot, &key, &value)) {
            break;
        }
        char path[BR_ASSET_PATH_MAX];
        br_str_copy(path, sizeof(path), key);
        br_assets_free_entry(e, path);
    }
}

void br_assets_add_alias(br_engine *e, const char *alias, const char *path)
{
    br_assets_state *a = e->assets;
    if (alias == NULL || path == NULL) {
        return;
    }
    char **old = br_map_get(&a->aliases, alias);
    if (old != NULL) {
        br_free(*old);
    }
    char *copy = br_strdup(path);
    br_map_set(&a->aliases, alias, &copy);
}

uint32_t br_assets_count(br_engine *e)
{
    return e->assets->cache.count;
}

uint32_t br_assets_list(br_engine *e, br_asset_info *out, uint32_t max)
{
    br_assets_state *a = e->assets;
    uint32_t slot = 0;
    uint32_t n = 0;
    const char *key = NULL;
    void *value = NULL;
    while (br_map_next(&a->cache, &slot, &key, &value)) {
        if (out != NULL && n < max) {
            br_asset_entry *entry = value;
            br_asset_info *info = &out[n];
            info->path = key;
            info->type = entry->loader != NULL ? entry->loader->type_name : NULL;
            info->ref_count = entry->refs;
            info->loading = entry->loading;
            info->byte_size = entry->file_size;
            if (!entry->loading && entry->loader->byte_size != NULL) {
                info->byte_size = entry->loader->byte_size(e, entry->asset);
            }
        }
        n++;
    }
    return n;
}

/* ---- background loading: worker side (no engine state touched except the asset root) */

static br_asset_result br_assets_run_job(br_engine *e, const br_asset_job *job)
{
    br_asset_result r;
    SDL_zero(r);
    r.path = job->path;
    if (job->candidate_count == 0) {
        br_log_error("asset '%s': no loader for it", job->path);
        return r;
    }
    if (!br_file_read_all(e, job->path, &r.bytes, &r.size)) {
        br_log_error("asset '%s': cannot read the file", job->path);
        return r;
    }
    const br_loader_entry *l = br_assets_pick(job->candidates, job->candidate_count, job->forced, job->path,
                                              r.bytes, r.size);
    if (l != NULL && l->decode != NULL) {
        r.decoded = l->decode(e, job->path, r.bytes, r.size);
        br_free(r.bytes);
        r.bytes = NULL;
        if (r.decoded == NULL) {
            br_log_error("asset '%s': the %s loader failed", job->path, l->type_name);
            l = NULL;
        }
    }
    if (l == NULL) {
        br_free(r.bytes);
        r.bytes = NULL;
    }
    r.loader = l;
    return r;
}

static int SDLCALL br_assets_worker(void *data)
{
    br_assets_state *a = data;
    SDL_LockMutex(a->mutex);
    while (!a->quit) {
        if (a->jobs.count == 0) {
            SDL_WaitCondition(a->wake, a->mutex);
            continue;
        }
        br_asset_job job = *(br_asset_job *)br_array_at(&a->jobs, 0);
        br_array_remove_ordered(&a->jobs, 0);
        if (job.background) {
            a->background_jobs--;
        }
        SDL_UnlockMutex(a->mutex);
        br_asset_result result = br_assets_run_job(a->engine, &job);
        SDL_LockMutex(a->mutex);
        br_array_push(&a->results, &result);
    }
    SDL_UnlockMutex(a->mutex);
    return 0;
}

/* ---- background loading: main-thread side */

static bool br_assets_start_workers(br_assets_state *a)
{
    if (a->worker_count > 0) {
        return true;
    }
    if (br_app_deterministic(a->engine)) {
        return false; /* --deterministic: every load completes on the main thread, in order */
    }
    if (a->mutex == NULL || a->wake == NULL) {
        return false;
    }
    int cores = SDL_GetNumLogicalCPUCores();
    uint32_t wanted = cores > 2 ? (uint32_t)(cores - 1) : 1;
    if (wanted > BR_ASSET_MAX_WORKERS) {
        wanted = BR_ASSET_MAX_WORKERS;
    }
    for (uint32_t i = 0; i < wanted; i++) {
        SDL_Thread *t = SDL_CreateThread(br_assets_worker, "br_assets", a);
        if (t == NULL) {
            br_log_error("assets: cannot create a worker thread: %s", SDL_GetError());
            break;
        }
        a->workers[a->worker_count] = t;
        a->worker_count++;
    }
    return a->worker_count > 0;
}

static void br_assets_enqueue(br_assets_state *a, const char *path, const char *type, bool background)
{
    br_asset_job job;
    SDL_zero(job);
    job.path = br_strdup(path);
    job.candidate_count = br_assets_candidates(a, path, type, job.candidates);
    job.forced = type != NULL;
    job.background = background;
    SDL_LockMutex(a->mutex);
    if (background) {
        br_array_push(&a->jobs, &job);
        a->background_jobs++;
    } else {
        br_array_insert(&a->jobs, a->jobs.count - a->background_jobs, &job);
    }
    SDL_SignalCondition(a->wake);
    SDL_UnlockMutex(a->mutex);
}

static void br_assets_add_waiter(br_assets_state *a, const char *path, br_asset_done_fn fn, void *user)
{
    if (fn == NULL) {
        return;
    }
    br_asset_waiter w;
    w.path = br_strdup(path);
    w.fn = fn;
    w.user = user;
    br_array_push(&a->waiters, &w);
}

void br_assets_load_async_resolved(br_engine *e, const char *path, const br_asset_options *options,
                                   br_asset_done_fn fn, void *user)
{
    br_assets_state *a = e->assets;
    br_asset_entry *entry = br_assets_entry(a, path);
    if (entry != NULL) {
        entry->refs++;
    } else if (!br_assets_start_workers(a)) {
        br_assets_load_resolved(e, path, options); /* no threads: load now, still report from the next step */
    } else {
        br_asset_entry fresh;
        SDL_zero(fresh);
        fresh.loading = true;
        fresh.refs = 1;
        fresh.resolution = br_assets_option_resolution(path, options);
        if (br_map_set(&a->cache, path, &fresh) != NULL) {
            br_assets_enqueue(a, path, options != NULL ? options->type : NULL, false);
        }
    }
    br_assets_add_waiter(a, path, fn, user);
}

void br_assets_load_async(br_engine *e, const char *name, const br_asset_options *options, br_asset_done_fn fn,
                          void *user)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || name[0] == 0 || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        br_log_error("br_assets_load_async: bad path");
        br_assets_add_waiter(e->assets, "", fn, user); /* reports failure from the next step */
        return;
    }
    br_assets_load_async_resolved(e, path, options, fn, user);
}

void br_assets_background_load(br_engine *e, const char *const *names, uint32_t count)
{
    br_assets_state *a = e->assets;
    if (!br_assets_start_workers(a)) {
        if (!br_app_deterministic(e)) {
            br_log_warn("br_assets_background_load: no worker threads, nothing prefetched");
        }
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        char path[BR_ASSET_PATH_MAX];
        if (names[i] == NULL || !br_assets_resolve_internal(a, names[i], path, sizeof(path))) {
            continue;
        }
        if (br_assets_entry(a, path) != NULL) {
            continue;
        }
        br_asset_entry fresh;
        SDL_zero(fresh);
        fresh.loading = true;
        fresh.background = true;
        fresh.resolution = br_assets_resolution_of(path);
        if (br_map_set(&a->cache, path, &fresh) != NULL) {
            br_assets_enqueue(a, path, NULL, true);
        }
    }
}

uint32_t br_assets_pending(br_engine *e)
{
    uint32_t slot = 0;
    uint32_t n = 0;
    const char *key = NULL;
    void *value = NULL;
    while (br_map_next(&e->assets->cache, &slot, &key, &value)) {
        br_asset_entry *entry = value;
        if (entry->loading) {
            n++;
        }
    }
    return n;
}

void br_assets_set_frame_budget(br_engine *e, float milliseconds)
{
    e->assets->frame_budget_ns = milliseconds > 0.0f ? (uint64_t)(milliseconds * 1000000.0f) : 0;
}

/* Turns a worker result into a cache entry (or drops it if nobody wants it any more). */
static void br_assets_finish_result(br_engine *e, br_asset_result *r)
{
    br_assets_state *a = e->assets;
    br_asset_entry *entry = br_assets_entry(a, r->path);
    bool wanted = entry != NULL && entry->loading;
    void *asset = NULL;
    const br_loader_entry *l = r->loader;
    if (l != NULL && l->decode != NULL) {
        asset = l->finish(e, r->path, r->decoded); /* always: it frees the decoded data */
    } else if (l != NULL && wanted) {
        asset = l->load(e, r->path, r->bytes, r->size);
    }
    br_free(r->bytes);
    if (l != NULL && asset == NULL && wanted) {
        br_log_error("asset '%s': the %s loader failed", r->path, l->type_name);
    }
    entry = br_assets_entry(a, r->path); /* finish/load may have changed the cache */
    if (entry == NULL || !entry->loading) {
        /* unloaded meanwhile, or loaded synchronously meanwhile: drop this copy */
        if (asset != NULL && l->unload != NULL) {
            l->unload(e, asset);
        }
    } else if (asset == NULL) {
        br_assets_free_entry(e, r->path); /* failed: forget it, waiters get NULL */
    } else {
        entry->loader = l;
        entry->asset = asset;
        entry->file_size = r->size;
        entry->loading = false;
        br_assets_apply_resolution(e, l, asset, entry->resolution);
    }
    br_free(r->path);
}

/* Calls every waiter whose asset is no longer loading. Callbacks may load or unload. */
static void br_assets_fire_waiters(br_engine *e)
{
    br_assets_state *a = e->assets;
    uint32_t i = 0;
    while (i < a->waiters.count) {
        br_asset_waiter w = *(br_asset_waiter *)br_array_at(&a->waiters, i);
        br_asset_entry *entry = br_assets_entry(a, w.path);
        if (entry != NULL && entry->loading) {
            i++;
            continue;
        }
        br_array_remove_ordered(&a->waiters, i);
        w.fn(e, w.path, entry != NULL ? entry->asset : NULL, w.user);
        br_free(w.path);
    }
}

/* Pops one finished result, false if there is none. */
static bool br_assets_pop_result(br_assets_state *a, br_asset_result *out)
{
    bool have = false;
    SDL_LockMutex(a->mutex);
    if (a->results.count > 0) {
        *out = *(br_asset_result *)br_array_at(&a->results, 0);
        br_array_remove_ordered(&a->results, 0);
        have = true;
    }
    SDL_UnlockMutex(a->mutex);
    return have;
}

/* System: finishes worker results within the frame budget, then reports to waiters. */
static void br_assets_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_assets_state *a = e->assets;
    if (a->worker_count > 0) {
        uint64_t start = SDL_GetTicksNS();
        br_asset_result r;
        while (br_assets_pop_result(a, &r)) {
            br_assets_finish_result(e, &r);
            if (SDL_GetTicksNS() - start >= a->frame_budget_ns) {
                break;
            }
        }
    }
    br_assets_fire_waiters(e);
}

/* ---- lifetime */

static void br_assets_stop_workers(br_engine *e)
{
    br_assets_state *a = e->assets;
    if (a->worker_count == 0) {
        return;
    }
    SDL_LockMutex(a->mutex);
    a->quit = true;
    SDL_BroadcastCondition(a->wake);
    SDL_UnlockMutex(a->mutex);
    for (uint32_t i = 0; i < a->worker_count; i++) {
        SDL_WaitThread(a->workers[i], NULL);
    }
    a->worker_count = 0;
    /* Threads are gone: no locking needed below. Jobs that never ran are dropped; finished
     * results are cached (finish frees their decoded data) and freed by unload_all. */
    for (uint32_t i = 0; i < a->jobs.count; i++) {
        br_asset_job *job = br_array_at(&a->jobs, i);
        br_free(job->path);
    }
    br_array_clear(&a->jobs);
    for (uint32_t i = 0; i < a->results.count; i++) {
        br_asset_result r = *(br_asset_result *)br_array_at(&a->results, i);
        br_assets_finish_result(e, &r);
    }
    br_array_clear(&a->results);
}

static void br_assets_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_assets_state *a = e->assets;
    br_assets_stop_workers(e);
    for (uint32_t i = 0; i < a->waiters.count; i++) {
        br_asset_waiter *w = br_array_at(&a->waiters, i);
        br_free(w->path);
    }
    br_assets_unload_all(e);
    for (uint32_t i = 0; i < a->loaders.count; i++) {
        br_loader_entry *l = *(br_loader_entry **)br_array_at(&a->loaders, i);
        br_free(l->type_name);
        br_free(l->extensions);
        br_free(l);
    }
    uint32_t slot = 0;
    const char *key = NULL;
    void *value = NULL;
    while (br_map_next(&a->aliases, &slot, &key, &value)) {
        br_free(*(char **)value);
    }
    br_assets_bundles_free(a);
    br_array_free(&a->loaders);
    br_array_free(&a->waiters);
    br_array_free(&a->jobs);
    br_array_free(&a->results);
    br_map_free(&a->cache);
    br_map_free(&a->aliases);
    SDL_DestroyCondition(a->wake);
    SDL_DestroyMutex(a->mutex);
    br_free(a);
    e->assets = NULL;
}

void br_assets_register(br_engine *e)
{
    br_assets_state *a = br_alloc(sizeof(br_assets_state));
    if (a == NULL) {
        return;
    }
    e->assets = a;
    a->engine = e;
    a->pref_resolution = 1.0f;
    a->frame_budget_ns = 4000000;
    br_array_init(&a->loaders, sizeof(br_loader_entry *));
    br_map_init(&a->cache, sizeof(br_asset_entry));
    br_map_init(&a->aliases, sizeof(char *));
    br_array_init(&a->waiters, sizeof(br_asset_waiter));
    br_array_init(&a->bundles, sizeof(br_bundle));
    br_array_init(&a->bundle_loads, sizeof(br_bundle_load *));
    br_array_init(&a->jobs, sizeof(br_asset_job));
    br_array_init(&a->results, sizeof(br_asset_result));
    a->mutex = SDL_CreateMutex();
    a->wake = SDL_CreateCondition();
    if (a->mutex == NULL || a->wake == NULL) {
        br_log_error("assets: cannot create the job queue lock (%s); async loads run synchronously", SDL_GetError());
    }
    br_engine_on_shutdown(e, br_assets_shutdown, NULL);
    br_system_add(e, "assets.update", BR_PHASE_INPUT, 1000, br_assets_update, NULL);
    br_assets_register_basic_loaders(e);
    br_assets_register_compressed_loader(e);
    br_spritesheet_register_loader(e);
}
