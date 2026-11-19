/* br_game_config: game.json -> br_game_config -> br_app_desc + engine settings. Parsing works
 * without an engine (br_main runs before one exists), so errors go straight to br_log_error. */
#include "data/br_data_internal.h"

void br_game_config_defaults(br_game_config *config)
{
    SDL_zerop(config);
    br_str_copy(config->title, sizeof(config->title), "braty");
    config->width = 1280;
    config->height = 720;
    config->vsync = BR_VSYNC_ON;
    config->resizable = true;
    config->min_fps = 10.0f;
    config->speed = 1.0f;
    br_str_copy(config->debug_key, sizeof(config->debug_key), "F1");
#ifndef NDEBUG
    config->hot_reload = true;
#endif
}

/* ---- typed reads of section.key with "source: section.key: reason" errors */

typedef struct br_config_reader {
    const char *source;
    const char *section;   /* "" for top-level keys */
    bool        ok;
} br_config_reader;

static void br_config_error(br_config_reader *r, const char *key, const char *message)
{
    if (r->section[0] != '\0') {
        br_log_error("%s: %s.%s: %s", r->source, r->section, key, message);
    } else {
        br_log_error("%s: %s: %s", r->source, key, message);
    }
    r->ok = false;
}

static void br_config_bool(br_config_reader *r, const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return;
    }
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
    } else {
        br_config_error(r, key, "expected true or false");
    }
}

static void br_config_int(br_config_reader *r, const cJSON *obj, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return;
    }
    if (cJSON_IsNumber(v) && v->valuedouble > 0) {
        *out = (int)v->valuedouble;
    } else {
        br_config_error(r, key, "expected a positive number");
    }
}

static void br_config_float(br_config_reader *r, const cJSON *obj, const char *key, float *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return;
    }
    if (cJSON_IsNumber(v) && v->valuedouble >= 0) {
        *out = (float)v->valuedouble;
    } else {
        br_config_error(r, key, "expected a number >= 0");
    }
}

static void br_config_string(br_config_reader *r, const cJSON *obj, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return;
    }
    if (!cJSON_IsString(v)) {
        br_config_error(r, key, "expected a string");
    } else if (!br_str_copy(out, size, v->valuestring)) {
        br_config_error(r, key, "too long");
        out[0] = '\0';
    }
}

/* A section object ("window", "ticker", ...): NULL if absent, NULL + error if not an object. */
static const cJSON *br_config_section(br_config_reader *r, const cJSON *doc, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, name);
    if (v != NULL && !cJSON_IsObject(v)) {
        br_config_error(r, name, "expected an object");
        return NULL;
    }
    r->section = name;
    return v;
}

/* Warns about keys of obj that `known` (space-separated, spaces at both ends) does not list:
 * a typo in game.json would otherwise be ignored silently. Game data belongs under "game". */
static void br_config_warn_keys(const char *source, const char *section, const cJSON *obj, const char *known)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, obj)
    {
        char word[BR_DATA_NAME_SIZE + 2];
        SDL_snprintf(word, sizeof(word), " %s ", item->string);
        if (SDL_strstr(known, word) != NULL) {
            continue;
        }
        if (section[0] != '\0') {
            br_log_warn("%s: %s.%s: unknown key (ignored; a typo?)", source, section, item->string);
        } else {
            br_log_warn("%s: %s: unknown key (ignored; game data goes under \"game\")", source, item->string);
        }
    }
}

static void br_config_check_keys(const char *source, const cJSON *doc)
{
    br_config_warn_keys(source, "", doc,
                        " window asset_root manifest start_scene scenes clear_color ticker input audio debug profiler game ");
    const char *sections[] = { "window", "ticker", "debug", "audio", "profiler" };
    const char *keys[] = { " title width height fullscreen vsync resizable logical_size ", " max_fps min_fps speed fixed_hz ",
                           " overlay toggle_key hot_reload ", " master_volume muted buses sprites ", " hud panel " };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(sections); i++) {
        const cJSON *section = cJSON_GetObjectItemCaseSensitive(doc, sections[i]);
        if (cJSON_IsObject(section)) {
            br_config_warn_keys(source, sections[i], section, keys[i]);
        }
    }
    const cJSON *action = NULL;
    cJSON_ArrayForEach(action, cJSON_GetObjectItemCaseSensitive(doc, "input"))
    {
        char section[BR_DATA_NAME_SIZE + 8];
        SDL_snprintf(section, sizeof(section), "input.%s", action->string);
        br_config_warn_keys(source, section, action, " keys mouse buttons axes threshold context ");
    }
}

