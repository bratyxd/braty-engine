# job - background jobs on worker threads

Header: `engine/include/braty/br_job.h`. Source: `engine/src/core/br_job.c`.
Test: `tests/test_job.c`.

A big task that can be cut into independent pieces (parsing a large file's rows, generating
chunks of a map, building a path grid, baking a table) must not run inside one frame. A job
splits items `[0, count)` into ranges of `batch` items; worker threads (cores - 1, at most 8,
started by the first job) run the ranges while the game keeps stepping. `br_job_run` returns at
once; a system polls `br_job_is_done` each frame.

## Functions you actually need

| Function | What for |
|---|---|
| `br_job_run(e, fn, user, count, batch)` | Start `fn(user, begin, end)` over `[0, count)`; returns a `br_job` handle without waiting. `batch` 0 = one range (a single background task: `count` 1). |
| `br_job_is_done(e, job)` | Poll from a system. True for a finished, cancelled or zeroed handle. Never waits. |
| `br_job_progress(e, job)` | Items finished / items, 0..1: the loading bar. |
| `br_job_wait(e, job)` | Block until done; the caller runs ranges too. A parallel loop whose result this frame needs. |
| `br_job_cancel(e, job)` / `br_job_cancel_all(e)` | Drop ranges not started, wait for the ones running. Before freeing the job's memory or `br_level_arena_reset`. |
| `br_job_worker_count(e)` | Threads running (0 before the first job and in `--deterministic`). |

## Snippet

```c
typedef struct map_build {          /* lives in the level arena, allocated before the job starts */
    const char *rows;               /* input: read only */
    uint8_t    *cells;              /* output: range [begin, end) writes cells of its own rows */
    uint32_t    width, height;
} map_build;

static void build_rows(void *user, uint32_t begin, uint32_t end)   /* worker thread */
{
    map_build *m = user;
    for (uint32_t y = begin; y < end; y++) {
        for (uint32_t x = 0; x < m->width; x++) {
            m->cells[y * m->width + x] = cell_from_char(m->rows[y * m->width + x]);
        }
    }
}

/* level load (main thread) */
g->build = br_level_alloc(e, sizeof(map_build));
g->build->cells = br_level_alloc(e, width * height);
g->build_job = br_job_run(e, build_rows, g->build, height, 32);

/* a system, every frame (main thread) */
if (!g->map_ready && br_job_is_done(e, g->build_job)) {
    g->map_ready = true;            /* now the main thread may read cells and create nodes */
}

/* leaving the level */
br_job_cancel(e, g->build_job);     /* first: nothing may still write into the level arena */
br_level_arena_reset(e);
```

## Rules

- **The job function touches only the memory behind `user`.** It may call `br_alloc` /
  `br_free` and `br_log_*`; nothing else of the engine is thread-safe: no nodes, components,
  assets, `br_file_*`, arenas (`br_frame_alloc`, `br_level_alloc`), audio, GPU, `br_random_engine`.
  Read files and allocate on the main thread first, hand buffers in, and create nodes / textures
  / geometry from the result on the main thread after `br_job_is_done`.
- **Each range writes its own items.** Two ranges run at the same time, so output goes to
  `out[i]` for `i` in `[begin, end)` (or to one slot per range: `begin / batch`), never to a
  shared counter or list. A total is summed on the main thread afterwards. While the job runs,
  the main thread does not touch the output either.
- **The memory outlives the job.** `user` and what it points at stay alive until done: game or
  level arena, never the frame arena or the stack of the function that started the job (unless
  it calls `br_job_wait` before returning). Cancel before `br_level_arena_reset` or freeing.
- **Pick the batch so one range takes about a millisecond or less.** Smaller ranges spread
  better and make cancel and shutdown quick (they wait for ranges in flight); far smaller ones
  only pay the mutex per range. One range is taken under one lock: 10 000 ranges are fine,
  a million one-item ranges are not.
- **Sim code stays engine-free.** The range function belongs in `src/sim/` as plain C
  (`void sim_build_rows(void *user, uint32_t begin, uint32_t end)`); only the view calls
  `br_job_run`. A sim test calls the function directly with `(state, 0, count)`.
- **`--deterministic` runs the job inside `br_job_run`** (play-tests, `--frames`, golden
  images), so results never depend on thread timing; the same happens when no thread can be
  created. Code that polls `br_job_is_done` works unchanged. A hitch there is expected; measure
  frame times without `--deterministic`.

## Gotchas

- At most `BR_JOB_MAX` (64) jobs pending; one more runs on the calling thread and logs a warning.
  One job with many ranges, not many jobs with one.
- No completion callback and no result value: the result is the memory the job wrote, the signal
  is `br_job_is_done`.
- `br_job_cancel` leaves the output half written; throw it away.
- The engine cancels every job at shutdown before the game's shutdown code runs.
- Asset loading has its own threads (`br_assets_load_async`, asset groups, async scene loads:
  `docs/agents/assets.md`); do not load assets from a job.
- Worker ranges do not show in the profiler's scopes (it records the main thread).
