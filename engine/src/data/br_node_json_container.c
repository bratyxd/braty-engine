/* br_node_json_container: JSON for the PixiJS Container fields every node has - transform,
 * appearance, culling, render groups/layers, masks, filters, blend modes, pointer events. */
#include "data/br_data_internal.h"

static const br_data_enum br_blend_names[] = {
    { "inherit", BR_BLEND_INHERIT },
    { "normal", BR_BLEND_NORMAL },
    { "add", BR_BLEND_ADD },
    { "multiply", BR_BLEND_MULTIPLY },
    { "screen", BR_BLEND_SCREEN },
    { "erase", BR_BLEND_ERASE },
    { "none", BR_BLEND_NONE },
    { "normal_npm", BR_BLEND_NORMAL_NPM },
    { "add_npm", BR_BLEND_ADD_NPM },
    { "screen_npm", BR_BLEND_SCREEN_NPM },
    { "color", BR_BLEND_COLOR },
    { "color_burn", BR_BLEND_COLOR_BURN },
    { "color_dodge", BR_BLEND_COLOR_DODGE },
    { "darken", BR_BLEND_DARKEN },
    { "difference", BR_BLEND_DIFFERENCE },
    { "divide", BR_BLEND_DIVIDE },
    { "exclusion", BR_BLEND_EXCLUSION },
    { "hard_light", BR_BLEND_HARD_LIGHT },
    { "hard_mix", BR_BLEND_HARD_MIX },
    { "lighten", BR_BLEND_LIGHTEN },
    { "linear_burn", BR_BLEND_LINEAR_BURN },
    { "linear_dodge", BR_BLEND_LINEAR_DODGE },
    { "linear_light", BR_BLEND_LINEAR_LIGHT },
    { "luminosity", BR_BLEND_LUMINOSITY },
    { "negation", BR_BLEND_NEGATION },
    { "overlay", BR_BLEND_OVERLAY },
    { "pin_light", BR_BLEND_PIN_LIGHT },
    { "saturation", BR_BLEND_SATURATION },
    { "soft_light", BR_BLEND_SOFT_LIGHT },
    { "subtract", BR_BLEND_SUBTRACT },
    { "vivid_light", BR_BLEND_VIVID_LIGHT },
};

static const br_data_enum br_event_mode_names[] = {
    { "passive", BR_EVENT_MODE_PASSIVE }, { "none", BR_EVENT_MODE_NONE },       { "auto", BR_EVENT_MODE_AUTO },
    { "static", BR_EVENT_MODE_STATIC },   { "dynamic", BR_EVENT_MODE_DYNAMIC },
};

static const br_data_enum br_mask_names[] = {
    { "scissor", BR_MASK_SCISSOR },
    { "stencil", BR_MASK_STENCIL },
    { "alpha", BR_MASK_ALPHA },
    { "color", BR_MASK_COLOR },
};

