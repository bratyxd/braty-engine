/* br_input_glyph: button prompts - labels and sprite-frame names per device and pad family. */
#include "events/br_input_internal.h"

/* Printed names of the non-face buttons, per family: xbox, playstation, nintendo. */
typedef struct br_glyph_names {
    int         button;
    const char *xbox;
    const char *ps;
    const char *nintendo;
} br_glyph_names;

static const br_glyph_names br_glyph_buttons[] = {
    { SDL_GAMEPAD_BUTTON_BACK, "View", "Share", "-" },
    { SDL_GAMEPAD_BUTTON_GUIDE, "Xbox", "PS", "Home" },
    { SDL_GAMEPAD_BUTTON_START, "Menu", "Options", "+" },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, "LS", "L3", "LS" },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RS", "R3", "RS" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB", "L1", "L" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB", "R1", "R" },
    { SDL_GAMEPAD_BUTTON_DPAD_UP, "D-Pad Up", "Up", "Up" },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-Pad Down", "Down", "Down" },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-Pad Left", "Left", "Left" },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-Pad Right", "Right", "Right" },
    { SDL_GAMEPAD_BUTTON_MISC1, "Share", "Mic", "Capture" },
    { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "P1", "P1", "P1" },
    { SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "P3", "P3", "P3" },
    { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "P2", "P2", "P2" },
    { SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "P4", "P4", "P4" },
    { SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad", "Touchpad", "Touchpad" },
};

static const br_glyph_names br_glyph_axes[] = {
    { SDL_GAMEPAD_AXIS_LEFTX, "Left Stick", "Left Stick", "Left Stick" },
    { SDL_GAMEPAD_AXIS_LEFTY, "Left Stick", "Left Stick", "Left Stick" },
    { SDL_GAMEPAD_AXIS_RIGHTX, "Right Stick", "Right Stick", "Right Stick" },
    { SDL_GAMEPAD_AXIS_RIGHTY, "Right Stick", "Right Stick", "Right Stick" },
    { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "LT", "L2", "ZL" },
    { SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "RT", "R2", "ZR" },
};

static const char *br_glyph_pick(const br_glyph_names *n, br_gamepad_family family)
{
    if (family == BR_GAMEPAD_FAMILY_PLAYSTATION) {
        return n->ps;
    }
    if (family == BR_GAMEPAD_FAMILY_NINTENDO) {
        return n->nintendo;
    }
    return n->xbox;
}

static const char *br_glyph_set(br_gamepad_family family)
{
    if (family == BR_GAMEPAD_FAMILY_PLAYSTATION) {
        return "ps";
    }
    if (family == BR_GAMEPAD_FAMILY_NINTENDO) {
        return "switch";
    }
    return "xbox";
}

/* "Left Shift" -> "left_shift". */
static void br_glyph_frame_name(char *out, size_t size, const char *prefix, const char *name)
{
    SDL_snprintf(out, size, "%s%s", prefix, name);
    for (char *c = out; *c != '\0'; c++) {
        if (*c == ' ') {
            *c = '_';
        } else if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
}

static const char *br_glyph_face_label(SDL_GamepadButtonLabel label)
{
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
    default: return NULL;
    }
}

