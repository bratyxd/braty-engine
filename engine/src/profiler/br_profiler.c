/* br_profiler: scope recording into the frame ring, counters, the verdict rule and its
 * smoothing, budgets, UI flags. GPU fences: br_profiler_gpu.c. Files: br_profiler_save.c. */
#include "profiler/br_profiler_internal.h"
#include "gpu/br_gpu_internal.h"

/* Frames the raw verdict must hold before the shown one changes (no flicker). */
#define BR_PROFILER_VERDICT_HOLD 15
/* The fence estimate's resolution: the poll thread sleeps 0.1 ms, which is ~0.5 ms on Windows,
 * and a late "done" also delays the next frame's start; below this the CPU side is preferred. */
#define BR_PROFILER_GPU_RESOLUTION_MS 1.0f
/* Frames a budget must stay under before it can warn again. */
#define BR_PROFILER_BUDGET_REARM 60

static const char *const br_profiler_counter_names[BR_PROFILER_COUNTER_COUNT] = {
    "draw calls",       "batches",          "vertices",        "triangles",       "passes",
    "break: texture",   "break: blend",     "break: state",    "break: shader",   "break: other",
    "pipeline switches", "target switches", "uploads",         "upload bytes",    "nodes",
    "nodes drawn",      "nodes culled",     "3D nodes",        "3D animators",    "audio voices",
    "heap bytes",       "heap allocs",      "assets",          "assets loading",  "animators",
    "tweens",           "particles drawn",  "3D items",        "3D items culled", "3D shadow casters",
};

static const char *const br_profiler_verdict_names[BR_PROFILER_VERDICT_COUNT] = {
    "measuring...",       "CPU bound (update)", "CPU bound (render submit)",
    "GPU bound",          "vsync limited",      "FPS cap limited",
};

const char *br_profiler_counter_name(br_profiler_counter c)
{
    return (int)c >= 0 && c < BR_PROFILER_COUNTER_COUNT ? br_profiler_counter_names[c] : "?";
}

const char *br_profiler_verdict_name(br_profiler_verdict v)
{
    return (int)v >= 0 && v < BR_PROFILER_VERDICT_COUNT ? br_profiler_verdict_names[v] : "?";
}

/* ---- clock */

static uint64_t br_profiler_default_clock(void *user)
{
    (void)user;
    return SDL_GetTicksNS();
}

uint64_t br_profiler_now(br_profiler_state *p)
{
    return p->clock(p->clock_user);
}

void br_profiler_set_clock(br_engine *e, uint64_t (*now_ns)(void *user), void *user)
{
    br_profiler_state *p = e->profiler;
    p->clock = now_ns != NULL ? now_ns : br_profiler_default_clock;
    p->clock_user = user;
}

/* ---- names and paths */

const char *br_profiler_name(br_profiler_state *p, uint16_t id)
{
    return id < p->name_count ? p->names[id] : "?";
}

static uint16_t br_profiler_find_text(br_profiler_state *p, const char *name)
{
    for (uint32_t i = 1; i < p->name_count; i++) {
        if (SDL_strncmp(p->names[i], name, BR_PROFILER_NAME_SIZE - 1) == 0) {
            return (uint16_t)i;
        }
    }
    return BR_PROFILER_NONE;
}

/* Interns by text only (no pointer caching: for names that do not outlive the call). */
static uint16_t br_profiler_name_id_text(br_profiler_state *p, const char *name)
{
    uint16_t id = br_profiler_find_text(p, name);
    if (id == BR_PROFILER_NONE) {
        if (p->name_count >= BR_PROFILER_MAX_NAMES) {
            return 0;
        }
        id = (uint16_t)p->name_count++;
        br_str_copy(p->names[id], BR_PROFILER_NAME_SIZE, name);
    }
    return id;
}

