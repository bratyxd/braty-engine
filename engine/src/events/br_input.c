/* br_input: polled keyboard / mouse / touch / pen / text state fed from SDL events, the
 * per-frame latch, and event injection. Gamepads: br_input_gamepad.c, actions:
 * br_input_action.c, glyphs: br_input_glyph.c. */
#include "events/br_input_internal.h"

/* ---- helpers */

void br_input_set_button(bool *down, uint8_t *edges, bool now_down)
{
    if (now_down && !*down) {
        *edges |= BR_EDGE_PRESSED;
    } else if (!now_down && *down) {
        *edges |= BR_EDGE_RELEASED;
    }
    *down = now_down;
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
    if (type == SDL_EVENT_FINGER_DOWN) {
        s->last_device = BR_INPUT_DEVICE_TOUCH;
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

/* True for SDL's mouse events synthesised from touch or pen, when those are switched off. */
static bool br_input_skip_mouse(const br_input_state *s, SDL_MouseID which)
{
    return !s->touch_mouse && (which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID);
}

static void br_input_mouse_used(br_input_state *s, SDL_MouseID which)
{
    if (which != SDL_TOUCH_MOUSEID && which != SDL_PEN_MOUSEID) {
        s->last_device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
    }
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
            if (ev->key.down) {
                s->last_device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (br_input_skip_mouse(s, ev->motion.which)) {
            break;
        }
        s->mouse = br_vec2_make(ev->motion.x, ev->motion.y);
        s->mouse_delta_acc = br_vec2_add(s->mouse_delta_acc, br_vec2_make(ev->motion.xrel, ev->motion.yrel));
        br_input_mouse_used(s, ev->motion.which);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (br_input_skip_mouse(s, ev->button.which)) {
            break;
        }
        s->mouse = br_vec2_make(ev->button.x, ev->button.y);
        if (ev->button.button < BR_INPUT_MOUSE_BUTTONS) {
            br_input_set_button(&s->mouse_buttons[ev->button.button], &s->mouse_edges[ev->button.button],
                                ev->button.down);
        }
        br_input_mouse_used(s, ev->button.which);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float dir = ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        s->wheel_acc = br_vec2_add(s->wheel_acc, br_vec2_make(ev->wheel.x * dir, ev->wheel.y * dir));
        s->wheel_ticks_acc = br_vec2_add(s->wheel_ticks_acc, br_vec2_make((float)ev->wheel.integer_x * dir,
                                                                          (float)ev->wheel.integer_y * dir));
        s->last_device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
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
        if (ev->ptouch.down) {
            s->last_device = BR_INPUT_DEVICE_PEN;
        }
        break;
    case SDL_EVENT_PEN_AXIS:
        if (ev->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
            s->pen.pressure = ev->paxis.value;
        }
        break;
    case SDL_EVENT_TEXT_INPUT:
        SDL_strlcat(s->text_acc, ev->text.text, sizeof(s->text_acc));
        s->composition[0] = '\0';
        break;
    case SDL_EVENT_TEXT_EDITING:
        SDL_strlcpy(s->composition, ev->edit.text != NULL ? ev->edit.text : "", sizeof(s->composition));
        s->composition_cursor = ev->edit.start;
        s->composition_length = ev->edit.length;
        break;
    default:
        br_input_gamepad_event(e, ev);
        break;
    }
}

/* ---- per frame */

static void br_input_begin_frame(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_input_state *s = e->input;
    if (!s->gamepad_subsystem && br_engine_window(e) != NULL) {
        /* only with a window, so headless tests never touch joystick drivers */
        br_input_gamepad_init_subsystem(s);
    }
    SDL_memcpy(s->key_frame_edges, s->key_edges, sizeof(s->key_edges));
    SDL_memset(s->key_edges, 0, sizeof(s->key_edges));
    SDL_memcpy(s->mouse_frame_edges, s->mouse_edges, sizeof(s->mouse_edges));
    SDL_memset(s->mouse_edges, 0, sizeof(s->mouse_edges));
    br_input_gamepad_latch(s);
    s->mouse_delta = s->mouse_delta_acc;
    s->mouse_delta_acc = br_vec2_make(0.0f, 0.0f);
    s->wheel = s->wheel_acc;
    s->wheel_acc = br_vec2_make(0.0f, 0.0f);
    s->wheel_ticks = s->wheel_ticks_acc;
    s->wheel_ticks_acc = br_vec2_make(0.0f, 0.0f);
    SDL_memcpy(s->text, s->text_acc, sizeof(s->text));
    s->text_acc[0] = '\0';
    br_input_capture_update(s);
    br_input_update_actions(e);
}

static void br_input_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_input_state *s = e->input;
    br_input_actions_free(s);
    br_input_gamepad_shutdown(s);
    if (s->custom_cursor != NULL) {
        SDL_DestroyCursor(s->custom_cursor);
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
    br_input_state *s = e->input;
    br_array_init(&s->actions, sizeof(br_action));
    s->touch_mouse = true;
    s->deadzone_inner = 0.15f;
    s->deadzone_outer = 0.95f;
    s->last_gamepad = -1;
    br_random_seed(&s->rng, 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL);
    br_engine_add_event_handler(e, br_input_event, NULL);
    br_system_add(e, "input.begin_frame", BR_PHASE_INPUT, -2000000, br_input_begin_frame, NULL);
    br_engine_on_shutdown(e, br_input_shutdown, NULL);
}

br_random *br_random_engine(br_engine *e)
{
    return &e->input->rng;
}

/* ---- queries */

static bool br_input_edge(const uint8_t *edges, int code, int limit, uint8_t bit)
{
    return code >= 0 && code < limit && (edges[code] & bit) != 0;
}

br_input_device br_input_last_device(br_engine *e)
{
    return e->input->last_device;
}

int br_input_last_gamepad(br_engine *e)
{
    return e->input->last_gamepad;
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

bool br_input_keycode_down(br_engine *e, SDL_Keycode key)
{
    return br_input_key_down(e, SDL_GetScancodeFromKey(key, NULL));
}

bool br_input_keycode_pressed(br_engine *e, SDL_Keycode key)
{
    return br_input_key_pressed(e, SDL_GetScancodeFromKey(key, NULL));
}

bool br_input_keycode_released(br_engine *e, SDL_Keycode key)
{
    return br_input_key_released(e, SDL_GetScancodeFromKey(key, NULL));
}

SDL_Keymod br_input_key_mods(br_engine *e)
{
    const bool *k = e->input->keys;
    /* from our own key state, so injected keys count; lock keys only SDL knows */
    SDL_Keymod mods = SDL_GetModState() & (SDL_KMOD_CAPS | SDL_KMOD_NUM | SDL_KMOD_SCROLL);
    mods |= k[SDL_SCANCODE_LSHIFT] ? SDL_KMOD_LSHIFT : 0;
    mods |= k[SDL_SCANCODE_RSHIFT] ? SDL_KMOD_RSHIFT : 0;
    mods |= k[SDL_SCANCODE_LCTRL] ? SDL_KMOD_LCTRL : 0;
    mods |= k[SDL_SCANCODE_RCTRL] ? SDL_KMOD_RCTRL : 0;
    mods |= k[SDL_SCANCODE_LALT] ? SDL_KMOD_LALT : 0;
    mods |= k[SDL_SCANCODE_RALT] ? SDL_KMOD_RALT : 0;
    mods |= k[SDL_SCANCODE_LGUI] ? SDL_KMOD_LGUI : 0;
    mods |= k[SDL_SCANCODE_RGUI] ? SDL_KMOD_RGUI : 0;
    return mods;
}

bool br_input_key_name(SDL_Scancode key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (key <= SDL_SCANCODE_UNKNOWN || key >= SDL_SCANCODE_COUNT) {
        return false;
    }
    const char *name = SDL_GetKeyName(SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false));
    if (name == NULL || name[0] == '\0') {
        name = SDL_GetScancodeName(key);
    }
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    br_str_copy(out, out_size, name);
    return true;
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

br_vec2 br_input_mouse_wheel_ticks(br_engine *e)
{
    return e->input->wheel_ticks;
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

bool br_input_mouse_set_relative(br_engine *e, bool relative)
{
    SDL_Window *w = br_engine_window(e);
    if (w == NULL || !SDL_SetWindowRelativeMouseMode(w, relative)) {
        br_log_error("input: relative mouse mode: %s", w == NULL ? "no window" : SDL_GetError());
        return false;
    }
    return true;
}

bool br_input_mouse_relative(br_engine *e)
{
    SDL_Window *w = br_engine_window(e);
    return w != NULL && SDL_GetWindowRelativeMouseMode(w);
}

bool br_input_mouse_set_confined(br_engine *e, bool confined)
{
    SDL_Window *w = br_engine_window(e);
    if (w == NULL || !SDL_SetWindowMouseGrab(w, confined)) {
        br_log_error("input: mouse confine: %s", w == NULL ? "no window" : SDL_GetError());
        return false;
    }
    return true;
}

void br_input_mouse_show_cursor(br_engine *e, bool visible)
{
    (void)e;
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

bool br_input_mouse_set_cursor_image(br_engine *e, const uint8_t *rgba, int width, int height, int hot_x, int hot_y)
{
    br_input_state *s = e->input;
    SDL_Cursor *cursor = NULL;
    if (rgba != NULL) {
        SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, (void *)rgba, width * 4);
        if (surface == NULL) {
            br_log_error("input: cursor image: %s", SDL_GetError());
            return false;
        }
        cursor = SDL_CreateColorCursor(surface, hot_x, hot_y);
        SDL_DestroySurface(surface);
        if (cursor == NULL) {
            br_log_error("input: cursor image: %s", SDL_GetError());
            return false;
        }
    }
    SDL_SetCursor(cursor != NULL ? cursor : SDL_GetDefaultCursor());
    if (s->custom_cursor != NULL) {
        SDL_DestroyCursor(s->custom_cursor);
    }
    s->custom_cursor = cursor;
    return true;
}

SDL_Cursor *br_input_custom_cursor(br_engine *e)
{
    return e->input != NULL ? e->input->custom_cursor : NULL;
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

void br_input_touch_set_mouse_emulation(br_engine *e, bool enabled)
{
    e->input->touch_mouse = enabled;
}

br_pen br_input_pen(br_engine *e)
{
    return e->input->pen;
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
    e->input->composition[0] = '\0';
}

const char *br_input_text(br_engine *e)
{
    return e->input->text;
}

const char *br_input_text_composition(br_engine *e, int *cursor, int *selection_length)
{
    br_input_state *s = e->input;
    if (cursor != NULL) {
        *cursor = s->composition_cursor;
    }
    if (selection_length != NULL) {
        *selection_length = s->composition_length;
    }
    return s->composition;
}

void br_input_text_set_area(br_engine *e, br_rect area, int cursor)
{
    if (br_engine_window(e) == NULL) {
        return;
    }
    SDL_Rect r = { (int)area.x, (int)area.y, (int)area.width, (int)area.height };
    SDL_SetTextInputArea(br_engine_window(e), &r, cursor);
}

/* ---- injection */

static SDL_WindowID br_input_window_id(br_engine *e)
{
    return br_engine_window(e) != NULL ? SDL_GetWindowID(br_engine_window(e)) : 0;
}

void br_input_send(br_engine *e, SDL_Event *ev)
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
    ev.wheel.integer_x = (Sint32)dx;
    ev.wheel.integer_y = (Sint32)dy;
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

void br_input_inject_text(br_engine *e, const char *utf8)
{
    if (utf8 == NULL) {
        return;
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.windowID = br_input_window_id(e);
    ev.text.text = utf8;   /* read synchronously by the handlers */
    br_input_send(e, &ev);
}
