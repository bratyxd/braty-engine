/* br_input_gamepad: gamepad slots (players), hot-plug, dead zones, families, feedback, sensors,
 * touchpads, injected and virtual pads. */
#include "events/br_input_internal.h"

/* ---- type mapping */

br_gamepad_type br_input_gamepad_type_from_sdl(SDL_GamepadType type)
{
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: return BR_GAMEPAD_TYPE_XBOX360;
    case SDL_GAMEPAD_TYPE_XBOXONE: return BR_GAMEPAD_TYPE_XBOXONE;
    case SDL_GAMEPAD_TYPE_PS3: return BR_GAMEPAD_TYPE_PS3;
    case SDL_GAMEPAD_TYPE_PS4: return BR_GAMEPAD_TYPE_PS4;
    case SDL_GAMEPAD_TYPE_PS5: return BR_GAMEPAD_TYPE_PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return BR_GAMEPAD_TYPE_SWITCH_PRO;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return BR_GAMEPAD_TYPE_GAMECUBE;
    case SDL_GAMEPAD_TYPE_STEAM: return BR_GAMEPAD_TYPE_STEAM;
    default: return BR_GAMEPAD_TYPE_GENERIC;
    }
}

SDL_GamepadType br_input_gamepad_type_to_sdl(br_gamepad_type type)
{
    switch (type) {
    case BR_GAMEPAD_TYPE_XBOX360: return SDL_GAMEPAD_TYPE_XBOX360;
    case BR_GAMEPAD_TYPE_XBOXONE: return SDL_GAMEPAD_TYPE_XBOXONE;
    case BR_GAMEPAD_TYPE_PS3: return SDL_GAMEPAD_TYPE_PS3;
    case BR_GAMEPAD_TYPE_PS4: return SDL_GAMEPAD_TYPE_PS4;
    case BR_GAMEPAD_TYPE_PS5: return SDL_GAMEPAD_TYPE_PS5;
    case BR_GAMEPAD_TYPE_SWITCH_PRO: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    case BR_GAMEPAD_TYPE_GAMECUBE: return SDL_GAMEPAD_TYPE_GAMECUBE;
    case BR_GAMEPAD_TYPE_STEAM: return SDL_GAMEPAD_TYPE_STEAM;
    default: return SDL_GAMEPAD_TYPE_STANDARD;
    }
}

br_gamepad_family br_input_gamepad_family_of(br_gamepad_type type)
{
    switch (type) {
    case BR_GAMEPAD_TYPE_XBOX360:
    case BR_GAMEPAD_TYPE_XBOXONE:
        return BR_GAMEPAD_FAMILY_XBOX;
    case BR_GAMEPAD_TYPE_PS3:
    case BR_GAMEPAD_TYPE_PS4:
    case BR_GAMEPAD_TYPE_PS5:
        return BR_GAMEPAD_FAMILY_PLAYSTATION;
    case BR_GAMEPAD_TYPE_SWITCH_PRO:
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT:
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT:
    case BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR:
    case BR_GAMEPAD_TYPE_GAMECUBE:
        return BR_GAMEPAD_FAMILY_NINTENDO;
    default:
        return BR_GAMEPAD_FAMILY_GENERIC;
    }
}

/* ---- slots */

static br_gamepad_slot *br_input_find_pad(br_input_state *s, SDL_JoystickID id)
{
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].id == id && id != 0) {
            return &s->gamepads[i];
        }
    }
    return NULL;
}

br_gamepad_slot *br_input_pad_at(br_engine *e, int slot)
{
    if (slot < 0 || slot >= BR_INPUT_MAX_GAMEPADS || e->input->gamepads[slot].id == 0) {
        return NULL;
    }
    return &e->input->gamepads[slot];
}

bool br_input_gamepad_init_subsystem(br_input_state *s)
{
    if (s->gamepad_subsystem) {
        return true;
    }
    s->gamepad_subsystem = true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        br_log_warn("input: no gamepad support: %s", SDL_GetError());
        s->gamepad_subsystem = false;
        return false;
    }
    return true;
}

