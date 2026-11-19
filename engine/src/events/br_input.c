/* br_input: polled input state fed from SDL events, named actions, event injection. */
#include "app/br_engine_internal.h"
#include "braty/br_app.h"
#include "braty/br_input.h"

#define BR_INPUT_MOUSE_BUTTONS 8
#define BR_INPUT_TEXT_SIZE     256

/* Edge flags: bit 0 = pressed since the last latch, bit 1 = released since the last latch. */
#define BR_EDGE_PRESSED  1u
#define BR_EDGE_RELEASED 2u

typedef enum br_binding_kind {
    BR_BINDING_KEY,
    BR_BINDING_MOUSE,
    BR_BINDING_GAMEPAD_BUTTON,
    BR_BINDING_GAMEPAD_AXIS
} br_binding_kind;

typedef struct br_binding {
    br_binding_kind kind;
    int             code;        /* scancode, mouse button, gamepad button or axis */
    int             direction;   /* axes: +1 or -1 */
} br_binding;

typedef struct br_action {
    char    *name;
    br_array bindings;           /* br_binding */
    bool     down;
    bool     was_down;
    float    value;
} br_action;

typedef struct br_gamepad_slot {
    SDL_JoystickID id;           /* 0 = empty slot */
    SDL_Gamepad   *pad;          /* NULL for injected pads */
    bool    down[SDL_GAMEPAD_BUTTON_COUNT];
    uint8_t edges[SDL_GAMEPAD_BUTTON_COUNT];       /* accumulated by events */
    uint8_t frame_edges[SDL_GAMEPAD_BUTTON_COUNT]; /* latched for this frame */
    float   axes[SDL_GAMEPAD_AXIS_COUNT];
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
    bool    mouse_buttons[BR_INPUT_MOUSE_BUTTONS];
    uint8_t mouse_edges[BR_INPUT_MOUSE_BUTTONS];
    uint8_t mouse_frame_edges[BR_INPUT_MOUSE_BUTTONS];

    br_touch touches[BR_INPUT_MAX_TOUCHES];
    uint32_t touch_count;
    br_pen   pen;

    br_gamepad_slot gamepads[BR_INPUT_MAX_GAMEPADS];
    bool            gamepad_subsystem;

    char     text_acc[BR_INPUT_TEXT_SIZE];
    char     text[BR_INPUT_TEXT_SIZE];

    br_array actions;            /* br_action */
} br_input_state;

/* ---- helpers */

static void br_input_set_button(bool *down, uint8_t *edges, bool now_down)
{
    if (now_down && !*down) {
        *edges |= BR_EDGE_PRESSED;
    } else if (!now_down && *down) {
        *edges |= BR_EDGE_RELEASED;
    }
    *down = now_down;
}

static br_gamepad_slot *br_input_find_pad(br_input_state *s, SDL_JoystickID id)
{
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].id == id && id != 0) {
            return &s->gamepads[i];
        }
    }
    return NULL;
}

static br_gamepad_slot *br_input_pad_at(br_engine *e, int slot)
{
    if (slot < 0 || slot >= BR_INPUT_MAX_GAMEPADS || e->input->gamepads[slot].id == 0) {
        return NULL;
    }
    return &e->input->gamepads[slot];
}

static br_vec2 br_input_touch_to_window(br_engine *e, float nx, float ny)
{
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    // ponytail: headless engines have no window size, so touch coordinates pass through unscaled.
    if (w <= 0 || h <= 0) {
        return br_vec2_make(nx, ny);
    }
    return br_vec2_make(nx * (float)w, ny * (float)h);
}

static void br_input_touch_event(br_engine *e, const SDL_TouchFingerEvent *f, SDL_EventType type)
{
    br_input_state *s = e->input;
    uint32_t found = s->touch_count;
    for (uint32_t i = 0; i < s->touch_count; i++) {
        if (s->touches[i].id == (uint64_t)f->fingerID) {
            found = i;
        }
    }
    if (type == SDL_EVENT_FINGER_UP || type == SDL_EVENT_FINGER_CANCELED) {
        if (found < s->touch_count) {
            s->touches[found] = s->touches[s->touch_count - 1];
            s->touch_count--;
        }
        return;
    }
    if (found == s->touch_count) {
        if (s->touch_count == BR_INPUT_MAX_TOUCHES) {
            return;
        }
        s->touch_count++;
    }
    s->touches[found].id = (uint64_t)f->fingerID;
    s->touches[found].position = br_input_touch_to_window(e, f->x, f->y);
    s->touches[found].pressure = f->pressure;
}

