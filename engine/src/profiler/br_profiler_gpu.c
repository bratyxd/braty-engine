/* br_profiler_gpu: the GPU frame-time estimate. SDL_GPU has no timer queries, so every frame
 * is submitted with a fence and a small thread polls the pending fences (SDL_QueryGPUFence never
 * blocks and takes no SDL lock on D3D12 or Vulkan); the moment a fence is first seen signalled
 * is the frame's GPU completion time. GPU time of frame i =
 *     done(i) - max(submit(i), done(i - 1))
 * i.e. from when the GPU could start it (submitted, and the previous frame finished) to done.
 * Limits (docs/PROFILING.md): poll resolution ~0.1-1 ms, and with vsync on the command buffer
 * may wait for a swapchain image, so the estimate then includes display waits. */
#include "profiler/br_profiler_internal.h"
#include "gpu/br_gpu_internal.h"

/* Poll period of the fence thread. */
#define BR_PROFILER_POLL_NS 100000u

static int SDLCALL br_profiler_fence_thread(void *user)
{
    br_profiler_state *p = user;
    SDL_GPUDevice *device = p->fence_device;
    SDL_LockMutex(p->fence_lock);
    while (!p->fence_stop) {
        SDL_GPUFence *pending[BR_PROFILER_FENCES];
        uint32_t count = 0;
        for (uint32_t i = 0; i < p->fence_count; i++) {
            if (p->fences[i].done_ns == 0) {
                pending[count++] = p->fences[i].fence;
            }
        }
        if (count == 0) {
            SDL_WaitCondition(p->fence_wake, p->fence_lock);
            continue;
        }
        /* query without the lock: the main thread only releases fences already seen done */
        SDL_UnlockMutex(p->fence_lock);
        uint64_t done[BR_PROFILER_FENCES];
        for (uint32_t i = 0; i < count; i++) {
            done[i] = SDL_QueryGPUFence(device, pending[i]) ? SDL_GetTicksNS() : 0;
        }
        SDL_LockMutex(p->fence_lock);
        for (uint32_t i = 0; i < count; i++) {
            for (uint32_t j = 0; j < p->fence_count && done[i] != 0; j++) {
                if (p->fences[j].fence == pending[i] && p->fences[j].done_ns == 0) {
                    p->fences[j].done_ns = done[i];
                }
            }
        }
        SDL_UnlockMutex(p->fence_lock);
        // ponytail: fixed 0.1 ms poll (SDL_DelayNS is ~0.5 ms on Windows), so estimates are that coarse; a timer-query backend would be exact.
        SDL_DelayNS(BR_PROFILER_POLL_NS);
        SDL_LockMutex(p->fence_lock);
    }
    SDL_UnlockMutex(p->fence_lock);
    return 0;
}

void br_profiler_gpu_start(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (p->fence_lock != NULL || e->gpu == NULL) {
        return;
    }
    p->fence_device = br_gpu_device(e->gpu);
    p->fence_stop = false;
    p->fence_lock = SDL_CreateMutex();
    p->fence_wake = SDL_CreateCondition();
    if (p->fence_lock == NULL || p->fence_wake == NULL) {
        br_log_error("profiler: no GPU estimate (cannot create a mutex: %s)", SDL_GetError());
        return;
    }
    p->fence_thread = SDL_CreateThread(br_profiler_fence_thread, "braty.profiler.fences", p);
    if (p->fence_thread == NULL) {
        br_log_error("profiler: no GPU estimate (cannot create the fence thread: %s)", SDL_GetError());
        return;
    }
    e->gpu->want_fence = true;
}