uint16_t br_profiler_name_id(br_profiler_state *p, const char *name)
{
    if (name == NULL) {
        name = "(null)";
    }
    /* literals hit the pointer table: one hash and a compare per scope */
    uint32_t hash = (uint32_t)((uintptr_t)name >> 3) * 2654435761u;
    uint32_t slot = 0;
    bool free_slot = false;
    for (uint32_t probe = 0; probe < BR_PROFILER_NAME_HASH; probe++) {
        slot = (hash + probe) & (BR_PROFILER_NAME_HASH - 1);
        if (p->name_keys[slot] == name) {
            return p->name_ids[slot];
        }
        if (p->name_keys[slot] == NULL) {
            free_slot = true;
            break;
        }
    }
    uint16_t id = br_profiler_name_id_text(p, name);
    if (id == 0) {
        return 0; /* "(overflow)", not cached so a later free slot is not wasted */
    }
    if (free_slot) {
        p->name_keys[slot] = name;
        p->name_ids[slot] = id;
    }
    return id;
}

const char *br_profiler_intern(br_engine *e, const char *name)
{
    br_profiler_state *p = e->profiler;
    return br_profiler_name(p, br_profiler_name_id_text(p, name != NULL ? name : "(null)"));
}

static uint16_t br_profiler_path_id(br_profiler_state *p, uint16_t parent, uint16_t name, uint16_t depth)
{
    uint32_t key = ((uint32_t)(parent + 1u) & 0xFFFFu) << 16 | name;
    key += 1u; /* 0 marks an empty slot */
    uint32_t hash = key * 2654435761u;
    for (uint32_t probe = 0; probe < BR_PROFILER_PATH_HASH; probe++) {
        uint32_t slot = (hash + probe) & (BR_PROFILER_PATH_HASH - 1);
        if (p->path_keys[slot] == key) {
            return p->path_ids[slot];
        }
        if (p->path_keys[slot] == 0) {
            if (p->path_count >= BR_PROFILER_MAX_PATHS) {
                return 0; /* path 0 is "(overflow)" */
            }
            uint16_t id = (uint16_t)p->path_count++;
            p->paths[id].name = name;
            p->paths[id].parent = parent;
            p->paths[id].depth = depth;
            p->path_keys[slot] = key;
            p->path_ids[slot] = id;
            return id;
        }
    }
    return 0;
}

/* ---- the ring */

br_profiler_frame *br_profiler_frame_at(br_profiler_state *p, uint32_t age)
{
    uint64_t held = p->recorded < BR_PROFILER_FRAMES ? p->recorded : BR_PROFILER_FRAMES;
    if (age >= held) {
        return NULL;
    }
    return &p->frames[(p->recorded - 1 - age) % BR_PROFILER_FRAMES];
}

br_profiler_scope *br_profiler_frame_scopes(br_profiler_state *p, const br_profiler_frame *f)
{
    return &p->scopes[(size_t)(f->number % BR_PROFILER_FRAMES) * BR_PROFILER_MAX_SCOPES];
}

uint64_t br_profiler_scope_self(const br_profiler_scope *scopes, uint32_t count, uint32_t i)
{
    uint64_t self = scopes[i].end - scopes[i].start;
    /* children follow their parent in begin order and end before the parent's next sibling */
    for (uint32_t j = i + 1; j < count && scopes[j].depth > scopes[i].depth; j++) {
        if (scopes[j].parent == i) {
            uint64_t child = scopes[j].end - scopes[j].start;
            self = child < self ? self - child : 0;
        }
    }
    return self;
}

void br_profiler_self_times(const br_profiler_scope *scopes, uint32_t count, uint64_t *self)
{
    for (uint32_t i = 0; i < count; i++) {
        self[i] = scopes[i].end - scopes[i].start;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t parent = scopes[i].parent;
        if (parent != BR_PROFILER_NONE) {
            uint64_t child = scopes[i].end - scopes[i].start;
            self[parent] = child < self[parent] ? self[parent] - child : 0;
        }
    }
}

/* ---- scopes */

