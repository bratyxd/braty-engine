/* br_input.h - keyboard, mouse, touch, pen, gamepads, text input, named actions, glyphs.
 *
 * Input is plain state that systems poll every frame:
 *
 *   if (br_input_key_pressed(e, SDL_SCANCODE_SPACE)) { jump(); }
 *   float x = br_input_action_value(e, "right") - br_input_action_value(e, "left");
 *
 * The state is fed from SDL events (br_engine_add_event_handler). "down" is live; "pressed" and
 * "released" are edges latched at the start of each frame (system "input.begin_frame", INPUT
 * phase, priority BR_INPUT_PRIORITY_LATCH), so a key tapped between two frames still reads as
 * pressed once. Input injected before the latch counts for the same frame.
 *
 * Actions map a name to keys, mouse buttons, gamepad buttons and gamepad axes, so games bind
 * input as data (br_input_actions_from_json). Gamepad bindings read every connected pad unless
 * asked per player (br_input_action_value_for). Actions can belong to an input context
 * ("gameplay", "menu"): only actions without a context or in the top context of the stack are
 * live. Bindings can be changed at runtime (capture + conflict check) and saved as JSON.
 *
 * Gamepads: SDL maps Xbox, PlayStation, Switch and generic pads to one positional layout
 * (SDL_GAMEPAD_BUTTON_SOUTH = bottom face button). br_input adds slots (players 0..7, stable
 * across reconnects), radial dead zones, the controller family for button prompts
 * (br_input_glyph), sensors, touchpads, LEDs and rumble.
 *
 * br_input_inject_* build an SDL_Event and send it through br_app_event, the exact path real
 * events take, so input state and pointer events (br_events.h) both see it. Tests, demos and
 * play-testing use them; coordinates are window coordinates (logical units = stage units).
 */
#ifndef BR_INPUT_H
#define BR_INPUT_H

#include "braty/br_render.h"
#include "cjson/cJSON.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_sensor.h>

#define BR_INPUT_MAX_TOUCHES  16
#define BR_INPUT_MAX_GAMEPADS 8
/* Priority of the "input.begin_frame" system (INPUT phase). Systems that inject input for the
 * current frame run before it (a lower number, e.g. BR_INPUT_PRIORITY_LATCH - 1000). */
#define BR_INPUT_PRIORITY_LATCH (-2000000)

/* Registers the module (state, event handler, begin-frame system). Called once by br_builtin.c. */
void br_input_register(br_engine *e);

/* ---- devices */

typedef enum br_input_device {
    BR_INPUT_DEVICE_NONE,
    BR_INPUT_DEVICE_KEYBOARD_MOUSE,
    BR_INPUT_DEVICE_GAMEPAD,
    BR_INPUT_DEVICE_TOUCH,
    BR_INPUT_DEVICE_PEN
} br_input_device;

/* The device that produced the last key/button press, wheel, mouse move, stick push or touch. */
br_input_device br_input_last_device(br_engine *e);
/* Slot of the gamepad used last (-1 if none yet). */
int br_input_last_gamepad(br_engine *e);

/* ---- keyboard (SDL_Scancode = physical key position) */

/* True while the key is held. */
bool br_input_key_down(br_engine *e, SDL_Scancode key);
/* True in the frame the key went down. */
bool br_input_key_pressed(br_engine *e, SDL_Scancode key);
/* True in the frame the key went up. */
bool br_input_key_released(br_engine *e, SDL_Scancode key);
/* Keycode (layout) versions: SDLK_Z is wherever the user's layout has Z (QWERTZ: the Y key). */
bool br_input_keycode_down(br_engine *e, SDL_Keycode key);
/* True in the frame the layout key went down. */
bool br_input_keycode_pressed(br_engine *e, SDL_Keycode key);
/* True in the frame the layout key went up. */
bool br_input_keycode_released(br_engine *e, SDL_Keycode key);
/* Held modifiers (shift / ctrl / alt / gui from the key state; caps and num lock from SDL). */
SDL_Keymod br_input_key_mods(br_engine *e);
/* Name of the key in the user's layout, for prompts ("Space", "Z", "Left Shift"). False if it has none. */
bool br_input_key_name(SDL_Scancode key, char *out, size_t out_size);

/* ---- mouse (buttons: SDL_BUTTON_LEFT=1, MIDDLE=2, RIGHT=3, X1=4, X2=5)
 * All mice are merged into one (SDL's default). Touch/pen-synthesised mouse events count too
 * unless br_input_touch_set_mouse_emulation(e, false). */

