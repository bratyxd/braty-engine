/* br_job.h - background jobs: split a big task into ranges and run them on worker threads, so
 * the frame loop never waits for it.
 *
 *   static void parse_rows(void *user, uint32_t begin, uint32_t end)   // on a worker thread
 *   {
 *       my_table *t = user;
 *       for (uint32_t i = begin; i < end; i++) {
 *           t->out[i] = parse_row(t->rows[i]);    // writes only its own items
 *       }
 *   }
 *
 *   g->parse = br_job_run(e, parse_rows, table, table->row_count, 256);   // returns at once
 *   ...each frame, in a system:
 *   if (br_job_is_done(e, g->parse)) { use table->out }  else { show br_job_progress(e, g->parse) }
 *
 * The job function runs on other threads while the game keeps stepping. It may read and write
 * only the memory behind `user` (each range its own items), and may call br_alloc / br_free and
 * br_log_*. Nothing else of the engine is thread-safe: no nodes, components, assets, arenas, files
 * through `e`, audio or GPU calls. Take the memory from an arena before br_job_run and hand it in.
 *
 * `user` and everything it points at must stay alive until the job is done: br_job_wait or
 * br_job_cancel it before freeing that memory or resetting the arena it lives in.
 *
 * In --deterministic runs (play-tests, --frames) and where threads are unavailable the whole job
 * runs inside br_job_run, so a run gives the same result every time.
 */
#ifndef BR_JOB_H
#define BR_JOB_H

#include "braty/br_core.h"

/* Jobs that can be pending at once. One more runs inside br_job_run (logged). */
#define BR_JOB_MAX 64
/* Worker threads at most (cores - 1, at least 1; started by the first br_job_run). */
#define BR_JOB_MAX_WORKERS 8

/* Handle of a started job. A zeroed or finished handle counts as done. */
typedef struct br_job {
    uint32_t index;
    uint32_t generation;
} br_job;

/* Processes items [begin, end). Called on worker threads (and inside br_job_wait). */
typedef void (*br_job_fn)(void *user, uint32_t begin, uint32_t end);

/* Starts fn over items [0, count) in ranges of `batch` items (0 = one range with everything)
 * and returns without waiting. Ranges run in any order, several at once. */
br_job br_job_run(br_engine *e, br_job_fn fn, void *user, uint32_t count, uint32_t batch);
/* True when every range has finished (also for a cancelled, zeroed or stale handle). Never waits. */
bool br_job_is_done(br_engine *e, br_job job);
/* Items finished / items, 0..1 (1 when done): for a loading bar. */
float br_job_progress(br_engine *e, br_job job);
/* Blocks until the job is done; the calling thread runs ranges too. For a parallel loop whose
 * result the same frame needs. */
void br_job_wait(br_engine *e, br_job job);
/* Drops the ranges not started yet and waits for the ones running now (one range at most per
 * worker, so keep ranges short). */
void br_job_cancel(br_engine *e, br_job job);
/* br_job_cancel for every job: before br_level_arena_reset when jobs use level memory. The
 * engine calls it at shutdown before the game's shutdown code runs. */
void br_job_cancel_all(br_engine *e);
/* Worker threads running (0 before the first job and in --deterministic runs). */
uint32_t br_job_worker_count(br_engine *e);
/* Creates the job table. Called by the engine; games never call it. */
void br_job_register(br_engine *e);

#endif