#ifndef BRATY_PROFILER_OFF
static bool br_profiler_recording(br_profiler_state *p)
{
    return p != NULL && p->in_frame && SDL_GetCurrentThreadID() == p->main_thread;
}

void br_profile_begin(br_engine *e, const char *name)
{
    br_profiler_state *p = e->profiler;
    if (!br_profiler_recording(p)) {
        return;
    }
    if (p->depth >= BR_PROFILER_MAX_DEPTH) {
        p->cur->dropped += 1;
        p->depth += 1; /* still counted so the matching end pops nothing */
        return;
    }
    uint16_t index = BR_PROFILER_NONE;
    br_profiler_scope *s = NULL;
    if (p->cur->scope_count < BR_PROFILER_MAX_SCOPES) {
        index = (uint16_t)p->cur->scope_count;
        p->cur->scope_count += 1;
        s = &p->cur_scopes[index];
        s->name = br_profiler_name_id(p, name);
        s->parent = p->depth > 0 ? p->stack[p->depth - 1] : BR_PROFILER_NONE;
        s->depth = (uint16_t)p->depth;
        s->path = 0;
    } else {
        p->cur->dropped += 1;
    }
    p->stack[p->depth] = index;
    p->depth += 1;
    if (s != NULL) {
        /* read the clock last: the bookkeeping above is not charged to the scope */
        uint64_t offset = br_profiler_now(p) - p->cur->start_ns;
        s->start = offset < UINT32_MAX ? (uint32_t)offset : UINT32_MAX;
        s->end = s->start;
    }
}

void br_profile_end(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (!br_profiler_recording(p)) {
        return;
    }
    uint64_t now = br_profiler_now(p);
    if (p->depth == 0) {
        if (!p->warned_unbalanced) {
            br_log_warn("profiler: br_profile_end without a matching br_profile_begin");
            p->warned_unbalanced = true;
        }
        return;
    }
    p->depth -= 1;
    if (p->depth >= BR_PROFILER_MAX_DEPTH) {
        return; /* a scope beyond the depth limit */
    }
    uint16_t index = p->stack[p->depth];
    if (index != BR_PROFILER_NONE) {
        // ponytail: offsets are 32-bit ns, so a single frame longer than 4.29 s is clamped; widen to 64-bit if loading hitches need exact numbers.
        uint64_t offset = now - p->cur->start_ns;
        p->cur_scopes[index].end = offset < UINT32_MAX ? (uint32_t)offset : UINT32_MAX;
    }
}

void br_profiler_count(br_engine *e, br_profiler_counter c, int64_t n)
{
    br_profiler_state *p = e->profiler;
    if (p != NULL && (int)c >= 0 && c < BR_PROFILER_COUNTER_COUNT) {
        p->counters[c] += n;
    }
}
#endif

/* ---- frames */

