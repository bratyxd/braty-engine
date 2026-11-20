/* flow.c - the game's states and scenes:
 *
 *   menu --play--> loading (br_scene_switch with the "loading" scene) --> playing <--> paused
 *     ^                                                                   |
 *     +------ results <-- level_end (all crates, or the clock ran out) <--+
 *
 * Systems: cr.flow (UPDATE 0). Scene events reset the level state when a level unloads.
 * A level is a .glb loaded as a scene; on activation this file reads its scene extras into a
 * "level" component, builds the collider list in the level arena, puts a following br_camera
 * over the level's 3D root (taking its pose from the file's camera), spawns the player and loads
 * the HUD on top.
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

/* Plays a sound by path (already loaded by a group: no reference is taken here). */
void cr_play(br_engine *e, cr_game *g, const char *path, float volume)
{
    br_sound *s = path[0] != '\0' ? br_assets_get(e, path) : NULL;
    if (s == NULL) {
        return;
    }
    br_audio_play_desc play = br_audio_play_defaults();
    play.volume = volume;
    play.bus = g->sfx_bus;
    br_audio_play(e, s, &play);
}

/* Plays the sound game.json names under "game.sounds.<name>". */
void cr_sound(br_engine *e, cr_game *g, const char *name)
{
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(game, "sounds"), name);
    if (cJSON_IsString(v)) {
        cr_play(e, g, v->valuestring, 1.0f);
    }
}

br_ease_type cr_ease(const char *name)
{
    bool ok = false;
    br_ease_type t = br_ease_from_name(name, &ok);
    return ok ? t : BR_EASE_OUT_QUAD;
}

float cr_random(cr_game *g)
{
    /* xorshift32: deterministic, so play-test runs give identical frames */
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return (float)(g->rng >> 8) / 16777216.0f;
}