static const br_data_enum br_cursor_names[] = {
    { "none", -1 },
    { "default", SDL_SYSTEM_CURSOR_DEFAULT },
    { "text", SDL_SYSTEM_CURSOR_TEXT },
    { "wait", SDL_SYSTEM_CURSOR_WAIT },
    { "crosshair", SDL_SYSTEM_CURSOR_CROSSHAIR },
    { "progress", SDL_SYSTEM_CURSOR_PROGRESS },
    { "move", SDL_SYSTEM_CURSOR_MOVE },
    { "not_allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED },
    { "pointer", SDL_SYSTEM_CURSOR_POINTER },
    { "ew_resize", SDL_SYSTEM_CURSOR_EW_RESIZE },
    { "ns_resize", SDL_SYSTEM_CURSOR_NS_RESIZE },
    { "nwse_resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE },
    { "nesw_resize", SDL_SYSTEM_CURSOR_NESW_RESIZE },
};

static const br_data_enum br_event_names[] = {
    { "pointerdown", BR_EVENT_POINTER_DOWN },
    { "pointerup", BR_EVENT_POINTER_UP },
    { "pointerupoutside", BR_EVENT_POINTER_UP_OUTSIDE },
    { "pointermove", BR_EVENT_POINTER_MOVE },
    { "pointerover", BR_EVENT_POINTER_OVER },
    { "pointerout", BR_EVENT_POINTER_OUT },
    { "pointerenter", BR_EVENT_POINTER_ENTER },
    { "pointerleave", BR_EVENT_POINTER_LEAVE },
    { "tap", BR_EVENT_TAP },
    { "click", BR_EVENT_CLICK },
    { "rightclick", BR_EVENT_RIGHT_CLICK },
    { "wheel", BR_EVENT_WHEEL },
    { "globalpointermove", BR_EVENT_GLOBAL_MOVE },
};

static const br_data_enum br_filter_names[] = {
    { "alpha", BR_FILTER_ALPHA },
    { "blur", BR_FILTER_BLUR },
    { "color_matrix", BR_FILTER_COLOR_MATRIX },
    { "displacement", BR_FILTER_DISPLACEMENT },
    { "noise", BR_FILTER_NOISE },
};

/* ---- masks, layers, hit areas, handlers */

static void br_data_read_mask(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *mask = cJSON_GetObjectItemCaseSensitive(json, "mask");
    if (mask == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "mask");
    br_data_ref ref;
    SDL_zero(ref);
    ref.kind = BR_DATA_REF_MASK;
    ref.node = n;
    ref.mask_kind = BR_MASK_STENCIL;
    bool ok = true;
    if (cJSON_IsString(mask)) {
        br_str_copy(ref.label, sizeof(ref.label), mask->valuestring);
    } else if (cJSON_IsObject(mask)) {
        int kind = BR_MASK_STENCIL;
        ok = br_data_get_string(e, mask, "node", ref.label, sizeof(ref.label));
        if (!ok) {
            br_data_errorf(e, "a mask needs \"node\": the mask node's label");
        }
        br_data_get_enum(e, mask, "kind", br_mask_names, BR_ARRAY_COUNT(br_mask_names), &kind);
        br_data_get_bool(e, mask, "inverse", &ref.inverse);
        ref.mask_kind = (br_mask_kind)kind;
    } else {
        br_data_errorf(e, "expected a label or { \"node\", \"kind\", \"inverse\" }");
        ok = false;
    }
    if (ok) {
        br_str_copy(ref.path, sizeof(ref.path), e->data->path);
        br_data_add_ref(e, &ref);
    }
    br_data_path_pop(e, mark);
}

static void br_data_read_layer(br_engine *e, br_node n, const cJSON *json)
{
    br_data_ref ref;
    SDL_zero(ref);
    if (!br_data_get_string(e, json, "layer", ref.label, sizeof(ref.label))) {
        return;
    }
    ref.kind = BR_DATA_REF_LAYER;
    ref.node = n;
    size_t mark = br_data_path_push_key(e, "layer");
    br_str_copy(ref.path, sizeof(ref.path), e->data->path);
    br_data_path_pop(e, mark);
    br_data_add_ref(e, &ref);
}

/* Reads `count` numbers from a JSON array (exact length). */
static bool br_data_numbers(const cJSON *v, float *out, int count)
{
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != count) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(v, i);
        if (!cJSON_IsNumber(item)) {
            return false;
        }
        out[i] = (float)item->valuedouble;
    }
    return true;
}

