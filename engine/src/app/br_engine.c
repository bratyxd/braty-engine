/* br_engine: lifetime, window, systems and phases, ticker, event and shutdown hooks. */
#include "app/br_engine_internal.h"
#include "gpu/br_gpu_internal.h"
#include "braty/br_job.h"
#include "braty/br_profiler.h"
#include "scene/br_scene_internal.h"
#include "three_d/br_three_d_internal.h"

/* ---- lifetime */

static char *br_resolve_asset_root(const char *asset_root)
{
    char path[1024];
    /* relative to the executable's folder (the storage layer's path table, br_core.h) */
    bool ok = br_file_resolve_exe(asset_root != NULL && asset_root[0] != 0 ? asset_root : ".", path, sizeof(path));
    if (!ok) {
        br_log_error("asset root path too long");
        return NULL;
    }
    size_t len = SDL_strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        path[len - 1] = 0;
        len--;
    }
    return br_strdup(path);
}

br_engine *br_engine_create(const char *title, const char *asset_root)
{
    br_engine *e = br_alloc(sizeof(br_engine));
    if (e == NULL) {
        return NULL;
    }
    e->title = br_strdup(title != NULL ? title : "braty");
    e->asset_root = br_resolve_asset_root(asset_root);
    e->save_org = br_strdup("braty");
    e->save_app = br_save_name_sanitize(e->title);
    if (e->title == NULL || e->asset_root == NULL || e->save_org == NULL || e->save_app == NULL) {
        br_free(e->title);
        br_free(e->asset_root);
        br_free(e->save_org);
        br_free(e->save_app);
        br_free(e);
        return NULL;
    }
    br_array_init(&e->systems, sizeof(br_system_entry));
    br_array_init(&e->pending_systems, sizeof(br_system_entry));
    br_map_init(&e->retired_system_names, sizeof(char *));
    br_array_init(&e->disabled_groups, sizeof(char *));
    br_array_init(&e->event_handlers, sizeof(br_event_handler_entry));
    br_array_init(&e->shutdown_handlers, sizeof(br_shutdown_entry));
    br_array_init(&e->lifecycle_handlers, sizeof(br_lifecycle_entry));
    e->pause_when_inactive = true;
    e->ticker.speed = 1.0f;
    e->ticker.min_fps = 10.0f;
    e->vsync = BR_VSYNC_ON;
    br_random_seed(&e->rng, SDL_GetPerformanceCounter(), 0);
    br_builtin_register(e);
    return e;
}

static void br_free_system_names(br_array *systems)
{
    for (uint32_t i = 0; i < systems->count; i++) {
        br_system_entry *s = br_array_at(systems, i);
        br_free(s->name);
        br_free(s->group);
    }
    br_array_free(systems);
}

void br_engine_destroy_nodes(br_engine *e)
{
    /* 3D first: a canvas3d's on_destroy destroys its 2D root */
    br_three_d_shutdown_nodes(e, NULL);
    br_scene_shutdown_nodes(e, NULL);
}

void br_engine_destroy(br_engine *e)
{
    if (e == NULL) {
        return;
    }
    br_job_cancel_all(e); /* before any shutdown callback frees memory a job works on */
    /* before any shutdown callback: node destroy listeners still see live module and game state */
    br_engine_destroy_nodes(e);
    for (uint32_t i = e->shutdown_handlers.count; i > 0; i--) {
        br_shutdown_entry *h = br_array_at(&e->shutdown_handlers, i - 1);
        h->fn(e, h->user);
    }
    br_array_free(&e->shutdown_handlers);
    br_free_system_names(&e->systems);
    br_free_system_names(&e->pending_systems);
    uint32_t slot = 0;
    void *retired = NULL;
    while (br_map_next(&e->retired_system_names, &slot, NULL, &retired)) {
        br_free(*(char **)retired);
    }
    br_map_free(&e->retired_system_names);
    for (uint32_t i = 0; i < e->disabled_groups.count; i++) {
        br_free(*(char **)br_array_at(&e->disabled_groups, i));
    }
    br_array_free(&e->disabled_groups);
    br_array_free(&e->event_handlers);
    br_array_free(&e->lifecycle_handlers);
    br_gpu_destroy(e->gpu);
    if (e->window != NULL) {
        SDL_DestroyWindow(e->window);
    }
    if (e->save_storage != NULL) {
        SDL_CloseStorage(e->save_storage);
    }
    br_free(e->save_org);
    br_free(e->save_app);
    br_free(e->screenshot_path);
    br_free(e->asset_root);
    br_free(e->title);
    br_free(e);
}

