/* br_profiler.h - CPU scopes, GPU frame-time estimate, counters, bottleneck verdict, capture.
 *
 * Every engine system and the frame steps are wrapped automatically; wrap your own code with
 *   br_profile_begin(e, "my_game.pathfinding");  ...  br_profile_end(e);
 * (names are string literals, or strings that live as long as the engine: br_profiler_intern).
 * The last BR_PROFILER_FRAMES frames are kept in a fixed ring (no per-frame allocation).
 *
 * F2 opens the profiler panel (debug overlay, br_debug.h); --profiler-hud (or
 * br_profiler_set_hud) shows a small corner readout. --profile out.json writes a Chrome
 * trace (chrome://tracing, ui.perfetto.dev) and out.json.txt (summary) at exit.
 * What the numbers mean, the verdict rule, overhead: docs/PROFILING.md.
 *
 * Build with -DBRATY_PROFILER_OFF (CMake option BRATY_PROFILER=OFF) and br_profile_begin /
 * br_profile_end / br_profiler_count compile to empty inline functions; frame times, the GPU
 * estimate and the verdict still work.
 */
#ifndef BR_PROFILER_H
#define BR_PROFILER_H

#include "braty/br_app.h"

/* Frames kept in the ring (5 s at 60 FPS). */
#define BR_PROFILER_FRAMES 300
/* Scopes recorded per frame; more are counted as dropped. */
#define BR_PROFILER_MAX_SCOPES 512
/* Nesting depth of scopes. */
#define BR_PROFILER_MAX_DEPTH 32
/* Distinct scope names (and distinct scope paths: BR_PROFILER_MAX_NAMES * 2). */
#define BR_PROFILER_MAX_NAMES 256
/* Frames the verdict and the window statistics average over. */
#define BR_PROFILER_WINDOW 30

/* Registers the module on an engine (called from br_builtin.c). */
void br_profiler_register(br_engine *e);

/* ---- scopes (main thread only; calls from other threads are ignored) */

#ifndef BRATY_PROFILER_OFF
/* Opens a scope named `name` (nested inside the open one). */
void br_profile_begin(br_engine *e, const char *name);
/* Closes the innermost open scope. */
void br_profile_end(br_engine *e);
#else
static inline void br_profile_begin(br_engine *e, const char *name) { (void)e; (void)name; }
static inline void br_profile_end(br_engine *e) { (void)e; }
#endif
/* A copy of `name` that lives as long as the engine (for names built at runtime). */
const char *br_profiler_intern(br_engine *e, const char *name);

/* ---- counters: summed over a frame (the engine fills the built-in ones) */

typedef enum br_profiler_counter {
    BR_PROFILER_DRAW_CALLS,        /* 2D batches + custom draws (br_render_stats) */
    BR_PROFILER_BATCHES,
    BR_PROFILER_VERTICES,
    BR_PROFILER_TRIANGLES,
    BR_PROFILER_PASSES,
    BR_PROFILER_BREAK_TEXTURE,     /* batch closed: all texture slots used */
    BR_PROFILER_BREAK_BLEND,       /* batch closed: blend mode changed */
    BR_PROFILER_BREAK_STATE,       /* batch closed: mask / scissor / stencil / transform changed */
    BR_PROFILER_BREAK_SHADER,      /* batch closed: custom-shader draw or render callback */
    BR_PROFILER_BREAK_OTHER,       /* batch closed: vertex limit or non-contiguous vertices */
    BR_PROFILER_PIPELINE_SWITCHES, /* pipeline binds in the 2D batcher */
    BR_PROFILER_TARGET_SWITCHES,   /* render passes begun (every render target change) */
    BR_PROFILER_UPLOADS,           /* GPU uploads (textures, buffers, per-frame geometry) */
    BR_PROFILER_UPLOAD_BYTES,
    BR_PROFILER_NODES,             /* 2D nodes alive */
    BR_PROFILER_NODES_DRAWN,       /* 2D nodes whose render callback ran */
    BR_PROFILER_NODES_CULLED,      /* 2D subtrees skipped by culling */
    BR_PROFILER_NODES_3D,          /* 3D nodes alive */
    BR_PROFILER_ANIMATORS_3D,
    BR_PROFILER_AUDIO_VOICES,      /* playing sound instances */
    BR_PROFILER_HEAP_BYTES,        /* live br_alloc bytes (whole process) */
    BR_PROFILER_HEAP_ALLOCS,       /* br_alloc calls this frame (whole process) */
    BR_PROFILER_ASSETS,            /* cached assets */
    BR_PROFILER_ASSETS_LOADING,    /* asset loads in flight */
    BR_PROFILER_COUNTER_COUNT
} br_profiler_counter;

