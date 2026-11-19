/* test_playtest.c - play-test scripts (parse errors, expect failure exit code), record -> replay
 * gives identical dumps, the interactive protocol through br_playtest_command. All headless. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

/* ---- a tiny game: a "walker" node moves right while Right is held, counts Space taps */

typedef struct walker {
    float   speed;
    int32_t jumps;
} walker;

typedef struct game_state {
    int32_t score;
    char    mode[BR_FIELD_STRING_SIZE];
} game_state;

static game_state s_game;

static void walker_system(br_engine *e, float dt, void *user)
{
    uint32_t id = *(uint32_t *)user;
    for (uint32_t i = 0; i < br_component_count(e, id); i++) {
        walker *w = br_component_at(e, id, i);
        br_node n = br_component_owner_at(e, id, i);
        br_vec2 p = br_node_position(e, n);
        if (br_input_key_down(e, SDL_SCANCODE_RIGHT)) {
            br_node_set_position(e, n, p.x + w->speed * dt, p.y);
        }
        /* the left stick of pad 0 moves it too; its south button jumps */
        p = br_node_position(e, n);
        br_node_set_position(e, n, p.x + w->speed * br_input_gamepad_axis(e, 0, SDL_GAMEPAD_AXIS_LEFTX) * dt, p.y);
        if (br_input_key_pressed(e, SDL_SCANCODE_SPACE) || br_input_gamepad_pressed(e, 0, SDL_GAMEPAD_BUTTON_SOUTH)) {
            w->jumps++;
            s_game.score += 10;
        }
    }
}

static uint32_t s_walker_id;

/* Registers the walker, a "hero" node and the exposed game struct on an engine. */
static bool setup_game(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "speed", BR_FIELD_FLOAT, offsetof(walker, speed) },
        { "jumps", BR_FIELD_INT, offsetof(walker, jumps) },
    };
    br_component_desc desc = { "walker", sizeof(walker), fields, BR_ARRAY_COUNT(fields) };
    s_walker_id = br_component_register(e, &desc);
    br_system_add(e, "walker", BR_PHASE_UPDATE, 0, walker_system, &s_walker_id);
    br_node n = br_node_create_container(e);
    br_node_set_label(e, n, "hero");
    br_node_set_position(e, n, 100.0f, 50.0f);
    br_node_add_child(e, br_scene_root(e), n);
    walker *w = br_component_add(e, n, s_walker_id);
    w->speed = 60.0f;
    SDL_zero(s_game);
    br_str_copy(s_game.mode, sizeof(s_game.mode), "menu");
    static const br_field_desc game_fields[] = {
        { "score", BR_FIELD_INT, offsetof(game_state, score) },
        { "mode", BR_FIELD_STRING, offsetof(game_state, mode) },
    };
    br_playtest_expose(e, "game", game_fields, BR_ARRAY_COUNT(game_fields), &s_game);
    return true;
}

static br_engine *make_game(void)
{
    br_engine *e = br_engine_create("test_playtest", NULL);
    setup_game(e);
    return e;
}

/* Test files go next to the executable (build dir), not into the source tree. Returns a buffer
 * that the next call overwrites: use the result right away. */
static const char *at_exe(const char *name)
{
    static char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%s", SDL_GetBasePath(), name);
    return path;
}

static void write_file(const char *path, const char *text)
{
    BR_CHECK(SDL_SaveFile(path, text, SDL_strlen(text)));
}

/* ---- script parse errors name the step (or the line for JSON syntax) */

