/* SDL main-callback glue: command line, the per-frame tick, --frames / --screenshot. */
#include "app/br_engine_internal.h"
#include "data/br_data_internal.h"
#include "playtest/br_playtest_internal.h"
#include "braty/br_audio.h"
#include "braty/br_profiler.h"

#include "stb/stb_image_write.h"

typedef struct br_app_args {
    uint64_t    frames;
    const char *screenshot;
    const char *gpu_driver;
    bool        headless;
    const char *assets;   /* --assets DIR or $BRATY_ASSETS, NULL if neither */
    const char *vsync;    /* --vsync MODE, NULL = game.json / default */
    const char *max_fps;  /* --max-fps N, NULL = game.json / default */
    bool        deterministic;
    uint64_t    seed;     /* --seed N, 1 by default */
    const char *playtest; /* --playtest FILE */
    bool        playtest_stdin;
    const char *record;   /* --record FILE */
} br_app_args;

static void br_app_parse_args(int argc, char **argv, br_app_args *out)
{
    SDL_zerop(out);
    out->seed = 1;
    out->assets = SDL_getenv("BRATY_ASSETS");
    for (int i = 1; i < argc; i++) {
        bool has_value = i + 1 < argc;
        if (SDL_strcmp(argv[i], "--frames") == 0 && has_value) {
            out->frames = SDL_strtoull(argv[++i], NULL, 10);
        } else if (SDL_strcmp(argv[i], "--screenshot") == 0 && has_value) {
            out->screenshot = argv[++i];
        } else if (SDL_strcmp(argv[i], "--gpu-driver") == 0 && has_value) {
            out->gpu_driver = argv[++i];
        } else if (SDL_strcmp(argv[i], "--headless") == 0) {
            out->headless = true;
        } else if (SDL_strcmp(argv[i], "--assets") == 0 && has_value) {
            out->assets = argv[++i];
        } else if (SDL_strcmp(argv[i], "--vsync") == 0 && has_value) {
            out->vsync = argv[++i];
        } else if (SDL_strcmp(argv[i], "--max-fps") == 0 && has_value) {
            out->max_fps = argv[++i];
        } else if (SDL_strcmp(argv[i], "--deterministic") == 0) {
            out->deterministic = true;
        } else if (SDL_strcmp(argv[i], "--seed") == 0 && has_value) {
            out->seed = SDL_strtoull(argv[++i], NULL, 10);
        } else if (SDL_strcmp(argv[i], "--playtest") == 0 && has_value) {
            out->playtest = argv[++i];
        } else if (SDL_strcmp(argv[i], "--playtest-stdin") == 0) {
            out->playtest_stdin = true;
        } else if (SDL_strcmp(argv[i], "--record") == 0 && has_value) {
            out->record = argv[++i];
        }
        /* anything else belongs to the game (it sees argv in br_main) */
    }
    if (out->playtest != NULL || out->playtest_stdin) {
        out->deterministic = true; /* play-tests run on deterministic frames */
    }
    if (out->screenshot != NULL && out->frames == 0) {
        out->frames = 1;
    }
}

static void br_png_write_callback(void *context, void *data, int size)
{
    SDL_IOStream *io = context;
    SDL_WriteIO(io, data, (size_t)size);
}

/* Averages each n x n block of RGBA pixels into one, in place. Returns the new size. */
static void br_app_downsample(uint8_t *pixels, uint32_t *width, uint32_t *height, uint32_t n)
{
    uint32_t w = *width / n;
    uint32_t h = *height / n;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sum[4] = { 0, 0, 0, 0 };
            for (uint32_t dy = 0; dy < n; dy++) {
                const uint8_t *row = pixels + ((size_t)(y * n + dy) * *width + x * n) * 4;
                for (uint32_t dx = 0; dx < n * 4; dx++) {
                    sum[dx % 4] += row[dx];
                }
            }
            /* the output index is never ahead of the blocks still to be read */
            uint8_t *out = pixels + ((size_t)y * w + x) * 4;
            for (uint32_t c = 0; c < 4; c++) {
                out[c] = (uint8_t)((sum[c] + n * n / 2) / (n * n));
            }
        }
    }
    *width = w;
    *height = h;
}

bool br_engine_screenshot(br_engine *e, const char *path)
{
    return br_engine_screenshot_downsampled(e, path, 1);
}

