/* br_engine_internal.h - the engine context. Engine-private; games use the br_* functions.
 *
 * Later modules keep their state behind one pointer each (see "module state" below). A module
 * defines its own `struct br_<module>_state` in its own *_internal.h, allocates it in its
 * register function, and frees it from a br_engine_on_shutdown callback.
 */
#ifndef BR_ENGINE_INTERNAL_H
#define BR_ENGINE_INTERNAL_H

#include "braty/br_app.h"

#include <SDL3/SDL.h>

typedef struct br_system_entry {
    char        *name;
    br_phase     phase;
    int          priority;
    br_system_fn fn;
    void        *user;
    bool         enabled;
    uint64_t     last_time_ns;
} br_system_entry;

typedef struct br_event_handler_entry {
    br_event_fn fn;
    void       *user;
} br_event_handler_entry;

typedef struct br_shutdown_entry {
    br_shutdown_fn fn;
    void          *user;
} br_shutdown_entry;

typedef struct br_ticker_state {
    float    delta;        /* scaled, clamped seconds of the current frame */
    double   elapsed;      /* sum of delta */
    uint64_t frame;        /* frames stepped so far */
    float    fps;          /* smoothed frames per second (unscaled) */
    float    speed;        /* time scale, 1 = normal */
    float    max_fps;      /* 0 = unlimited */
    float    min_fps;      /* delta is clamped to 1/min_fps; 0 = no clamp */
    uint64_t last_ns;      /* SDL_GetTicksNS of the previous iterate, 0 before the first */
} br_ticker_state;

struct br_engine {
    char *title;
    char *asset_root;               /* absolute, ends without '/' */

    SDL_Window *window;             /* NULL when headless */
    br_gpu     *gpu;                /* NULL when headless */

    br_array systems;               /* br_system_entry, sorted by phase then priority */
    br_array pending_systems;       /* added while stepping; merged after the step */
    bool     stepping;
    br_array event_handlers;        /* br_event_handler_entry */
    br_array shutdown_handlers;     /* br_shutdown_entry, run in reverse order */

    br_ticker_state ticker;
    bool     quit_requested;
    uint64_t skipped_frames;        /* frames where no swapchain texture was available */

    SDL_Storage *save_storage;      /* opened on first save access */

    /* --frames / --screenshot (0 / NULL when not given) */
    uint64_t frame_limit;
    char    *screenshot_path;
    void   (*game_shutdown)(br_engine *e);

    /* module state: one pointer per later module, NULL until that module registers */
    struct br_scene_state     *scene;
    struct br_render_state    *render;
    struct br_assets_state    *assets;
    struct br_graphics_state  *graphics;
    struct br_text_state      *text;
    struct br_sprites_state   *sprites;
    struct br_mesh_state      *mesh;
    struct br_particles_state *particles;
    struct br_events_state    *events;
    struct br_audio_state     *audio;
    struct br_filters_state   *filters;
    struct br_three_d_state   *three_d;
    struct br_gltf_state      *gltf;
    struct br_data_state      *data;
    struct br_debug_state     *debug;
};

/* Registers every built-in module. Defined in br_builtin.c (owned by Task 2 onwards). */
void br_builtin_register(br_engine *e);

#endif
