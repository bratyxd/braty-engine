/* test_app.c - the app glue (br_app_init / iterate / quit) run headless, boot options. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

typedef struct boot_probe {
    int      init_calls;
    void    *seen_user;
    int      seen_argc;
    bool     saw_flag;
} boot_probe;

static boot_probe s_probe;

static bool probe_init(br_engine *e)
{
    s_probe.init_calls++;
    s_probe.seen_user = br_engine_app_user(e);
    char **argv = br_engine_args(e, &s_probe.seen_argc);
    for (int i = 0; i < s_probe.seen_argc; i++) {
        s_probe.saw_flag = s_probe.saw_flag || SDL_strcmp(argv[i], "--my-flag") == 0;
    }
    return true;
}

/* API_REVIEW item 1: init gets the user pointer and the command line. */
static void test_user_and_args(void)
{
    int marker = 42;
    char *argv[] = { "test_app", "--headless", "--my-flag", "--frames", "3" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.init = probe_init;
    desc.user = &marker;
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, (int)BR_ARRAY_COUNT(argv), argv) == SDL_APP_CONTINUE);
    BR_CHECK(s_probe.init_calls == 1);
    BR_CHECK(s_probe.seen_user == &marker);
    BR_CHECK(s_probe.seen_argc == 5);
    BR_CHECK(s_probe.saw_flag);
    br_engine *e = state;
    BR_CHECK(br_engine_window(e) == NULL); /* --headless */
    SDL_AppResult r = SDL_APP_CONTINUE;
    int iterations = 0;
    while (r == SDL_APP_CONTINUE && iterations < 10) {
        r = br_app_iterate(state);
        iterations++;
    }
    BR_CHECK(r == SDL_APP_SUCCESS && iterations == 3); /* --frames 3, headless */
    br_app_quit(state, r);

    /* engines made without the app have neither */
    br_engine *plain = br_engine_create("test_app", NULL);
    int argc = -1;
    BR_CHECK(br_engine_args(plain, &argc) == NULL && argc == 0);
    BR_CHECK(br_engine_app_user(plain) == NULL);
    br_engine_destroy(plain);
}

/* ---- API_REVIEW item 2: br_game_config_main keeps the config; apply before init, scene after */

typedef struct probe_component {
    int32_t value;
} probe_component;

typedef struct config_probe {
    bool     json_seen_in_init;
    uint32_t component_id;
} config_probe;

static config_probe s_config;

static bool config_init(br_engine *e)
{
    s_config.json_seen_in_init = br_game_config_json(e) != NULL; /* applied before init */
    static const br_field_desc fields[] = { { "value", BR_FIELD_INT, offsetof(probe_component, value) } };
    br_component_desc desc = { "probe", sizeof(probe_component), fields, 1 };
    s_config.component_id = br_component_register(e, &desc);
    return true;
}

static bool write_next_to_exe(const char *name, const char *text)
{
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%s", SDL_GetBasePath(), name);
    return SDL_SaveFile(path, text, SDL_strlen(text));
}

