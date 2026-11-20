/* br_assets_internal.h - state of the assets module, shared by its .c files. Engine-private. */
#ifndef BR_ASSETS_INTERNAL_H
#define BR_ASSETS_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_asset_group.h"
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
    bool     sync_loading;          /* a loader for this path runs now: loading it again is a cycle */
    uint32_t serial;                /* unique per entry created: tells a new entry from an old one */
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
    void                  *bytes;     /* the file, for load() or for the next candidates (else NULL) */
    size_t                 size;
    void                  *decoded;   /* loaders with decode */
    /* the job's candidates, and the first one not tried yet: a claimed load that fails falls back */
    const br_loader_entry *candidates[BR_ASSET_MAX_CANDIDATES];
    uint32_t               candidate_count;
    uint32_t               next;
    bool                   forced;
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
    char    *owner;   /* "bundle:<name>", how br_assets_owners names it */
    br_array assets;  /* br_bundle_asset */
    bool     loaded;  /* br_assets_load_bundle holds references (until br_assets_unload_bundle) */
} br_bundle;

/* ---- asset groups (br_asset_group.c) */

typedef enum br_group_state {
    BR_GROUP_IDLE,     /* no reference held */
    BR_GROUP_LOADING,  /* reference held, the cache entry is still loading */
    BR_GROUP_LOADED,   /* reference held */
    BR_GROUP_FAILED    /* the last load failed; no reference held */
} br_group_state;

typedef struct br_group_entry {
    char          *path;    /* resolved */
    char          *type;    /* forced loader type (manifest "type") or NULL */
    bool           lazy;
    br_group_state state;
    uint32_t       serial;  /* the cache entry our reference is in (LOADING / LOADED) */
} br_group_entry;

struct br_asset_group {
    br_engine                 *engine;
    char                      *name;
    br_array                   entries;     /* br_group_entry */
    bool                       async;       /* a load_async is running */
    bool                       dead;        /* destroyed while callbacks ran: freed after them */
    uint32_t                   issue_next;  /* deterministic mode: next entry to request */
    uint32_t                   reported;    /* finished entries already reported to on_progress */
    br_asset_group_progress_fn on_progress;
    br_asset_group_done_fn     on_done;
    void                      *user;
};

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
    br_map     frame_sheets;  /* frame name -> char * path of the first loaded spritesheet with it (owned) */
    br_array   groups;        /* br_asset_group *, every live group */
    bool       polling;       /* groups are being polled: destroy defers the free */
    /* capture (br_assets_capture_begin): references taken outside loaders are recorded */
    br_array   captured;      /* char * resolved paths, one per reference */
    uint32_t   capture_depth;
    uint32_t   loader_depth;  /* > 0 while a loader runs (its own loads belong to its asset) */
    uint32_t   next_serial;   /* br_asset_entry.serial of the last entry created */

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
/* Group polling, run by assets.update after the waiters (br_asset_group.c). */
void br_asset_groups_poll(br_engine *e);
/* Destroys every group (shutdown). */
void br_asset_groups_free(br_engine *e);
/* Serial number of the cache entry of a resolved path, 0 if not cached. A failed async load
 * drops its entry with every reference in it; a later load makes a new entry with a new serial,
 * so a group can tell its own reference from another owner's taken after the failure. */
uint32_t br_assets_serial(br_assets_state *a, const char *path);
/* Owner names of a cached path (groups and loaded bundles), as br_assets_owners. */
uint32_t br_asset_groups_owners(br_engine *e, const char *path, const char **out, uint32_t max);

/* ---- capture: who took which reference (scene files use it to own what they load)
 *
 *   uint32_t mark = br_assets_capture_begin(e);
 *   ... loads ...
 *   br_assets_capture_end(e, mark, group);   (group NULL = release them)
 *
 * Every reference taken by br_assets_load* while a capture is open (not by loaders for their
 * own sub-assets) is recorded; an unload removes the latest matching record. Captures nest. */
uint32_t br_assets_capture_begin(br_engine *e);
/* References recorded since `mark`. */
uint32_t br_assets_capture_count(br_engine *e, uint32_t mark);
/* Closes a capture: the recorded references move into `owner` (it keeps one per path, extra
 * ones are dropped), or are released when owner is NULL. */
void br_assets_capture_end(br_engine *e, uint32_t mark, br_asset_group *owner);
/* Hands a reference already taken on a resolved path to a group (drops it if the group holds one). */
void br_asset_group_adopt(br_asset_group *g, const char *path);

/* Registers json/text/binary loaders (br_assets_loaders.c). */
void br_assets_register_basic_loaders(br_engine *e);
/* Registers the dds/ktx texture loader (br_assets_compressed.c). */
void br_assets_register_compressed_loader(br_engine *e);
/* Registers the spritesheet loader (br_spritesheet.c). */
void br_spritesheet_register_loader(br_engine *e);
/* Resolved path of the first loaded spritesheet with a frame called `name`, NULL if none (bare
 * frame names in scene files). */
const char *br_spritesheet_frame_owner(br_engine *e, const char *name);

#endif
