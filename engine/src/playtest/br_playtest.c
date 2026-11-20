/* br_playtest: script runner, command execution, input injection, record and replay.
 * Queries (paths, get/set, dump, conditions) are in br_playtest_query.c, stdin in
 * br_playtest_stdin.c. */
#include "playtest/br_playtest_internal.h"

#include "data/br_data_internal.h"
#include "profiler/br_profiler_internal.h"

#include <stdarg.h>

/* Runs before input.begin_frame (-2000000), so injected input counts in the same frame. */
#define BR_PLAYTEST_PRIORITY (-3000000)
/* wait_until gives up after this many frames unless the command says otherwise. */
#define BR_PLAYTEST_DEFAULT_TIMEOUT 600

typedef enum br_pt_result {
    BR_PT_DONE,
    BR_PT_FAILED,
    BR_PT_WAIT
} br_pt_result;

/* ---- small helpers */

bool br_playtest_fail(br_engine *e, const char *fmt, ...)
{
    br_playtest_state *s = e->playtest;
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(s->error, sizeof(s->error), fmt, args);
    va_end(args);
    br_log_error("playtest: %s", s->error);
    return false;
}

float br_playtest_frame_dt(br_engine *e)
{
    return br_ticker_compute_delta(1.0f / 60.0f, e->ticker.min_fps, e->ticker.speed);
}

