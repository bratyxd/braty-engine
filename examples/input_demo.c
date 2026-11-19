/* input_demo - live view of every input device, gamepad family detection and button glyphs.
 *
 *   input_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Top row: keyboard (held keys, modifiers, typed text, IME composition), mouse (position, delta,
 *   wheel, buttons, a cursor marker), touch and pen.
 * Middle: gamepad slots 0-3: name, type/family, battery, every button labelled with what is
 *   printed on that pad (A / Cross / B...), sticks after the dead zone, triggers, gyro, touchpad.
 * Bottom: actions (bars + down marker), the last used device, a prompt with "{action}"
 *   placeholders expanded for that device, and the history of glyph switches.
 *
 * Plug in a controller and it shows up. With --frames the demo turns off physical joystick
 * drivers and plays a fixed script instead (keyboard, mouse, touch, then SDL virtual Xbox One,
 * Switch Pro and PS5 pads plus one injected pad), so the screenshot is reproducible and shows
 * the prompt switching Space -> A -> B -> Cross.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>

static bool g_scripted = false;

enum { SLOTS_SHOWN = 4, PAD_BUTTONS = 15, ACTION_COUNT = 8, MAX_TOUCH_DOTS = 4 };

static const char *g_actions[ACTION_COUNT] = { "jump", "fire", "sprint", "pause", "left", "right", "up", "down" };

/* The 15 buttons drawn per pad, in a 5 x 3 grid. */
static const SDL_GamepadButton g_pad_buttons[PAD_BUTTONS] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
};

typedef struct pad_view {
    br_node header;
    br_node cells[PAD_BUTTONS];
    br_node cell_labels[PAD_BUTTONS];
    br_node stick_dot[2];
    br_node trigger_fill[2];
    br_node extra;
} pad_view;

typedef struct demo_state {
    br_texture white;
    br_node    keyboard_text;
    br_node    mouse_text;
    br_node    mouse_buttons[3];
    br_node    cursor_marker;
    br_node    touch_text;
    br_node    touch_dots[MAX_TOUCH_DOTS];
    pad_view   pads[SLOTS_SHOWN];
    br_node    action_fill[ACTION_COUNT];
    br_node    action_down[ACTION_COUNT];
    br_node    move_text;
    br_node    glyph_text;
    char       typed[128];
    char       history[160];
    char       last_label[32];
    int        virtual_slots[3];
} demo_state;

/* ---- helpers */

static br_node make_rect(demo_state *s, br_engine *e, br_node parent, float x, float y, float w, float h, uint32_t rgb)
{
    br_node n = br_sprite_create(e, s->white);
    br_sprite_set_size(e, n, w, h);
    br_node_set_position(e, n, x, y);
    br_node_set_tint(e, n, br_color_from_hex(rgb, 1.0f));
    br_node_add_child(e, parent, n);
    return n;
}

static br_node make_text(br_engine *e, br_node parent, float x, float y, float size, uint32_t rgb, const char *text)
{
    br_text_style st = br_text_style_default();
    st.font_size = size;
    st.fill = br_color_from_hex(rgb, 1.0f);
    br_node n = br_text_create(e, text, &st);
    br_node_set_position(e, n, x, y);
    br_node_add_child(e, parent, n);
    return n;
}

static br_node make_panel(demo_state *s, br_engine *e, float x, float y, float w, float h, const char *title)
{
    br_node root = br_scene_root(e);
    make_rect(s, e, root, x, y, w, h, 0x2a3040);
    make_text(e, root, x + 10, y + 6, 16, 0x9fb4d8, title);
    return root;
}

static const char *device_name(br_input_device d)
{
    switch (d) {
    case BR_INPUT_DEVICE_KEYBOARD_MOUSE: return "keyboard + mouse";
    case BR_INPUT_DEVICE_GAMEPAD: return "gamepad";
    case BR_INPUT_DEVICE_TOUCH: return "touch";
    case BR_INPUT_DEVICE_PEN: return "pen";
    default: return "none yet";
    }
}

