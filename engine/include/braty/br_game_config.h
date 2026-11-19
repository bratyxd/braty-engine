/* br_game_config.h - game.json: window, asset root, manifest, start scene, ticker, clear
 * colour, input actions, audio buses, debug overlay. A whole br_main can be:
 *
 *   static br_game_config config;   (the desc returned by br_main points into it)
 *
 *   static bool game_init(br_engine *e)
 *   {
 *       ...register components and systems...
 *       return br_node_valid(e, br_game_config_apply(e, &config));   (loads the start scene)
 *   }
 *
 *   br_app_desc br_main(int argc, char **argv)
 *   {
 *       br_game_config_load(NULL, "game.json", &config);   (no engine yet: read from the exe dir)
 *       br_game_config_args(&config, argc, argv);           (--debug, --scene path)
 *       br_app_desc desc = br_game_config_app_desc(&config);
 *       desc.init = game_init;
 *       return desc;
 *   }
 *
 * Every key is optional; see docs/DATA_FORMATS.md for the full file. The whole document is kept
 * for the game (br_game_config_json); put game settings under "game" (other unknown keys warn).
 */
#ifndef BR_GAME_CONFIG_H
#define BR_GAME_CONFIG_H

#include "braty/br_app.h"
#include "braty/br_render.h"

#define BR_GAME_CONFIG_PATH_SIZE 256

/* Everything game.json sets. br_game_config_load fills defaults first. */
typedef struct br_game_config {
    char     title[128];                           /* "braty" */
    int      width, height;                        /* 1280 x 720 */
    bool     fullscreen;                           /* false */
    bool     vsync;                                /* true */
    bool     resizable;                            /* true */
    char     asset_root[BR_GAME_CONFIG_PATH_SIZE]; /* relative to the executable dir; "" = exe dir */
    char     manifest[BR_GAME_CONFIG_PATH_SIZE];   /* asset manifest added at apply; "" = none */
    char     start_scene[BR_GAME_CONFIG_PATH_SIZE];/* scene loaded under the stage at apply; "" = none */
    float    max_fps;                              /* 0 = uncapped */
    float    min_fps;                              /* 10 */
    float    speed;                                /* 1 */
    bool     has_clear_color;
    br_color clear_color;                          /* window background (when has_clear_color) */
    bool     debug_overlay;                        /* debug overlay open at start (false) */
    char     debug_key[32];                        /* SDL key name toggling it, "F1" */
    bool     hot_reload;                           /* scene hot reload (debug builds: true) */
    cJSON   *json;                                 /* the whole document (owned) */
} br_game_config;

/* Defaults only (no file). */
void br_game_config_defaults(br_game_config *config);
/* Parses game.json text into *config (defaults first). Bad values are logged
 * ("source: window.width: expected a number") and keep their default. False on a JSON syntax error. */
bool br_game_config_parse(const char *json, size_t size, const char *source_name, br_game_config *config);
/* Reads and parses a game.json file. The path is relative to the executable directory (works
 * before an engine exists; e may be NULL). False if missing or not JSON (defaults are kept). */
bool br_game_config_load(br_engine *e, const char *path, br_game_config *config);
/* Applies command-line options: --debug (overlay open), --scene <path> (start scene). */
void br_game_config_args(br_game_config *config, int argc, char **argv);
/* A br_app_desc with title, size and asset root pointing into config (keep config alive). */
br_app_desc br_game_config_app_desc(const br_game_config *config);
/* Applies everything else to a running engine: vsync, fullscreen, resizable, ticker, clear
 * colour, input actions, audio, manifest, debug overlay, hot reload; then loads the start scene
 * under the stage and returns its root (zero handle if none or it failed). The engine takes
 * over config->json (it is set to NULL). */
br_node br_game_config_apply(br_engine *e, br_game_config *config);
/* The game.json document given to br_game_config_apply (custom game keys), NULL if none. */
const cJSON *br_game_config_json(br_engine *e);
/* Frees config->json (only needed if br_game_config_apply was never called). */
void br_game_config_free(br_game_config *config);

#endif