static void test_parse_errors(void)
{
    br_engine *e = make_game();
    write_file(at_exe("pt_bad_cmd.json"), "[ { \"cmd\": \"wait\", \"frames\": 2 },\n  { \"cmd\": \"jump\" } ]");
    BR_CHECK(!br_playtest_load_script(e, at_exe("pt_bad_cmd.json")));
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "step 2") != NULL);
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "unknown command 'jump'") != NULL);

    write_file(at_exe("pt_bad_key.json"), "[ { \"cmd\": \"key\", \"key\": \"Spaec\" } ]");
    BR_CHECK(!br_playtest_load_script(e, at_exe("pt_bad_key.json")));
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "step 1: key: unknown key name") != NULL);

    write_file(at_exe("pt_missing.json"), "[ {\"cmd\":\"log\",\"message\":\"x\"}, {\"cmd\":\"log\",\"message\":\"y\"},"
                                  " { \"cmd\": \"set\", \"path\": \"hero:x\" } ]");
    BR_CHECK(!br_playtest_load_script(e, at_exe("pt_missing.json")));
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "step 3: set: missing \"value\"") != NULL);

    write_file(at_exe("pt_syntax.json"), "[\n  { \"cmd\": \"wait\" },\n  { \"cmd\": \"wait\" \n]");
    BR_CHECK(!br_playtest_load_script(e, at_exe("pt_syntax.json")));
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "line 4") != NULL);
    BR_CHECK(!br_playtest_running(e));
    br_engine_destroy(e);
}

/* ---- scripts through the real app loop: exit code 0 when every expect holds, failure otherwise */

static SDL_AppResult run_app_script(const char *path)
{
    char *argv[] = { "test_playtest", "--headless", "--playtest", (char *)path };
    br_app_desc desc;
    SDL_zero(desc);
    desc.init = setup_game;
    void *state = NULL;
    SDL_AppResult r = br_app_init(&state, &desc, 4, argv);
    int guard = 0;
    while (r == SDL_APP_CONTINUE && guard++ < 1000) {
        r = br_app_iterate(state);
    }
    br_app_quit(state, r);
    return r;
}

static void test_expect_exit_code(void)
{
    const char *body = "[ { \"cmd\": \"key\", \"key\": \"Right\", \"frames\": 30 },\n"
                       "  { \"cmd\": \"key\", \"key\": \"Space\" },\n"
                       "  { \"cmd\": \"wait\", \"frames\": 40 },\n"
                       "  { \"cmd\": \"expect\", \"path\": \"hero:x\", \"value\": 130 },\n"
                       "  { \"cmd\": \"expect\", \"path\": \"hero:walker.jumps\", \"value\": 1 },\n"
                       "  { \"cmd\": \"expect\", \"path\": \"game:score\", \"op\": \">=\", \"value\": %d } ]";
    char text[1024];
    SDL_snprintf(text, sizeof(text), body, 10);
    write_file(at_exe("pt_pass.json"), text);
    BR_CHECK(run_app_script(at_exe("pt_pass.json")) == SDL_APP_SUCCESS);
    SDL_snprintf(text, sizeof(text), body, 11);
    write_file(at_exe("pt_fail.json"), text);
    BR_CHECK(run_app_script(at_exe("pt_fail.json")) == SDL_APP_FAILURE);
    write_file(at_exe("pt_timeout.json"), "[ { \"cmd\": \"wait_until\", \"path\": \"game:score\", \"value\": 99,"
                                  " \"timeout_frames\": 5 } ]");
    BR_CHECK(run_app_script(at_exe("pt_timeout.json")) == SDL_APP_FAILURE);
    BR_CHECK(run_app_script(at_exe("pt_bad_cmd.json")) == SDL_APP_FAILURE); /* never starts */
}

/* ---- record -> replay gives the same dump */

