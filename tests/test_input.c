/* test_input - br_input beyond test_events.c: keyboard extras, wheel ticks, text/IME, injected
 * and virtual gamepads (families, dead zones, slots, sensors, touchpad), actions (thresholds,
 * per player, contexts, injection, capture/rebinding, conflicts, JSON, save data), glyphs and
 * prompts. Headless. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>

static void step(br_engine *e)
{
    br_engine_step(e, 1.0f / 60.0f);
}

static bool near(float a, float b)
{
    return br_nearly_equal(a, b, 1e-3f);
}

/* API_REVIEW 11: the latch priority is a public constant, and input injected by a system that
 * runs before it counts in the same frame. */
static void inject_before_latch(br_engine *e, float dt, void *user)
{
    (void)dt;
    int *calls = user;
    if (++*calls == 2) {
        br_input_inject_key(e, SDL_SCANCODE_K, true);
    }
}

static void test_latch_priority(br_engine *e)
{
    static int calls = 0;
    BR_CHECK(BR_INPUT_PRIORITY_LATCH == -2000000);
    br_system_add(e, "test.inject", BR_PHASE_INPUT, BR_INPUT_PRIORITY_LATCH - 1000, inject_before_latch, &calls);
    step(e);
    BR_CHECK(!br_input_key_pressed(e, SDL_SCANCODE_K));
    step(e);   /* injected before the latch -> pressed in this same frame */
    BR_CHECK(br_input_key_pressed(e, SDL_SCANCODE_K));
    br_system_set_enabled(e, "test.inject", false);
    br_input_inject_key(e, SDL_SCANCODE_K, false);
    step(e);
}

static void test_keyboard_mouse_text(br_engine *e)
{
    br_input_inject_key(e, SDL_SCANCODE_LSHIFT, true);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    step(e);
    BR_CHECK(br_input_keycode_down(e, SDLK_SPACE));
    BR_CHECK(br_input_keycode_pressed(e, SDLK_SPACE));
    BR_CHECK((br_input_key_mods(e) & SDL_KMOD_SHIFT) != 0);
    BR_CHECK((br_input_key_mods(e) & SDL_KMOD_CTRL) == 0);
    BR_CHECK(br_input_last_device(e) == BR_INPUT_DEVICE_KEYBOARD_MOUSE);
    br_input_inject_key(e, SDL_SCANCODE_LSHIFT, false);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    step(e);
    BR_CHECK(br_input_keycode_released(e, SDLK_SPACE) && !br_input_keycode_down(e, SDLK_SPACE));
    BR_CHECK((br_input_key_mods(e) & SDL_KMOD_SHIFT) == 0);

    char name[32];
    BR_CHECK(br_input_key_name(SDL_SCANCODE_SPACE, name, sizeof(name)) && SDL_strcmp(name, "Space") == 0);
    BR_CHECK(!br_input_key_name(SDL_SCANCODE_UNKNOWN, name, sizeof(name)) && name[0] == '\0');

    /* wheel: smooth keeps fractions, ticks are whole notches */
    br_input_inject_wheel(e, 0.0f, 2.5f);
    step(e);
    BR_CHECK(near(br_input_mouse_wheel(e).y, 2.5f) && near(br_input_mouse_wheel_ticks(e).y, 2.0f));
    step(e);
    BR_CHECK(br_input_mouse_wheel_ticks(e).y == 0.0f);

    /* touch-synthesised mouse events feed the mouse unless switched off */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.which = SDL_TOUCH_MOUSEID;
    ev.motion.x = 50.0f;
    ev.motion.y = 60.0f;
    br_app_event(e, &ev);
    BR_CHECK(near(br_input_mouse_position(e).x, 50.0f));
    br_input_touch_set_mouse_emulation(e, false);
    ev.motion.x = 70.0f;
    br_app_event(e, &ev);
    BR_CHECK(near(br_input_mouse_position(e).x, 50.0f));
    br_input_touch_set_mouse_emulation(e, true);

    /* text and IME composition */
    br_input_inject_text(e, "h\xC3\xA9");
    step(e);
    BR_CHECK(SDL_strcmp(br_input_text(e), "h\xC3\xA9") == 0);
    SDL_zero(ev);
    ev.type = SDL_EVENT_TEXT_EDITING;
    ev.edit.text = "kan";
    ev.edit.start = 3;
    ev.edit.length = 0;
    br_app_event(e, &ev);
    int cursor = -1;
    BR_CHECK(SDL_strcmp(br_input_text_composition(e, &cursor, NULL), "kan") == 0 && cursor == 3);
    br_input_inject_text(e, "\xE6\xBC\xA2");
    BR_CHECK(br_input_text_composition(e, NULL, NULL)[0] == '\0');   /* committed text ends composition */
    step(e);
}

