/* br_assets_internal.h - state of the assets module, shared by its .c files. Engine-private. */
#ifndef BR_ASSETS_INTERNAL_H
#define BR_ASSETS_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_assets.h"

#define BR_ASSET_MAX_CANDIDATES 8   /* loaders tried for one file (same extension) */
#define BR_ASSET_MAX_WORKERS 4

/* A registered loader. Heap-allocated once and never moved, so jobs may point at it. */
typedef struct br_loader_entry {
    char *type_name;
    char *extensions;
    void *(*load)(br_engine *e, const char *path, const void *bytes, size_t size);
    void (*unload)(br_engine *e, void *asset);
    bool (*test)(const char *path, const void *bytes, size_t size);
    void *(*decode)(br_engine *e, const char *path, const void *bytes, size_t size);
    void *(*finish)(br_engine *e, const char *path, void *decoded);
    size_t (*byte_size)(br_engine *e, const void *asset);
} br_loader_entry;

/* One cache entry, keyed by resolved path. */
typedef struct br_asset_entry {
    const br_loader_entry *loader;  /* NULL while loading */
    void    *asset;
    uint32_t refs;
    size_t   file_size;
    float    resolution;            /* from the options or the file name */
    bool     loading;               /* a job for this path is queued, running or waiting to finish */
    bool     background;            /* prefetch: keep it when it finishes with zero references */
} br_asset_entry;

/* Work for a worker thread: read the file, pick a loader by test, decode. */
typedef struct br_asset_job {
    char                  *path;
    const br_loader_entry *candidates[BR_ASSET_MAX_CANDIDATES];
    uint32_t               candidate_count;
    bool                   forced;    /* options.type chose the loader: skip test */
    bool                   background;
} br_asset_job;

/* What a worker hands back to the main thread. */
typedef struct br_asset_result {
    char                  *path;
    const br_loader_entry *loader;    /* NULL = failed (already logged) */
    void                  *bytes;     /* loaders without decode: the file, for load() */
    size_t                 size;
    void                  *decoded;   /* loaders with decode */
} br_asset_result;

/* Someone waiting for an async load (user callback or bundle progress). */
typedef struct br_asset_waiter {
    char            *path;
    br_asset_done_fn fn;
    void            *user;
} br_asset_waiter;

typedef struct br_bundle_asset {
    char *path;   /* resolved */
    char *type;   /* forced loader type or NULL */
} br_bundle_asset;

typedef struct br_bundle {
    char    *name;
    br_array assets;  /* br_bundle_asset */
} br_bundle;

/* Progress of one br_assets_load_bundle call (heap, freed when done). */
typedef struct br_bundle_load {
    char                *name;
    uint32_t             total;
    uint32_t             done;
    br_asset_progress_fn fn;
    void                *user;
} br_bundle_load;

typedef struct br_assets_state {
    br_engine *engine;
    br_array   loaders;      /* br_loader_entry *; later entries win for the same extension */
    br_map     cache;        /* path -> br_asset_entry */
    br_map     aliases;      /* alias -> char * path (owned) */
    br_array   waiters;      /* br_asset_waiter */
    br_array   bundles;      /* br_bundle */
    br_array   bundle_loads; /* br_bundle_load *, freed when done (or at shutdown) */
    char      *base_path;    /* NULL = none */
    float      pref_resolution;
    char      *pref_formats; /* NULL = none */
    uint64_t   frame_budget_ns;
    uint32_t   sheet_nesting; /* > 0 while a spritesheet loads its linked packs (they list each other) */

    /* worker threads: everything below `mutex` is protected by it */
    SDL_Thread    *workers[BR_ASSET_MAX_WORKERS];
    uint32_t       worker_count;  /* 0 until the first async load */
    SDL_Mutex     *mutex;
    SDL_Condition *wake;          /* signalled when a job is queued or on quit */
    br_array       jobs;          /* br_asset_job, foreground first, background after */
    uint32_t       background_jobs;  /* how many of `jobs` (at the end) are background */
    br_array       results;       /* br_asset_result */
    bool           quit;
} br_assets_state;

/* Resolves a name (alias, cached path, pattern, base path) into out. */
bool br_assets_resolve_internal(br_assets_state *a, const char *name, char *out, size_t out_size);
/* Load / async load / unload of an already resolved path (no alias or base path applied again).
 * Loaders that load other assets (spritesheet image) and bundles use these. */
void *br_assets_load_resolved(br_engine *e, const char *path, const br_asset_options *options);
void br_assets_load_async_resolved(br_engine *e, const char *path, const br_asset_options *options,
                                   br_asset_done_fn fn, void *user);
void br_assets_unload_resolved(br_engine *e, const char *path);
/* Frees bundles and resolver strings (shutdown). */
void br_assets_bundles_free(br_assets_state *a);
/* Registers json/text/binary loaders (br_assets_loaders.c). */
void br_assets_register_basic_loaders(br_engine *e);
/* Registers the dds/ktx texture loader (br_assets_compressed.c). */
void br_assets_register_compressed_loader(br_engine *e);
/* Registers the spritesheet loader (br_spritesheet.c). */
void br_spritesheet_register_loader(br_engine *e);

#endif