static const char *family_name(br_gamepad_family f)
{
    switch (f) {
    case BR_GAMEPAD_FAMILY_XBOX: return "Xbox";
    case BR_GAMEPAD_FAMILY_PLAYSTATION: return "PlayStation";
    case BR_GAMEPAD_FAMILY_NINTENDO: return "Nintendo";
    default: return "generic";
    }
}

static const char *type_name(br_gamepad_type t)
{
    static const char *names[BR_GAMEPAD_TYPE_COUNT] = { "Generic", "Xbox 360", "Xbox One", "PS3", "PS4", "PS5",
                                                         "Switch Pro", "Joy-Con L", "Joy-Con R", "Joy-Con pair",
                                                         "GameCube", "Steam" };
    return t >= 0 && t < BR_GAMEPAD_TYPE_COUNT ? names[t] : "?";
}

/* ---- the scripted session (--frames): injected before the input latch */

static void script_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    uint64_t f = br_ticker_frame(e);
    switch (f) {
    case 2:
        br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
        br_input_inject_text(e, "hi ");
        break;
    case 4:
        br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
        break;
    case 6:
        br_input_inject_key(e, SDL_SCANCODE_W, true);
        br_input_inject_key(e, SDL_SCANCODE_LSHIFT, true);
        br_input_inject_pointer_move(e, 610.0f, 150.0f);
        br_input_inject_pointer_down(e, 610.0f, 150.0f, SDL_BUTTON_LEFT);
        break;
    case 8:
        br_input_inject_wheel(e, 0.0f, 1.0f);
        br_input_inject_text(e, "there");
        break;
    case 10:
        br_input_inject_touch(e, SDL_EVENT_FINGER_DOWN, 1, 1100.0f, 200.0f);
        break;
    case 12:
        s->virtual_slots[0] = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_XBOXONE);
        s->virtual_slots[1] = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_SWITCH_PRO);
        s->virtual_slots[2] = br_input_gamepad_attach_virtual(e, BR_GAMEPAD_TYPE_PS5);
        br_input_inject_gamepad_axis(e, 3, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.3f);   /* an injected pad */
        break;
    case 20:
        br_input_inject_gamepad_button(e, s->virtual_slots[0], SDL_GAMEPAD_BUTTON_SOUTH, true);
        br_input_inject_gamepad_axis(e, s->virtual_slots[0], SDL_GAMEPAD_AXIS_LEFTX, -0.8f);
        break;
    case 30:
        br_input_inject_gamepad_button(e, s->virtual_slots[0], SDL_GAMEPAD_BUTTON_SOUTH, false);
        br_input_inject_gamepad_button(e, s->virtual_slots[1], SDL_GAMEPAD_BUTTON_SOUTH, true);
        br_input_inject_gamepad_button(e, s->virtual_slots[1], SDL_GAMEPAD_BUTTON_EAST, true);
        break;
    case 40: {
        br_input_inject_gamepad_button(e, s->virtual_slots[1], SDL_GAMEPAD_BUTTON_SOUTH, false);
        int ps = s->virtual_slots[2];
        br_input_inject_gamepad_button(e, ps, SDL_GAMEPAD_BUTTON_SOUTH, true);
        br_input_inject_gamepad_button(e, ps, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
        br_input_inject_gamepad_axis(e, ps, SDL_GAMEPAD_AXIS_LEFTX, 0.6f);
        br_input_inject_gamepad_axis(e, ps, SDL_GAMEPAD_AXIS_LEFTY, -0.5f);
        br_input_inject_gamepad_axis(e, ps, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.8f);
        /* touchpad and gyro go through SDL's virtual device state */
        br_gamepad_info info;
        if (br_input_gamepad_info(e, ps, &info)) {
            SDL_Joystick *joy = SDL_GetJoystickFromID(info.id);
            float gyro[3] = { 0.25f, -1.5f, 0.75f };
            br_vec3 unused;
            br_input_gamepad_sensor(e, ps, SDL_SENSOR_GYRO, &unused);   /* enable */
            SDL_SetJoystickVirtualTouchpad(joy, 0, 0, true, 0.3f, 0.6f, 1.0f);
            SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, f, gyro, 3);
            SDL_UpdateJoysticks();
        }
        break;
    }
    default:
        break;
    }
}

