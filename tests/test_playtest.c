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
            br_node_set_position(e, n, br_vec2_make(p.x + w->speed * dt, p.y));
        }
        /* the left stick of pad 0 moves it too; its south button jumps */
        p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + w->speed * br_input_gamepad_axis(e, 0, SDL_GAMEPAD_AXIS_LEFTX) * dt, p.y));
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
    br_node_set_position(e, n, br_vec2_make(100.0f, 50.0f));
    br_node_add_child(e, br_stage_root(e), n);
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

/* ---- hardening (R5 review): long view labels, unknown arguments, phantom pads, injected pads
 * with a family, components with a dot, the window command, out-of-range ints */

typedef struct dotted {
    br_vec2 offset;
} dotted;

static void test_hardening(void)
{
    br_engine *e = make_game();
    /* a "view/node" label longer than the buffer used to write a NUL past it on the stack */
    static char cmd[6000];
    char label[5001];
    SDL_memset(label, 'A', sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    SDL_snprintf(cmd, sizeof(cmd), "{\"cmd\":\"click\",\"label\":\"%s/x\"}", label);
    BR_CHECK(reply_has(e, cmd, "too long"));
    SDL_snprintf(cmd, sizeof(cmd), "{\"cmd\":\"find\",\"label\":\"%s/x\"}", label);
    BR_CHECK(reply_has(e, cmd, "\"ok\""));

    /* a typo in an argument name fails instead of falling back to the default */
    BR_CHECK(reply_has(e, "{\"cmd\":\"key\",\"key\":\"Right\",\"frame\":30}", "key: unknown argument 'frame'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"wait_until\",\"path\":\"game:score\",\"value\":0,\"timeout_frame\":5}",
                       "unknown argument 'timeout_frame'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"quit\",\"now\":true}", "takes: nothing"));
    write_file(at_exe("pt_bad_arg.json"), "[ { \"cmd\": \"step\", \"frame\": 2 } ]");
    BR_CHECK(!br_playtest_load_script(e, at_exe("pt_bad_arg.json")));
    BR_CHECK(SDL_strstr(br_playtest_last_error(e), "step 1: step: unknown argument 'frame'") != NULL);

    /* a tap released after "disconnect" does not bring the pad back */
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"tap\",\"button\":\"a\",\"slot\":1}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"disconnect\",\"slot\":1}", "\"ok\":true"));
    br_playtest_step(e, 3);
    BR_CHECK(!br_input_gamepad_connected(e, 1) && br_input_gamepad_count(e) == 0);

    /* an injected pad (no SDL device) of a family */
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"connect\",\"type\":\"ps4\",\"injected\":true,\"slot\":2}",
                       "\"slot\":2"));
    br_gamepad_info info;
    BR_CHECK(br_input_gamepad_info(e, 2, &info) && info.injected && info.type == BR_GAMEPAD_TYPE_PS4);
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"connect\",\"type\":\"xbox\",\"injected\":true,\"slot\":2}",
                       "slot 2 is taken"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"gamepad\",\"action\":\"disconnect\",\"slot\":2}", "\"ok\":true"));

    /* components whose name has a dot (engine ones: camera.follow, gltf.collider) */
    static const br_field_desc fields[] = { { "offset", BR_FIELD_VEC2, offsetof(dotted, offset) } };
    br_component_desc desc = { "pt.follow", sizeof(dotted), fields, BR_ARRAY_COUNT(fields) };
    uint32_t id = br_component_register(e, &desc);
    br_node hero = br_node_find_by_label(e, br_stage_root(e), "hero", true);
    dotted *d = br_component_add(e, hero, id);
    BR_CHECK(d != NULL);
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"hero:pt.follow.offset\",\"value\":[3,4]}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"hero:pt.follow.offset.y\",\"value\":9}", "\"ok\":true"));
    BR_CHECK(d != NULL && d->offset.x == 3.0f && d->offset.y == 9.0f);
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:pt.follow.offset.x\"}", "\"value\":3"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:pt.follow\"}", "name a field of 'pt.follow'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"hero:walker.jumps\"}", "\"value\":0")); /* plain names still work */

    /* int fields clamp out-of-range numbers instead of an undefined cast */
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"game:score\",\"value\":1e20}", "\"ok\":true"));
    BR_CHECK(s_game.score == INT32_MAX);
    s_game.score = 0;

    /* window: headless, the screen size every camera sees */
    br_camera c = br_camera_default_2d();
    br_node cam = br_camera_create(e, &c);
    br_node_add_child(e, br_stage_root(e), cam);
    BR_CHECK(reply_has(e, "{\"cmd\":\"window\",\"width\":720,\"height\":1280}", "\"ok\":true"));
    br_playtest_step(e, 1);
    br_camera_view v = br_camera_get_view(e, cam);
    BR_CHECK(v.screen_size.x == 720.0f && v.screen_size.y == 1280.0f);
    BR_CHECK(reply_has(e, "{\"cmd\":\"window\",\"width\":2520,\"height\":1080}", "\"ok\":true"));
    v = br_camera_get_view(e, cam);
    BR_CHECK(v.screen_size.x == 2520.0f && v.screen_size.y == 1080.0f);
    BR_CHECK(reply_has(e, "{\"cmd\":\"window\",\"width\":0,\"height\":1080}", "16..16384"));
    br_engine_destroy(e);
}

