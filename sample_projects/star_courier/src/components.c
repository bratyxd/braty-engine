/* components.c - every component's field list, so scene files, prefabs and tuning.json can fill
 * them. Runtime-only fields (intent, particle slots, ...) are simply not listed. */
#include "game.h"

static uint32_t sc_register(br_engine *e, const char *name, size_t size, const br_field_desc *fields,
                            uint32_t count)
{
    br_component_desc desc = { name, size, fields, count };
    return br_component_register(e, &desc);
}

void sc_components_register(br_engine *e, sc_game *g)
{
    static const br_field_desc body[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(sc_body, velocity) },
        { "spin", BR_FIELD_FLOAT, offsetof(sc_body, spin) },
        { "drag", BR_FIELD_FLOAT, offsetof(sc_body, drag) },
        { "wrap", BR_FIELD_BOOL, offsetof(sc_body, wrap) },
    };
    static const br_field_desc pilot[] = {
        { "thrust", BR_FIELD_FLOAT, offsetof(sc_pilot, thrust) },
        { "max_speed", BR_FIELD_FLOAT, offsetof(sc_pilot, max_speed) },
        { "radius", BR_FIELD_FLOAT, offsetof(sc_pilot, radius) },
        { "turn_rate", BR_FIELD_FLOAT, offsetof(sc_pilot, turn_rate) },
        { "cargo_icon", BR_FIELD_NODE, offsetof(sc_pilot, cargo_icon) },
    };
    static const br_field_desc crate[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(sc_crate, radius) },
        { "value", BR_FIELD_INT, offsetof(sc_crate, value) },
    };
    static const br_field_desc debris[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(sc_debris, radius) },
    };
    static const br_field_desc station[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(sc_station, radius) },
        { "halo", BR_FIELD_NODE, offsetof(sc_station, halo) },
    };
    static const br_field_desc emitter[] = {
        { "rate", BR_FIELD_FLOAT, offsetof(sc_emitter, rate) },
        { "life", BR_FIELD_FLOAT, offsetof(sc_emitter, life) },
        { "speed", BR_FIELD_VEC2, offsetof(sc_emitter, speed) },
        { "spread", BR_FIELD_FLOAT, offsetof(sc_emitter, spread) },
        { "direction", BR_FIELD_FLOAT, offsetof(sc_emitter, direction) },
        { "scale", BR_FIELD_VEC2, offsetof(sc_emitter, scale) },
        { "color_start", BR_FIELD_COLOR, offsetof(sc_emitter, color_start) },
        { "color_end", BR_FIELD_COLOR, offsetof(sc_emitter, color_end) },
        { "drag", BR_FIELD_FLOAT, offsetof(sc_emitter, drag) },
        { "follow", BR_FIELD_NODE, offsetof(sc_emitter, follow) },
        { "offset", BR_FIELD_VEC2, offsetof(sc_emitter, offset) },
        { "needs_thrust", BR_FIELD_BOOL, offsetof(sc_emitter, needs_thrust) },
    };
    static const br_field_desc pulse[] = {
        { "wave", BR_FIELD_FLOAT, offsetof(sc_pulse, wave) },
        { "speed", BR_FIELD_FLOAT, offsetof(sc_pulse, speed) },
        { "kick", BR_FIELD_FLOAT, offsetof(sc_pulse, kick) },
        { "decay", BR_FIELD_FLOAT, offsetof(sc_pulse, decay) },
    };
    static const br_field_desc blink[] = {
        { "period", BR_FIELD_FLOAT, offsetof(sc_blink, period) },
        { "min_alpha", BR_FIELD_FLOAT, offsetof(sc_blink, min_alpha) },
    };
    static const br_field_desc scroll[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(sc_scroll, velocity) },
    };
    static const br_field_desc tuning[] = {
        { "lives", BR_FIELD_INT, offsetof(sc_tuning, lives) },
        { "invulnerable_time", BR_FIELD_FLOAT, offsetof(sc_tuning, invulnerable_time) },
        { "gameover_delay", BR_FIELD_FLOAT, offsetof(sc_tuning, gameover_delay) },
        { "field_min", BR_FIELD_VEC2, offsetof(sc_tuning, field_min) },
        { "field_max", BR_FIELD_VEC2, offsetof(sc_tuning, field_max) },
        { "crates_on_field", BR_FIELD_INT, offsetof(sc_tuning, crates_on_field) },
        { "crate_value", BR_FIELD_INT, offsetof(sc_tuning, crate_value) },
        { "level_bonus", BR_FIELD_FLOAT, offsetof(sc_tuning, level_bonus) },
        { "deliveries_per_level", BR_FIELD_INT, offsetof(sc_tuning, deliveries_per_level) },
        { "station_move_time", BR_FIELD_FLOAT, offsetof(sc_tuning, station_move_time) },
        { "debris_interval", BR_FIELD_FLOAT, offsetof(sc_tuning, debris_interval) },
        { "debris_interval_min", BR_FIELD_FLOAT, offsetof(sc_tuning, debris_interval_min) },
        { "debris_interval_scale", BR_FIELD_FLOAT, offsetof(sc_tuning, debris_interval_scale) },
        { "debris_speed", BR_FIELD_VEC2, offsetof(sc_tuning, debris_speed) },
        { "debris_speed_scale", BR_FIELD_FLOAT, offsetof(sc_tuning, debris_speed_scale) },
        { "debris_max", BR_FIELD_INT, offsetof(sc_tuning, debris_max) },
        { "debris_small", BR_FIELD_ASSET, offsetof(sc_tuning, debris_small) },
        { "debris_medium", BR_FIELD_ASSET, offsetof(sc_tuning, debris_medium) },
        { "debris_large", BR_FIELD_ASSET, offsetof(sc_tuning, debris_large) },
        { "crate_prefab", BR_FIELD_ASSET, offsetof(sc_tuning, crate_prefab) },
        { "flash_time", BR_FIELD_FLOAT, offsetof(sc_tuning, flash_time) },
        { "flash_blur", BR_FIELD_FLOAT, offsetof(sc_tuning, flash_blur) },
        { "flash_color", BR_FIELD_COLOR, offsetof(sc_tuning, flash_color) },
        { "pickup_burst", BR_FIELD_INT, offsetof(sc_tuning, pickup_burst) },
        { "deliver_burst", BR_FIELD_INT, offsetof(sc_tuning, deliver_burst) },
        { "hit_burst", BR_FIELD_INT, offsetof(sc_tuning, hit_burst) },
    };
    g->id.body = sc_register(e, "body", sizeof(sc_body), body, BR_ARRAY_COUNT(body));
    g->id.pilot = sc_register(e, "pilot", sizeof(sc_pilot), pilot, BR_ARRAY_COUNT(pilot));
    g->id.crate = sc_register(e, "crate", sizeof(sc_crate), crate, BR_ARRAY_COUNT(crate));
    g->id.debris = sc_register(e, "debris", sizeof(sc_debris), debris, BR_ARRAY_COUNT(debris));
    g->id.station = sc_register(e, "station", sizeof(sc_station), station, BR_ARRAY_COUNT(station));
    g->id.emitter = sc_register(e, "emitter", sizeof(sc_emitter), emitter, BR_ARRAY_COUNT(emitter));
    g->id.pulse = sc_register(e, "pulse", sizeof(sc_pulse), pulse, BR_ARRAY_COUNT(pulse));
    g->id.blink = sc_register(e, "blink", sizeof(sc_blink), blink, BR_ARRAY_COUNT(blink));
    g->id.scroll = sc_register(e, "scroll", sizeof(sc_scroll), scroll, BR_ARRAY_COUNT(scroll));
    g->id.tuning = sc_register(e, "tuning", sizeof(sc_tuning), tuning, BR_ARRAY_COUNT(tuning));
}