static void test_game_config_main(void)
{
    BR_CHECK(write_next_to_exe("test_app_game.json",
                               "{ \"window\": { \"title\": \"boot test\" }, \"start_scene\": \"test_app_scene.json\","
                               "  \"save\": { \"org\": \"braty-tests\", \"app\": \"config app\" },"
                               "  \"memory\": { \"frame_kb\": 2048, \"level_kb\": 512, \"game_kb\": 256 },"
                               "  \"game\": { \"level\": 3 } }"));
    /* the scene uses a component that only exists once init ran */
    BR_CHECK(write_next_to_exe("test_app_scene.json",
                               "{ \"type\": \"container\", \"children\": [ { \"type\": \"container\","
                               "  \"label\": \"booted\", \"components\": { \"probe\": { \"value\": 7 } } } ] }"));
    char *argv[] = { "test_app", "--headless" };
    br_app_desc desc = br_game_config_main("test_app_game.json", 2, argv, config_init);
    BR_CHECK(desc.config != NULL && SDL_strcmp(desc.title, "boot test") == 0);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    br_engine *e = state;
    BR_CHECK(s_config.json_seen_in_init);
    /* game.json "memory" preallocates the three arenas */
    BR_CHECK(br_frame_arena(e)->stats.capacity == 2048 * 1024);
    BR_CHECK(br_level_arena(e)->stats.capacity == 512 * 1024 && br_game_arena(e)->stats.capacity == 256 * 1024);
    br_node booted = br_node_find_by_label(e, br_stage_root(e), "booted", true);
    probe_component *p = br_component_get(e, booted, s_config.component_id);
    BR_CHECK(p != NULL && p->value == 7);
    const cJSON *level = cJSON_GetObjectItem(cJSON_GetObjectItem(br_game_config_json(e), "game"), "level");
    BR_CHECK(cJSON_IsNumber(level) && level->valueint == 3);
    /* game.json "save" names the save folder, not the title */
    BR_CHECK(br_save_write(e, "config.json", "1", 1));
    br_engine *other = br_engine_create("another title", NULL);
    br_engine_set_save_name(other, "braty-tests", "config app");
    void *bytes = NULL;
    size_t size = 0;
    BR_CHECK(br_save_read(other, "config.json", &bytes, &size) && size == 1);
    br_free(bytes);
    BR_CHECK(br_save_remove(other, "config.json"));
    br_engine_destroy(other);
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* game.json window.pause_when_inactive and "save" reach the br_app_desc */
static void test_game_config_lifecycle_keys(void)
{
    const char *json = "{ \"window\": { \"pause_when_inactive\": false }, \"save\": { \"org\": \"o\", \"app\": \"a\" } }";
    br_game_config config;
    BR_CHECK(br_game_config_parse(json, SDL_strlen(json), "test", &config));
    br_app_desc desc = br_game_config_app_desc(&config);
    BR_CHECK(desc.run_when_inactive && SDL_strcmp(desc.save_org, "o") == 0 && SDL_strcmp(desc.save_app, "a") == 0);
    cJSON_Delete(config.json);
    br_game_config_defaults(&config);
    desc = br_game_config_app_desc(&config);
    BR_CHECK(!desc.run_when_inactive && desc.save_org == NULL && desc.save_app == NULL);
}

/* API_REVIEW item 4: --assets / BRATY_ASSETS point the asset root anywhere (cwd-relative). */
static void test_assets_override(void)
{
    char *cwd = SDL_GetCurrentDirectory();
    char expected[1024];
    SDL_snprintf(expected, sizeof(expected), "%sdata", cwd);
    char *argv[] = { "test_app", "--headless", "--assets", "data" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.asset_root = "somewhere/else";
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 4, argv) == SDL_APP_CONTINUE);
    BR_CHECK(SDL_strcmp(br_engine_asset_root(state), expected) == 0);
    br_app_quit(state, SDL_APP_SUCCESS);

    SDL_setenv_unsafe("BRATY_ASSETS", "data", 1);
    state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    BR_CHECK(SDL_strcmp(br_engine_asset_root(state), expected) == 0);
    br_app_quit(state, SDL_APP_SUCCESS);
    SDL_unsetenv_unsafe("BRATY_ASSETS");
    SDL_free(cwd);
}

/* API_REVIEW item 13: switch a group of systems off and on. */
static void count_system(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    *(int *)user += 1;
}

static void test_system_groups(void)
{
    br_engine *e = br_engine_create("test_app", NULL);
    int move = 0, spawn = 0, hud = 0, late = 0;
    br_system_add(e, "move", BR_PHASE_UPDATE, 0, count_system, &move);
    br_system_add(e, "spawn", BR_PHASE_UPDATE, 0, count_system, &spawn);
    br_system_add(e, "hud", BR_PHASE_UPDATE, 0, count_system, &hud);
    br_system_set_group(e, "move", "gameplay");
    br_system_set_group(e, "spawn", "gameplay");
    br_engine_step(e, 0.016f);
    BR_CHECK(move == 1 && spawn == 1 && hud == 1);

    br_system_group_set_enabled(e, "gameplay", false);
    br_system_add(e, "late", BR_PHASE_UPDATE, 0, count_system, &late);
    br_system_set_group(e, "late", "gameplay"); /* joins a group that is already off */
    br_engine_step(e, 0.016f);
    BR_CHECK(move == 1 && spawn == 1 && hud == 2 && late == 0);

    br_system_set_enabled(e, "spawn", false); /* own flag and group flag are independent */
    br_system_group_set_enabled(e, "gameplay", true);
    br_engine_step(e, 0.016f);
    BR_CHECK(move == 2 && spawn == 1 && hud == 3 && late == 1);

    br_system_set_group(e, "spawn", NULL);
    br_system_set_enabled(e, "spawn", true);
    br_system_group_set_enabled(e, "gameplay", false);
    br_engine_step(e, 0.016f);
    BR_CHECK(move == 2 && spawn == 2 && late == 1);
    br_engine_destroy(e);
}