static void test_record_replay(void)
{
    br_engine *a = make_game();
    BR_CHECK(br_playtest_record_start(a, at_exe("pt_recorded.json")));
    for (int frame = 1; frame <= 90; frame++) {
        /* "real" input: arrives between frames, like SDL events */
        if (frame == 10 || frame == 50) {
            br_input_inject_key(a, SDL_SCANCODE_RIGHT, true);
        }
        if (frame == 30 || frame == 70) {
            br_input_inject_key(a, SDL_SCANCODE_RIGHT, false);
        }
        if (frame == 20 || frame == 21 || frame == 60) {
            br_input_inject_key(a, SDL_SCANCODE_SPACE, frame != 21);
        }
        br_playtest_step(a, 1);
    }
    char *dump_a = br_playtest_dump_json(a);
    BR_CHECK(br_playtest_record_stop(a));
    br_engine_destroy(a);

    br_engine *b = make_game();
    BR_CHECK(br_playtest_run_script(b, at_exe("pt_recorded.json")));
    char *dump_b = br_playtest_dump_json(b);
    BR_CHECK(dump_a != NULL && dump_b != NULL && SDL_strcmp(dump_a, dump_b) == 0);
    BR_CHECK(SDL_strstr(dump_b, "\"frame\":\t90") != NULL);
    cJSON *x = br_playtest_get(b, "hero:x");
    BR_CHECK(x != NULL && x->valuedouble > 100.0); /* it really moved */
    cJSON_Delete(x);
    if (dump_a != NULL && dump_b != NULL && SDL_strcmp(dump_a, dump_b) != 0) {
        printf("record:\n%s\nreplay:\n%s\n", dump_a, dump_b);
    }
    br_free(dump_a);
    br_free(dump_b);
    br_engine_destroy(b);
}

/* ---- a recorded gamepad session (a "real" PS5 pad: connect, stick, button, a second pad that
 * comes and goes) replays to the same dump */

static void test_record_replay_gamepad(void)
{
    br_engine *a = make_game();
    BR_CHECK(br_playtest_record_start(a, at_exe("pt_recorded_pad.json")));
    br_playtest_step(a, 3);
    int pad = br_input_gamepad_attach_virtual(a, BR_GAMEPAD_TYPE_PS5);
    BR_CHECK(pad == 0);
    for (int frame = 1; frame <= 60; frame++) {
        if (frame == 5) {
            br_input_inject_gamepad_axis(a, pad, SDL_GAMEPAD_AXIS_LEFTX, 0.37f); /* not a round number */
        }
        if (frame == 25) {
            br_input_inject_gamepad_axis(a, pad, SDL_GAMEPAD_AXIS_LEFTX, -0.81f);
        }
        if (frame == 10 || frame == 12) {
            br_input_inject_gamepad_button(a, pad, SDL_GAMEPAD_BUTTON_SOUTH, frame == 10);
        }
        if (frame == 30) {
            BR_CHECK(br_input_gamepad_attach_virtual(a, BR_GAMEPAD_TYPE_XBOXONE) == 1);
        }
        if (frame == 40) {
            BR_CHECK(br_input_gamepad_detach_virtual(a, 1));
        }
        br_playtest_step(a, 1);
    }
    char *dump_a = br_playtest_dump_json(a);
    BR_CHECK(br_playtest_record_stop(a));
    br_engine_destroy(a);
    char *script = SDL_LoadFile(at_exe("pt_recorded_pad.json"), NULL);
    BR_CHECK(script != NULL && SDL_strstr(script, "\"action\":\"connect\",\"type\":\"ps5\",\"slot\":0") != NULL);
    BR_CHECK(script != NULL && SDL_strstr(script, "\"action\":\"disconnect\",\"slot\":1") != NULL);
    SDL_free(script);

    br_engine *b = make_game();
    BR_CHECK(br_playtest_run_script(b, at_exe("pt_recorded_pad.json")));
    char *dump_b = br_playtest_dump_json(b);
    BR_CHECK(dump_a != NULL && dump_b != NULL && SDL_strcmp(dump_a, dump_b) == 0);
    BR_CHECK(br_input_gamepad_type(b, 0) == BR_GAMEPAD_TYPE_PS5 && !br_input_gamepad_connected(b, 1));
    cJSON *jumps = br_playtest_get(b, "hero:walker.jumps");
    BR_CHECK(jumps != NULL && jumps->valueint == 1);
    cJSON_Delete(jumps);
    if (dump_a != NULL && dump_b != NULL && SDL_strcmp(dump_a, dump_b) != 0) {
        printf("record:\n%s\nreplay:\n%s\n", dump_a, dump_b);
    }
    br_free(dump_a);
    br_free(dump_b);
    br_engine_destroy(b);
}

/* ---- the interactive protocol, one command per call */

