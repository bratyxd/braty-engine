/* flow.c - the game's states and scenes:
 *
 *   menu --play--> loading (br_scene_switch with the "loading" scene) --> playing <--> paused
 *     ^                                                                   |
 *     +------ results <-- level_end (all crates, or the clock ran out) <--+
 *
 * Systems: cr.flow (UPDATE 0). Scene events reset the level state when a level unloads.
 * A level is a .glb loaded as a scene; on activation this file reads its scene extras into a
 * "level" component, builds the collider list in the level arena, makes the file's camera (a
 * br_camera through the .glb's own view, br_gltf_options.camera_as_br_camera) follow the player,
 * spawns the player and loads the HUD on top.
 */
#include "game.h"

static const char *cr_state_names[] = { "boot", "menu", "loading", "playing", "paused", "level_end", "results" };

/* ---- small helpers */

const cr_tuning *cr_tuning_get(br_engine *e, cr_game *g)
{
    static const cr_tuning none; /* all zero if tuning.json failed to load (already logged) */
    const cr_tuning *t = br_component_get(e, g->tuning_node, g->id.tuning);
    return t != NULL ? t : &none;
}

const char *cr_config_string(br_engine *e, const char *key)
{
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(game, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void cr_play_on(br_engine *e, const char *path, float volume, int bus)
{
    br_sound *s = path[0] != '\0' ? br_assets_get(e, path) : NULL;
    if (s == NULL) {
        return;
    }
    br_audio_play_desc play = br_audio_play_defaults();
    play.volume = volume;
    play.bus = bus;
    br_audio_play(e, s, &play);
}

/* Plays a sound by path on the sfx bus (already loaded by a group: no reference is taken here). */
void cr_play(br_engine *e, cr_game *g, const char *path, float volume)
{
    cr_play_on(e, path, volume, g->sfx_bus);
}

static const char *cr_sound_path(br_engine *e, const char *name)
{
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(game, "sounds"), name);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* Plays the sound game.json names under "game.sounds.<name>" on the sfx bus (the pause holds it). */
void cr_sound(br_engine *e, cr_game *g, const char *name)
{
    cr_play_on(e, cr_sound_path(e, name), 1.0f, g->sfx_bus);
}

/* The same on the ui bus, which the pause leaves running (menu clicks while paused). */
void cr_ui_sound(br_engine *e, cr_game *g, const char *name)
{
    cr_play_on(e, cr_sound_path(e, name), 1.0f, g->ui_bus);
}

float cr_random(cr_game *g)
{
    /* xorshift32: deterministic, so play-test runs give identical frames */
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return (float)(g->rng >> 8) / 16777216.0f;
}

/* Live counts for play-tests (the soak compares them between menu visits). */
static void cr_update_stats(br_engine *e, cr_game *g)
{
    uint32_t animators = 0;
    uint32_t tracks = 0;
    br_animation_stats(e, &animators, &tracks);
    g->stat_nodes = (int32_t)br_node_count(e);
    g->stat_nodes3d = (int32_t)br_node3d_count(e);
    g->stat_assets = (int32_t)br_assets_count(e);
    g->stat_animators = (int32_t)animators;
    g->stat_tweens = (int32_t)br_tween_count(e);
    g->stat_sounds = (int32_t)br_audio_instance_count(e);
    g->stat_speed = br_ticker_speed(e);
    g->stat_sfx_paused = br_audio_bus_paused(e, g->sfx_bus);
}

void cr_log_stats(br_engine *e, cr_game *g, const char *when)
{
    br_memory_stats heap;
    br_memory_get_stats(&heap);
    uint32_t animators = 0;
    uint32_t tracks = 0;
    br_animation_stats(e, &animators, &tracks);
    br_log_info("crate_rush stats (%s): nodes %u, 3d nodes %u, assets %u, animators %u, tweens %u, sounds %u, "
                "level arena %llu bytes, heap %llu blocks, levels done %u",
                when, br_node_count(e), br_node3d_count(e), br_assets_count(e), animators, br_tween_count(e),
                br_audio_instance_count(e), (unsigned long long)g->level_arena.stats.used,
                (unsigned long long)heap.live_allocations, g->levels_completed);
}

/* Makes `context` the only input context ("" = none). */
static void cr_context(br_engine *e, const char *context)
{
    while (br_input_context_top(e)[0] != '\0') {
        br_input_context_pop(e);
    }
    if (context[0] != '\0') {
        br_input_context_push(e, context);
    }
}

static br_node cr_find(br_engine *e, br_scene s, const char *label)
{
    return br_node_find_by_label(e, br_scene_root_2d(e, s), label, true);
}

/* ---- settings and best scores (save data; skipped under --deterministic so play-tests never
 * depend on what an earlier run saved) */

void cr_settings_apply(br_engine *e, cr_game *g)
{
    br_app_set_vsync(e, (br_vsync)g->settings.vsync);
    br_app_set_max_fps(e, (float)g->settings.max_fps);
    br_audio_set_master_volume(e, (float)g->settings.volume / 100.0f);
    br_profiler_set_hud(e, g->settings.profiler_hud);
}

void cr_settings_save(br_engine *e, cr_game *g)
{
    if (br_app_deterministic(e)) {
        return;
    }
    /* cJSON escapes the name's quotes and backslashes */
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "vsync", g->settings.vsync);
    cJSON_AddNumberToObject(doc, "max_fps", g->settings.max_fps);
    cJSON_AddNumberToObject(doc, "volume", g->settings.volume);
    cJSON_AddBoolToObject(doc, "profiler_hud", g->settings.profiler_hud);
    cJSON_AddStringToObject(doc, "language", g->settings.language);
    cJSON_AddStringToObject(doc, "name", g->settings.name);
    char *text = cJSON_Print(doc);
    cJSON_Delete(doc);
    if (text != NULL) {
        br_save_write(e, "settings.json", text, SDL_strlen(text));
        cJSON_free(text);
    }
    br_input_bindings_save(e, "bindings.json");
}

static void cr_settings_load(br_engine *e, cr_game *g)
{
    g->settings.vsync = (int32_t)br_app_vsync(e);
    g->settings.max_fps = (int32_t)br_app_max_fps(e);
    g->settings.volume = 80;
    g->settings.profiler_hud = br_profiler_hud(e); /* --profiler-hud / game.json "profiler.hud" */
    if (br_app_deterministic(e)) {
        return;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (br_save_read(e, "settings.json", &bytes, &size)) {
        cJSON *doc = cJSON_ParseWithLength(bytes, size);
        br_free(bytes);
        int v = 0;
        if (br_data_get_int(e, doc, "vsync", &v)) {
            g->settings.vsync = v;
        }
        if (br_data_get_int(e, doc, "max_fps", &v)) {
            g->settings.max_fps = v;
        }
        if (br_data_get_int(e, doc, "volume", &v)) {
            g->settings.volume = SDL_clamp(v, 0, 100);
        }
        br_data_get_bool(e, doc, "profiler_hud", &g->settings.profiler_hud);
        br_data_get_string(e, doc, "name", g->settings.name, sizeof(g->settings.name));
        char language[BR_LOC_NAME_SIZE] = "";
        /* a language the game no longer ships keeps the one br_loc_setup picked (logged) */
        if (br_data_get_string(e, doc, "language", language, sizeof(language)) && language[0] != '\0' &&
            br_loc_set_locale(e, language)) {
            br_str_copy(g->settings.language, sizeof(g->settings.language), language);
        }
        cJSON_Delete(doc);
    }
    br_input_bindings_load(e, "bindings.json"); /* missing file: the game.json bindings stay */
    if (br_save_read(e, "best.json", &bytes, &size)) {
        cJSON *doc = cJSON_ParseWithLength(bytes, size);
        br_free(bytes);
        for (int i = 1; i <= CR_MAX_LEVELS; i++) {
            const cJSON *v = cJSON_GetArrayItem(doc, i - 1);
            g->best[i] = cJSON_IsNumber(v) ? v->valueint : 0;
        }
        cJSON_Delete(doc);
    }
}

static void cr_best_save(br_engine *e, cr_game *g)
{
    if (br_app_deterministic(e)) {
        return;
    }
    char text[96];
    SDL_snprintf(text, sizeof(text), "[%d, %d, %d]\n", (int)g->best[1], (int)g->best[2], (int)g->best[3]);
    br_save_write(e, "best.json", text, SDL_strlen(text));
}

/* ---- level setup (the .glb scene is active and built) */

/* The model3d instance the .glb scene put under its 3D root. */
static br_node3d cr_level_instance(br_engine *e, br_node3d root)
{
    for (uint32_t c_i = 0; c_i < br_node3d_child_count(e, root); c_i++) {
        br_node3d c = br_node3d_child_at(e, root, c_i);
        if (br_node3d_type_of(e, c) == br_model3d_type(e)) {
            return c;
        }
    }
    br_node3d none = { 0, 0 };
    return none;
}

/* World-space boxes of every solid "gltf.collider" (from "-colonly" / "-col" nodes) into the
 * level arena: the movement code loops over this dense array. */
static void cr_build_colliders(br_engine *e, cr_game *g)
{
    uint32_t id = br_gltf_collider_component(e);
    uint32_t count = br_component3d_count(e, id);
    g->colliders = br_arena_linear_alloc(&g->level_arena, sizeof(br_aabb) * (count > 0 ? count : 1));
    g->collider_count = 0;
    for (uint32_t i = 0; g->colliders != NULL && i < count; i++) {
        const br_gltf_collider *c = br_component3d_at(e, id, i);
        br_node3d n = br_component3d_owner_at(e, id, i);
        if (c->trigger || !br_node3d_valid(e, n)) {
            continue;
        }
        br_mat4 world = br_node3d_world_transform(e, n);
        br_aabb box = br_aabb_empty();
        for (int k = 0; k < 8; k++) {
            br_vec3 corner = br_vec3_make(c->center.x + ((k & 1) ? 0.5f : -0.5f) * c->size.x,
                                          c->center.y + ((k & 2) ? 0.5f : -0.5f) * c->size.y,
                                          c->center.z + ((k & 4) ? 0.5f : -0.5f) * c->size.z);
            box = br_aabb_extend(box, br_mat4_transform_point(world, corner));
        }
        g->colliders[g->collider_count++] = box;
    }
}

static void cr_level_setup(br_engine *e, cr_game *g)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    g->level = br_scene_active(e);
    g->camera = cr_find(e, g->level, "Camera"); /* the .glb's camera, labelled after its glTF node */
    br_node_set_label(e, g->camera, "level_camera");
    g->level_root = br_scene_root_3d(e, g->level);
    br_node3d inst = cr_level_instance(e, g->level_root);
    const br_model *model = br_model3d_model(e, inst);

    /* level settings live in the glTF scene extras: {"level": {...}, "sky": ..., "background": ...} */
    cJSON *extras = model != NULL && model->scene_extras != NULL ? cJSON_Parse(model->scene_extras) : NULL;
    br_scene_file_apply_component(e, g->camera, "level", cJSON_GetObjectItemCaseSensitive(extras, "level"));
    cJSON_Delete(extras);
    const cr_level *lvl = br_component_get(e, g->camera, g->id.level);
    cr_level defaults;
    SDL_zero(defaults);
    defaults.time_limit = 60.0f;
    if (lvl == NULL) {
        br_log_warn("crate_rush: %s has no \"level\" scene extras; using defaults", g->level_scene);
        lvl = &defaults;
    }
    br_str_copy(g->level_title, sizeof(g->level_title), lvl->title);
    g->time_limit = lvl->time_limit;
    g->time_left = lvl->time_limit;
    g->kill_y = lvl->kill_y;
    g->last_tick = -1;

    /* the camera draws through the .glb's view: sky, ground, background and MSAA come from the
     * scene extras already; bloom is tuning */
    br_post3d_settings *post = br_view3d_post(e, br_camera_view3d(e, g->camera));
    if (post != NULL) {
        post->bloom_threshold = t->bloom_threshold;
        post->bloom_intensity = t->bloom_intensity;
    }

    /* per-level memory and data */
    br_arena_linear_reset(&g->level_arena);
    cr_build_colliders(e, g);
    g->spawn = br_vec3_make(0.0f, 1.0f, 0.0f);
    g->spawn_yaw = 0.0f;
    if (br_component3d_count(e, g->id.spawn_point) > 0) {
        const cr_spawn_point *sp = br_component3d_at(e, g->id.spawn_point, 0);
        g->spawn = br_node3d_world_position(e, br_component3d_owner_at(e, g->id.spawn_point, 0));
        g->spawn_yaw = sp->yaw * BR_DEG_TO_RAD;
    }
    g->crates_total = 0;
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.crate); i++) {
        g->crates_total += br_node3d_valid(e, br_component3d_owner_at(e, g->id.crate, i)) ? 1 : 0;
    }
    g->crates_taken = 0;
    g->score = 0;
    g->hits = 0;
    g->falls = 0;
    g->level_time = 0.0f;
    g->cleared = false;

    /* the rig starts at the file camera; the follow offset keeps that framing */
    br_node3d rig = br_camera_node3d(e, g->camera);
    br_vec3 eye = br_node3d_world_position(e, rig);
    br_vec3 look = br_vec3_add(g->spawn, br_vec3_make(0.0f, t->height, 0.0f));
    br_node3d_set_position(e, rig, eye);
    br_node3d_look_at(e, rig, look, br_vec3_make(0.0f, 1.0f, 0.0f));
    cr_player_spawn(e, g);
    br_camera_follow *f = br_component_add(e, g->camera, br_camera_follow_component(e));
    f->target3d = g->camera_target;
    /* camera_distance scales the file camera's offset (1 = as authored) */
    f->offset3d = br_vec3_scale(br_vec3_sub(eye, look), t->camera_distance > 0.0f ? t->camera_distance : 1.0f);
    f->damping = t->camera_damping;
    br_camera_shake *sh = br_component_add(e, g->camera, br_camera_shake_component(e));
    sh->decay = 1.6f;
    sh->max_offset = 0.35f;
    sh->max_angle = 0.05f;
    sh->frequency = 18.0f;

    /* per-level assets: the theme's footsteps belong to the level scene's group and go with it */
    br_str_copy(g->footsteps, sizeof(g->footsteps), lvl->footsteps);
    br_asset_group *group = br_scene_assets(e, g->level);
    for (int i = 0; i < 5 && g->footsteps[0] != '\0'; i++) {
        char path[96];
        SDL_snprintf(path, sizeof(path), "%s_%03d.ogg", g->footsteps, i);
        br_asset_group_add(group, path);
    }
    br_asset_group_load(group);

    cr_world_setup(e, g);
    br_scene_load(e, "hud", BR_SCENE_ADDITIVE);
    br_scene_set_active(e, g->level);
    cr_context(e, "gameplay");
    cr_sound(e, g, "start");
    br_log_info("crate_rush: %s '%s' started: %u colliders, %d crates, %.0f s", g->level_scene, g->level_title,
                g->collider_count, (int)g->crates_total, (double)g->time_limit);
    cr_log_stats(e, g, "level start");
}