#ifndef BRATY_PROFILER_OFF
/* Adds n to counter c for the current frame. */
void br_profiler_count(br_engine *e, br_profiler_counter c, int64_t n);
#else
static inline void br_profiler_count(br_engine *e, br_profiler_counter c, int64_t n) { (void)e; (void)c; (void)n; }
#endif
/* Display name of a counter ("draw calls", ...). */
const char *br_profiler_counter_name(br_profiler_counter c);

/* ---- frames (the engine calls these; br_engine_step brackets every frame) */

/* Starts a frame (no-op if one is open). br_app_iterate starts it before the FPS-cap wait. */
void br_profiler_frame_begin(br_engine *e);
/* Ends the frame: closes forgotten scopes, gathers counters, GPU estimate, verdict, budgets. */
void br_profiler_frame_end(br_engine *e);

/* ---- the verdict */

typedef enum br_profiler_verdict {
    BR_PROFILER_VERDICT_NONE,        /* not enough frames yet */
    BR_PROFILER_VERDICT_CPU_UPDATE,  /* CPU bound (update): game/engine logic is the slowest part */
    BR_PROFILER_VERDICT_CPU_RENDER,  /* CPU bound (render submit): building/submitting draws */
    BR_PROFILER_VERDICT_GPU,         /* GPU bound: the CPU waits for the GPU */
    BR_PROFILER_VERDICT_VSYNC,       /* vsync limited: frames run at the display rate */
    BR_PROFILER_VERDICT_FPS_CAP,     /* FPS cap limited: the CPU sleeps to hold the cap */
    BR_PROFILER_VERDICT_COUNT
} br_profiler_verdict;

/* Window averages the verdict rule reads (milliseconds; 0 = unknown / off). */
typedef struct br_profiler_verdict_input {
    float interval_ms;  /* frame start to next frame start */
    float update_ms;    /* CPU: INPUT..LATE_UPDATE systems */
    float render_ms;    /* CPU: PRE_RENDER..POST_RENDER systems (build + record draws) */
    float acquire_ms;   /* blocked acquiring the swapchain texture (frames in flight / vsync) */
    float present_ms;   /* in SDL submit + present */
    float pace_ms;      /* sleeping for the FPS cap */
    float gpu_ms;       /* GPU estimate from fences, 0 = unknown */
    float refresh_ms;   /* display refresh period when vsync waits for the display, else 0 */
    float cap_ms;       /* FPS cap period, 0 = no cap */
} br_profiler_verdict_input;

/* The rule of docs/PROFILING.md applied to one set of numbers (no smoothing). */
br_profiler_verdict br_profiler_verdict_rule(const br_profiler_verdict_input *in);
/* "CPU bound (update)", "GPU bound", ... */
const char *br_profiler_verdict_name(br_profiler_verdict v);

/* The latest numbers, averaged over the last BR_PROFILER_WINDOW frames. */
typedef struct br_profiler_summary_info {
    uint32_t frames;            /* frames averaged */
    float    fps;
    float    frame_ms;          /* interval */
    float    cpu_ms;            /* frame time minus waits (acquire, present, cap) */
    float    gpu_ms;            /* fence estimate, 0 = unknown */
    bool     gpu_includes_vsync;/* vsync on: the fence also waits for the display, an upper bound */
    br_profiler_verdict_input input;
    br_profiler_verdict verdict;/* smoothed: changes after holding for 15 frames */
    char     top[3][48];        /* the three scopes with most self time ("" if fewer) */
    float    top_ms[3];
} br_profiler_summary_info;
/* Fills *out with the current window (zeros before the first frames). */
void br_profiler_get_summary(br_engine *e, br_profiler_summary_info *out);