void cr_log_stats(br_engine *e, cr_game *g, const char *when)
{
    br_memory_stats heap;
    br_memory_get_stats(&heap);
    uint32_t animators = 0;
    uint32_t tracks = 0;
    br_animation_stats(e, &animators, &tracks);
    g->stat_nodes = (int32_t)br_scene_node_count(e);
    g->stat_nodes3d = (int32_t)br_node3d_count(e);
    g->stat_assets = (int32_t)br_assets_count(e);
    g->stat_animators = (int32_t)animators;
    g->stat_tweens = (int32_t)br_tween_count(e);
    g->stat_sounds = (int32_t)br_audio_instance_count(e);
    br_log_info("crate_rush stats (%s): nodes %u, 3d nodes %u, assets %u, animators %u, tweens %u, sounds %u, "
                "level arena %llu bytes, heap %llu blocks, levels done %u",
                when, br_scene_node_count(e), br_node3d_count(e), br_assets_count(e), animators, br_tween_count(e),
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
    char text[256];
    SDL_snprintf(text, sizeof(text), "{ \"vsync\": %d, \"max_fps\": %d, \"volume\": %d, \"profiler_hud\": %s }\n",
                 (int)g->settings.vsync, (int)g->settings.max_fps, (int)g->settings.volume,
                 g->settings.profiler_hud ? "true" : "false");
    br_save_write(e, "settings.json", text, SDL_strlen(text));
    br_input_bindings_save(e, "bindings.json");
}

static void cr_settings_load(br_engine *e, cr_game *g)
{
    g->settings.vsync = (int32_t)br_app_vsync(e);
    g->settings.max_fps = (int32_t)br_app_max_fps(e);
    g->settings.volume = 80;
    g->settings.profiler_hud = false;
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
    for (br_node3d c = br_node3d_first_child(e, root); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        if (br_node3d_type_of(e, c) == br_model3d_type(e)) {
            return c;
        }
    }
    br_node3d none = { 0, 0 };
    return none;
}

static br_color cr_extras_color(br_engine *e, const cJSON *extras, const char *key, uint32_t fallback)
{
    br_color c = br_color_from_hex(fallback, 1.0f);
    if (cJSON_HasObjectItem(extras, key)) {
        br_data_get_color(e, extras, key, &c);
    }
    return c;
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
        br_mat4 world = br_node3d_world_matrix(e, n);
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
    g->level_view = cr_find(e, g->level, g->level_scene); /* the view is labelled after the file */
    g->level_root = br_scene_root_3d(e, g->level);
    br_node3d inst = cr_level_instance(e, g->level_root);
    const br_model *model = br_model3d_model(e, inst);

    /* level settings live in the glTF scene extras: {"level": {...}, "sky": ..., "background": ...} */
    cJSON *extras = model != NULL && model->scene_extras != NULL ? cJSON_Parse(model->scene_extras) : NULL;
    br_scene_file_apply_component(e, g->level_view, "level", cJSON_GetObjectItemCaseSensitive(extras, "level"));
    const cr_level *lvl = br_component_get(e, g->level_view, g->id.level);
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

    /* the following camera: a br_camera over the level's 3D root, starting where the file's camera
     * is. The .glb view stays hidden so the level is not drawn twice (docs/API_REVIEW.md). */
    br_node3d file_cam = br_model3d_camera(e, inst, NULL);
    br_camera c = br_camera_default_3d();
    const br_camera3d *fc = br_camera3d_get(e, file_cam);
    if (fc != NULL) {
        c.field_of_view = fc->fov_y;
        c.near_z = fc->near_z;
        c.far_z = fc->far_z;
    }
    c.background_color = cr_extras_color(e, extras, "background", 0x5fa8de);
    g->camera = br_camera_create_3d(e, g->level_root, &c);
    br_node_set_label(e, g->camera, "level_camera");
    br_node_add_child(e, br_scene_root_2d(e, g->level), g->camera); /* dies with the level */
    br_node cam_view = br_camera_view3d(e, g->camera);
    float ambient = 0.45f;
    const cJSON *amb = cJSON_GetObjectItemCaseSensitive(extras, "ambient");
    if (cJSON_IsNumber(amb)) {
        ambient = (float)amb->valuedouble;
    }
    br_view3d_set_environment(e, cam_view, br_color_to_linear(cr_extras_color(e, extras, "sky", 0xbfe3ff)),
                              br_color_to_linear(cr_extras_color(e, extras, "ground", 0x6f8f5a)), ambient);
    br_view3d_set_msaa(e, cam_view, 4);
    br_post3d_settings *post = br_view3d_post(e, cam_view);
    if (post != NULL) {
        post->bloom_threshold = t->bloom_threshold;
        post->bloom_intensity = t->bloom_intensity;
    }
    br_node_set_visible(e, g->level_view, false);
    cJSON_Delete(extras);

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
    br_vec3 eye = br_node3d_world_position(e, file_cam);
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
    g->level_view = none;
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
            br_animator_set_paused(e, g->animator, false);
            cr_context(e, "gameplay");
        }
        break;
    case CR_STATE_PAUSED:
        br_system_group_set_enabled(e, "gameplay", false);
        br_animator_set_paused(e, g->animator, true);
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
    const cr_level *lvl = br_component_get(e, g->level_view, g->id.level);
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

/* The results scene became active: fill it in. */
static void cr_results_fill(br_engine *e, cr_game *g)
{
    br_scene s = br_scene_active(e);
    char text[64];
    cr_ui_text(e, br_scene_root_2d(e, s), "result_title", g->cleared ? "LEVEL CLEAR!" : "TIME UP!");
    cr_ui_text(e, br_scene_root_2d(e, s), "result_level", g->level_title);
    SDL_snprintf(text, sizeof(text), "%d / %d", (int)g->result_crates, (int)g->result_total);
    cr_ui_text(e, br_scene_root_2d(e, s), "result_crates", text);
    cr_ui_count_to(e, g, br_scene_root_2d(e, s), "result_score", (float)g->score);
    cr_ui_count_to(e, g, br_scene_root_2d(e, s), "result_bonus", (float)g->result_time_bonus);
    cr_ui_count_to(e, g, br_scene_root_2d(e, s), "result_total", (float)g->result_score);
    cr_ui_text(e, br_scene_root_2d(e, s), "result_best", g->result_new_best ? "NEW BEST!" : "");
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

/* The menu became active: best scores in the level select. */
static void cr_menu_fill(br_engine *e, cr_game *g)
{
    br_scene s = br_scene_active(e);
    for (int i = 1; i <= CR_MAX_LEVELS; i++) {
        char label[32];
        char text[48];
        SDL_snprintf(label, sizeof(label), "best_%d", i);
        if (g->best[i] > 0) {
            SDL_snprintf(text, sizeof(text), "BEST %d", (int)g->best[i]);
        } else {
            br_str_copy(text, sizeof(text), "NOT PLAYED");
        }
        cr_ui_text(e, br_scene_root_2d(e, s), label, text);
    }
    br_node_set_visible(e, cr_find(e, s, "levels_panel"), false);
    cr_log_stats(e, g, "menu");
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
    g->state_time += dt;
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
void cr_setting_change(br_engine *e, cr_game *g, const char *key, int step)
{
    static const int32_t vsync_modes[] = { BR_VSYNC_OFF, BR_VSYNC_ON, BR_VSYNC_MAILBOX };
    static const int32_t fps_caps[] = { 0, 30, 60, 120, 144 };
    if (SDL_strcmp(key, "vsync") == 0) {
        g->settings.vsync = cr_cycle(g->settings.vsync, vsync_modes, 3, step);
    } else if (SDL_strcmp(key, "fps") == 0) {
        g->settings.max_fps = cr_cycle(g->settings.max_fps, fps_caps, 5, step);
    } else if (SDL_strcmp(key, "volume") == 0) {
        g->settings.volume = SDL_clamp(g->settings.volume + step * 10, 0, 100);
    } else if (SDL_strcmp(key, "profiler") == 0) {
        g->settings.profiler_hud = !g->settings.profiler_hud;
    } else if (SDL_strcmp(key, "bind_jump") == 0) {
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

void cr_button_action(br_engine *e, cr_game *g, const cr_button *b)
{
    const char *a = b->action;
    cr_sound(e, g, "confirm");
    if (SDL_strcmp(a, "play") == 0) {
        cr_start_level(e, g, b->arg[0] != '\0' ? b->arg : "level1");
    } else if (SDL_strcmp(a, "levels") == 0 || SDL_strcmp(a, "main") == 0) {
        bool levels = SDL_strcmp(a, "levels") == 0;
        br_scene s = br_scene_active(e);
        br_node_set_visible(e, cr_find(e, s, "levels_panel"), levels);
        br_node_set_visible(e, cr_find(e, s, "main_panel"), !levels);
        g->focus = 0;
    } else if (SDL_strcmp(a, "settings") == 0) {
        cr_open_overlay(e, g, "settings");
    } else if (SDL_strcmp(a, "close_settings") == 0) {
        if (g->state == CR_STATE_PAUSED) {
            cr_open_overlay(e, g, "pause");
        } else {
            cr_close_overlay(e, g);
            cr_context(e, "menu");
        }
    } else if (SDL_strcmp(a, "setting") == 0) {
        cr_setting_change(e, g, b->arg, 1);
    } else if (SDL_strcmp(a, "quit") == 0) {
        br_engine_quit(e);
    } else if (SDL_strcmp(a, "resume") == 0) {
        cr_enter_state(e, g, CR_STATE_PLAYING);
    } else if (SDL_strcmp(a, "restart") == 0 || SDL_strcmp(a, "retry") == 0) {
        cr_start_level(e, g, g->level_scene);
    } else if (SDL_strcmp(a, "next") == 0 && b->arg[0] != '\0') {
        cr_start_level(e, g, b->arg);
    } else if (SDL_strcmp(a, "menu") == 0) {
        cr_enter_state(e, g, CR_STATE_MENU);
    }
}

void cr_flow_register(br_engine *e, cr_game *g)
{
    br_arena_linear_create(&g->level_arena, "crate_rush.level", 16 * 1024, true);
    br_arena_track(e, &g->level_arena.stats); /* the profiler's memory tab lists it */
    cr_settings_load(e, g);
    cr_settings_apply(e, g);
    br_scene_on_event(e, cr_on_scene_event, g);
    br_system_add(e, "cr.flow", BR_PHASE_UPDATE, 0, cr_flow_system, g);
}