bool br_input_binding_glyph(br_input_binding b, br_gamepad_type type, br_glyph *out)
{
    SDL_zerop(out);
    out->binding = b;
    out->family = br_input_gamepad_family_of(type);
    br_gamepad_family family = out->family;
    char part[40];
    switch (b.kind) {
    case BR_INPUT_BINDING_KEY: {
        out->device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
        out->family = BR_GAMEPAD_FAMILY_GENERIC;
        const char *stable = SDL_GetScancodeName((SDL_Scancode)b.code);
        if (stable == NULL || stable[0] == '\0' || !br_input_key_name((SDL_Scancode)b.code, out->label, sizeof(out->label))) {
            return false;
        }
        br_glyph_frame_name(out->frame, sizeof(out->frame), "key_", stable);
        return true;
    }
    case BR_INPUT_BINDING_MOUSE: {
        static const char *labels[] = { "", "LMB", "MMB", "RMB", "Mouse 4", "Mouse 5" };
        static const char *frames[] = { "", "mouse_left", "mouse_middle", "mouse_right", "mouse_x1", "mouse_x2" };
        out->device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
        out->family = BR_GAMEPAD_FAMILY_GENERIC;
        if (b.code <= 0 || b.code >= (int)BR_ARRAY_COUNT(labels)) {
            return false;
        }
        br_str_copy(out->label, sizeof(out->label), labels[b.code]);
        br_str_copy(out->frame, sizeof(out->frame), frames[b.code]);
        return true;
    }
    case BR_INPUT_BINDING_GAMEPAD_BUTTON: {
        out->device = BR_INPUT_DEVICE_GAMEPAD;
        /* face buttons: what SDL says is printed there for this pad type (Switch south = "B") */
        const char *face = br_glyph_face_label(
            SDL_GetGamepadButtonLabelForType(br_input_gamepad_type_to_sdl(type), (SDL_GamepadButton)b.code));
        if (b.code <= SDL_GAMEPAD_BUTTON_NORTH && face != NULL) {
            br_str_copy(out->label, sizeof(out->label), face);
            br_glyph_frame_name(part, sizeof(part), "", face);
        } else {
            const char *sdl_name = SDL_GetGamepadStringForButton((SDL_GamepadButton)b.code);
            if (sdl_name == NULL) {
                return false;
            }
            br_str_copy(part, sizeof(part), sdl_name);
            br_str_copy(out->label, sizeof(out->label), sdl_name);
            for (uint32_t i = 0; i < BR_ARRAY_COUNT(br_glyph_buttons); i++) {
                if (br_glyph_buttons[i].button == b.code) {
                    br_str_copy(out->label, sizeof(out->label), br_glyph_pick(&br_glyph_buttons[i], family));
                }
            }
            if (b.code == SDL_GAMEPAD_BUTTON_BACK && type == BR_GAMEPAD_TYPE_PS5) {
                br_str_copy(out->label, sizeof(out->label), "Create");
            } else if (b.code == SDL_GAMEPAD_BUTTON_BACK && type == BR_GAMEPAD_TYPE_PS3) {
                br_str_copy(out->label, sizeof(out->label), "Select");
            } else if (b.code == SDL_GAMEPAD_BUTTON_START && type == BR_GAMEPAD_TYPE_PS3) {
                br_str_copy(out->label, sizeof(out->label), "Start");
            }
        }
        SDL_snprintf(out->frame, sizeof(out->frame), "%s_%s", br_glyph_set(family), part);
        return true;
    }
    case BR_INPUT_BINDING_GAMEPAD_AXIS: {
        out->device = BR_INPUT_DEVICE_GAMEPAD;
        if (b.code <= SDL_GAMEPAD_AXIS_INVALID || b.code >= SDL_GAMEPAD_AXIS_COUNT) {
            return false;
        }
        static const char *parts[] = { "leftstick", "leftstick", "rightstick", "rightstick", "lefttrigger", "righttrigger" };
        br_str_copy(out->label, sizeof(out->label), br_glyph_pick(&br_glyph_axes[b.code], family));
        SDL_snprintf(out->frame, sizeof(out->frame), "%s_%s", br_glyph_set(family), parts[b.code]);
        return true;
    }
    }
    return false;
}

static bool br_glyph_matches(br_input_binding b, br_input_device device)
{
    bool pad = b.kind == BR_INPUT_BINDING_GAMEPAD_BUTTON || b.kind == BR_INPUT_BINDING_GAMEPAD_AXIS;
    return device == BR_INPUT_DEVICE_GAMEPAD ? pad : !pad;
}

bool br_input_glyph_for(br_engine *e, const char *action, br_input_device device, br_gamepad_type type, br_glyph *out)
{
    br_action *a = br_input_find_action(e->input, action);
    if (a == NULL || a->bindings.count == 0 || out == NULL) {
        return false;
    }
    if (device != BR_INPUT_DEVICE_GAMEPAD) {
        device = BR_INPUT_DEVICE_KEYBOARD_MOUSE;
    }
    for (uint32_t i = 0; i < a->bindings.count; i++) {
        br_input_binding b = *(br_input_binding *)br_array_at(&a->bindings, i);
        if (br_glyph_matches(b, device) && br_input_binding_glyph(b, type, out)) {
            return true;
        }
    }
    /* no binding for that device: show what the action has */
    return br_input_binding_glyph(*(br_input_binding *)br_array_at(&a->bindings, 0), type, out);
}

bool br_input_glyph(br_engine *e, const char *action, br_glyph *out)
{
    br_input_state *s = e->input;
    br_input_device device = s->last_device == BR_INPUT_DEVICE_GAMEPAD ? BR_INPUT_DEVICE_GAMEPAD
                                                                       : BR_INPUT_DEVICE_KEYBOARD_MOUSE;
    br_gamepad_type type = s->last_gamepad >= 0 ? br_input_gamepad_type(e, s->last_gamepad) : BR_GAMEPAD_TYPE_GENERIC;
    return br_input_glyph_for(e, action, device, type, out);
}

bool br_input_format_prompt(br_engine *e, const char *text, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    size_t n = 0;
    bool fits = true;
    out[0] = '\0';
    for (const char *p = text != NULL ? text : ""; *p != '\0';) {
        const char *piece = p;
        size_t piece_len = 1;
        br_glyph glyph;
        const char *close = *p == '{' ? SDL_strchr(p, '}') : NULL;
        if (close != NULL && close - p - 1 > 0 && close - p - 1 < 64) {
            char name[64];
            SDL_memcpy(name, p + 1, (size_t)(close - p - 1));
            name[close - p - 1] = '\0';
            if (br_input_glyph(e, name, &glyph)) {
                piece = glyph.label;
                piece_len = SDL_strlen(glyph.label);
            } else {
                piece_len = (size_t)(close - p + 1);   /* unknown action: copy "{name}" as written */
            }
            p = close + 1;
        } else {
            p++;
        }
        if (n + piece_len >= out_size) {
            piece_len = out_size - 1 - n;
            fits = false;
        }
        SDL_memcpy(out + n, piece, piece_len);
        n += piece_len;
        out[n] = '\0';
        if (!fits) {
            break;
        }
    }
    return fits;
}