/* A level (or anything) unloads: forget handles into it. */
static void cr_on_scene_event(br_engine *e, br_scene scene, br_scene_event event, void *user)
{
    cr_game *g = user;
    if (event != BR_SCENE_EVENT_UNLOADING) {
        return;
    }
    if (scene.index == g->overlay.index && scene.generation == g->overlay.generation) {
        g->overlay.index = 0;
        g->overlay.generation = 0;
    }
    if (scene.index != g->level.index || scene.generation != g->level.generation) {
        return;
    }
    br_node3d none3d = { 0, 0 };
    br_node none = { 0, 0 };
    br_scene no_scene = { 0, 0 };
    g->level = no_scene;
    g->level_root = none3d;
    g->camera = none;
    g->player = none3d;
    g->camera_target = none3d;
    g->colliders = NULL;
    g->collider_count = 0;
    br_arena_linear_reset(&g->level_arena); /* everything the level put there goes at once */
    br_system_group_set_enabled(e, "gameplay", true);
}

/* ---- states */

static void cr_open_overlay(br_engine *e, cr_game *g, const char *name)
{
    if (br_scene_valid(e, g->overlay)) {
        br_scene_unload(e, g->overlay);
    }
    g->overlay = br_scene_load(e, name, BR_SCENE_ADDITIVE);
    br_str_copy(g->overlay_name, sizeof(g->overlay_name), name);
    if (br_scene_valid(e, g->level)) {
        br_scene_set_active(e, g->level);
    }
    g->focus = 0;
    cr_context(e, "menu");
}

