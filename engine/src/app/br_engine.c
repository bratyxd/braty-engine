/* br_engine: lifetime, window, systems and phases, ticker, event and shutdown hooks. */
#include "app/br_engine_internal.h"
#include "gpu/br_gpu_internal.h"

/* ---- lifetime */

static char *br_resolve_asset_root(const char *asset_root)
{
    const char *base = SDL_GetBasePath(); /* owned by SDL, ends with a separator */
    char path[1024];
    bool ok;
    if (asset_root != NULL && br_path_is_absolute(asset_root)) {
        ok = br_str_copy(path, sizeof(path), asset_root);
    } else if (asset_root != NULL && asset_root[0] != 0) {
        ok = br_path_join(path, sizeof(path), base != NULL ? base : "", asset_root);
    } else {
        ok = br_str_copy(path, sizeof(path), base != NULL ? base : ".");
    }
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
    if (e->title == NULL || e->asset_root == NULL) {
        br_free(e->title);
        br_free(e->asset_root);
        br_free(e);
        return NULL;
    }
    br_array_init(&e->systems, sizeof(br_system_entry));
    br_array_init(&e->pending_systems, sizeof(br_system_entry));
    br_array_init(&e->event_handlers, sizeof(br_event_handler_entry));
    br_array_init(&e->shutdown_handlers, sizeof(br_shutdown_entry));
    e->ticker.speed = 1.0f;
    e->ticker.min_fps = 10.0f;
    br_builtin_register(e);
    return e;
}

static void br_free_system_names(br_array *systems)
{
    for (uint32_t i = 0; i < systems->count; i++) {
        br_system_entry *s = br_array_at(systems, i);
        br_free(s->name);
    }
    br_array_free(systems);
}

void br_engine_destroy(br_engine *e)
{
    if (e == NULL) {
        return;
    }
    for (uint32_t i = e->shutdown_handlers.count; i > 0; i--) {
        br_shutdown_entry *h = br_array_at(&e->shutdown_handlers, i - 1);
        h->fn(e, h->user);
    }
    br_array_free(&e->shutdown_handlers);
    br_free_system_names(&e->systems);
    br_free_system_names(&e->pending_systems);
    br_array_free(&e->event_handlers);
    br_gpu_destroy(e->gpu);
    if (e->window != NULL) {
        SDL_DestroyWindow(e->window);
    }
    if (e->save_storage != NULL) {
        SDL_CloseStorage(e->save_storage);
    }
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

/* ---- systems */

static br_system_entry *br_system_find(br_array *systems, const char *name)
{
    for (uint32_t i = 0; i < systems->count; i++) {
        br_system_entry *s = br_array_at(systems, i);
        if (SDL_strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

/* Inserts after every entry that sorts before or equal, so equal keys keep adding order. */
static void br_system_insert_sorted(br_array *systems, const br_system_entry *entry)
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
    br_array_insert(systems, index, entry);
}

void br_system_add(br_engine *e, const char *name, br_phase phase, int priority, br_system_fn fn, void *user)
{
    if (name == NULL || fn == NULL || phase < 0 || phase >= BR_PHASE_COUNT) {
        br_log_error("br_system_add: invalid arguments");
        return;
    }
    if (br_system_find(&e->systems, name) != NULL || br_system_find(&e->pending_systems, name) != NULL) {
        br_log_error("system '%s' already exists", name);
        return;
    }
    br_system_entry entry;
    entry.name = br_strdup(name);
    entry.phase = phase;
    entry.priority = priority;
    entry.fn = fn;
    entry.user = user;
    entry.enabled = true;
    entry.last_time_ns = 0;
    if (e->stepping) {
        /* the system list must not shift under the running loop; merged after the frame */
        br_array_push(&e->pending_systems, &entry);
    } else {
        br_system_insert_sorted(&e->systems, &entry);
    }
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
        if (s->phase < first || s->phase > last || !s->enabled) {
            continue;
        }
        uint64_t start = SDL_GetTicksNS();
        s->fn(e, dt, s->user);
        s = br_array_at(&e->systems, i); /* re-fetch: nothing should move it, but stay safe */
        s->last_time_ns = SDL_GetTicksNS() - start;
    }
}

bool br_engine_step(br_engine *e, float dt_seconds)
{
    e->ticker.delta = dt_seconds;
    e->ticker.elapsed += dt_seconds;
    e->ticker.frame += 1;
    if (e->gpu != NULL) {
        br_gpu_transient_reset(e->gpu);
    }
    e->stepping = true;
    br_run_phases(e, BR_PHASE_INPUT, BR_PHASE_PRE_RENDER, dt_seconds);
    bool presented = false;
    if (e->gpu != NULL) {
        if (br_gpu_frame_begin(e->gpu)) {
            br_run_phases(e, BR_PHASE_RENDER, BR_PHASE_POST_RENDER, dt_seconds);
            br_gpu_frame_end(e->gpu);
            presented = true;
        } else {
            e->skipped_frames += 1;
        }
    }
    e->stepping = false;
    for (uint32_t i = 0; i < e->pending_systems.count; i++) {
        br_system_insert_sorted(&e->systems, br_array_at(&e->pending_systems, i));
    }
    br_array_clear(&e->pending_systems);
    return presented;
}

/* ---- ticker */

float br_ticker_delta(br_engine *e) { return e->ticker.delta; }
double br_ticker_elapsed(br_engine *e) { return e->ticker.elapsed; }
uint64_t br_ticker_frame(br_engine *e) { return e->ticker.frame; }
float br_ticker_fps(br_engine *e) { return e->ticker.fps; }
void br_ticker_set_speed(br_engine *e, float speed) { e->ticker.speed = speed; }
void br_ticker_set_min_fps(br_engine *e, float min_fps) { e->ticker.min_fps = min_fps; }

void br_ticker_set_max_fps(br_engine *e, float max_fps)
{
    e->ticker.max_fps = max_fps;
    char rate[32];
    SDL_snprintf(rate, sizeof(rate), "%g", max_fps > 0.0f ? (double)max_fps : 0.0);
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, rate);
}

float br_ticker_compute_delta(float raw_seconds, float min_fps, float speed)
{
    float dt = raw_seconds > 0.0f ? raw_seconds : 0.0f;
    if (min_fps > 0.0f && dt > 1.0f / min_fps) {
        dt = 1.0f / min_fps;
    }
    return dt * speed;
}