bool br_engine_open_window(br_engine *e, int width, int height, const char *gpu_driver)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        br_log_error("cannot init SDL video: %s", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    e->window = SDL_CreateWindow(e->title, width, height, flags);
    if (e->window == NULL) {
        br_log_error("cannot create window: %s", SDL_GetError());
        return false;
    }
    e->gpu = br_gpu_create(e->window, gpu_driver);
    if (e->gpu != NULL && e->vsync != BR_VSYNC_ON) {
        br_app_set_vsync(e, e->vsync); /* chosen before the window existed */
    }
    return e->gpu != NULL;
}

void br_engine_quit(br_engine *e)
{
    e->quit_requested = true;
}

br_gpu *br_engine_gpu(br_engine *e)
{
    return e->gpu;
}

SDL_Window *br_engine_window(br_engine *e)
{
    return e->window;
}

void br_engine_window_size(br_engine *e, int *width, int *height)
{
    *width = 0;
    *height = 0;
    if (e->window != NULL) {
        SDL_GetWindowSize(e->window, width, height);
    }
}

void br_engine_pixel_size(br_engine *e, int *width, int *height)
{
    *width = 0;
    *height = 0;
    if (e->window != NULL) {
        SDL_GetWindowSizeInPixels(e->window, width, height);
    }
}

float br_engine_pixel_density(br_engine *e)
{
    if (e->window == NULL) {
        return 1.0f;
    }
    float density = SDL_GetWindowPixelDensity(e->window);
    return density > 0.0f ? density : 1.0f;
}

const char *br_engine_asset_root(br_engine *e)
{
    return e->asset_root;
}

uint64_t br_engine_skipped_frames(br_engine *e)
{
    return e->skipped_frames;
}

bool br_app_deterministic(br_engine *e)
{
    return e->deterministic;
}

void br_app_set_deterministic(br_engine *e, bool on, uint64_t seed)
{
    e->deterministic = on;
    br_random_seed(&e->rng, seed, 0);
}

void *br_engine_app_user(br_engine *e)
{
    return e->app_user;
}

char **br_engine_args(br_engine *e, int *argc)
{
    if (argc != NULL) {
        *argc = e->argc;
    }
    return e->argv;
}

void br_engine_add_event_handler(br_engine *e, br_event_fn fn, void *user)
{
    br_event_handler_entry entry = { fn, user };
    br_array_push(&e->event_handlers, &entry);
}

void br_engine_on_shutdown(br_engine *e, br_shutdown_fn fn, void *user)
{
    br_shutdown_entry entry = { fn, user };
    br_array_push(&e->shutdown_handlers, &entry);
}

/* ---- add-on registry */

/* Index of the add-on called name, -1 when none. The same literal is usually the same pointer,
 * which skips the string compare. */
