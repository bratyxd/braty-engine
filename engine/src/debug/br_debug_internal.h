/* br_debug_internal.h - state of the debug overlay. */
#ifndef BR_DEBUG_INTERNAL_H
#define BR_DEBUG_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"
#include "microui/microui.h"
#include "profiler/br_profiler_internal.h"

#define BR_DEBUG_LOG_LINE_SIZE 160
#define BR_DEBUG_FRAME_SAMPLES 120

typedef struct br_debug_state {
    mu_Context  *ui;
    bool         open;
    bool         keys_enabled;   /* F1 / F2 react (br_debug_set_keys_enabled) */
    SDL_Scancode toggle_key;     /* SDL_SCANCODE_UNKNOWN = no key */
    br_node      selected;
    br_array     expanded;   /* br_node: scene-tree rows the user opened */

    /* glyphs: the bitmap font "braty.debug", installed from the text module's default font on first
     * draw and looked up by name on every use (a game may uninstall or replace it) */
    bool     font_failed;
    float    line_height;

    /* frame times (ms), a ring of the last BR_DEBUG_FRAME_SAMPLES frames */
    float    frame_ms[BR_DEBUG_FRAME_SAMPLES];
    uint32_t frame_index;
    uint64_t last_ns;

    /* log console: a ring of lines filled from SDL's log output (any thread) */
    SDL_Mutex            *log_lock;
    char                  log[BR_DEBUG_LOG_LINES][BR_DEBUG_LOG_LINE_SIZE];
    uint32_t              log_start;
    uint32_t              log_count;
    uint32_t              log_seen;     /* log_total when the console last scrolled */
    uint32_t              log_total;
    SDL_LogOutputFunction prev_log_fn;
    void                 *prev_log_user;

    /* input: the overlay takes clicks, wheel and typing that land on its windows */
    bool            ui_live;         /* windows were built last frame (hit tests use their rects) */
    mu_Container   *hud;             /* the profiler corner readout: display only, clicks pass */
    uint32_t        captured;        /* mouse buttons pressed on the overlay (bit = SDL button) */
    bool            filter_set;
    SDL_EventFilter prev_filter;
    void           *prev_filter_user;

    /* profiler panel (br_debug_profiler.c) */
    SDL_Scancode profiler_key;       /* SDL_SCANCODE_UNKNOWN = no key */
    int          profiler_tab;       /* 0 scopes, 1 counters, 2 memory */
    uint32_t     profiler_age;       /* inspected frame when paused (age in the ring), else newest */
    bool         profiler_toggled[BR_PROFILER_MAX_PATHS];  /* tree rows flipped from the default */
    struct br_prof_rows *prof_rows;   /* scratch for the scope tree, allocated on first use */
    br_asset_info       *prof_assets; /* scratch for the memory tab */
} br_debug_state;

/* Builds every panel for this frame (between mu_begin and mu_end). */
void br_debug_panels(br_engine *e, br_debug_state *s);
/* The profiler panel (F2). */
void br_debug_profiler_panel(br_engine *e, br_debug_state *s);
/* The profiler's compact corner readout. */
void br_debug_profiler_hud(br_engine *e, br_debug_state *s);
/* printf into a microui label. */
void br_debug_labelf(mu_Context *ui, const char *fmt, ...);

#endif