static void test_injected_gamepad(br_engine *e)
{
    BR_CHECK(!br_input_gamepad_connected(e, 2));
    br_input_inject_gamepad_button(e, 2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    step(e);
    BR_CHECK(br_input_gamepad_connected(e, 2) && br_input_gamepad_count(e) == 1);
    BR_CHECK(br_input_gamepad_pressed(e, 2, SDL_GAMEPAD_BUTTON_SOUTH));
    BR_CHECK(br_input_last_device(e) == BR_INPUT_DEVICE_GAMEPAD && br_input_last_gamepad(e) == 2);
    br_gamepad_info info;
    BR_CHECK(br_input_gamepad_info(e, 2, &info) && info.injected && info.type == BR_GAMEPAD_TYPE_GENERIC);
    BR_CHECK(info.battery_percent == -1 && !info.has_rumble);
    BR_CHECK(!br_input_gamepad_rumble(e, 2, 1.0f, 1.0f, 100));   /* no device: no rumble, no crash */
    br_input_inject_gamepad_button(e, 2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    step(e);
    BR_CHECK(br_input_gamepad_released(e, 2, SDL_GAMEPAD_BUTTON_SOUTH));

    /* dead zones: default inner 0.15, outer 0.95 */
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, 0.1f);
    BR_CHECK(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX) == 0.0f);
    BR_CHECK(near(br_input_gamepad_axis_raw(e, 2, SDL_GAMEPAD_AXIS_LEFTX), 0.1f));
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, 0.55f);
    BR_CHECK(near(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX), 0.5f));
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, -1.0f);
    BR_CHECK(near(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX), -1.0f));
    /* radial: (0.6, 0.8) has length 1 -> direction kept, length 1 */
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, 0.6f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTY, 0.8f);
    br_vec2 stick = br_input_gamepad_stick(e, 2, BR_GAMEPAD_STICK_LEFT);
    BR_CHECK(near(stick.x, 0.6f) && near(stick.y, 0.8f));
    /* radial, not per axis: (0.12, 0.12) has length 0.17 > 0.15, so x is not zeroed */
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, 0.12f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTY, 0.12f);
    BR_CHECK(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX) > 0.0f);
    BR_CHECK(br_vec2_length(br_input_gamepad_stick(e, 2, BR_GAMEPAD_STICK_RIGHT)) == 0.0f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.5f);
    BR_CHECK(near(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.4375f));
    br_input_gamepad_set_deadzone(e, 0.0f, 1.0f);
    BR_CHECK(near(br_input_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.5f));
    br_input_gamepad_set_deadzone(e, 0.15f, 0.95f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTX, 0.0f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFTY, 0.0f);
    br_input_inject_gamepad_axis(e, 2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f);

    /* explicit player assignment: slot 2 -> slot 0 */
    BR_CHECK(br_input_gamepad_assign(e, 2, 0));
    BR_CHECK(br_input_gamepad_connected(e, 0) && !br_input_gamepad_connected(e, 2) && br_input_last_gamepad(e) == 0);
    BR_CHECK(!br_input_gamepad_assign(e, 5, 1));
    br_input_inject_gamepad_disconnect(e, 0);
    BR_CHECK(br_input_gamepad_count(e) == 0);
    br_input_inject_gamepad_button(e, -1, SDL_GAMEPAD_BUTTON_SOUTH, true);   /* bad slot: logged, ignored */
    BR_CHECK(br_input_gamepad_count(e) == 0);
    step(e);
}