static int br_input_free_slot(br_input_state *s, SDL_GUID guid)
{
    /* 1. the slot this device had before, 2. a never used slot, 3. any free slot */
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        br_gamepad_slot *p = &s->gamepads[i];
        if (p->id == 0 && p->had_pad && SDL_memcmp(&p->last_guid, &guid, sizeof(guid)) == 0) {
            return i;
        }
    }
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].id == 0 && !s->gamepads[i].had_pad) {
            return i;
        }
    }
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->gamepads[i].id == 0) {
            return i;
        }
    }
    return -1;
}

/* Empties a slot but keeps what reconnects need. */
static void br_input_clear_slot(br_gamepad_slot *p)
{
    SDL_GUID guid = p->last_guid;
    bool had = p->had_pad;
    SDL_zerop(p);
    p->last_guid = guid;
    p->had_pad = had;
}

static void br_input_gamepad_added(br_input_state *s, SDL_JoystickID id)
{
    if (br_input_find_pad(s, id) != NULL) {
        return;
    }
    SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
    int slot = br_input_free_slot(s, guid);
    if (slot < 0) {
        br_log_warn("input: more than %d gamepads, ignoring one", BR_INPUT_MAX_GAMEPADS);
        return;
    }
    br_gamepad_slot *p = &s->gamepads[slot];
    br_input_clear_slot(p);
    p->id = id;
    p->pad = SDL_OpenGamepad(id);
    p->last_guid = guid;
    p->had_pad = true;
    if (p->pad != NULL) {
        SDL_SetGamepadPlayerIndex(p->pad, slot);
    }
    br_log_info("input: gamepad %d connected (%s)", slot, p->pad != NULL ? SDL_GetGamepadName(p->pad) : "unknown");
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
    br_input_clear_slot(pad);
    br_log_info("input: gamepad %d disconnected", (int)(pad - s->gamepads));
}

void br_input_gamepad_event(br_engine *e, const SDL_Event *ev)
{
    br_input_state *s = e->input;
    switch (ev->type) {
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
            if (ev->gbutton.down) {
                s->last_device = BR_INPUT_DEVICE_GAMEPAD;
                s->last_gamepad = (int)(pad - s->gamepads);
            }
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        br_gamepad_slot *pad = br_input_find_pad(s, ev->gaxis.which);
        if (pad != NULL && ev->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
            float v = br_clampf((float)ev->gaxis.value / 32767.0f, -1.0f, 1.0f);
            pad->axes[ev->gaxis.axis] = v;
            if (v > 0.5f || v < -0.5f) {
                s->last_device = BR_INPUT_DEVICE_GAMEPAD;
                s->last_gamepad = (int)(pad - s->gamepads);
            }
        }
        break;
    }
    default:
        break;
    }
}

void br_input_gamepad_latch(br_input_state *s)
{
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        SDL_memcpy(s->gamepads[i].frame_edges, s->gamepads[i].edges, sizeof(s->gamepads[i].edges));
        SDL_memset(s->gamepads[i].edges, 0, sizeof(s->gamepads[i].edges));
    }
}

