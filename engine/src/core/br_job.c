/* br_job: a fixed table of jobs and a few worker threads that take ranges from it.
 * Everything in br_job_state is guarded by one mutex; job functions run with it released. */
#include "braty/br_job.h"

#include "app/br_engine_internal.h"

typedef struct br_job_slot {
    br_job_fn fn;
    void     *user;
    uint32_t  count;       /* items; br_job_cancel lowers it to `next` */
    uint32_t  batch;       /* items per range, >= 1 */
    uint32_t  next;        /* first item not handed out yet */
    uint32_t  running;     /* ranges in flight */
    uint32_t  finished;    /* items done */
    uint32_t  generation;  /* never 0; bumped when the job ends, so old handles read as done */
    bool      active;
} br_job_slot;

typedef struct br_job_state {
    br_job_slot    slots[BR_JOB_MAX];
    SDL_Thread    *workers[BR_JOB_MAX_WORKERS];
    uint32_t       worker_count;
    bool           start_failed;  /* thread creation failed once: stay inline, do not log again */
    bool           quit;
    SDL_Mutex     *mutex;
    SDL_Condition *wake;          /* a range is available, or quit */
    SDL_Condition *done;          /* a job ended */
} br_job_state;

/* The live slot behind a handle, NULL when the job has ended. Mutex held. */
static br_job_slot *br_job_slot_get(br_job_state *s, br_job job)
{
    if (job.index >= BR_JOB_MAX) {
        return NULL;
    }
    br_job_slot *slot = &s->slots[job.index];
    return slot->active && slot->generation == job.generation ? slot : NULL;
}

/* Ends the job when nothing is left and nothing is in flight. Mutex held. */
static void br_job_end_if_finished(br_job_state *s, br_job_slot *slot)
{
    if (slot->next < slot->count || slot->running > 0) {
        return;
    }
    slot->active = false;
    slot->generation = slot->generation == UINT32_MAX ? 1 : slot->generation + 1;
    SDL_BroadcastCondition(s->done);
}

/* Takes the next range of `slot` and runs it. Mutex held on entry and exit, released while
 * the job function runs. */
static void br_job_run_range(br_job_state *s, br_job_slot *slot)
{
    br_job_fn fn = slot->fn;
    void *user = slot->user;
    uint32_t begin = slot->next;
    uint32_t left = slot->count - begin;
    uint32_t end = begin + (left < slot->batch ? left : slot->batch);
    slot->next = end;
    slot->running++;
    SDL_UnlockMutex(s->mutex);
    fn(user, begin, end);
    SDL_LockMutex(s->mutex);
    slot->running--;
    slot->finished += end - begin;
    br_job_end_if_finished(s, slot);
}

static int br_job_worker(void *data)
{
    br_job_state *s = data;
    SDL_LockMutex(s->mutex);
    while (!s->quit) {
        // ponytail: scans all 64 slots per range and favours low slots; a FIFO of active
        // slots if ranges ever get so small that the scan shows in a profile.
        br_job_slot *slot = NULL;
        for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
            if (s->slots[i].active && s->slots[i].next < s->slots[i].count) {
                slot = &s->slots[i];
                break;
            }
        }
        if (slot == NULL) {
            SDL_WaitCondition(s->wake, s->mutex);
        } else {
            br_job_run_range(s, slot);
        }
    }
    SDL_UnlockMutex(s->mutex);
    return 0;
}

static void br_job_start_workers(br_engine *e, br_job_state *s)
{
    if (s->worker_count > 0 || s->start_failed || e->deterministic) {
        return; /* --deterministic: the job runs inside br_job_run */
    }
    int cores = SDL_GetNumLogicalCPUCores();
    uint32_t wanted = cores > 2 ? (uint32_t)(cores - 1) : 1; /* one core stays with the frame loop */
    if (wanted > BR_JOB_MAX_WORKERS) {
        wanted = BR_JOB_MAX_WORKERS;
    }
    for (uint32_t i = 0; i < wanted; i++) {
        SDL_Thread *t = SDL_CreateThread(br_job_worker, "br_job", s);
        if (t == NULL) {
            br_log_warn("job: cannot create a worker thread (%s); jobs run on the calling thread", SDL_GetError());
            s->start_failed = s->worker_count == 0;
            break;
        }
        s->workers[s->worker_count] = t;
        s->worker_count++;
    }
}