bool br_game_config_parse(const char *json, size_t size, const char *source_name, br_game_config *config)
{
    br_game_config_defaults(config);
    const char *source = source_name != NULL ? source_name : "game.json";
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!cJSON_IsObject(doc)) {
        int line = 1;
        for (const char *p = json; end != NULL && p < end; p++) {
            line += *p == '\n' ? 1 : 0;
        }
        br_log_error("%s: line %d: not a JSON object (syntax error)", source, line);
        cJSON_Delete(doc);
        return false;
    }
    br_config_check_keys(source, doc);
    br_config_reader r = { source, "", true };
    br_config_string(&r, doc, "asset_root", config->asset_root, sizeof(config->asset_root));
    br_config_string(&r, doc, "manifest", config->manifest, sizeof(config->manifest));
    br_config_string(&r, doc, "start_scene", config->start_scene, sizeof(config->start_scene));
    const cJSON *clear = cJSON_GetObjectItemCaseSensitive(doc, "clear_color");
    if (clear != NULL) {
        if (cJSON_IsString(clear) && br_color_parse(clear->valuestring, &config->clear_color)) {
            config->has_clear_color = true;
        } else {
            br_config_error(&r, "clear_color", "expected a colour (\"#rrggbb\" or a CSS name)");
        }
    }

    const cJSON *window = br_config_section(&r, doc, "window");
    br_config_string(&r, window, "title", config->title, sizeof(config->title));
    br_config_int(&r, window, "width", &config->width);
    br_config_int(&r, window, "height", &config->height);
    br_config_bool(&r, window, "fullscreen", &config->fullscreen);
    const cJSON *vsync = cJSON_GetObjectItemCaseSensitive(window, "vsync");
    if (cJSON_IsBool(vsync)) {
        config->vsync = cJSON_IsTrue(vsync) ? BR_VSYNC_ON : BR_VSYNC_OFF;
    } else if (vsync != NULL && !(cJSON_IsString(vsync) && br_vsync_from_name(vsync->valuestring, &config->vsync))) {
        br_config_error(&r, "vsync", "expected true, false, \"off\", \"on\", \"adaptive\" or \"mailbox\"");
    }
    br_config_bool(&r, window, "resizable", &config->resizable);
    const cJSON *logical = cJSON_GetObjectItemCaseSensitive(window, "logical_size");
    if (logical != NULL) {
        const cJSON *lw = cJSON_GetArrayItem(logical, 0);
        const cJSON *lh = cJSON_GetArrayItem(logical, 1);
        if (cJSON_IsArray(logical) && cJSON_GetArraySize(logical) == 2 && cJSON_IsNumber(lw) && cJSON_IsNumber(lh) &&
            lw->valuedouble > 0.0 && lh->valuedouble > 0.0) {
            config->logical_width = (float)lw->valuedouble;
            config->logical_height = (float)lh->valuedouble;
        } else {
            br_config_error(&r, "logical_size", "expected [width, height] (positive numbers)");
        }
    }

    const cJSON *ticker = br_config_section(&r, doc, "ticker");
    br_config_float(&r, ticker, "max_fps", &config->max_fps);
    br_config_float(&r, ticker, "min_fps", &config->min_fps);
    br_config_float(&r, ticker, "speed", &config->speed);
    br_config_float(&r, ticker, "fixed_hz", &config->fixed_hz);

    const cJSON *debug = br_config_section(&r, doc, "debug");
    br_config_bool(&r, debug, "overlay", &config->debug_overlay);
    br_config_string(&r, debug, "toggle_key", config->debug_key, sizeof(config->debug_key));
    br_config_bool(&r, debug, "hot_reload", &config->hot_reload);

    const cJSON *profiler = br_config_section(&r, doc, "profiler");
    br_config_bool(&r, profiler, "hud", &config->profiler_hud);
    br_config_bool(&r, profiler, "panel", &config->profiler_panel);

    r.section = "";
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(doc, "input");
    if (input != NULL && !cJSON_IsObject(input)) {
        br_config_error(&r, "input", "expected an object of actions");
    }
    br_config_section(&r, doc, "audio");
    const cJSON *scenes = cJSON_GetObjectItemCaseSensitive(doc, "scenes");
    const cJSON *scene = NULL;
    if (scenes != NULL && !cJSON_IsObject(scenes)) {
        br_config_error(&r, "scenes", "expected an object { \"name\": \"path.scene.json\" }");
    }
    if (!cJSON_IsObject(scenes)) {
        scenes = NULL;
    }
    cJSON_ArrayForEach(scene, scenes)
    {
        if (!cJSON_IsString(scene)) {
            br_log_error("%s: scenes.%s: expected a scene file path", source, scene->string);
        }
    }
    config->json = doc;
    return true;
}

