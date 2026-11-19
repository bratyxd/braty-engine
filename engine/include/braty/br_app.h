/* br_app.h - the engine context, systems and phases, ticker, window, entry-point glue.
 *
 * A game defines br_main() (see braty_main.h) and returns a br_app_desc. The engine then:
 *   1. creates the br_engine and the window + GPU, registers built-in modules,
 *   2. calls desc.init(e), where the game registers components and systems,
 *   3. every frame: computes dt (ticker), runs all systems in phase order, presents,
 *   4. calls desc.shutdown(e) and destroys everything.
 *
 * Systems run in phase order, then by ascending priority, then in the order they were added:
 *   BR_PHASE_INPUT, UPDATE, LATE_UPDATE, PRE_RENDER   always run (also headless, also minimised)
 *   BR_PHASE_RENDER, POST_RENDER                      only when a frame target was acquired
 *
 * Command line (parsed by the engine, also visible to br_main):
 *   --frames N            run N frames, then exit 0
 *   --screenshot out.png  with --frames: write the last frame as PNG before exiting
 *   --gpu-driver NAME     vulkan | direct3d12 | metal (default: SDL's choice)
 */
#ifndef BR_APP_H
#define BR_APP_H

#include "braty/br_core.h"
#include "braty/br_gpu.h"
#include "braty/br_math.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

/* ---- game entry point */

/* What the game hands the engine. Zero fields get defaults (title "braty", 1280x720, asset
 * root = executable directory). */
typedef struct br_app_desc {
    const char *title;
    int         width;
    int         height;
    const char *asset_root;               /* relative to the executable directory, or absolute */
    bool (*init)(br_engine *e);           /* register components, systems, load first scene */
    void (*shutdown)(br_engine *e);
} br_app_desc;

/* The only function a game must define (include braty_main.h once in that file). */
br_app_desc br_main(int argc, char **argv);

/* ---- systems */

typedef enum br_phase {
    BR_PHASE_INPUT,
    BR_PHASE_UPDATE,
    BR_PHASE_LATE_UPDATE,
    BR_PHASE_PRE_RENDER,
    BR_PHASE_RENDER,
    BR_PHASE_POST_RENDER,
    BR_PHASE_COUNT
} br_phase;

/* A system: called once per frame with the frame's scaled delta time. */
typedef void (*br_system_fn)(br_engine *e, float dt_seconds, void *user);

/* Adds a system. Lower priority runs first within a phase. Names must be unique (copied). */
void br_system_add(br_engine *e, const char *name, br_phase phase, int priority, br_system_fn fn, void *user);
/* Enables or disables a system by name. Disabled systems are skipped. */
void br_system_set_enabled(br_engine *e, const char *name, bool enabled);

/* Read-only view of one system, for debug tools. */
typedef struct br_system_info {
    const char *name;
    br_phase    phase;
    int         priority;
    bool        enabled;
    uint64_t    last_time_ns;   /* time the last call took */
} br_system_info;

/* Number of systems. */
uint32_t br_system_count(br_engine *e);
/* Fills *out for system `index` (in run order). False if out of range. */
bool br_system_get_info(br_engine *e, uint32_t index, br_system_info *out);

/* ---- engine */

/* Creates a headless engine (no window, no GPU): enough for tests and tools.
 * title names the save-data folder; asset_root as in br_app_desc (NULL = executable dir). */
br_engine *br_engine_create(const char *title, const char *asset_root);
/* Runs shutdown callbacks (reverse order), destroys GPU and window, frees the engine. */
void br_engine_destroy(br_engine *e);
/* Opens a resizable window and the GPU device. gpu_driver may be NULL. */
bool br_engine_open_window(br_engine *e, int width, int height, const char *gpu_driver);
/* Runs one frame: all systems with this dt; renders if there is a window and a swapchain.
 * Returns true if a frame was presented. */
bool br_engine_step(br_engine *e, float dt_seconds);
/* Asks the app to exit after the current frame. */
void br_engine_quit(br_engine *e);
/* The GPU layer, NULL when headless. */
br_gpu *br_engine_gpu(br_engine *e);
/* The window, NULL when headless. */
SDL_Window *br_engine_window(br_engine *e);
/* Window size in logical units (what layout and input use). 0x0 when headless. */
void br_engine_window_size(br_engine *e, int *width, int *height);
/* Window size in pixels (what the frame target has). 0x0 when headless. */
void br_engine_pixel_size(br_engine *e, int *width, int *height);
/* Pixels per logical unit (PixiJS `resolution`); 1 when headless. */
float br_engine_pixel_density(br_engine *e);
/* Absolute asset root directory (no trailing slash). */
const char *br_engine_asset_root(br_engine *e);
/* Frames that were stepped but not rendered because no swapchain texture was available. */
uint64_t br_engine_skipped_frames(br_engine *e);

/* Callback for every SDL event (input modules use this). */
typedef void (*br_event_fn)(br_engine *e, const SDL_Event *event, void *user);
/* Adds an event handler, called in the order added. */
void br_engine_add_event_handler(br_engine *e, br_event_fn fn, void *user);

/* Callback run by br_engine_destroy (modules free their state here). */
typedef void (*br_shutdown_fn)(br_engine *e, void *user);
/* Adds a shutdown callback. They run in reverse order of adding, before the GPU is destroyed. */
void br_engine_on_shutdown(br_engine *e, br_shutdown_fn fn, void *user);

/* ---- ticker (PixiJS Ticker) */

/* Scaled, clamped delta of the current frame in seconds. */
float br_ticker_delta(br_engine *e);
/* Sum of all deltas so far, in seconds. */
double br_ticker_elapsed(br_engine *e);
/* Number of frames stepped so far. */
uint64_t br_ticker_frame(br_engine *e);
/* Measured frames per second (smoothed, unscaled). */
float br_ticker_fps(br_engine *e);
/* Time scale: 0.5 = slow motion, 0 = paused. Default 1. */
void br_ticker_set_speed(br_engine *e, float speed);
/* Caps the frame rate (0 = uncapped; vsync still applies). Uses SDL_HINT_MAIN_CALLBACK_RATE. */
void br_ticker_set_max_fps(br_engine *e, float max_fps);
/* Clamps delta to at most 1/min_fps so a long stall does not produce one huge step. Default 10. */
void br_ticker_set_min_fps(br_engine *e, float min_fps);
/* Pure helper used by the app: raw seconds -> clamped, scaled delta. */
float br_ticker_compute_delta(float raw_seconds, float min_fps, float speed);

/* ---- SDL main-callback glue (called by braty_main.h, not by games) */

/* SDL_AppInit body. */
SDL_AppResult br_app_init(void **appstate, const br_app_desc *desc, int argc, char **argv);
/* SDL_AppIterate body. */
SDL_AppResult br_app_iterate(void *appstate);
/* SDL_AppEvent body. */
SDL_AppResult br_app_event(void *appstate, SDL_Event *event);
/* SDL_AppQuit body. */
void br_app_quit(void *appstate, SDL_AppResult result);

#endif