/* Log lines from other threads (asset workers) while the main thread reads the ring. */
static int SDLCALL log_spammer(void *data)
{
    (void)data;
    for (int i = 0; i < 2000; i++) {
        br_log_info("spam line %d from a worker thread", i);
    }
    return 0;
}

static void test_log_threads(void)
{
    br_engine *e = make_game();
    BR_CHECK(reply_has(e, "{\"cmd\":\"logs\",\"count\":1}", "\"ok\":true")); /* hooks the log */
    SDL_Thread *threads[4];
    for (int i = 0; i < 4; i++) {
        threads[i] = SDL_CreateThread(log_spammer, "spam", NULL);
    }
    int found = 0;
    for (int i = 0; i < 200; i++) {
        char *reply = br_playtest_command(e, "{\"cmd\":\"expect\",\"log\":\"from a worker thread\"}");
        found += reply != NULL && SDL_strstr(reply, "\"ok\":true") != NULL ? 1 : 0;
        br_free(reply);
    }
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(threads[i], NULL);
    }
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"log\":\"spam line 1999 from a worker thread\"}", "\"ok\":true"));
    BR_CHECK(found >= 0);
    br_engine_destroy(e);
}

/* The recorder drops SDL's mouse copies of touch / pen input and keeps relative deltas. */
static void test_record_synthetic_mouse(void)
{
    br_engine *a = make_game();
    BR_CHECK(br_playtest_record_start(a, at_exe("pt_recorded_mouse.json")));
    br_playtest_step(a, 1);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.which = SDL_TOUCH_MOUSEID;
    ev.motion.x = 10.0f;
    ev.motion.y = 20.0f;
    br_app_event(a, &ev);
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.which = SDL_PEN_MOUSEID;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.down = true;
    br_app_event(a, &ev);
    br_playtest_step(a, 1);
    br_input_inject_pointer_motion(a, 50.0f, 50.0f, 7.0f, -3.0f); /* relative mode: pinned, but moving */
    br_playtest_step(a, 1);
    BR_CHECK(br_playtest_record_stop(a));
    br_engine_destroy(a);
    char *script = SDL_LoadFile(at_exe("pt_recorded_mouse.json"), NULL);
    BR_CHECK(script != NULL && SDL_strstr(script, "\"x\":10") == NULL && SDL_strstr(script, "\"down\"") == NULL);
    BR_CHECK(script != NULL && SDL_strstr(script, "\"x\":50,\"y\":50,\"dx\":7,\"dy\":-3") != NULL);
    SDL_free(script);
    br_engine *b = make_game();
    BR_CHECK(br_playtest_load_script(b, at_exe("pt_recorded_mouse.json")));
    bool saw_delta = false;
    for (int i = 0; i < 10 && br_playtest_running(b); i++) {
        br_playtest_step(b, 1);
        saw_delta = saw_delta || (br_input_mouse_delta(b).x == 7.0f && br_input_mouse_delta(b).y == -3.0f);
    }
    BR_CHECK(saw_delta && !br_input_mouse_down(b, SDL_BUTTON_LEFT));
    br_engine_destroy(b);
}