bool br_game_config_load(br_engine *e, const char *path, br_game_config *config)
{
    (void)e; /* the file is always relative to the executable dir, engine or not */
    br_game_config_defaults(config);
    char full[1024];
    const char *base = SDL_GetBasePath();
    if (br_path_is_absolute(path)) {
        br_str_copy(full, sizeof(full), path);
    } else {
        br_path_join(full, sizeof(full), base != NULL ? base : "", path);
    }
    size_t size = 0;
    void *bytes = SDL_LoadFile(full, &size);
    if (bytes == NULL) {
        br_log_error("%s: cannot read game config: %s", full, SDL_GetError());
        return false;
    }
    bool ok = br_game_config_parse(bytes, size, path, config);
    SDL_free(bytes);
    return ok;
}

void br_game_config_args(br_game_config *config, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--debug") == 0) {
            config->debug_overlay = true;
        } else if (SDL_strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            br_str_copy(config->start_scene, sizeof(config->start_scene), argv[++i]);
        }
    }
}

br_app_desc br_game_config_app_desc(const br_game_config *config)
{
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = config->title;
    desc.width = config->width;
    desc.height = config->height;
    desc.asset_root = config->asset_root[0] != '\0' ? config->asset_root : NULL;
    return desc;
}

/* "audio": { "master_volume", "muted", "buses": { name: volume or { volume, muted } },
 *            "sprites": { "path": { name: [start, duration] } } } */
static void br_config_apply_audio(br_engine *e, const cJSON *audio)
{
    br_config_reader r = { "game.json", "audio", true };
    float volume = 1.0f;
    bool muted = false;
    br_config_float(&r, audio, "master_volume", &volume);
    br_config_bool(&r, audio, "muted", &muted);
    br_audio_set_master_volume(e, volume);
    br_audio_set_muted(e, muted);
    const cJSON *bus = NULL;
    cJSON_ArrayForEach(bus, cJSON_GetObjectItemCaseSensitive(audio, "buses"))
    {
        int id = br_audio_bus_create(e, bus->string);
        float bus_volume = 1.0f;
        bool bus_muted = false;
        if (cJSON_IsNumber(bus)) {
            bus_volume = (float)bus->valuedouble;
        } else {
            r.section = "audio.buses";
            br_config_float(&r, bus, "volume", &bus_volume);
            br_config_bool(&r, bus, "muted", &bus_muted);
        }
        br_audio_bus_set_volume(e, id, bus_volume);
        br_audio_bus_set_muted(e, id, bus_muted);
    }
    const cJSON *file = NULL;
    cJSON_ArrayForEach(file, cJSON_GetObjectItemCaseSensitive(audio, "sprites"))
    {
        br_sound *sound = br_sound_load(e, file->string);
        const cJSON *sprite = NULL;
        cJSON_ArrayForEach(sprite, file)
        {
            const cJSON *start = cJSON_GetArrayItem(sprite, 0);
            const cJSON *length = cJSON_GetArrayItem(sprite, 1);
            if (sound == NULL || !cJSON_IsNumber(start) || !cJSON_IsNumber(length) ||
                !br_sound_add_sprite(e, sound, sprite->string, (float)start->valuedouble, (float)length->valuedouble)) {
                br_log_error("game.json: audio.sprites.%s.%s: expected [start, duration] of a loadable sound",
                             file->string, sprite->string);
            }
        }
    }
}