static void br_input_gamepad_added(br_input_state *s, SDL_JoystickID id)
{
    if (br_input_find_pad(s, id) != NULL) {
        return;
    }
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].id == 0) {
            SDL_zero(s->gamepads[i]);
            s->gamepads[i].id = id;
            s->gamepads[i].pad = SDL_OpenGamepad(id);
            br_log_info("input: gamepad %d connected (%s)", i,
                        s->gamepads[i].pad != NULL ? SDL_GetGamepadName(s->gamepads[i].pad) : "unknown");
            return;
        }
    }
    br_log_warn("input: more than %d gamepads, ignoring one", BR_INPUT_MAX_GAMEPADS);
}

static void br_input_gamepad_removed(br_input_state *s, SDL_JoystickID id)
{
    br_gamepad_slot *pad = br_input_find_pad(s, id);
    if (pad == NULL) {
        return;
    }
    if (pad->pad != NULL) {
        SDL_CloseGamepad(pad->pad);
    }
    SDL_zerop(pad);
    br_log_info("input: gamepad disconnected");
}

static void br_input_event(br_engine *e, const SDL_Event *ev, void *user)
{
    (void)user;
    br_input_state *s = e->input;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (ev->key.scancode < SDL_SCANCODE_COUNT && !ev->key.repeat) {
            br_input_set_button(&s->keys[ev->key.scancode], &s->key_edges[ev->key.scancode], ev->key.down);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        s->mouse = br_vec2_make(ev->motion.x, ev->motion.y);
        s->mouse_delta_acc = br_vec2_add(s->mouse_delta_acc, br_vec2_make(ev->motion.xrel, ev->motion.yrel));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        s->mouse = br_vec2_make(ev->button.x, ev->button.y);
        if (ev->button.button < BR_INPUT_MOUSE_BUTTONS) {
            br_input_set_button(&s->mouse_buttons[ev->button.button], &s->mouse_edges[ev->button.button],
                                ev->button.down);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float dir = ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        s->wheel_acc = br_vec2_add(s->wheel_acc, br_vec2_make(ev->wheel.x * dir, ev->wheel.y * dir));
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        br_input_touch_event(e, &ev->tfinger, (SDL_EventType)ev->type);
        break;
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        s->pen.in_proximity = ev->type == SDL_EVENT_PEN_PROXIMITY_IN;
        break;
    case SDL_EVENT_PEN_MOTION:
        s->pen.position = br_vec2_make(ev->pmotion.x, ev->pmotion.y);
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        s->pen.position = br_vec2_make(ev->ptouch.x, ev->ptouch.y);
        s->pen.down = ev->ptouch.down;
        s->pen.eraser = ev->ptouch.eraser;
        break;
    case SDL_EVENT_PEN_AXIS:
        if (ev->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
            s->pen.pressure = ev->paxis.value;
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        br_input_gamepad_added(s, ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        br_input_gamepad_removed(s, ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        br_gamepad_slot *pad = br_input_find_pad(s, ev->gbutton.which);
        if (pad != NULL && ev->gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
            br_input_set_button(&pad->down[ev->gbutton.button], &pad->edges[ev->gbutton.button], ev->gbutton.down);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        br_gamepad_slot *pad = br_input_find_pad(s, ev->gaxis.which);
        if (pad != NULL && ev->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
            float v = (float)ev->gaxis.value / 32767.0f;
            pad->axes[ev->gaxis.axis] = br_clampf(v, -1.0f, 1.0f);
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        SDL_strlcat(s->text_acc, ev->text.text, sizeof(s->text_acc));
        break;
    default:
        break;
    }
}

/* ---- actions */

static br_action *br_input_find_action(br_input_state *s, const char *name)
{
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = br_array_at(&s->actions, i);
        if (br_str_equal(a->name, name)) {
            return a;
        }
    }
    return NULL;
}

static float br_input_binding_value(br_input_state *s, const br_binding *b)
{
    float best = 0.0f;
    switch (b->kind) {
    case BR_BINDING_KEY:
        return s->keys[b->code] || (s->key_frame_edges[b->code] & BR_EDGE_PRESSED) ? 1.0f : 0.0f;
    case BR_BINDING_MOUSE:
        return s->mouse_buttons[b->code] || (s->mouse_frame_edges[b->code] & BR_EDGE_PRESSED) ? 1.0f : 0.0f;
    case BR_BINDING_GAMEPAD_BUTTON:
        for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
            br_gamepad_slot *pad = &s->gamepads[i];
            if (pad->id != 0 && (pad->down[b->code] || (pad->frame_edges[b->code] & BR_EDGE_PRESSED))) {
                best = 1.0f;
            }
        }
        return best;
    case BR_BINDING_GAMEPAD_AXIS:
        for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
            br_gamepad_slot *pad = &s->gamepads[i];
            float v = pad->id != 0 ? pad->axes[b->code] * (float)b->direction : 0.0f;
            if (v > best) {
                best = v;
            }
        }
        return best;
    }
    return 0.0f;
}

static void br_input_update_actions(br_input_state *s)
{
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = br_array_at(&s->actions, i);
        a->value = 0.0f;
        for (uint32_t j = 0; j < a->bindings.count; j++) {
            float v = br_input_binding_value(s, br_array_at(&a->bindings, j));
            if (v > a->value) {
                a->value = v;
            }
        }
        a->was_down = a->down;
        a->down = a->value >= 0.5f;
    }
}

static void br_input_add_binding(br_engine *e, const char *name, br_binding b)
{
    br_input_state *s = e->input;
    br_action *a = br_input_find_action(s, name);
    if (a == NULL) {
        br_action fresh;
        SDL_zero(fresh);
        fresh.name = br_strdup(name);
        br_array_init(&fresh.bindings, sizeof(br_binding));
        a = br_array_push(&s->actions, &fresh);
        if (a == NULL) {
            br_free(fresh.name);
            return;
        }
    }
    br_array_push(&a->bindings, &b);
}

void br_input_action_bind_key(br_engine *e, const char *action, SDL_Scancode key)
{
    if (key <= SDL_SCANCODE_UNKNOWN || key >= SDL_SCANCODE_COUNT) {
        br_log_error("input: action '%s': bad scancode %d", action, (int)key);
        return;
    }
    br_binding b = { BR_BINDING_KEY, (int)key, 1 };
    br_input_add_binding(e, action, b);
}

void br_input_action_bind_mouse(br_engine *e, const char *action, int button)
{
    if (button <= 0 || button >= BR_INPUT_MOUSE_BUTTONS) {
        br_log_error("input: action '%s': bad mouse button %d", action, button);
        return;
    }
    br_binding b = { BR_BINDING_MOUSE, button, 1 };
    br_input_add_binding(e, action, b);
}

void br_input_action_bind_gamepad_button(br_engine *e, const char *action, SDL_GamepadButton button)
{
    if (button <= SDL_GAMEPAD_BUTTON_INVALID || button >= SDL_GAMEPAD_BUTTON_COUNT) {
        br_log_error("input: action '%s': bad gamepad button %d", action, (int)button);
        return;
    }
    br_binding b = { BR_BINDING_GAMEPAD_BUTTON, (int)button, 1 };
    br_input_add_binding(e, action, b);
}

void br_input_action_bind_gamepad_axis(br_engine *e, const char *action, SDL_GamepadAxis axis, int direction)
{
    if (axis <= SDL_GAMEPAD_AXIS_INVALID || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        br_log_error("input: action '%s': bad gamepad axis %d", action, (int)axis);
        return;
    }
    br_binding b = { BR_BINDING_GAMEPAD_AXIS, (int)axis, direction < 0 ? -1 : 1 };
    br_input_add_binding(e, action, b);
}

void br_input_actions_clear(br_engine *e)
{
    br_input_state *s = e->input;
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = br_array_at(&s->actions, i);
        br_free(a->name);
        br_array_free(&a->bindings);
    }
    br_array_clear(&s->actions);
}

bool br_input_actions_from_json(br_engine *e, const cJSON *json)
{
    if (!cJSON_IsObject(json)) {
        br_log_error("input: actions JSON must be an object");
        return false;
    }
    const cJSON *action = NULL;
    cJSON_ArrayForEach(action, json)
    {
        const char *name = action->string;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(action, "keys"))
        {
            SDL_Scancode key = cJSON_IsString(item) ? SDL_GetScancodeFromName(item->valuestring) : SDL_SCANCODE_UNKNOWN;
            if (key == SDL_SCANCODE_UNKNOWN) {
                br_log_error("input: action '%s': unknown key name", name);
                continue;
            }
            br_input_action_bind_key(e, name, key);
        }
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(action, "mouse"))
        {
            br_input_action_bind_mouse(e, name, cJSON_IsNumber(item) ? item->valueint : 0);
        }
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(action, "buttons"))
        {
            SDL_GamepadButton b = cJSON_IsString(item) ? SDL_GetGamepadButtonFromString(item->valuestring)
                                                       : SDL_GAMEPAD_BUTTON_INVALID;
            br_input_action_bind_gamepad_button(e, name, b);
        }
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(action, "axes"))
        {
            if (!cJSON_IsString(item) || (item->valuestring[0] != '+' && item->valuestring[0] != '-')) {
                br_log_error("input: action '%s': axes must look like \"+leftx\" or \"-lefty\"", name);
                continue;
            }
            int direction = item->valuestring[0] == '-' ? -1 : 1;
            br_input_action_bind_gamepad_axis(e, name, SDL_GetGamepadAxisFromString(item->valuestring + 1), direction);
        }
    }
    return true;
}

