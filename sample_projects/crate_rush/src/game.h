/* game.h - Crate Rush: component structs (filled from glTF node extras, scene files and
 * tuning.json through their field lists), the one game-state struct, and the system entry points.
 *
 * Data lives in data/: game.json (window, scenes, input actions, audio), assets/tuning.json (every
 * gameplay number), assets/levels/levelN.glb (the levels: kit meshes + extras components +
 * scene-extras settings, written by tools/build_assets.py), assets/scenes/ (menu, loading, HUD,
 * pause, settings, results), assets/animation/character.animgraph.json. C only says how the
 * components behave.
 */
#ifndef CRATE_RUSH_GAME_H
#define CRATE_RUSH_GAME_H

#include "braty/braty.h"

/* ---- 3D components (glTF node extras of the level files) */

/* "spawn_point": where the player starts and respawns. */
typedef struct cr_spawn_point {
    float yaw;        /* degrees the character faces */
} cr_spawn_point;

/* "crate": collect it for `value` points. */
typedef struct cr_crate {
    int32_t value;
    bool    taken;    /* runtime: picked up, playing its pop */
} cr_crate;

/* "hazard": touching it costs time and knocks the player back. Saws move along `move`. */
typedef struct cr_hazard {
    float   radius;   /* touch distance (horizontal) */
    br_vec3 move;     /* half the travel: position = base + move * sin(time * speed + phase) */
    float   speed;
    float   spin;     /* radians per second around the node's Z (saw blades) */
    float   phase;
    /* runtime */
    bool    ready;
    br_vec3 base;
} cr_hazard;

/* "sign": a sign post that gets a world-space name plate; it says `hint` while the player is near. */
typedef struct cr_sign {
    char  text[BR_FIELD_STRING_SIZE];
    char  hint[BR_FIELD_STRING_SIZE];
    float radius;
    /* runtime */
    br_node3d plate;   /* the canvas3d made for it */
    bool      near;
} cr_sign;

/* "player": added by code to the character instance; physics state of the fixed-step simulation. */
typedef struct cr_player {
    br_vec3 position;       /* feet, this fixed step */
    br_vec3 previous;       /* feet, the step before (render interpolation) */
    br_vec3 velocity;
    float   yaw;            /* radians, the way the model faces */
    bool    grounded;
    float   coyote;         /* seconds left to jump after leaving the ground */
    float   jump_buffer;    /* seconds a jump press is remembered */
    float   invulnerable;   /* seconds left after a hit */
    bool    jumped;         /* set by the step that jumped, read by the animation system */
    bool    landed;
} cr_player;

/* "popup": a floating "+10" in the world; destroyed when `life` runs out. */
typedef struct cr_popup {
    float life;
} cr_popup;

/* ---- 2D components (scene files, the level's scene extras, tuning.json) */

/* "level": a level's settings, from the glTF scene extras {"level": {...}} (on the level's view). */
typedef struct cr_level {
    char    title[BR_FIELD_STRING_SIZE];
    float   time_limit;     /* seconds */
    char    next[BR_FIELD_STRING_SIZE];      /* scene name of the next level, "" = last */
    char    footsteps[BR_FIELD_STRING_SIZE]; /* sound path prefix: <prefix>_000.ogg .. _004.ogg */
    float   kill_y;         /* falling below this respawns the player */
    int32_t index;          /* 1-based, for the level select and best scores */
} cr_level;

/* "button": a UI button (keyboard / pad focus + mouse). */
typedef struct cr_button {
    char    action[BR_FIELD_STRING_SIZE];   /* "play", "levels", "settings", "resume", ... */
    char    arg[BR_FIELD_STRING_SIZE];      /* e.g. the level scene name */
    int32_t order;                          /* focus order inside its scene */
    /* runtime */
    bool    focused;
    br_vec2 base_scale;
    bool    ready;
} cr_button;

/* "setting": a settings row: its text child shows the current value of `key`. */
typedef struct cr_setting {
    char key[BR_FIELD_STRING_SIZE];         /* "vsync", "fps", "volume", "profiler", "bind_jump" */
} cr_setting;