/* --profile FILE, --profiler-hud, --profiler: read once, on first use after the app set argv. */
static void br_profiler_read_args(br_engine *e, br_profiler_state *p)
{
    if (p->args_read || e->argv == NULL) {
        return;
    }
    p->args_read = true;
    int argc = 0;
    char **argv = br_engine_args(e, &argc);
    for (int i = 1; i < argc && argv != NULL; i++) {
        bool has_value = i + 1 < argc && SDL_strncmp(argv[i + 1], "--", 2) != 0;
        if (SDL_strcmp(argv[i], "--profile") == 0 && has_value) {
            br_free(p->save_path);
            p->save_path = br_strdup(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--profiler-hud") == 0) {
            p->hud = true;
        } else if (SDL_strcmp(argv[i], "--profiler") == 0) {
            p->open = true;
        }
    }
}

void br_profiler_frame_begin(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (p == NULL || p->in_frame || p->frames == NULL) {
        return;
    }
    br_profiler_read_args(e, p);
    /* the fence thread and a fence per frame cost every shipped game something: they run only
     * while something shows or saves the GPU estimate */
    bool want_gpu = p->open || p->hud || p->save_path != NULL || p->recorded < p->gpu_wanted_until;
    if (e->gpu != NULL && want_gpu && p->fence_thread == NULL) {
        br_profiler_gpu_start(e);
    } else if (!want_gpu && p->fence_thread != NULL) {
        br_profiler_gpu_stop(e);
    }
    if (p->paused) {
        p->interval_pending = false; /* no interval across a pause */
        return;
    }
    uint64_t now = br_profiler_now(p);
    br_profiler_frame *prev = br_profiler_frame_at(p, 0);
    if (prev != NULL && p->interval_pending) {
        prev->interval_ns = now > prev->start_ns ? now - prev->start_ns : 1;
    }
    p->interval_pending = false;
    br_profiler_frame *f = &p->frames[p->recorded % BR_PROFILER_FRAMES];
    SDL_zerop(f);
    f->number = p->recorded;
    f->start_ns = now;
    p->cur = f;
    p->cur_scopes = br_profiler_frame_scopes(p, f);
    p->depth = 0;
    p->main_thread = SDL_GetCurrentThreadID();
    p->in_frame = true;
}

/* Fills the counters every module exposes publicly. */
static void br_profiler_gather_counters(br_engine *e, br_profiler_state *p)
{
    int64_t *c = p->counters;
    if (e->render != NULL) {
        br_render_stats rs;
        br_render_get_stats(e, &rs);
        c[BR_PROFILER_DRAW_CALLS] += rs.draw_calls;
        c[BR_PROFILER_BATCHES] += rs.batches;
        c[BR_PROFILER_VERTICES] += rs.vertices;
        c[BR_PROFILER_PASSES] += rs.passes;
        c[BR_PROFILER_NODES_DRAWN] += rs.nodes_drawn;
    }
    if (e->scene != NULL) {
        c[BR_PROFILER_NODES] += br_scene_node_count(e);
    }
    c[BR_PROFILER_NODES_3D] += br_node3d_count(e);
    c[BR_PROFILER_ANIMATORS_3D] += br_animator3d_count(e);
    c[BR_PROFILER_ANIMATORS] += br_animator_count(e);
    c[BR_PROFILER_TWEENS] += br_tween_count(e);
    if (e->audio != NULL) {
        c[BR_PROFILER_AUDIO_VOICES] += br_audio_instance_count(e);
    }
    if (e->assets != NULL) {
        c[BR_PROFILER_ASSETS] += br_assets_count(e);
        c[BR_PROFILER_ASSETS_LOADING] += br_assets_pending(e);
    }
    br_memory_stats mem;
    br_memory_get_stats(&mem);
    c[BR_PROFILER_HEAP_BYTES] += (int64_t)mem.live_bytes;
    if (p->last_total_allocs != 0) {
        c[BR_PROFILER_HEAP_ALLOCS] += (int64_t)(mem.total_allocations - p->last_total_allocs);
    }
    p->last_total_allocs = mem.total_allocations;
}

/* Sums the top-level scopes into the frame's update / render / wait times, assigns paths. */
static void br_profiler_frame_totals(br_profiler_state *p, br_profiler_frame *f, br_profiler_scope *scopes)
{
    for (uint32_t i = 0; i < f->scope_count; i++) {
        br_profiler_scope *s = &scopes[i];
        uint16_t parent_path = s->parent == BR_PROFILER_NONE ? BR_PROFILER_NONE : scopes[s->parent].path;
        s->path = br_profiler_path_id(p, parent_path, s->name, s->depth);
        if (s->parent != BR_PROFILER_NONE) {
            continue;
        }
        uint64_t total = s->end - s->start;
        if (s->name == p->name_update) {
            f->update_ns += total;
        } else if (s->name == p->name_prepare || s->name == p->name_render) {
            f->render_ns += total;
        } else if (s->name == p->name_acquire) {
            f->acquire_ns += total;
        } else if (s->name == p->name_present) {
            f->present_ns += total;
        } else if (s->name == p->name_pace) {
            f->pace_ns += total;
        }
    }
}

static void br_profiler_check_budgets(br_profiler_state *p, br_profiler_frame *f, br_profiler_scope *scopes)
{
    for (uint32_t b = 0; b < p->budget_count; b++) {
        br_profiler_budget *budget = &p->budgets[b];
        uint64_t total = 0;
        for (uint32_t i = 0; i < f->scope_count; i++) {
            if (scopes[i].name == budget->name) {
                total += scopes[i].end - scopes[i].start;
            }
        }
        double ms = (double)total / 1e6;
        if (ms > (double)budget->ms) {
            if (!budget->breached) {
                br_log_warn("profiler: '%s' took %.3f ms, budget %.3f ms (frame %llu)",
                            br_profiler_name(p, budget->name), ms, (double)budget->ms,
                            (unsigned long long)f->number);
                budget->breached = true;
            }
            budget->frames_under = 0;
        } else if (budget->breached) {
            budget->frames_under += 1;
            if (budget->frames_under >= BR_PROFILER_BUDGET_REARM) {
                budget->breached = false;
            }
        }
    }
}

static float br_profiler_ms(uint64_t ns)
{
    return (float)((double)ns / 1e6);
}

static uint64_t br_profiler_cpu_ns(const br_profiler_frame *f)
{
    uint64_t wall = f->end_ns > f->start_ns ? f->end_ns - f->start_ns : 0;
    uint64_t waits = f->acquire_ns + f->present_ns + f->pace_ns;
    return wall > waits ? wall - waits : 0;
}

static bool br_profiler_waits_for_display(br_engine *e)
{
    return e->window != NULL && (e->vsync == BR_VSYNC_ON || e->vsync == BR_VSYNC_ADAPTIVE);
}

/* Window averages, the top three scopes and the smoothed verdict (after a frame ends). */
static void br_profiler_update_summary(br_engine *e, br_profiler_state *p)
{
    br_profiler_summary_info *out = &p->summary;
    double sum[7] = { 0 }; /* interval, update, render, acquire, present, pace, cpu */
    double gpu_sum = 0.0;
    uint32_t gpu_n = 0;
    uint32_t n = 0;
    float self_ms[BR_PROFILER_MAX_NAMES];
    uint64_t self_ns[BR_PROFILER_MAX_SCOPES];
    SDL_zeroa(self_ms);
    for (uint32_t age = 1; n < BR_PROFILER_WINDOW; age++) {
        br_profiler_frame *f = br_profiler_frame_at(p, age);
        if (f == NULL) {
            break;
        }
        if (f->interval_ns == 0) {
            continue;
        }
        n += 1;
        sum[0] += (double)f->interval_ns;
        sum[1] += (double)f->update_ns;
        sum[2] += (double)f->render_ns;
        sum[3] += (double)f->acquire_ns;
        sum[4] += (double)f->present_ns;
        sum[5] += (double)f->pace_ns;
        sum[6] += (double)br_profiler_cpu_ns(f);
        if (f->gpu_ns != 0) {
            gpu_sum += (double)f->gpu_ns;
            gpu_n += 1;
        }
        const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
        br_profiler_self_times(scopes, f->scope_count, self_ns);
        for (uint32_t i = 0; i < f->scope_count; i++) {
            self_ms[scopes[i].name] += br_profiler_ms(self_ns[i]);
        }
    }
    br_profiler_verdict_input *in = &out->input;
    SDL_zerop(in);
    out->frames = n;
    if (n > 0) {
        in->interval_ms = (float)(sum[0] / n / 1e6);
        in->update_ms = (float)(sum[1] / n / 1e6);
        in->render_ms = (float)(sum[2] / n / 1e6);
        in->acquire_ms = (float)(sum[3] / n / 1e6);
        in->present_ms = (float)(sum[4] / n / 1e6);
        in->pace_ms = (float)(sum[5] / n / 1e6);
        out->cpu_ms = (float)(sum[6] / n / 1e6);
    }
    in->gpu_ms = gpu_n > 0 ? (float)(gpu_sum / gpu_n / 1e6) : 0.0f;
    in->cap_ms = e->ticker.max_fps > 0.0f ? 1000.0f / e->ticker.max_fps : 0.0f;
    out->gpu_includes_vsync = br_profiler_waits_for_display(e);
    if (out->gpu_includes_vsync) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(e->window));
        in->refresh_ms = mode != NULL && mode->refresh_rate > 0.0f ? 1000.0f / mode->refresh_rate : 0.0f;
    }
    out->frame_ms = in->interval_ms;
    out->fps = in->interval_ms > 0.0f ? 1000.0f / in->interval_ms : 0.0f;
    out->gpu_ms = in->gpu_ms;

    /* top three by self time; waiting is in the verdict, not a "scope to optimise" */
    SDL_zeroa(out->top);
    SDL_zeroa(out->top_ms);
    for (uint32_t k = 0; k < 3; k++) {
        int best = -1;
        for (uint32_t id = 1; id < p->name_count; id++) {
            bool wait = id == p->name_acquire || id == p->name_present || id == p->name_pace;
            bool taken = false;
            for (uint32_t j = 0; j < k; j++) {
                taken = taken || SDL_strcmp(out->top[j], p->names[id]) == 0;
            }
            if (!wait && !taken && self_ms[id] > 0.0f && (best < 0 || self_ms[id] > self_ms[best])) {
                best = (int)id;
            }
        }
        if (best >= 0) {
            br_str_copy(out->top[k], sizeof(out->top[k]), p->names[best]);
            out->top_ms[k] = n > 0 ? self_ms[best] / (float)n : 0.0f;
        }
    }

    /* smoothing: the shown verdict changes only after the new one held for a while */
    br_profiler_verdict raw = n >= BR_PROFILER_WINDOW / 3 ? br_profiler_verdict_rule(in) : BR_PROFILER_VERDICT_NONE;
    if (p->shown == BR_PROFILER_VERDICT_NONE || raw == p->shown) {
        p->shown = raw;
        p->candidate_frames = 0;
    } else if (raw == p->candidate) {
        p->candidate_frames += 1;
        if (p->candidate_frames >= BR_PROFILER_VERDICT_HOLD) {
            p->shown = raw;
            p->candidate_frames = 0;
        }
    } else {
        p->candidate = raw;
        p->candidate_frames = 1;
    }
    out->verdict = p->shown;
}