/* ---- 3D nodes inside a view3d: "view/node:property" for get, set, expect, find, click, dump */

static int s_cube_clicks;

static void on_cube_click(br_engine *e, br_event3d *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    s_cube_clicks++;
}

static void test_3d_paths(void)
{
    br_engine *e = make_game();
    br_node view = br_view3d_create(e, 200.0f, 200.0f);
    br_node_set_label(e, view, "world");
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.0f, 10.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_material3d_desc md = br_material3d_default(BR_MATERIAL3D_PBR);
    br_node3d group = br_node3d_create_container(e);
    br_node3d_set_label(e, group, "props");
    br_node3d_add_child(e, br_view3d_root(e, view), group);
    br_node3d cube = br_mesh3d_create(e, br_geometry3d_cube(e, br_vec3_make(2.0f, 2.0f, 2.0f)), br_material3d_create(e, &md));
    br_node3d_set_label(e, cube, "cube");
    br_node3d_set_position(e, cube, br_vec3_make(1.0f, 2.0f, 3.0f));
    br_node3d_add_child(e, group, cube);
    br_node3d_on(e, cube, BR_EVENT_CLICK, on_cube_click, NULL);

    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/cube:position\"}", "\"value\":[1,2,3]"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"*/cube:position.z\"}", "\"value\":3")); /* any view */
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/props:children\"}", "\"value\":1"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/cube:position.y\",\"value\":5}", "\"ok\":true"));
    BR_CHECK(br_node3d_position(e, cube).y == 5.0f && br_node3d_position(e, cube).x == 1.0f);
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/cube:position\",\"value\":[0,0,0]}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/cube:euler\",\"value\":[0,1.5,0]}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"world/cube:euler.y\",\"value\":1.5}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/cube:euler\",\"value\":[0,0,0]}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/props:visible\",\"value\":false}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"world/props:visible\",\"value\":false}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/props:visible\",\"value\":true}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/cube:screen_position\"}", "\"value\":[100,100]"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"find\",\"label\":\"world/cube\"}", "\"found\":true,\"type\":\"mesh3d\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/ghost:exists\"}", "\"value\":false"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/ghost:position\"}", "no 3D node labelled 'ghost'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"moon/cube:position\"}", "no view3d labelled 'moon'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"world/cube:bounds\"}", "unknown 3D property 'bounds'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"set\",\"path\":\"world/cube:scale\",\"value\":[1,2]}", "expected 3 numbers"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"dump\"}", "\"views3d\":[{\"view\":\"world\",\"root\":{\"label\":\"root\",\"type\":\"group3d\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"dump\"}", "\"label\":\"cube\",\"type\":\"mesh3d\",\"position\":[0,0,0]"));
    /* click: the node's origin through the view's camera, picked as a 3D CLICK */
    BR_CHECK(reply_has(e, "{\"cmd\":\"click\",\"label\":\"world/cube\"}", "\"ok\":true"));
    br_playtest_step(e, 1); /* the move */
    BR_CHECK(!br_input_mouse_down(e, SDL_BUTTON_LEFT) && br_input_mouse_delta(e).x > 0.0f);
    br_playtest_step(e, 1); /* the press: no pointer jump while down (camera drags stay put) */
    BR_CHECK(br_input_mouse_down(e, SDL_BUTTON_LEFT) && br_input_mouse_delta(e).x == 0.0f);
    br_playtest_step(e, 1); /* the release */
    BR_CHECK(s_cube_clicks == 1);
    br_engine_destroy(e);
}

