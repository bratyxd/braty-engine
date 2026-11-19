/* br_input.h - keyboard, mouse, touch, pen, gamepads, text input, named actions.
 *
 * Input is plain state that systems poll every frame:
 *
 *   if (br_input_key_pressed(e, SDL_SCANCODE_SPACE)) { jump(); }
 *   float x = br_input_action_value(e, "right") - br_input_action_value(e, "left");
 *
 * The state is fed from SDL events (br_engine_add_event_handler). "down" is live; "pressed" and
 * "released" are edges latched at the start of each frame (system "input.begin_frame", INPUT
 * phase, priority -2000000), so a key tapped between two frames still reads as pressed once.
 *
 * Actions map a name to keys, mouse buttons, gamepad buttons and gamepad axes, so games bind
 * input as data (br_input_actions_from_json). Gamepad bindings read every connected pad.
 *
 * br_input_inject_* build an SDL_Event and send it through br_app_event, the exact path real
 * events take, so input state and pointer events (br_events.h) both see it. Tests and demos use
 * them; coordinates are window coordinates (logical units = stage units).
 */
#ifndef BR_INPUT_H
#define BR_INPUT_H

#include "braty/br_render.h"
#include "cjson/cJSON.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#define BR_INPUT_MAX_TOUCHES  16
#define BR_INPUT_MAX_GAMEPADS 8

/* Registers the module (state, event handler, begin-frame system). Called once by br_builtin.c. */
void br_input_register(br_engine *e);

/* ---- keyboard (SDL_Scancode = physical key position) */

/* True while the key is held. */
bool br_input_key_down(br_engine *e, SDL_Scancode key);
/* True in the frame the key went down. */
bool br_input_key_pressed(br_engine *e, SDL_Scancode key);
/* True in the frame the key went up. */
bool br_input_key_released(br_engine *e, SDL_Scancode key);

/* ---- mouse (buttons: SDL_BUTTON_LEFT=1, MIDDLE=2, RIGHT=3, X1=4, X2=5) */

/* Last known mouse position in window coordinates. */
br_vec2 br_input_mouse_position(br_engine *e);
/* Mouse movement during the last frame. */
br_vec2 br_input_mouse_delta(br_engine *e);
/* Wheel movement during the last frame (y > 0 = away from the user). */
br_vec2 br_input_mouse_wheel(br_engine *e);
/* True while the button is held. */
bool br_input_mouse_down(br_engine *e, int button);
/* True in the frame the button went down. */
bool br_input_mouse_pressed(br_engine *e, int button);
/* True in the frame the button went up. */
bool br_input_mouse_released(br_engine *e, int button);

/* ---- touch */

typedef struct br_touch {
    uint64_t id;          /* SDL_FingerID */
    br_vec2  position;    /* window coordinates */
    float    pressure;
} br_touch;

/* Number of fingers currently down. */
uint32_t br_input_touch_count(br_engine *e);
/* The i-th finger currently down (false if out of range). */
bool br_input_touch_get(br_engine *e, uint32_t i, br_touch *out);

/* ---- pen */

typedef struct br_pen {
    bool    in_proximity;
    bool    down;         /* tip touching */
    bool    eraser;
    br_vec2 position;     /* window coordinates */
    float   pressure;     /* 0..1 */
} br_pen;

/* State of the last used pen. */
br_pen br_input_pen(br_engine *e);

/* ---- gamepads (slots 0..BR_INPUT_MAX_GAMEPADS-1, hot-plugged; needs a window) */

/* True if a gamepad is connected in this slot. */
bool br_input_gamepad_connected(br_engine *e, int slot);
/* Number of connected gamepads. */
int br_input_gamepad_count(br_engine *e);
/* True while the button is held. */
bool br_input_gamepad_down(br_engine *e, int slot, SDL_GamepadButton button);
/* True in the frame the button went down. */
bool br_input_gamepad_pressed(br_engine *e, int slot, SDL_GamepadButton button);
/* True in the frame the button went up. */
bool br_input_gamepad_released(br_engine *e, int slot, SDL_GamepadButton button);
/* Axis value: sticks -1..1, triggers 0..1. */
float br_input_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis);
/* Rumble: motor strengths 0..1 for duration_ms. False if no pad or no rumble support. */
bool br_input_gamepad_rumble(br_engine *e, int slot, float low, float high, uint32_t duration_ms);

/* ---- text input */

/* Starts sending text input (on-screen keyboard on mobile). */
void br_input_text_start(br_engine *e);
/* Stops text input. */
void br_input_text_stop(br_engine *e);
/* UTF-8 text typed during the last frame ("" if none). */
const char *br_input_text(br_engine *e);

/* ---- actions (named, data-driven bindings) */

/* Binds a key to an action (the action is created on first use; name copied). */
void br_input_action_bind_key(br_engine *e, const char *action, SDL_Scancode key);
/* Binds a mouse button. */
void br_input_action_bind_mouse(br_engine *e, const char *action, int button);
/* Binds a gamepad button (any pad). */
void br_input_action_bind_gamepad_button(br_engine *e, const char *action, SDL_GamepadButton button);
/* Binds one direction of a gamepad axis (direction +1 or -1; any pad). Counts as down past 0.5. */
void br_input_action_bind_gamepad_axis(br_engine *e, const char *action, SDL_GamepadAxis axis, int direction);
/* Removes all actions and bindings. */
void br_input_actions_clear(br_engine *e);
/* Adds actions from a JSON object:
 *   { "jump": { "keys": ["Space"], "mouse": [1], "buttons": ["a"], "axes": ["-lefty"] } }
 * keys = SDL scancode names ("Space", "A", "Left"), buttons = SDL gamepad button names ("a" =
 * south face button, "start", "dpup"), axes = SDL axis names prefixed + or - ("-leftx").
 * Unknown names are logged and skipped. False if json is not an object. */
bool br_input_actions_from_json(br_engine *e, const cJSON *json);
/* True while any binding is held (or was pressed this frame). */
bool br_input_action_down(br_engine *e, const char *action);
/* True in the frame the action went down. */
bool br_input_action_pressed(br_engine *e, const char *action);
/* True in the frame the action went up. */
bool br_input_action_released(br_engine *e, const char *action);
/* Strength 0..1: 1 for keys/buttons, the axis value for axes (largest binding wins). */
float br_input_action_value(br_engine *e, const char *action);

/* ---- injection (same path as real SDL events: SDL_Event -> br_app_event) */

/* Mouse moves to (x, y) in window coordinates. */
void br_input_inject_pointer_move(br_engine *e, float x, float y);
/* Mouse button goes down at (x, y). */
void br_input_inject_pointer_down(br_engine *e, float x, float y, int button);
/* Mouse button goes up at (x, y). */
void br_input_inject_pointer_up(br_engine *e, float x, float y, int button);
/* Mouse wheel scrolls by (dx, dy) at the current mouse position. */
void br_input_inject_wheel(br_engine *e, float dx, float dy);
/* Key goes down (true) or up (false). */
void br_input_inject_key(br_engine *e, SDL_Scancode key, bool down);
/* Finger goes down / moves / goes up (type = SDL_EVENT_FINGER_DOWN/MOTION/UP), window coordinates. */
void br_input_inject_touch(br_engine *e, SDL_EventType type, uint64_t finger, float x, float y);

#endif
