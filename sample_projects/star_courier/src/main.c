/* Star Courier - a small arcade game made with Braty: steer a courier ship, pick up cargo
 * crates, deliver them to a station that moves after every delivery, dodge drifting debris.
 *
 * Everything is data (data/game.json + data/assets/); this file boots it. Command line, on top
 * of the engine's (--frames N --screenshot out.png --gpu-driver NAME):
 *   --autoplay   scripted input (title -> play -> pause -> game over -> restart ...), for
 *                screenshots and soak tests; deterministic with --frames.
 *   --debug      opens the debug overlay (F1 toggles it).
 */
#include "braty/braty_main.h"
#include "game.h"

/* What br_main hands to init. br_main runs before the engine exists and br_app_desc.init gets no
 * user pointer, so this is the one static (see docs/API_REVIEW.md); nothing changes it later. */
typedef struct sc_boot {
    br_game_config config;   /* br_main's desc points into it */
    bool           autoplay;
} sc_boot;

static sc_boot s_boot;

/* ---- --autoplay: injects real key events, the same path a keyboard takes */

enum { SC_BOT_LEFT = 1, SC_BOT_RIGHT = 2, SC_BOT_UP = 4, SC_BOT_DOWN = 8 };

static void sc_bot_tap(br_engine *e, sc_game *g, SDL_Scancode key)
{
    if (g->bot_tap == SDL_SCANCODE_UNKNOWN) {
        br_input_inject_key(e, key, true);
        g->bot_tap = key;
    }
}

/* Where the bot wants to go: cargo, then the station; after 14 s of a round it hunts debris so
 * the run reaches the game-over screen. */
static br_vec2 sc_bot_direction(br_engine *e, sc_game *g)
{
    const sc_pilot *pilot = br_component_get(e, g->ship, g->id.pilot);
    br_vec2 ship = br_node_position(e, g->ship);
    bool hunt = g->state_time > 14.0f;
    br_vec2 target = br_node_position(e, g->station);
    float best = 1e9f;
    uint32_t goal = hunt ? g->id.debris : g->id.crate;
    if (hunt || (pilot != NULL && !pilot->carrying)) {
        for (uint32_t i = 0; i < br_component_count(e, goal); i++) {
            br_node n = br_component_owner_at(e, goal, i);
            float d = br_vec2_length(br_vec2_sub(br_node_position(e, n), ship));
            if (br_node_valid(e, n) && d < best) {
                best = d;
                target = br_node_position(e, n);
            }
        }
    }
    br_vec2 want = br_vec2_normalize(br_vec2_sub(target, ship));
    for (uint32_t i = 0; !hunt && i < br_component_count(e, g->id.debris); i++) {
        br_node n = br_component_owner_at(e, g->id.debris, i);
        br_vec2 away = br_vec2_sub(ship, br_node_position(e, n));
        float d = br_vec2_length(away);
        if (br_node_valid(e, n) && d < 130.0f && d > 0.0f) {
            want = br_vec2_add(want, br_vec2_scale(away, 1.6f * (130.0f - d) / (130.0f * d)));
        }
    }
    return want;
}

static void sc_autoplay_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    sc_game *g = user;
    if (g->bot_tap != SDL_SCANCODE_UNKNOWN) {
        br_input_inject_key(e, g->bot_tap, false); /* taps last one frame */
        g->bot_tap = SDL_SCANCODE_UNKNOWN;
        return;
    }
    uint32_t keys = 0;
    if (g->state == SC_STATE_TITLE && g->assets_ready && g->state_time > 1.5f) {
        sc_bot_tap(e, g, SDL_SCANCODE_RETURN);
    } else if (g->state == SC_STATE_GAMEOVER && g->state_time > 2.5f) {
        sc_log_stats(e, "game over"); /* the play scene is freed by now: compare rounds */
        /* every second game over goes back to the title, so all three scenes get reloaded */
        sc_bot_tap(e, g, g->round % 2 == 0 ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_RETURN);
    } else if (g->state == SC_STATE_PLAY) {
        bool pause_window = g->round == 1 && g->wall_time > 6.0f && g->wall_time < 6.6f;
        if (pause_window != g->paused) {
            sc_bot_tap(e, g, SDL_SCANCODE_P); /* pause for ~0.6 s in the first round */
        }
        if (!g->paused) {
            br_vec2 want = sc_bot_direction(e, g);
            keys |= want.x < -0.3f ? SC_BOT_LEFT : 0;
            keys |= want.x > 0.3f ? SC_BOT_RIGHT : 0;
            keys |= want.y < -0.3f ? SC_BOT_UP : 0;
            keys |= want.y > 0.3f ? SC_BOT_DOWN : 0;
        }
    }
    const SDL_Scancode codes[4] = { SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN };
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t bit = 1u << i;
        if ((keys & bit) != (g->bot_keys & bit)) {
            br_input_inject_key(e, codes[i], (keys & bit) != 0);
        }
    }
    g->bot_keys = keys;
}

/* ---- boot */

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

static bool sc_init(br_engine *e)
{
    sc_game *g = br_alloc(sizeof(sc_game));
    if (g == NULL) {
        return false;
    }
    g->autoplay = s_boot.autoplay;
    g->rng = 0x2545f491u;
    br_engine_on_shutdown(e, sc_shutdown, g);
    sc_components_register(e, g); /* before any file that uses them is loaded */
    sc_systems_register(e, g);
    if (g->autoplay) {
        br_system_add(e, "sc.autoplay", BR_PHASE_INPUT, -3000000, sc_autoplay_system, g); /* before input latches */
    }

    /* window, input, audio, manifest; no start_scene: the state machine loads game.scenes.* */
    br_game_config_apply(e, &s_boot.config);
    g->tuning_node = br_scene_file_load(e, sc_config_string(e, NULL, "tuning"), br_scene_root(e));
    if (br_component_get(e, g->tuning_node, g->id.tuning) == NULL) {
        return false; /* reasons are logged with file and JSON path */
    }
    g->sfx = br_sound_load(e, sc_config_string(e, NULL, "sfx")); /* its sprites come from game.json */
    g->sfx_bus = br_audio_bus_find(e, "sfx");
    g->music_bus = br_audio_bus_find(e, "music");
    void *best = NULL;
    size_t size = 0;
    if (!g->autoplay && br_save_read(e, "best.txt", &best, &size)) {
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
    for (int i = 1; i < argc; i++) {
        s_boot.autoplay = s_boot.autoplay || SDL_strcmp(argv[i], "--autoplay") == 0;
    }
    br_game_config_load(NULL, "game.json", &s_boot.config); /* next to the executable */
    br_game_config_args(&s_boot.config, argc, argv);        /* --debug, --scene */
    br_app_desc desc = br_game_config_app_desc(&s_boot.config);
    desc.init = sc_init;
    return desc;
}