static const char *br_pt_string(const cJSON *cmd, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cmd, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double br_pt_number(const cJSON *cmd, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cmd, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

/* A number clamped to [lo, hi] before any integer conversion (commands come from outside). */
static double br_pt_clamped(const cJSON *cmd, const char *key, double fallback, double lo, double hi)
{
    double v = br_pt_number(cmd, key, fallback);
    if (v < lo) {
        return lo;
    }
    return v > hi ? hi : v;
}

/* The frame that will see input sent now: this one inside the playtest system, else the next. */
static uint64_t br_pt_input_frame(br_engine *e)
{
    return e->playtest->in_frame ? e->ticker.frame : e->ticker.frame + 1;
}

/* ---- log capture */

static void SDLCALL br_pt_log_output(void *user, int category, SDL_LogPriority priority, const char *message)
{
    br_playtest_state *s = user;
    SDL_LockMutex(s->log_lock);
    br_str_copy(s->log_lines[s->log_count % BR_PLAYTEST_LOG_LINES], BR_PLAYTEST_LOG_SIZE, message);
    s->log_count++;
    SDL_UnlockMutex(s->log_lock);
    if (s->log_prev != NULL) {
        s->log_prev(s->log_prev_user, category, priority, message);
    }
}

bool br_playtest_log_contains(br_engine *e, const char *text, uint32_t *kept)
{
    br_playtest_state *s = e->playtest;
    bool found = false;
    SDL_LockMutex(s->log_lock);
    uint32_t have = s->log_count < BR_PLAYTEST_LOG_LINES ? s->log_count : BR_PLAYTEST_LOG_LINES;
    for (uint32_t i = s->log_count - have; i < s->log_count && !found; i++) {
        found = SDL_strstr(s->log_lines[i % BR_PLAYTEST_LOG_LINES], text) != NULL;
    }
    SDL_UnlockMutex(s->log_lock);
    *kept = have;
    return found;
}

void br_playtest_hook_log(br_engine *e)
{
    br_playtest_state *s = e->playtest;
    if (s->log_hooked) {
        return;
    }
    // ponytail: SDL has one process-wide log function; two engines hooking it must shut down in reverse order.
    SDL_GetLogOutputFunction(&s->log_prev, &s->log_prev_user);
    SDL_SetLogOutputFunction(br_pt_log_output, s);
    s->log_hooked = true;
}

/* ---- input */

static void br_pt_key(br_engine *e, SDL_Scancode key, bool down)
{
    e->playtest->injecting = true;
    br_input_inject_key(e, key, down);
    e->playtest->injecting = false;
}

static void br_pt_button(br_engine *e, float x, float y, int button, bool down)
{
    e->playtest->injecting = true;
    if (down) {
        br_input_inject_pointer_down(e, x, y, button);
    } else {
        br_input_inject_pointer_up(e, x, y, button);
    }
    e->playtest->injecting = false;
}

static void br_pt_schedule_release(br_engine *e, uint64_t frame, br_playtest_release_kind kind, SDL_Scancode key,
                                   int button, float x, float y)
{
    br_playtest_release r;
    SDL_zero(r);
    r.frame = frame;
    r.kind = kind;
    r.key = key;
    r.button = button;
    r.x = x;
    r.y = y;
    br_array_push(&e->playtest->releases, &r);
}

static void br_pt_pad_button(br_engine *e, int slot, SDL_GamepadButton button, bool down)
{
    e->playtest->injecting = true;
    br_input_inject_gamepad_button(e, slot, button, down);
    e->playtest->injecting = false;
}

static void br_pt_schedule_pad_release(br_engine *e, uint64_t frame, int slot, SDL_GamepadButton button)
{
    br_playtest_release r;
    SDL_zero(r);
    r.frame = frame;
    r.kind = BR_PLAYTEST_RELEASE_GAMEPAD;
    r.slot = slot;
    r.button = (int)button;
    br_array_push(&e->playtest->releases, &r);
}

static void br_pt_run_releases(br_engine *e)
{
    br_playtest_state *s = e->playtest;
    uint32_t i = 0;
    while (i < s->releases.count) {
        br_playtest_release r = *(br_playtest_release *)br_array_at(&s->releases, i);
        if (r.frame > e->ticker.frame) {
            i++;
            continue;
        }
        br_array_remove_ordered(&s->releases, i);
        if (r.kind == BR_PLAYTEST_RELEASE_KEY) {
            br_pt_key(e, r.key, false);
        } else if (r.kind == BR_PLAYTEST_RELEASE_MOUSE || r.kind == BR_PLAYTEST_PRESS_MOUSE) {
            br_pt_button(e, r.x, r.y, r.button, r.kind == BR_PLAYTEST_PRESS_MOUSE);
        } else {
            br_pt_pad_button(e, r.slot, (SDL_GamepadButton)r.button, false);
        }
    }
}

/* down / up / tap ("frames" = how long a tap holds, default 1) */
static bool br_pt_cmd_key(br_engine *e, const cJSON *cmd, SDL_Scancode key)
{
    const char *action = br_pt_string(cmd, "action");
    if (action == NULL || SDL_strcmp(action, "tap") == 0) {
        uint64_t frames = (uint64_t)br_pt_clamped(cmd, "frames", 1.0, 1.0, 1e9);
        br_pt_key(e, key, true);
        br_pt_schedule_release(e, br_pt_input_frame(e) + (frames > 0 ? frames : 1), BR_PLAYTEST_RELEASE_KEY, key, 0,
                               0.0f, 0.0f);
    } else if (SDL_strcmp(action, "down") == 0 || SDL_strcmp(action, "up") == 0) {
        br_pt_key(e, key, action[0] == 'd');
    } else {
        return br_playtest_fail(e, "key: action must be tap, down or up, got '%s'", action);
    }
    return true;
}

/* A named input action, whatever it is bound to (game.json or code): tap (default, "frames"
 * long), down / press (held until up), up / release. "value" 0..1 drives analogue actions. */
static bool br_pt_cmd_action(br_engine *e, const cJSON *cmd)
{
    const char *name = br_pt_string(cmd, "name");
    const char *action = br_pt_string(cmd, "action");
    float value = (float)br_pt_clamped(cmd, "value", 1.0, 0.0, 1.0);
    uint32_t frames = (uint32_t)br_pt_clamped(cmd, "frames", 1.0, 1.0, 1e9);
    if (action == NULL || SDL_strcmp(action, "tap") == 0) {
        /* frames as given */
    } else if (SDL_strcmp(action, "down") == 0 || SDL_strcmp(action, "press") == 0) {
        frames = UINT32_MAX; /* held until "up" */
    } else if (SDL_strcmp(action, "up") == 0 || SDL_strcmp(action, "release") == 0) {
        frames = 0;
        value = 0.0f;
    } else {
        return br_playtest_fail(e, "action: action must be tap, down (press) or up (release), got '%s'", action);
    }
    if (!br_input_inject_action(e, name, value, frames)) {
        return br_playtest_fail(e, "action: unknown input action '%s'", name);
    }
    return true;
}

static bool br_pt_cmd_mouse(br_engine *e, const cJSON *cmd)
{
    const char *action = br_pt_string(cmd, "action");
    br_vec2 at = br_input_mouse_position(e);
    float x = (float)br_pt_number(cmd, "x", at.x);
    float y = (float)br_pt_number(cmd, "y", at.y);
    int button = (int)br_pt_clamped(cmd, "button", SDL_BUTTON_LEFT, 1.0, 8.0);
    if (action == NULL || SDL_strcmp(action, "move") == 0) {
        /* recordings carry the relative movement (mouse-look in relative mode moves while the
         * position stays pinned); without dx/dy it is the difference of positions */
        float dx = (float)br_pt_number(cmd, "dx", x - at.x);
        float dy = (float)br_pt_number(cmd, "dy", y - at.y);
        e->playtest->injecting = true;
        br_input_inject_pointer_motion(e, x, y, dx, dy);
        e->playtest->injecting = false;
    } else if (SDL_strcmp(action, "down") == 0 || SDL_strcmp(action, "up") == 0) {
        br_pt_button(e, x, y, button, action[0] == 'd');
    } else if (SDL_strcmp(action, "tap") == 0) {
        br_pt_button(e, x, y, button, true);
        br_pt_schedule_release(e, br_pt_input_frame(e) + 1, BR_PLAYTEST_RELEASE_MOUSE, SDL_SCANCODE_UNKNOWN, button, x, y);
    } else if (SDL_strcmp(action, "wheel") == 0) {
        e->playtest->injecting = true;
        br_input_inject_wheel(e, (float)br_pt_number(cmd, "dx", 0.0), (float)br_pt_number(cmd, "dy", 0.0));
        e->playtest->injecting = false;
    } else {
        return br_playtest_fail(e, "mouse: action must be move, down, up, tap or wheel, got '%s'", action);
    }
    return true;
}

static bool br_pt_cmd_touch(br_engine *e, const cJSON *cmd)
{
    const char *action = br_pt_string(cmd, "action");
    SDL_EventType type = SDL_EVENT_FINGER_MOTION;
    if (SDL_strcmp(action, "down") == 0) {
        type = SDL_EVENT_FINGER_DOWN;
    } else if (SDL_strcmp(action, "up") == 0) {
        type = SDL_EVENT_FINGER_UP;
    } else if (SDL_strcmp(action, "move") != 0) {
        return br_playtest_fail(e, "touch: action must be down, move or up, got '%s'", action);
    }
    e->playtest->injecting = true;
    br_input_inject_touch(e, type, (uint64_t)br_pt_clamped(cmd, "finger", 0.0, 0.0, 1e9), (float)br_pt_number(cmd, "x", 0.0),
                          (float)br_pt_number(cmd, "y", 0.0));
    e->playtest->injecting = false;
    return true;
}

/* Typed text arrives as one SDL_EVENT_TEXT_INPUT, like an IME commit. */
static void br_pt_cmd_text(br_engine *e, const char *text)
{
    e->playtest->injecting = true;
    br_input_inject_text(e, text);
    e->playtest->injecting = false;
}

/* Resizes the window (SDL_SetWindowSize; the next frame renders at the new size). Headless, it
 * sets the screen size the cameras see (br_camera_set_headless_screen), so aspect ratios and
 * letterboxing can be checked in one run. */
static bool br_pt_cmd_window(br_engine *e, const cJSON *cmd)
{
    int w = (int)br_pt_clamped(cmd, "width", 0.0, 0.0, 16384.0);
    int h = (int)br_pt_clamped(cmd, "height", 0.0, 0.0, 16384.0);
    if (w < 16 || h < 16) {
        return br_playtest_fail(e, "window: width and height must be 16..16384");
    }
    if (e->window == NULL) {
        br_camera_set_headless_screen(e, (float)w, (float)h);
        return true;
    }
    if (!SDL_SetWindowSize(e->window, w, h)) {
        return br_playtest_fail(e, "window: %s", SDL_GetError());
    }
    SDL_SyncWindow(e->window);
    return true;
}

static bool br_pt_cmd_click(br_engine *e, const cJSON *cmd)
{
    float x = 0.0f;
    float y = 0.0f;
    if (!br_playtest_query_click_point(e, br_pt_string(cmd, "label"), &x, &y)) {
        return false;
    }
    int button = (int)br_pt_clamped(cmd, "button", SDL_BUTTON_LEFT, 1.0, 8.0);
    /* move now, press next frame, release the frame after: like a hand, and the press frame
     * sees no pointer jump (a game that drags a camera while the button is down stays put) */
    e->playtest->injecting = true;
    br_input_inject_pointer_move(e, x, y);
    e->playtest->injecting = false;
    uint64_t frame = br_pt_input_frame(e);
    br_pt_schedule_release(e, frame + 1, BR_PLAYTEST_PRESS_MOUSE, SDL_SCANCODE_UNKNOWN, button, x, y);
    br_pt_schedule_release(e, frame + 2, BR_PLAYTEST_RELEASE_MOUSE, SDL_SCANCODE_UNKNOWN, button, x, y);
    return true;
}

/* ---- gamepads (virtual SDL pads of a chosen family, or injected pads with no device) */

/* Names of br_gamepad_type values in commands and recordings (SDL's type names, plus steam). */
static const char *const br_pt_pad_type_names[BR_GAMEPAD_TYPE_COUNT] = {
    "generic",   "xbox360",     "xboxone",    "ps3",      "ps4",   "ps5", "switchpro",
    "joyconleft", "joyconright", "joyconpair", "gamecube", "steam",
};

/* A type name, or a family shorthand ("xbox", "playstation", "switch"). -1 if unknown. */
static int br_pt_pad_type(const char *name)
{
    if (SDL_strcmp(name, "xbox") == 0) {
        return BR_GAMEPAD_TYPE_XBOXONE;
    }
    if (SDL_strcmp(name, "playstation") == 0) {
        return BR_GAMEPAD_TYPE_PS5;
    }
    if (SDL_strcmp(name, "switch") == 0) {
        return BR_GAMEPAD_TYPE_SWITCH_PRO;
    }
    for (int i = 0; i < BR_GAMEPAD_TYPE_COUNT; i++) {
        if (SDL_strcmp(name, br_pt_pad_type_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool br_pt_gamepad_connect(br_engine *e, const cJSON *cmd, cJSON *reply)
{
    const char *name = br_pt_string(cmd, "type");
    int type = br_pt_pad_type(name != NULL ? name : "generic");
    if (type < 0) {
        return br_playtest_fail(e, "gamepad: unknown type '%s' (xbox, playstation, switch, generic, xbox360, xboxone, "
                                   "ps3, ps4, ps5, switchpro, joyconleft, joyconright, joyconpair, gamecube, steam)",
                                name);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cmd, "injected"))) {
        /* no SDL device: works where virtual joysticks do not, carries the family for prompts */
        int at = (int)br_pt_clamped(cmd, "slot", 0.0, 0.0, BR_INPUT_MAX_GAMEPADS - 1);
        e->playtest->injecting = true;
        bool ok = br_input_inject_gamepad_connect(e, at, (br_gamepad_type)type);
        e->playtest->injecting = false;
        if (!ok) {
            return br_playtest_fail(e, "gamepad: slot %d is taken", at);
        }
        if (reply != NULL) {
            cJSON_AddNumberToObject(reply, "slot", at);
        }
        return true;
    }
    e->playtest->injecting = true;
    int slot = br_input_gamepad_attach_virtual(e, (br_gamepad_type)type);
    e->playtest->injecting = false;
    if (slot < 0) {
        return br_playtest_fail(e, "gamepad: cannot attach a virtual %s pad (see the log)", br_pt_pad_type_names[type]);
    }
    /* a recording names the slot the pad had */
    int want = (int)br_pt_clamped(cmd, "slot", slot, 0.0, BR_INPUT_MAX_GAMEPADS - 1);
    if (want != slot && br_input_gamepad_assign(e, slot, want)) {
        slot = want;
    }
    if (reply != NULL) {
        cJSON_AddNumberToObject(reply, "slot", slot);
    }
    return true;
}

/* connect / disconnect, down / up / tap a button, axis. "slot" defaults to 0. */
static bool br_pt_cmd_gamepad(br_engine *e, const cJSON *cmd, cJSON *reply)
{
    const char *action = br_pt_string(cmd, "action");
    if (SDL_strcmp(action, "connect") == 0) {
        return br_pt_gamepad_connect(e, cmd, reply);
    }
    int slot = (int)br_pt_clamped(cmd, "slot", 0.0, 0.0, BR_INPUT_MAX_GAMEPADS - 1);
    if (SDL_strcmp(action, "disconnect") == 0) {
        br_gamepad_info info;
        if (!br_input_gamepad_info(e, slot, &info)) {
            return br_playtest_fail(e, "gamepad: no pad in slot %d", slot);
        }
        e->playtest->injecting = true;
        if (info.virtual_device) {
            br_input_gamepad_detach_virtual(e, slot);
        } else {
            br_input_inject_gamepad_disconnect(e, slot);
        }
        e->playtest->injecting = false;
        return true;
    }
    if (SDL_strcmp(action, "axis") == 0) {
        const char *name = br_pt_string(cmd, "axis");
        SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name);
        if (axis == SDL_GAMEPAD_AXIS_INVALID || !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(cmd, "value"))) {
            return br_playtest_fail(e, "gamepad axis: needs \"axis\" (leftx lefty rightx righty lefttrigger "
                                       "righttrigger) and a number \"value\" (-1..1, triggers 0..1)");
        }
        e->playtest->injecting = true;
        br_input_inject_gamepad_axis(e, slot, axis, (float)br_pt_clamped(cmd, "value", 0.0, -1.0, 1.0));
        e->playtest->injecting = false;
        return true;
    }
    bool tap = SDL_strcmp(action, "tap") == 0;
    if (!tap && SDL_strcmp(action, "down") != 0 && SDL_strcmp(action, "up") != 0) {
        return br_playtest_fail(e, "gamepad: action must be connect, disconnect, down, up, tap or axis, got '%s'",
                                action);
    }
    const char *name = br_pt_string(cmd, "button");
    SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name);
    if (button == SDL_GAMEPAD_BUTTON_INVALID) {
        return br_playtest_fail(e, "gamepad %s: needs \"button\" (SDL names: a b x y back guide start leftstick "
                                   "rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 touchpad...)",
                                action);
    }
    br_pt_pad_button(e, slot, button, action[0] != 'u');
    if (tap) {
        uint64_t frames = (uint64_t)br_pt_clamped(cmd, "frames", 1.0, 1.0, 1e9);
        br_pt_schedule_pad_release(e, br_pt_input_frame(e) + frames, slot, button);
    }
    return true;
}

/* ---- command table and validation */

typedef struct br_pt_command_info {
    const char *name;
    /* arguments as "name:type", optional ones "name?:type"; type = string, number, bool or any.
     * The validator checks them and "help" sends them to the agent. */
    const char *args;
    const char *doc;
} br_pt_command_info;

static const br_pt_command_info br_pt_commands[] = {
    { "step", "frames?:number seconds?:number", "advance N frames of 1/60 s (default 1)" },
    { "wait", "frames?:number seconds?:number", "same as step" },
    { "wait_until", "path?:string op?:string value?:any log?:string timeout_frames?:number timeout?:number",
      "step until path op value holds (or a log line contains log); fails after the timeout" },
    { "key", "key:string action?:string frames?:number", "SDL key name; action tap (default), down or up" },
    { "action", "name:string action?:string value?:number frames?:number",
      "input action; action tap (default), down/press, up/release; value 0..1" },
    { "mouse", "action?:string x?:number y?:number button?:number dx?:number dy?:number",
      "action move (default), down, up, tap or wheel; window coordinates" },
    { "touch", "action:string finger?:number x?:number y?:number", "action down, move or up" },
    { "text", "text:string", "type text" },
    { "click", "label:string button?:number", "click a node by label (\"view/node\" for a 3D node)" },
    { "gamepad",
      "action:string type?:string slot?:number button?:string axis?:string value?:number frames?:number injected?:bool",
      "action connect (type: xbox, playstation, switch, ps5...; injected: no SDL device), disconnect, tap, down, up "
      "(button), axis (axis, value)" },
    { "window", "width:number height:number",
      "resize the window (headless: the screen size cameras see) to test aspect ratios" },
    { "screenshot", "file:string downsample?:number", "write the last rendered frame as PNG" },
    { "dump", "file?:string", "scene tree, components, exposed state, 3D views" },
    { "find", "label:string", "type, position, bounds... of a node" },
    { "get", "path:string", "read label:property, label:component.field, name:field, view/node:property" },
    { "set", "path:string value:any", "write a path" },
    { "list", "type?:string component?:string", "nodes by type and/or component" },
    { "logs", "count?:number", "the last log lines" },
    { "stats", "", "frame, fps, nodes, draw calls, memory..." },
    { "profile", "frames?:number",
      "wait N frames (default 300), reply with the profiler summary of them; profile:<key> reads it" },
    { "expect", "path?:string op?:string value?:any log?:string", "check path op value (== != < <= > >= contains) or a log line" },
    { "log", "message:string", "write a line to the log" },
    { "help", "", "this list" },
    { "quit", "", "exit after this frame" },
};

static bool br_pt_has_type(const cJSON *v, const char *type)
{
    if (SDL_strcmp(type, "string") == 0) {
        return cJSON_IsString(v);
    }
    if (SDL_strcmp(type, "number") == 0) {
        return cJSON_IsNumber(v);
    }
    if (SDL_strcmp(type, "bool") == 0) {
        return cJSON_IsBool(v);
    }
    return true; /* any */
}

/* True if `args` ("name:type name?:type ...") declares an argument called `key`. */
static bool br_pt_declares(const char *args, const char *key)
{
    size_t len = SDL_strlen(key);
    for (const char *p = args; *p != '\0';) {
        if (SDL_strncmp(p, key, len) == 0 && (p[len] == ':' || p[len] == '?')) {
            return true;
        }
        const char *space = SDL_strchr(p, ' ');
        if (space == NULL) {
            break;
        }
        p = space + 1;
    }
    return false;
}

/* Checks one command before anything runs. False with the reason in why. */
static bool br_pt_validate(const cJSON *cmd, char *why, size_t why_size)
{
    const char *name = br_pt_string(cmd, "cmd");
    if (!cJSON_IsObject(cmd) || name == NULL) {
        SDL_snprintf(why, why_size, "expected an object with a \"cmd\" string (send {\"cmd\":\"help\"} for the list)");
        return false;
    }
    const br_pt_command_info *info = NULL;
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(br_pt_commands); i++) {
        if (SDL_strcmp(br_pt_commands[i].name, name) == 0) {
            info = &br_pt_commands[i];
        }
    }
    if (info == NULL) {
        SDL_snprintf(why, why_size, "unknown command '%s' (send {\"cmd\":\"help\"} for the list)", name);
        return false;
    }
    char args[160];
    br_str_copy(args, sizeof(args), info->args);
    char *save = NULL;
    for (char *arg = SDL_strtok_r(args, " ", &save); arg != NULL; arg = SDL_strtok_r(NULL, " ", &save)) {
        char *type = SDL_strchr(arg, ':');
        *type = '\0'; /* arg = the name, type = what follows the colon */
        type++;
        size_t len = SDL_strlen(arg);
        bool optional = arg[len - 1] == '?';
        if (optional) {
            arg[len - 1] = '\0';
        }
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(cmd, arg);
        if (v == NULL && !optional) {
            SDL_snprintf(why, why_size, "%s: missing \"%s\"", name, arg);
            return false;
        }
        if (v != NULL && !br_pt_has_type(v, type)) {
            SDL_snprintf(why, why_size, "%s: \"%s\" must be a %s", name, arg, type);
            return false;
        }
    }
    /* a typo ("frame" for "frames") would silently fall back to the default: reject it */
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cmd)
    {
        if (SDL_strcmp(item->string, "cmd") != 0 && !br_pt_declares(info->args, item->string)) {
            SDL_snprintf(why, why_size, "%s: unknown argument '%s' (takes: %s)", name, item->string,
                         info->args[0] != '\0' ? info->args : "nothing");
            return false;
        }
    }
    if (SDL_strcmp(name, "key") == 0 && SDL_GetScancodeFromName(br_pt_string(cmd, "key")) == SDL_SCANCODE_UNKNOWN) {
        SDL_snprintf(why, why_size, "key: unknown key name (SDL names: \"Space\", \"Return\", \"Left\", \"A\"...)");
        return false;
    }
    if ((SDL_strcmp(name, "expect") == 0 || SDL_strcmp(name, "wait_until") == 0) &&
        br_pt_string(cmd, "path") == NULL && br_pt_string(cmd, "log") == NULL) {
        SDL_snprintf(why, why_size, "%s: needs \"path\" (with \"value\", optional \"op\") or \"log\"", name);
        return false;
    }
    return true;
}