/* "prompt": a button prompt. On a sprite: the glyph of `action` for the device used last; on a
 * text: `text` with "{action}" placeholders expanded. */
typedef struct cr_prompt {
    char action[BR_FIELD_STRING_SIZE];
    char text[BR_FIELD_STRING_SIZE];
    /* runtime */
    char shown[BR_FIELD_STRING_SIZE];
} cr_prompt;

/* "counter": a number shown by a text node; tweens move `value`, the text follows. */
typedef struct cr_counter {
    float value;
    char  format[BR_FIELD_STRING_SIZE];     /* "%d", "SCORE %d" */
    int32_t shown;
    bool  ready;
} cr_counter;

/* Every gameplay number, from assets/tuning.json (hot-reloadable in debug builds). */
typedef struct cr_tuning {
    float   run_speed, accel, air_accel, decel;
    float   gravity, jump_speed, max_fall;
    float   coyote_time, jump_buffer;
    float   radius, height;
    float   turn_rate;
    float   run_anim_speed;                    /* run clip playback rate at full speed (feet keep up) */
    float   bloom_threshold, bloom_intensity;  /* the level camera's post settings */
    float   hit_penalty, fall_penalty;         /* seconds taken off the clock */
    float   knockback, invulnerable_time;
    float   pickup_radius;
    float   camera_damping, camera_distance, shake_hit, shake_fall, shake_pickup;
    char    pickup_ease[BR_FIELD_STRING_SIZE];  /* easing names (br_ease_from_name) */
    char    popup_ease[BR_FIELD_STRING_SIZE];
    char    counter_ease[BR_FIELD_STRING_SIZE];
    char    focus_ease[BR_FIELD_STRING_SIZE];
    float   pickup_time, popup_time, popup_rise, counter_time;
    br_vec2 footstep_walk, footstep_sprint;    /* marker times (seconds) in the walk / sprint clips */
    float   end_delay;                         /* seconds between the last crate and the results */
    int32_t time_bonus;                        /* points per second left */
    char    character[BR_FIELD_STRING_SIZE];   /* model path */
    char    animgraph[BR_FIELD_STRING_SIZE];
    char    sdf_font[BR_FIELD_STRING_SIZE];    /* TTF turned into an SDF font for world text */
    char    plate_texture[BR_FIELD_STRING_SIZE];
} cr_tuning;

/* ---- the game state: the only mutable state outside components */

typedef enum cr_state {
    CR_STATE_BOOT,
    CR_STATE_MENU,
    CR_STATE_LOADING,     /* a switch to a level runs */
    CR_STATE_PLAYING,
    CR_STATE_PAUSED,
    CR_STATE_LEVEL_END,   /* all crates or time up: a short beat, then the results */
    CR_STATE_RESULTS
} cr_state;

typedef struct cr_ids {
    uint32_t spawn_point, crate, hazard, sign, player, popup;
    uint32_t level, button, setting, prompt, counter, tuning;
} cr_ids;

/* Player settings, saved with br_save_write("settings.json") and applied live. */
typedef struct cr_settings {
    int32_t vsync;        /* br_vsync */
    int32_t max_fps;      /* 0 = uncapped */
    int32_t volume;       /* percent */
    bool    profiler_hud;
} cr_settings;

#define CR_MAX_LEVELS 3
#define CR_CLIPS 5