/* br_system_remove: immediate outside a frame, deferred (but at once silent) inside one; the name
 * can be added again with another phase and priority. */
typedef struct remover {
    br_engine *e;
    int        runs;
} remover;

static void remove_victim_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    remover *r = user;
    r->runs++;
    br_system_remove(e, "victim");      /* runs after this one in the same phase: skipped now */
    br_system_remove(e, "self_remover"); /* removing itself mid-run is fine */
}

static void test_system_remove(void)
{
    br_engine *e = br_engine_create("test_app_remove", NULL);
    int victim = 0;
    remover r = { e, 0 };
    br_system_add(e, "self_remover", BR_PHASE_UPDATE, 0, remove_victim_system, &r);
    br_system_add(e, "victim", BR_PHASE_UPDATE, 10, count_system, &victim);
    uint32_t before = br_system_count(e);
    br_engine_step(e, 0.016f);
    BR_CHECK(r.runs == 1 && victim == 0);
    BR_CHECK(br_system_count(e) == before - 2);
    br_engine_step(e, 0.016f);
    BR_CHECK(r.runs == 1 && victim == 0);
    /* re-added with a new phase and priority */
    br_system_add(e, "victim", BR_PHASE_LATE_UPDATE, 7, count_system, &victim);
    br_engine_step(e, 0.016f);
    BR_CHECK(victim == 1);
    bool found = false;
    for (uint32_t i = 0; i < br_system_count(e); i++) {
        br_system_info info;
        if (br_system_get_info(e, i, &info) && SDL_strcmp(info.name, "victim") == 0) {
            found = info.phase == BR_PHASE_LATE_UPDATE && info.priority == 7;
        }
    }
    BR_CHECK(found);
    br_system_remove(e, "victim"); /* outside a frame: gone now */
    BR_CHECK(br_system_count(e) == before - 2);
    br_engine_destroy(e);
}

/* --deterministic: constant dt whatever the wall clock, seeded RNG, synchronous asset loads. */
static void on_loaded(br_engine *e, const char *path, void *asset, void *user)
{
    (void)e;
    (void)path;
    *(bool *)user = asset != NULL;
}

static void test_deterministic(void)
{
    char *argv[] = { "test_app", "--headless", "--deterministic", "--seed", "7" };
    br_app_desc desc;
    SDL_zero(desc);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 5, argv) == SDL_APP_CONTINUE);
    br_engine *e = state;
    BR_CHECK(br_app_deterministic(e));
    uint32_t first = br_random_u32(br_random_engine(e));
    for (int i = 0; i < 3; i++) {
        SDL_Delay(30); /* the wall clock says 30+ ms; the step stays 1/60 s */
        br_app_iterate(state);
        BR_CHECK(br_ticker_delta(e) == 1.0f / 60.0f);
    }
    br_random_seed(br_random_engine(e), 7, 0);
    BR_CHECK(br_random_u32(br_random_engine(e)) == first);
    float f = br_random_float(br_random_engine(e));
    BR_CHECK(f >= 0.0f && f < 1.0f);

    /* async loads finish on the main thread at once, callbacks still come from the next step */
    BR_CHECK(write_next_to_exe("test_app_async.json", "{ \"x\": 1 }"));
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%stest_app_async.json", SDL_GetBasePath());
    bool loaded = false;
    br_assets_load_async(e, path, NULL, on_loaded, &loaded);
    BR_CHECK(br_assets_pending(e) == 0 && !loaded);
    br_app_iterate(state);
    BR_CHECK(loaded);
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* R1: nodes are torn down before any game shutdown callback (and before desc.shutdown), so a
 * node-destroy listener never sees game state that was already freed. */
typedef struct teardown_probe {
    bool dead;             /* "freed" by the game's shutdown */
    int  calls_2d;
    int  calls_3d;
    int  calls_after_death;
} teardown_probe;

static teardown_probe s_teardown;

static void teardown_listener_2d(br_engine *e, br_node n, void *user)
{
    (void)e;
    (void)n;
    teardown_probe *p = user;
    p->calls_2d++;
    p->calls_after_death += p->dead ? 1 : 0;
}

static void teardown_listener_3d(br_engine *e, br_node3d n, void *user)
{
    (void)e;
    (void)n;
    teardown_probe *p = user;
    p->calls_3d++;
    p->calls_after_death += p->dead ? 1 : 0;
}

static void teardown_kill(br_engine *e, void *user)
{
    (void)e;
    ((teardown_probe *)user)->dead = true;
}

