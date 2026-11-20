/* br_node_json_mesh: JSON for "mesh" and its variants (simple, plane, perspective, rope).
 *
 *   { "type": "mesh", "texture": "a.png", "positions": [0,0, 100,0, 0,100], "uvs": [0,0, 1,0, 0,1],
 *     "colors": ["red", "lime", "blue"], "indices": [0, 1, 2] }
 *   { "type": "mesh", "texture": "a.png", "plane": [10, 10] }
 *   { "type": "mesh", "texture": "a.png", "perspective": { "verts": [8, 8], "corners": [[0,0],[200,20],[200,180],[0,200]] } }
 *   { "type": "mesh", "texture": "rope.png", "rope": { "points": [0,0, 50,10, 100,0], "texture_scale": 0, "width": 20 } }
 * The variant is fixed when the node is created (factory); "width" and "corners" are re-applied
 * by from_json.
 */
#include "data/br_data_internal.h"

/* A flat [x, y, x, y, ...] array as points (br_alloc'd); count = pairs. NULL on a bad array. */
static br_vec2 *br_data_points(br_engine *e, const cJSON *obj, const char *key, uint32_t *count)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    *count = 0;
    if (v == NULL) {
        return NULL;
    }
    int n = cJSON_IsArray(v) ? cJSON_GetArraySize(v) : 0;
    br_vec2 *points = n >= 2 && n % 2 == 0 ? br_alloc(sizeof(br_vec2) * (size_t)(n / 2)) : NULL;
    for (int i = 0; points != NULL && i < n / 2; i++) {
        const cJSON *x = cJSON_GetArrayItem(v, i * 2);
        const cJSON *y = cJSON_GetArrayItem(v, i * 2 + 1);
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) {
            br_free(points);
            points = NULL;
        } else {
            points[i] = br_vec2_make((float)x->valuedouble, (float)y->valuedouble);
        }
    }
    if (points == NULL) {
        br_data_error(e, key, "expected a flat array of numbers [x0, y0, x1, y1, ...]");
        return NULL;
    }
    *count = (uint32_t)(n / 2);
    return points;
}

/* Most vertices per side of a plane / perspective grid (1024 x 1024 is already a million). */
#define BR_DATA_MAX_GRID 1024

/* "plane" / "verts": [columns, rows] of vertices, each a whole number 2..BR_DATA_MAX_GRID. */
static bool br_data_grid(br_engine *e, const cJSON *obj, const char *key, uint32_t *vx, uint32_t *vy)
{
    br_vec2 v;
    if (!br_data_get_vec2(e, obj, key, &v)) {
        return false;
    }
    size_t mark = br_data_path_push_key(e, key);
    bool ok = br_data_value_count(e, v.x, 2, BR_DATA_MAX_GRID, vx) && br_data_value_count(e, v.y, 2, BR_DATA_MAX_GRID, vy);
    br_data_path_pop(e, mark);
    return ok;
}

/* Plain geometry: positions (+ uvs, colours, indices). */
static bool br_data_mesh_geometry(br_engine *e, br_node n, const cJSON *json)
{
    uint32_t count = 0;
    uint32_t uv_count = 0;
    br_vec2 *positions = br_data_points(e, json, "positions", &count);
    if (positions == NULL) {
        return false;
    }
    br_vec2 *uvs = br_data_points(e, json, "uvs", &uv_count);
    if (uvs != NULL && uv_count != count) {
        br_data_error(e, "uvs", "needs one [u, v] per position");
        br_free(uvs);
        uvs = NULL;
    }
    if (uvs == NULL) {
        uvs = br_alloc(sizeof(br_vec2) * count); /* zero uvs: untextured look */
    }
    uint32_t *colors = NULL;
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(json, "colors");
    if (cv != NULL) {
        size_t mark = br_data_path_push_key(e, "colors");
        if (!cJSON_IsArray(cv) || (uint32_t)cJSON_GetArraySize(cv) != count) {
            br_data_errorf(e, "needs one colour per position");
        } else {
            colors = br_alloc(sizeof(uint32_t) * count);
            for (uint32_t i = 0; colors != NULL && i < count; i++) {
                br_color c = br_color_make(1, 1, 1, 1);
                size_t im = br_data_path_push_index(e, (int)i);
                br_data_value_color(e, cJSON_GetArrayItem(cv, (int)i), &c);
                br_data_path_pop(e, im);
                colors[i] = br_render_pack_color(c, 1.0f);
            }
        }
        br_data_path_pop(e, mark);
    }
    uint32_t *indices = NULL;
    uint32_t index_count = 0;
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(json, "indices");
    if (iv != NULL) {
        int ni = cJSON_IsArray(iv) ? cJSON_GetArraySize(iv) : 0;
        indices = ni > 0 && ni % 3 == 0 ? br_alloc(sizeof(uint32_t) * (size_t)ni) : NULL;
        for (int i = 0; indices != NULL && i < ni; i++) {
            const cJSON *item = cJSON_GetArrayItem(iv, i);
            /* compared as doubles: casting 1e20 to uint32_t first would be undefined */
            double index = cJSON_IsNumber(item) ? item->valuedouble : -1.0;
            if (!(index >= 0.0 && index < (double)count && index == SDL_floor(index))) {
                br_free(indices);
                indices = NULL;
            } else {
                indices[i] = (uint32_t)item->valuedouble;
            }
        }
        if (indices == NULL) {
            br_data_error(e, "indices", "expected triangles: a multiple of 3 vertex indices in range");
        } else {
            index_count = (uint32_t)ni;
        }
    }
    bool ok = br_mesh_set_geometry(e, n, positions, uvs, colors, count, indices, index_count);
    br_free(positions);
    br_free(uvs);
    br_free(colors);
    br_free(indices);
    return ok;
}

