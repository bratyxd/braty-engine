/* br_app.h - the engine context, systems and phases, ticker, window, entry-point glue.
 *
 * A game defines br_main() (see braty_main.h) and returns a br_app_desc. The engine then:
 *   1. creates the br_engine and the window + GPU, registers built-in modules,
 *   2. calls desc.init(e), where the game registers components and systems,
 *   3. every frame: computes dt (ticker), runs all systems in phase order, presents,
 *   4. destroys every node, calls desc.shutdown(e), then destroys everything else.
 *
 * Systems run in phase order, then by ascending priority, then in the order they were added:
 *   BR_PHASE_INPUT, FIXED_UPDATE, UPDATE, LATE_UPDATE, PRE_RENDER
 *                                                     always run (also headless, also minimised)
 *   BR_PHASE_RENDER, POST_RENDER                      only when a frame target was acquired
 *
 * Command line (parsed by the engine, also visible to br_main):
 *   --frames N            run N frames, then exit 0
 *   --screenshot out.png  with --frames: write the last frame as PNG before exiting
 *   --gpu-driver NAME     vulkan | direct3d12 | metal (default: SDL's choice)
 *   --headless            no window and no GPU: systems run, nothing is drawn
 *   --assets DIR          asset root override, relative to the working directory (or set
 *                         the BRATY_ASSETS environment variable)
 *   --vsync MODE          off | on | adaptive | mailbox (overrides game.json)
 *   --max-fps N           frame-rate cap, 0 = uncapped (overrides game.json)
 *   --deterministic       every frame steps exactly 1/60 s whatever the wall clock says, the
 *                         engine RNG is seeded (--seed N, default 1) and asset loads complete
 *                         synchronously: the same input gives the same frames, bit for bit
 *   --playtest FILE       run a play-test script, then exit (br_playtest.h, docs/PLAYTESTING.md)
 *   --playtest-stdin      interactive play-test: JSON commands on stdin, replies on stdout
 *   --record FILE         write this run's real input as a play-test script at exit
 * --frames runs log the measured frame interval (avg / max) at exit.
 * The game reads the whole command line with br_engine_args (or in br_main).
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

struct br_game_config;

/* What the game hands the engine. Zero fields get defaults (title "braty", 1280x720, asset
 * root = executable directory). */
typedef struct br_app_desc {
    const char *title;
    int         width;
    int         height;
    const char *asset_root;               /* relative to the executable directory, or absolute */
    bool (*init)(br_engine *e);           /* register components, systems, load first scene */
    void (*shutdown)(br_engine *e);
    void *user;                           /* for the game: br_engine_app_user(e) in init and later */
    const char *save_org;                 /* save identity (br_engine_set_save_name); NULL = "braty" */
    const char *save_app;                 /* NULL = the title */
    bool run_when_inactive;               /* false: pause while unfocused / minimised / in the
                                             background (br_app_set_pause_when_inactive) */
    /* Set by br_game_config_main, owned by the engine: applied before init (window, input,
     * audio, manifest, ...), its start scene loaded after init, then freed. */
    struct br_game_config *config;
} br_app_desc;

/* The only function a game must define (include braty_main.h once in that file). */
br_app_desc br_main(int argc, char **argv);

/* ---- systems */

/* BR_PHASE_FIXED_UPDATE runs 0..N times per frame with a constant dt when a fixed timestep is
 * set (br_app_set_fixed_timestep), otherwise once per frame with the frame's dt. */
typedef enum br_phase {
    BR_PHASE_INPUT,
    BR_PHASE_FIXED_UPDATE,
    BR_PHASE_UPDATE,
    BR_PHASE_LATE_UPDATE,
    BR_PHASE_PRE_RENDER,
    BR_PHASE_RENDER,
    BR_PHASE_POST_RENDER,
    BR_PHASE_COUNT
} br_phase;

/* A system: called once per frame with the frame's scaled delta time (FIXED_UPDATE systems:
 * 0..N times per frame with the fixed step). */
typedef void (*br_system_fn)(br_engine *e, float dt_seconds, void *user);

/* Adds a system. Lower priority runs first within a phase. Names must be unique (copied).
 * False (logged) for a duplicate name, a NULL fn or name, or a bad phase. */
bool br_system_add(br_engine *e, const char *name, br_phase phase, int priority, br_system_fn fn, void *user);
/* Removes a system by name. Inside a frame it stops running at once and leaves the list after
 * the frame, so a system may remove itself or others; the name can be added again right away. */