/* Last known mouse position in window coordinates. */
br_vec2 br_input_mouse_position(br_engine *e);
/* Mouse movement during the last frame (also in relative mode). */
br_vec2 br_input_mouse_delta(br_engine *e);
/* Smooth wheel movement during the last frame (y > 0 = away from the user; fractions on touchpads). */
br_vec2 br_input_mouse_wheel(br_engine *e);
/* Whole wheel notches during the last frame (discrete: menus, weapon switching). */
br_vec2 br_input_mouse_wheel_ticks(br_engine *e);
/* True while the button is held. */
bool br_input_mouse_down(br_engine *e, int button);
/* True in the frame the button went down. */
bool br_input_mouse_pressed(br_engine *e, int button);
/* True in the frame the button went up. */
bool br_input_mouse_released(br_engine *e, int button);
/* Relative mode: cursor hidden and locked, unbounded deltas (mouse-look). Needs a window. */
bool br_input_mouse_set_relative(br_engine *e, bool relative);
/* True while in relative mode. */
bool br_input_mouse_relative(br_engine *e);
/* Confines the cursor to the window (it stays visible). Needs a window. */
bool br_input_mouse_set_confined(br_engine *e, bool confined);
/* Shows or hides the cursor. */
void br_input_mouse_show_cursor(br_engine *e, bool visible);
/* Custom cursor from RGBA8 pixels (straight alpha), hot spot in pixels. NULL pixels = default
 * arrow. Node cursors (br_node_set_cursor) still win while hovering such a node. */
bool br_input_mouse_set_cursor_image(br_engine *e, const uint8_t *rgba, int width, int height, int hot_x, int hot_y);

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
/* Whether SDL's mouse events synthesised from touch and pen feed the mouse state (default true). */
void br_input_touch_set_mouse_emulation(br_engine *e, bool enabled);

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

/* ---- gamepads (slots 0..BR_INPUT_MAX_GAMEPADS-1 = players, hot-plugged; real pads need a window)
 *
 * A new pad takes the slot it had before if that is free (same SDL GUID), else the first free
 * slot; br_input_gamepad_assign moves it explicitly. The slot number is also sent to the pad as
 * its player index (player LEDs). Axis values pass a radial dead zone (sticks) or a linear one
 * (triggers): br_input_gamepad_set_deadzone. */

typedef enum br_gamepad_type {
    BR_GAMEPAD_TYPE_GENERIC,
    BR_GAMEPAD_TYPE_XBOX360,
    BR_GAMEPAD_TYPE_XBOXONE,
    BR_GAMEPAD_TYPE_PS3,
    BR_GAMEPAD_TYPE_PS4,
    BR_GAMEPAD_TYPE_PS5,
    BR_GAMEPAD_TYPE_SWITCH_PRO,
    BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT,
    BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT,
    BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR,
    BR_GAMEPAD_TYPE_GAMECUBE,
    BR_GAMEPAD_TYPE_STEAM,
    BR_GAMEPAD_TYPE_COUNT
} br_gamepad_type;

/* Families share button labels and glyph art. */
typedef enum br_gamepad_family {
    BR_GAMEPAD_FAMILY_GENERIC,     /* SDL standard layout; prompts use the Xbox set */
    BR_GAMEPAD_FAMILY_XBOX,
    BR_GAMEPAD_FAMILY_PLAYSTATION,
    BR_GAMEPAD_FAMILY_NINTENDO
} br_gamepad_family;

typedef enum br_gamepad_stick {
    BR_GAMEPAD_STICK_LEFT,
    BR_GAMEPAD_STICK_RIGHT
} br_gamepad_stick;

typedef struct br_gamepad_info {
    char              name[64];
    br_gamepad_type   type;
    br_gamepad_family family;
    uint16_t          vendor;
    uint16_t          product;
    SDL_JoystickID    id;
    bool              injected;           /* created by br_input_inject_gamepad_* (no SDL device) */
    bool              virtual_device;     /* SDL virtual joystick (br_input_gamepad_attach_virtual) */
    bool              wireless;
    SDL_PowerState    power;              /* SDL_POWERSTATE_UNKNOWN if not reported */
    int               battery_percent;    /* -1 if unknown */
    bool              has_rumble;
    bool              has_trigger_rumble;
    bool              has_rgb_led;
    bool              has_player_led;
    bool              has_gyro;
    bool              has_accel;
    int               touchpads;
} br_gamepad_info;

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
/* Axis value after the dead zone: sticks -1..1, triggers 0..1. */
float br_input_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis);
/* Axis value as reported, without dead zone. */
float br_input_gamepad_axis_raw(br_engine *e, int slot, SDL_GamepadAxis axis);
/* Stick as a vector after the radial dead zone (length <= 1; y > 0 = down, like SDL). */
br_vec2 br_input_gamepad_stick(br_engine *e, int slot, br_gamepad_stick stick);
/* Dead zone for every pad: below `inner` reads 0, above `outer` reads 1, rescaled in between.
 * Defaults 0.15 and 0.95. Sticks use the stick length (radial), triggers their own value. */