static void cr_close_overlay(br_engine *e, cr_game *g)
{
    if (br_scene_valid(e, g->overlay)) {
        br_scene_unload(e, g->overlay);
    }
    br_scene none = { 0, 0 };
    g->overlay = none;
    g->overlay_name[0] = '\0';
    g->focus = 0;
}

void cr_enter_state(br_engine *e, cr_game *g, cr_state state)
{
    cr_state from = g->state;
    g->state = state;
    br_str_copy(g->state_name, sizeof(g->state_name), cr_state_names[state]);
    g->state_time = 0.0f;
    g->focus = 0;
    g->filled = false;
    /* the pause freezes game time (tweens, animators, camera shake and follow, popups) and holds
     * the gameplay sounds; the pause and settings UI run on the unscaled clock and the ui bus */
    bool paused = state == CR_STATE_PAUSED;
    br_ticker_set_speed(e, paused ? 0.0f : g->speed);
    br_audio_bus_set_paused(e, g->sfx_bus, paused);
    switch (state) {
    case CR_STATE_MENU:
        br_scene_set_loading_scene(e, NULL);
        if (from == CR_STATE_BOOT) {
            br_scene_load(e, "menu", BR_SCENE_SINGLE);
        } else {
            br_scene_switch(e, "menu", BR_TRANSITION_FADE, 0.25f);
        }
        cr_close_overlay(e, g);
        br_system_group_set_enabled(e, "gameplay", true);
        cr_context(e, "menu");
        break;
    case CR_STATE_LOADING:
        cr_close_overlay(e, g);
        cr_context(e, "");
        break;
    case CR_STATE_PLAYING:
        if (from == CR_STATE_PAUSED) {
            cr_close_overlay(e, g);
            br_system_group_set_enabled(e, "gameplay", true);
            cr_context(e, "gameplay");
        }
        break;
    case CR_STATE_PAUSED:
        br_system_group_set_enabled(e, "gameplay", false); /* their input reads stop too */
        cr_open_overlay(e, g, "pause");
        break;
    case CR_STATE_LEVEL_END:
        cr_context(e, "");
        cr_sound(e, g, g->cleared ? "clear" : "timeup");
        break;
    case CR_STATE_RESULTS:
        br_scene_set_loading_scene(e, NULL);
        br_scene_switch(e, "results", BR_TRANSITION_FADE, 0.3f);
        cr_context(e, "menu");
        break;
    case CR_STATE_BOOT:
        break;
    }
}