void br_profiler_frame_end(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (p == NULL || !p->in_frame) {
        return;
    }
    uint64_t now = br_profiler_now(p);
    br_profiler_frame *f = p->cur;
    br_profiler_scope *scopes = p->cur_scopes;
    if (p->depth > 0) {
        if (!p->warned_unbalanced) {
            br_log_warn("profiler: %u scope(s) still open at the end of a frame (missing br_profile_end)", p->depth);
            p->warned_unbalanced = true;
        }
        uint64_t offset = now - f->start_ns;
        for (uint32_t d = 0; d < p->depth && d < BR_PROFILER_MAX_DEPTH; d++) {
            if (p->stack[d] != BR_PROFILER_NONE) {
                scopes[p->stack[d]].end = offset < UINT32_MAX ? (uint32_t)offset : UINT32_MAX;
            }
        }
        p->depth = 0;
    }
    f->end_ns = now;
    br_profiler_frame_totals(p, f, scopes);
    br_profiler_gather_counters(e, p);
    br_profiler_gpu_frame_end(e, f);
    SDL_memcpy(f->counters, p->counters, sizeof(f->counters));
    SDL_zeroa(p->counters);
    br_profiler_check_budgets(p, f, scopes);
    p->in_frame = false;
    p->interval_pending = true;
    p->recorded += 1;
    br_profiler_update_summary(e, p);
    f->verdict = p->shown;
}