static void test_actions(br_engine *e)
{
    br_input_actions_clear(e);
    br_input_action_bind_key(e, "jump", SDL_SCANCODE_SPACE);
    br_input_action_bind_gamepad_button(e, "jump", SDL_GAMEPAD_BUTTON_SOUTH);
    br_input_action_bind_gamepad_axis(e, "brake", SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
    br_input_action_set_threshold(e, "brake", 0.8f);

    /* threshold: 0.75 after the dead zone is not enough, 1.0 is */
    br_input_inject_gamepad_axis(e, 1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.75f);
    step(e);
    BR_CHECK(near(br_input_action_value(e, "brake"), 0.75f) && !br_input_action_down(e, "brake"));
    br_input_inject_gamepad_axis(e, 1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f);
    step(e);
    BR_CHECK(br_input_action_pressed(e, "brake"));
    BR_CHECK(br_input_action_last_gamepad(e, "brake") == 1);   /* API_REVIEW 12: rumble the right pad */
    br_input_inject_gamepad_axis(e, 1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f);

    /* per player: pad 3 presses jump; only player 3 sees it */
    br_input_inject_gamepad_button(e, 3, SDL_GAMEPAD_BUTTON_SOUTH, true);
    step(e);
    BR_CHECK(br_input_action_down(e, "jump") && br_input_action_last_gamepad(e, "jump") == 3);
    BR_CHECK(br_input_action_last_gamepad(e, "nope") == -1);
    BR_CHECK(br_input_action_down_for(e, "jump", 3) && !br_input_action_down_for(e, "jump", 1));
    BR_CHECK(!br_input_action_down_for(e, "jump", -1));
    br_input_inject_gamepad_button(e, 3, SDL_GAMEPAD_BUTTON_SOUTH, false);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    step(e);
    BR_CHECK(br_input_action_down_for(e, "jump", -1) && !br_input_action_down_for(e, "jump", 3));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    step(e);
    br_input_inject_gamepad_disconnect(e, 1);
    br_input_inject_gamepad_disconnect(e, 3);

    /* API_REVIEW 10: inject an action without knowing its bindings; 1 frame = tap */
    br_input_inject_action(e, "jump", 1.0f, 1);
    step(e);
    BR_CHECK(br_input_action_pressed(e, "jump"));
    step(e);
    BR_CHECK(br_input_action_released(e, "jump"));
    step(e);
    BR_CHECK(!br_input_action_down(e, "jump"));
    br_input_inject_action(e, "jump", 1.0f, 3);
    step(e);
    step(e);
    step(e);
    BR_CHECK(br_input_action_down(e, "jump") && !br_input_action_pressed(e, "jump"));
    step(e);
    BR_CHECK(br_input_action_released(e, "jump"));

    /* composites: WASD diagonal is normalised */
    br_input_action_bind_key(e, "left", SDL_SCANCODE_A);
    br_input_action_bind_key(e, "right", SDL_SCANCODE_D);
    br_input_action_bind_key(e, "up", SDL_SCANCODE_W);
    br_input_action_bind_key(e, "down", SDL_SCANCODE_S);
    br_input_inject_key(e, SDL_SCANCODE_D, true);
    br_input_inject_key(e, SDL_SCANCODE_W, true);
    step(e);
    br_vec2 move = br_input_action_vec2(e, "left", "right", "up", "down");
    BR_CHECK(near(move.x, 0.70711f) && near(move.y, -0.70711f));
    BR_CHECK(near(br_input_action_axis(e, "left", "right"), 1.0f));
    br_input_inject_key(e, SDL_SCANCODE_D, false);
    br_input_inject_key(e, SDL_SCANCODE_W, false);
    step(e);
    BR_CHECK(br_input_action_vec2(e, "left", "right", "up", "down").x == 0.0f);

    /* binding list editing */
    BR_CHECK(br_input_action_binding_count(e, "jump") == 2);
    br_input_binding b;
    BR_CHECK(br_input_action_binding_get(e, "jump", 1, &b) && b.kind == BR_INPUT_BINDING_GAMEPAD_BUTTON);
    br_input_binding enter = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_RETURN, 1 };
    BR_CHECK(br_input_action_binding_set(e, "jump", 0, enter));
    br_input_binding bad = { BR_INPUT_BINDING_MOUSE, 42, 1 };
    BR_CHECK(!br_input_action_binding_set(e, "jump", 0, bad));
    BR_CHECK(!br_input_action_binding_set(e, "jump", 9, enter));
    BR_CHECK(br_input_action_binding_remove(e, "jump", 1) && br_input_action_binding_count(e, "jump") == 1);
    BR_CHECK(br_input_action_binding_count(e, "nope") == 0 && !br_input_action_binding_remove(e, "nope", 0));
}

