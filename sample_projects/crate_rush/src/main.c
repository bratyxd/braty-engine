/* Crate Rush - a small 3D game made with Braty: run around little islands collecting crates
 * before the clock runs out, dodging spikes and saws. Three levels, a menu with level select and
 * settings, pause, results with stars and best scores.
 *
 * Everything is data (data/game.json + data/assets/); the C code is components (components.c)
 * and systems (flow.c, player.c, world.c, ui.c). Command line: the engine's (--frames N
 * --screenshot out.png --gpu-driver NAME --vsync off --profiler-hud --playtest FILE ...).
 * Play-test scripts are in playtest/ (docs/PLAYTESTING.md).
 */
#include "braty/braty_main.h"
#include "game.h"

static const char *cr_clip_names[CR_CLIPS] = { "idle", "walk", "sprint", "jump", "fall" };

/* Copies of the character's glTF clips with "step" markers where the feet land (the markers
 * cannot be added to the model's own clips from data; docs/API_REVIEW.md). */
static void cr_make_clips(br_engine *e, cr_game *g)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    for (uint32_t i = 0; i < CR_CLIPS; i++) {
        int32_t index = br_model_find_clip(g->character_model, cr_clip_names[i]);
        br_anim_clip *clip = index >= 0 ? br_anim_clip_from_gltf(g->character_model, (uint32_t)index) : NULL;
        if (clip == NULL) {
            br_log_error("crate_rush: the character has no '%s' clip", cr_clip_names[i]);
            continue;
        }
        if (SDL_strcmp(cr_clip_names[i], "walk") == 0) {
            br_anim_clip_add_marker(clip, t->footstep_walk.x, "step");
            br_anim_clip_add_marker(clip, t->footstep_walk.y, "step");
        } else if (SDL_strcmp(cr_clip_names[i], "sprint") == 0) {
            br_anim_clip_add_marker(clip, t->footstep_sprint.x, "step");
            br_anim_clip_add_marker(clip, t->footstep_sprint.y, "step");
        }
        g->clips[g->clip_count++] = clip;
    }
}

static void cr_shutdown(br_engine *e, void *user)
{
    cr_game *g = user;
    cr_log_stats(e, g, "shutdown");
    /* the animator borrows our clips: remove it before freeing them */
    if (br_animator_valid(e, g->animator)) {
        br_animator_remove(e, g->animator);
    }
    for (uint32_t i = 0; i < g->clip_count; i++) {
        br_anim_clip_destroy(g->clips[i]);
    }
    if (g->sdf_font != NULL) {
        br_font_destroy(e, g->sdf_font);
    }
    br_asset_group_destroy(g->shared);
    br_arena_untrack(e, &g->level_arena.stats);
    br_arena_linear_destroy(&g->level_arena);
    br_free(g);
}

/* What play-test scripts can read and write as "game:<field>" (docs/PLAYTESTING.md). */
static void cr_expose(br_engine *e, cr_game *g)
{
    static const br_field_desc fields[] = {
        { "state", BR_FIELD_STRING, offsetof(cr_game, state_name) },
        { "level", BR_FIELD_STRING, offsetof(cr_game, level_scene) },
        { "score", BR_FIELD_INT, offsetof(cr_game, score) },
        { "crates", BR_FIELD_INT, offsetof(cr_game, crates_taken) },
        { "crates_total", BR_FIELD_INT, offsetof(cr_game, crates_total) },
        { "time_left", BR_FIELD_FLOAT, offsetof(cr_game, time_left) },
        { "hits", BR_FIELD_INT, offsetof(cr_game, hits) },
        { "falls", BR_FIELD_INT, offsetof(cr_game, falls) },
        { "result_score", BR_FIELD_INT, offsetof(cr_game, result_score) },
        { "result_stars", BR_FIELD_INT, offsetof(cr_game, result_stars) },
        { "colliders", BR_FIELD_INT, offsetof(cr_game, collider_count) },
        { "candidates_peak", BR_FIELD_INT, offsetof(cr_game, candidates_peak) },
        { "levels_completed", BR_FIELD_INT, offsetof(cr_game, levels_completed) },
        { "focus", BR_FIELD_INT, offsetof(cr_game, focus) },
        { "anim_state", BR_FIELD_STRING, offsetof(cr_game, anim_state) },
        { "stat_nodes", BR_FIELD_INT, offsetof(cr_game, stat_nodes) },
        { "stat_nodes3d", BR_FIELD_INT, offsetof(cr_game, stat_nodes3d) },
        { "stat_assets", BR_FIELD_INT, offsetof(cr_game, stat_assets) },
        { "stat_animators", BR_FIELD_INT, offsetof(cr_game, stat_animators) },
        { "stat_tweens", BR_FIELD_INT, offsetof(cr_game, stat_tweens) },
        { "stat_sounds", BR_FIELD_INT, offsetof(cr_game, stat_sounds) },
    };
    br_playtest_expose(e, "game", fields, BR_ARRAY_COUNT(fields), g);
}