static bool teardown_init(br_engine *e)
{
    br_node_on_destroy(e, teardown_listener_2d, &s_teardown);
    br_node3d_on_destroy(e, teardown_listener_3d, &s_teardown);
    br_node_add_child(e, br_stage_root(e), br_node_create_container(e));
    br_node3d_create_container(e);
    return true;
}

static void teardown_desc_shutdown(br_engine *e)
{
    teardown_kill(e, &s_teardown);
}

static void test_teardown_order(void)
{
    /* the documented pattern: state freed in a br_engine_on_shutdown callback */
    SDL_zero(s_teardown);
    br_engine *e = br_engine_create("test_app_teardown", NULL);
    teardown_init(e);
    br_engine_on_shutdown(e, teardown_kill, &s_teardown);
    br_engine_destroy(e);
    BR_CHECK(s_teardown.calls_2d >= 2 && s_teardown.calls_3d >= 1);
    BR_CHECK(s_teardown.calls_after_death == 0);

    /* desc.shutdown runs after the nodes are gone too */
    SDL_zero(s_teardown);
    char *argv[] = { "test_app", "--headless" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.init = teardown_init;
    desc.shutdown = teardown_desc_shutdown;
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    br_app_iterate(state);
    br_app_quit(state, SDL_APP_SUCCESS);
    BR_CHECK(s_teardown.calls_2d >= 2 && s_teardown.calls_3d >= 1);
    BR_CHECK(s_teardown.calls_after_death == 0);
}

/* ---- R8: app lifecycle (focus, minimise, background) through injected SDL events */

typedef struct lifecycle_log {
    int activated;
    int deactivated;
    int low_memory;
    int terminating;
} lifecycle_log;

static void on_lifecycle(br_engine *e, br_app_lifecycle event, void *user)
{
    (void)e;
    lifecycle_log *log = user;
    log->activated += event == BR_APP_LIFECYCLE_ACTIVATED ? 1 : 0;
    log->deactivated += event == BR_APP_LIFECYCLE_DEACTIVATED ? 1 : 0;
    log->low_memory += event == BR_APP_LIFECYCLE_LOW_MEMORY ? 1 : 0;
    log->terminating += event == BR_APP_LIFECYCLE_TERMINATING ? 1 : 0;
}

static void send_event(void *state, Uint32 type)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = type;
    br_app_event(state, &ev);
}

