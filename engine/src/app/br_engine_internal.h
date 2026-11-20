/* br_engine_internal.h - the engine context. Engine-private; games use the br_* functions.
 *
 * Later modules keep their state behind one pointer each (see "module state" below). A module
 * defines its own `struct br_<module>_state` in its own *_internal.h, allocates it in its
 * register function, and frees it from a br_engine_on_shutdown callback.
 */
#ifndef BR_ENGINE_INTERNAL_H
#define BR_ENGINE_INTERNAL_H

#include "braty/br_app.h"
#include "braty/br_random.h"

#include <SDL3/SDL.h>

typedef struct br_system_entry {
    char        *name;
    br_phase     phase;
    int          priority;
    br_system_fn fn;
    void        *user;
    bool         enabled;
    char        *group;           /* NULL = none */
    bool         group_enabled;   /* cached from engine.disabled_groups */
    uint64_t     last_time_ns;
    bool         removed;         /* br_system_remove while stepping: purged after the step */
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
    uint64_t deadline_ns;  /* FPS cap: when the next frame may start, 0 = no schedule yet */
    /* measured frame intervals (logged at exit of --frames runs) */
    uint64_t interval_count;
    uint64_t interval_sum_ns;
    uint64_t interval_max_ns;
    /* fixed timestep */
    float    fixed_hz;     /* 0 = FIXED_UPDATE runs once per frame with the frame's dt */
    double   fixed_accumulator;
    float    fixed_alpha;  /* leftover / step after this frame's fixed steps, in [0, 1) */
    uint32_t fixed_steps;  /* FIXED_UPDATE runs this frame */
} br_ticker_state;

/* At most this many fixed steps per frame; the rest of the backlog is dropped (no spiral of death). */
#define BR_FIXED_MAX_STEPS 8

struct br_engine {
    char *title;
    char *asset_root;               /* absolute, ends without '/' */

    SDL_Window *window;             /* NULL when headless */
    br_gpu     *gpu;                /* NULL when headless */

    br_array systems;               /* br_system_entry, sorted by phase then priority */
    br_array pending_systems;       /* added while stepping; merged after the step */
    br_map   retired_system_names;  /* name -> char *, names of removed systems (see br_system_name_copy) */
    bool     stepping;
    br_array disabled_groups;       /* char *, system groups switched off */
    br_array event_handlers;       /* br_event_handler_entry */
    br_array shutdown_handlers;     /* br_shutdown_entry, run in reverse order */

    br_ticker_state ticker;
    br_vsync vsync;                 /* present mode in effect (or requested, while headless) */
    bool     deterministic;         /* --deterministic: constant dt, seeded RNG, sync asset loads */
    br_random rng;                  /* the engine RNG: br_engine_random and br_random_engine(e) */
    bool     quit_requested;
    bool     exit_failure;          /* the app exits with a failure code (a play-test step failed) */
    bool     playtest_stdin;        /* --playtest-stdin: br_app_iterate reads commands instead */
    uint64_t skipped_frames;        /* frames where no swapchain texture was available */

    SDL_Storage *save_storage;      /* opened on first save access */

    /* --frames / --screenshot (0 / NULL when not given) */
    uint64_t frame_limit;
    char    *screenshot_path;
    void   (*game_shutdown)(br_engine *e);
    void    *app_user;              /* br_app_desc.user */
    int      argc;                  /* the app's command line (SDL owns argv) */
    char   **argv;

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
    struct br_input_state     *input;
    struct br_audio_state     *audio;
    struct br_filters_state   *filters;
    struct br_three_d_state   *three_d;
    struct br_gltf_state      *gltf;      /* glTF + 3D animation (Task 11) */
    struct br_data_state      *data;
    struct br_debug_state     *debug;
    struct br_playtest_state  *playtest;  /* play-test scripts, interactive mode, record (Task 14) */
    struct br_arena_state     *arena;     /* frame arena + tracked arenas (Task 14) */
    struct br_camera_state      *camera;
    struct br_canvas3d_state    *canvas3d;
    struct br_scenes_state      *scenes;
    struct br_animation_state   *animation;
    struct br_profiler_state    *profiler;
};

/* FPS cap: waits until the next frame's deadline (br_pacing.c). No-op when uncapped. */
void br_app_pace_frame(br_engine *e);

/* br_engine_screenshot, averaging n x n pixel blocks first (golden images stay small). */
bool br_engine_screenshot_downsampled(br_engine *e, const char *path, uint32_t downsample);

/* Destroys every 3D and 2D node (their on_destroy and destroy listeners run). Called by
 * br_app_quit before desc.shutdown and by br_engine_destroy before any shutdown callback. */
void br_engine_destroy_nodes(br_engine *e);

/* Registers every built-in module. Defined in br_builtin.c (owned by Task 2). */
void br_builtin_register(br_engine *e);

#endif
