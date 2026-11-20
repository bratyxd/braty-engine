/* br_node_json_sprite: JSON for "sprite" and "animated_sprite" (a sprite with frames).
 *
 *   { "type": "sprite", "texture": "atlas.json#hero.png", "anchor": [0.5, 0.5], "width": 64 }
 *   { "type": "animated_sprite", "animation": "atlas.json#walk", "fps": 12, "loop": true }
 */
#include "data/br_data_internal.h"

/* One "frames" entry: a texture reference or { "texture": ..., "duration": seconds }. */
static bool br_data_read_frame(br_engine *e, const cJSON *item, float default_duration, br_anim_frame *out)
{
    out->duration = default_duration;
    if (cJSON_IsString(item)) {
        out->texture = br_data_texture(e, item->valuestring);
        return true;
    }
    if (cJSON_IsObject(item) && br_data_get_texture(e, item, "texture", &out->texture)) {
        br_data_get_float(e, item, "duration", &out->duration);
        return true;
    }
    br_data_errorf(e, "expected a texture or { \"texture\", \"duration\" }");
    return false;
}

static br_node br_data_animated_create(br_engine *e, const cJSON *json)
{
    br_node none = { 0, 0 };
    float fps = 10.0f;
    br_data_get_float(e, json, "fps", &fps);
    float duration = fps > 0.0f ? 1.0f / fps : 0.1f;
    char ref[BR_DATA_PATH_SIZE];

    if (br_data_get_string(e, json, "gif", ref, sizeof(ref))) {
        const br_frame_set *gif = br_frame_set_load(e, ref);
        if (gif == NULL) {
            br_data_error(e, "gif", "GIF could not be loaded");
            return none;
        }
        return br_animated_sprite_create(e, gif->frames, gif->count);
    }
    if (br_data_get_string(e, json, "animation", ref, sizeof(ref))) {
        char *hash = SDL_strchr(ref, '#');
        br_spritesheet *sheet = NULL;
        if (hash != NULL) {
            *hash = '\0';
            sheet = br_spritesheet_load(e, ref);
        }
        const br_texture *textures = NULL;
        uint32_t count = sheet != NULL ? br_spritesheet_animation(e, sheet, hash + 1, &textures) : 0;
        if (count == 0) {
            br_data_error(e, "animation", "expected \"sheet.json#animation\" naming a spritesheet animation");
            return none;
        }
        return br_animated_sprite_create_uniform(e, textures, count, fps);
    }
    const cJSON *frames = cJSON_GetObjectItemCaseSensitive(json, "frames");
    int count = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : 0;
    if (count == 0) {
        br_data_errorf(e, "an animated_sprite needs \"frames\", \"animation\" or \"gif\"; node skipped");
        return none;
    }
    br_anim_frame *list = br_alloc(sizeof(br_anim_frame) * (size_t)count);
    if (list == NULL) {
        return none;
    }
    uint32_t used = 0;
    size_t mark = br_data_path_push_key(e, "frames");
    for (int i = 0; i < count; i++) {
        size_t im = br_data_path_push_index(e, i);
        if (br_data_read_frame(e, cJSON_GetArrayItem(frames, i), duration, &list[used])) {
            used++;
        }
        br_data_path_pop(e, im);
    }
    br_data_path_pop(e, mark);
    br_node n = used > 0 ? br_animated_sprite_create(e, list, used) : none;
    br_free(list);
    return n;
}

static bool br_data_sprite_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_texture texture;
    if (br_data_get_texture(e, json, "texture", &texture)) {
        br_sprite_set_texture(e, n, texture);
        br_texture_info info;
        if (cJSON_GetObjectItemCaseSensitive(json, "anchor") == NULL && br_texture_get_info(e, texture, &info)) {
            br_sprite_set_anchor(e, n, info.frame.default_anchor.x, info.frame.default_anchor.y);
        }
    }
    br_vec2 anchor;
    if (br_data_get_vec2(e, json, "anchor", &anchor)) {
        br_sprite_set_anchor(e, n, anchor.x, anchor.y);
    }
    bool b;
    if (br_data_get_bool(e, json, "round_pixels", &b)) {
        br_sprite_set_round_pixels(e, n, b);
    }
    br_vec2 size = br_sprite_size(e, n);
    bool sized = br_data_get_float(e, json, "width", &size.x);
    sized = br_data_get_float(e, json, "height", &size.y) || sized;
    if (sized) {
        br_sprite_set_size(e, n, size.x, size.y);
    }

    if (!br_animated_sprite_is(e, n)) {
        return true;
    }
    float f;
    uint32_t frame = 0;
    if (br_data_get_float(e, json, "speed", &f)) {
        br_animated_sprite_set_speed(e, n, f);
    }
    if (br_data_get_bool(e, json, "loop", &b)) {
        br_animated_sprite_set_loop(e, n, b);
    }
    if (br_data_get_bool(e, json, "update_anchor", &b)) {
        br_animated_sprite_set_update_anchor(e, n, b);
    }
    bool playing = true; /* data-driven animations play unless told not to */
    br_data_get_bool(e, json, "playing", &playing);
    if (!br_data_get_count(e, json, "frame", 0, UINT32_MAX, &frame)) {
        frame = 0;
    }
    if (playing) {
        br_animated_sprite_goto_and_play(e, n, frame);
    } else {
        br_animated_sprite_goto_and_stop(e, n, frame);
    }
    return true;
}

static void br_data_sprite_write(br_engine *e, br_node n, cJSON *out)
{
    br_vec2 anchor = br_sprite_anchor(e, n);
    br_data_json_set(out, "anchor", br_data_vec2_json(anchor));
    /* the size is in the live scale */
    cJSON_DeleteItemFromObjectCaseSensitive(out, "width");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "height");
    if (br_animated_sprite_is(e, n)) {
        br_data_json_set(out, "playing", cJSON_CreateBool(br_animated_sprite_playing(e, n)));
    }
}

void br_node_json_sprite_register(br_engine *e)
{
    uint32_t type = br_sprite_type(e);
    br_node_type_set_from_json(e, type, br_data_sprite_from_json);
    br_data_add_type(e, "sprite", type, NULL, br_data_sprite_write);
    br_data_add_type(e, "animated_sprite", type, br_data_animated_create, br_data_sprite_write);
}