bool br_input_action_down(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL && a->down;
}

bool br_input_action_pressed(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL && a->down && !a->was_down;
}

bool br_input_action_released(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL && !a->down && a->was_down;
}

float br_input_action_value(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL ? a->value : 0.0f;
}

/* ---- per frame */

static void br_input_begin_frame(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_input_state *s = e->input;
    if (!s->gamepad_subsystem && br_engine_window(e) != NULL) {
        /* only with a window, so headless tests never touch joystick drivers */
        s->gamepad_subsystem = true;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            br_log_warn("input: no gamepad support: %s", SDL_GetError());
        }
    }
    SDL_memcpy(s->key_frame_edges, s->key_edges, sizeof(s->key_edges));
    SDL_memset(s->key_edges, 0, sizeof(s->key_edges));
    SDL_memcpy(s->mouse_frame_edges, s->mouse_edges, sizeof(s->mouse_edges));
    SDL_memset(s->mouse_edges, 0, sizeof(s->mouse_edges));
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        SDL_memcpy(s->gamepads[i].frame_edges, s->gamepads[i].edges, sizeof(s->gamepads[i].edges));
        SDL_memset(s->gamepads[i].edges, 0, sizeof(s->gamepads[i].edges));
    }
    s->mouse_delta = s->mouse_delta_acc;
    s->mouse_delta_acc = br_vec2_make(0.0f, 0.0f);
    s->wheel = s->wheel_acc;
    s->wheel_acc = br_vec2_make(0.0f, 0.0f);
    SDL_memcpy(s->text, s->text_acc, sizeof(s->text));
    s->text_acc[0] = '\0';
    br_input_update_actions(s);
}