static void test_contexts_and_rebinding(br_engine *e)
{
    br_input_actions_clear(e);
    br_input_action_bind_key(e, "jump", SDL_SCANCODE_SPACE);
    br_input_action_set_context(e, "jump", "gameplay");
    br_input_action_bind_key(e, "confirm", SDL_SCANCODE_SPACE);
    br_input_action_set_context(e, "confirm", "menu");
    br_input_action_bind_key(e, "pause", SDL_SCANCODE_ESCAPE);   /* no context: always live */

    BR_CHECK(SDL_strcmp(br_input_context_top(e), "") == 0);
    br_input_context_push(e, "gameplay");
    br_input_context_push(e, "menu");
    BR_CHECK(SDL_strcmp(br_input_context_top(e), "menu") == 0);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, true);
    step(e);
    BR_CHECK(br_input_action_pressed(e, "confirm") && !br_input_action_down(e, "jump"));
    BR_CHECK(br_input_action_down(e, "pause"));
    /* the menu closes while Space is still held: jump must not fire until Space is released */
    br_input_context_pop(e);
    step(e);
    BR_CHECK(!br_input_action_down(e, "confirm") && !br_input_action_down(e, "jump"));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, false);
    step(e);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    step(e);
    BR_CHECK(br_input_action_pressed(e, "jump"));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    step(e);
    br_input_context_pop(e);
    br_input_context_pop(e);   /* extra pop: warned, harmless */

    /* conflicts: same context -> conflict; different contexts may share */
    br_input_binding space = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_SPACE, 1 };
    br_input_binding esc = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_ESCAPE, 1 };
    BR_CHECK(br_input_binding_conflict(e, space, "confirm") == NULL);
    BR_CHECK(br_str_equal(br_input_binding_conflict(e, esc, "jump"), "pause"));
    BR_CHECK(br_str_equal(br_input_binding_conflict(e, space, NULL), "jump"));

    /* rebinding: capture the next key; actions are idle meanwhile */
    br_input_capture_start(e, BR_INPUT_DEVICE_KEYBOARD_MOUSE);
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, true);
    step(e);
    BR_CHECK(!br_input_action_down(e, "pause"));
    br_input_binding got;
    BR_CHECK(br_input_capture_poll(e, &got) && got.kind == BR_INPUT_BINDING_KEY && got.code == SDL_SCANCODE_ESCAPE);
    BR_CHECK(!br_input_capture_poll(e, &got));   /* once */
    step(e);
    BR_CHECK(!br_input_action_down(e, "pause"));   /* still held from the capture: waits for release */
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, false);
    step(e);

    br_input_capture_start(e, BR_INPUT_DEVICE_GAMEPAD);
    br_input_inject_gamepad_axis(e, 0, SDL_GAMEPAD_AXIS_RIGHTY, -0.9f);
    step(e);
    BR_CHECK(br_input_capture_poll(e, &got) && got.kind == BR_INPUT_BINDING_GAMEPAD_AXIS &&
             got.code == SDL_GAMEPAD_AXIS_RIGHTY && got.direction == -1);
    br_input_inject_gamepad_disconnect(e, 0);
    br_input_capture_start(e, BR_INPUT_DEVICE_GAMEPAD);
    br_input_capture_cancel(e);
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, true);
    step(e);
    BR_CHECK(!br_input_capture_poll(e, &got) && br_input_action_down(e, "pause"));
    br_input_inject_key(e, SDL_SCANCODE_ESCAPE, false);
    step(e);
}