/* ---- per-frame view update */

static void update_keyboard_mouse(demo_state *s, br_engine *e)
{
    char buf[512];
    char keys[256] = "";
    for (int k = 0; k < SDL_SCANCODE_COUNT; k++) {
        char name[32];
        if (br_input_key_down(e, (SDL_Scancode)k) && br_input_key_name((SDL_Scancode)k, name, sizeof(name))) {
            SDL_strlcat(keys, name, sizeof(keys));
            SDL_strlcat(keys, "  ", sizeof(keys));
        }
    }
    SDL_Keymod mods = br_input_key_mods(e);
    SDL_strlcat(s->typed, br_input_text(e), sizeof(s->typed));
    SDL_snprintf(buf, sizeof(buf), "held: %s\nmods: %s%s%s%s\ntyped: \"%s\"\nIME composition: \"%s\"",
                 keys[0] != '\0' ? keys : "-", (mods & SDL_KMOD_SHIFT) ? "Shift " : "",
                 (mods & SDL_KMOD_CTRL) ? "Ctrl " : "", (mods & SDL_KMOD_ALT) ? "Alt " : "",
                 (mods & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT)) ? "" : "-", s->typed,
                 br_input_text_composition(e, NULL, NULL));
    br_text_set_text(e, s->keyboard_text, buf);

    br_vec2 m = br_input_mouse_position(e);
    br_vec2 d = br_input_mouse_delta(e);
    br_vec2 w = br_input_mouse_wheel(e);
    br_vec2 t = br_input_mouse_wheel_ticks(e);
    SDL_snprintf(buf, sizeof(buf), "position %.0f, %.0f   delta %.0f, %.0f\nwheel %.2f (ticks %.0f)\nrelative mode: %s",
                 m.x, m.y, d.x, d.y, w.y, t.y, br_input_mouse_relative(e) ? "on" : "off");
    br_text_set_text(e, s->mouse_text, buf);
    int buttons[3] = { SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT };
    for (int i = 0; i < 3; i++) {
        br_node_set_tint(e, s->mouse_buttons[i], br_color_from_hex(br_input_mouse_down(e, buttons[i]) ? 0x40d080 : 0x485068, 1.0f));
    }
    br_node_set_position(e, s->cursor_marker, m.x - 4.0f, m.y - 4.0f);

    uint32_t count = br_input_touch_count(e);
    br_pen pen = br_input_pen(e);
    SDL_snprintf(buf, sizeof(buf), "fingers down: %u\npen: %s%s, pressure %.2f", count,
                 pen.in_proximity ? "in range" : "not in range", pen.down ? ", touching" : "", pen.pressure);
    br_text_set_text(e, s->touch_text, buf);
    for (uint32_t i = 0; i < MAX_TOUCH_DOTS; i++) {
        br_touch touch;
        bool down = br_input_touch_get(e, i, &touch);
        br_node_set_visible(e, s->touch_dots[i], down);
        if (down) {
            br_node_set_position(e, s->touch_dots[i], touch.position.x - 8.0f, touch.position.y - 8.0f);
        }
    }
}

