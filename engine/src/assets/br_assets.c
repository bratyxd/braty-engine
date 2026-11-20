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

/* Index of the first candidate from `first` on whose test accepts the bytes, `count` if none.
 * Thread-safe (tests are pure). */
static uint32_t br_assets_pick(const br_loader_entry *const *candidates, uint32_t count, uint32_t first, bool forced,
                               const char *path, const void *bytes, size_t size)
{
    for (uint32_t i = first; i < count; i++) {
        const br_loader_entry *l = candidates[i];
        if (forced || l->test == NULL || l->test(path, bytes, size)) {
            return i;
        }
    }
    return count;
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
    void *asset = NULL;
    e->assets->loader_depth++; /* references the loader takes belong to its asset, not to a capture */
    if (l->load != NULL) {
        asset = l->load(e, path, bytes, size);
    } else {
        void *decoded = l->decode(e, path, bytes, size);
        asset = decoded != NULL ? l->finish(e, path, decoded) : NULL;
    }
    e->assets->loader_depth--;
    return asset;
}

/* Runs the candidates from `first` on, on the main thread, until one returns an asset: a loader
 * whose test claimed the file may still fail, and then the next candidate gets the file (a .json
 * that looks like a spritesheet but is not one still loads as JSON). NULL if all fail (logged). */
static void *br_assets_try_loaders(br_engine *e, const br_loader_entry *const *candidates, uint32_t count,
                                   uint32_t first, bool forced, const char *path, const void *bytes, size_t size,
                                   const br_loader_entry **used)
{
    uint32_t i = br_assets_pick(candidates, count, first, forced, path, bytes, size);
    if (i == count && first == 0) {
        br_log_error("asset '%s': no loader accepts the file contents", path);
    }
    while (i < count) {
        void *asset = br_assets_run_loader(e, candidates[i], path, bytes, size);
        if (asset != NULL) {
            *used = candidates[i];
            return asset;
        }
        br_log_error("asset '%s': the %s loader failed", path, candidates[i]->type_name);
        i = br_assets_pick(candidates, count, i + 1, forced, path, bytes, size);
        if (i < count) {
            br_log_info("asset '%s': trying the %s loader instead", path, candidates[i]->type_name);
        }
    }
    return NULL;
}

/* Records a reference taken outside any loader while a capture is open. */
static void br_assets_capture_ref(br_assets_state *a, const char *path)
{
    if (a->capture_depth > 0 && a->loader_depth == 0) {
        char *copy = br_strdup(path);
        br_array_push(&a->captured, &copy);
    }
}

/* An unload while a capture is open cancels the latest matching record. */
static void br_assets_capture_unref(br_assets_state *a, const char *path)
{
    if (a->capture_depth == 0 || a->loader_depth > 0) {
        return;
    }
    for (uint32_t i = a->captured.count; i > 0; i--) {
        char *recorded = *(char **)br_array_at(&a->captured, i - 1);
        if (SDL_strcmp(recorded, path) == 0) {
            br_free(recorded);
            br_array_remove_ordered(&a->captured, i - 1);
            return;
        }
    }
}

/* ---- synchronous loading */

void *br_assets_load_resolved(br_engine *e, const char *path, const br_asset_options *options)
{
    br_assets_state *a = e->assets;
    br_asset_entry *entry = br_assets_entry(a, path);
    if (entry != NULL && entry->sync_loading) {
        br_log_error("asset '%s' loads itself (directly or through an asset it loads); refused", path);
        return NULL;
    }
    if (entry != NULL && !entry->loading) {
        entry->refs++;
        br_assets_capture_ref(a, path);
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
    /* The entry exists while the loader runs, marked: a loader that loads its own path (a
     * spritesheet listing itself as a linked pack) gets an error instead of a second copy. */
    bool placeholder = entry == NULL;
    if (placeholder) {
        br_asset_entry fresh;
        SDL_zero(fresh);
        fresh.loading = true;
        fresh.serial = ++a->next_serial;
        entry = br_map_set(&a->cache, path, &fresh);
        if (entry == NULL) {
            br_free(bytes);
            return NULL;
        }
    }
    entry->sync_loading = true;
    const br_loader_entry *loader = NULL;
    void *asset = br_assets_try_loaders(e, candidates, count, 0, type != NULL, path, bytes, size, &loader);
    br_free(bytes);
    /* The loader may have loaded or unloaded other assets (the map moved): look it up again. */
    entry = br_assets_entry(a, path);
    if (entry != NULL) {
        entry->sync_loading = false;
    }
    if (asset == NULL) {
        if (placeholder && entry != NULL && entry->refs == 0) {
            br_assets_free_entry(e, path); /* nobody asked for it meanwhile */
        }
        return NULL;
    }
    float resolution = br_assets_option_resolution(path, options);
    br_assets_apply_resolution(e, loader, asset, resolution);
    if (entry == NULL) {
        br_asset_entry fresh;
        SDL_zero(fresh);
        fresh.serial = ++a->next_serial;
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
    br_assets_capture_ref(a, path);
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
    br_assets_capture_unref(e->assets, path);
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
            if (!entry->loading && entry->loader != NULL && entry->loader->byte_size != NULL) {
                info->byte_size = entry->loader->byte_size(e, entry->asset);
            }
        }
        n++;
    }
    return n;
}

uint32_t br_assets_serial(br_assets_state *a, const char *path)
{
    const br_asset_entry *entry = br_assets_entry(a, path);
    return entry != NULL ? entry->serial : 0;
}

uint32_t br_assets_owners(br_engine *e, const char *name, const char **out, uint32_t max)
{
    char path[BR_ASSET_PATH_MAX];
    if (name == NULL || !br_assets_resolve_internal(e->assets, name, path, sizeof(path))) {
        return 0;
    }
    return br_asset_groups_owners(e, path, out, max);
}