static void test_json_and_save(br_engine *e)
{
    br_input_actions_clear(e);
    cJSON *json = cJSON_Parse("{ \"jump\": { \"keys\": [\"Space\"], \"buttons\": [\"a\"], \"context\": \"gameplay\" },"
                              "  \"fire\": { \"mouse\": [1], \"axes\": [\"+righttrigger\"], \"threshold\": 0.3 } }");
    BR_CHECK(br_input_actions_from_json(e, json));
    cJSON_Delete(json);
    cJSON *out = br_input_actions_to_json(e);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    BR_CHECK(text != NULL && SDL_strcmp(text, "{\"jump\":{\"keys\":[\"Space\"],\"buttons\":[\"a\"],\"context\":\"gameplay\"},"
                                              "\"fire\":{\"mouse\":[1],\"axes\":[\"+righttrigger\"],\"threshold\":0.3}}") == 0);
    if (text != NULL) {
        printf("bindings json: %s\n", text);
    }
    cJSON_free(text);

    /* save, rebind, load: the saved bindings come back; actions not in the file keep theirs */
    br_save_remove(e, "test_input_bindings.json");
    BR_CHECK(!br_input_bindings_load(e, "test_input_bindings.json"));
    BR_CHECK(br_input_bindings_save(e, "test_input_bindings.json"));
    br_input_binding f = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_F, 1 };
    BR_CHECK(br_input_action_binding_set(e, "jump", 0, f));
    br_input_action_bind_key(e, "extra", SDL_SCANCODE_E);
    BR_CHECK(br_input_bindings_load(e, "test_input_bindings.json"));
    br_input_binding b;
    BR_CHECK(br_input_action_binding_count(e, "jump") == 2);
    BR_CHECK(br_input_action_binding_get(e, "jump", 0, &b) && b.code == SDL_SCANCODE_SPACE);
    BR_CHECK(br_input_action_binding_count(e, "extra") == 1);
    BR_CHECK(br_save_write(e, "test_input_bindings.json", "[1,2]", 5));
    BR_CHECK(!br_input_bindings_load(e, "test_input_bindings.json"));
    br_save_remove(e, "test_input_bindings.json");
}

static bool glyph_is(br_input_binding b, br_gamepad_type type, const char *label, const char *frame)
{
    br_glyph g;
    bool ok = br_input_binding_glyph(b, type, &g) && SDL_strcmp(g.label, label) == 0 && SDL_strcmp(g.frame, frame) == 0;
    if (!ok) {
        printf("glyph mismatch: got '%s' / '%s', want '%s' / '%s'\n", g.label, g.frame, label, frame);
    }
    return ok;
}