void br_input_gamepad_shutdown(br_input_state *s)
{
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        br_gamepad_slot *p = &s->gamepads[i];
        if (p->pad != NULL) {
            SDL_CloseGamepad(p->pad);
        }
        if (p->virtual_device) {
            SDL_DetachVirtualJoystick(p->id);
        }
    }
    if (s->gamepad_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

/* ---- dead zones */

static float br_input_deadzone(const br_input_state *s, float v)
{
    float range = s->deadzone_outer - s->deadzone_inner;
    if (v <= s->deadzone_inner) {
        return 0.0f;
    }
    if (v >= s->deadzone_outer || range <= 0.0f) {
        return 1.0f;
    }
    return (v - s->deadzone_inner) / range;
}

static br_vec2 br_input_stick_of(const br_input_state *s, const br_gamepad_slot *pad, int axis_x)
{
    br_vec2 raw = br_vec2_make(pad->axes[axis_x], pad->axes[axis_x + 1]);
    float len = br_vec2_length(raw);
    if (len <= 0.0f) {
        return br_vec2_make(0.0f, 0.0f);
    }
    /* radial: the stick's length goes through the dead zone, its direction is kept */
    return br_vec2_scale(raw, br_input_deadzone(s, len) / len);
}

float br_input_pad_axis(br_input_state *s, const br_gamepad_slot *pad, int axis)
{
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        return br_input_deadzone(s, pad->axes[axis]);
    }
    int base = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY ? SDL_GAMEPAD_AXIS_LEFTX
                                                                                 : SDL_GAMEPAD_AXIS_RIGHTX;
    br_vec2 v = br_input_stick_of(s, pad, base);
    return axis == base ? v.x : v.y;
}

void br_input_gamepad_set_deadzone(br_engine *e, float inner, float outer)
{
    e->input->deadzone_inner = br_clampf(inner, 0.0f, 1.0f);
    e->input->deadzone_outer = br_clampf(outer, e->input->deadzone_inner, 1.0f);
}

/* ---- queries */

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

static bool br_input_pad_edge(br_engine *e, int slot, SDL_GamepadButton button, uint8_t bit)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    return pad != NULL && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && (pad->frame_edges[button] & bit) != 0;
}

bool br_input_gamepad_pressed(br_engine *e, int slot, SDL_GamepadButton button)
{
    return br_input_pad_edge(e, slot, button, BR_EDGE_PRESSED);
}

bool br_input_gamepad_released(br_engine *e, int slot, SDL_GamepadButton button)
{
    return br_input_pad_edge(e, slot, button, BR_EDGE_RELEASED);
}

float br_input_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        return 0.0f;
    }
    return br_input_pad_axis(e->input, pad, axis);
}

float br_input_gamepad_axis_raw(br_engine *e, int slot, SDL_GamepadAxis axis)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        return 0.0f;
    }
    return pad->axes[axis];
}

br_vec2 br_input_gamepad_stick(br_engine *e, int slot, br_gamepad_stick stick)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL) {
        return br_vec2_make(0.0f, 0.0f);
    }
    return br_input_stick_of(e->input, pad, stick == BR_GAMEPAD_STICK_RIGHT ? SDL_GAMEPAD_AXIS_RIGHTX
                                                                            : SDL_GAMEPAD_AXIS_LEFTX);
}

br_gamepad_type br_input_gamepad_type(br_engine *e, int slot)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || pad->pad == NULL) {
        return BR_GAMEPAD_TYPE_GENERIC;
    }
    return br_input_gamepad_type_from_sdl(SDL_GetGamepadType(pad->pad));
}

bool br_input_gamepad_info(br_engine *e, int slot, br_gamepad_info *out)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    if (pad == NULL || out == NULL) {
        return false;
    }
    SDL_zerop(out);
    out->id = pad->id;
    out->injected = pad->injected;
    out->virtual_device = pad->virtual_device;
    out->type = br_input_gamepad_type(e, slot);
    out->family = br_input_gamepad_family_of(out->type);
    out->power = SDL_POWERSTATE_UNKNOWN;
    out->battery_percent = -1;
    if (pad->pad == NULL) {
        br_str_copy(out->name, sizeof(out->name), "Injected gamepad");
        return true;
    }
    const char *name = SDL_GetGamepadName(pad->pad);
    br_str_copy(out->name, sizeof(out->name), name != NULL ? name : "Gamepad");
    out->vendor = SDL_GetGamepadVendor(pad->pad);
    out->product = SDL_GetGamepadProduct(pad->pad);
    out->wireless = SDL_GetGamepadConnectionState(pad->pad) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    out->power = SDL_GetGamepadPowerInfo(pad->pad, &out->battery_percent);
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad->pad);
    out->has_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    out->has_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    out->has_rgb_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    out->has_player_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_PLAYER_LED_BOOLEAN, false);
    out->has_gyro = SDL_GamepadHasSensor(pad->pad, SDL_SENSOR_GYRO);
    out->has_accel = SDL_GamepadHasSensor(pad->pad, SDL_SENSOR_ACCEL);
    out->touchpads = SDL_GetNumGamepadTouchpads(pad->pad);
    return true;
}