/* ---- execution */

static void br_pt_finish_script(br_engine *e, bool failed)
{
    br_playtest_state *s = e->playtest;
    s->running = false;
    s->waiting = false;
    s->failed = s->failed || failed;
    if (failed) {
        br_log_error("playtest: script '%s' FAILED at step %d (frame %llu): %s", s->script_path, s->step_index,
                     (unsigned long long)e->ticker.frame, s->error);
    } else {
        br_log_info("playtest: script '%s' passed (%d steps, frame %llu)", s->script_path, s->step_index,
                    (unsigned long long)e->ticker.frame);
    }
    e->exit_failure = e->exit_failure || failed;
    br_engine_quit(e);
}

static bool br_pt_save_text(br_engine *e, const char *path, const char *text)
{
    char full[1024];
    if (text == NULL || !br_file_resolve_cwd(path, full, sizeof(full)) || !br_file_write_all(e, full, text, SDL_strlen(text))) {
        return br_playtest_fail(e, "cannot write '%s'", path);
    }
    return true;
}

static bool br_pt_save_json(br_engine *e, const char *path, const cJSON *json)
{
    char *text = cJSON_Print(json);
    bool ok = br_pt_save_text(e, path, text);
    cJSON_free(text);
    return ok;
}

/* A script with one command per line: diffs and hand edits stay readable. */
static bool br_pt_save_script(br_engine *e, const char *path, const cJSON *script)
{
    br_array text;
    br_array_init(&text, 1);
    br_array_push_n(&text, "[\n", 2);
    const cJSON *cmd = NULL;
    cJSON_ArrayForEach(cmd, script)
    {
        char *line = cJSON_PrintUnformatted(cmd);
        br_array_push_n(&text, "  ", 2);
        br_array_push_n(&text, line, (uint32_t)SDL_strlen(line));
        if (cmd->next != NULL) {
            br_array_push_n(&text, ",", 1);
        }
        br_array_push_n(&text, "\n", 1);
        cJSON_free(line);
    }
    br_array_push_n(&text, "]\n", 3); /* the third byte is the terminating NUL */
    bool ok = br_pt_save_text(e, path, text.data);
    br_array_free(&text);
    return ok;
}

