/* br_input_action: named actions, bindings (JSON, save data, rebinding), contexts, composites. */
#include "events/br_input_internal.h"

#define BR_INPUT_ALL_DEVICES (-2)
#define BR_INPUT_KEYBOARD_MOUSE (-1)

br_action *br_input_find_action(br_input_state *s, const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        if (br_str_equal(a->name, name)) {
            return a;
        }
    }
    return NULL;
}

static br_action *br_input_get_or_add_action(br_input_state *s, const char *name)
{
    br_action *a = br_input_find_action(s, name);
    if (a != NULL || name == NULL) {
        return a;
    }
    br_action fresh;
    SDL_zero(fresh);
    fresh.name = br_strdup(name);
    fresh.threshold = 0.5f;
    fresh.last_gamepad = -1;
    br_array_init(&fresh.bindings, sizeof(br_input_binding));
    a = br_array_push(&s->actions, &fresh);
    if (a == NULL) {
        br_free(fresh.name);
    }
    return a;
}

/* ---- values */

/* Value of one binding. player: BR_INPUT_ALL_DEVICES, BR_INPUT_KEYBOARD_MOUSE or a pad slot.
 * *pad_out = the pad slot that gave the value (unchanged for keys and mouse). */
static float br_input_binding_value(br_input_state *s, const br_input_binding *b, int player, int *pad_out)
{
    float best = 0.0f;
    if (b->kind == BR_INPUT_BINDING_KEY || b->kind == BR_INPUT_BINDING_MOUSE) {
        if (player >= 0) {
            return 0.0f;
        }
        if (b->kind == BR_INPUT_BINDING_KEY) {
            if (s->keyboard_captured) {
                return 0.0f;
            }
            return s->keys[b->code] || (s->key_frame_edges[b->code] & BR_EDGE_PRESSED) ? 1.0f : 0.0f;
        }
        return s->mouse_buttons[b->code] || (s->mouse_frame_edges[b->code] & BR_EDGE_PRESSED) ? 1.0f : 0.0f;
    }
    if (player == BR_INPUT_KEYBOARD_MOUSE) {
        return 0.0f;
    }
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        br_gamepad_slot *pad = &s->gamepads[i];
        if (pad->id == 0 || (player >= 0 && player != i)) {
            continue;
        }
        float v;
        if (b->kind == BR_INPUT_BINDING_GAMEPAD_BUTTON) {
            v = pad->down[b->code] || (pad->frame_edges[b->code] & BR_EDGE_PRESSED) ? 1.0f : 0.0f;
        } else {
            v = br_input_pad_axis(s, pad, b->code) * (float)b->direction;
        }
        if (v > best) {
            best = v;
            *pad_out = i;
        }
    }
    return best;
}

static float br_input_action_raw(br_input_state *s, const br_action *a, int player, int *pad_out)
{
    float value = 0.0f;
    for (uint32_t j = 0; j < a->bindings.count; j++) {
        int pad = -1;
        float v = br_input_binding_value(s, (const br_input_binding *)a->bindings.data + j, player, &pad);
        if (v > value) {
            value = v;
            *pad_out = pad;
        }
    }
    return value;
}

static bool br_input_action_live(const br_input_state *s, const br_action *a)
{
    if (s->capture_device != BR_INPUT_DEVICE_NONE) {
        return false;
    }
    if (a->context == NULL) {
        return true;
    }
    return s->context_count > 0 && br_str_equal(a->context, s->contexts[s->context_count - 1]);
}

void br_input_update_actions(br_engine *e)
{
    br_input_state *s = e->input;
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        int pad = -1;
        float raw = br_input_action_raw(s, a, BR_INPUT_ALL_DEVICES, &pad);
        if (a->inject_frames > 0) {
            if (a->inject_value > raw) {
                raw = a->inject_value;
                pad = -1;
            }
            a->inject_frames--;
        }
        a->was_down = a->down;
        if (!br_input_action_live(s, a)) {
            /* inactive: idle, and once live again it waits for the input to be released */
            a->blocked = true;
            a->value = 0.0f;
        } else {
            if (a->blocked && raw < a->threshold) {
                a->blocked = false;
            }
            a->value = a->blocked ? 0.0f : raw;
        }
        a->down = a->value > 0.0f && a->value >= a->threshold;
        if (a->value > 0.0f && pad >= 0) {
            a->last_gamepad = pad;
        }
    }
}