void cr_start_level(br_engine *e, cr_game *g, const char *scene_name)
{
    if (br_scene_switching(e)) {
        return;
    }
    br_str_copy(g->level_scene, sizeof(g->level_scene), scene_name);
    br_scene_set_loading_scene(e, "loading");
    br_scene_switch(e, scene_name, BR_TRANSITION_FADE, 0.25f);
    cr_enter_state(e, g, CR_STATE_LOADING);
}

/* The results of the level just played (the results scene shows them). */
static void cr_finish_level(br_engine *e, cr_game *g)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    int32_t seconds = g->cleared ? (int32_t)g->time_left : 0;
    g->result_time_bonus = seconds * t->time_bonus;
    g->result_score = g->score + g->result_time_bonus;
    g->result_crates = g->crates_taken;
    g->result_total = g->crates_total;
    g->result_stars = 0;
    if (g->cleared) {
        g->result_stars = g->time_left >= g->time_limit * 0.4f ? 3 : 2;
    } else if (g->crates_taken * 2 >= g->crates_total) {
        g->result_stars = 1;
    }
    const cr_level *lvl = br_component_get(e, g->camera, g->id.level);
    int index = lvl != NULL ? SDL_clamp(lvl->index, 1, CR_MAX_LEVELS) : 1;
    g->result_new_best = g->result_score > g->best[index];
    if (g->result_new_best) {
        g->best[index] = g->result_score;
        cr_best_save(e, g);
    }
    g->levels_completed++;
    br_log_info("crate_rush: %s over (%s): %d/%d crates, score %d + time bonus %d, %d stars", g->level_scene,
                g->cleared ? "cleared" : "time up", (int)g->crates_taken, (int)g->crates_total, (int)g->score,
                (int)g->result_time_bonus, (int)g->result_stars);
}