static void br_pt_add_stats(br_engine *e, cJSON *out)
{
    br_render_stats rs;
    SDL_zero(rs);
    br_render_get_stats(e, &rs);
    br_memory_stats ms;
    br_memory_get_stats(&ms);
    cJSON_AddNumberToObject(out, "frame", (double)e->ticker.frame);
    cJSON_AddNumberToObject(out, "time", e->ticker.elapsed);
    cJSON_AddNumberToObject(out, "fps", e->ticker.fps);
    cJSON_AddNumberToObject(out, "nodes", br_node_count(e));
    cJSON_AddNumberToObject(out, "draw_calls", rs.draw_calls);
    cJSON_AddNumberToObject(out, "textures", br_texture_count(e));
    cJSON_AddNumberToObject(out, "skipped_frames", (double)e->skipped_frames);
    cJSON_AddNumberToObject(out, "heap_bytes", (double)ms.live_bytes);
    cJSON_AddStringToObject(out, "vsync", br_vsync_name(e->vsync));
    cJSON_AddNumberToObject(out, "max_fps", e->ticker.max_fps);
    cJSON_AddBoolToObject(out, "deterministic", e->deterministic);
}

/* A wait / wait_until in progress: DONE when over, WAIT to keep waiting, FAILED on timeout. */
static br_pt_result br_pt_wait_check(br_engine *e)
{
    br_playtest_state *s = e->playtest;
    if (!s->waiting) {
        return BR_PT_DONE;
    }
    if (s->until == NULL) {
        if (e->ticker.frame < s->resume_frame) {
            return BR_PT_WAIT;
        }
        s->waiting = false;
        return BR_PT_DONE;
    }
    bool ok = false;
    char detail[256] = "";
    bool evaluated = br_playtest_query_condition(e, s->until, &ok, detail, sizeof(detail));
    if (evaluated && ok) {
        s->waiting = false;
        return BR_PT_DONE;
    }
    if (e->ticker.frame >= s->until_deadline) {
        s->waiting = false;
        /* an unresolvable path counts as "not yet" until the timeout, then it is the reason */
        char reason[BR_PLAYTEST_ERROR_SIZE];
        br_str_copy(reason, sizeof(reason), evaluated ? detail : s->error);
        br_playtest_fail(e, "wait_until timed out: %s", reason);
        return BR_PT_FAILED;
    }
    return BR_PT_WAIT;
}