static void br_data_read_hit_area(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *area = cJSON_GetObjectItemCaseSensitive(json, "hit_area");
    if (area == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "hit_area");
    br_hit_area h;
    SDL_zero(h);
    float f[6];
    const cJSON *v = NULL;
    br_vec2 *points = NULL;
    if ((v = cJSON_GetObjectItemCaseSensitive(area, "rect")) != NULL && br_data_numbers(v, f, 4)) {
        h.type = BR_HIT_AREA_RECT;
        h.rect = br_rect_make(f[0], f[1], f[2], f[3]);
    } else if ((v = cJSON_GetObjectItemCaseSensitive(area, "circle")) != NULL && br_data_numbers(v, f, 3)) {
        h.type = BR_HIT_AREA_CIRCLE;
        h.circle = (br_circle){ f[0], f[1], f[2] };
    } else if ((v = cJSON_GetObjectItemCaseSensitive(area, "ellipse")) != NULL && br_data_numbers(v, f, 4)) {
        h.type = BR_HIT_AREA_ELLIPSE;
        h.ellipse = (br_ellipse){ f[0], f[1], f[2], f[3] };
    } else if ((v = cJSON_GetObjectItemCaseSensitive(area, "rounded_rect")) != NULL && br_data_numbers(v, f, 5)) {
        h.type = BR_HIT_AREA_ROUNDED_RECT;
        h.rounded_rect = (br_rounded_rect){ f[0], f[1], f[2], f[3], f[4] };
    } else if ((v = cJSON_GetObjectItemCaseSensitive(area, "triangle")) != NULL && br_data_numbers(v, f, 6)) {
        h.type = BR_HIT_AREA_TRIANGLE;
        h.triangle = (br_triangle){ f[0], f[1], f[2], f[3], f[4], f[5] };
    } else if ((v = cJSON_GetObjectItemCaseSensitive(area, "polygon")) != NULL && cJSON_IsArray(v) &&
               cJSON_GetArraySize(v) >= 6 && cJSON_GetArraySize(v) % 2 == 0) {
        uint32_t count = (uint32_t)cJSON_GetArraySize(v) / 2;
        points = br_alloc(sizeof(br_vec2) * count);
        bool numbers = points != NULL;
        for (uint32_t i = 0; numbers && i < count; i++) {
            const cJSON *x = cJSON_GetArrayItem(v, (int)(i * 2));
            const cJSON *y = cJSON_GetArrayItem(v, (int)(i * 2 + 1));
            numbers = cJSON_IsNumber(x) && cJSON_IsNumber(y);
            if (numbers) {
                points[i] = br_vec2_make((float)x->valuedouble, (float)y->valuedouble);
            }
        }
        if (numbers) {
            h.type = BR_HIT_AREA_POLYGON;
            h.points = points;
            h.point_count = count;
        }
    }
    if (h.type == BR_HIT_AREA_NONE) {
        br_data_errorf(e, "expected { \"rect\": [x,y,w,h] } / circle [x,y,r] / ellipse [x,y,hw,hh] / "
                          "rounded_rect [x,y,w,h,r] / triangle [6 numbers] / polygon [x,y,...]");
    } else {
        br_node_set_hit_area(e, n, &h); /* copies the polygon points */
    }
    br_free(points);
    br_data_path_pop(e, mark);
}

static void br_data_read_handlers(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(json, "on");
    if (on == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "on");
    if (!cJSON_IsObject(on)) {
        br_data_errorf(e, "expected { \"click\": \"handler name\", ... }");
        br_data_path_pop(e, mark);
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, on)
    {
        /* the key is the event name, the value the handler name */
        int type = 0;
        bool known = false;
        for (uint32_t i = 0; i < BR_ARRAY_COUNT(br_event_names); i++) {
            if (SDL_strcmp(br_event_names[i].name, item->string) == 0) {
                type = br_event_names[i].value;
                known = true;
            }
        }
        size_t item_mark = br_data_path_push_key(e, item->string);
        br_data_handler *h = cJSON_IsString(item) ? br_map_get(&e->data->handlers, item->valuestring) : NULL;
        if (!known) {
            br_data_errorf(e, "unknown event (pointerdown, pointerup, click, tap, ...)");
        } else if (!cJSON_IsString(item)) {
            br_data_errorf(e, "expected a handler name");
        } else if (h == NULL) {
            br_data_errorf(e, "no handler named '%s' (br_scene_file_add_handler)", item->valuestring);
        } else {
            br_node_on(e, n, (br_event_type)type, h->fn, h->user);
        }
        br_data_path_pop(e, item_mark);
    }
    br_data_path_pop(e, mark);
}