static void test_lifecycle_state(void)
{
    char *argv[] = { "test_app", "--headless" };
    br_app_desc desc;
    SDL_zero(desc);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    br_engine *e = state;
    lifecycle_log log;
    SDL_zero(log);
    br_app_on_lifecycle(e, on_lifecycle, &log);
    BR_CHECK(br_app_is_active(e));

    send_event(state, SDL_EVENT_WINDOW_FOCUS_LOST);
    BR_CHECK(!br_app_is_active(e) && log.deactivated == 1);
    send_event(state, SDL_EVENT_WINDOW_MINIMIZED); /* still inactive: no second callback */
    BR_CHECK(log.deactivated == 1);
    send_event(state, SDL_EVENT_WINDOW_FOCUS_GAINED);
    BR_CHECK(!br_app_is_active(e) && log.activated == 0); /* still minimised */
    send_event(state, SDL_EVENT_WINDOW_RESTORED);
    BR_CHECK(br_app_is_active(e) && log.activated == 1);

    send_event(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
    BR_CHECK(!br_app_is_active(e) && log.deactivated == 2);
    send_event(state, SDL_EVENT_DID_ENTER_BACKGROUND);
    send_event(state, SDL_EVENT_WILL_ENTER_FOREGROUND);
    BR_CHECK(!br_app_is_active(e));
    send_event(state, SDL_EVENT_DID_ENTER_FOREGROUND);
    BR_CHECK(br_app_is_active(e) && log.activated == 2);

    send_event(state, SDL_EVENT_LOW_MEMORY);
    send_event(state, SDL_EVENT_TERMINATING);
    BR_CHECK(log.low_memory == 1 && log.terminating == 1 && br_app_is_active(e));

    /* headless: inactive never pauses time */
    send_event(state, SDL_EVENT_WINDOW_FOCUS_LOST);
    BR_CHECK(!br_app_is_paused(e));
    br_app_iterate(state);
    BR_CHECK(br_ticker_delta(e) > 0.0f);
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* Unscaled delta: pause menus animate with it while the ticker speed is 0. */
static void test_unscaled_delta(void)
{
    char *argv[] = { "test_app", "--headless", "--deterministic" };
    br_app_desc desc;
    SDL_zero(desc);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 3, argv) == SDL_APP_CONTINUE);
    br_engine *e = state;
    br_ticker_set_speed(e, 0.0f);
    br_app_iterate(state);
    BR_CHECK(br_ticker_delta(e) == 0.0f);
    BR_CHECK(br_ticker_unscaled_delta(e) == 1.0f / 60.0f);
    br_ticker_set_speed(e, 0.5f);
    br_app_iterate(state);
    BR_CHECK(br_ticker_delta(e) == 0.5f / 60.0f && br_ticker_unscaled_delta(e) == 1.0f / 60.0f);
    br_engine_step(e, 0.02f); /* a plain step: both are the given dt */
    BR_CHECK(br_ticker_unscaled_delta(e) == 0.02f);
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* --deterministic drops real device input (br_app_device_event) but not window events. */
static void count_events(br_engine *e, const SDL_Event *event, void *user)
{
    (void)e;
    (void)event;
    *(int *)user += 1;
}

static void test_deterministic_input_filter(void)
{
    char *argv[] = { "test_app", "--headless", "--deterministic" };
    br_app_desc desc;
    SDL_zero(desc);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 3, argv) == SDL_APP_CONTINUE);
    int seen = 0;
    br_engine_add_event_handler(state, count_events, &seen);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    br_app_device_event(state, &ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    br_app_device_event(state, &ev);
    BR_CHECK(seen == 0);
    ev.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    br_app_device_event(state, &ev);
    BR_CHECK(seen == 1 && !br_app_is_active(state) && !br_app_is_paused(state)); /* deterministic: no pause */
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* With a window, an inactive app stops time and audio (skipped when no window/GPU). */
static void test_pause_when_inactive(void)
{
    char *argv[] = { "test_app" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.width = 64;
    desc.height = 64;
    void *state = NULL;
    if (br_app_init(&state, &desc, 1, argv) != SDL_APP_CONTINUE) {
        printf("test_pause_when_inactive: skipped, no window or GPU\n");
        if (state != NULL) {
            br_app_quit(state, SDL_APP_FAILURE);
        }
        return;
    }
    br_engine *e = state;
    br_app_iterate(state);
    double elapsed = br_ticker_elapsed(e);
    send_event(state, SDL_EVENT_WINDOW_FOCUS_LOST);
    BR_CHECK(br_app_is_paused(e) && br_audio_device_paused(e));
    br_app_iterate(state);
    BR_CHECK(br_ticker_delta(e) == 0.0f && br_ticker_unscaled_delta(e) == 0.0f);
    BR_CHECK(br_ticker_elapsed(e) == elapsed);
    send_event(state, SDL_EVENT_WINDOW_FOCUS_GAINED);
    BR_CHECK(!br_app_is_paused(e) && !br_audio_device_paused(e));
    br_app_iterate(state);
    BR_CHECK(br_ticker_delta(e) > 0.0f && br_ticker_delta(e) <= 0.1f);

    br_app_set_pause_when_inactive(e, false); /* opted out: keeps running */
    send_event(state, SDL_EVENT_WINDOW_MINIMIZED);
    BR_CHECK(!br_app_is_active(e) && !br_app_is_paused(e));
    br_app_set_pause_when_inactive(e, true); /* applies at once while inactive */
    BR_CHECK(br_app_is_paused(e));
    br_app_quit(state, SDL_APP_SUCCESS);

    /* --frames runs are scripted: never paused */
    char *frames_argv[] = { "test_app", "--frames", "100" };
    state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 3, frames_argv) == SDL_APP_CONTINUE);
    send_event(state, SDL_EVENT_WINDOW_FOCUS_LOST);
    BR_CHECK(!br_app_is_paused(state));
    br_app_quit(state, SDL_APP_SUCCESS);

    /* br_app_desc.run_when_inactive opts out from the start */
    desc.run_when_inactive = true;
    state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 1, argv) == SDL_APP_CONTINUE);
    send_event(state, SDL_EVENT_WINDOW_FOCUS_LOST);
    BR_CHECK(!br_app_is_paused(state));
    br_app_quit(state, SDL_APP_SUCCESS);
}

int main(void)
{
    test_lifecycle_state();
    test_game_config_lifecycle_keys();
    test_unscaled_delta();
    test_deterministic_input_filter();
    test_pause_when_inactive();
    test_teardown_order();
    test_deterministic();
    test_system_groups();
    test_system_remove();
    test_assets_override();
    test_user_and_args();
    test_game_config_main();
    return br_test_finish();
}
