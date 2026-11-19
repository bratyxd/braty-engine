/* br_filters_internal.h - state of the filters module (filters, masks, cache, blend, extract). */
#ifndef BR_FILTERS_INTERNAL_H
#define BR_FILTERS_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_extract.h"
#include "braty/br_filter.h"

/* One pooled render texture. */
typedef struct br_pool_entry {
    br_texture texture;
    uint32_t   width;         /* pixels (a bucket size) */
    uint32_t   height;
    bool       in_use;
    uint64_t   last_used;     /* fs->frame when last acquired */
} br_pool_entry;

/* Per-node data: the filter list (what br_node_set_filters points at) and the cache. */
typedef struct br_fx_record {
    br_node    node;
    br_filter  filters[BR_FILTER_MAX_PER_NODE];
    uint32_t   count;
    float      resolution;
    bool       use_area;
    br_rect    area;          /* local */
    /* cache_as_texture */
    br_texture cache;
    bool       cache_valid;
    br_rect    cache_area;    /* world rect the cached content covers */
    br_mat3    cache_world;   /* node world transform when cached */
    float      cache_scale;   /* pixels per world unit when cached */
    float      cache_u;       /* uv extent of the content in the cache texture */
    float      cache_v;
} br_fx_record;

typedef enum br_fx_pass_kind {
    BR_FX_PASS_SHADER,  /* full-texture triangle: input (+ extras) -> output */
    BR_FX_PASS_COPY     /* blit a region of the current target into output (blend backdrop) */
} br_fx_pass_kind;

/* One offscreen pass, recorded at collect and run by br_fx_run_job in RENDER. */
typedef struct br_fx_pass {
    br_fx_pass_kind       kind;
    const br_shader_blob *shader;
    br_texture            input;
    br_texture            output;
    br_texture            extra[BR_FILTER_MAX_TEXTURES];
    uint32_t              extra_count;
    uint32_t              block_size;   /* bytes of `block` pushed to fragment b0 */
    uint8_t               block[sizeof(br_filter_globals) + BR_FILTER_MAX_UNIFORM_BYTES];
    SDL_Rect              copy_src;     /* COPY: target pixels */
    uint32_t              copy_w;       /* COPY: destination size in pixels */
    uint32_t              copy_h;
} br_fx_pass;

typedef struct br_filters_state br_filters_state;

/* A callback's user data: a run of passes. Allocated once and reused, so the pointer stays
 * valid until RENDER while the jobs array grows. */
typedef struct br_fx_job {
    br_filters_state *fs;
    uint32_t          first;
    uint32_t          count;
} br_fx_job;

typedef struct br_generate_job {
    br_node    node;
    br_texture target;
    br_rect    region;
    br_color   clear;
} br_generate_job;

struct br_filters_state {
    br_array pool;            /* br_pool_entry */
    br_array records;         /* br_fx_record*, indexed by node index (NULL = none) */
    br_array passes;          /* br_fx_pass, this frame */
    br_array jobs;            /* br_fx_job*, reused across frames */
    uint32_t jobs_used;
    br_array generate_jobs;   /* br_generate_job, rendered by the overlay */
    uint64_t frame;
    int      pixel_w;         /* window pixel size at the last frame (trim on change) */
    int      pixel_h;
};

/* ---- br_extract.c */

/* Installs the overlay that renders br_generate_texture jobs. */
void br_extract_init(br_engine *e);

#endif
