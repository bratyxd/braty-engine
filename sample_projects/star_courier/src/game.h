/* game.h - Star Courier: component structs (filled from data files through their field lists),
 * the one game-state struct, and the systems that iterate them.
 *
 * Data lives in data/: game.json (window, input actions, audio), assets/tuning.json (every
 * gameplay number), assets/scenes/ (title, play, game over), assets/prefabs/ (ship, crate,
 * debris, station, starfield, panel). C only says how the components behave.
 */
#ifndef STAR_COURIER_GAME_H
#define STAR_COURIER_GAME_H

#include "braty/braty.h"

/* ---- components (one struct each; the fields named in components.c are settable from JSON) */

/* Anything that moves: integrated by the movement system. */
typedef struct sc_body {
    br_vec2 velocity;   /* units per second */
    float   spin;       /* radians per second */
    float   drag;       /* velocity *= exp(-drag * dt) */
    bool    wrap;       /* wraps around the screen edges (title decoration) */
} sc_body;

/* The player's ship. */
typedef struct sc_pilot {
    float   thrust;       /* acceleration at full stick */
    float   max_speed;
    float   radius;       /* collision circle */
    float   turn_rate;    /* how fast the nose follows the velocity */
    br_node cargo_icon;   /* shown while carrying */
    /* runtime */
    br_vec2 intent;       /* -1..1 per axis, written by the input system */
    bool    carrying;
    float   invulnerable; /* seconds left */
} sc_pilot;

typedef struct sc_crate {
    float   radius;
    int32_t value;
} sc_crate;

typedef struct sc_debris {
    float radius;
} sc_debris;

typedef struct sc_station {
    float   radius;
    br_node halo;         /* glows while the ship carries cargo */
    /* runtime: tweens between rounds */
    bool    moving;
    br_vec2 from, to;
    float   move_time;    /* seconds since the move started */
} sc_station;

#define SC_EMITTER_MAX 256

/* A particle emitter on a particle_container node: continuous (rate > 0) and/or bursts. */
typedef struct sc_emitter {
    float    rate;          /* particles per second (0 = bursts only) */
    float    life;          /* seconds */
    br_vec2  speed;         /* min, max */
    float    spread;        /* radians around direction */
    float    direction;     /* radians, in the followed node's space */
    br_vec2  scale;         /* start, end */
    br_color color_start;
    br_color color_end;
    float    drag;
    br_node  follow;        /* emit at this node (offset in its local space) */
    br_vec2  offset;
    bool     needs_thrust;  /* only while the followed pilot steers */
    /* runtime: particle i of the container is slot i here */
    bool     ready;
    float    accumulator;
    float    age[SC_EMITTER_MAX];
    br_vec2  velocity[SC_EMITTER_MAX];
    br_color color[SC_EMITTER_MAX];
} sc_emitter;

/* Hand-tweened UI motion: a steady wave plus a decaying kick (score changes, level ups). */
typedef struct sc_pulse {
    float   wave;    /* scale amplitude of the steady wave */
    float   speed;   /* wave radians per second */
    float   kick;    /* scale amplitude of a kick */
    float   decay;   /* kick fades at this rate per second */
    /* runtime */
    bool    ready;
    br_vec2 base;
    float   amount;
    float   time;
} sc_pulse;

/* Fades a node's alpha up and down ("PRESS START"). */
typedef struct sc_blink {
    float period;
    float min_alpha;
    float time;
} sc_blink;

/* Scrolls a tiling sprite's pattern (the starfield). */
typedef struct sc_scroll {
    br_vec2 velocity;
} sc_scroll;

/* Every gameplay number, from assets/tuning.json (hot-reloadable in debug builds). */
typedef struct sc_tuning {
    int32_t  lives;
    float    invulnerable_time;
    float    gameover_delay;
    br_vec2  field_min, field_max;
    int32_t  crates_on_field;
    int32_t  crate_value;
    float    level_bonus;
    int32_t  deliveries_per_level;
    float    station_move_time;
    float    debris_interval, debris_interval_min, debris_interval_scale;
    br_vec2  debris_speed;
    float    debris_speed_scale;
    int32_t  debris_max;
    char     debris_small[BR_FIELD_STRING_SIZE];
    char     debris_medium[BR_FIELD_STRING_SIZE];
    char     debris_large[BR_FIELD_STRING_SIZE];
    char     crate_prefab[BR_FIELD_STRING_SIZE];
    float    flash_time;
    float    flash_blur;
    br_color flash_color;
    int32_t  pickup_burst, deliver_burst, hit_burst;
} sc_tuning;

/* ---- the game state: the only mutable state outside components */

typedef enum sc_state {
    SC_STATE_TITLE,
    SC_STATE_PLAY,
    SC_STATE_DYING,
    SC_STATE_GAMEOVER
} sc_state;

typedef enum sc_event_kind {
    SC_EVENT_PICKUP,
    SC_EVENT_DELIVER,
    SC_EVENT_HIT
} sc_event_kind;

/* What collision found this frame; the rules system applies it. */
typedef struct sc_event {
    sc_event_kind kind;
    br_node       other;
    br_vec2       position;
} sc_event;

#define SC_MAX_EVENTS 32
#define SC_MAX_LIFE_ICONS 4

typedef struct sc_ids {
    uint32_t body, pilot, crate, debris, station, emitter, pulse, blink, scroll, tuning;
} sc_ids;

typedef struct sc_game {
    sc_ids    id;
    sc_state  state;
    bool      paused;
    float     state_time;    /* seconds in this state, pauses excluded */
    float     wall_time;     /* seconds in this state, pauses included */
    uint32_t  round;
    int32_t   score, best, lives, level, deliveries;
    float     spawn_timer;
    float     flash;
    uint32_t  rng;
    sc_event  events[SC_MAX_EVENTS];
    uint32_t  event_count;

    /* nodes of the current scene (looked up by label after each load) */
    br_node   tuning_node;
    br_node   scene;
    br_node   field, ship, station, crates, debris, burst_fx;
    br_node   score_text, level_text, hint, pause_panel, press_start, best_text, final_score;
    br_node   life_icons[SC_MAX_LIFE_ICONS];

    /* audio */
    bool              assets_ready;
    br_sound         *sfx;
    br_sound         *music;
    br_sound_instance music_instance;
    int               sfx_bus, music_bus;

    /* --autoplay: scripted input for screenshots and soak tests */
    bool      autoplay;
    uint32_t  bot_keys;      /* steering keys currently held (bit per key) */
    SDL_Scancode bot_tap;    /* key pressed last frame, released this frame */
} sc_game;

/* components.c */
void sc_components_register(br_engine *e, sc_game *g);

/* systems.c */
void sc_systems_register(br_engine *e, sc_game *g);
const sc_tuning *sc_tuning_get(br_engine *e, sc_game *g);
const char *sc_config_string(br_engine *e, const char *section, const char *key);
void sc_enter_state(br_engine *e, sc_game *g, sc_state state);
float sc_random(sc_game *g);
/* Logs node / texture / asset / sound counts (the soak test compares them between rounds). */
void sc_log_stats(br_engine *e, const char *when);

#endif