/* The name field: Enter keeps the name (save data), and the menu greets the player with it. */
static void cr_on_name(br_engine *e, br_node field, br_text_field_event event, void *user)
{
    cr_game *g = user;
    if (event != BR_TEXT_FIELD_SUBMITTED) {
        return;
    }
    br_str_copy(g->settings.name, sizeof(g->settings.name), br_text_field_text(e, field));
    cr_settings_save(e, g);
    br_node none = { 0, 0 };
    br_text_field_focus(e, none);
    cr_ui_sound(e, g, "confirm");
}

/* The results scene became active: fill it in. */
static void cr_results_fill(br_engine *e, cr_game *g)
{
    br_scene s = br_scene_active(e);
    br_node root = br_scene_root_2d(e, s);
    char text[64];
    /* bound to their keys: the texts follow a language change */
    br_loc_bind_text(e, cr_find(e, s, "result_title"), g->cleared ? "results.clear" : "results.time_up");
    br_loc_bind_text(e, cr_find(e, s, "result_level"), g->level_title);
    SDL_snprintf(text, sizeof(text), "%d / %d", (int)g->result_crates, (int)g->result_total);
    cr_ui_text(e, root, "result_crates", text);
    cr_ui_count_to(e, g, root, "result_score", (float)g->score);
    cr_ui_count_to(e, g, root, "result_bonus", (float)g->result_time_bonus);
    cr_ui_count_to(e, g, root, "result_total", (float)g->result_score);
    if (g->result_new_best) {
        br_loc_bind_text(e, cr_find(e, s, "result_best"), "results.new_best");
    }
    br_node name = cr_find(e, s, "player_name");
    br_text_field_set_max_length(e, name, CR_NAME_MAX);
    br_text_field_set_placeholder(e, name, br_loc_get(e, "results.name_placeholder"));
    br_text_field_set_text(e, name, g->settings.name); /* the name entered last time */
    br_text_field_set_listener(e, name, cr_on_name, g);
    for (int i = 1; i <= 3; i++) {
        SDL_snprintf(text, sizeof(text), "star_%d", i);
        br_node star = cr_find(e, s, text);
        bool lit = i <= g->result_stars;
        br_sprite_set_texture(e, star, br_spritesheet_frame(e, br_assets_get(e, "ui/ui.json"), lit ? "star" : "star_empty"));
        if (lit) {
            br_vec2 base = br_node_scale(e, star);
            float from[2] = { 0.0f, 0.0f };
            float to[2] = { base.x, base.y };
            br_tween t = br_tween_from_to(e, br_anim_target_2d(star), "scale", from, to, 0.45f, BR_EASE_OUT_BACK);
            br_tween_set_delay(e, t, 0.35f + 0.2f * (float)i);
        }
    }
    /* "next" only after a cleared level that has one */
    const char *next = "";
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    const cJSON *levels = cJSON_GetObjectItemCaseSensitive(game, "levels");
    for (int i = 0; i < cJSON_GetArraySize(levels) - 1; i++) {
        if (SDL_strcmp(cJSON_GetArrayItem(levels, i)->valuestring, g->level_scene) == 0) {
            next = cJSON_GetArrayItem(levels, i + 1)->valuestring;
        }
    }
    br_node next_button = cr_find(e, s, "next_button");
    cr_button *nb = br_component_get(e, next_button, g->id.button);
    if (nb != NULL) {
        br_str_copy(nb->arg, sizeof(nb->arg), next);
    }
    br_node_set_visible(e, next_button, g->cleared && next[0] != '\0');
}

