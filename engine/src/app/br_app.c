/* SDL main-callback glue: command line, the per-frame tick, --frames / --screenshot. */
#include "app/br_engine_internal.h"
#include "data/br_data_internal.h"

#include "stb/stb_image_write.h"

typedef struct br_app_args {
    uint64_t    frames;
    const char *screenshot;
    const char *gpu_driver;
    bool        headless;
} br_app_args;

static void br_app_parse_args(int argc, char **argv, br_app_args *out)
{
    SDL_zerop(out);
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
        }
        /* anything else belongs to the game (it sees argv in br_main) */
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

static bool br_app_write_screenshot(br_engine *e, const char *path)
{
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
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    bool ok = false;
    if (io == NULL) {
        br_log_error("screenshot: cannot open '%s': %s", path, SDL_GetError());
    } else {
        ok = stbi_write_png_to_func(br_png_write_callback, io, (int)rt->width, (int)rt->height, 4, pixels,
                                    (int)rt->width * 4) != 0;
        ok = SDL_CloseIO(io) && ok;
        if (ok) {
            br_log_info("screenshot: wrote %ux%u to '%s'", rt->width, rt->height, path);
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
    br_engine *e = br_engine_create(title, desc->asset_root);
    bool ok = e != NULL;
    if (ok) {
        *appstate = e;
        e->frame_limit = args.frames;
        e->screenshot_path = br_strdup(args.screenshot);
        e->app_user = desc->user;
        e->argc = argc;
        e->argv = argv;
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
    uint64_t now = SDL_GetTicksNS();
    float raw = 1.0f / 60.0f;
    if (e->ticker.last_ns != 0 && now > e->ticker.last_ns) {
        raw = (float)((double)(now - e->ticker.last_ns) / 1e9);
    }
    e->ticker.last_ns = now;
    e->ticker.fps = e->ticker.fps == 0.0f ? 1.0f / raw : e->ticker.fps * 0.9f + (1.0f / raw) * 0.1f;

    float dt = br_ticker_compute_delta(raw, e->ticker.min_fps, e->ticker.speed);
    if (e->frame_limit > 0) {
        /* scripted runs (--frames) use a fixed step so screenshots are reproducible */
        dt = br_ticker_compute_delta(1.0f / 60.0f, e->ticker.min_fps, e->ticker.speed);
    }
    bool presented = br_engine_step(e, dt);
    if (!presented && e->window != NULL) {
        // ponytail: fixed 16 ms nap while minimised so we do not spin a core; wake on SDL_EVENT_WINDOW_RESTORED if latency matters.
        SDL_Delay(16);
    }

    if (e->frame_limit > 0 && e->ticker.frame >= e->frame_limit) {
        if (e->screenshot_path != NULL) {
            if (!presented) {
                br_log_error("screenshot: the last frame was not rendered (window minimised?)");
                return SDL_APP_FAILURE;
            }
            if (!br_app_write_screenshot(e, e->screenshot_path)) {
                return SDL_APP_FAILURE;
            }
        }
        return SDL_APP_SUCCESS;
    }
    return e->quit_requested ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

SDL_AppResult br_app_event(void *appstate, SDL_Event *event)
{
    br_engine *e = appstate;
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    for (uint32_t i = 0; i < e->event_handlers.count; i++) {
        br_event_handler_entry *h = br_array_at(&e->event_handlers, i);
        h->fn(e, event, h->user);
    }
    return SDL_APP_CONTINUE;
}

void br_app_quit(void *appstate, SDL_AppResult result)
{
    (void)result;
    br_engine *e = appstate;
    if (e == NULL) {
        return;
    }
    if (e->game_shutdown != NULL) {
        e->game_shutdown(e);
    }
    br_engine_destroy(e);
}
