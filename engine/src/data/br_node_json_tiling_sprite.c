/* br_node_json_tiling_sprite: JSON for "tiling_sprite".
 *
 *   { "type": "tiling_sprite", "texture": "bg.png", "width": 800, "height": 600,
 *     "tile_position": [0, 0], "tile_scale": [2, 2], "tile_rotation": 0.3 }
 */
#include "data/br_data_internal.h"

/* br_tiling_sprite_create sets the defaults (tile scale 1, clamp margin 0.5) that a bare node lacks. */
static br_node br_data_tiling_create(br_engine *e, const cJSON *json)
{
    (void)json;
    return br_tiling_sprite_create(e, br_texture_white(e), 0.0f, 0.0f);
}

static bool br_data_tiling_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_texture texture;
    if (br_data_get_texture(e, json, "texture", &texture)) {
        br_tiling_sprite *ts = br_tiling_sprite_get(e, n);
        br_texture_info info;
        br_texture_get_info(e, texture, &info);
        ts->texture = texture;
        ts->width = info.width;
        ts->height = info.height;
    }
    br_tiling_sprite *ts = br_tiling_sprite_get(e, n);
    br_data_get_float(e, json, "width", &ts->width);
    br_data_get_float(e, json, "height", &ts->height);
    br_data_get_vec2(e, json, "anchor", &ts->anchor);
    br_data_get_vec2(e, json, "tile_position", &ts->tile_position);
    br_data_get_vec2(e, json, "tile_scale", &ts->tile_scale);
    br_data_get_float(e, json, "tile_rotation", &ts->tile_rotation);
    br_data_get_float(e, json, "clamp_margin", &ts->clamp_margin);
    return true;
}

static void br_data_tiling_write(br_engine *e, br_node n, cJSON *out)
{
    const br_tiling_sprite *ts = br_tiling_sprite_get(e, n);
    br_data_json_set(out, "width", br_data_number(ts->width));
    br_data_json_set(out, "height", br_data_number(ts->height));
    br_data_json_set(out, "anchor", br_data_vec2_json(ts->anchor));
    br_data_json_set(out, "tile_position", br_data_vec2_json(ts->tile_position));
    br_data_json_set(out, "tile_scale", br_data_vec2_json(ts->tile_scale));
    br_data_json_set(out, "tile_rotation", br_data_number(ts->tile_rotation));
    br_data_json_set(out, "clamp_margin", br_data_number(ts->clamp_margin));
}

void br_node_json_tiling_sprite_register(br_engine *e)
{
    uint32_t type = br_tiling_sprite_type(e);
    br_node_type_set_from_json(e, type, br_data_tiling_from_json);
    br_data_add_type(e, "tiling_sprite", type, br_data_tiling_create, br_data_tiling_write);
}