typedef struct cr_game {
    cr_ids    id;
    cr_state  state;
    char      state_name[BR_FIELD_STRING_SIZE];  /* for play-tests: "menu", "playing", ... */
    float     state_time;
    bool      filled;               /* the state's scene was filled in (menu bests, results) */

    /* the level being played */
    char      level_scene[BR_FIELD_STRING_SIZE]; /* "level1" */
    char      level_title[BR_FIELD_STRING_SIZE];
    br_scene  level;
    br_node   level_view;           /* the .glb's view3d (hidden; the camera renders the level) */
    br_node3d level_root;           /* its 3D root */
    br_node   camera;               /* the following 3D br_camera */
    br_node3d player;               /* the character instance (model3d) */
    br_node3d camera_target;        /* child of the player the camera looks at */
    br_animator animator;
    char      anim_state[BR_FIELD_STRING_SIZE]; /* the state machine's state, for play-tests */
    br_vec3   spawn;
    float     spawn_yaw;
    float     time_left;
    float     time_limit;
    float     kill_y;
    int32_t   score;
    int32_t   crates_taken, crates_total;
    int32_t   hits, falls;
    bool      cleared;              /* every crate taken (else time ran out) */
    float     level_time;           /* seconds played (moves the saws) */
    int32_t   last_tick;            /* the last whole second a warning tick played for */

    /* per-level memory: colliders, reset when the level unloads */
    br_arena_linear level_arena;
    br_aabb  *colliders;
    uint32_t  collider_count;
    uint32_t  candidates_peak;      /* most colliders one fixed step looked at (frame arena) */

    /* results of the last level (the results scene reads them) */
    int32_t   result_score, result_time_bonus, result_crates, result_total, result_stars;
    bool      result_new_best;
    int32_t   best[CR_MAX_LEVELS + 1];

    /* UI */
    br_node   ui_camera;            /* persistent 2D camera over everything */
    br_scene  overlay;              /* pause / settings scene on top, zero if none */
    char      overlay_name[BR_FIELD_STRING_SIZE];
    int32_t   focus;                /* focused button order in the top interactive scene */
    bool      capturing;            /* rebinding: waiting for a key / button */
    br_input_device capture_device;
    br_input_device last_device;
    int32_t   last_pad_type;

    /* assets */
    br_asset_group *shared;         /* character, UI atlases, fonts, sounds: every scene uses them */
    br_model  *character_model;
    br_anim_clip *clips[CR_CLIPS];  /* the character's clips with footstep markers (owned) */
    uint32_t  clip_count;
    const br_anim_graph *graph;
    br_font   *sdf_font;            /* world text */
    int       sfx_bus, music_bus;
    char      footsteps[BR_FIELD_STRING_SIZE];
    uint32_t  footstep_index;

    cr_settings settings;
    br_node   tuning_node;
    uint32_t  rng;
    uint32_t  levels_completed;     /* for the soak log */
    /* live counts (updated every frame by cr.flow; the soak play-test compares them between menu visits) */
    int32_t   stat_nodes, stat_nodes3d, stat_assets, stat_animators, stat_tweens, stat_sounds;
} cr_game;

/* components.c */
void cr_components_register(br_engine *e, cr_game *g);

/* flow.c: states, scenes, level setup and teardown, results, settings */
void cr_flow_register(br_engine *e, cr_game *g);
void cr_enter_state(br_engine *e, cr_game *g, cr_state state);
void cr_start_level(br_engine *e, cr_game *g, const char *scene_name);
void cr_button_action(br_engine *e, cr_game *g, const cr_button *b);
void cr_setting_change(br_engine *e, cr_game *g, const char *key, int step);
void cr_settings_apply(br_engine *e, cr_game *g);
void cr_settings_save(br_engine *e, cr_game *g);
void cr_log_stats(br_engine *e, cr_game *g, const char *when);

/* player.c: input, fixed-step movement and collision, camera, animation */
void cr_player_register(br_engine *e, cr_game *g);
bool cr_player_spawn(br_engine *e, cr_game *g);
void cr_player_respawn(br_engine *e, cr_game *g);

/* world.c: crates, hazards, signs, popups */
void cr_world_register(br_engine *e, cr_game *g);
void cr_world_setup(br_engine *e, cr_game *g);
void cr_world_hit(br_engine *e, cr_game *g, br_vec3 from);

/* ui.c: buttons, focus, prompts, counters, settings rows, HUD, loading bar */
void cr_ui_register(br_engine *e, cr_game *g);
void cr_ui_text(br_engine *e, br_node root, const char *label, const char *text);
void cr_ui_count_to(br_engine *e, cr_game *g, br_node root, const char *label, float value);

/* shared helpers (flow.c) */
const cr_tuning *cr_tuning_get(br_engine *e, cr_game *g);
const char *cr_config_string(br_engine *e, const char *key);
void cr_play(br_engine *e, cr_game *g, const char *path, float volume);
void cr_sound(br_engine *e, cr_game *g, const char *name);
br_ease_type cr_ease(const char *name);
float cr_random(cr_game *g);

#endif