void br_input_block_held_keys(br_input_state *s)
{
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        for (uint32_t j = 0; j < a->bindings.count; j++) {
            const br_input_binding *b = (const br_input_binding *)a->bindings.data + j;
            if (b->kind == BR_INPUT_BINDING_KEY && (s->keys[b->code] || (s->key_edges[b->code] & BR_EDGE_PRESSED))) {
                a->blocked = true;
            }
        }
    }
}

void br_input_actions_free(br_input_state *s)
{
    br_input_actions_clear_state(s);
    br_array_free(&s->actions);
}

/* ---- binding */

static bool br_input_binding_valid(br_input_binding b)
{
    switch (b.kind) {
    case BR_INPUT_BINDING_KEY:
        return b.code > SDL_SCANCODE_UNKNOWN && b.code < SDL_SCANCODE_COUNT;
    case BR_INPUT_BINDING_MOUSE:
        return b.code > 0 && b.code < BR_INPUT_MOUSE_BUTTONS;
    case BR_INPUT_BINDING_GAMEPAD_BUTTON:
        return b.code > SDL_GAMEPAD_BUTTON_INVALID && b.code < SDL_GAMEPAD_BUTTON_COUNT;
    case BR_INPUT_BINDING_GAMEPAD_AXIS:
        return b.code > SDL_GAMEPAD_AXIS_INVALID && b.code < SDL_GAMEPAD_AXIS_COUNT &&
               (b.direction == 1 || b.direction == -1);
    }
    return false;
}

static bool br_input_binding_equal(br_input_binding a, br_input_binding b)
{
    return a.kind == b.kind && a.code == b.code && (a.kind != BR_INPUT_BINDING_GAMEPAD_AXIS || a.direction == b.direction);
}

void br_input_action_bind(br_engine *e, const char *action, br_input_binding binding)
{
    if (binding.kind != BR_INPUT_BINDING_GAMEPAD_AXIS) {
        binding.direction = 1;
    }
    if (!br_input_binding_valid(binding)) {
        br_log_error("input: action '%s': bad binding (kind %d, code %d)", action, (int)binding.kind, binding.code);
        return;
    }
    br_action *a = br_input_get_or_add_action(e->input, action);
    if (a != NULL) {
        br_array_push(&a->bindings, &binding);
    }
}

void br_input_action_bind_key(br_engine *e, const char *action, SDL_Scancode key)
{
    br_input_binding b = { BR_INPUT_BINDING_KEY, (int)key, 1 };
    br_input_action_bind(e, action, b);
}

void br_input_action_bind_mouse(br_engine *e, const char *action, int button)
{
    br_input_binding b = { BR_INPUT_BINDING_MOUSE, button, 1 };
    br_input_action_bind(e, action, b);
}

void br_input_action_bind_gamepad_button(br_engine *e, const char *action, SDL_GamepadButton button)
{
    br_input_binding b = { BR_INPUT_BINDING_GAMEPAD_BUTTON, (int)button, 1 };
    br_input_action_bind(e, action, b);
}

void br_input_action_bind_gamepad_axis(br_engine *e, const char *action, SDL_GamepadAxis axis, int direction)
{
    br_input_binding b = { BR_INPUT_BINDING_GAMEPAD_AXIS, (int)axis, direction < 0 ? -1 : 1 };
    br_input_action_bind(e, action, b);
}

uint32_t br_input_action_binding_count(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL ? a->bindings.count : 0;
}

bool br_input_action_binding_get(br_engine *e, const char *action, uint32_t i, br_input_binding *out)
{
    br_action *a = br_input_find_action(e->input, action);
    br_input_binding *b = a != NULL ? br_array_at(&a->bindings, i) : NULL;
    if (b == NULL) {
        return false;
    }
    *out = *b;
    return true;
}

bool br_input_action_binding_set(br_engine *e, const char *action, uint32_t i, br_input_binding binding)
{
    br_action *a = br_input_find_action(e->input, action);
    br_input_binding *b = a != NULL ? br_array_at(&a->bindings, i) : NULL;
    if (binding.kind != BR_INPUT_BINDING_GAMEPAD_AXIS) {
        binding.direction = 1;
    }
    if (b == NULL || !br_input_binding_valid(binding)) {
        br_log_error("input: action '%s': cannot set binding %u", action, i);
        return false;
    }
    *b = binding;
    return true;
}