/* ---- the verdict rule (docs/PROFILING.md, "The verdict") */

static bool br_profiler_near(float value, float target)
{
    return target > 0.0f && SDL_fabsf(value - target) <= target * 0.1f;
}

br_profiler_verdict br_profiler_verdict_rule(const br_profiler_verdict_input *in)
{
    if (in->interval_ms <= 0.0f) {
        return BR_PROFILER_VERDICT_NONE;
    }
    float cpu = in->update_ms + in->render_ms;
    float waits = in->acquire_ms + in->present_ms;
    /* 1. the cap holds the rate and the CPU sleeps for it */
    if (in->cap_ms > 0.0f && br_profiler_near(in->interval_ms, in->cap_ms) && in->pace_ms >= 0.1f * in->interval_ms) {
        return BR_PROFILER_VERDICT_FPS_CAP;
    }
    /* 2. frames come at the display rate with CPU time to spare */
    if (in->refresh_ms > 0.0f && br_profiler_near(in->interval_ms, in->refresh_ms) && cpu < 0.9f * in->refresh_ms) {
        return BR_PROFILER_VERDICT_VSYNC;
    }
    /* 3. GPU: the fence estimate (only without vsync, where it is not inflated by display waits)
     * exceeds the CPU's work by more than its resolution, or the CPU spends more time waiting
     * for the GPU than working */
    if (in->refresh_ms <= 0.0f && in->gpu_ms > cpu + BR_PROFILER_GPU_RESOLUTION_MS) {
        return BR_PROFILER_VERDICT_GPU;
    }
    if (waits > cpu) {
        return BR_PROFILER_VERDICT_GPU;
    }
    /* 4. CPU: whichever side of the frame costs more */
    return in->update_ms >= in->render_ms ? BR_PROFILER_VERDICT_CPU_UPDATE : BR_PROFILER_VERDICT_CPU_RENDER;
}