/* ---- frames in the ring */

/* One recorded frame (milliseconds). */
typedef struct br_profiler_frame_info {
    uint64_t number;        /* frames recorded before this one */
    float    frame_ms;      /* start to the next frame's start, 0 for the newest frame */
    float    wall_ms;       /* frame begin to frame end */
    float    cpu_ms;        /* wall minus acquire, present and cap waits */
    float    update_ms;
    float    render_ms;
    float    acquire_ms;
    float    present_ms;
    float    pace_ms;
    float    gpu_ms;        /* 0 = not known (yet) */
    uint32_t scopes;
    uint32_t dropped;       /* scopes over BR_PROFILER_MAX_SCOPES or BR_PROFILER_MAX_DEPTH */
    br_profiler_verdict verdict;
    int64_t  counters[BR_PROFILER_COUNTER_COUNT];
} br_profiler_frame_info;

/* Frames in the ring (at most BR_PROFILER_FRAMES). */
uint32_t br_profiler_frame_count(br_engine *e);
/* Frame `age` (0 = newest finished). False if out of range. */
bool br_profiler_get_frame(br_engine *e, uint32_t age, br_profiler_frame_info *out);
/* Total and self time and call count of scope `name` in frame `age`. False if it did not run. */
bool br_profiler_scope_time(br_engine *e, uint32_t age, const char *name, float *total_ms, float *self_ms,
                            uint32_t *calls);

/* ---- UI state */

/* Opens or closes the profiler panel (F2). */
void br_profiler_set_open(br_engine *e, bool open);
/* True while the profiler panel is shown. */
bool br_profiler_is_open(br_engine *e);
/* Shows the compact corner readout (FPS, CPU, GPU, verdict), with or without the panel. */
void br_profiler_set_hud(br_engine *e, bool on);
/* True while the corner readout is shown. */
bool br_profiler_hud(br_engine *e);
/* Stops recording new frames (the panel's pause); the ring keeps the frames captured so far. */
void br_profiler_set_paused(br_engine *e, bool paused);
/* True while paused. */
bool br_profiler_paused(br_engine *e);

/* ---- capture, summaries, budgets */

/* Writes the ring as Chrome trace-event JSON to path (relative = working directory) and the
 * text summary to path + ".txt". */
bool br_profiler_save(br_engine *e, const char *path);
/* Text summary of the last `frames` frames (0 = all kept): avg/p95/p99 frame ms, CPU, GPU,
 * verdict, top scopes. Returns false if no frame was recorded. */
bool br_profiler_summary(br_engine *e, uint32_t frames, char *out, size_t out_size);
/* The same summary as JSON object members of `out` (a cJSON object): frames, fps, frame_ms_avg,
 * frame_ms_p95, frame_ms_p99, frame_ms_max, cpu_ms_avg, cpu_ms_p95, gpu_ms_avg (null if
 * unknown), update_ms_avg, render_ms_avg, verdict, top_scopes [{name, self_ms, total_ms, calls}]. */
struct cJSON;
bool br_profiler_summary_json(br_engine *e, uint32_t frames, struct cJSON *out);
/* Logs a warning once when scope `name` (summed over a frame) takes longer than ms; armed
 * again after 60 frames under budget. ms <= 0 removes the budget. */
void br_profiler_set_budget(br_engine *e, const char *name, float ms);

/* ---- testing */

/* Replaces the nanosecond clock (NULL = SDL_GetTicksNS) so tests can inject timestamps. */
void br_profiler_set_clock(br_engine *e, uint64_t (*now_ns)(void *user), void *user);

#endif