/* The menu's texts made from data: best scores in the level select, the greeting. */
static void cr_menu_texts(br_engine *e, cr_game *g)
{
    br_scene s = br_scene_find(e, "menu");
    if (!br_scene_is_loaded(e, s)) {
        return;
    }
    br_node root = br_scene_root_2d(e, s);
    for (int i = 1; i <= CR_MAX_LEVELS; i++) {
        char label[32];
        char score[16];
        SDL_snprintf(label, sizeof(label), "best_%d", i);
        SDL_snprintf(score, sizeof(score), "%d", (int)g->best[i]);
        br_loc_arg arg = { "score", score };
        cr_ui_text(e, root, label, g->best[i] > 0 ? br_loc_format(e, "menu.best", &arg, 1) : br_loc_get(e, "menu.not_played"));
    }
    br_loc_arg name = { "name", g->settings.name };
    cr_ui_text(e, root, "player_greeting", g->settings.name[0] != '\0' ? br_loc_format(e, "menu.greeting", &name, 1) : "");
}

/* The menu became active. */
static void cr_menu_fill(br_engine *e, cr_game *g)
{
    br_scene s = br_scene_active(e);
    cr_menu_texts(e, g);
    br_node_set_visible(e, cr_find(e, s, "levels_panel"), false);
    cr_log_stats(e, g, "menu");
    br_assets_report(e); /* every live asset with refs and owners: per-level assets must be gone */
}