/* ---- filters */

/* Colour-matrix presets: { "name": "sepia" } or with a value ("brightness", "hue", ...). */
static bool br_data_color_preset(br_engine *e, float m[20], const cJSON *preset)
{
    char name[32] = "";
    float value = 1.0f;
    if (cJSON_IsString(preset)) {
        br_str_copy(name, sizeof(name), preset->valuestring);
    } else if (cJSON_IsObject(preset)) {
        br_data_get_string(e, preset, "name", name, sizeof(name));
        br_data_get_float(e, preset, "value", &value);
    }
    if (br_str_equal(name, "brightness")) {
        br_color_matrix_brightness(m, value, true);
    } else if (br_str_equal(name, "tint")) {
        br_color c = br_color_make(1, 1, 1, 1);
        br_data_get_color(e, preset, "color", &c);
        br_color_matrix_tint(m, c, true);
    } else if (br_str_equal(name, "greyscale")) {
        br_color_matrix_greyscale(m, value, true);
    } else if (br_str_equal(name, "black_and_white")) {
        br_color_matrix_black_and_white(m, true);
    } else if (br_str_equal(name, "hue")) {
        br_color_matrix_hue(m, value, true);
    } else if (br_str_equal(name, "contrast")) {
        br_color_matrix_contrast(m, value, true);
    } else if (br_str_equal(name, "saturate")) {
        br_color_matrix_saturate(m, value, true);
    } else if (br_str_equal(name, "desaturate")) {
        float d[20];
        br_color_matrix_desaturate(d);
        br_color_matrix_multiply(m, m, d);
    } else if (br_str_equal(name, "negative")) {
        br_color_matrix_negative(m, true);
    } else if (br_str_equal(name, "sepia")) {
        br_color_matrix_sepia(m, true);
    } else if (br_str_equal(name, "technicolor")) {
        br_color_matrix_technicolor(m, true);
    } else if (br_str_equal(name, "polaroid")) {
        br_color_matrix_polaroid(m, true);
    } else if (br_str_equal(name, "to_bgr")) {
        br_color_matrix_to_bgr(m, true);
    } else if (br_str_equal(name, "kodachrome")) {
        br_color_matrix_kodachrome(m, true);
    } else if (br_str_equal(name, "browni")) {
        br_color_matrix_browni(m, true);
    } else if (br_str_equal(name, "vintage")) {
        br_color_matrix_vintage(m, true);
    } else if (br_str_equal(name, "night")) {
        br_color_matrix_night(m, value, true);
    } else if (br_str_equal(name, "predator")) {
        br_color_matrix_predator(m, value, true);
    } else if (br_str_equal(name, "lsd")) {
        br_color_matrix_lsd(m, true);
    } else {
        br_data_errorf(e, "unknown colour-matrix preset '%s'", name);
        return false;
    }
    return true;
}