static bool reply_has(br_engine *e, const char *command, const char *expected)
{
    char *reply = br_playtest_command(e, command);
    bool ok = reply != NULL && SDL_strstr(reply, expected) != NULL;
    if (!ok) {
        printf("command %s\n  reply %s\n  expected to contain %s\n", command, reply, expected);
    }
    br_free(reply);
    return ok;
}

static void test_interactive(void)
{
    br_engine *e = make_game();
    BR_CHECK(reply_has(e, "{\"cmd\":\"step\",\"frames\":5}", "{\"ok\":true,\"frame\":5}"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:position\"}", "\"value\":[100,50]"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"key\",\"key\":\"Right\",\"action\":\"down\"}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"step\",\"frames\":60}", "\"frame\":65"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:x\"}", "\"value\":160"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"key\",\"key\":\"Right\",\"action\":\"up\"}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"key\",\"key\":\"Space\"}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"step\"}", "\"frame\":66"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:walker.jumps\"}", "\"value\":1"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"game:score\"}", "\"value\":10"));
    /* set: node property, element, component field, exposed field */
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"hero:position.y\",\"value\":7}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:y\"}", "\"value\":7"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"hero:walker.speed\",\"value\":0}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"game:mode\",\"value\":\"boss\"}", "\"ok\":true"));
    BR_CHECK(s_game.mode[0] == 'b');
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"game:mode\",\"value\":\"boss\"}", "\"ok\":true"));
    /* queries */
    BR_CHECK(reply_has(e, "{\"cmd\":\"find\",\"label\":\"hero\"}", "\"found\":true,\"type\":\"container\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"find\",\"label\":\"villain\"}", "\"found\":false"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"list\",\"component\":\"walker\"}", "\"nodes\":[{\"label\":\"hero\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"dump\"}", "\"state\":{\"game\":{\"score\":10,\"mode\":\"boss\"}}"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"stats\"}", "\"stats\":{\"frame\":66"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"log\",\"message\":\"hello from the test\"}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"logs\",\"count\":1}", "hello from the test"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"wait_until\",\"log\":\"hello from\"}", "\"ok\":true,\"frame\":66"));
    /* errors come back as ok:false with a reason; the game keeps going */
    BR_CHECK(reply_has(e, "not json", "\"ok\":false,\"frame\":66,\"error\":\"not JSON"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:colour\"}", "unknown property 'colour'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"villain:x\"}", "no node labelled 'villain'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"game:score\",\"op\":\"<\",\"value\":5}",
                       "expect failed: game:score is 10, expected < 5"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"screenshot\",\"file\":\"x.png\"}", "\"ok\":false"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\"}", "gamepad: missing \\\"action\\\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"touch\",\"action\":3}", "must be a string"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"key\",\"key\":\"Space\",\"frames\":-5}", "\"ok\":true")); /* clamped to 1 */
    BR_CHECK(reply_has(e, "{\"cmd\":\"logs\",\"count\":-1}", "\"lines\":[]"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"step\",\"frames\":-3}", "\"ok\":true,\"frame\":66"));
    BR_CHECK(br_playtest_failed(e));
    BR_CHECK(reply_has(e, "{\"cmd\":\"quit\"}", "\"ok\":true"));
    br_engine_destroy(e);
}

/* ---- gamepad commands: a virtual pad of a family, buttons, axes, disconnect */