/* ---- click by label with a zoomed, offset 2D camera: the pointer goes where the node is drawn */

static int s_button_clicks;

static void on_button_click(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    s_button_clicks++;
}

static void test_click_with_camera(void)
{
    br_engine *e = make_game();
    br_camera_set_headless_screen(e, 1280.0f, 720.0f);
    br_camera c = br_camera_default_2d();
    c.orthographic_size = 180.0f;
    br_node cam = br_camera_create(e, &c);
    br_node_set_position(e, cam, br_vec2_make(1000.0f, 1000.0f));
    br_node_add_child(e, br_stage_root(e), cam);
    br_node button = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, button, 40.0f, 40.0f);
    br_node_set_position(e, button, br_vec2_make(1000.0f, 1000.0f));
    br_node_set_label(e, button, "button");
    br_node_set_event_mode(e, button, BR_EVENT_MODE_STATIC);
    br_node_on(e, button, BR_EVENT_CLICK, on_button_click, NULL);
    br_node_add_child(e, br_stage_root(e), button);
    br_playtest_step(e, 1);
    BR_CHECK(reply_has(e, "{\"cmd\":\"click\",\"label\":\"button\"}", "\"ok\":true"));
    br_playtest_step(e, 3); /* move, press, release */
    BR_CHECK(s_button_clicks == 1);
    /* the pointer went to the window point the camera shows the centre (1020, 1020) at */
    BR_CHECK(SDL_fabsf(br_input_mouse_position(e).x - 680.0f) < 0.01f);
    BR_CHECK(SDL_fabsf(br_input_mouse_position(e).y - 400.0f) < 0.01f);
    br_engine_destroy(e);
}

/* ---- "profile" waits and replies with the profiler summary; profile:<key> and engine:scene paths */

static void test_profile_and_scene(void)
{
    br_engine *e = br_engine_create("test_playtest_profile", BRATY_SOURCE_DIR "/tests/data");
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"engine:scene\"}", "\"value\":\"\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"profile\",\"frames\":10}", "\"ok\":true,\"frame\":10,\"profile\":{\"frames\":9"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"profile\"}", "\"frame\":310,\"profile\":{\"frames\":299"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"profile:frame_ms_p95\",\"op\":\">=\",\"value\":0}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"profile:verdict\"}", "\"value\":\""));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"profile:fps\"}", "\"ok\":true"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"get\",\"path\":\"profile:nope\"}", "no profile key 'nope'"));
    BR_CHECK(reply_has(e, "{\"cmd\":\"help\"}", "\"cmd\":\"profile\""));
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    BR_CHECK(br_scene_is_loaded(e, a));
    BR_CHECK(reply_has(e, "{\"cmd\":\"expect\",\"path\":\"engine:scene\",\"value\":\"a\"}", "\"ok\":true"));
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

/* Path of an example built next to this test; false (skipped) if it is not there. */
static bool example_path(const char *name, char *path, size_t size)
{
    SDL_snprintf(path, size, "%s%s.exe", SDL_GetBasePath(), name);
    if (!SDL_GetPathInfo(path, NULL)) {
        SDL_snprintf(path, size, "%s%s", SDL_GetBasePath(), name); /* no .exe outside Windows */
    }
    if (!SDL_GetPathInfo(path, NULL)) {
        printf("skipped %s: not built next to the test\n", name);
        return false;
    }
    return true;
}