static void br_input_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_input_state *s = e->input;
    br_input_actions_clear(e);
    br_array_free(&s->actions);
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].pad != NULL) {
            SDL_CloseGamepad(s->gamepads[i].pad);
        }
    }
    if (s->gamepad_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    br_free(s);
    e->input = NULL;
}

void br_input_register(br_engine *e)
{
    e->input = br_alloc(sizeof(br_input_state));
    if (e->input == NULL) {
        return;
    }
    br_array_init(&e->input->actions, sizeof(br_action));
    br_engine_add_event_handler(e, br_input_event, NULL);
    br_system_add(e, "input.begin_frame", BR_PHASE_INPUT, -2000000, br_input_begin_frame, NULL);
    br_engine_on_shutdown(e, br_input_shutdown, NULL);
}

/* ---- queries */

static bool br_input_edge(const uint8_t *edges, int code, int limit, uint8_t bit)
{
    return code >= 0 && code < limit && (edges[code] & bit) != 0;
}

bool br_input_key_down(br_engine *e, SDL_Scancode key)
{
    return key >= 0 && key < SDL_SCANCODE_COUNT && e->input->keys[key];
}

bool br_input_key_pressed(br_engine *e, SDL_Scancode key)
{
    return br_input_edge(e->input->key_frame_edges, (int)key, SDL_SCANCODE_COUNT, BR_EDGE_PRESSED);
}

bool br_input_key_released(br_engine *e, SDL_Scancode key)
{
    return br_input_edge(e->input->key_frame_edges, (int)key, SDL_SCANCODE_COUNT, BR_EDGE_RELEASED);
}

br_vec2 br_input_mouse_position(br_engine *e)
{
    return e->input->mouse;
}

br_vec2 br_input_mouse_delta(br_engine *e)
{
    return e->input->mouse_delta;
}

br_vec2 br_input_mouse_wheel(br_engine *e)
{
    return e->input->wheel;
}

bool br_input_mouse_down(br_engine *e, int button)
{
    return button >= 0 && button < BR_INPUT_MOUSE_BUTTONS && e->input->mouse_buttons[button];
}

bool br_input_mouse_pressed(br_engine *e, int button)
{
    return br_input_edge(e->input->mouse_frame_edges, button, BR_INPUT_MOUSE_BUTTONS, BR_EDGE_PRESSED);
}

bool br_input_mouse_released(br_engine *e, int button)
{
    return br_input_edge(e->input->mouse_frame_edges, button, BR_INPUT_MOUSE_BUTTONS, BR_EDGE_RELEASED);
}

uint32_t br_input_touch_count(br_engine *e)
{
    return e->input->touch_count;
}