br_job br_job_run(br_engine *e, br_job_fn fn, void *user, uint32_t count, uint32_t batch)
{
    br_job job = { 0, 0 };
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL || fn == NULL) {
        br_log_error("br_job_run: no engine job table or no function");
        return job;
    }
    if (count == 0) {
        return job;
    }
    br_job_start_workers(e, s);
    bool background = s->worker_count > 0 && !e->deterministic;

    SDL_LockMutex(s->mutex);
    br_job_slot *slot = NULL;
    for (uint32_t i = 0; i < BR_JOB_MAX && slot == NULL; i++) {
        if (!s->slots[i].active) {
            slot = &s->slots[i];
            job.index = i;
        }
    }
    if (slot == NULL) {
        SDL_UnlockMutex(s->mutex);
        br_log_warn("job: %d jobs are pending, running this one on the calling thread", BR_JOB_MAX);
        fn(user, 0, count);
        return job;
    }
    slot->fn = fn;
    slot->user = user;
    slot->count = count;
    slot->batch = batch == 0 ? count : batch;
    slot->next = 0;
    slot->running = 0;
    slot->finished = 0;
    slot->active = true;
    job.generation = slot->generation;
    if (background) {
        SDL_BroadcastCondition(s->wake);
    }
    SDL_UnlockMutex(s->mutex);
    if (!background) {
        br_job_wait(e, job);
    }
    return job;
}

bool br_job_is_done(br_engine *e, br_job job)
{
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL) {
        return true;
    }
    SDL_LockMutex(s->mutex);
    bool done = br_job_slot_get(s, job) == NULL;
    SDL_UnlockMutex(s->mutex);
    return done;
}

float br_job_progress(br_engine *e, br_job job)
{
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL) {
        return 1.0f;
    }
    SDL_LockMutex(s->mutex);
    br_job_slot *slot = br_job_slot_get(s, job);
    float progress = slot != NULL ? (float)slot->finished / (float)slot->count : 1.0f;
    SDL_UnlockMutex(s->mutex);
    return progress;
}

void br_job_wait(br_engine *e, br_job job)
{
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL) {
        return;
    }
    SDL_LockMutex(s->mutex);
    for (br_job_slot *slot = br_job_slot_get(s, job); slot != NULL; slot = br_job_slot_get(s, job)) {
        if (slot->next < slot->count) {
            br_job_run_range(s, slot); /* help instead of sleeping */
        } else {
            SDL_WaitCondition(s->done, s->mutex);
        }
    }
    SDL_UnlockMutex(s->mutex);
}

void br_job_cancel(br_engine *e, br_job job)
{
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL) {
        return;
    }
    SDL_LockMutex(s->mutex);
    br_job_slot *slot = br_job_slot_get(s, job);
    if (slot != NULL) {
        slot->count = slot->next;
        br_job_end_if_finished(s, slot);
    }
    while (br_job_slot_get(s, job) != NULL) {
        SDL_WaitCondition(s->done, s->mutex);
    }
    SDL_UnlockMutex(s->mutex);
}

void br_job_cancel_all(br_engine *e)
{
    br_job_state *s = e != NULL ? e->job : NULL;
    if (s == NULL) {
        return;
    }
    SDL_LockMutex(s->mutex);
    uint32_t active = 0;
    for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
        br_job_slot *slot = &s->slots[i];
        if (slot->active) {
            slot->count = slot->next;
            br_job_end_if_finished(s, slot);
            active += slot->active ? 1u : 0u;
        }
    }
    while (active > 0) {
        SDL_WaitCondition(s->done, s->mutex);
        active = 0;
        for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
            active += s->slots[i].active ? 1u : 0u;
        }
    }
    SDL_UnlockMutex(s->mutex);
}

uint32_t br_job_worker_count(br_engine *e)
{
    return e != NULL && e->job != NULL ? e->job->worker_count : 0;
}

static void br_job_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_job_state *s = e->job;
    br_job_cancel_all(e);
    SDL_LockMutex(s->mutex);
    s->quit = true;
    SDL_BroadcastCondition(s->wake);
    SDL_UnlockMutex(s->mutex);
    for (uint32_t i = 0; i < s->worker_count; i++) {
        SDL_WaitThread(s->workers[i], NULL);
    }
    SDL_DestroyCondition(s->wake);
    SDL_DestroyCondition(s->done);
    SDL_DestroyMutex(s->mutex);
    br_free(s);
    e->job = NULL;
}

void br_job_register(br_engine *e)
{
    br_job_state *s = br_alloc(sizeof(br_job_state));
    if (s == NULL) {
        return;
    }
    s->mutex = SDL_CreateMutex();
    s->wake = SDL_CreateCondition();
    s->done = SDL_CreateCondition();
    if (s->mutex == NULL || s->wake == NULL || s->done == NULL) {
        br_log_error("job: cannot create the mutex: %s", SDL_GetError());
        SDL_DestroyCondition(s->wake);
        SDL_DestroyCondition(s->done);
        SDL_DestroyMutex(s->mutex);
        br_free(s);
        return;
    }
    for (uint32_t i = 0; i < BR_JOB_MAX; i++) {
        s->slots[i].generation = 1;
    }
    e->job = s;
    br_engine_on_shutdown(e, br_job_shutdown, NULL);
}
