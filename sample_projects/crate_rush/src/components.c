/* components.c - every component's field list, so glTF extras, scene files and tuning.json can
 * fill them. Runtime-only fields are simply not listed. */
#include "game.h"

/* Enum names, index = value (cr_action / cr_setting_key in game.h). */
static const char *const cr_action_names[CR_ACTION_COUNT] = {
    "none", "play", "levels", "main", "settings", "close_settings", "setting", "quit", "resume", "restart",
    "retry", "next", "menu", "edit_name"
};
static const char *const cr_setting_names[CR_SETTING_COUNT] = {
    "language", "vsync", "fps", "volume", "profiler", "bind_jump"
};

static uint32_t cr_register(br_engine *e, const char *name, size_t size, const br_field_desc *fields, uint32_t count)
{
    br_component_desc desc = { name, size, fields, count };
    return br_component_register(e, &desc);
}

void cr_components_register(br_engine *e, cr_game *g)
{
    /* 3D: level extras ("spawn_point": {"yaw": 180}, "crate.value": 10, "hazard": {...}, "sign.text": ...) */
    static const br_field_desc spawn_point[] = {
        { "yaw", BR_FIELD_FLOAT, offsetof(cr_spawn_point, yaw) },
    };
    static const br_field_desc crate[] = {
        { "value", BR_FIELD_INT, offsetof(cr_crate, value) },
    };
    static const br_field_desc hazard[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(cr_hazard, radius) },
        { "move", BR_FIELD_VEC3, offsetof(cr_hazard, move) },
        { "speed", BR_FIELD_FLOAT, offsetof(cr_hazard, speed) },
        { "spin", BR_FIELD_FLOAT, offsetof(cr_hazard, spin) },
        { "phase", BR_FIELD_FLOAT, offsetof(cr_hazard, phase) },
    };
    static const br_field_desc sign[] = {
        { "text", BR_FIELD_TEXT, offsetof(cr_sign, text) },
        { "hint", BR_FIELD_TEXT, offsetof(cr_sign, hint) },
        { "radius", BR_FIELD_FLOAT, offsetof(cr_sign, radius) },
    };
    /* runtime-only 3D components: no fields to fill, but play-tests can read them by name */
    static const br_field_desc player[] = {
        { "position", BR_FIELD_VEC3, offsetof(cr_player, position) },
        { "velocity", BR_FIELD_VEC3, offsetof(cr_player, velocity) },
        { "grounded", BR_FIELD_BOOL, offsetof(cr_player, grounded) },
    };
    static const br_field_desc popup[] = {
        { "life", BR_FIELD_FLOAT, offsetof(cr_popup, life) },
    };

    /* 2D */
    static const br_field_desc level[] = {
        { "title", BR_FIELD_STRING, offsetof(cr_level, title) },
        { "time_limit", BR_FIELD_FLOAT, offsetof(cr_level, time_limit) },
        { "next", BR_FIELD_STRING, offsetof(cr_level, next) },
        { "footsteps", BR_FIELD_STRING, offsetof(cr_level, footsteps) },
        { "kill_y", BR_FIELD_FLOAT, offsetof(cr_level, kill_y) },
        { "index", BR_FIELD_INT, offsetof(cr_level, index) },
    };
    static const br_field_desc button[] = {
        { "action", BR_FIELD_ENUM, offsetof(cr_button, action) },
        { "arg", BR_FIELD_STRING, offsetof(cr_button, arg) },
        { "order", BR_FIELD_INT, offsetof(cr_button, order) },
    };
    static const br_field_desc setting[] = {
        { "key", BR_FIELD_ENUM, offsetof(cr_setting, key) },
    };
    static const br_field_desc prompt[] = {
        { "action", BR_FIELD_STRING, offsetof(cr_prompt, action) },
        { "key", BR_FIELD_STRING, offsetof(cr_prompt, key) },
    };
    static const br_field_desc counter[] = {
        { "value", BR_FIELD_FLOAT, offsetof(cr_counter, value) },
        { "format", BR_FIELD_STRING, offsetof(cr_counter, format) },
    };
    static const br_field_desc tuning[] = {
        { "run_speed", BR_FIELD_FLOAT, offsetof(cr_tuning, run_speed) },
        { "accel", BR_FIELD_FLOAT, offsetof(cr_tuning, accel) },
        { "air_accel", BR_FIELD_FLOAT, offsetof(cr_tuning, air_accel) },
        { "decel", BR_FIELD_FLOAT, offsetof(cr_tuning, decel) },
        { "gravity", BR_FIELD_FLOAT, offsetof(cr_tuning, gravity) },
        { "jump_speed", BR_FIELD_FLOAT, offsetof(cr_tuning, jump_speed) },
        { "max_fall", BR_FIELD_FLOAT, offsetof(cr_tuning, max_fall) },
        { "coyote_time", BR_FIELD_FLOAT, offsetof(cr_tuning, coyote_time) },
        { "jump_buffer", BR_FIELD_FLOAT, offsetof(cr_tuning, jump_buffer) },
        { "radius", BR_FIELD_FLOAT, offsetof(cr_tuning, radius) },
        { "height", BR_FIELD_FLOAT, offsetof(cr_tuning, height) },
        { "turn_rate", BR_FIELD_FLOAT, offsetof(cr_tuning, turn_rate) },
        { "run_anim_speed", BR_FIELD_FLOAT, offsetof(cr_tuning, run_anim_speed) },
        { "bloom_threshold", BR_FIELD_FLOAT, offsetof(cr_tuning, bloom_threshold) },
        { "bloom_intensity", BR_FIELD_FLOAT, offsetof(cr_tuning, bloom_intensity) },
        { "hit_penalty", BR_FIELD_FLOAT, offsetof(cr_tuning, hit_penalty) },
        { "fall_penalty", BR_FIELD_FLOAT, offsetof(cr_tuning, fall_penalty) },
        { "knockback", BR_FIELD_FLOAT, offsetof(cr_tuning, knockback) },
        { "invulnerable_time", BR_FIELD_FLOAT, offsetof(cr_tuning, invulnerable_time) },
        { "pickup_radius", BR_FIELD_FLOAT, offsetof(cr_tuning, pickup_radius) },
        { "camera_damping", BR_FIELD_FLOAT, offsetof(cr_tuning, camera_damping) },
        { "camera_distance", BR_FIELD_FLOAT, offsetof(cr_tuning, camera_distance) },
        { "shake_hit", BR_FIELD_FLOAT, offsetof(cr_tuning, shake_hit) },
        { "shake_fall", BR_FIELD_FLOAT, offsetof(cr_tuning, shake_fall) },
        { "shake_pickup", BR_FIELD_FLOAT, offsetof(cr_tuning, shake_pickup) },
        { "pickup_ease", BR_FIELD_ENUM, offsetof(cr_tuning, pickup_ease) },
        { "popup_ease", BR_FIELD_ENUM, offsetof(cr_tuning, popup_ease) },
        { "counter_ease", BR_FIELD_ENUM, offsetof(cr_tuning, counter_ease) },
        { "focus_ease", BR_FIELD_ENUM, offsetof(cr_tuning, focus_ease) },
        { "pickup_time", BR_FIELD_FLOAT, offsetof(cr_tuning, pickup_time) },
        { "popup_time", BR_FIELD_FLOAT, offsetof(cr_tuning, popup_time) },
        { "popup_rise", BR_FIELD_FLOAT, offsetof(cr_tuning, popup_rise) },
        { "counter_time", BR_FIELD_FLOAT, offsetof(cr_tuning, counter_time) },
        { "footstep_walk", BR_FIELD_VEC2, offsetof(cr_tuning, footstep_walk) },
        { "footstep_sprint", BR_FIELD_VEC2, offsetof(cr_tuning, footstep_sprint) },
        { "end_delay", BR_FIELD_FLOAT, offsetof(cr_tuning, end_delay) },
        { "time_bonus", BR_FIELD_INT, offsetof(cr_tuning, time_bonus) },
        { "character", BR_FIELD_ASSET, offsetof(cr_tuning, character) },
        { "animgraph", BR_FIELD_ASSET, offsetof(cr_tuning, animgraph) },
        { "sdf_font", BR_FIELD_ASSET, offsetof(cr_tuning, sdf_font) },
        { "plate_texture", BR_FIELD_STRING, offsetof(cr_tuning, plate_texture) },
    };
    g->id.spawn_point = cr_register(e, "spawn_point", sizeof(cr_spawn_point), spawn_point, BR_ARRAY_COUNT(spawn_point));
    g->id.crate = cr_register(e, "crate", sizeof(cr_crate), crate, BR_ARRAY_COUNT(crate));
    g->id.hazard = cr_register(e, "hazard", sizeof(cr_hazard), hazard, BR_ARRAY_COUNT(hazard));
    g->id.sign = cr_register(e, "sign", sizeof(cr_sign), sign, BR_ARRAY_COUNT(sign));
    g->id.player = cr_register(e, "player", sizeof(cr_player), player, BR_ARRAY_COUNT(player));
    g->id.popup = cr_register(e, "popup", sizeof(cr_popup), popup, BR_ARRAY_COUNT(popup));
    g->id.level = cr_register(e, "level", sizeof(cr_level), level, BR_ARRAY_COUNT(level));
    g->id.button = cr_register(e, "button", sizeof(cr_button), button, BR_ARRAY_COUNT(button));
    g->id.setting = cr_register(e, "setting", sizeof(cr_setting), setting, BR_ARRAY_COUNT(setting));
    g->id.prompt = cr_register(e, "prompt", sizeof(cr_prompt), prompt, BR_ARRAY_COUNT(prompt));
    g->id.counter = cr_register(e, "counter", sizeof(cr_counter), counter, BR_ARRAY_COUNT(counter));
    g->id.tuning = cr_register(e, "tuning", sizeof(cr_tuning), tuning, BR_ARRAY_COUNT(tuning));

    /* enum fields read names: "action": "play", "key": "vsync", "pickup_ease": "out_back" */
    br_component_set_enum(e, g->id.button, "action", cr_action_names, CR_ACTION_COUNT);
    br_component_set_enum(e, g->id.setting, "key", cr_setting_names, CR_SETTING_COUNT);
    const char *eases[BR_EASE_COUNT];
    for (int i = 0; i < BR_EASE_COUNT; i++) {
        eases[i] = br_ease_name((br_ease_type)i);
    }
    br_component_set_enum(e, g->id.tuning, "pickup_ease", eases, BR_EASE_COUNT);
    br_component_set_enum(e, g->id.tuning, "popup_ease", eases, BR_EASE_COUNT);
    br_component_set_enum(e, g->id.tuning, "counter_ease", eases, BR_EASE_COUNT);
    br_component_set_enum(e, g->id.tuning, "focus_ease", eases, BR_EASE_COUNT);
}