void br_profiler_get_summary(br_engine *e, br_profiler_summary_info *out)
{
    *out = e->profiler->summary;
}

/* ---- frame access */

uint32_t br_profiler_frame_count(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    return (uint32_t)(p->recorded < BR_PROFILER_FRAMES ? p->recorded : BR_PROFILER_FRAMES);
}

bool br_profiler_get_frame(br_engine *e, uint32_t age, br_profiler_frame_info *out)
{
    br_profiler_frame *f = br_profiler_frame_at(e->profiler, age);
    if (f == NULL) {
        return false;
    }
    out->number = f->number;
    out->frame_ms = br_profiler_ms(f->interval_ns);
    out->wall_ms = br_profiler_ms(f->end_ns - f->start_ns);
    out->cpu_ms = br_profiler_ms(br_profiler_cpu_ns(f));
    out->update_ms = br_profiler_ms(f->update_ns);
    out->render_ms = br_profiler_ms(f->render_ns);
    out->acquire_ms = br_profiler_ms(f->acquire_ns);
    out->present_ms = br_profiler_ms(f->present_ns);
    out->pace_ms = br_profiler_ms(f->pace_ns);
    out->gpu_ms = br_profiler_ms(f->gpu_ns);
    out->scopes = f->scope_count;
    out->dropped = f->dropped;
    out->verdict = f->verdict;
    SDL_memcpy(out->counters, f->counters, sizeof(out->counters));
    return true;
}

bool br_profiler_scope_time(br_engine *e, uint32_t age, const char *name, float *total_ms, float *self_ms,
                            uint32_t *calls)
{
    br_profiler_state *p = e->profiler;
    br_profiler_frame *f = br_profiler_frame_at(p, age);
    uint16_t id = name != NULL ? br_profiler_find_text(p, name) : BR_PROFILER_NONE;
    *total_ms = 0.0f;
    *self_ms = 0.0f;
    *calls = 0;
    if (f == NULL || id == BR_PROFILER_NONE) {
        return false;
    }
    const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
    for (uint32_t i = 0; i < f->scope_count; i++) {
        if (scopes[i].name == id) {
            *total_ms += br_profiler_ms(scopes[i].end - scopes[i].start);
            *self_ms += br_profiler_ms(br_profiler_scope_self(scopes, f->scope_count, i));
            *calls += 1;
        }
    }
    return *calls > 0;
}