static br_pt_result br_pt_ok(bool ok)
{
    return ok ? BR_PT_DONE : BR_PT_FAILED;
}

/* Puts value into reply under key, or frees it when there is no reply (script mode). */
static void br_pt_reply_add(cJSON *reply, const char *key, cJSON *value)
{
    if (reply != NULL) {
        cJSON_AddItemToObject(reply, key, value);
    } else {
        cJSON_Delete(value);
    }
}

static br_pt_result br_pt_exec_find(br_engine *e, const cJSON *cmd, cJSON *reply)
{
    const char *label = br_pt_string(cmd, "label");
    const char *props2d[] = { "type", "position", "world_position", "bounds", "visible" };
    const char *props3d[] = { "type", "position", "world_position", "screen_position", "visible" };
    const char **props = SDL_strchr(label, '/') != NULL ? props3d : props2d; /* "view/node": a 3D node */
    char path[256];
    SDL_snprintf(path, sizeof(path), "%s:exists", label);
    cJSON *exists = br_playtest_query_get(e, path);
    bool found = cJSON_IsTrue(exists);
    cJSON_Delete(exists);
    br_pt_reply_add(reply, "found", cJSON_CreateBool(found));
    for (uint32_t i = 0; found && i < BR_ARRAY_COUNT(props2d); i++) {
        SDL_snprintf(path, sizeof(path), "%s:%s", label, props[i]);
        cJSON *v = br_playtest_query_get(e, path);
        if (v != NULL) {
            br_pt_reply_add(reply, props[i], v);
        }
    }
    return BR_PT_DONE;
}

static br_pt_result br_pt_exec_logs(br_engine *e, const cJSON *cmd, cJSON *reply)
{
    br_playtest_state *s = e->playtest;
    uint32_t want = (uint32_t)br_pt_clamped(cmd, "count", 20.0, 0.0, BR_PLAYTEST_LOG_LINES);
    cJSON *lines = cJSON_CreateArray();
    SDL_LockMutex(s->log_lock);
    uint32_t have = s->log_count < BR_PLAYTEST_LOG_LINES ? s->log_count : BR_PLAYTEST_LOG_LINES;
    uint32_t n = want < have ? want : have;
    for (uint32_t i = s->log_count - n; i < s->log_count; i++) {
        cJSON_AddItemToArray(lines, cJSON_CreateString(s->log_lines[i % BR_PLAYTEST_LOG_LINES]));
    }
    SDL_UnlockMutex(s->log_lock);
    br_pt_reply_add(reply, "lines", lines);
    return BR_PT_DONE;
}

/* wait / step: "frames" (default 1) or "seconds" (60 frames per second). */
static br_pt_result br_pt_exec_wait(br_engine *e, const cJSON *cmd)
{
    br_playtest_state *s = e->playtest;
    double frames = br_pt_clamped(cmd, "frames", 1.0, 0.0, 1e9);
    if (cJSON_GetObjectItemCaseSensitive(cmd, "seconds") != NULL) {
        frames = SDL_ceil(br_pt_clamped(cmd, "seconds", 0.0, 0.0, 1e7) * 60.0);
    }
    if (frames < 1.0) {
        return BR_PT_DONE;
    }
    s->waiting = true;
    s->until = NULL;
    s->resume_frame = e->ticker.frame + (uint64_t)frames;
    return BR_PT_WAIT;
}

static br_pt_result br_pt_exec_wait_until(br_engine *e, const cJSON *cmd)
{
    br_playtest_state *s = e->playtest;
    double timeout = br_pt_clamped(cmd, "timeout_frames", BR_PLAYTEST_DEFAULT_TIMEOUT, 0.0, 1e9);
    if (cJSON_GetObjectItemCaseSensitive(cmd, "timeout") != NULL) {
        timeout = SDL_ceil(br_pt_clamped(cmd, "timeout", 10.0, 0.0, 1e7) * 60.0);
    }
    s->waiting = true;
    s->until = cmd;
    s->until_deadline = e->ticker.frame + (uint64_t)(timeout > 0.0 ? timeout : 0.0);
    return br_pt_wait_check(e);
}

