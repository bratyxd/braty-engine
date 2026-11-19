/* Star Courier - a small arcade game made with Braty: steer a courier ship, pick up cargo
 * crates, deliver them to a station that moves after every delivery, dodge drifting debris.
 *
 * Everything is data (data/game.json + data/assets/); this file boots it. Command line: the
 * engine's (--frames N --screenshot out.png --gpu-driver NAME --playtest FILE ...) plus
 *   --debug      opens the debug overlay (F1 toggles it).
 * Play-test scripts are in playtest/: `star_courier --playtest playtest/round.json` plays a
 * round and checks score, lives and state (see docs/PLAYTESTING.md).
 */
#include "braty/braty_main.h"
#include "game.h"

static void sc_on_bundle(br_engine *e, const char *bundle, float progress, void *user)
{
    (void)bundle;
    sc_game *g = user;
    if (progress < 1.0f || g->assets_ready) {
        return;
    }
    g->assets_ready = true;
    g->music = br_sound_load(e, sc_config_string(e, NULL, "music"));
    if (g->music != NULL) {
        br_audio_play_desc play = br_audio_play_defaults();
        play.loop = true;
        play.bus = g->music_bus;
        g->music_instance = br_audio_play(e, g->music, &play);
    }
}

static void sc_shutdown(br_engine *e, void *user)
{
    sc_log_stats(e, "shutdown");
    br_free(user);
}

/* What play-test scripts can read and write as "game:<field>" (docs/PLAYTESTING.md). */
static void sc_expose(br_engine *e, sc_game *g)
{
    static const br_field_desc fields[] = {
        { "state", BR_FIELD_STRING, offsetof(sc_game, state_name) },
        { "score", BR_FIELD_INT, offsetof(sc_game, score) },
        { "best", BR_FIELD_INT, offsetof(sc_game, best) },
        { "lives", BR_FIELD_INT, offsetof(sc_game, lives) },
        { "level", BR_FIELD_INT, offsetof(sc_game, level) },
        { "deliveries", BR_FIELD_INT, offsetof(sc_game, deliveries) },
        { "paused", BR_FIELD_BOOL, offsetof(sc_game, paused) },
    };
    br_playtest_expose(e, "game", fields, BR_ARRAY_COUNT(fields), g);
}

static bool sc_init(br_engine *e)
{
    sc_game *g = br_alloc(sizeof(sc_game));
    if (g == NULL) {
        return false;
    }
    g->rng = 0x2545f491u;
    br_engine_on_shutdown(e, sc_shutdown, g);
    sc_components_register(e, g); /* before any file that uses them is loaded */
    sc_systems_register(e, g);
    sc_expose(e, g);

    /* game.json (window, input, audio, manifest) was applied before init; there is no
     * start_scene: the state machine loads game.scenes.* */
    g->tuning_node = br_scene_file_load(e, sc_config_string(e, NULL, "tuning"), br_scene_root(e));
    if (br_component_get(e, g->tuning_node, g->id.tuning) == NULL) {
        return false; /* reasons are logged with file and JSON path */
    }
    g->sfx = br_sound_load(e, sc_config_string(e, NULL, "sfx")); /* its sprites come from game.json */
    g->sfx_bus = br_audio_bus_find(e, "sfx");
    g->music_bus = br_audio_bus_find(e, "music");
    void *best = NULL;
    size_t size = 0;
    /* deterministic runs (play-tests) must not depend on what an earlier run saved */
    if (!br_app_deterministic(e) && br_save_read(e, "best.txt", &best, &size)) {
        g->best = SDL_atoi(best);
        br_free(best);
    }
    br_assets_load_bundle(e, "game", sc_on_bundle, g); /* music + textures in the background */
    sc_enter_state(e, g, SC_STATE_TITLE);
    sc_log_stats(e, "start");
    return br_node_valid(e, g->scene);
}

br_app_desc br_main(int argc, char **argv)
{
    /* game.json next to the executable; --debug, --scene from the command line */
    return br_game_config_main("game.json", argc, argv, sc_init);
}