bool br_input_action_binding_remove(br_engine *e, const char *action, uint32_t i)
{
    br_action *a = br_input_find_action(e->input, action);
    if (a == NULL || i >= a->bindings.count) {
        return false;
    }
    br_array_remove_ordered(&a->bindings, i);
    return true;
}

const char *br_input_binding_conflict(br_engine *e, br_input_binding binding, const char *except_action)
{
    br_input_state *s = e->input;
    br_action *except = br_input_find_action(s, except_action);
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        if (a == except) {
            continue;
        }
        /* actions in different contexts never fire together, so they may share an input */
        if (except != NULL && !br_str_equal(a->context, except->context) && a->context != NULL &&
            except->context != NULL) {
            continue;
        }
        for (uint32_t j = 0; j < a->bindings.count; j++) {
            if (br_input_binding_equal(*(br_input_binding *)br_array_at(&a->bindings, j), binding)) {
                return a->name;
            }
        }
    }
    return NULL;
}

void br_input_action_set_threshold(br_engine *e, const char *action, float threshold)
{
    br_action *a = br_input_get_or_add_action(e->input, action);
    if (a != NULL) {
        a->threshold = br_clampf(threshold, 0.01f, 1.0f);
    }
}

void br_input_action_set_context(br_engine *e, const char *action, const char *context)
{
    br_action *a = br_input_get_or_add_action(e->input, action);
    if (a == NULL) {
        return;
    }
    br_free(a->context);
    a->context = context != NULL && context[0] != '\0' ? br_strdup(context) : NULL;
}

void br_input_actions_clear_state(br_input_state *s)
{
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        br_free(a->name);
        br_free(a->context);
        br_array_free(&a->bindings);
    }
    br_array_clear(&s->actions);
}

void br_input_actions_clear(br_engine *e)
{
    br_input_actions_clear_state(e->input);
}

/* ---- JSON */

/* Adds one action's bindings from its JSON object. */
static void br_input_action_from_json(br_engine *e, const char *name, const cJSON *action)
{
    if (!cJSON_IsObject(action)) {
        br_log_error("input: action '%s' must be an object", name);
        return;
    }
    br_input_get_or_add_action(e->input, name);
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
    const cJSON *threshold = cJSON_GetObjectItemCaseSensitive(action, "threshold");
    if (cJSON_IsNumber(threshold)) {
        br_input_action_set_threshold(e, name, (float)threshold->valuedouble);
    }
    const cJSON *context = cJSON_GetObjectItemCaseSensitive(action, "context");
    if (cJSON_IsString(context)) {
        br_input_action_set_context(e, name, context->valuestring);
    }
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
        br_input_action_from_json(e, action->string, action);
    }
    return true;
}

static void br_input_add_string(cJSON *obj, const char *array_name, const char *value)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(obj, array_name);
    if (array == NULL) {
        array = cJSON_AddArrayToObject(obj, array_name);
    }
    cJSON_AddItemToArray(array, cJSON_CreateString(value));
}

cJSON *br_input_actions_to_json(br_engine *e)
{
    br_input_state *s = e->input;
    cJSON *root = cJSON_CreateObject();
    for (uint32_t i = 0; i < s->actions.count; i++) {
        br_action *a = (br_action *)s->actions.data + i;
        cJSON *obj = cJSON_AddObjectToObject(root, a->name);
        for (uint32_t j = 0; j < a->bindings.count; j++) {
            br_input_binding *b = br_array_at(&a->bindings, j);
            char axis[48];
            switch (b->kind) {
            case BR_INPUT_BINDING_KEY:
                br_input_add_string(obj, "keys", SDL_GetScancodeName((SDL_Scancode)b->code));
                break;
            case BR_INPUT_BINDING_MOUSE: {
                cJSON *mouse = cJSON_GetObjectItemCaseSensitive(obj, "mouse");
                if (mouse == NULL) {
                    mouse = cJSON_AddArrayToObject(obj, "mouse");
                }
                cJSON_AddItemToArray(mouse, cJSON_CreateNumber(b->code));
                break;
            }
            case BR_INPUT_BINDING_GAMEPAD_BUTTON:
                br_input_add_string(obj, "buttons", SDL_GetGamepadStringForButton((SDL_GamepadButton)b->code));
                break;
            case BR_INPUT_BINDING_GAMEPAD_AXIS:
                SDL_snprintf(axis, sizeof(axis), "%c%s", b->direction < 0 ? '-' : '+',
                             SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b->code));
                br_input_add_string(obj, "axes", axis);
                break;
            }
        }
        if (a->threshold != 0.5f) {
            cJSON_AddNumberToObject(obj, "threshold", SDL_round((double)a->threshold * 1000.0) / 1000.0);
        }
        if (a->context != NULL) {
            cJSON_AddStringToObject(obj, "context", a->context);
        }
    }
    return root;
}