static bool cr_active_is(br_engine *e, const char *name)
{
    return !br_scene_switching(e) && SDL_strcmp(br_scene_active_name(e), name) == 0 &&
           br_scene_is_loaded(e, br_scene_active(e));
}

static void cr_flow_system(br_engine *e, float dt, void *user)
{
    cr_game *g = user;
    const cr_tuning *t = cr_tuning_get(e, g);
    g->state_time += br_ticker_unscaled_delta(e); /* runs in the pause too (dt is 0 there) */
    cr_update_stats(e, g);
    switch (g->state) {
    case CR_STATE_BOOT:
        break;
    case CR_STATE_MENU:
        if (!g->filled && cr_active_is(e, "menu")) {
            g->filled = true;
            cr_menu_fill(e, g);
        }
        break;
    case CR_STATE_LOADING:
        if (cr_active_is(e, g->level_scene)) {
            cr_level_setup(e, g);
            cr_enter_state(e, g, CR_STATE_PLAYING);
        }
        break;
    case CR_STATE_PLAYING: {
        g->time_left -= dt;
        g->level_time += dt;
        int32_t whole = (int32_t)SDL_ceilf(g->time_left);
        if (whole <= 10 && whole > 0 && whole != g->last_tick) {
            g->last_tick = whole;
            cr_sound(e, g, "tick");
        }
        if (g->crates_total > 0 && g->crates_taken >= g->crates_total) {
            g->cleared = true;
            cr_enter_state(e, g, CR_STATE_LEVEL_END);
        } else if (g->time_left <= 0.0f) {
            g->time_left = 0.0f;
            cr_enter_state(e, g, CR_STATE_LEVEL_END);
        } else if (br_input_action_pressed(e, "pause")) {
            cr_enter_state(e, g, CR_STATE_PAUSED);
        }
        break;
    }
    case CR_STATE_PAUSED:
        if (g->capturing) {
            break;
        }
        if (SDL_strcmp(g->overlay_name, "settings") == 0 &&
            (br_input_action_pressed(e, "back") || br_input_action_pressed(e, "pause"))) {
            cr_open_overlay(e, g, "pause"); /* settings opened from the pause menu go back to it */
        } else if (g->state_time > 0.1f && (br_input_action_pressed(e, "pause") || br_input_action_pressed(e, "back"))) {
            cr_enter_state(e, g, CR_STATE_PLAYING);
        }
        break;
    case CR_STATE_LEVEL_END:
        if (g->state_time >= t->end_delay) {
            cr_finish_level(e, g);
            cr_enter_state(e, g, CR_STATE_RESULTS);
        }
        break;
    case CR_STATE_RESULTS:
        if (!g->filled && cr_active_is(e, "results")) {
            g->filled = true;
            cr_results_fill(e, g);
        }
        break;
    }
}

/* ---- buttons (ui.c calls this when one is activated) */

static int32_t cr_cycle(int32_t value, const int32_t *values, int count, int step)
{
    int at = 0;
    for (int i = 0; i < count; i++) {
        at = values[i] == value ? i : at;
    }
    return values[(at + step + count) % count];
}

/* A settings row changed by `step` (confirm / click = +1, left / right = -1 / +1). */
/* The next of game.json "game.languages" (["en", "de"]) after the current locale. */
static void cr_language_change(br_engine *e, cr_game *g, int step)
{
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(game, "languages");
    int count = cJSON_GetArraySize(list);
    if (count <= 0) {
        return;
    }
    int at = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *v = cJSON_GetArrayItem(list, i);
        at = cJSON_IsString(v) && SDL_strcmp(v->valuestring, br_loc_locale(e)) == 0 ? i : at;
    }
    const cJSON *next = cJSON_GetArrayItem(list, ((at + step) % count + count) % count);
    if (cJSON_IsString(next) && br_loc_set_locale(e, next->valuestring)) {
        br_str_copy(g->settings.language, sizeof(g->settings.language), next->valuestring);
        cr_menu_texts(e, g); /* bound texts follow by themselves; these are formatted */
    }
}