void br_input_gamepad_set_deadzone(br_engine *e, float inner, float outer);
/* Rumble: motor strengths 0..1 for duration_ms. False if no pad or no rumble support. */
bool br_input_gamepad_rumble(br_engine *e, int slot, float low, float high, uint32_t duration_ms);
/* Trigger rumble (Xbox One impulse triggers): 0..1 each. False if unsupported. */
bool br_input_gamepad_rumble_triggers(br_engine *e, int slot, float left, float right, uint32_t duration_ms);
/* Light bar colour (PlayStation, some others). False if unsupported. */
bool br_input_gamepad_set_led(br_engine *e, int slot, br_color color);
/* Raw device effect packet (e.g. DualSense adaptive triggers), passed to SDL_SendGamepadEffect.
 * Untested on hardware: the packet format is the device's own. */
bool br_input_gamepad_send_effect(br_engine *e, int slot, const void *data, int size);
/* Family-level type of the pad in this slot (GENERIC if empty). */
br_gamepad_type br_input_gamepad_type(br_engine *e, int slot);
/* Everything known about the pad in this slot. False if the slot is empty. */
bool br_input_gamepad_info(br_engine *e, int slot, br_gamepad_info *out);
/* Moves the pad in `from_slot` to `to_slot`, swapping with whatever is there. */
bool br_input_gamepad_assign(br_engine *e, int from_slot, int to_slot);
/* Sensor reading (SDL_SENSOR_GYRO in rad/s, SDL_SENSOR_ACCEL in m/s^2). Enables the sensor on
 * the first call, so the first reading may be zero. False if the pad has no such sensor. */
bool br_input_gamepad_sensor(br_engine *e, int slot, SDL_SensorType type, br_vec3 *out);
/* Touchpad finger (PlayStation): position 0..1 within the pad, pressure 0..1. False if not down. */
bool br_input_gamepad_touchpad(br_engine *e, int slot, int touchpad, int finger, br_vec2 *position, float *pressure);

/* Family of a type. */
br_gamepad_family br_input_gamepad_family_of(br_gamepad_type type);
/* Maps SDL's gamepad type to ours (UNKNOWN / STANDARD -> GENERIC). */
br_gamepad_type br_input_gamepad_type_from_sdl(SDL_GamepadType type);
/* Maps our type back to SDL's. */
SDL_GamepadType br_input_gamepad_type_to_sdl(br_gamepad_type type);

/* ---- text input */

/* Starts sending text input (on-screen keyboard on mobile). */
void br_input_text_start(br_engine *e);
/* Stops text input. */
void br_input_text_stop(br_engine *e);
/* UTF-8 text typed during the last frame ("" if none). */
const char *br_input_text(br_engine *e);
/* IME composition (pre-edit) text being typed, "" if none. cursor / selection_length may be NULL. */
const char *br_input_text_composition(br_engine *e, int *cursor, int *selection_length);
/* Tells the IME where the text field is (window coordinates) so its candidate list appears next to it. */
void br_input_text_set_area(br_engine *e, br_rect area, int cursor);

/* ---- actions (named, data-driven bindings) */

typedef enum br_input_binding_kind {
    BR_INPUT_BINDING_KEY,             /* code = SDL_Scancode */
    BR_INPUT_BINDING_MOUSE,           /* code = mouse button */
    BR_INPUT_BINDING_GAMEPAD_BUTTON,  /* code = SDL_GamepadButton */
    BR_INPUT_BINDING_GAMEPAD_AXIS     /* code = SDL_GamepadAxis, direction +1 or -1 */
} br_input_binding_kind;

typedef struct br_input_binding {
    br_input_binding_kind kind;
    int                   code;
    int                   direction;   /* axes only: +1 or -1 */
} br_input_binding;