/* The persistent 2D camera every screen is drawn through: the 1280 x 720 design rect always fits
 * (FIT_EXPAND), and HUD nodes anchor to its edges inside the safe area (camera.anchor). */
static void cr_make_ui_camera(br_engine *e, cr_game *g)
{
    br_camera c = br_camera_default_2d();
    c.fit = BR_CAMERA_FIT_EXPAND;
    c.reference_aspect = 16.0f / 9.0f;
    c.depth = 10.0f;
    c.clear_flags = BR_CAMERA_CLEAR_NOTHING;
    g->ui_camera = br_camera_create(e, &c);
    br_node_set_label(e, g->ui_camera, "ui_camera");
    br_node_set_position(e, g->ui_camera, 640.0f, 360.0f);
    br_node_add_child(e, br_scene_root(e), g->ui_camera);
    br_node_set_persistent(e, g->ui_camera, true);
}

static bool cr_init(br_engine *e)
{
    cr_game *g = br_alloc(sizeof(cr_game));
    if (g == NULL) {
        return false;
    }
    g->rng = 0x2545f491u;
    br_engine_on_shutdown(e, cr_shutdown, g);
    cr_components_register(e, g); /* before any file that fills them is loaded */
    cr_flow_register(e, g);
    cr_player_register(e, g);
    cr_world_register(e, g);
    cr_ui_register(e, g);
    cr_expose(e, g);

    g->tuning_node = br_scene_file_load(e, cr_config_string(e, "tuning"), br_scene_root(e));
    if (br_component_get(e, g->tuning_node, g->id.tuning) == NULL) {
        return false; /* reasons are logged with file and JSON path */
    }
    br_node_set_persistent(e, g->tuning_node, true);
    const cr_tuning *t = cr_tuning_get(e, g);

    /* shared assets: every screen and level uses them, so one group holds them for the whole run */
    g->shared = br_asset_group_create(e, "shared");
    br_asset_group_add_manifest_bundle(g->shared, cr_config_string(e, "shared_bundle"));
    br_asset_group_load(g->shared);
    g->character_model = br_asset_group_get(g->shared, t->character);
    g->graph = br_asset_group_get(g->shared, t->animgraph);
    if (g->character_model == NULL || g->graph == NULL) {
        return false;
    }
    cr_make_clips(e, g);
    void *ttf = NULL;
    size_t size = 0;
    if (br_file_read_all(e, t->sdf_font, &ttf, &size)) {
        br_font_options opts = { true, 64.0f, 12 };
        g->sdf_font = br_font_create(e, ttf, size, &opts);
        br_free(ttf);
    }
    g->sfx_bus = br_audio_bus_find(e, "sfx");
    g->music_bus = br_audio_bus_find(e, "music");
    cr_make_ui_camera(e, g);
    cr_enter_state(e, g, CR_STATE_MENU);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, cr_init);
}
