/* br_playtest_internal.h - play-test state shared by br_playtest*.c. Engine-private. */
#ifndef BR_PLAYTEST_INTERNAL_H
#define BR_PLAYTEST_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"

#define BR_PLAYTEST_ERROR_SIZE 512
#define BR_PLAYTEST_LOG_LINES 128
#define BR_PLAYTEST_LOG_SIZE 256
#define BR_PLAYTEST_NAME_SIZE 64

/* A struct the game made addressable as "name:field". */
typedef struct br_playtest_exposed {
    char                 name[BR_PLAYTEST_NAME_SIZE];
    const br_field_desc *fields;
    uint32_t             field_count;
    void                *base;
} br_playtest_exposed;

typedef enum br_playtest_release_kind {
    BR_PLAYTEST_RELEASE_KEY,
    BR_PLAYTEST_RELEASE_MOUSE,
    BR_PLAYTEST_RELEASE_GAMEPAD,
    BR_PLAYTEST_PRESS_MOUSE      /* the delayed press of a click (the move came a frame before) */
} br_playtest_release_kind;

/* Input to send later (the second half of a tap / hold, the press and release of a click). */
typedef struct br_playtest_release {
    uint64_t     frame;    /* released at the start of this frame */
    br_playtest_release_kind kind;
    SDL_Scancode key;
    int          button;   /* mouse button or SDL_GamepadButton */
    int          slot;     /* gamepad slot */
    float        x, y;
} br_playtest_release;

/* One recorded input command and the frame that consumed it. */
typedef struct br_playtest_recorded {
    uint64_t frame;
    cJSON   *cmd;
} br_playtest_recorded;

typedef struct br_playtest_state {
    /* script mode */
    cJSON   *script;            /* JSON array of commands, NULL when none */
    char     script_path[512];
    int      step_index;        /* next step to run */
    bool     running;
    bool     failed;

    /* a wait in progress (script: across frames; interactive: stepped in place) */
    bool         waiting;
    uint64_t     resume_frame;  /* "wait"/"step": continue when ticker.frame reaches it */
    const cJSON *until;         /* "wait_until": the command, owned by the script or the caller */
    uint64_t     until_deadline;
    uint32_t     profile_frames; /* frames the last "profile" covered (0 = all kept) */

    bool     in_frame;          /* true while the playtest system runs inside a frame */
    bool     injecting;         /* true while the playtest itself sends input (not recorded) */
    br_array releases;          /* br_playtest_release */
    br_array exposed;           /* br_playtest_exposed */
    char     error[BR_PLAYTEST_ERROR_SIZE];

    /* log capture (installed on first use, restored at shutdown). Any thread logs (asset
     * workers), so the ring is behind log_lock; never log while holding it. */
    SDL_Mutex         *log_lock;
    bool               log_hooked;
    SDL_LogOutputFunction log_prev;
    void              *log_prev_user;
    char               log_lines[BR_PLAYTEST_LOG_LINES][BR_PLAYTEST_LOG_SIZE];
    uint32_t           log_count;   /* lines ever captured; the ring holds the last LOG_LINES */

    /* recording */
    bool     recording;
    char     record_path[512];
    br_array recorded;          /* br_playtest_recorded */
    SDL_JoystickID pad_ids[BR_INPUT_MAX_GAMEPADS]; /* pad in each slot as seen by the recorder */

    /* --playtest-stdin */
    bool     interactive;
} br_playtest_state;

/* ---- br_playtest.c */

/* Formats an error into s->error and logs it; returns false. */
bool br_playtest_fail(br_engine *e, const char *fmt, ...);
/* Starts capturing log lines (for "logs" and log expectations). */
void br_playtest_hook_log(br_engine *e);
/* Seconds of one deterministic play-test frame. */
float br_playtest_frame_dt(br_engine *e);
/* True if some captured log line (of the last BR_PLAYTEST_LOG_LINES) contains `text`; *kept =
 * how many lines are kept. Thread-safe. */
bool br_playtest_log_contains(br_engine *e, const char *text, uint32_t *kept);

/* ---- br_playtest_query.c */

/* Resolves a path to a JSON value (new object), NULL + error if it does not resolve. */
cJSON *br_playtest_query_get(br_engine *e, const char *path);
/* Writes a value at a path. False + error. */
bool br_playtest_query_set(br_engine *e, const char *path, const cJSON *value);
/* The whole dump as a JSON object. */
cJSON *br_playtest_query_dump(br_engine *e);
/* Nodes by type and/or component: [{label, type, position}]. */
cJSON *br_playtest_query_list(br_engine *e, const char *type, const char *component);
/* Evaluates an expect / wait_until condition. *ok = result; false + error if it cannot be
 * evaluated (bad path, bad operator). `detail` receives "game:score is 0" for messages. */
bool br_playtest_query_condition(br_engine *e, const cJSON *cmd, bool *ok, char *detail, size_t detail_size);
/* Node centre in window coordinates for "click". False + error. */
bool br_playtest_query_click_point(br_engine *e, const char *label, float *x, float *y);

/* ---- br_playtest_stdin.c (the only file with C stdio; behind BRATY_PLAYTEST_STDIN) */

/* --playtest-stdin: reads one command line (blocking), runs it, writes the reply.
 * Returns what br_app_iterate should return. */
SDL_AppResult br_playtest_stdin_iterate(br_engine *e);

#endif