void br_system_remove(br_engine *e, const char *name);
/* Enables or disables a system by name. Disabled systems are skipped. */
void br_system_set_enabled(br_engine *e, const char *name, bool enabled);
/* Puts a system into a named group ("gameplay"), or into none with NULL. */
void br_system_set_group(br_engine *e, const char *name, const char *group);
/* Enables or disables a whole group (pause = gameplay off, UI on). Applies to systems added to
 * the group later too. A system runs only when it and its group are both enabled. */
void br_system_group_set_enabled(br_engine *e, const char *group, bool enabled);

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
 * title also names the save-data folder (br_engine_set_save_name); asset_root as in br_app_desc
 * (NULL = executable dir). */
br_engine *br_engine_create(const char *title, const char *asset_root);
/* Destroys every 3D and 2D node (destroy listeners run), then runs shutdown callbacks (reverse
 * order), destroys GPU and window, frees the engine. */
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
/* True under --deterministic (or br_app_set_deterministic). */
bool br_app_deterministic(br_engine *e);
/* Turns deterministic stepping on or off (see --deterministic); also seeds the RNG with `seed`. */
void br_app_set_deterministic(br_engine *e, bool on, uint64_t seed);

/* ---- engine random numbers (one generator per engine; the same seed gives the same sequence) */

/* Next 32 random bits. */
uint32_t br_engine_random(br_engine *e);
/* Random float in [0, 1). */
float br_engine_random_float(br_engine *e);
/* Restarts the generator from `seed`. Unseeded engines start from the clock. */
void br_engine_set_seed(br_engine *e, uint64_t seed);

/* Writes the last rendered frame as PNG (path relative to the working directory). False (logged)
 * when nothing was rendered (headless, minimised, first frame). */
bool br_engine_screenshot(br_engine *e, const char *path);
/* br_app_desc.user of the running app (NULL for engines made with br_engine_create). */
void *br_engine_app_user(br_engine *e);
/* The app's command line, argv[0] included (NULL / 0 for br_engine_create). argc may be NULL. */
char **br_engine_args(br_engine *e, int *argc);

/* Callback for every SDL event (input modules use this). */
typedef void (*br_event_fn)(br_engine *e, const SDL_Event *event, void *user);
/* Adds an event handler, called in the order added. */
void br_engine_add_event_handler(br_engine *e, br_event_fn fn, void *user);

/* Callback run by br_engine_destroy (modules free their state here). */
typedef void (*br_shutdown_fn)(br_engine *e, void *user);
/* Adds a shutdown callback. They run in reverse order of adding, after every node was destroyed
 * and before the GPU is destroyed. */
void br_engine_on_shutdown(br_engine *e, br_shutdown_fn fn, void *user);

/* ---- ticker (PixiJS Ticker) */

/* Scaled, clamped delta of the current frame in seconds (what systems get as dt). */
float br_ticker_delta(br_engine *e);
/* Clamped delta NOT scaled by the ticker speed: pause menus and UI animate with it while
 * br_ticker_set_speed(e, 0) freezes the game. 0 while the app is paused for being inactive. */
float br_ticker_unscaled_delta(br_engine *e);
/* Sum of all deltas so far, in seconds. */
double br_ticker_elapsed(br_engine *e);
/* Number of frames stepped so far. */
uint64_t br_ticker_frame(br_engine *e);
/* Measured frames per second (smoothed, unscaled). */
float br_ticker_fps(br_engine *e);
/* Time scale: 0.5 = slow motion, 0 = paused. Default 1. Negative or NaN is clamped to 0. */
void br_ticker_set_speed(br_engine *e, float speed);
/* Same as br_app_set_max_fps (kept for existing code). */
void br_ticker_set_max_fps(br_engine *e, float max_fps);
/* Clamps delta to at most 1/min_fps so a long stall does not produce one huge step. Default 10. */
void br_ticker_set_min_fps(br_engine *e, float min_fps);
/* Pure helper used by the app: raw seconds -> clamped, scaled delta. */
float br_ticker_compute_delta(float raw_seconds, float min_fps, float speed);

/* ---- frame pacing: vsync, FPS cap, fixed timestep (independent; all switchable at runtime,
 * from game.json and from the command line) */

typedef enum br_vsync {
    BR_VSYNC_OFF,       /* present at once, may tear (falls back to mailbox, then on) */
    BR_VSYNC_ON,        /* wait for the display (always supported) */
    BR_VSYNC_ADAPTIVE,  /* SDL GPU has no adaptive mode: falls back to on, with a log line */
    BR_VSYNC_MAILBOX    /* no tearing, never blocks, newest frame wins (falls back to on) */
} br_vsync;