static void test_gamepad(void)
{
    br_engine *e = make_game();
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"connect\",\"type\":\"ps5\"}", "\"slot\":0"));
    BR_CHECK(br_input_gamepad_type(e, 0) == BR_GAMEPAD_TYPE_PS5);
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"connect\",\"type\":\"xbox\",\"slot\":3}", "\"slot\":3"));
    BR_CHECK(br_input_gamepad_family_of(br_input_gamepad_type(e, 3)) == BR_GAMEPAD_FAMILY_XBOX);
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"axis\",\"axis\":\"leftx\",\"value\":1}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"step\",\"frames\":60}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:x\"}", "\"value\":160")); /* 60 units/s for 1 s */
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"tap\",\"button\":\"a\",\"frames\":2}", "\"ok\":true"));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_gamepad_pressed(e, 0, SDL_GAMEPAD_BUTTON_SOUTH));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_gamepad_down(e, 0, SDL_GAMEPAD_BUTTON_SOUTH));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_gamepad_released(e, 0, SDL_GAMEPAD_BUTTON_SOUTH));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"down\",\"button\":\"dpup\",\"slot\":5}", "\"ok\":true"));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_gamepad_down(e, 5, SDL_GAMEPAD_BUTTON_DPAD_UP)); /* an injected pad, no device */
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"disconnect\",\"slot\":5}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"disconnect\"}", "\"ok\":true"));
    BR_CHECK(!br_input_gamepad_connected(e, 0) && !br_input_gamepad_connected(e, 5) && br_input_gamepad_connected(e, 3));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"disconnect\"}", "no pad in slot 0"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"connect\",\"type\":\"n64\"}", "unknown type 'n64'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"tap\",\"button\":\"jump\"}", "needs \\\"button\\\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"axis\",\"axis\":\"leftx\"}", "number \\\"value\\\""));
    br_engine_destroy(e);
}

/* ---- "action" drives any action, also code-bound ones without a key, with analogue values */

static void test_action(void)
{
    br_engine *e = make_game();
    br_input_action_bind_gamepad_axis(e, "throttle", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1); /* no key, not in game.json */
    BR_CHECK(reply_has(e, "{\"cmd\":\"action\",\"name\":\"throttle\",\"action\":\"press\",\"value\":0.25}", "\"ok\":true"));
    br_playtest_step(e, 10);
    BR_CHECK(br_input_action_value(e, "throttle") == 0.25f && !br_input_action_down(e, "throttle")); /* held, below 0.5 */
    BR_CHECK(reply_has(e, "{\"cmd\":\"action\",\"name\":\"throttle\",\"action\":\"release\"}", "\"ok\":true"));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_action_value(e, "throttle") == 0.0f);
    BR_CHECK(reply_has(e, "{\"cmd\":\"action\",\"name\":\"throttle\",\"frames\":3}", "\"ok\":true")); /* tap, 3 frames */
    br_playtest_step(e, 1);
    BR_CHECK(br_input_action_pressed(e, "throttle"));
    br_playtest_step(e, 2);
    BR_CHECK(br_input_action_down(e, "throttle"));
    br_playtest_step(e, 1);
    BR_CHECK(br_input_action_released(e, "throttle"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"action\",\"name\":\"nitro\"}", "unknown input action 'nitro'"));
    br_engine_destroy(e);
}

/* ---- examples that talk to the GPU directly still run (and exit 0) under --headless, the mode
 * play-test scripts run in on CI */

static void test_examples_headless(void)
{
    const char *names[] = { "hello_quad", "sprites_demo", "text_demo" };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(names); i++) {
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%s%s.exe", SDL_GetBasePath(), names[i]);
        if (!SDL_GetPathInfo(path, NULL)) {
            SDL_snprintf(path, sizeof(path), "%s%s", SDL_GetBasePath(), names[i]); /* no .exe outside Windows */
        }
        if (!SDL_GetPathInfo(path, NULL)) {
            printf("skipped %s: not built next to the test\n", names[i]);
            continue;
        }
        const char *args[] = { path, "--headless", "--frames", "3", NULL };
        SDL_Process *process = SDL_CreateProcess(args, false);
        int code = -1;
        BR_CHECK(process != NULL && SDL_WaitProcess(process, true, &code));
        BR_CHECK(code == 0);
        if (code != 0) {
            printf("%s --headless exited with %d\n", names[i], code);
        }
        SDL_DestroyProcess(process);
    }
}

int main(void)
{
    test_parse_errors();
    test_action();
    test_gamepad();
    test_interactive();
    test_record_replay();
    test_record_replay_gamepad();
    test_expect_exit_code();
    test_examples_headless();
    return br_test_finish();
}