static void update_pads(demo_state *s, br_engine *e)
{
    char buf[256];
    for (int slot = 0; slot < SLOTS_SHOWN; slot++) {
        pad_view *v = &s->pads[slot];
        br_gamepad_info info;
        bool connected = br_input_gamepad_info(e, slot, &info);
        if (!connected) {
            SDL_snprintf(buf, sizeof(buf), "Slot %d: empty", slot);
        } else {
            SDL_snprintf(buf, sizeof(buf), "Slot %d: %s\n%s, %s family%s%s", slot, info.name, type_name(info.type),
                         family_name(info.family), info.virtual_device ? ", SDL virtual" : "",
                         info.injected ? ", injected" : "");
        }
        br_text_set_text(e, v->header, buf);
        br_gamepad_type type = connected ? info.type : BR_GAMEPAD_TYPE_GENERIC;
        for (int b = 0; b < PAD_BUTTONS; b++) {
            br_input_binding binding = { BR_INPUT_BINDING_GAMEPAD_BUTTON, (int)g_pad_buttons[b], 1 };
            br_glyph glyph;
            br_input_binding_glyph(binding, type, &glyph);
            br_text_set_text(e, v->cell_labels[b], glyph.label);
            bool down = br_input_gamepad_down(e, slot, g_pad_buttons[b]);
            br_node_set_tint(e, v->cells[b], br_color_from_hex(down ? 0x40d080 : (connected ? 0x485068 : 0x343a4a), 1.0f));
        }
        for (int st = 0; st < 2; st++) {
            br_vec2 stick = br_input_gamepad_stick(e, slot, st == 0 ? BR_GAMEPAD_STICK_LEFT : BR_GAMEPAD_STICK_RIGHT);
            float cx = 20.0f + (float)st * 80.0f + 30.0f;
            br_node_set_position(e, v->stick_dot[st], cx + stick.x * 26.0f - 4.0f, 30.0f + stick.y * 26.0f - 4.0f);
            float trig = br_input_gamepad_axis(e, slot, st == 0 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            br_sprite_set_size(e, v->trigger_fill[st], 14.0f, 60.0f * trig + 0.01f);
            br_node_set_position(e, v->trigger_fill[st], 180.0f + (float)st * 24.0f, 60.0f - 60.0f * trig);
        }
        buf[0] = '\0';
        if (connected) {
            br_vec3 gyro = br_vec3_make(0.0f, 0.0f, 0.0f);
            br_vec2 finger = br_vec2_make(0.0f, 0.0f);
            bool has_gyro = info.has_gyro && br_input_gamepad_sensor(e, slot, SDL_SENSOR_GYRO, &gyro);
            bool touching = info.touchpads > 0 && br_input_gamepad_touchpad(e, slot, 0, 0, &finger, NULL);
            char battery[32];
            SDL_snprintf(battery, sizeof(battery), info.battery_percent >= 0 ? "%d%%" : "unknown", info.battery_percent);
            SDL_snprintf(buf, sizeof(buf), "battery %s, %s\ngyro %s%.2f %.2f %.2f\ntouchpad %s (%.2f, %.2f)", battery,
                         info.wireless ? "wireless" : "wired/unknown", has_gyro ? "" : "(none) ", gyro.x, gyro.y,
                         gyro.z, info.touchpads > 0 ? (touching ? "finger" : "idle") : "none", finger.x, finger.y);
        }
        br_text_set_text(e, v->extra, buf);
    }
}

static void update_actions_glyphs(demo_state *s, br_engine *e)
{
    for (int i = 0; i < ACTION_COUNT; i++) {
        float value = br_input_action_value(e, g_actions[i]);
        br_sprite_set_size(e, s->action_fill[i], 200.0f * value + 0.01f, 14.0f);
        br_node_set_tint(e, s->action_down[i], br_color_from_hex(br_input_action_down(e, g_actions[i]) ? 0x40d080 : 0x485068, 1.0f));
    }
    br_vec2 move = br_input_action_vec2(e, "left", "right", "up", "down");
    char buf[640];
    SDL_snprintf(buf, sizeof(buf), "move (vec2 composite): %.2f, %.2f   context: \"%s\"", move.x, move.y,
                 br_input_context_top(e));
    br_text_set_text(e, s->move_text, buf);

    br_glyph jump;
    br_glyph fire;
    br_input_glyph(e, "jump", &jump);
    br_input_glyph(e, "fire", &fire);
    if (SDL_strcmp(jump.label, s->last_label) != 0) {
        if (s->history[0] != '\0') {
            SDL_strlcat(s->history, "  ->  ", sizeof(s->history));
        }
        SDL_strlcat(s->history, jump.label, sizeof(s->history));
        br_str_copy(s->last_label, sizeof(s->last_label), jump.label);
    }
    char prompt[160];
    br_input_format_prompt(e, "Press {jump} to jump, hold {fire} to fire, {pause} to pause", prompt, sizeof(prompt));
    int pad = br_input_last_gamepad(e);
    char last[96];
    if (br_input_last_device(e) == BR_INPUT_DEVICE_GAMEPAD && pad >= 0) {
        SDL_snprintf(last, sizeof(last), "gamepad slot %d (%s)", pad, type_name(br_input_gamepad_type(e, pad)));
    } else {
        SDL_snprintf(last, sizeof(last), "%s", device_name(br_input_last_device(e)));
    }
    br_glyph fam[3];
    br_input_glyph_for(e, "jump", BR_INPUT_DEVICE_GAMEPAD, BR_GAMEPAD_TYPE_XBOXONE, &fam[0]);
    br_input_glyph_for(e, "jump", BR_INPUT_DEVICE_GAMEPAD, BR_GAMEPAD_TYPE_PS5, &fam[1]);
    br_input_glyph_for(e, "jump", BR_INPUT_DEVICE_GAMEPAD, BR_GAMEPAD_TYPE_SWITCH_PRO, &fam[2]);
    SDL_snprintf(buf, sizeof(buf),
                 "last device: %s\nprompt: %s\nsprite frames: jump = %s, fire = %s\nglyph history: %s\n"
                 "jump per family: %s (%s) | %s (%s) | %s (%s)",
                 last, prompt, jump.frame, fire.frame, s->history, fam[0].label, fam[0].frame, fam[1].label,
                 fam[1].frame, fam[2].label, fam[2].frame);
    br_text_set_text(e, s->glyph_text, buf);
}

static void view_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    update_keyboard_mouse(s, e);
    update_pads(s, e);
    update_actions_glyphs(s, e);
}