void br_profiler_gpu_stop(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (p->fence_thread != NULL) {
        SDL_LockMutex(p->fence_lock);
        p->fence_stop = true;
        SDL_SignalCondition(p->fence_wake);
        SDL_UnlockMutex(p->fence_lock);
        SDL_WaitThread(p->fence_thread, NULL);
        p->fence_thread = NULL;
    }
    /* a fence released while its frame is in flight could be reused by the next submit */
    bool pending = false;
    for (uint32_t i = 0; i < p->fence_count; i++) {
        pending = pending || p->fences[i].done_ns == 0;
    }
    if (pending) {
        SDL_WaitForGPUIdle(p->fence_device);
    }
    for (uint32_t i = 0; i < p->fence_count; i++) {
        SDL_ReleaseGPUFence(p->fence_device, p->fences[i].fence);
    }
    p->fence_count = 0;
    if (e->gpu != NULL) {
        e->gpu->want_fence = false;
    }
    SDL_DestroyCondition(p->fence_wake);
    SDL_DestroyMutex(p->fence_lock);
    p->fence_wake = NULL;
    p->fence_lock = NULL;
}

void br_profiler_gpu_want(br_engine *e, uint32_t frames)
{
    br_profiler_state *p = e->profiler;
    if (p != NULL && p->recorded + frames > p->gpu_wanted_until) {
        p->gpu_wanted_until = p->recorded + frames;
    }
}

bool br_profiler_gpu_estimating(br_engine *e)
{
    return e->profiler != NULL && e->profiler->fence_thread != NULL;
}

/* The ring record of frame `number`, NULL if it was overwritten. */
static br_profiler_frame *br_profiler_frame_numbered(br_profiler_state *p, uint64_t number)
{
    br_profiler_frame *f = &p->frames[number % BR_PROFILER_FRAMES];
    return f->number == number ? f : NULL;
}

void br_profiler_gpu_frame_end(br_engine *e, br_profiler_frame *f)
{
    br_profiler_state *p = e->profiler;
    br_gpu *g = e->gpu;
    if (g == NULL) {
        return;
    }
    p->counters[BR_PROFILER_UPLOADS] += g->stats.uploads;
    p->counters[BR_PROFILER_UPLOAD_BYTES] += (int64_t)g->stats.upload_bytes;
    p->counters[BR_PROFILER_TARGET_SWITCHES] += g->stats.target_switches;
    f->submit_ns = g->stats.submit_ns;
    SDL_zero(g->stats);
    SDL_GPUFence *fence = g->frame_fence;
    g->frame_fence = NULL;
    if (p->fence_thread == NULL) {
        if (fence != NULL) {
            SDL_ReleaseGPUFence(br_gpu_device(g), fence);
        }
        return;
    }
    SDL_LockMutex(p->fence_lock);
    /* file finished frames in submission order (one queue: they finish in that order) */
    while (p->fence_count > 0 && p->fences[0].done_ns != 0) {
        br_profiler_fence done = p->fences[0];
        uint64_t start = done.submit_ns > p->last_gpu_done_ns ? done.submit_ns : p->last_gpu_done_ns;
        p->last_gpu_done_ns = done.done_ns;
        br_profiler_frame *rec = br_profiler_frame_numbered(p, done.frame);
        if (rec != NULL) {
            rec->gpu_start_ns = start;
            rec->gpu_ns = done.done_ns > start ? done.done_ns - start : 1; /* 0 means unknown */
        }
        SDL_ReleaseGPUFence(p->fence_device, done.fence);
        SDL_memmove(&p->fences[0], &p->fences[1], sizeof(p->fences[0]) * (p->fence_count - 1));
        p->fence_count -= 1;
    }
    if (fence != NULL) {
        if (p->fence_count < BR_PROFILER_FENCES && f->submit_ns != 0) {
            br_profiler_fence *slot = &p->fences[p->fence_count++];
            slot->fence = fence;
            slot->frame = f->number;
            slot->submit_ns = f->submit_ns;
            slot->done_ns = 0;
            SDL_SignalCondition(p->fence_wake);
        } else {
            SDL_ReleaseGPUFence(p->fence_device, fence); /* GPU far behind: skip this frame */
        }
    }
    SDL_UnlockMutex(p->fence_lock);
}