/* Runs one validated command. Results go into reply (NULL in scripts). */
static br_pt_result br_pt_exec(br_engine *e, const cJSON *cmd, cJSON *reply)
{
    const char *name = br_pt_string(cmd, "cmd");
    if (SDL_strcmp(name, "wait") == 0 || SDL_strcmp(name, "step") == 0) {
        return br_pt_exec_wait(e, cmd);
    }
    if (SDL_strcmp(name, "wait_until") == 0) {
        return br_pt_exec_wait_until(e, cmd);
    }
    if (SDL_strcmp(name, "key") == 0) {
        return br_pt_ok(br_pt_cmd_key(e, cmd, SDL_GetScancodeFromName(br_pt_string(cmd, "key"))));
    }
    if (SDL_strcmp(name, "action") == 0) {
        return br_pt_ok(br_pt_cmd_action(e, cmd));
    }
    if (SDL_strcmp(name, "mouse") == 0) {
        return br_pt_ok(br_pt_cmd_mouse(e, cmd));
    }
    if (SDL_strcmp(name, "touch") == 0) {
        return br_pt_ok(br_pt_cmd_touch(e, cmd));
    }
    if (SDL_strcmp(name, "text") == 0) {
        br_pt_cmd_text(e, br_pt_string(cmd, "text"));
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "click") == 0) {
        return br_pt_ok(br_pt_cmd_click(e, cmd));
    }
    if (SDL_strcmp(name, "gamepad") == 0) {
        return br_pt_ok(br_pt_cmd_gamepad(e, cmd, reply));
    }
    if (SDL_strcmp(name, "window") == 0) {
        return br_pt_ok(br_pt_cmd_window(e, cmd));
    }
    if (SDL_strcmp(name, "screenshot") == 0) {
        const char *file = br_pt_string(cmd, "file");
        uint32_t downsample = (uint32_t)br_pt_clamped(cmd, "downsample", 1.0, 1.0, 64.0);
        if (!br_engine_screenshot_downsampled(e, file, downsample > 0 ? downsample : 1)) {
            return br_pt_ok(br_playtest_fail(e, "screenshot '%s' failed (headless, or nothing rendered yet?)", file));
        }
        br_pt_reply_add(reply, "file", cJSON_CreateString(file));
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "dump") == 0) {
        cJSON *dump = br_playtest_query_dump(e);
        const char *file = br_pt_string(cmd, "file");
        if (file != NULL) {
            bool ok = br_pt_save_json(e, file, dump);
            cJSON_Delete(dump);
            return br_pt_ok(ok);
        }
        br_pt_reply_add(reply, "dump", dump);
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "find") == 0) {
        return br_pt_exec_find(e, cmd, reply);
    }
    if (SDL_strcmp(name, "get") == 0) {
        cJSON *v = br_playtest_query_get(e, br_pt_string(cmd, "path"));
        if (v == NULL) {
            return BR_PT_FAILED;
        }
        br_pt_reply_add(reply, "value", v);
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "set") == 0) {
        return br_pt_ok(br_playtest_query_set(e, br_pt_string(cmd, "path"),
                                              cJSON_GetObjectItemCaseSensitive(cmd, "value")));
    }
    if (SDL_strcmp(name, "list") == 0) {
        br_pt_reply_add(reply, "nodes",
                        br_playtest_query_list(e, br_pt_string(cmd, "type"), br_pt_string(cmd, "component")));
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "logs") == 0) {
        return br_pt_exec_logs(e, cmd, reply);
    }
    if (SDL_strcmp(name, "profile") == 0) {
        /* the summary is read after the wait: br_playtest_command adds it, scripts use profile:<key> */
        double frames = br_pt_clamped(cmd, "frames", BR_PROFILER_FRAMES, 1.0, 1e9);
        e->playtest->profile_frames = frames < BR_PROFILER_FRAMES ? (uint32_t)frames : BR_PROFILER_FRAMES;
        br_profiler_gpu_want(e, (uint32_t)frames + 1u); /* the estimate runs only while measured */
        e->playtest->waiting = true;
        e->playtest->until = NULL;
        e->playtest->resume_frame = e->ticker.frame + (uint64_t)frames;
        return BR_PT_WAIT;
    }
    if (SDL_strcmp(name, "stats") == 0) {
        cJSON *stats = cJSON_CreateObject();
        br_pt_add_stats(e, stats);
        br_pt_reply_add(reply, "stats", stats);
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "expect") == 0) {
        bool ok = false;
        char detail[256] = "";
        if (!br_playtest_query_condition(e, cmd, &ok, detail, sizeof(detail))) {
            return BR_PT_FAILED;
        }
        return br_pt_ok(ok || br_playtest_fail(e, "expect failed: %s", detail));
    }
    if (SDL_strcmp(name, "log") == 0) {
        br_log_info("playtest: %s", br_pt_string(cmd, "message"));
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "help") == 0) {
        cJSON *list = cJSON_CreateArray();
        for (uint32_t i = 0; i < BR_ARRAY_COUNT(br_pt_commands); i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "cmd", br_pt_commands[i].name);
            cJSON_AddStringToObject(item, "args", br_pt_commands[i].args);
            cJSON_AddStringToObject(item, "doc", br_pt_commands[i].doc);
            cJSON_AddItemToArray(list, item);
        }
        br_pt_reply_add(reply, "commands", list);
        return BR_PT_DONE;
    }
    if (SDL_strcmp(name, "quit") == 0) {
        e->exit_failure = e->exit_failure || e->playtest->failed;
        br_engine_quit(e);
        return BR_PT_DONE;
    }
    return br_pt_ok(br_playtest_fail(e, "unknown command '%s'", name));
}

/* ---- script mode */

static void br_pt_script_advance(br_engine *e)
{
    br_playtest_state *s = e->playtest;
    int count = cJSON_GetArraySize(s->script);
    while (s->running) {
        br_pt_result r = br_pt_wait_check(e);
        if (r == BR_PT_WAIT) {
            return;
        }
        if (r == BR_PT_FAILED) {
            br_pt_finish_script(e, true);
            return;
        }
        if (s->step_index >= count) {
            br_pt_finish_script(e, false);
            return;
        }
        const cJSON *cmd = cJSON_GetArrayItem(s->script, s->step_index);
        s->step_index++;
        s->error[0] = '\0';
        if (br_pt_exec(e, cmd, NULL) == BR_PT_FAILED) {
            br_pt_finish_script(e, true);
            return;
        }
        if (SDL_strcmp(br_pt_string(cmd, "cmd"), "quit") == 0) {
            br_pt_finish_script(e, false);
            return;
        }
    }
}

/* System: releases due input, then runs the script until it has to wait for a frame. */
static void br_pt_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_playtest_state *s = e->playtest;
    s->in_frame = true;
    br_pt_run_releases(e);
    if (s->running) {
        br_pt_script_advance(e);
    }
    s->in_frame = false;
}

/* Marks the play-test failed with the reason already in s->error. */
static bool br_pt_load_failed(br_engine *e, cJSON *script)
{
    cJSON_Delete(script);
    e->playtest->failed = true;
    e->exit_failure = true;
    return false;
}

bool br_playtest_load_script(br_engine *e, const char *path)
{
    br_playtest_state *s = e->playtest;
    br_playtest_hook_log(e);
    size_t size = 0;
    void *bytes = NULL;
    char full[1024];
    if (!br_file_resolve_cwd(path, full, sizeof(full)) || !br_file_read_all(e, full, &bytes, &size)) {
        br_playtest_fail(e, "%s: cannot read the script", path);
        return br_pt_load_failed(e, NULL);
    }
    cJSON *script = br_data_parse(e, path, bytes, size); /* logs "path: JSON syntax error at line L, column C" */
    br_free(bytes);
    if (script == NULL) {
        br_str_copy(s->error, sizeof(s->error), e->data != NULL ? e->data->last_error : "JSON syntax error");
        return br_pt_load_failed(e, NULL);
    }
    if (!cJSON_IsArray(script)) {
        br_playtest_fail(e, "%s: a script is a JSON array of commands", path);
        return br_pt_load_failed(e, script);
    }
    int index = 0;
    const cJSON *cmd = NULL;
    cJSON_ArrayForEach(cmd, script)
    {
        index++;
        char why[256];
        if (!br_pt_validate(cmd, why, sizeof(why))) {
            br_playtest_fail(e, "%s: step %d: %s", path, index, why);
            return br_pt_load_failed(e, script);
        }
    }
    cJSON_Delete(s->script);
    s->script = script;
    br_str_copy(s->script_path, sizeof(s->script_path), path);
    s->step_index = 0;
    s->running = true;
    s->waiting = false;
    br_log_info("playtest: running '%s' (%d steps)", path, index);
    return true;
}

