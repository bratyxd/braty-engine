/* br_node_json_nine_slice: JSON for "nine_slice" (alias "nine_slice_sprite").
 *
 *   { "type": "nine_slice", "texture": "ui/panel.png", "borders": [12, 12, 12, 12],
 *     "width": 200, "height": 120, "edge_fill": "tile", "center_fill": "stretch" }
 * Without "borders", a spritesheet frame's 9-slice borders are used when it has them.
 */
#include "data/br_data_internal.h"

static const br_data_enum br_fill_names[] = {
    { "stretch", BR_NINE_SLICE_STRETCH },
    { "tile", BR_NINE_SLICE_TILE },
};

/* Borders a spritesheet frame carries ("sheet.json#frame" references only). */
static bool br_data_sheet_borders(br_engine *e, const cJSON *json, float out[4])
{
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(json, "texture");
    const char *hash = cJSON_IsString(ref) ? SDL_strchr(ref->valuestring, '#') : NULL;
    char sheet_path[BR_DATA_PATH_SIZE];
    if (hash == NULL || (size_t)(hash - ref->valuestring) >= sizeof(sheet_path)) {
        return false;
    }
    SDL_memcpy(sheet_path, ref->valuestring, (size_t)(hash - ref->valuestring));
    sheet_path[hash - ref->valuestring] = '\0';
    const br_spritesheet *sheet = br_assets_get(e, sheet_path);
    return sheet != NULL && br_spritesheet_frame_borders(e, sheet, hash + 1, out);
}

static bool br_data_nine_slice_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_texture texture;
    if (br_data_get_texture(e, json, "texture", &texture)) {
        br_nine_slice *ns = br_nine_slice_get(e, n);
        br_texture_info info;
        br_texture_get_info(e, texture, &info);
        ns->texture = texture;
        ns->width = info.width;
        ns->height = info.height;
        float borders[4];
        if (br_data_sheet_borders(e, json, borders)) {
            br_nine_slice_set_borders(e, n, borders[0], borders[1], borders[2], borders[3]);
        }
    }
    const cJSON *borders = cJSON_GetObjectItemCaseSensitive(json, "borders");
    if (borders != NULL) {
        float b[4];
        bool ok = cJSON_IsArray(borders) && cJSON_GetArraySize(borders) == 4;
        for (int i = 0; ok && i < 4; i++) {
            ok = cJSON_IsNumber(cJSON_GetArrayItem(borders, i));
            b[i] = ok ? (float)cJSON_GetArrayItem(borders, i)->valuedouble : 0.0f;
        }
        if (ok) {
            br_nine_slice_set_borders(e, n, b[0], b[1], b[2], b[3]);
        } else {
            br_data_error(e, "borders", "expected [left, top, right, bottom]");
        }
    }
    br_nine_slice *ns = br_nine_slice_get(e, n);
    br_data_get_float(e, json, "width", &ns->width);
    br_data_get_float(e, json, "height", &ns->height);
    br_data_get_vec2(e, json, "anchor", &ns->anchor);
    int fill;
    if (br_data_get_enum(e, json, "edge_fill", br_fill_names, BR_ARRAY_COUNT(br_fill_names), &fill)) {
        ns->edge_fill = (br_nine_slice_fill)fill;
    }
    if (br_data_get_enum(e, json, "center_fill", br_fill_names, BR_ARRAY_COUNT(br_fill_names), &fill)) {
        ns->center_fill = (br_nine_slice_fill)fill;
    }
    br_data_get_bool(e, json, "hide_center", &ns->hide_center);
    return true;
}

static void br_data_nine_slice_write(br_engine *e, br_node n, cJSON *out)
{
    const br_nine_slice *ns = br_nine_slice_get(e, n);
    cJSON *borders = cJSON_CreateArray();
    cJSON_AddItemToArray(borders, br_data_number(ns->left));
    cJSON_AddItemToArray(borders, br_data_number(ns->top));
    cJSON_AddItemToArray(borders, br_data_number(ns->right));
    cJSON_AddItemToArray(borders, br_data_number(ns->bottom));
    br_data_json_set(out, "borders", borders);
    br_data_json_set(out, "width", br_data_number(ns->width));
    br_data_json_set(out, "height", br_data_number(ns->height));
    br_data_json_set(out, "anchor", br_data_vec2_json(ns->anchor));
    br_data_json_set(out, "edge_fill", cJSON_CreateString(br_data_enum_name(br_fill_names, 2, (int)ns->edge_fill)));
    br_data_json_set(out, "center_fill", cJSON_CreateString(br_data_enum_name(br_fill_names, 2, (int)ns->center_fill)));
    br_data_json_set(out, "hide_center", cJSON_CreateBool(ns->hide_center));
}

void br_node_json_nine_slice_register(br_engine *e)
{
    uint32_t type = br_nine_slice_type(e);
    br_node_type_set_from_json(e, type, br_data_nine_slice_from_json);
    br_data_add_type(e, "nine_slice", type, NULL, br_data_nine_slice_write);
    br_data_add_type(e, "nine_slice_sprite", type, NULL, br_data_nine_slice_write);
}
