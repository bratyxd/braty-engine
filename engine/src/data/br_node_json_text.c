/* br_node_json_text: JSON for "text", "bitmap_text" and "rich_text" and their text style.
 *
 *   { "type": "text", "text": "Hello", "anchor": [0.5, 0.5],
 *     "style": { "font_size": 32, "fill": "white", "stroke": { "width": 3, "color": "black" },
 *                "drop_shadow": { "color": "#00000080", "distance": 4 }, "align": "center" } }
 *   { "type": "bitmap_text", "text": "SCORE 0", "font": "fonts/pixel.fnt", "size": 24 }
 *   { "type": "rich_text", "text": "<b>Bold</b> and <warn>red</warn>",
 *     "tag_styles": { "warn": { "color": "red", "bold": true } } }
 */
#include "data/br_data_internal.h"

static const br_data_enum br_align_names[] = {
    { "left", BR_TEXT_ALIGN_LEFT },
    { "center", BR_TEXT_ALIGN_CENTER },
    { "right", BR_TEXT_ALIGN_RIGHT },
    { "justify", BR_TEXT_ALIGN_JUSTIFY },
};

static const br_data_enum br_white_space_names[] = {
    { "pre", BR_TEXT_WHITE_SPACE_PRE },
    { "normal", BR_TEXT_WHITE_SPACE_NORMAL },
    { "pre_line", BR_TEXT_WHITE_SPACE_PRE_LINE },
};

static const br_data_enum br_baseline_names[] = {
    { "top", BR_TEXT_BASELINE_TOP },
    { "alphabetic", BR_TEXT_BASELINE_ALPHABETIC },
    { "middle", BR_TEXT_BASELINE_MIDDLE },
    { "bottom", BR_TEXT_BASELINE_BOTTOM },
};

/* A font file: .fnt/.xml = bitmap font, anything else = TTF/OTF. */
static br_font *br_data_font(br_engine *e, const cJSON *obj, const char *key)
{
    char path[BR_DATA_PATH_SIZE];
    if (!br_data_get_string(e, obj, key, path, sizeof(path))) {
        return NULL;
    }
    const char *ext = br_path_extension(path);
    bool bitmap = SDL_strcasecmp(ext, "fnt") == 0 || SDL_strcasecmp(ext, "xml") == 0;
    br_font *font = bitmap ? br_bitmap_font_load(e, path) : br_font_load(e, path);
    if (font == NULL) {
        size_t mark = br_data_path_push_key(e, key);
        br_data_errorf(e, "font '%s' could not be loaded", path);
        br_data_path_pop(e, mark);
    }
    return font;
}

static void br_data_text_gradient(br_engine *e, const cJSON *json, br_text_style *style)
{
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(json, "gradient");
    if (g == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "gradient");
    const cJSON *stops = cJSON_GetObjectItemCaseSensitive(g, "stops");
    int count = cJSON_IsArray(stops) ? cJSON_GetArraySize(stops) : 0;
    if (count < 2 || count > BR_TEXT_MAX_STOPS) {
        br_data_errorf(e, "expected { \"stops\": [[offset, colour], ...] } with 2..8 stops");
        br_data_path_pop(e, mark);
        return;
    }
    br_text_gradient grad;
    SDL_zero(grad);
    grad.start = br_vec2_make(0.0f, 0.0f);
    grad.end = br_vec2_make(0.0f, 1.0f);
    br_data_get_vec2(e, g, "start", &grad.start);
    br_data_get_vec2(e, g, "end", &grad.end);
    size_t sm = br_data_path_push_key(e, "stops");
    for (int i = 0; i < count; i++) {
        const cJSON *stop = cJSON_GetArrayItem(stops, i);
        const cJSON *offset = cJSON_GetArrayItem(stop, 0);
        size_t im = br_data_path_push_index(e, i);
        br_color c;
        if (cJSON_IsArray(stop) && cJSON_GetArraySize(stop) == 2 && cJSON_IsNumber(offset) &&
            br_data_value_color(e, cJSON_GetArrayItem(stop, 1), &c)) {
            grad.offsets[grad.stop_count] = (float)offset->valuedouble;
            grad.colors[grad.stop_count] = c;
            grad.stop_count++;
        } else {
            br_data_errorf(e, "expected [offset, colour]");
        }
        br_data_path_pop(e, im);
    }
    br_data_path_pop(e, sm);
    if (grad.stop_count >= 2) {
        style->gradient = grad;
        style->use_gradient = true;
    }
    br_data_path_pop(e, mark);
}