static void test_glyphs(br_engine *e)
{
    br_input_binding south = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 1 };
    br_input_binding east = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_EAST, 1 };
    br_input_binding north = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_NORTH, 1 };
    br_input_binding start = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_START, 1 };
    br_input_binding back = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_BACK, 1 };
    br_input_binding lb = { BR_INPUT_BINDING_GAMEPAD_BUTTON, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 1 };
    br_input_binding lt = { BR_INPUT_BINDING_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1 };
    br_input_binding stick = { BR_INPUT_BINDING_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFTX, -1 };
    br_input_binding space = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_SPACE, 1 };
    br_input_binding shift = { BR_INPUT_BINDING_KEY, SDL_SCANCODE_LSHIFT, 1 };
    br_input_binding lmb = { BR_INPUT_BINDING_MOUSE, SDL_BUTTON_LEFT, 1 };
    br_input_binding rmb = { BR_INPUT_BINDING_MOUSE, SDL_BUTTON_RIGHT, 1 };

    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_XBOXONE, "A", "xbox_a"));
    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_XBOX360, "A", "xbox_a"));
    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_GENERIC, "A", "xbox_a"));
    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_PS5, "Cross", "ps_cross"));
    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_PS4, "Cross", "ps_cross"));
    BR_CHECK(glyph_is(east, BR_GAMEPAD_TYPE_PS4, "Circle", "ps_circle"));
    BR_CHECK(glyph_is(north, BR_GAMEPAD_TYPE_PS3, "Triangle", "ps_triangle"));
    BR_CHECK(glyph_is(south, BR_GAMEPAD_TYPE_SWITCH_PRO, "B", "switch_b"));   /* positional: Nintendo south is B */
    BR_CHECK(glyph_is(east, BR_GAMEPAD_TYPE_SWITCH_PRO, "A", "switch_a"));
    BR_CHECK(glyph_is(north, BR_GAMEPAD_TYPE_XBOXONE, "Y", "xbox_y"));
    BR_CHECK(glyph_is(start, BR_GAMEPAD_TYPE_XBOXONE, "Menu", "xbox_start"));
    BR_CHECK(glyph_is(start, BR_GAMEPAD_TYPE_PS5, "Options", "ps_start"));
    BR_CHECK(glyph_is(start, BR_GAMEPAD_TYPE_SWITCH_PRO, "+", "switch_start"));
    BR_CHECK(glyph_is(back, BR_GAMEPAD_TYPE_PS5, "Create", "ps_back"));
    BR_CHECK(glyph_is(back, BR_GAMEPAD_TYPE_PS4, "Share", "ps_back"));
    BR_CHECK(glyph_is(lb, BR_GAMEPAD_TYPE_PS4, "L1", "ps_leftshoulder"));
    BR_CHECK(glyph_is(lb, BR_GAMEPAD_TYPE_SWITCH_PRO, "L", "switch_leftshoulder"));
    BR_CHECK(glyph_is(lt, BR_GAMEPAD_TYPE_XBOXONE, "LT", "xbox_lefttrigger"));
    BR_CHECK(glyph_is(lt, BR_GAMEPAD_TYPE_PS5, "L2", "ps_lefttrigger"));
    BR_CHECK(glyph_is(lt, BR_GAMEPAD_TYPE_SWITCH_PRO, "ZL", "switch_lefttrigger"));
    BR_CHECK(glyph_is(stick, BR_GAMEPAD_TYPE_PS5, "Left Stick", "ps_leftstick"));
    BR_CHECK(glyph_is(space, BR_GAMEPAD_TYPE_PS5, "Space", "key_space"));
    BR_CHECK(glyph_is(shift, BR_GAMEPAD_TYPE_GENERIC, "Left Shift", "key_left_shift"));
    BR_CHECK(glyph_is(lmb, BR_GAMEPAD_TYPE_GENERIC, "LMB", "mouse_left"));
    BR_CHECK(glyph_is(rmb, BR_GAMEPAD_TYPE_GENERIC, "RMB", "mouse_right"));
    br_glyph g;
    br_input_binding nothing = { BR_INPUT_BINDING_MOUSE, 9, 1 };
    BR_CHECK(!br_input_binding_glyph(nothing, BR_GAMEPAD_TYPE_GENERIC, &g));

    /* br_input_glyph follows the last used device */
    br_input_actions_clear(e);
    br_input_action_bind_key(e, "jump", SDL_SCANCODE_SPACE);
    br_input_action_bind_gamepad_button(e, "jump", SDL_GAMEPAD_BUTTON_SOUTH);
    br_input_action_bind_mouse(e, "shoot", SDL_BUTTON_LEFT);   /* no pad binding: falls back */
    br_input_inject_key(e, SDL_SCANCODE_Q, true);
    br_input_inject_key(e, SDL_SCANCODE_Q, false);
    step(e);
    BR_CHECK(br_input_glyph(e, "jump", &g) && SDL_strcmp(g.frame, "key_space") == 0);
    br_input_inject_gamepad_button(e, 4, SDL_GAMEPAD_BUTTON_EAST, true);
    step(e);
    BR_CHECK(br_input_glyph(e, "jump", &g) && SDL_strcmp(g.frame, "xbox_a") == 0 && g.device == BR_INPUT_DEVICE_GAMEPAD);
    BR_CHECK(br_input_glyph(e, "shoot", &g) && SDL_strcmp(g.frame, "mouse_left") == 0);
    BR_CHECK(!br_input_glyph(e, "unknown", &g));
    BR_CHECK(br_input_glyph_for(e, "jump", BR_INPUT_DEVICE_GAMEPAD, BR_GAMEPAD_TYPE_PS5, &g) &&
             SDL_strcmp(g.label, "Cross") == 0);

    /* prompts */
    char text[64];
    BR_CHECK(br_input_format_prompt(e, "Press {jump} to {fly}!", text, sizeof(text)));
    BR_CHECK(SDL_strcmp(text, "Press A to {fly}!") == 0);
    br_input_inject_gamepad_button(e, 4, SDL_GAMEPAD_BUTTON_EAST, false);
    br_input_inject_gamepad_disconnect(e, 4);
    br_input_inject_key(e, SDL_SCANCODE_Q, true);
    br_input_inject_key(e, SDL_SCANCODE_Q, false);
    step(e);
    BR_CHECK(br_input_format_prompt(e, "{jump}/{shoot} {", text, sizeof(text)) && SDL_strcmp(text, "Space/LMB {") == 0);
    char small[8];
    BR_CHECK(!br_input_format_prompt(e, "Press {jump} now", small, sizeof(small)) && SDL_strcmp(small, "Press S") == 0);
    BR_CHECK(br_input_format_prompt(e, NULL, small, sizeof(small)) && small[0] == '\0');
}