static bool br_data_read_filter(br_engine *e, const cJSON *item, br_filter *out)
{
    if (!cJSON_IsObject(item)) {
        br_data_errorf(e, "expected a filter object { \"type\": ..., params }");
        return false;
    }
    int type = -1;
    if (!br_data_get_enum(e, item, "type", br_filter_names, BR_ARRAY_COUNT(br_filter_names), &type)) {
        if (cJSON_GetObjectItemCaseSensitive(item, "type") == NULL) {
            br_data_errorf(e, "a filter needs \"type\" (alpha, blur, color_matrix, displacement, noise)");
        }
        return false;
    }
    /* parameters inline or under "params" */
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "params");
    size_t mark = p != NULL ? br_data_path_push_key(e, "params") : e->data->path_len;
    if (p == NULL) {
        p = item;
    }
    bool ok = true;
    if (type == BR_FILTER_ALPHA) {
        float alpha = 1.0f;
        br_data_get_float(e, p, "alpha", &alpha);
        *out = br_filter_alpha(alpha);
    } else if (type == BR_FILTER_BLUR) {
        float strength = 8.0f;
        int quality = 4;
        int kernel = 5;
        br_data_get_float(e, p, "strength", &strength);
        br_data_get_int_range(e, p, "quality", 1, 16, &quality); /* passes per frame */
        br_data_get_int(e, p, "kernel_size", &kernel); /* clamped to 5..15 by the filter */
        *out = br_filter_blur(strength, quality, kernel);
        br_data_get_float(e, p, "blur_x", &out->blur_x);
        br_data_get_float(e, p, "blur_y", &out->blur_y);
    } else if (type == BR_FILTER_COLOR_MATRIX) {
        *out = br_filter_color_matrix();
        const cJSON *matrix = cJSON_GetObjectItemCaseSensitive(p, "matrix");
        if (matrix != NULL && !br_data_numbers(matrix, out->matrix, 20)) {
            br_data_error(e, "matrix", "expected 20 numbers (4x5, row-major)");
        }
        const cJSON *presets = cJSON_GetObjectItemCaseSensitive(p, "presets");
        if (presets != NULL) {
            size_t pm = br_data_path_push_key(e, "presets");
            int i = 0;
            const cJSON *preset = NULL;
            cJSON_ArrayForEach(preset, presets)
            {
                size_t im = br_data_path_push_index(e, i++);
                br_data_color_preset(e, out->matrix, preset);
                br_data_path_pop(e, im);
            }
            br_data_path_pop(e, pm);
        }
        br_data_get_float(e, p, "alpha", &out->alpha);
    } else if (type == BR_FILTER_DISPLACEMENT) {
        br_texture map = { 0, 0 };
        br_vec2 scale = br_vec2_make(20.0f, 20.0f);
        if (!br_data_get_texture(e, p, "map", &map)) {
            br_data_errorf(e, "a displacement filter needs \"map\": a texture");
            ok = false;
        }
        br_data_get_vec2(e, p, "scale", &scale);
        *out = br_filter_displacement(map, scale.x, scale.y);
        br_data_get_vec2(e, p, "map_offset", &out->map_offset);
    } else {
        float noise = 0.5f;
        float seed = 0.0f;
        br_data_get_float(e, p, "noise", &noise);
        br_data_get_float(e, p, "seed", &seed);
        *out = br_filter_noise(noise, seed);
    }
    br_data_get_bool(e, p, "enabled", &out->enabled);
    br_data_get_float(e, p, "padding", &out->padding);
    br_data_path_pop(e, mark);
    return ok;
}

static void br_data_read_filters(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *filters = cJSON_GetObjectItemCaseSensitive(json, "filters");
    if (filters != NULL) {
        size_t mark = br_data_path_push_key(e, "filters");
        if (!cJSON_IsArray(filters)) {
            br_data_errorf(e, "expected an array of { \"type\": ..., params }");
        } else {
            br_filter list[BR_FILTER_MAX_PER_NODE];
            uint32_t count = 0;
            int i = 0;
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, filters)
            {
                size_t im = br_data_path_push_index(e, i++);
                if (count == BR_FILTER_MAX_PER_NODE) {
                    br_data_errorf(e, "more than %d filters on one node; ignored", BR_FILTER_MAX_PER_NODE);
                } else if (br_data_read_filter(e, item, &list[count])) {
                    count++;
                }
                br_data_path_pop(e, im);
            }
            br_filters_set(e, n, list, count);
        }
        br_data_path_pop(e, mark);
    }
    float resolution = 0.0f;
    if (br_data_get_float(e, json, "filter_resolution", &resolution)) {
        br_filters_set_resolution(e, n, resolution);
    }
    br_rect area;
    if (br_data_get_rect(e, json, "filter_area", &area)) {
        br_filters_set_area(e, n, &area);
    }
}

/* ---- read: every key optional */