void br_game_config_apply_settings(br_engine *e, br_game_config *config)
{
    SDL_Window *window = br_engine_window(e);
    br_gpu *gpu = br_engine_gpu(e);
    if (window != NULL) {
        SDL_SetWindowResizable(window, config->resizable);
        if (config->fullscreen) {
            SDL_SetWindowFullscreen(window, true);
        }
    }
    br_app_set_vsync(e, config->vsync);
    if (gpu != NULL) {
        if (config->has_clear_color) {
            br_gpu_set_clear_color(gpu, config->clear_color);
        }
    }
    br_ticker_set_max_fps(e, config->max_fps);
    br_ticker_set_min_fps(e, config->min_fps);
    br_ticker_set_speed(e, config->speed);
    br_app_set_fixed_timestep(e, config->fixed_hz);
    br_scene_file_set_hot_reload(e, config->hot_reload, 0);

    SDL_Scancode key = SDL_GetScancodeFromName(config->debug_key);
    if (key == SDL_SCANCODE_UNKNOWN) {
        br_log_error("game.json: debug.toggle_key: unknown key '%s'", config->debug_key);
    } else {
        br_debug_set_toggle_key(e, key);
    }
    br_debug_set_open(e, config->debug_overlay);
    /* only switched on: false is the default, and must not undo --profiler / --profiler-hud */
    if (config->profiler_hud) {
        br_profiler_set_hud(e, true);
    }
    if (config->profiler_panel) {
        br_profiler_set_open(e, true);
    }
    if (config->logical_width > 0.0f && config->logical_height > 0.0f) {
        br_camera_create_logical(e, config->logical_width, config->logical_height);
    }

    const cJSON *input = cJSON_GetObjectItemCaseSensitive(config->json, "input");
    if (cJSON_IsObject(input)) {
        br_input_actions_from_json(e, input);
    }
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(config->json, "audio");
    if (cJSON_IsObject(audio)) {
        br_config_apply_audio(e, audio);
    }
    if (config->manifest[0] != '\0') {
        br_assets_load_manifest(e, config->manifest);
    }

    cJSON_Delete(e->data->game_json);
    e->data->game_json = config->json;
    config->json = NULL;
}

br_node br_game_config_load_start_scene(br_engine *e, const br_game_config *config)
{
    br_node none = { 0, 0 };
    if (config->start_scene[0] == '\0') {
        return none;
    }
    /* a br_scene (a name from "scenes" or a path): the root returned is the file's own root */
    br_scene scene = br_scene_load(e, config->start_scene, BR_SCENE_SINGLE);
    br_node root = br_node_child_at(e, br_scene_root_2d(e, scene), 0);
    return br_node_valid(e, root) ? root : none;
}

br_node br_game_config_apply(br_engine *e, br_game_config *config)
{
    br_game_config_apply_settings(e, config);
    return br_game_config_load_start_scene(e, config);
}

br_app_desc br_game_config_main(const char *path, int argc, char **argv, bool (*init)(br_engine *e))
{
    br_app_desc desc;
    SDL_zero(desc);
    br_game_config *config = br_alloc(sizeof(br_game_config));
    if (config != NULL) {
        br_game_config_load(NULL, path, config); /* a missing file is logged; defaults are kept */
        br_game_config_args(config, argc, argv);
        desc = br_game_config_app_desc(config);
    }
    desc.init = init;
    desc.config = config;
    return desc;
}

const cJSON *br_game_config_json(br_engine *e)
{
    return e->data != NULL ? e->data->game_json : NULL;
}

void br_game_config_free(br_game_config *config)
{
    cJSON_Delete(config->json);
    config->json = NULL;
}