static void test_type_mapping(void)
{
    static const struct {
        SDL_GamepadType   sdl;
        br_gamepad_type   type;
        br_gamepad_family family;
    } table[] = {
        { SDL_GAMEPAD_TYPE_UNKNOWN, BR_GAMEPAD_TYPE_GENERIC, BR_GAMEPAD_FAMILY_GENERIC },
        { SDL_GAMEPAD_TYPE_STANDARD, BR_GAMEPAD_TYPE_GENERIC, BR_GAMEPAD_FAMILY_GENERIC },
        { SDL_GAMEPAD_TYPE_XBOX360, BR_GAMEPAD_TYPE_XBOX360, BR_GAMEPAD_FAMILY_XBOX },
        { SDL_GAMEPAD_TYPE_XBOXONE, BR_GAMEPAD_TYPE_XBOXONE, BR_GAMEPAD_FAMILY_XBOX },
        { SDL_GAMEPAD_TYPE_PS3, BR_GAMEPAD_TYPE_PS3, BR_GAMEPAD_FAMILY_PLAYSTATION },
        { SDL_GAMEPAD_TYPE_PS4, BR_GAMEPAD_TYPE_PS4, BR_GAMEPAD_FAMILY_PLAYSTATION },
        { SDL_GAMEPAD_TYPE_PS5, BR_GAMEPAD_TYPE_PS5, BR_GAMEPAD_FAMILY_PLAYSTATION },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, BR_GAMEPAD_TYPE_SWITCH_PRO, BR_GAMEPAD_FAMILY_NINTENDO },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT, BR_GAMEPAD_FAMILY_NINTENDO },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT, BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT, BR_GAMEPAD_FAMILY_NINTENDO },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR, BR_GAMEPAD_FAMILY_NINTENDO },
        { SDL_GAMEPAD_TYPE_GAMECUBE, BR_GAMEPAD_TYPE_GAMECUBE, BR_GAMEPAD_FAMILY_NINTENDO },
        { SDL_GAMEPAD_TYPE_STEAM, BR_GAMEPAD_TYPE_STEAM, BR_GAMEPAD_FAMILY_GENERIC },
    };
    BR_CHECK(BR_ARRAY_COUNT(table) == SDL_GAMEPAD_TYPE_COUNT);   /* every SDL type is covered */
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(table); i++) {
        br_gamepad_type t = br_input_gamepad_type_from_sdl(table[i].sdl);
        BR_CHECK(t == table[i].type);
        BR_CHECK(br_input_gamepad_family_of(t) == table[i].family);
        if (table[i].sdl != SDL_GAMEPAD_TYPE_UNKNOWN) {
            BR_CHECK(br_input_gamepad_type_to_sdl(t) == table[i].sdl);
        }
    }
}

/* SDL virtual joysticks: real SDL devices with chosen USB ids. Physical drivers are switched off
 * so a pad plugged into the test machine cannot change slot numbers. */