bool br_engine_screenshot_downsampled(br_engine *e, const char *path, uint32_t downsample)
{
    if (e->gpu == NULL) {
        br_log_error("screenshot: headless, nothing is rendered");
        return false;
    }
    br_render_target *rt = br_gpu_frame_target(e->gpu);
    if (rt == NULL) {
        br_log_error("screenshot: no frame was rendered");
        return false;
    }
    uint8_t *pixels = NULL;
    if (!br_gpu_texture_read(e->gpu, rt->color, &pixels)) {
        return false;
    }
    /* The window is opaque: force alpha to 255 so viewers do not show a see-through image. */
    for (uint32_t i = 0; i < rt->width * rt->height; i++) {
        pixels[i * 4 + 3] = 255;
    }
    uint32_t width = rt->width;
    uint32_t height = rt->height;
    if (downsample > 1 && width >= downsample && height >= downsample) {
        br_app_downsample(pixels, &width, &height, downsample);
    }
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    bool ok = false;
    if (io == NULL) {
        br_log_error("screenshot: cannot open '%s': %s", path, SDL_GetError());
    } else {
        ok = stbi_write_png_to_func(br_png_write_callback, io, (int)width, (int)height, 4, pixels, (int)width * 4) != 0;
        ok = SDL_CloseIO(io) && ok;
        if (ok) {
            br_log_info("screenshot: wrote %ux%u to '%s'", width, height, path);
        } else {
            br_log_error("screenshot: cannot write '%s'", path);
        }
    }
    br_free(pixels);
    return ok;
}

/* Window, game.json (desc->config), the game's init, the start scene. */
static bool br_app_start(br_engine *e, const br_app_desc *desc, const br_app_args *args)
{
    int width = desc->width > 0 ? desc->width : 1280;
    int height = desc->height > 0 ? desc->height : 720;
    if (!args->headless && !br_engine_open_window(e, width, height, args->gpu_driver)) {
        return false;
    }
    if (desc->config != NULL) {
        br_game_config_apply_settings(e, desc->config);
    }
    if (desc->init != NULL && !desc->init(e)) {
        br_log_error("game init failed");
        return false;
    }
    /* set only after a successful init, so a failed init does not run the game's shutdown */
    e->game_shutdown = desc->shutdown;
    /* the command line overrides game.json and whatever init chose */
    if (args->vsync != NULL) {
        br_vsync mode;
        if (br_vsync_from_name(args->vsync, &mode)) {
            br_app_set_vsync(e, mode);
        } else {
            br_log_error("--vsync: expected off, on, adaptive or mailbox, got '%s'", args->vsync);
        }
    }
    if (args->max_fps != NULL) {
        br_app_set_max_fps(e, (float)SDL_atof(args->max_fps));
    }
    bool playtest = args->playtest != NULL || args->playtest_stdin;
    if (playtest && args->vsync == NULL) {
        br_app_set_vsync(e, BR_VSYNC_OFF); /* deterministic frames: no reason to wait for the display */
    }
    if (args->record != NULL && !br_playtest_record_start(e, args->record)) {
        return false;
    }
    if (args->playtest != NULL && !br_playtest_load_script(e, args->playtest)) {
        return false; /* the reason (file, step number) is logged */
    }
    e->playtest_stdin = args->playtest_stdin;
    if (desc->config != NULL && desc->config->start_scene[0] != '\0') {
        /* after init: the scene may use components the game registers there */
        if (!br_node_valid(e, br_game_config_load_start_scene(e, desc->config))) {
            return false;
        }
    }
    return true;
}