static void test_examples_headless(void)
{
    const char *names[] = { "hello_quad", "sprites_demo", "text_demo" };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(names); i++) {
        char path[1024];
        if (!example_path(names[i], path, sizeof(path))) {
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

/* ---- --playtest-stdin over real pipes: every bad line gets exactly one {"ok":false} reply, the
 * session goes on, and closing stdin ends the process */

static void test_stdin_session(void)
{
#ifndef BRATY_PLAYTEST_STDIN
    printf("test_stdin_session skipped: BRATY_PLAYTEST_STDIN is off (release preset)\n");
    return;
#endif
    char path[1024];
    if (!example_path("hello_quad", path, sizeof(path))) {
        return;
    }
    const char *args[] = { path, "--headless", "--playtest-stdin", NULL };
    SDL_Process *process = SDL_CreateProcess(args, true);
    BR_CHECK(process != NULL);
    if (process == NULL) {
        return;
    }
    SDL_IOStream *in = SDL_GetProcessInput(process);
    const char *lines = "not json\n"
                        "[1, 2]\n"
                        "{\"cmd\":\"fly\"}\n"
                        "{\"cmd\":\"step\",\"frames\":\"ten\"}\n"
                        "{\"cmd\":\"get\",\"path\":5}\n";
    SDL_WriteIO(in, lines, SDL_strlen(lines));
    static char huge[70000]; /* longer than the 64 KB line buffer */
    SDL_memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 2] = '\n';
    size_t sent = 0;
    while (sent < sizeof(huge) - 1) { /* a pipe may take it in parts */
        size_t n = SDL_WriteIO(in, huge + sent, sizeof(huge) - 1 - sent);
        if (n == 0 && SDL_GetIOStatus(in) != SDL_IO_STATUS_NOT_READY) {
            break;
        }
        if (n == 0) {
            SDL_Delay(1); /* the pipe is full until the game reads */
        }
        sent += n;
    }
    BR_CHECK(sent == sizeof(huge) - 1);
    const char *last = "{\"cmd\":\"step\",\"frames\":2}\n{\"cmd\":\"help\"}\n";
    SDL_WriteIO(in, last, SDL_strlen(last));
    SDL_CloseIO(in); /* closed stdin: the game quits */
    size_t size = 0;
    int code = -1;
    char *out = SDL_ReadProcess(process, &size, &code);
    BR_CHECK(out != NULL);
    int replies = 0;
    int failed = 0;
    for (char *line = out; line != NULL && *line != '\0'; replies++) {
        failed += SDL_strncmp(line, "{\"ok\":false", 11) == 0 ? 1 : 0;
        char *next = SDL_strchr(line, '\n');
        line = next != NULL ? next + 1 : NULL;
    }
    BR_CHECK(replies == 8 && failed == 6);
    BR_CHECK(out != NULL && SDL_strstr(out, "unknown command 'fly'") != NULL);
    BR_CHECK(out != NULL && SDL_strstr(out, "step: \\\"frames\\\" must be a number") != NULL);
    BR_CHECK(out != NULL && SDL_strstr(out, "get: \\\"path\\\" must be a string") != NULL);
    BR_CHECK(out != NULL && SDL_strstr(out, "longer than 64 KB") != NULL);
    BR_CHECK(out != NULL && SDL_strstr(out, "{\"ok\":true,\"frame\":2}") != NULL);
    BR_CHECK(out != NULL && SDL_strstr(out, "{\"cmd\":\"gamepad\",\"args\":\"action:string") != NULL);
    BR_CHECK(code == 1); /* exits on its own; 1 because commands failed */
    if (replies != 8) {
        printf("stdin session output:\n%s\n", out != NULL ? out : "(none)");
    }
    SDL_free(out);
    SDL_DestroyProcess(process);
}

int main(void)
{
    test_parse_errors();
    test_stdin_session();
    test_action();
    test_gamepad();
    test_hardening();
    test_log_threads();
    test_record_synthetic_mouse();
    test_3d_paths();
    test_click_with_camera();
    test_profile_and_scene();
    test_interactive();
    test_record_replay();
    test_record_replay_gamepad();
    test_expect_exit_code();
    test_examples_headless();
    return br_test_finish();
}