/* ---- UI flags, budgets */

void br_profiler_set_open(br_engine *e, bool open)
{
    br_profiler_read_args(e, e->profiler); /* code wins over the command line from here on */
    e->profiler->open = open;
}

bool br_profiler_is_open(br_engine *e)
{
    if (e->profiler == NULL) {
        return false;
    }
    br_profiler_read_args(e, e->profiler);
    return e->profiler->open;
}

void br_profiler_set_hud(br_engine *e, bool on)
{
    br_profiler_read_args(e, e->profiler);
    e->profiler->hud = on;
}

bool br_profiler_hud(br_engine *e)
{
    if (e->profiler == NULL) {
        return false;
    }
    br_profiler_read_args(e, e->profiler);
    return e->profiler->hud;
}
void br_profiler_set_paused(br_engine *e, bool paused) { e->profiler->paused = paused; }
bool br_profiler_paused(br_engine *e) { return e->profiler != NULL && e->profiler->paused; }

void br_profiler_set_budget(br_engine *e, const char *name, float ms)
{
    br_profiler_state *p = e->profiler;
    uint16_t id = br_profiler_name_id_text(p, name != NULL ? name : "(null)");
    for (uint32_t i = 0; i < p->budget_count; i++) {
        if (p->budgets[i].name == id) {
            if (ms <= 0.0f) {
                p->budgets[i] = p->budgets[p->budget_count - 1];
                p->budget_count -= 1;
            } else {
                p->budgets[i].ms = ms;
            }
            return;
        }
    }
    if (ms <= 0.0f) {
        return;
    }
    if (p->budget_count >= BR_PROFILER_MAX_BUDGETS) {
        br_log_error("br_profiler_set_budget: at most %d budgets", BR_PROFILER_MAX_BUDGETS);
        return;
    }
    br_profiler_budget *b = &p->budgets[p->budget_count++];
    SDL_zerop(b);
    b->name = id;
    b->ms = ms;
}

/* ---- module */

static void br_profiler_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_profiler_state *p = e->profiler;
    br_profiler_save_at_exit(e);
    br_profiler_gpu_stop(e);
    br_free(p->save_path);
    br_free(p->scopes);
    br_free(p->frames);
    br_free(p);
    e->profiler = NULL;
}

void br_profiler_register(br_engine *e)
{
    br_profiler_state *p = br_alloc(sizeof(br_profiler_state));
    if (p == NULL) {
        return;
    }
    p->frames = br_alloc(sizeof(br_profiler_frame) * BR_PROFILER_FRAMES);
    p->scopes = br_alloc(sizeof(br_profiler_scope) * BR_PROFILER_FRAMES * BR_PROFILER_MAX_SCOPES);
    if (p->frames == NULL || p->scopes == NULL) {
        br_free(p->frames);
        br_free(p->scopes);
        br_free(p);
        return;
    }
    e->profiler = p;
    p->clock = br_profiler_default_clock;
    br_str_copy(p->names[0], BR_PROFILER_NAME_SIZE, "(overflow)");
    p->name_count = 1;
    p->paths[0].name = 0;
    p->paths[0].parent = BR_PROFILER_NONE;
    p->path_count = 1;
    /* the engine's top-level frame steps (br_engine_step) */
    p->name_update = br_profiler_name_id(p, "update");
    p->name_prepare = br_profiler_name_id(p, "render.prepare");
    p->name_render = br_profiler_name_id(p, "render");
    p->name_acquire = br_profiler_name_id(p, "gpu.acquire");
    p->name_present = br_profiler_name_id(p, "gpu.present");
    p->name_pace = br_profiler_name_id(p, "pace");
    p->main_thread = SDL_GetCurrentThreadID();
    br_engine_on_shutdown(e, br_profiler_shutdown, NULL);
}