SDL_AppResult br_app_init(void **appstate, const br_app_desc *desc, int argc, char **argv)
{
    br_app_args args;
    br_app_parse_args(argc, argv, &args);
    const char *title = desc->title != NULL ? desc->title : "braty";
    const char *asset_root = desc->asset_root;
    char override_root[1024];
    if (args.assets != NULL && args.assets[0] != '\0') {
        /* relative to the working directory (a shell path), unlike asset_root (exe-relative) */
        char *cwd = SDL_GetCurrentDirectory();
        bool ok = br_path_is_absolute(args.assets) || cwd == NULL
                      ? br_str_copy(override_root, sizeof(override_root), args.assets)
                      : br_path_join(override_root, sizeof(override_root), cwd, args.assets);
        SDL_free(cwd);
        if (ok) {
            asset_root = override_root;
            br_log_info("asset root overridden: %s", override_root);
        } else {
            br_log_error("--assets / BRATY_ASSETS: path too long, ignored");
        }
    }
    br_engine *e = br_engine_create(title, asset_root);
    bool ok = e != NULL;
    if (ok) {
        *appstate = e;
        e->frame_limit = args.frames;
        e->screenshot_path = br_strdup(args.screenshot);
        e->app_user = desc->user;
        e->argc = argc;
        e->argv = argv;
        br_engine_set_save_name(e, desc->save_org, desc->save_app);
        e->pause_when_inactive = !desc->run_when_inactive;
        if (args.deterministic) {
            br_app_set_deterministic(e, true, args.seed); /* before init: the game may draw numbers there */
        }
        ok = br_app_start(e, desc, &args);
    }
    if (desc->config != NULL) {
        br_game_config_free(desc->config);
        br_free(desc->config);
    }
    return ok ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult br_app_iterate(void *appstate)
{
    br_engine *e = appstate;
    if (e->playtest_stdin) {
        return br_playtest_stdin_iterate(e); /* frames advance only on "step" commands */
    }
    br_profiler_frame_begin(e); /* the frame starts before the FPS-cap wait, so the wait shows */
    br_profile_begin(e, "pace");
    br_app_pace_frame(e);
    br_profile_end(e);
    uint64_t now = SDL_GetTicksNS();
    float raw = 1.0f / 60.0f;
    if (e->ticker.last_ns != 0 && now > e->ticker.last_ns) {
        uint64_t interval = now - e->ticker.last_ns;
        raw = (float)((double)interval / 1e9);
    }
    if (e->ticker.last_ns != 0 && now > e->ticker.last_ns && e->ticker.frame >= 10) {
        /* frame statistics skip the first frames (window, pipelines and swapchain warm up) */
        uint64_t interval = now - e->ticker.last_ns;
        e->ticker.interval_count++;
        e->ticker.interval_sum_ns += interval;
        e->ticker.interval_max_ns = interval > e->ticker.interval_max_ns ? interval : e->ticker.interval_max_ns;
    }
    e->ticker.last_ns = now;
    e->ticker.fps = e->ticker.fps == 0.0f ? 1.0f / raw : e->ticker.fps * 0.9f + (1.0f / raw) * 0.1f;

    if (e->frame_limit > 0 || e->deterministic) {
        /* scripted runs (--frames, --deterministic) use a fixed step so screenshots are reproducible */
        raw = 1.0f / 60.0f;
    }
    float dt = br_ticker_compute_delta(raw, e->ticker.min_fps, e->ticker.speed);
    float unscaled = br_ticker_compute_delta(raw, e->ticker.min_fps, 1.0f);
    if (e->paused_inactive) {
        dt = 0.0f; /* inactive: systems still run (a pause screen can draw) but no time passes */
        unscaled = 0.0f;
    }
    bool presented = br_engine_step_frame(e, dt, unscaled);
    if ((!presented || e->paused_inactive) && e->window != NULL) {
        // ponytail: fixed 16 ms nap while minimised or paused so we do not spin a core; wake on SDL_EVENT_WINDOW_RESTORED if latency matters.
        SDL_Delay(16);
    }

    if (e->frame_limit > 0 && e->ticker.frame >= e->frame_limit) {
        if (e->ticker.interval_count > 0) {
            double avg_ms = (double)e->ticker.interval_sum_ns / (double)e->ticker.interval_count / 1e6;
            br_log_info("frames: %llu, frame interval avg %.3f ms (%.2f fps), max %.3f ms, vsync %s, cap %g",
                        (unsigned long long)e->ticker.frame, avg_ms, 1000.0 / avg_ms,
                        (double)e->ticker.interval_max_ns / 1e6, br_vsync_name(e->vsync), (double)e->ticker.max_fps);
        }
        if (e->screenshot_path != NULL) {
            if (!presented) {
                br_log_error("screenshot: the last frame was not rendered (window minimised?)");
                return SDL_APP_FAILURE;
            }
            if (!br_engine_screenshot(e, e->screenshot_path)) {
                return SDL_APP_FAILURE;
            }
        }
        return e->exit_failure ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    if (e->quit_requested) {
        return e->exit_failure ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

/* ---- lifecycle */

void br_app_on_lifecycle(br_engine *e, br_app_lifecycle_fn fn, void *user)
{
    br_lifecycle_entry entry = { fn, user };
    br_array_push(&e->lifecycle_handlers, &entry);
}

bool br_app_is_active(br_engine *e)
{
    return !e->focus_lost && !e->minimized && !e->in_background;
}

bool br_app_is_paused(br_engine *e)
{
    return e->paused_inactive;
}

static void br_app_notify_lifecycle(br_engine *e, br_app_lifecycle event)
{
    for (uint32_t i = 0; i < e->lifecycle_handlers.count; i++) {
        br_lifecycle_entry *h = br_array_at(&e->lifecycle_handlers, i);
        h->fn(e, event, h->user);
    }
}

/* Applies or lifts the inactive pause. Never in scripted runs (--deterministic, --frames) or
 * headless: those must give the same frames whatever the window manager does. */
static void br_app_update_pause(br_engine *e)
{
    bool pause = e->pause_when_inactive && !br_app_is_active(e) && !e->deterministic && e->frame_limit == 0 &&
                 e->window != NULL;
    if (pause == e->paused_inactive) {
        return;
    }
    e->paused_inactive = pause;
    br_audio_set_device_paused(e, pause);
    br_log_info(pause ? "app inactive: paused" : "app active: resumed");
}

void br_app_set_pause_when_inactive(br_engine *e, bool on)
{
    e->pause_when_inactive = on;
    br_app_update_pause(e);
}

/* Window focus / minimise and OS background / memory / terminate events. */
static void br_app_lifecycle_event(br_engine *e, const SDL_Event *event)
{
    bool was_active = br_app_is_active(e);
    switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        e->focus_lost = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        e->focus_lost = false;
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        e->minimized = true;
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        e->minimized = false;
        break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        e->in_background = true;
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        e->in_background = false;
        break;
    case SDL_EVENT_LOW_MEMORY:
        br_app_notify_lifecycle(e, BR_APP_LIFECYCLE_LOW_MEMORY);
        return;
    case SDL_EVENT_TERMINATING:
        br_app_notify_lifecycle(e, BR_APP_LIFECYCLE_TERMINATING);
        return;
    default:
        return;
    }
    bool active = br_app_is_active(e);
    if (active != was_active) {
        if (active) {
            e->ticker.last_ns = 0; /* the time spent inactive is not one giant frame */
        }
        br_app_update_pause(e);
        br_app_notify_lifecycle(e, active ? BR_APP_LIFECYCLE_ACTIVATED : BR_APP_LIFECYCLE_DEACTIVATED);
    }
}

SDL_AppResult br_app_event(void *appstate, SDL_Event *event)
{
    br_engine *e = appstate;
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    br_app_lifecycle_event(e, event);
    for (uint32_t i = 0; i < e->event_handlers.count; i++) {
        br_event_handler_entry *h = br_array_at(&e->event_handlers, i);
        h->fn(e, event, h->user);
    }
    return SDL_APP_CONTINUE;
}

static bool br_app_is_device_input(Uint32 type)
{
    if (type >= SDL_EVENT_KEY_DOWN && type < SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        return true; /* keyboard, text input and mouse */
    }
    if (type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && type < SDL_EVENT_FINGER_DOWN) {
        return true; /* joysticks and gamepads, including hot-plug */
    }
    if (type >= SDL_EVENT_FINGER_DOWN && type <= SDL_EVENT_FINGER_CANCELED) {
        return true;
    }
    if (type >= SDL_EVENT_PEN_PROXIMITY_IN && type <= SDL_EVENT_PEN_AXIS) {
        return true;
    }
    return false;
}

SDL_AppResult br_app_device_event(void *appstate, SDL_Event *event)
{
    br_engine *e = appstate;
    if (e->deterministic && !br_playtest_recording(e) && br_app_is_device_input(event->type)) {
        return SDL_APP_CONTINUE;
    }
    return br_app_event(appstate, event);
}

void br_app_quit(void *appstate, SDL_AppResult result)
{
    (void)result;
    br_engine *e = appstate;
    if (e == NULL) {
        return;
    }
    if (e->game_shutdown != NULL) {
        br_engine_destroy_nodes(e); /* desc.shutdown may free state that node listeners use */
        e->game_shutdown(e);
    }
    br_engine_destroy(e);
}