bool br_playtest_run_script(br_engine *e, const char *path)
{
    if (!br_playtest_load_script(e, path)) {
        return false;
    }
    float dt = br_playtest_frame_dt(e);
    while (e->playtest->running) {
        br_engine_step_frame(e, dt, br_ticker_compute_delta(1.0f / 60.0f, e->ticker.min_fps, 1.0f));
    }
    return !e->playtest->failed;
}

bool br_playtest_running(br_engine *e)
{
    return e->playtest->running;
}

bool br_playtest_failed(br_engine *e)
{
    return e->playtest->failed;
}

const char *br_playtest_last_error(br_engine *e)
{
    return e->playtest->error;
}

/* ---- interactive */

char *br_playtest_command(br_engine *e, const char *json_line)
{
    br_playtest_state *s = e->playtest;
    br_playtest_hook_log(e);
    s->error[0] = '\0';
    /* "ok" and "frame" lead the reply; results follow */
    cJSON *results = cJSON_CreateObject();
    cJSON *cmd = cJSON_Parse(json_line);
    char why[256];
    br_pt_result r = BR_PT_FAILED;
    if (!br_pt_validate(cmd, why, sizeof(why))) {
        br_playtest_fail(e, "%s", cmd == NULL ? "not JSON (send one command object per line)" : why);
    } else {
        r = br_pt_exec(e, cmd, results);
        float dt = br_playtest_frame_dt(e);
        while (r == BR_PT_WAIT) {
            br_engine_step_frame(e, dt, br_ticker_compute_delta(1.0f / 60.0f, e->ticker.min_fps, 1.0f));
            r = br_pt_wait_check(e);
        }
    }
    if (r == BR_PT_DONE && SDL_strcmp(br_pt_string(cmd, "cmd"), "profile") == 0) {
        cJSON *summary = br_playtest_query_get(e, "profile:*");
        if (summary == NULL) {
            r = BR_PT_FAILED;
        } else {
            cJSON_AddItemToObject(results, "profile", summary);
        }
    }
    s->failed = s->failed || r == BR_PT_FAILED;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", r == BR_PT_DONE);
    cJSON_AddNumberToObject(reply, "frame", (double)e->ticker.frame);
    if (r == BR_PT_FAILED) {
        cJSON_AddStringToObject(reply, "error", s->error);
    }
    while (results->child != NULL) {
        cJSON *item = results->child;
        cJSON_AddItemToObject(reply, item->string, cJSON_DetachItemViaPointer(results, item));
    }
    char *text = cJSON_PrintUnformatted(reply);
    char *copy = br_strdup(text != NULL ? text : "{\"ok\":false,\"error\":\"out of memory\"}");
    cJSON_free(text);
    cJSON_Delete(reply);
    cJSON_Delete(results);
    cJSON_Delete(cmd);
    return copy;
}

/* ---- C API */

void br_playtest_step(br_engine *e, uint32_t frames)
{
    float dt = br_playtest_frame_dt(e);
    for (uint32_t i = 0; i < frames; i++) {
        br_engine_step_frame(e, dt, br_ticker_compute_delta(1.0f / 60.0f, e->ticker.min_fps, 1.0f));
    }
}

bool br_playtest_screenshot(br_engine *e, const char *path)
{
    return br_engine_screenshot(e, path);
}

char *br_playtest_dump_json(br_engine *e)
{
    cJSON *dump = br_playtest_query_dump(e);
    char *text = cJSON_Print(dump);
    char *copy = br_strdup(text);
    cJSON_free(text);
    cJSON_Delete(dump);
    return copy;
}

cJSON *br_playtest_get(br_engine *e, const char *path)
{
    return br_playtest_query_get(e, path);
}

bool br_playtest_set(br_engine *e, const char *path, const cJSON *value)
{
    return br_playtest_query_set(e, path, value);
}

void br_playtest_expose(br_engine *e, const char *name, const br_field_desc *fields, uint32_t count, void *base)
{
    br_playtest_exposed x;
    SDL_zero(x);
    br_str_copy(x.name, sizeof(x.name), name);
    x.fields = fields;
    x.field_count = count;
    x.base = base;
    br_array_push(&e->playtest->exposed, &x);
}

/* ---- record */

static cJSON *br_pt_new_cmd(const char *name, const char *action)
{
    cJSON *cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "cmd", name);
    if (action != NULL) {
        cJSON_AddStringToObject(cmd, "action", action);
    }
    return cmd;
}

/* True for SDL's mouse events synthesised from touch or pen: the touch / pen itself is recorded,
 * a replayed copy would arrive as a real mouse and click twice. */
static bool br_pt_synthetic_mouse(const SDL_Event *ev)
{
    SDL_MouseID which = 0;
    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        which = ev->motion.which;
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        which = ev->button.which;
    } else if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        which = ev->wheel.which;
    }
    return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID;
}

/* A real input event as a command, NULL for events that are not recorded. */
static cJSON *br_pt_event_cmd(br_engine *e, const SDL_Event *ev)
{
    cJSON *cmd = NULL;
    if (br_pt_synthetic_mouse(ev)) {
        return NULL;
    }
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (ev->key.repeat || SDL_GetScancodeName(ev->key.scancode)[0] == '\0') {
            return NULL;
        }
        cmd = br_pt_new_cmd("key", ev->key.down ? "down" : "up");
        cJSON_AddStringToObject(cmd, "key", SDL_GetScancodeName(ev->key.scancode));
        return cmd;
    case SDL_EVENT_MOUSE_MOTION:
        // ponytail: every motion event is a command; coalesce per frame if recordings get big.
        cmd = br_pt_new_cmd("mouse", "move");
        cJSON_AddNumberToObject(cmd, "x", ev->motion.x);
        cJSON_AddNumberToObject(cmd, "y", ev->motion.y);
        cJSON_AddNumberToObject(cmd, "dx", ev->motion.xrel); /* relative mode moves with x/y pinned */
        cJSON_AddNumberToObject(cmd, "dy", ev->motion.yrel);
        return cmd;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        cmd = br_pt_new_cmd("mouse", ev->button.down ? "down" : "up");
        cJSON_AddNumberToObject(cmd, "x", ev->button.x);
        cJSON_AddNumberToObject(cmd, "y", ev->button.y);
        cJSON_AddNumberToObject(cmd, "button", ev->button.button);
        return cmd;
    case SDL_EVENT_MOUSE_WHEEL:
        cmd = br_pt_new_cmd("mouse", "wheel");
        cJSON_AddNumberToObject(cmd, "dx", ev->wheel.x);
        cJSON_AddNumberToObject(cmd, "dy", ev->wheel.y);
        return cmd;
    case SDL_EVENT_TEXT_INPUT:
        cmd = br_pt_new_cmd("text", NULL);
        cJSON_AddStringToObject(cmd, "text", ev->text.text);
        return cmd;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
        /* SDL fingers are normalised; injection takes window coordinates */
        int w = 0;
        int h = 0;
        br_engine_window_size(e, &w, &h);
        const char *action = "move";
        if (ev->type == SDL_EVENT_FINGER_DOWN) {
            action = "down";
        } else if (ev->type == SDL_EVENT_FINGER_UP) {
            action = "up";
        }
        cmd = br_pt_new_cmd("touch", action);
        cJSON_AddNumberToObject(cmd, "finger", (double)ev->tfinger.fingerID);
        cJSON_AddNumberToObject(cmd, "x", ev->tfinger.x * (float)w);
        cJSON_AddNumberToObject(cmd, "y", ev->tfinger.y * (float)h);
        return cmd;
    }
    default:
        return NULL;
    }
}