bool br_input_gamepad_assign(br_engine *e, int from_slot, int to_slot)
{
    br_input_state *s = e->input;
    if (br_input_pad_at(e, from_slot) == NULL || to_slot < 0 || to_slot >= BR_INPUT_MAX_GAMEPADS) {
        br_log_error("input: gamepad assign %d -> %d: no pad or bad slot", from_slot, to_slot);
        return false;
    }
    if (from_slot == to_slot) {
        return true;
    }
    br_gamepad_slot tmp = s->gamepads[to_slot];
    s->gamepads[to_slot] = s->gamepads[from_slot];
    s->gamepads[from_slot] = tmp;
    if (s->gamepads[to_slot].pad != NULL) {
        SDL_SetGamepadPlayerIndex(s->gamepads[to_slot].pad, to_slot);
    }
    if (s->gamepads[from_slot].pad != NULL) {
        SDL_SetGamepadPlayerIndex(s->gamepads[from_slot].pad, from_slot);
    }
    if (s->last_gamepad == from_slot) {
        s->last_gamepad = to_slot;
    } else if (s->last_gamepad == to_slot) {
        s->last_gamepad = from_slot;
    }
    return true;
}

/* ---- feedback */

static SDL_Gamepad *br_input_sdl_pad(br_engine *e, int slot)
{
    br_gamepad_slot *pad = br_input_pad_at(e, slot);
    return pad != NULL ? pad->pad : NULL;
}

static Uint16 br_input_motor(float v)
{
    return (Uint16)(br_clampf(v, 0.0f, 1.0f) * 65535.0f);
}

bool br_input_gamepad_rumble(br_engine *e, int slot, float low, float high, uint32_t duration_ms)
{
    SDL_Gamepad *pad = br_input_sdl_pad(e, slot);
    return pad != NULL && SDL_RumbleGamepad(pad, br_input_motor(low), br_input_motor(high), duration_ms);
}

bool br_input_gamepad_rumble_triggers(br_engine *e, int slot, float left, float right, uint32_t duration_ms)
{
    SDL_Gamepad *pad = br_input_sdl_pad(e, slot);
    return pad != NULL && SDL_RumbleGamepadTriggers(pad, br_input_motor(left), br_input_motor(right), duration_ms);
}