bool br_input_bindings_save(br_engine *e, const char *save_path)
{
    cJSON *json = br_input_actions_to_json(e);
    char *text = cJSON_Print(json);
    cJSON_Delete(json);
    if (text == NULL) {
        br_log_error("input: bindings save: out of memory");
        return false;
    }
    bool ok = br_save_write(e, save_path, text, SDL_strlen(text));
    cJSON_free(text);
    return ok;
}

/* Number of binding entries (keys, mouse, buttons, axes) one saved action lists. */
static int br_input_saved_entry_count(const cJSON *action)
{
    const char *lists[4] = { "keys", "mouse", "buttons", "axes" };
    int count = 0;
    for (int i = 0; i < 4; i++) {
        count += cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(action, lists[i]));
    }
    return count;
}

bool br_input_bindings_load(br_engine *e, const char *save_path)
{
    void *bytes = NULL;
    size_t size = 0;
    if (!br_save_read(e, save_path, &bytes, &size)) {
        return false;
    }
    cJSON *json = cJSON_ParseWithLength(bytes, size);
    br_free(bytes);
    if (!cJSON_IsObject(json)) {
        br_log_error("input: bindings '%s' are not a JSON object", save_path);
        cJSON_Delete(json);
        return false;
    }
    const cJSON *action = NULL;
    cJSON_ArrayForEach(action, json)
    {
        br_action *a = br_input_find_action(e->input, action->string);
        if (a == NULL) {
            br_input_action_from_json(e, action->string, action);
            continue;
        }
        br_array old = a->bindings;
        br_array_init(&a->bindings, sizeof(br_input_binding));
        br_input_action_from_json(e, action->string, action);
        a = br_input_find_action(e->input, action->string);
        /* entries that no longer parse (renamed keys, a hand-edited file) must not unbind it */
        if (a->bindings.count == 0 && br_input_saved_entry_count(action) > 0) {
            br_log_warn("input: bindings '%s': none of '%s' could be read; keeping its current bindings",
                        save_path, action->string);
            br_array_free(&a->bindings);
            a->bindings = old;
        } else {
            br_array_free(&old);
        }
    }
    cJSON_Delete(json);
    return true;
}

/* ---- queries */

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

float br_input_action_value_for(br_engine *e, const char *action, int player)
{
    br_input_state *s = e->input;
    br_action *a = br_input_find_action(s, action);
    if (a == NULL || player < BR_INPUT_KEYBOARD_MOUSE || player >= BR_INPUT_MAX_GAMEPADS || a->blocked ||
        !br_input_action_live(s, a)) {
        return 0.0f;
    }
    int pad = -1;
    return br_input_action_raw(s, a, player, &pad);
}

bool br_input_action_down_for(br_engine *e, const char *action, int player)
{
    br_action *a = br_input_find_action(e->input, action);
    float v = br_input_action_value_for(e, action, player);
    return a != NULL && v > 0.0f && v >= a->threshold;
}

int br_input_action_last_gamepad(br_engine *e, const char *action)
{
    br_action *a = br_input_find_action(e->input, action);
    return a != NULL ? a->last_gamepad : -1;
}

float br_input_action_axis(br_engine *e, const char *negative, const char *positive)
{
    return br_input_action_value(e, positive) - br_input_action_value(e, negative);
}

br_vec2 br_input_action_vec2(br_engine *e, const char *left, const char *right, const char *up, const char *down)
{
    br_vec2 v = br_vec2_make(br_input_action_axis(e, left, right), br_input_action_axis(e, up, down));
    float len = br_vec2_length(v);
    return len > 1.0f ? br_vec2_scale(v, 1.0f / len) : v;
}

