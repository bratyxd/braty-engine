/* br_input_internal.h - state shared by br_input*.c (and the cursor hook used by br_events.c). */
#ifndef BR_INPUT_INTERNAL_H
#define BR_INPUT_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_app.h"
#include "braty/br_input.h"

#define BR_INPUT_MOUSE_BUTTONS  8
#define BR_INPUT_TEXT_SIZE      256
#define BR_INPUT_CONTEXT_DEPTH  8
#define BR_INPUT_CONTEXT_NAME   32

/* Edge flags: bit 0 = pressed since the last latch, bit 1 = released since the last latch. */
#define BR_EDGE_PRESSED  1u
#define BR_EDGE_RELEASED 2u

/* Injected pads (no SDL device) use ids far above anything SDL hands out: BASE + a counter, never
 * derived from the slot (br_input_gamepad_assign moves pads between slots). */
#define BR_INPUT_INJECTED_ID_BASE 0xB0000000u

/* Connection edges of a slot (br_gamepad_slot.conn_edges). */
#define BR_EDGE_CONNECTED    1u
#define BR_EDGE_DISCONNECTED 2u

typedef struct br_action {
    char    *name;
    char    *context;            /* NULL = always live */
    br_array bindings;           /* br_input_binding */
    float    threshold;          /* down at value >= threshold */
    bool     down;
    bool     was_down;
    bool     blocked;            /* became live while held: idle until released */
    float    value;
    float    inject_value;
    uint32_t inject_frames;      /* latches left for inject_value */
    int      last_gamepad;       /* slot that last drove it, -1 = none */
} br_action;

typedef struct br_gamepad_slot {
    SDL_JoystickID id;           /* 0 = empty slot */
    SDL_Gamepad   *pad;          /* NULL for injected pads */
    bool    injected;
    bool    virtual_device;
    br_gamepad_type injected_type; /* injected pads: the type they pretend to be */
    SDL_GUID last_guid;          /* of the last pad in this slot (reconnects return here) */
    bool    had_pad;
    bool    sensor_on[SDL_SENSOR_COUNT];
    bool    down[SDL_GAMEPAD_BUTTON_COUNT];
    uint8_t edges[SDL_GAMEPAD_BUTTON_COUNT];       /* accumulated by events */
    uint8_t frame_edges[SDL_GAMEPAD_BUTTON_COUNT]; /* latched for this frame */
    float   axes[SDL_GAMEPAD_AXIS_COUNT];          /* raw, -1..1 */
    uint8_t conn_edges;                            /* BR_EDGE_CONNECTED / _DISCONNECTED, by events */
    uint8_t conn_frame_edges;                      /* latched for this frame */
} br_gamepad_slot;

typedef struct br_input_state {
    bool    keys[SDL_SCANCODE_COUNT];
    uint8_t key_edges[SDL_SCANCODE_COUNT];
    uint8_t key_frame_edges[SDL_SCANCODE_COUNT];

    br_vec2 mouse;
    br_vec2 mouse_delta_acc;
    br_vec2 mouse_delta;
    br_vec2 wheel_acc;
    br_vec2 wheel;
    br_vec2 wheel_ticks_acc;
    br_vec2 wheel_ticks;
    bool    mouse_buttons[BR_INPUT_MOUSE_BUTTONS];
    uint8_t mouse_edges[BR_INPUT_MOUSE_BUTTONS];
    uint8_t mouse_frame_edges[BR_INPUT_MOUSE_BUTTONS];
    bool    touch_mouse;          /* accept SDL's touch/pen-synthesised mouse events */
    SDL_Cursor *custom_cursor;

    br_touch touches[BR_INPUT_MAX_TOUCHES];
    uint32_t touch_count;
    br_pen   pen;

    br_gamepad_slot gamepads[BR_INPUT_MAX_GAMEPADS];
    bool            gamepad_subsystem;
    float           deadzone_inner;        /* sticks (radial) */
    float           deadzone_outer;
    float           trigger_deadzone_inner;
    float           trigger_deadzone_outer;
    uint32_t        next_injected_id;

    br_input_device last_device;
    int             last_gamepad;          /* -1 = none, or that pad disconnected */
    br_gamepad_type last_gamepad_type;     /* of the pad used last; kept after it disconnects */

    char     text_acc[BR_INPUT_TEXT_SIZE];
    char     text[BR_INPUT_TEXT_SIZE];
    char     composition[BR_INPUT_TEXT_SIZE];
    int      composition_cursor;
    int      composition_length;

    br_array actions;            /* br_action */
    char     contexts[BR_INPUT_CONTEXT_DEPTH][BR_INPUT_CONTEXT_NAME];
    uint32_t context_count;

    br_input_device capture_device;  /* NONE = not capturing */
    bool             capture_done;
    br_input_binding capture_result;

} br_input_state;

/* ---- br_input.c */

/* Sets a button's state, recording edges. */
void br_input_set_button(bool *down, uint8_t *edges, bool now_down);
/* Stamps and sends an event through br_app_event. */
void br_input_send(br_engine *e, SDL_Event *ev);
/* The custom cursor (br_input_mouse_set_cursor_image) or NULL: br_events shows it instead of the default. */
SDL_Cursor *br_input_custom_cursor(br_engine *e);

/* ---- br_input_gamepad.c */

/* Handles gamepad events. */
void br_input_gamepad_event(br_engine *e, const SDL_Event *ev);
/* Latches pad edges for this frame. */
void br_input_gamepad_latch(br_input_state *s);
/* Closes every pad and the subsystem. */
void br_input_gamepad_shutdown(br_input_state *s);
/* The slot if it holds a pad, else NULL. */
br_gamepad_slot *br_input_pad_at(br_engine *e, int slot);
/* Axis of a slot after the dead zone. */
float br_input_pad_axis(br_input_state *s, const br_gamepad_slot *pad, int axis);
/* Initialises SDL's gamepad subsystem once. */
bool br_input_gamepad_init_subsystem(br_input_state *s);

/* ---- br_input_action.c */

/* Recomputes every action from the latched state (begin of frame). */
void br_input_update_actions(br_engine *e);
/* Frees every action. */
void br_input_actions_free(br_input_state *s);
/* Removes every action (keeps the array). */
void br_input_actions_clear_state(br_input_state *s);
/* Finds an action by name. */
br_action *br_input_find_action(br_input_state *s, const char *name);
/* Checks the latched state for a capture result. */
void br_input_capture_update(br_input_state *s);

#endif