bool br_input_gamepad_set_led(br_engine *e, int slot, br_color color)
{
    SDL_Gamepad *pad = br_input_sdl_pad(e, slot);
    uint32_t rgb = br_color_to_hex(color);
    return pad != NULL && SDL_SetGamepadLED(pad, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
}

bool br_input_gamepad_send_effect(br_engine *e, int slot, const void *data, int size)
{
    SDL_Gamepad *pad = br_input_sdl_pad(e, slot);
    return pad != NULL && data != NULL && size > 0 && SDL_SendGamepadEffect(pad, data, size);
}

/* ---- sensors and touchpads */

bool br_input_gamepad_sensor(br_engine *e, int slot, SDL_SensorType type, br_vec3 *out)
{
    br_gamepad_slot *p = br_input_pad_at(e, slot);
    if (out != NULL) {
        *out = br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    if (p == NULL || p->pad == NULL || type <= SDL_SENSOR_UNKNOWN || type >= SDL_SENSOR_COUNT ||
        !SDL_GamepadHasSensor(p->pad, type)) {
        return false;
    }
    if (!p->sensor_on[type]) {
        p->sensor_on[type] = SDL_SetGamepadSensorEnabled(p->pad, type, true);
    }
    float data[3] = { 0.0f, 0.0f, 0.0f };
    if (!SDL_GetGamepadSensorData(p->pad, type, data, 3)) {
        return false;
    }
    if (out != NULL) {
        *out = br_vec3_make(data[0], data[1], data[2]);
    }
    return true;
}

bool br_input_gamepad_touchpad(br_engine *e, int slot, int touchpad, int finger, br_vec2 *position, float *pressure)
{
    SDL_Gamepad *pad = br_input_sdl_pad(e, slot);
    bool down = false;
    float x = 0.0f;
    float y = 0.0f;
    float p = 0.0f;
    if (pad == NULL || !SDL_GetGamepadTouchpadFinger(pad, touchpad, finger, &down, &x, &y, &p)) {
        down = false;
    }
    if (position != NULL) {
        *position = br_vec2_make(x, y);
    }
    if (pressure != NULL) {
        *pressure = p;
    }
    return down;
}

/* ---- injected pads */

static void br_input_send_device(br_engine *e, SDL_EventType type, SDL_JoystickID id)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = type;
    ev.gdevice.which = id;
    br_input_send(e, &ev);
}

/* The id of the pad in `slot`, creating an injected pad there if empty (0 on a bad slot). */
static SDL_JoystickID br_input_injected_id(br_engine *e, int slot)
{
    br_input_state *s = e->input;
    if (slot < 0 || slot >= BR_INPUT_MAX_GAMEPADS) {
        br_log_error("input: inject gamepad: bad slot %d", slot);
        return 0;
    }
    br_gamepad_slot *p = &s->gamepads[slot];
    if (p->id == 0) {
        br_input_clear_slot(p);
        p->id = (SDL_JoystickID)(BR_INPUT_INJECTED_ID_BASE + (uint32_t)slot);
        p->injected = true;
        p->had_pad = true;
        br_log_info("input: injected gamepad in slot %d", slot);
    }
    return p->id;
}

void br_input_inject_gamepad_button(br_engine *e, int slot, SDL_GamepadButton button, bool down)
{
    SDL_JoystickID id = br_input_injected_id(e, slot);
    if (id == 0) {
        return;
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    ev.gbutton.which = id;
    ev.gbutton.button = (Uint8)button;
    ev.gbutton.down = down;
    br_input_send(e, &ev);
}

void br_input_inject_gamepad_axis(br_engine *e, int slot, SDL_GamepadAxis axis, float value)
{
    SDL_JoystickID id = br_input_injected_id(e, slot);
    if (id == 0) {
        return;
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    ev.gaxis.which = id;
    ev.gaxis.axis = (Uint8)axis;
    ev.gaxis.value = (Sint16)(br_clampf(value, -1.0f, 1.0f) * 32767.0f);
    br_input_send(e, &ev);
}

void br_input_inject_gamepad_disconnect(br_engine *e, int slot)
{
    br_gamepad_slot *p = br_input_pad_at(e, slot);
    if (p != NULL && p->injected) {
        br_input_send_device(e, SDL_EVENT_GAMEPAD_REMOVED, p->id);
    }
}

/* ---- SDL virtual pads */

typedef struct br_virtual_ids {
    br_gamepad_type type;
    uint16_t vendor;
    uint16_t product;
    const char *name;
} br_virtual_ids;

/* USB ids SDL recognises for each family (SDL's usb_ids.h / controller_list.h). */
static const br_virtual_ids br_virtual_table[] = {
    { BR_GAMEPAD_TYPE_GENERIC, 0x0000, 0x0000, "Braty virtual gamepad" },
    { BR_GAMEPAD_TYPE_XBOX360, 0x045e, 0x028e, "Braty virtual Xbox 360" },
    { BR_GAMEPAD_TYPE_XBOXONE, 0x045e, 0x02ea, "Braty virtual Xbox One" },
    { BR_GAMEPAD_TYPE_PS3, 0x054c, 0x0268, "Braty virtual PS3" },
    { BR_GAMEPAD_TYPE_PS4, 0x054c, 0x09cc, "Braty virtual PS4" },
    { BR_GAMEPAD_TYPE_PS5, 0x054c, 0x0ce6, "Braty virtual PS5" },
    { BR_GAMEPAD_TYPE_SWITCH_PRO, 0x057e, 0x2009, "Braty virtual Switch Pro" },
    { BR_GAMEPAD_TYPE_SWITCH_JOYCON_LEFT, 0x057e, 0x2006, "Braty virtual Joy-Con (L)" },
    { BR_GAMEPAD_TYPE_SWITCH_JOYCON_RIGHT, 0x057e, 0x2007, "Braty virtual Joy-Con (R)" },
    { BR_GAMEPAD_TYPE_SWITCH_JOYCON_PAIR, 0x057e, 0x2008, "Braty virtual Joy-Con pair" },
    { BR_GAMEPAD_TYPE_GAMECUBE, 0x057e, 0x0337, "Braty virtual GameCube" },
    { BR_GAMEPAD_TYPE_STEAM, 0x28de, 0x1102, "Braty virtual Steam Controller" },
};

int br_input_gamepad_attach_virtual(br_engine *e, br_gamepad_type type)
{
    br_input_state *s = e->input;
    if ((int)type < 0 || type >= BR_GAMEPAD_TYPE_COUNT || !br_input_gamepad_init_subsystem(s)) {
        return -1;
    }
    const br_virtual_ids *ids = &br_virtual_table[type];
    br_gamepad_family family = br_input_gamepad_family_of(type);
    SDL_VirtualJoystickTouchpadDesc touchpad;
    SDL_zero(touchpad);
    touchpad.nfingers = 2;
    SDL_VirtualJoystickSensorDesc sensors[2];
    SDL_zeroa(sensors);
    sensors[0].type = SDL_SENSOR_GYRO;
    sensors[0].rate = 250.0f;
    sensors[1].type = SDL_SENSOR_ACCEL;
    sensors[1].rate = 250.0f;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = ids->vendor;
    desc.product_id = ids->product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = ids->name;
    if (type == BR_GAMEPAD_TYPE_PS4 || type == BR_GAMEPAD_TYPE_PS5) {
        desc.ntouchpads = 1;
        desc.touchpads = &touchpad;
    }
    if (type == BR_GAMEPAD_TYPE_PS4 || type == BR_GAMEPAD_TYPE_PS5 || family == BR_GAMEPAD_FAMILY_NINTENDO) {
        desc.nsensors = 2;
        desc.sensors = sensors;
    }
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        br_log_error("input: virtual gamepad: %s", SDL_GetError());
        return -1;
    }
    /* add it now (SDL's own ADDED event arrives later and is ignored as a duplicate) */
    br_input_send_device(e, SDL_EVENT_GAMEPAD_ADDED, id);
    br_gamepad_slot *p = br_input_find_pad(s, id);
    if (p == NULL || p->pad == NULL) {
        br_log_error("input: virtual gamepad could not be opened: %s", SDL_GetError());
        if (p != NULL) {
            br_input_send_device(e, SDL_EVENT_GAMEPAD_REMOVED, id);
        }
        SDL_DetachVirtualJoystick(id);
        return -1;
    }
    p->virtual_device = true;
    return (int)(p - s->gamepads);
}

bool br_input_gamepad_detach_virtual(br_engine *e, int slot)
{
    br_gamepad_slot *p = br_input_pad_at(e, slot);
    if (p == NULL || !p->virtual_device) {
        return false;
    }
    SDL_JoystickID id = p->id;
    br_input_send_device(e, SDL_EVENT_GAMEPAD_REMOVED, id);
    return SDL_DetachVirtualJoystick(id);
}