void br_node_json_text_style(br_engine *e, const cJSON *json, br_text_style *style)
{
    if (!cJSON_IsObject(json)) {
        br_data_errorf(e, "expected a text style object");
        return;
    }
    br_font *font = br_data_font(e, json, "font");
    if (font != NULL) {
        style->font = font;
    }
    char family[BR_TEXT_FAMILY_SIZE];
    if (br_data_get_string(e, json, "font_family", family, sizeof(family))) {
        br_text_style_set_family(style, family);
    }
    br_data_get_float(e, json, "font_size", &style->font_size);
    br_data_get_bool(e, json, "bold", &style->bold);
    br_data_get_bool(e, json, "italic", &style->italic);
    if (br_data_get_color(e, json, "fill", &style->fill)) {
        style->use_gradient = false;
    }
    br_data_text_gradient(e, json, style);

    const cJSON *stroke = cJSON_GetObjectItemCaseSensitive(json, "stroke");
    if (cJSON_IsObject(stroke)) {
        size_t mark = br_data_path_push_key(e, "stroke");
        br_data_get_float(e, stroke, "width", &style->stroke_width);
        br_data_get_color(e, stroke, "color", &style->stroke_color);
        br_data_path_pop(e, mark);
    } else if (stroke != NULL) {
        br_data_error(e, "stroke", "expected { \"width\", \"color\" }");
    }

    const cJSON *shadow = cJSON_GetObjectItemCaseSensitive(json, "drop_shadow");
    if (cJSON_IsBool(shadow)) {
        style->drop_shadow = cJSON_IsTrue(shadow);
    } else if (cJSON_IsObject(shadow)) {
        size_t mark = br_data_path_push_key(e, "drop_shadow");
        style->drop_shadow = true;
        br_data_get_color(e, shadow, "color", &style->shadow_color);
        br_data_get_float(e, shadow, "angle", &style->shadow_angle);
        br_data_get_float(e, shadow, "distance", &style->shadow_distance);
        br_data_get_float(e, shadow, "blur", &style->shadow_blur);
        br_data_path_pop(e, mark);
    } else if (shadow != NULL) {
        br_data_error(e, "drop_shadow", "expected true/false or { \"color\", \"angle\", \"distance\", \"blur\" }");
    }

    int i;
    if (br_data_get_enum(e, json, "align", br_align_names, BR_ARRAY_COUNT(br_align_names), &i)) {
        style->align = (br_text_align)i;
    }
    br_data_get_bool(e, json, "word_wrap", &style->word_wrap);
    br_data_get_float(e, json, "wrap_width", &style->wrap_width);
    br_data_get_bool(e, json, "break_words", &style->break_words);
    br_data_get_float(e, json, "line_height", &style->line_height);
    br_data_get_float(e, json, "leading", &style->leading);
    br_data_get_float(e, json, "letter_spacing", &style->letter_spacing);
    br_data_get_float(e, json, "padding", &style->padding);
    if (br_data_get_enum(e, json, "white_space", br_white_space_names, BR_ARRAY_COUNT(br_white_space_names), &i)) {
        style->white_space = (br_text_white_space)i;
    }
    if (br_data_get_enum(e, json, "baseline", br_baseline_names, BR_ARRAY_COUNT(br_baseline_names), &i)) {
        style->baseline = (br_text_baseline)i;
    }
}

