/* br_debug_internal.h - state of the debug overlay. */
#ifndef BR_DEBUG_INTERNAL_H
#define BR_DEBUG_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"
#include "microui/microui.h"

#define BR_DEBUG_LOG_LINE_SIZE 160
#define BR_DEBUG_FRAME_SAMPLES 120

typedef struct br_debug_state {
    mu_Context  *ui;
    bool         open;
    SDL_Scancode toggle_key;
    br_node      selected;
    br_array     expanded;   /* br_node: scene-tree rows the user opened */

    /* glyphs: a bitmap font installed from the text module's default font on first draw */
    br_font *font;
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
} br_debug_state;

/* Builds every panel for this frame (between mu_begin and mu_end). */
void br_debug_panels(br_engine *e, br_debug_state *s);

#endif