/* Sets the present mode; unsupported modes fall back (logged). Returns the mode now in effect.
 * Headless: remembered and applied when a window opens. Default BR_VSYNC_ON. */
br_vsync br_app_set_vsync(br_engine *e, br_vsync mode);
/* The present mode in effect. */
br_vsync br_app_vsync(br_engine *e);
/* "off" / "on" / "adaptive" / "mailbox" -> mode. False for anything else. */
bool br_vsync_from_name(const char *name, br_vsync *out);
/* Mode -> "off" / "on" / "adaptive" / "mailbox". */
const char *br_vsync_name(br_vsync mode);
/* Caps the frame rate (0 = uncapped). Works with vsync off: sleeps most of the wait and spins
 * the last part (SDL_DelayPrecise) against a deadline, so frames do not drift. */
void br_app_set_max_fps(br_engine *e, float fps);
/* The cap, 0 = uncapped. */
float br_app_max_fps(br_engine *e);
/* Runs BR_PHASE_FIXED_UPDATE at `hz` steps per simulated second (dt = 1/hz each), 0..8 times
 * per frame; a larger backlog is dropped. 0 = once per frame with the frame's dt (default). */
void br_app_set_fixed_timestep(br_engine *e, float hz);
/* The fixed rate, 0 = off. */
float br_app_fixed_timestep(br_engine *e);
/* Fraction of a fixed step left over after this frame's steps, in [0, 1): interpolate rendered
 * positions between the previous and current fixed state with it. 0 when the timestep is off. */
float br_app_fixed_alpha(br_engine *e);
/* FIXED_UPDATE runs in the current frame. */
uint32_t br_app_fixed_steps(br_engine *e);
/* The frame time the pacing aims for, in ms: the FPS cap, or the display refresh with vsync on
 * (whichever is slower); 1000/60 when neither is known. The debug frame graph colours by it. */
float br_app_frame_budget_ms(br_engine *e);

/* ---- app lifecycle: focus, minimise, background (mobile), low memory, terminating
 *
 * The app is active while its window has focus, is not minimised and the OS has not sent it to
 * the background. While inactive, by default (br_app_set_pause_when_inactive, game.json
 * window.pause_when_inactive, br_app_desc.run_when_inactive) every system still runs but with
 * dt 0 (FIXED_UPDATE does not step, ticker elapsed stops) and audio output is paused. Never in
 * --deterministic, --frames or headless runs: those must not depend on the window manager. */

typedef enum br_app_lifecycle {
    BR_APP_LIFECYCLE_DEACTIVATED,  /* lost focus, minimised or sent to the background */
    BR_APP_LIFECYCLE_ACTIVATED,    /* active again */
    BR_APP_LIFECYCLE_LOW_MEMORY,   /* the OS asks the app to free what it can (drop caches) */
    BR_APP_LIFECYCLE_TERMINATING   /* the OS is ending the app (mobile): save now */
} br_app_lifecycle;

/* Lifecycle callback (show a pause menu on DEACTIVATED, save on TERMINATING). */
typedef void (*br_app_lifecycle_fn)(br_engine *e, br_app_lifecycle event, void *user);
/* Adds a lifecycle callback, called in the order added, from the event that caused it. */
void br_app_on_lifecycle(br_engine *e, br_app_lifecycle_fn fn, void *user);
/* True while focused, not minimised and in the foreground (true until told otherwise). */
bool br_app_is_active(br_engine *e);
/* True while time and audio are paused because the app is inactive. */
bool br_app_is_paused(br_engine *e);
/* Pause time and audio while inactive (default true). */
void br_app_set_pause_when_inactive(br_engine *e, bool on);

/* ---- SDL main-callback glue (called by braty_main.h, not by games) */

/* SDL_AppInit body. */
SDL_AppResult br_app_init(void **appstate, const br_app_desc *desc, int argc, char **argv);
/* SDL_AppIterate body. */
SDL_AppResult br_app_iterate(void *appstate);
/* SDL_AppEvent body. */
SDL_AppResult br_app_event(void *appstate, SDL_Event *event);
/* Entry for events that come from SDL itself (real devices). Under --deterministic, real
 * keyboard / mouse / touch / pen / gamepad input is dropped unless it is being recorded, so a
 * stray mouse over the window cannot change a scripted run. Injected input calls br_app_event. */
SDL_AppResult br_app_device_event(void *appstate, SDL_Event *event);
/* SDL_AppQuit body. */
void br_app_quit(void *appstate, SDL_AppResult result);

#endif