void br_node_json_container_read(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
    if (label != NULL) {
        if (cJSON_IsString(label)) {
            br_node_set_label(e, n, label->valuestring);
        } else {
            br_data_error(e, "label", "expected a string");
        }
    }
    br_vec2 v;
    float f;
    bool b;
    int i;
    br_color c;
    br_rect r;
    if (br_data_get_vec2(e, json, "position", &v)) {
        br_node_set_position(e, n, v.x, v.y);
    }
    if (br_data_get_vec2(e, json, "scale", &v)) {
        br_node_set_scale(e, n, v.x, v.y);
    }
    if (br_data_get_float(e, json, "rotation", &f)) {
        br_node_set_rotation(e, n, f);
    }
    if (br_data_get_float(e, json, "angle", &f)) {
        br_node_set_rotation(e, n, f * SDL_PI_F / 180.0f); /* degrees, like Pixi's angle */
    }
    if (br_data_get_vec2(e, json, "skew", &v)) {
        br_node_set_skew(e, n, v.x, v.y);
    }
    if (br_data_get_vec2(e, json, "pivot", &v)) {
        br_node_set_pivot(e, n, v.x, v.y);
    }
    if (br_data_get_vec2(e, json, "origin", &v)) {
        br_node_set_origin(e, n, v.x, v.y);
    }
    if (br_data_get_float(e, json, "alpha", &f)) {
        br_node_set_alpha(e, n, f);
    }
    if (br_data_get_color(e, json, "tint", &c)) {
        br_node_set_tint(e, n, c);
    }
    if (br_data_get_bool(e, json, "visible", &b)) {
        br_node_set_visible(e, n, b);
    }
    if (br_data_get_bool(e, json, "renderable", &b)) {
        br_node_set_renderable(e, n, b);
    }
    if (br_data_get_int(e, json, "z_index", &i)) {
        br_node_set_z_index(e, n, i);
    }
    if (br_data_get_bool(e, json, "sortable_children", &b)) {
        br_node_set_sortable_children(e, n, b);
    }
    if (br_data_get_enum(e, json, "blend", br_blend_names, BR_ARRAY_COUNT(br_blend_names), &i)) {
        br_node_set_blend_mode(e, n, i);
    }
    if (br_data_get_bool(e, json, "cullable", &b)) {
        br_node_set_cullable(e, n, b);
    }
    if (br_data_get_bool(e, json, "cullable_children", &b)) {
        br_node_set_cullable_children(e, n, b);
    }
    if (br_data_get_rect(e, json, "cull_area", &r)) {
        br_node_set_cull_area(e, n, r);
    }
    if (br_data_get_bool(e, json, "render_group", &b)) {
        br_node_set_render_group(e, n, b);
    }
    if (br_data_get_bool(e, json, "cache_as_texture", &b)) {
        br_node_set_cache_as_texture(e, n, b);
    }
    if (br_data_get_enum(e, json, "event_mode", br_event_mode_names, BR_ARRAY_COUNT(br_event_mode_names), &i)) {
        br_node_set_event_mode(e, n, (br_event_mode)i);
    }
    if (br_data_get_bool(e, json, "interactive_children", &b)) {
        br_node_set_interactive_children(e, n, b);
    }
    if (br_data_get_enum(e, json, "cursor", br_cursor_names, BR_ARRAY_COUNT(br_cursor_names), &i)) {
        br_node_set_cursor(e, n, i);
    }
    br_data_read_hit_area(e, n, json);
    br_data_read_mask(e, n, json);
    br_data_read_layer(e, n, json);
    br_data_read_filters(e, n, json);
    br_data_read_handlers(e, n, json);
}

/* ---- write: live values (defaults are left out; stale stored keys removed) */

static void br_data_put(cJSON *out, const char *key, bool keep, cJSON *value)
{
    if (keep) {
        br_data_json_set(out, key, value);
    } else {
        cJSON_Delete(value);
        cJSON_DeleteItemFromObjectCaseSensitive(out, key);
    }
}