/* Binds a key to an action (the action is created on first use; name copied). */
void br_input_action_bind_key(br_engine *e, const char *action, SDL_Scancode key);
/* Binds a mouse button. */
void br_input_action_bind_mouse(br_engine *e, const char *action, int button);
/* Binds a gamepad button (any pad). */
void br_input_action_bind_gamepad_button(br_engine *e, const char *action, SDL_GamepadButton button);
/* Binds one direction of a gamepad axis (direction +1 or -1; any pad). Down past the threshold. */
void br_input_action_bind_gamepad_axis(br_engine *e, const char *action, SDL_GamepadAxis axis, int direction);
/* Adds any binding (validated, logged if bad). */
void br_input_action_bind(br_engine *e, const char *action, br_input_binding binding);
/* Number of bindings of an action (0 if unknown). */
uint32_t br_input_action_binding_count(br_engine *e, const char *action);
/* The i-th binding. False if out of range. */
bool br_input_action_binding_get(br_engine *e, const char *action, uint32_t i, br_input_binding *out);
/* Replaces the i-th binding. False if out of range or invalid. */
bool br_input_action_binding_set(br_engine *e, const char *action, uint32_t i, br_input_binding binding);
/* Removes the i-th binding. */
bool br_input_action_binding_remove(br_engine *e, const char *action, uint32_t i);
/* Name of another action (not `except_action`, may be NULL) in the same context that already
 * uses this binding, or NULL. Actions in different contexts may share inputs. */
const char *br_input_binding_conflict(br_engine *e, br_input_binding binding, const char *except_action);
/* Value at or above which the action counts as down (default 0.5; matters for axes and triggers). */
void br_input_action_set_threshold(br_engine *e, const char *action, float threshold);
/* Puts the action in an input context ("gameplay", "menu"); NULL or "" = always live. */
void br_input_action_set_context(br_engine *e, const char *action, const char *context);
/* Removes all actions and bindings. */
void br_input_actions_clear(br_engine *e);
/* Adds actions from a JSON object:
 *   { "jump": { "keys": ["Space"], "mouse": [1], "buttons": ["a"], "axes": ["-lefty"],
 *               "threshold": 0.5, "context": "gameplay" } }
 * keys = SDL scancode names ("Space", "A", "Left"), buttons = SDL gamepad button names ("a" =
 * south face button, "start", "dpup"), axes = SDL axis names prefixed + or - ("-leftx").
 * threshold and context are optional. Unknown names are logged and skipped. False if json is
 * not an object. */
bool br_input_actions_from_json(br_engine *e, const cJSON *json);
/* All actions in the br_input_actions_from_json format (free with cJSON_Delete). */
cJSON *br_input_actions_to_json(br_engine *e);
/* Writes all bindings as JSON to save data (br_save_write), e.g. "bindings.json". */
bool br_input_bindings_save(br_engine *e, const char *save_path);
/* Reads bindings saved by br_input_bindings_save: every action in the file gets exactly the
 * file's bindings; other actions keep theirs. False if missing or not JSON. */
bool br_input_bindings_load(br_engine *e, const char *save_path);
/* True while any binding is held (or was pressed this frame). */
bool br_input_action_down(br_engine *e, const char *action);
/* True in the frame the action went down. */
bool br_input_action_pressed(br_engine *e, const char *action);
/* True in the frame the action went up. */
bool br_input_action_released(br_engine *e, const char *action);
/* Strength 0..1: 1 for keys/buttons, the axis value for axes (largest binding wins). */
float br_input_action_value(br_engine *e, const char *action);
/* Value from one player's device only: player -1 = keyboard and mouse, 0..7 = that gamepad slot. */
float br_input_action_value_for(br_engine *e, const char *action, int player);
/* br_input_action_value_for(...) >= threshold. */
bool br_input_action_down_for(br_engine *e, const char *action, int player);
/* Slot of the gamepad that last drove the action (e.g. to rumble the right pad), -1 if none. */
int br_input_action_last_gamepad(br_engine *e, const char *action);
/* 1D composite: value(positive) - value(negative), -1..1. */
float br_input_action_axis(br_engine *e, const char *negative, const char *positive);
/* 2D composite (WASD, d-pad, stick directions): x = right - left, y = down - up (screen y-down),
 * length clamped to 1. */
br_vec2 br_input_action_vec2(br_engine *e, const char *left, const char *right, const char *up, const char *down);

/* ---- input contexts: a small stack (max 8). Actions with a context are live only while it is
 * on top; actions without one are always live. Changes take effect at the next latch; an action
 * that becomes live while its input is held stays up until that input is released. */