void cr_setting_change(br_engine *e, cr_game *g, int32_t key, int step)
{
    static const int32_t vsync_modes[] = { BR_VSYNC_OFF, BR_VSYNC_ON, BR_VSYNC_MAILBOX };
    static const int32_t fps_caps[] = { 0, 30, 60, 120, 144 };
    if (key == CR_SETTING_LANGUAGE) {
        cr_language_change(e, g, step);
    } else if (key == CR_SETTING_VSYNC) {
        g->settings.vsync = cr_cycle(g->settings.vsync, vsync_modes, 3, step);
    } else if (key == CR_SETTING_FPS) {
        g->settings.max_fps = cr_cycle(g->settings.max_fps, fps_caps, 5, step);
    } else if (key == CR_SETTING_VOLUME) {
        g->settings.volume = SDL_clamp(g->settings.volume + step * 10, 0, 100);
    } else if (key == CR_SETTING_PROFILER) {
        g->settings.profiler_hud = !g->settings.profiler_hud;
    } else if (key == CR_SETTING_BIND_JUMP) {
        br_input_device device = br_input_last_device(e) == BR_INPUT_DEVICE_GAMEPAD ? BR_INPUT_DEVICE_GAMEPAD
                                                                                     : BR_INPUT_DEVICE_KEYBOARD_MOUSE;
        g->capturing = true;
        g->capture_device = device;
        br_input_capture_start(e, device);
        return;
    }
    cr_settings_apply(e, g);
    cr_settings_save(e, g);
}

/* b's action; row is the settings row on the same node (NULL if none). */
void cr_button_action(br_engine *e, cr_game *g, const cr_button *b, const cr_setting *row)
{
    cr_ui_sound(e, g, "confirm");
    switch ((cr_action)b->action) {
    case CR_ACTION_PLAY:
        cr_start_level(e, g, b->arg[0] != '\0' ? b->arg : "level1");
        break;
    case CR_ACTION_LEVELS:
    case CR_ACTION_MAIN: {
        bool levels = b->action == CR_ACTION_LEVELS;
        br_scene s = br_scene_active(e);
        br_node_set_visible(e, cr_find(e, s, "levels_panel"), levels);
        br_node_set_visible(e, cr_find(e, s, "main_panel"), !levels);
        g->focus = 0;
        break;
    }
    case CR_ACTION_SETTINGS:
        cr_open_overlay(e, g, "settings");
        break;
    case CR_ACTION_CLOSE_SETTINGS:
        if (g->state == CR_STATE_PAUSED) {
            cr_open_overlay(e, g, "pause");
        } else {
            cr_close_overlay(e, g);
            cr_context(e, "menu");
        }
        break;
    case CR_ACTION_SETTING:
        if (row != NULL) {
            cr_setting_change(e, g, row->key, 1);
        }
        break;
    case CR_ACTION_QUIT:
        br_engine_quit(e);
        break;
    case CR_ACTION_RESUME:
        cr_enter_state(e, g, CR_STATE_PLAYING);
        break;
    case CR_ACTION_RESTART:
    case CR_ACTION_RETRY:
        cr_start_level(e, g, g->level_scene);
        break;
    case CR_ACTION_NEXT:
        if (b->arg[0] != '\0') {
            cr_start_level(e, g, b->arg);
        }
        break;
    case CR_ACTION_MENU:
        cr_enter_state(e, g, CR_STATE_MENU);
        break;
    case CR_ACTION_EDIT_NAME:
        /* the menu's focus landed on the name row: the field takes the keyboard */
        br_text_field_focus(e, cr_find(e, br_scene_active(e), "player_name"));
        break;
    case CR_ACTION_NONE:
    case CR_ACTION_COUNT:
        break;
    }
}

void cr_flow_register(br_engine *e, cr_game *g)
{
    br_arena_linear_create(&g->level_arena, "crate_rush.level", 16 * 1024, true);
    br_arena_track(e, &g->level_arena.stats); /* the profiler's memory tab lists it */
    g->speed = br_ticker_speed(e); /* game.json "ticker.speed"; the pause sets 0 and puts it back */
    cr_settings_load(e, g);
    cr_settings_apply(e, g);
    br_scene_on_event(e, cr_on_scene_event, g);
    br_gltf_options levels;
    SDL_zero(levels);
    levels.camera_as_br_camera = true; /* a level's camera follows and shakes (br_camera) */
    br_gltf_set_load_options(e, &levels);
    br_system_add(e, "cr.flow", BR_PHASE_UPDATE, 0, cr_flow_system, g);
}