static void test_virtual_gamepads(br_engine *e)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");

    /* family detection for every type SDL can tell from a USB id */
    int detected = 0;
    for (int t = 0; t < BR_GAMEPAD_TYPE_COUNT; t++) {
        int slot = br_input_gamepad_attach_virtual(e, (br_gamepad_type)t);
        BR_CHECK(slot >= 0);
        br_gamepad_info info;
        if (slot >= 0 && br_input_gamepad_info(e, slot, &info)) {
            BR_CHECK(info.virtual_device && !info.injected);
            printf("virtual %-28s -> type %d family %d (wanted type %d)\n", info.name, (int)info.type,
                   (int)info.family, t);
            detected += info.type == (br_gamepad_type)t ? 1 : 0;
            BR_CHECK(br_input_gamepad_family_of(info.type) == br_input_gamepad_family_of((br_gamepad_type)t));
            BR_CHECK(br_input_gamepad_detach_virtual(e, slot));
            BR_CHECK(!br_input_gamepad_connected(e, slot));
        }
    }
    printf("virtual pads: %d of %d types reported exactly\n", detected, (int)BR_GAMEPAD_TYPE_COUNT);

    /* PS5: touchpad and sensors through SDL's own state */
    int ps = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_PS5);
    br_gamepad_info info;
    BR_CHECK(ps >= 0 && br_input_gamepad_info(e, ps, &info));
    BR_CHECK(info.touchpads == 1 && info.has_gyro && info.has_accel);
    SDL_Joystick *joy = SDL_GetJoystickFromID(info.id);
    BR_CHECK(joy != NULL);
    br_vec3 gyro;
    BR_CHECK(br_input_gamepad_sensor(e, ps, SDL_SENSOR_GYRO, &gyro));   /* first call enables it */
    float g[3] = { 0.5f, -1.0f, 2.0f };
    SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, 1, g, 3);
    SDL_SetJoystickVirtualTouchpad(joy, 0, 0, true, 0.25f, 0.75f, 0.5f);
    SDL_UpdateJoysticks();
    BR_CHECK(br_input_gamepad_sensor(e, ps, SDL_SENSOR_GYRO, &gyro) && near(gyro.x, 0.5f) && near(gyro.z, 2.0f));
    br_vec2 pos;
    float pressure = 0.0f;
    BR_CHECK(br_input_gamepad_touchpad(e, ps, 0, 0, &pos, &pressure) && near(pos.x, 0.25f) && near(pos.y, 0.75f));
    BR_CHECK(!br_input_gamepad_touchpad(e, ps, 0, 1, &pos, &pressure));
    BR_CHECK(!br_input_gamepad_sensor(e, ps, SDL_SENSOR_GYRO_L, &gyro));
    br_input_gamepad_set_led(e, ps, br_color_from_hex(0xff0000, 1.0f));   /* no callback: false, no crash */
    br_input_gamepad_rumble(e, ps, 0.5f, 0.5f, 10);

    /* glyph switches to the PlayStation set when that pad is used */
    br_input_actions_clear(e);
    br_input_action_bind_gamepad_button(e, "jump", SDL_GAMEPAD_BUTTON_SOUTH);
    br_input_inject_gamepad_button(e, ps, SDL_GAMEPAD_BUTTON_SOUTH, true);
    step(e);
    br_glyph glyph;
    BR_CHECK(br_input_glyph(e, "jump", &glyph) && SDL_strcmp(glyph.frame, "ps_cross") == 0);
    br_input_inject_gamepad_button(e, ps, SDL_GAMEPAD_BUTTON_SOUTH, false);
    step(e);

    /* stable slots: a pad that comes back gets its old slot even if another pad came in between */
    int xbox = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_XBOXONE);
    BR_CHECK(xbox >= 0 && xbox != ps);
    BR_CHECK(br_input_gamepad_detach_virtual(e, ps));
    int generic = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_GENERIC);
    BR_CHECK(generic >= 0 && generic != ps);
    int ps_again = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_PS5);
    BR_CHECK(ps_again == ps);
    br_input_gamepad_detach_virtual(e, ps_again);
    br_input_gamepad_detach_virtual(e, xbox);
    br_input_gamepad_detach_virtual(e, generic);
    BR_CHECK(br_input_gamepad_count(e) == 0);
}

int main(void)
{
    br_engine *e = br_engine_create("test_input", NULL);
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    test_latch_priority(e);
    test_keyboard_mouse_text(e);
    test_injected_gamepad(e);
    test_actions(e);
    test_contexts_and_rebinding(e);
    test_json_and_save(e);
    test_glyphs(e);
    test_type_mapping();
    test_virtual_gamepads(e);
    br_engine_destroy(e);
    return br_test_finish();
}