/* Pushes a context (name copied, max 31 chars). */
void br_input_context_push(br_engine *e, const char *context);
/* Pops the top context. */
void br_input_context_pop(br_engine *e);
/* The top context, "" if the stack is empty. */
const char *br_input_context_top(br_engine *e);

/* ---- rebinding: capture the next input a player presses */

/* Starts listening for the next press on `device` (KEYBOARD_MOUSE: keys and mouse buttons;
 * GAMEPAD: buttons and axes past 0.5). All actions read as idle while capturing. */
void br_input_capture_start(br_engine *e, br_input_device device);
/* True once, in the frame something was pressed; writes it to *out and ends the capture. */
bool br_input_capture_poll(br_engine *e, br_input_binding *out);
/* Stops capturing without a result. */
void br_input_capture_cancel(br_engine *e);

/* ---- glyphs: what to show in a button prompt
 *
 * Sprite frame naming scheme (ship a glyph atlas with these frame names):
 *   keyboard   "key_" + SDL scancode name, lower case, spaces as '_': key_space, key_left_shift, key_a
 *   mouse      mouse_left, mouse_middle, mouse_right, mouse_x1, mouse_x2
 *   gamepad    <set>_<part> with set = xbox | ps | switch (generic and Steam pads use xbox,
 *              GameCube uses switch). Face buttons use the printed label: xbox_a, ps_cross,
 *              ps_circle, switch_b. Other buttons use SDL's button name: xbox_start, ps_back,
 *              switch_leftshoulder, xbox_dpup, ps_touchpad. Axes: <set>_leftstick,
 *              <set>_rightstick, <set>_lefttrigger, <set>_righttrigger.
 * Labels are what is printed on the device: "A", "Cross", "B" (Switch south), "LB", "L1", "ZL",
 * "Space", "LMB". */

typedef struct br_glyph {
    br_input_device   device;      /* KEYBOARD_MOUSE or GAMEPAD */
    br_gamepad_family family;      /* for GAMEPAD */
    br_input_binding  binding;     /* the binding shown */
    char              label[32];
    char              frame[48];
} br_glyph;

/* Glyph of the action's first binding for the last used device (keyboard/mouse if none yet,
 * the last pad's family for gamepads). Falls back to any binding. False if the action has none. */
bool br_input_glyph(br_engine *e, const char *action, br_glyph *out);
/* Same for a chosen device and pad type (type ignored for KEYBOARD_MOUSE). */
bool br_input_glyph_for(br_engine *e, const char *action, br_input_device device, br_gamepad_type type, br_glyph *out);
/* Glyph of one binding for a pad type. */
bool br_input_binding_glyph(br_input_binding binding, br_gamepad_type type, br_glyph *out);
/* Copies text replacing "{action}" with that action's glyph label ("Press {jump}" -> "Press A").
 * Unknown actions are left as written. Always terminated; false if truncated. */
bool br_input_format_prompt(br_engine *e, const char *text, char *out, size_t out_size);

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
/* Types UTF-8 text (as SDL_EVENT_TEXT_INPUT). */
void br_input_inject_text(br_engine *e, const char *utf8);
/* Gamepad button in a slot. An empty slot gets an injected generic pad first (no SDL device). */
void br_input_inject_gamepad_button(br_engine *e, int slot, SDL_GamepadButton button, bool down);
/* Gamepad axis in a slot (-1..1 sticks, 0..1 triggers). An empty slot gets an injected pad. */
void br_input_inject_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis, float value);
/* Holds an action at `value` for `frames` latches (1 = pressed this frame, released the next),
 * whatever it is bound to. Respects contexts like real input. */
void br_input_inject_action(br_engine *e, const char *action, float value, uint32_t frames);
/* Removes an injected pad (br_input_inject_gamepad_*) from its slot. */
void br_input_inject_gamepad_disconnect(br_engine *e, int slot);

/* ---- virtual gamepads (SDL virtual joysticks: real SDL devices with a chosen USB id)
 * Used to test family detection, sensors and touchpads without hardware. Works headless
 * (initialises SDL's gamepad subsystem). Drive their buttons with br_input_inject_gamepad_*. */

/* Attaches a virtual pad that reports this type's vendor/product ids. Returns its slot, -1 on failure. */
int br_input_gamepad_attach_virtual(br_engine *e, br_gamepad_type type);
/* Detaches a virtual pad. */
bool br_input_gamepad_detach_virtual(br_engine *e, int slot);

#endif