bool br_input_touch_get(br_engine *e, uint32_t i, br_touch *out)
{
    if (i >= e->input->touch_count) {
        return false;
    }
    *out = e->input->touches[i];
    return true;
}

br_pen br_input_pen(br_engine *e)
{
    return e->input->pen;
}

bool br_input_gamepad_connected(br_engine *e, int slot)
{
    return br_input_pad_at(e, slot) != NULL;
}

int br_input_gamepad_count(br_engine *e)
{
    int count = 0;
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        count += e->input->gamepads[i].id != 0 ? 1 : 0;
    }
    return count;
}

bool br_input_gamepad_down(br_engine *e, int slot, SDL_GamepadButton button)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    return pad != NULL && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && pad->down[button];
}

bool br_input_gamepad_pressed(br_engine *e, int slot, SDL_GamepadButton button)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    return pad != NULL && br_input_edge(pad->frame_edges, (int)button, SDL_GAMEPAD_BUTTON_COUNT, BR_EDGE_PRESSED);
}

bool br_input_gamepad_released(br_engine *e, int slot, SDL_GamepadButton button)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    return pad != NULL && br_input_edge(pad->frame_edges, (int)button, SDL_GAMEPAD_BUTTON_COUNT, BR_EDGE_RELEASED);
}

float br_input_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        return 0.0f;
    }
    return pad->axes[axis];
}

bool br_input_gamepad_rumble(br_engine *e, int slot, float low, float high, uint32_t duration_ms)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || pad->pad == NULL) {
        return false;
    }
    Uint16 lo = (Uint16)(br_clampf(low, 0.0f, 1.0f) * 65535.0f);
    Uint16 hi = (Uint16)(br_clampf(high, 0.0f, 1.0f) * 65535.0f);
    return SDL_RumbleGamepad(pad->pad, lo, hi, duration_ms);
}

void br_input_text_start(br_engine *e)
{
    if (br_engine_window(e) != NULL) {
        SDL_StartTextInput(br_engine_window(e));
    }
}

void br_input_text_stop(br_engine *e)
{
    if (br_engine_window(e) != NULL) {
        SDL_StopTextInput(br_engine_window(e));
    }
}

const char *br_input_text(br_engine *e)
{
    return e->input->text;
}

/* ---- injection */

static SDL_WindowID br_input_window_id(br_engine *e)
{
    return br_engine_window(e) != NULL ? SDL_GetWindowID(br_engine_window(e)) : 0;
}

static void br_input_send(br_engine *e, SDL_Event *ev)
{
    ev->common.timestamp = SDL_GetTicksNS();
    br_app_event(e, ev);
}

void br_input_inject_pointer_move(br_engine *e, float x, float y)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.windowID = br_input_window_id(e);
    ev.motion.x = x;
    ev.motion.y = y;
    ev.motion.xrel = x - e->input->mouse.x;
    ev.motion.yrel = y - e->input->mouse.y;
    br_input_send(e, &ev);
}

static void br_input_inject_button(br_engine *e, float x, float y, int button, bool down)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.windowID = br_input_window_id(e);
    ev.button.button = (Uint8)button;
    ev.button.down = down;
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    br_input_send(e, &ev);
}

void br_input_inject_pointer_down(br_engine *e, float x, float y, int button)
{
    br_input_inject_button(e, x, y, button, true);
}

void br_input_inject_pointer_up(br_engine *e, float x, float y, int button)
{
    br_input_inject_button(e, x, y, button, false);
}

void br_input_inject_wheel(br_engine *e, float dx, float dy)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.windowID = br_input_window_id(e);
    ev.wheel.x = dx;
    ev.wheel.y = dy;
    ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    ev.wheel.mouse_x = e->input->mouse.x;
    ev.wheel.mouse_y = e->input->mouse.y;
    br_input_send(e, &ev);
}

void br_input_inject_key(br_engine *e, SDL_Scancode key, bool down)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.windowID = br_input_window_id(e);
    ev.key.scancode = key;
    ev.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    ev.key.down = down;
    br_input_send(e, &ev);
}

void br_input_inject_touch(br_engine *e, SDL_EventType type, uint64_t finger, float x, float y)
{
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = type;
    ev.tfinger.touchID = 1;
    ev.tfinger.fingerID = (SDL_FingerID)finger;
    ev.tfinger.windowID = br_input_window_id(e);
    ev.tfinger.x = w > 0 ? x / (float)w : x;   /* SDL touch coordinates are normalised */
    ev.tfinger.y = h > 0 ? y / (float)h : y;
    ev.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0f : 1.0f;
    br_input_send(e, &ev);
}