static void br_data_tag_styles(br_engine *e, br_node n, const cJSON *json)
{
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(json, "tag_styles");
    if (tags == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "tag_styles");
    const cJSON *tag = NULL;
    cJSON_ArrayForEach(tag, tags)
    {
        size_t tm = br_data_path_push_key(e, tag->string);
        br_rich_tag_style st;
        SDL_zero(st);
        if (!cJSON_IsObject(tag)) {
            br_data_errorf(e, "expected a tag style object");
        } else {
            br_data_get_string(e, tag, "font_family", st.font_family, sizeof(st.font_family));
            br_data_get_float(e, tag, "font_size", &st.font_size);
            st.set_color = br_data_get_color(e, tag, "color", &st.color);
            br_data_get_bool(e, tag, "bold", &st.bold);
            br_data_get_bool(e, tag, "italic", &st.italic);
            br_data_get_bool(e, tag, "underline", &st.underline);
            br_data_get_bool(e, tag, "strike", &st.strike);
            br_rich_text_set_tag_style(e, n, tag->string, &st);
        }
        br_data_path_pop(e, tm);
    }
    br_data_path_pop(e, mark);
}

static bool br_data_text_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
    if (cJSON_IsString(text)) {
        br_text_set_text(e, n, text->valuestring);
    } else if (text != NULL) {
        br_data_error(e, "text", "expected a string");
    }
    br_text_style style;
    br_text_get_style(e, n, &style);
    br_font *font = br_data_font(e, json, "font"); /* bitmap_text shortcut */
    if (font != NULL) {
        style.font = font;
    }
    br_data_get_float(e, json, "size", &style.font_size);
    const cJSON *style_json = cJSON_GetObjectItemCaseSensitive(json, "style");
    if (style_json != NULL) {
        size_t mark = br_data_path_push_key(e, "style");
        br_node_json_text_style(e, style_json, &style);
        br_data_path_pop(e, mark);
    }
    br_text_set_style(e, n, &style);
    br_vec2 anchor;
    if (br_data_get_vec2(e, json, "anchor", &anchor)) {
        br_text_set_anchor(e, n, anchor.x, anchor.y);
    }
    float resolution;
    if (br_data_get_float(e, json, "resolution", &resolution)) {
        br_text_set_resolution(e, n, resolution);
    }
    if (br_node_type_of(e, n) == br_rich_text_type(e)) {
        br_data_tag_styles(e, n, json);
    }
    return true;
}

static void br_data_text_write(br_engine *e, br_node n, cJSON *out)
{
    br_data_json_set(out, "text", cJSON_CreateString(br_text_get_text(e, n)));
}

/* Each kind is made by its own create call (rich text and bitmap defaults are set there). */
static br_node br_data_text_create(br_engine *e, const cJSON *json)
{
    (void)json;
    return br_text_create(e, "", NULL);
}

static br_node br_data_bitmap_text_create(br_engine *e, const cJSON *json)
{
    (void)json;
    return br_bitmap_text_create(e, "", NULL, 0.0f);
}

static br_node br_data_rich_text_create(br_engine *e, const cJSON *json)
{
    (void)json;
    return br_rich_text_create(e, "", NULL);
}

void br_node_json_text_register(br_engine *e)
{
    br_node_type_set_from_json(e, br_text_type(e), br_data_text_from_json);
    br_node_type_set_from_json(e, br_bitmap_text_type(e), br_data_text_from_json);
    br_node_type_set_from_json(e, br_rich_text_type(e), br_data_text_from_json);
    br_data_add_type(e, "text", br_text_type(e), br_data_text_create, br_data_text_write);
    br_data_add_type(e, "bitmap_text", br_bitmap_text_type(e), br_data_bitmap_text_create, br_data_text_write);
    br_data_add_type(e, "rich_text", br_rich_text_type(e), br_data_rich_text_create, br_data_text_write);
}
