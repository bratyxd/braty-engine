/* br_profiler_internal.h - profiler state: name table, frame ring, GPU fence tracking.
 * Read by the debug overlay's profiler panel (engine/src/debug/br_debug_profiler.c). */
#ifndef BR_PROFILER_INTERNAL_H
#define BR_PROFILER_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"

#define BR_PROFILER_NONE 0xFFFFu
#define BR_PROFILER_NAME_SIZE 48
#define BR_PROFILER_MAX_PATHS (BR_PROFILER_MAX_NAMES * 2)
#define BR_PROFILER_NAME_HASH 1024  /* pointer -> name id, open addressing */
#define BR_PROFILER_PATH_HASH 2048  /* (parent path, name) -> path id */
#define BR_PROFILER_FENCES 8        /* frames in flight tracked by the fence thread */
#define BR_PROFILER_MAX_BUDGETS 16

/* One scope of one frame. Times are ns since the frame's start. */
typedef struct br_profiler_scope {
    uint32_t start;
    uint32_t end;
    uint16_t name;
    uint16_t path;    /* call path id (filled at frame end) */
    uint16_t parent;  /* scope index in the same frame, BR_PROFILER_NONE at the top */
    uint16_t depth;
} br_profiler_scope;

/* A call path: a name under a parent path (the scope tree's nodes). */
typedef struct br_profiler_path {
    uint16_t name;
    uint16_t parent;  /* path id, BR_PROFILER_NONE at the top */
    uint16_t depth;
} br_profiler_path;

typedef struct br_profiler_frame {
    uint64_t number;
    uint64_t start_ns;     /* profiler clock */
    uint64_t end_ns;
    uint64_t interval_ns;  /* to the next frame's start, 0 until known */
    uint64_t submit_ns;    /* when the frame's command buffer was submitted, 0 = no GPU frame */
    uint64_t gpu_start_ns; /* estimate: max(submit, previous fence done) */
    uint64_t gpu_ns;       /* estimate: fence done - gpu_start, 0 = unknown */
    uint64_t update_ns, render_ns, acquire_ns, present_ns, pace_ns;
    uint32_t scope_count;
    uint32_t dropped;
    br_profiler_verdict verdict;
    int64_t  counters[BR_PROFILER_COUNTER_COUNT];
} br_profiler_frame;

typedef struct br_profiler_fence {
    SDL_GPUFence *fence;
    uint64_t      frame;     /* frame number */
    uint64_t      submit_ns;
    uint64_t      done_ns;   /* written by the poll thread, 0 = not signalled yet */
} br_profiler_fence;

typedef struct br_profiler_budget {
    uint16_t name;
    float    ms;
    bool     breached;
    uint32_t frames_under;
} br_profiler_budget;

typedef struct br_profiler_state {
    /* names: copied text + pointer hash for fast lookups of literals */
    char        names[BR_PROFILER_MAX_NAMES][BR_PROFILER_NAME_SIZE];
    uint32_t    name_count;
    const char *name_keys[BR_PROFILER_NAME_HASH];
    uint16_t    name_ids[BR_PROFILER_NAME_HASH];
    uint16_t    name_update, name_prepare, name_render, name_acquire, name_present, name_pace;

    br_profiler_path paths[BR_PROFILER_MAX_PATHS];
    uint32_t         path_count;
    uint32_t         path_keys[BR_PROFILER_PATH_HASH];  /* (parent + 1) << 16 | name, 0 = empty */
    uint16_t         path_ids[BR_PROFILER_PATH_HASH];

    /* the ring */
    br_profiler_frame *frames;  /* BR_PROFILER_FRAMES */
    br_profiler_scope *scopes;  /* BR_PROFILER_FRAMES * BR_PROFILER_MAX_SCOPES */
    uint64_t           recorded; /* frames finished (the next frame's number) */

    /* the open frame */
    bool               in_frame;
    bool               interval_pending;  /* the newest frame's interval is set by the next begin */
    br_profiler_frame *cur;
    br_profiler_scope *cur_scopes;
    uint16_t           stack[BR_PROFILER_MAX_DEPTH];  /* open scope indices, BR_PROFILER_NONE = dropped */
    uint32_t           depth;
    int64_t            counters[BR_PROFILER_COUNTER_COUNT];
    uint64_t           last_total_allocs;
    bool               warned_unbalanced;
    SDL_ThreadID       main_thread;

    uint64_t (*clock)(void *user);
    void     *clock_user;

    /* verdict smoothing */
    br_profiler_verdict shown;
    br_profiler_verdict candidate;
    uint32_t            candidate_frames;
    br_profiler_summary_info summary;

    br_profiler_budget budgets[BR_PROFILER_MAX_BUDGETS];
    uint32_t           budget_count;

    /* UI and command line */
    bool  open;
    bool  hud;
    bool  paused;
    bool  args_read;
    char *save_path;  /* --profile FILE: written at shutdown */

    /* GPU fence estimate */
    SDL_GPUDevice    *fence_device;
    SDL_Thread       *fence_thread;
    SDL_Mutex        *fence_lock;
    SDL_Condition    *fence_wake;
    bool              fence_stop;
    br_profiler_fence fences[BR_PROFILER_FENCES];
    uint32_t          fence_count;   /* oldest first */
    uint64_t          last_gpu_done_ns;
    uint64_t          gpu_wanted_until; /* frame number until which a play-test "profile" wants it */
} br_profiler_state;

/* Name of id (the interned copy). */
const char *br_profiler_name(br_profiler_state *p, uint16_t id);
/* Interns name; id 0 is "(overflow)" when the table is full. */
uint16_t br_profiler_name_id(br_profiler_state *p, const char *name);
/* Frame record `age` frames back (0 = newest finished), NULL if out of range. */
br_profiler_frame *br_profiler_frame_at(br_profiler_state *p, uint32_t age);
/* Scopes of a frame record. */
br_profiler_scope *br_profiler_frame_scopes(br_profiler_state *p, const br_profiler_frame *f);
/* Current clock reading in ns. */
uint64_t br_profiler_now(br_profiler_state *p);
/* Self times (ns) of every scope of a frame into self[count]. */
void br_profiler_self_times(const br_profiler_scope *scopes, uint32_t count, uint64_t *self);
/* Self time (ns) of scope i: total minus its direct children. */
uint64_t br_profiler_scope_self(const br_profiler_scope *scopes, uint32_t count, uint32_t i);

/* ---- br_profiler_gpu.c */

/* Starts the fence poll thread (when a GPU exists). */
void br_profiler_gpu_start(br_engine *e);
/* Keeps the GPU estimate running for the next `frames` frames (play-test "profile"). */
void br_profiler_gpu_want(br_engine *e, uint32_t frames);
/* Stops the thread and releases tracked fences. */
void br_profiler_gpu_stop(br_engine *e);
/* At frame end: takes the frame's fence from the GPU module, files finished estimates. */
void br_profiler_gpu_frame_end(br_engine *e, br_profiler_frame *f);

/* ---- br_profiler_save.c */

/* Writes the --profile file if one was asked for (at shutdown). */
void br_profiler_save_at_exit(br_engine *e);

#endif