/* ---- setup */

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static void build_pad_view(demo_state *s, br_engine *e, int slot)
{
    br_node root = br_scene_root(e);
    float x0 = 20.0f + (float)slot * 312.0f;
    float y0 = 262.0f;
    make_rect(s, e, root, x0, y0, 300, 262, 0x2a3040);
    pad_view *v = &s->pads[slot];
    v->header = make_text(e, root, x0 + 10, y0 + 6, 14, 0x9fb4d8, "");
    for (int b = 0; b < PAD_BUTTONS; b++) {
        float x = x0 + 10.0f + (float)(b % 5) * 57.0f;
        float y = y0 + 50.0f + (float)(b / 5) * 28.0f;
        v->cells[b] = make_rect(s, e, root, x, y, 53, 24, 0x343a4a);
        v->cell_labels[b] = make_text(e, root, x + 4, y + 4, 12, 0xffffff, "");
    }
    br_node sticks = br_node_create_container(e);
    br_node_set_position(e, sticks, x0, y0 + 136);
    br_node_add_child(e, root, sticks);
    for (int st = 0; st < 2; st++) {
        make_rect(s, e, sticks, 20.0f + (float)st * 80.0f, 0, 60, 60, 0x3a4256);
        v->stick_dot[st] = make_rect(s, e, sticks, 0, 0, 8, 8, 0xffd040);
        make_rect(s, e, sticks, 180.0f + (float)st * 24.0f, 0, 14, 60, 0x3a4256);
        v->trigger_fill[st] = make_rect(s, e, sticks, 0, 0, 14, 1, 0xff8040);
    }
    make_text(e, sticks, 230, 4, 11, 0x9fb4d8, "L/R stick\nL/R trigger");
    v->extra = make_text(e, root, x0 + 10, y0 + 204, 12, 0xd0d8e8, "");
}