void br_node_json_container_write(br_engine *e, br_node n, cJSON *out)
{
    const char *label = br_node_label(e, n);
    br_data_put(out, "label", label[0] != '\0', cJSON_CreateString(label));
    br_vec2 p = br_node_position(e, n);
    br_data_put(out, "position", p.x != 0.0f || p.y != 0.0f, br_data_vec2_json(p));
    br_vec2 s = br_node_scale(e, n);
    br_data_put(out, "scale", s.x != 1.0f || s.y != 1.0f, br_data_vec2_json(s));
    float rotation = br_node_rotation(e, n);
    cJSON_DeleteItemFromObjectCaseSensitive(out, "angle"); /* saved as radians */
    br_data_put(out, "rotation", rotation != 0.0f, br_data_number(rotation));
    br_vec2 skew = br_node_skew(e, n);
    br_data_put(out, "skew", skew.x != 0.0f || skew.y != 0.0f, br_data_vec2_json(skew));
    br_vec2 pivot = br_node_pivot(e, n);
    br_data_put(out, "pivot", pivot.x != 0.0f || pivot.y != 0.0f, br_data_vec2_json(pivot));
    br_vec2 origin = br_node_origin(e, n);
    br_data_put(out, "origin", origin.x != 0.0f || origin.y != 0.0f, br_data_vec2_json(origin));
    float alpha = br_node_alpha(e, n);
    br_data_put(out, "alpha", alpha != 1.0f, br_data_number(alpha));
    br_color tint = br_node_tint(e, n);
    tint.a = 1.0f;
    br_data_put(out, "tint", br_color_to_hex(tint) != 0xffffff, br_data_color_json(tint));
    br_data_put(out, "visible", !br_node_visible(e, n), cJSON_CreateFalse());
    br_data_put(out, "renderable", !br_node_renderable(e, n), cJSON_CreateFalse());
    int z = br_node_z_index(e, n);
    br_data_put(out, "z_index", z != 0, cJSON_CreateNumber(z));
    br_data_put(out, "sortable_children", br_node_sortable_children(e, n), cJSON_CreateTrue());
    int blend = br_node_blend_mode(e, n);
    const char *blend_name = br_data_enum_name(br_blend_names, BR_ARRAY_COUNT(br_blend_names), blend);
    br_data_put(out, "blend", blend != BR_BLEND_INHERIT && blend_name != NULL, cJSON_CreateString(blend_name ? blend_name : ""));
    br_data_put(out, "cullable", br_node_cullable(e, n), cJSON_CreateTrue());
    br_data_put(out, "render_group", br_node_is_render_group(e, n), cJSON_CreateTrue());
    br_data_put(out, "cache_as_texture", br_node_cache_as_texture(e, n), cJSON_CreateTrue());
    br_event_mode mode = br_node_event_mode(e, n);
    const char *mode_name = br_data_enum_name(br_event_mode_names, BR_ARRAY_COUNT(br_event_mode_names), (int)mode);
    br_data_put(out, "event_mode", mode != BR_EVENT_MODE_PASSIVE && mode_name != NULL,
                cJSON_CreateString(mode_name ? mode_name : ""));
    br_data_put(out, "interactive_children", !br_node_interactive_children(e, n), cJSON_CreateFalse());
    int cursor = br_node_cursor(e, n);
    const char *cursor_name = br_data_enum_name(br_cursor_names, BR_ARRAY_COUNT(br_cursor_names), cursor);
    br_data_put(out, "cursor", cursor != -1 && cursor_name != NULL, cJSON_CreateString(cursor_name ? cursor_name : ""));
    // ponytail: mask, layer, filters, hit area, cull area and handlers are saved as loaded (not read back live); add getters-based writers if tools start editing them.
}

void br_node_json_container_register(br_engine *e)
{
    br_data_add_type(e, "container", BR_NODE_TYPE_CONTAINER, NULL, NULL);
}