static int br_addon_find(br_engine *e, const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < e->addon_count; i++) {
        if (e->addon_names[i] == name || SDL_strcmp(e->addon_names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool br_addon_add(br_engine *e, const char *name, void *state)
{
    if (name == NULL) {
        br_log_error("br_addon_add: name is NULL");
        return false;
    }
    if (br_addon_find(e, name) >= 0) {
        br_log_error("add-on '%s' is already registered", name);
        return false;
    }
    if (e->addon_count >= BR_ADDON_MAX) {
        br_log_error("add-on '%s': registry full (BR_ADDON_MAX %d)", name, BR_ADDON_MAX);
        return false;
    }
    e->addon_names[e->addon_count] = name;
    e->addon_states[e->addon_count] = state;
    e->addon_count++;
    return true;
}

void *br_addon_get(br_engine *e, const char *name)
{
    int i = br_addon_find(e, name);
    return i >= 0 ? e->addon_states[i] : NULL;
}

uint32_t br_addon_count(br_engine *e)
{
    return e->addon_count;
}

const char *br_addon_name_at(br_engine *e, uint32_t index)
{
    return index < e->addon_count ? e->addon_names[index] : NULL;
}

/* ---- systems */

static br_system_entry *br_system_find(br_array *systems, const char *name)
{
    for (uint32_t i = 0; i < systems->count; i++) {
        br_system_entry *s = br_array_at(systems, i);
        if (!s->removed && SDL_strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

/* A copy of name for a new system. A removed system's name is kept and handed back when the
 * name is added again: the profiler caches scope names by pointer, so a freed name whose
 * address a different system's name reused would be timed under the old name. */
static char *br_system_name_copy(br_engine *e, const char *name)
{
    char **old = br_map_get(&e->retired_system_names, name);
    if (old != NULL) {
        char *copy = *old;
        br_map_remove(&e->retired_system_names, name);
        return copy;
    }
    return br_strdup(name);
}

/* Drops entry i of systems: its name is retired, its group freed. */
static void br_system_drop(br_engine *e, br_array *systems, uint32_t i)
{
    br_system_entry *s = br_array_at(systems, i);
    // ponytail: unique names removed and never re-added stay here until destroy; key profiler scopes by content if that grows.
    if (br_map_set(&e->retired_system_names, s->name, &s->name) == NULL) {
        br_free(s->name);
    }
    br_free(s->group);
    br_array_remove_ordered(systems, i);
}

void br_system_remove(br_engine *e, const char *name)
{
    for (uint32_t i = 0; name != NULL && i < e->pending_systems.count; i++) {
        br_system_entry *s = br_array_at(&e->pending_systems, i);
        if (SDL_strcmp(s->name, name) == 0) {
            br_system_drop(e, &e->pending_systems, i); /* not in the running list yet */
            return;
        }
    }
    for (uint32_t i = 0; name != NULL && i < e->systems.count; i++) {
        br_system_entry *s = br_array_at(&e->systems, i);
        if (s->removed || SDL_strcmp(s->name, name) != 0) {
            continue;
        }
        if (e->stepping) {
            s->removed = true; /* the running loop must not shift: skipped now, purged after the frame */
            s->enabled = false;
        } else {
            br_system_drop(e, &e->systems, i);
        }
        return;
    }
    br_log_warn("br_system_remove: no system '%s'", name != NULL ? name : "(null)");
}

/* Inserts after every entry that sorts before or equal, so equal keys keep adding order. */
static bool br_system_insert_sorted(br_array *systems, const br_system_entry *entry)
{
    uint32_t index = 0;
    while (index < systems->count) {
        br_system_entry *s = br_array_at(systems, index);
        bool after = s->phase > entry->phase || (s->phase == entry->phase && s->priority > entry->priority);
        if (after) {
            break;
        }
        index++;
    }
    return br_array_insert(systems, index, entry) != NULL;
}

bool br_system_add(br_engine *e, const char *name, br_phase phase, int priority, br_system_fn fn, void *user)
{
    if (name == NULL || fn == NULL || phase < 0 || phase >= BR_PHASE_COUNT) {
        br_log_error("br_system_add: invalid arguments");
        return false;
    }
    if (br_system_find(&e->systems, name) != NULL || br_system_find(&e->pending_systems, name) != NULL) {
        br_log_error("system '%s' already exists", name);
        return false;
    }
    br_system_entry entry;
    entry.name = br_system_name_copy(e, name);
    if (entry.name == NULL) {
        return false;
    }
    entry.phase = phase;
    entry.priority = priority;
    entry.fn = fn;
    entry.user = user;
    entry.enabled = true;
    entry.group = NULL;
    entry.group_enabled = true;
    entry.last_time_ns = 0;
    entry.removed = false;
    bool ok;
    if (e->stepping) {
        /* the system list must not shift under the running loop; merged after the frame */
        ok = br_array_push(&e->pending_systems, &entry) != NULL;
    } else {
        ok = br_system_insert_sorted(&e->systems, &entry);
    }
    if (!ok) {
        br_free(entry.name);
    }
    return ok;
}

void br_system_set_enabled(br_engine *e, const char *name, bool enabled)
{
    br_system_entry *s = br_system_find(&e->systems, name);
    if (s == NULL) {
        s = br_system_find(&e->pending_systems, name);
    }
    if (s == NULL) {
        br_log_warn("br_system_set_enabled: no system '%s'", name);
        return;
    }
    s->enabled = enabled;
}

/* Index of a group in disabled_groups, -1 if it is enabled. */
static int br_group_disabled_index(br_engine *e, const char *group)
{
    for (uint32_t i = 0; i < e->disabled_groups.count; i++) {
        if (SDL_strcmp(*(char **)br_array_at(&e->disabled_groups, i), group) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void br_system_set_group(br_engine *e, const char *name, const char *group)
{
    br_system_entry *s = br_system_find(&e->systems, name);
    if (s == NULL) {
        s = br_system_find(&e->pending_systems, name);
    }
    if (s == NULL) {
        br_log_warn("br_system_set_group: no system '%s'", name);
        return;
    }
    br_free(s->group);
    s->group = br_strdup(group);
    s->group_enabled = group == NULL || br_group_disabled_index(e, group) < 0;
}

static void br_group_update(br_array *systems, const char *group, bool enabled)
{
    for (uint32_t i = 0; i < systems->count; i++) {
        br_system_entry *s = br_array_at(systems, i);
        if (s->group != NULL && SDL_strcmp(s->group, group) == 0) {
            s->group_enabled = enabled;
        }
    }
}

void br_system_group_set_enabled(br_engine *e, const char *group, bool enabled)
{
    if (group == NULL) {
        return;
    }
    int index = br_group_disabled_index(e, group);
    if (enabled && index >= 0) {
        br_free(*(char **)br_array_at(&e->disabled_groups, (uint32_t)index));
        br_array_remove_swap(&e->disabled_groups, (uint32_t)index);
    } else if (!enabled && index < 0) {
        char *copy = br_strdup(group);
        br_array_push(&e->disabled_groups, &copy);
    }
    br_group_update(&e->systems, group, enabled);
    br_group_update(&e->pending_systems, group, enabled);
}

uint32_t br_system_count(br_engine *e)
{
    return e->systems.count;
}

bool br_system_get_info(br_engine *e, uint32_t index, br_system_info *out)
{
    br_system_entry *s = br_array_at(&e->systems, index);
    if (s == NULL) {
        return false;
    }
    out->name = s->name;
    out->phase = s->phase;
    out->priority = s->priority;
    out->enabled = s->enabled;
    out->last_time_ns = s->last_time_ns;
    return true;
}

static void br_run_phases(br_engine *e, br_phase first, br_phase last, float dt)
{
    for (uint32_t i = 0; i < e->systems.count; i++) {
        br_system_entry *s = br_array_at(&e->systems, i);
        if (s->phase < first || s->phase > last || !s->enabled || !s->group_enabled) {
            continue;
        }
        uint64_t start = SDL_GetTicksNS();
        br_profile_begin(e, s->name); /* every system is a profiler scope */
        s->fn(e, dt, s->user);
        br_profile_end(e);
        s = br_array_at(&e->systems, i); /* re-fetch: nothing should move it, but stay safe */
        s->last_time_ns = SDL_GetTicksNS() - start;
    }
}

/* BR_PHASE_FIXED_UPDATE: 0..BR_FIXED_MAX_STEPS runs of 1/hz each, or once with dt when off. */
static void br_run_fixed_update(br_engine *e, float dt)
{
    br_ticker_state *t = &e->ticker;
    if (t->fixed_hz <= 0.0f) {
        t->fixed_steps = 1;
        t->fixed_alpha = 0.0f;
        br_run_phases(e, BR_PHASE_FIXED_UPDATE, BR_PHASE_FIXED_UPDATE, dt);
        return;
    }
    double step = 1.0 / (double)t->fixed_hz;
    /* dt is a float: 1/50 s arrives as 0.0199999996. The tolerance keeps such a frame at one
     * step instead of alternating 0 and 2. */
    double tolerance = 1e-6;
    t->fixed_accumulator += (double)dt;
    t->fixed_steps = 0;
    while (t->fixed_accumulator + tolerance >= step && t->fixed_steps < BR_FIXED_MAX_STEPS) {
        br_run_phases(e, BR_PHASE_FIXED_UPDATE, BR_PHASE_FIXED_UPDATE, (float)step);
        t->fixed_accumulator -= step;
        t->fixed_steps++;
    }
    if (t->fixed_accumulator + tolerance >= step) {
        /* still behind after the maximum: drop the backlog (no spiral of death) */
        t->fixed_accumulator = SDL_fmod(t->fixed_accumulator, step);
    }
    if (t->fixed_accumulator < 0.0) {
        t->fixed_accumulator = 0.0;
    }
    t->fixed_alpha = (float)(t->fixed_accumulator / step);
}

bool br_engine_step(br_engine *e, float dt_seconds)
{
    return br_engine_step_frame(e, dt_seconds, dt_seconds);
}

bool br_engine_step_frame(br_engine *e, float dt_seconds, float unscaled_seconds)
{
    e->ticker.delta = dt_seconds;
    e->ticker.unscaled_delta = unscaled_seconds;
    e->ticker.elapsed += dt_seconds;
    e->ticker.frame += 1;
    if (e->gpu != NULL) {
        br_gpu_transient_reset(e->gpu);
    }
    e->stepping = true;
    /* profiler: the top-level scopes are the frame's steps (docs/PROFILING.md) */
    br_profiler_frame_begin(e); /* no-op when br_app_iterate already began it */
    br_profile_begin(e, "update");
    br_run_phases(e, BR_PHASE_INPUT, BR_PHASE_INPUT, dt_seconds);
    br_run_fixed_update(e, dt_seconds);
    br_run_phases(e, BR_PHASE_UPDATE, BR_PHASE_LATE_UPDATE, dt_seconds);
    br_profile_end(e);
    br_profile_begin(e, "render.prepare");
    br_run_phases(e, BR_PHASE_PRE_RENDER, BR_PHASE_PRE_RENDER, dt_seconds);
    br_profile_end(e);
    bool presented = false;
    if (e->gpu != NULL) {
        br_profile_begin(e, "gpu.acquire");
        bool acquired = br_gpu_frame_begin(e->gpu);
        br_profile_end(e);
        if (acquired) {
            br_profile_begin(e, "render");
            br_run_phases(e, BR_PHASE_RENDER, BR_PHASE_POST_RENDER, dt_seconds);
            br_profile_end(e);
            br_profile_begin(e, "gpu.present");
            br_gpu_frame_end(e->gpu);
            br_profile_end(e);
            presented = true;
        } else {
            e->skipped_frames += 1;
        }
    }
    br_profiler_frame_end(e);
    e->stepping = false;
    for (uint32_t i = e->systems.count; i > 0; i--) {
        br_system_entry *s = br_array_at(&e->systems, i - 1);
        if (s->removed) {
            br_system_drop(e, &e->systems, i - 1);
        }
    }
    for (uint32_t i = 0; i < e->pending_systems.count; i++) {
        br_system_insert_sorted(&e->systems, br_array_at(&e->pending_systems, i));
    }
    br_array_clear(&e->pending_systems);
    return presented;
}

/* ---- ticker */

float br_ticker_delta(br_engine *e) { return e->ticker.delta; }
float br_ticker_unscaled_delta(br_engine *e) { return e->ticker.unscaled_delta; }
double br_ticker_elapsed(br_engine *e) { return e->ticker.elapsed; }
uint64_t br_ticker_frame(br_engine *e) { return e->ticker.frame; }
float br_ticker_fps(br_engine *e) { return e->ticker.fps; }
/* negative or NaN speed would run time backwards (or poison every dt): clamp to 0 = paused */
void br_ticker_set_speed(br_engine *e, float speed) { e->ticker.speed = speed > 0.0f ? speed : 0.0f; }
float br_ticker_speed(br_engine *e) { return e->ticker.speed; }
void br_ticker_set_min_fps(br_engine *e, float min_fps) { e->ticker.min_fps = min_fps > 0.0f ? min_fps : 0.0f; }

float br_ticker_compute_delta(float raw_seconds, float min_fps, float speed)
{
    float dt = raw_seconds > 0.0f ? raw_seconds : 0.0f;
    if (min_fps > 0.0f && dt > 1.0f / min_fps) {
        dt = 1.0f / min_fps;
    }
    return speed > 0.0f ? dt * speed : 0.0f;
}