static void bind_actions(br_engine *e)
{
    cJSON *json = cJSON_Parse(
        "{ \"jump\":   { \"keys\": [\"Space\"], \"buttons\": [\"a\"], \"context\": \"gameplay\" },"
        "  \"fire\":   { \"mouse\": [1], \"axes\": [\"+righttrigger\"], \"threshold\": 0.3 },"
        "  \"sprint\": { \"keys\": [\"Left Shift\"], \"buttons\": [\"rightshoulder\"] },"
        "  \"pause\":  { \"keys\": [\"Escape\"], \"buttons\": [\"start\"] },"
        "  \"left\":   { \"keys\": [\"A\"], \"axes\": [\"-leftx\"], \"buttons\": [\"dpleft\"] },"
        "  \"right\":  { \"keys\": [\"D\"], \"axes\": [\"+leftx\"], \"buttons\": [\"dpright\"] },"
        "  \"up\":     { \"keys\": [\"W\"], \"axes\": [\"-lefty\"], \"buttons\": [\"dpup\"] },"
        "  \"down\":   { \"keys\": [\"S\"], \"axes\": [\"+lefty\"], \"buttons\": [\"dpdown\"] } }");
    br_input_actions_from_json(e, json);
    cJSON_Delete(json);
    br_input_context_push(e, "gameplay");
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    s->white = br_texture_white(e);
    br_node root = br_scene_root(e);
    make_rect(s, e, root, 0, 0, 1280, 720, 0x1c2030);
    make_text(e, root, 20, 10, 22, 0xffffff, "Braty input: every device, pad families, glyphs");

    make_panel(s, e, 20, 48, 400, 200, "Keyboard + text");
    s->keyboard_text = make_text(e, root, 30, 76, 14, 0xd0d8e8, "");
    make_panel(s, e, 440, 48, 400, 200, "Mouse");
    s->mouse_text = make_text(e, root, 450, 76, 14, 0xd0d8e8, "");
    const char *button_names[3] = { "LMB", "MMB", "RMB" };
    for (int i = 0; i < 3; i++) {
        s->mouse_buttons[i] = make_rect(s, e, root, 450.0f + (float)i * 70.0f, 200, 60, 30, 0x485068);
        make_text(e, root, 462.0f + (float)i * 70.0f, 206, 14, 0xffffff, button_names[i]);
    }
    make_panel(s, e, 860, 48, 400, 200, "Touch + pen");
    s->touch_text = make_text(e, root, 870, 76, 14, 0xd0d8e8, "");
    for (int i = 0; i < SLOTS_SHOWN; i++) {
        build_pad_view(s, e, i);
    }
    make_panel(s, e, 20, 540, 420, 170, "Actions");
    for (int i = 0; i < ACTION_COUNT; i++) {
        float y = 566.0f + (float)i * 15.0f;
        make_text(e, root, 30, y - 1, 12, 0xd0d8e8, g_actions[i]);
        make_rect(s, e, root, 100, y, 200, 12, 0x3a4256);
        s->action_fill[i] = make_rect(s, e, root, 100, y, 1, 12, 0x6090ff);
        s->action_down[i] = make_rect(s, e, root, 310, y, 12, 12, 0x485068);
    }
    s->move_text = make_text(e, root, 330, 566, 12, 0xd0d8e8, "");
    br_node_set_position(e, s->move_text, 30, 690);
    make_panel(s, e, 460, 540, 800, 170, "Last device and glyphs");
    s->glyph_text = make_text(e, root, 470, 566, 14, 0xd0d8e8, "");
    for (int i = 0; i < MAX_TOUCH_DOTS; i++) {
        s->touch_dots[i] = make_rect(s, e, root, 0, 0, 16, 16, 0xff60a0);
        br_node_set_visible(e, s->touch_dots[i], false);
    }
    s->cursor_marker = make_rect(s, e, root, 0, 0, 8, 8, 0xffd040);

    bind_actions(e);
    br_system_add(e, "demo.view", BR_PHASE_UPDATE, 0, view_system, s);
    if (g_scripted) {
        br_system_add(e, "demo.script", BR_PHASE_INPUT, BR_INPUT_PRIORITY_LATCH - 1000, script_system, s);
    }
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--frames") == 0) {
            g_scripted = true;
        }
    }
    if (g_scripted) {
        /* reproducible screenshot: only the scripted virtual pads, never a pad on the desk */
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        /* the --frames window is usually not focused; SDL ignores pad state of unfocused apps */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - input";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