/* ---- injection */

bool br_input_inject_action(br_engine *e, const char *action, float value, uint32_t frames)
{
    br_action *a = br_input_find_action(e->input, action);
    if (a == NULL) {
        br_log_error("input: inject: unknown action '%s'", action != NULL ? action : "(null)");
        return false;
    }
    a->inject_value = br_clampf(value, 0.0f, 1.0f);
    a->inject_frames = frames;
    return true;
}

/* ---- contexts */

void br_input_context_push(br_engine *e, const char *context)
{
    br_input_state *s = e->input;
    if (context == NULL || s->context_count == BR_INPUT_CONTEXT_DEPTH) {
        br_log_error("input: context stack full (%d) or NULL context", BR_INPUT_CONTEXT_DEPTH);
        return;
    }
    if (!br_str_copy(s->contexts[s->context_count], BR_INPUT_CONTEXT_NAME, context)) {
        br_log_warn("input: context name '%s' truncated", context);
    }
    s->context_count++;
}

void br_input_context_pop(br_engine *e)
{
    br_input_state *s = e->input;
    if (s->context_count == 0) {
        br_log_warn("input: context pop on an empty stack");
        return;
    }
    s->context_count--;
}

const char *br_input_context_top(br_engine *e)
{
    br_input_state *s = e->input;
    return s->context_count > 0 ? s->contexts[s->context_count - 1] : "";
}

/* ---- capture (rebinding) */

void br_input_capture_start(br_engine *e, br_input_device device)
{
    br_input_state *s = e->input;
    if (device != BR_INPUT_DEVICE_KEYBOARD_MOUSE && device != BR_INPUT_DEVICE_GAMEPAD) {
        br_log_error("input: capture needs KEYBOARD_MOUSE or GAMEPAD");
        return;
    }
    s->capture_device = device;
    s->capture_done = false;
}

void br_input_capture_cancel(br_engine *e)
{
    e->input->capture_device = BR_INPUT_DEVICE_NONE;
    e->input->capture_done = false;
}

bool br_input_capture_poll(br_engine *e, br_input_binding *out)
{
    br_input_state *s = e->input;
    if (!s->capture_done) {
        return false;
    }
    s->capture_done = false;
    if (out != NULL) {
        *out = s->capture_result;
    }
    return true;
}

static void br_input_capture_found(br_input_state *s, br_input_binding_kind kind, int code, int direction)
{
    s->capture_result.kind = kind;
    s->capture_result.code = code;
    s->capture_result.direction = direction;
    s->capture_done = true;
    s->capture_device = BR_INPUT_DEVICE_NONE;
    /* the captured press must not also fire whatever it was bound to */
    for (uint32_t i = 0; i < s->actions.count; i++) {
        ((br_action *)s->actions.data + i)->blocked = true;
    }
}

void br_input_capture_update(br_input_state *s)
{
    if (s->capture_device == BR_INPUT_DEVICE_KEYBOARD_MOUSE) {
        for (int k = 1; k < SDL_SCANCODE_COUNT; k++) {
            if (s->key_frame_edges[k] & BR_EDGE_PRESSED) {
                br_input_capture_found(s, BR_INPUT_BINDING_KEY, k, 1);
                return;
            }
        }
        for (int b = 1; b < BR_INPUT_MOUSE_BUTTONS; b++) {
            if (s->mouse_frame_edges[b] & BR_EDGE_PRESSED) {
                br_input_capture_found(s, BR_INPUT_BINDING_MOUSE, b, 1);
                return;
            }
        }
    } else if (s->capture_device == BR_INPUT_DEVICE_GAMEPAD) {
        for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
            br_gamepad_slot *pad = &s->gamepads[i];
            if (pad->id == 0) {
                continue;
            }
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
                if (pad->frame_edges[b] & BR_EDGE_PRESSED) {
                    br_input_capture_found(s, BR_INPUT_BINDING_GAMEPAD_BUTTON, b, 1);
                    return;
                }
            }
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
                if (pad->axes[a] > 0.5f || pad->axes[a] < -0.5f) {
                    br_input_capture_found(s, BR_INPUT_BINDING_GAMEPAD_AXIS, a, pad->axes[a] < 0.0f ? -1 : 1);
                    return;
                }
            }
        }
    }
}