static br_node br_data_mesh_create(br_engine *e, const cJSON *json)
{
    br_texture texture = br_texture_white(e);
    br_data_get_texture(e, json, "texture", &texture);
    uint32_t vx = 0;
    uint32_t vy = 0;
    const cJSON *rope = cJSON_GetObjectItemCaseSensitive(json, "rope");
    const cJSON *perspective = cJSON_GetObjectItemCaseSensitive(json, "perspective");
    if (rope != NULL) {
        size_t mark = br_data_path_push_key(e, "rope");
        uint32_t count = 0;
        float scale = 0.0f;
        br_vec2 *points = br_data_points(e, rope, "points", &count);
        br_data_get_float(e, rope, "texture_scale", &scale);
        br_node n = { 0, 0 };
        if (points == NULL || count < 2) {
            br_data_errorf(e, "a rope needs \"points\" with at least 2 points");
        } else {
            n = br_mesh_rope_create(e, texture, points, count, scale);
        }
        br_free(points);
        br_data_path_pop(e, mark);
        return n;
    }
    if (perspective != NULL) {
        size_t mark = br_data_path_push_key(e, "perspective");
        if (!br_data_grid(e, perspective, "verts", &vx, &vy)) {
            vx = 2;
            vy = 2;
        }
        br_data_path_pop(e, mark);
        return br_perspective_mesh_create(e, texture, vx, vy);
    }
    if (br_data_grid(e, json, "plane", &vx, &vy)) {
        return br_mesh_plane_create(e, texture, vx, vy);
    }
    br_node n = br_node_create(e, br_mesh_type(e));
    br_mesh_set_texture(e, n, texture);
    return n;
}

static bool br_data_mesh_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    if (cJSON_GetObjectItemCaseSensitive(json, "positions") != NULL) {
        br_data_mesh_geometry(e, n, json);
    }
    const cJSON *rope = cJSON_GetObjectItemCaseSensitive(json, "rope");
    float width;
    if (rope != NULL && br_data_get_float(e, rope, "width", &width)) {
        br_mesh_rope_set_width(e, n, width);
    }
    const cJSON *perspective = cJSON_GetObjectItemCaseSensitive(json, "perspective");
    const cJSON *corners = cJSON_GetObjectItemCaseSensitive(perspective, "corners");
    if (corners != NULL) {
        size_t mark = br_data_path_push_key(e, "perspective");
        size_t cm = br_data_path_push_key(e, "corners");
        br_vec2 c[4];
        bool ok = cJSON_IsArray(corners) && cJSON_GetArraySize(corners) == 4;
        for (int i = 0; ok && i < 4; i++) {
            size_t im = br_data_path_push_index(e, i);
            ok = br_data_value_vec2(e, cJSON_GetArrayItem(corners, i), &c[i]);
            br_data_path_pop(e, im);
        }
        if (ok) {
            br_perspective_mesh_set_corners(e, n, c[0], c[1], c[2], c[3]);
        } else {
            br_data_errorf(e, "expected 4 corners [[x, y], ...]: top-left, top-right, bottom-right, bottom-left");
        }
        br_data_path_pop(e, cm);
        br_data_path_pop(e, mark);
    }
    return true;
}

void br_node_json_mesh_register(br_engine *e)
{
    uint32_t type = br_mesh_type(e);
    br_node_type_set_from_json(e, type, br_data_mesh_from_json);
    br_data_add_type(e, "mesh", type, br_data_mesh_create, NULL);
    br_data_add_type(e, "mesh_simple", type, br_data_mesh_create, NULL);
    br_data_add_type(e, "mesh_plane", type, br_data_mesh_create, NULL);
    br_data_add_type(e, "perspective_mesh", type, br_data_mesh_create, NULL);
    br_data_add_type(e, "mesh_rope", type, br_data_mesh_create, NULL);
}