void br_assets_report(br_engine *e)
{
    uint32_t count = br_assets_count(e);
    br_asset_info *list = count > 0 ? br_alloc(sizeof(br_asset_info) * count) : NULL;
    count = list != NULL ? br_assets_list(e, list, count) : 0;
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        total += list[i].byte_size;
    }
    br_log_info("assets: %u live, %u KB, %u loading", count, (unsigned)(total / 1024), br_assets_pending(e));
    for (uint32_t i = 0; i < count; i++) {
        const br_asset_info *info = &list[i];
        const char *owners[8];
        uint32_t owner_count = br_asset_groups_owners(e, info->path, owners, BR_ARRAY_COUNT(owners));
        char text[512];
        text[0] = '\0';
        for (uint32_t k = 0; k < owner_count && k < BR_ARRAY_COUNT(owners); k++) {
            SDL_strlcat(text, k > 0 ? ", " : "", sizeof(text));
            SDL_strlcat(text, owners[k], sizeof(text));
        }
        /* references nobody above accounts for: direct br_assets_load calls, loaders (a sheet's image) */
        uint32_t other = info->ref_count > owner_count ? info->ref_count - owner_count : 0;
        if (other > 0) {
            char more[32];
            SDL_snprintf(more, sizeof(more), "%s%u other", owner_count > 0 ? ", " : "", other);
            SDL_strlcat(text, more, sizeof(text));
        }
        br_log_info("  %-11s %3u ref %7u KB  %s%s  [%s]", info->type != NULL ? info->type : "?", info->ref_count,
                    (unsigned)(info->byte_size / 1024), info->path, info->loading ? " (loading)" : "", text);
    }
    br_free(list);
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
    for (uint32_t i = 0; i < job->candidate_count; i++) {
        r.candidates[i] = job->candidates[i];
    }
    r.candidate_count = job->candidate_count;
    r.forced = job->forced;
    uint32_t i = br_assets_pick(job->candidates, job->candidate_count, 0, job->forced, job->path, r.bytes, r.size);
    if (i == job->candidate_count) {
        br_log_error("asset '%s': no loader accepts the file contents", job->path);
        br_free(r.bytes);
        r.bytes = NULL;
        return r;
    }
    const br_loader_entry *l = job->candidates[i];
    r.next = i + 1;
    if (l->decode != NULL) {
        r.decoded = l->decode(e, job->path, r.bytes, r.size);
        if (r.decoded == NULL) {
            br_log_error("asset '%s': the %s loader failed", job->path, l->type_name);
            l = NULL; /* the bytes stay: the main thread tries the next candidates */
        } else {
            br_free(r.bytes);
            r.bytes = NULL;
        }
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
        br_assets_capture_ref(a, path);
    } else if (!br_assets_start_workers(a)) {
        br_assets_load_resolved(e, path, options); /* no threads: load now, still report from the next step */
    } else {
        br_asset_entry fresh;
        SDL_zero(fresh);
        fresh.loading = true;
        fresh.serial = ++a->next_serial;
        fresh.refs = 1;
        fresh.resolution = br_assets_option_resolution(path, options);
        if (br_map_set(&a->cache, path, &fresh) != NULL) {
            br_assets_enqueue(a, path, options != NULL ? options->type : NULL, false);
            br_assets_capture_ref(a, path);
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
        fresh.serial = ++a->next_serial;
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
    a->loader_depth++;
    if (l != NULL && l->decode != NULL) {
        asset = l->finish(e, r->path, r->decoded); /* always: it frees the decoded data */
    } else if (l != NULL && wanted) {
        asset = l->load(e, r->path, r->bytes, r->size);
    }
    a->loader_depth--;
    if (l != NULL && asset == NULL && wanted) {
        br_log_error("asset '%s': the %s loader failed", r->path, l->type_name);
    }
    if (asset == NULL && wanted && r->bytes != NULL) {
        /* the claimed loader failed: the next candidates get the file */
        l = NULL;
        asset = br_assets_try_loaders(e, r->candidates, r->candidate_count, r->next, r->forced, r->path, r->bytes,
                                      r->size, &l);
    }
    br_free(r->bytes);
    entry = br_assets_entry(a, r->path); /* finish/load may have changed the cache */
    if (entry == NULL || !entry->loading) {
        /* unloaded meanwhile, or loaded synchronously meanwhile: drop this copy */
        if (asset != NULL && l->unload != NULL) {
            l->unload(e, asset);
        }
    } else if (asset == NULL) {
        br_assets_free_entry(e, r->path); /* failed: forget it (groups notice by its serial); waiters get NULL */
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
    br_asset_groups_poll(e);
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
    br_asset_groups_free(e);
    for (uint32_t i = 0; i < a->captured.count; i++) {
        br_free(*(char **)br_array_at(&a->captured, i));
    }
    br_array_free(&a->captured);
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
    slot = 0;
    while (br_map_next(&a->frame_sheets, &slot, &key, &value)) {
        br_free(*(char **)value); /* empty unless a sheet outlived unload_all */
    }
    br_map_free(&a->frame_sheets);
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
    br_map_init(&a->frame_sheets, sizeof(char *));
    br_array_init(&a->waiters, sizeof(br_asset_waiter));
    br_array_init(&a->bundles, sizeof(br_bundle));
    br_array_init(&a->bundle_loads, sizeof(br_bundle_load *));
    br_array_init(&a->jobs, sizeof(br_asset_job));
    br_array_init(&a->results, sizeof(br_asset_result));
    br_array_init(&a->groups, sizeof(br_asset_group *));
    br_array_init(&a->captured, sizeof(char *));
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