/* Slot of a pad in the recorder's map (as of the previous gamepad event), -1 if unknown. */
static int br_pt_known_pad(const br_playtest_state *s, SDL_JoystickID id)
{
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        if (s->pad_ids[i] == id && id != 0) {
            return i;
        }
    }
    return -1;
}

/* A gamepad event as a command. br_input saw the event first (it registered its handler
 * earlier), so a removed pad is gone from its slot: the recorder's own map remembers it. */
static cJSON *br_pt_gamepad_event_cmd(br_engine *e, const SDL_Event *ev)
{
    br_playtest_state *s = e->playtest;
    int known = br_pt_known_pad(s, ev->gdevice.which); /* which is first in every gamepad event */
    for (int i = 0; i < BR_INPUT_MAX_GAMEPADS; i++) {
        br_gamepad_info info;
        s->pad_ids[i] = br_input_gamepad_info(e, i, &info) ? info.id : 0;
    }
    int slot = ev->type == SDL_EVENT_GAMEPAD_REMOVED ? known : br_pt_known_pad(s, ev->gdevice.which);
    bool record = s->recording && !s->injecting && slot >= 0;
    if (!record || (ev->type == SDL_EVENT_GAMEPAD_ADDED && known >= 0)) {
        return NULL; /* not ours, or SDL's late duplicate ADDED for a virtual pad */
    }
    cJSON *cmd = NULL;
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED) {
        cmd = br_pt_new_cmd("gamepad", "connect");
        cJSON_AddStringToObject(cmd, "type", br_pt_pad_type_names[br_input_gamepad_type(e, slot)]);
    } else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED) {
        cmd = br_pt_new_cmd("gamepad", "disconnect");
    } else if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        cmd = br_pt_new_cmd("gamepad", "axis");
        cJSON_AddStringToObject(cmd, "axis", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)ev->gaxis.axis));
        /* br_input reads value / 32767 clamped; injection rounds back to the same Sint16 */
        cJSON_AddNumberToObject(cmd, "value", br_clampf((float)ev->gaxis.value / 32767.0f, -1.0f, 1.0f));
    } else {
        cmd = br_pt_new_cmd("gamepad", ev->gbutton.down ? "down" : "up");
        cJSON_AddStringToObject(cmd, "button", SDL_GetGamepadStringForButton((SDL_GamepadButton)ev->gbutton.button));
    }
    cJSON_AddNumberToObject(cmd, "slot", slot);
    return cmd;
}

static void br_pt_record_event(br_engine *e, const SDL_Event *ev, void *user)
{
    (void)user;
    br_playtest_state *s = e->playtest;
    bool pad_event = ev->type == SDL_EVENT_GAMEPAD_ADDED || ev->type == SDL_EVENT_GAMEPAD_REMOVED ||
                     ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION || ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                     ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    if (!pad_event && (!s->recording || s->injecting)) {
        return;
    }
    cJSON *cmd = pad_event ? br_pt_gamepad_event_cmd(e, ev) : br_pt_event_cmd(e, ev);
    if (cmd != NULL) {
        br_playtest_recorded r;
        /* real events arrive between frames (the next frame sees them); input injected by a
         * system inside a frame, before input latches, counts in that frame */
        r.frame = e->stepping ? e->ticker.frame : e->ticker.frame + 1;
        r.cmd = cmd;
        br_array_push(&s->recorded, &r);
    }
}

bool br_playtest_recording(br_engine *e)
{
    return e->playtest != NULL && e->playtest->recording;
}

bool br_playtest_record_start(br_engine *e, const char *path)
{
    br_playtest_state *s = e->playtest;
    if (path == NULL || !br_str_copy(s->record_path, sizeof(s->record_path), path)) {
        return br_playtest_fail(e, "record: bad output path");
    }
    s->recording = true;
    br_log_info("playtest: recording input to '%s'", path);
    return true;
}

static void br_pt_add_wait(cJSON *script, uint64_t frames)
{
    if (frames > 0) {
        cJSON *wait = br_pt_new_cmd("wait", NULL);
        cJSON_AddNumberToObject(wait, "frames", (double)frames);
        cJSON_AddItemToArray(script, wait);
    }
}

bool br_playtest_record_stop(br_engine *e)
{
    br_playtest_state *s = e->playtest;
    if (!s->recording) {
        return false;
    }
    s->recording = false;
    /* a script starts in frame 1; each command runs in the frame that saw the event */
    cJSON *script = cJSON_CreateArray();
    uint64_t cursor = 1;
    for (uint32_t i = 0; i < s->recorded.count; i++) {
        br_playtest_recorded *r = br_array_at(&s->recorded, i);
        if (r->frame > cursor) {
            br_pt_add_wait(script, r->frame - cursor);
            cursor = r->frame;
        }
        cJSON_AddItemToArray(script, r->cmd);
    }
    br_array_clear(&s->recorded);
    /* the replay ends in the frame the session ended */
    if (e->ticker.frame > cursor) {
        br_pt_add_wait(script, e->ticker.frame - cursor);
    }
    cJSON_AddItemToArray(script, br_pt_new_cmd("quit", NULL));
    bool ok = br_pt_save_script(e, s->record_path, script);
    cJSON_Delete(script);
    if (ok) {
        br_log_info("playtest: recorded %llu frames of input to '%s'", (unsigned long long)e->ticker.frame,
                    s->record_path);
    }
    return ok;
}

/* ---- lifetime */

static void br_pt_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_playtest_state *s = e->playtest;
    if (s->recording) {
        br_playtest_record_stop(e);
    }
    if (s->log_hooked) {
        SDL_SetLogOutputFunction(s->log_prev, s->log_prev_user);
    }
    SDL_DestroyMutex(s->log_lock);
    cJSON_Delete(s->script);
    br_array_free(&s->releases);
    br_array_free(&s->exposed);
    br_array_free(&s->recorded);
    br_free(s);
    e->playtest = NULL;
}

void br_playtest_register(br_engine *e)
{
    br_playtest_state *s = br_alloc(sizeof(br_playtest_state));
    if (s == NULL) {
        return;
    }
    s->log_lock = SDL_CreateMutex();
    if (s->log_lock == NULL) {
        br_log_error("playtest: cannot create mutex: %s", SDL_GetError());
        br_free(s);
        return;
    }
    e->playtest = s;
    br_array_init(&s->releases, sizeof(br_playtest_release));
    br_array_init(&s->exposed, sizeof(br_playtest_exposed));
    br_array_init(&s->recorded, sizeof(br_playtest_recorded));
    br_engine_on_shutdown(e, br_pt_shutdown, NULL);
    br_engine_add_event_handler(e, br_pt_record_event, NULL);
    br_system_add(e, "playtest.frame", BR_PHASE_INPUT, BR_PLAYTEST_PRIORITY, br_pt_system, NULL);
}
