/* br_node_json_graphics: JSON for "graphics". "commands" mirrors the graphics context calls,
 * one array per call: [name, arguments...]; "svg" is a shortcut for svg_path + fill/stroke.
 *
 *   { "type": "graphics", "commands": [
 *       ["rect", 0, 0, 100, 50], ["fill", "#ff3355"], ["stroke", { "width": 4, "color": "white" }],
 *       ["circle", 50, 25, 10], ["cut"] ] }
 *   { "type": "graphics", "svg": "M0 0 L 40 0 L 20 30 Z", "fill": "gold", "stroke": "black" }
 */
#include "data/br_data_internal.h"

static const br_data_enum br_space_names[] = {
    { "local", BR_GRAPHICS_SPACE_LOCAL },
    { "global", BR_GRAPHICS_SPACE_GLOBAL },
};

static const br_data_enum br_cap_names[] = {
    { "butt", BR_GRAPHICS_CAP_BUTT },
    { "round", BR_GRAPHICS_CAP_ROUND },
    { "square", BR_GRAPHICS_CAP_SQUARE },
};

static const br_data_enum br_join_names[] = {
    { "miter", BR_GRAPHICS_JOIN_MITER },
    { "bevel", BR_GRAPHICS_JOIN_BEVEL },
    { "round", BR_GRAPHICS_JOIN_ROUND },
};

/* Reads exactly `count` numbers from a JSON array. */
static bool br_data_read_numbers(const cJSON *v, float *out, int count)
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

static void br_data_read_stops(br_engine *e, const cJSON *json, br_fill_style *style)
{
    const cJSON *stops = cJSON_GetObjectItemCaseSensitive(json, "stops");
    if (stops == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, "stops");
    int i = 0;
    const cJSON *stop = NULL;
    cJSON_ArrayForEach(stop, stops)
    {
        size_t im = br_data_path_push_index(e, i++);
        const cJSON *offset = cJSON_GetArrayItem(stop, 0);
        br_color c;
        if (cJSON_IsArray(stop) && cJSON_GetArraySize(stop) == 2 && cJSON_IsNumber(offset)) {
            size_t cm = br_data_path_push_index(e, 1);
            if (br_data_value_color(e, cJSON_GetArrayItem(stop, 1), &c)) {
                br_graphics_style_add_stop(style, (float)offset->valuedouble, c);
            }
            br_data_path_pop(e, cm);
        } else {
            br_data_errorf(e, "expected [offset, colour]");
        }
        br_data_path_pop(e, im);
    }
    br_data_path_pop(e, mark);
}

bool br_node_json_fill_style(br_engine *e, const cJSON *json, br_fill_style *out)
{
    br_color c = br_color_make(1, 1, 1, 1);
    if (!cJSON_IsObject(json)) {
        if (!br_data_value_color(e, json, &c)) {
            return false;
        }
        *out = br_graphics_style_color(c);
        return true;
    }
    int space = BR_GRAPHICS_SPACE_LOCAL;
    br_data_get_enum(e, json, "space", br_space_names, BR_ARRAY_COUNT(br_space_names), &space);
    float f[6];
    const cJSON *linear = cJSON_GetObjectItemCaseSensitive(json, "linear");
    const cJSON *radial = cJSON_GetObjectItemCaseSensitive(json, "radial");
    br_texture texture;
    if (linear != NULL) {
        if (!br_data_read_numbers(linear, f, 4)) {
            br_data_error(e, "linear", "expected [x0, y0, x1, y1]");
            return false;
        }
        *out = br_graphics_style_linear((br_graphics_space)space, f[0], f[1], f[2], f[3]);
        br_data_read_stops(e, json, out);
    } else if (radial != NULL) {
        if (!br_data_read_numbers(radial, f, 4)) {
            br_data_error(e, "radial", "expected [cx, cy, inner_radius, outer_radius]");
            return false;
        }
        *out = br_graphics_style_radial((br_graphics_space)space, f[0], f[1], f[2], f[3]);
        br_data_read_stops(e, json, out);
    } else if (br_data_get_texture(e, json, "texture", &texture)) {
        br_mat3 m = br_mat3_identity();
        const cJSON *matrix = cJSON_GetObjectItemCaseSensitive(json, "matrix");
        if (matrix != NULL) {
            if (br_data_read_numbers(matrix, f, 6)) {
                m = (br_mat3){ f[0], f[1], f[2], f[3], f[4], f[5] };
            } else {
                br_data_error(e, "matrix", "expected [a, b, c, d, tx, ty]");
            }
        }
        *out = br_graphics_style_pattern(texture, m);
    } else {
        *out = br_graphics_style_color(c);
    }
    /* "color" tints gradients/patterns or is the plain colour; "alpha" is the opacity */
    if (br_data_get_color(e, json, "color", &c)) {
        out->color = c;
    }
    br_data_get_float(e, json, "alpha", &out->color.a);
    return true;
}

static bool br_data_stroke_style(br_engine *e, const cJSON *json, br_stroke_style *out)
{
    *out = br_graphics_stroke_style(1.0f, br_color_make(1, 1, 1, 1));
    if (!br_node_json_fill_style(e, json, &out->fill)) {
        return false;
    }
    if (!cJSON_IsObject(json)) {
        return true; /* a plain colour: 1 unit wide */
    }
    int i;
    br_data_get_float(e, json, "width", &out->width);
    br_data_get_float(e, json, "alignment", &out->alignment);
    if (br_data_get_enum(e, json, "cap", br_cap_names, BR_ARRAY_COUNT(br_cap_names), &i)) {
        out->cap = (br_graphics_cap)i;
    }
    if (br_data_get_enum(e, json, "join", br_join_names, BR_ARRAY_COUNT(br_join_names), &i)) {
        out->join = (br_graphics_join)i;
    }
    br_data_get_float(e, json, "miter_limit", &out->miter_limit);
    br_data_get_bool(e, json, "pixel_line", &out->pixel_line);
    br_data_get_float(e, json, "dash_offset", &out->dash_offset);
    const cJSON *dash = cJSON_GetObjectItemCaseSensitive(json, "dash");
    if (dash != NULL) {
        int count = cJSON_IsArray(dash) ? cJSON_GetArraySize(dash) : -1;
        if (count < 0 || count > BR_GRAPHICS_MAX_DASHES || !br_data_read_numbers(dash, out->dash, count)) {
            br_data_error(e, "dash", "expected up to 8 dash lengths");
        } else {
            out->dash_count = (uint32_t)count;
        }
    }
    return true;
}

/* ---- commands */

/* Numbers args[first .. first+count-1]; optional ones keep their preset value when absent. */
static bool br_data_args(const cJSON *cmd, int first, int required, int count, float *out)
{
    int size = cJSON_GetArraySize(cmd);
    if (size < first + required || size > first + count) {
        return false;
    }
    for (int i = 0; i < count && first + i < size; i++) {
        const cJSON *item = cJSON_GetArrayItem(cmd, first + i);
        if (!cJSON_IsNumber(item)) {
            return false;
        }
        out[i] = (float)item->valuedouble;
    }
    return true;
}

static bool br_data_flag(const cJSON *cmd, int index, bool fallback)
{
    const cJSON *item = cJSON_GetArrayItem(cmd, index);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

/* Most sides / star points a command may ask for (each costs vertices; more never looks different). */
#define BR_DATA_MAX_SIDES 1024

/* Runs one command; false = bad arguments (the caller reports usage). A count out of range is
 * reported here and the command skipped. */
static bool br_data_command(br_engine *e, br_graphics_context *ctx, const char *name, const cJSON *cmd,
                            const char **usage)
{
    float a[8] = { 0 };
    if (SDL_strcmp(name, "move_to") == 0) {
        *usage = "[\"move_to\", x, y]";
        if (!br_data_args(cmd, 1, 2, 2, a)) {
            return false;
        }
        br_graphics_move_to(ctx, a[0], a[1]);
    } else if (SDL_strcmp(name, "line_to") == 0) {
        *usage = "[\"line_to\", x, y]";
        if (!br_data_args(cmd, 1, 2, 2, a)) {
            return false;
        }
        br_graphics_line_to(ctx, a[0], a[1]);
    } else if (SDL_strcmp(name, "bezier_curve_to") == 0) {
        *usage = "[\"bezier_curve_to\", cp1x, cp1y, cp2x, cp2y, x, y]";
        if (!br_data_args(cmd, 1, 6, 6, a)) {
            return false;
        }
        br_graphics_bezier_curve_to(ctx, a[0], a[1], a[2], a[3], a[4], a[5]);
    } else if (SDL_strcmp(name, "quadratic_curve_to") == 0) {
        *usage = "[\"quadratic_curve_to\", cpx, cpy, x, y]";
        if (!br_data_args(cmd, 1, 4, 4, a)) {
            return false;
        }
        br_graphics_quadratic_curve_to(ctx, a[0], a[1], a[2], a[3]);
    } else if (SDL_strcmp(name, "arc") == 0) {
        *usage = "[\"arc\", cx, cy, radius, start, end, counterclockwise?]";
        int size = cJSON_GetArraySize(cmd);
        if ((size != 6 && size != 7) || (size == 7 && !cJSON_IsBool(cJSON_GetArrayItem(cmd, 6)))) {
            return false;
        }
        for (int i = 0; i < 5; i++) {
            const cJSON *item = cJSON_GetArrayItem(cmd, 1 + i);
            if (!cJSON_IsNumber(item)) {
                return false;
            }
            a[i] = (float)item->valuedouble;
        }
        bool ccw = br_data_flag(cmd, 6, false);
        br_graphics_arc(ctx, a[0], a[1], a[2], a[3], a[4], ccw);
    } else if (SDL_strcmp(name, "arc_to") == 0) {
        *usage = "[\"arc_to\", x1, y1, x2, y2, radius]";
        if (!br_data_args(cmd, 1, 5, 5, a)) {
            return false;
        }
        br_graphics_arc_to(ctx, a[0], a[1], a[2], a[3], a[4]);
    } else if (SDL_strcmp(name, "arc_to_svg") == 0) {
        *usage = "[\"arc_to_svg\", rx, ry, x_axis_rotation, large_arc, sweep, x, y]";
        const int numeric[5] = { 1, 2, 3, 6, 7 };
        if (cJSON_GetArraySize(cmd) != 8 || !cJSON_IsBool(cJSON_GetArrayItem(cmd, 4)) ||
            !cJSON_IsBool(cJSON_GetArrayItem(cmd, 5))) {
            return false;
        }
        for (int i = 0; i < 5; i++) {
            const cJSON *item = cJSON_GetArrayItem(cmd, numeric[i]);
            if (!cJSON_IsNumber(item)) {
                return false;
            }
            a[i] = (float)item->valuedouble;
        }
        br_graphics_arc_to_svg(ctx, a[0], a[1], a[2], br_data_flag(cmd, 4, false), br_data_flag(cmd, 5, false), a[3],
                               a[4]);
    } else if (SDL_strcmp(name, "close_path") == 0) {
        *usage = "[\"close_path\"]";
        br_graphics_close_path(ctx);
    } else if (SDL_strcmp(name, "svg_path") == 0) {
        *usage = "[\"svg_path\", \"M0 0 L10 0 ...\"]";
        const cJSON *d = cJSON_GetArrayItem(cmd, 1);
        if (!cJSON_IsString(d)) {
            return false;
        }
        if (!br_graphics_svg_path(ctx, d->valuestring)) {
            br_data_errorf(e, "SVG path data has a syntax error (drawn up to it)");
        }
    } else if (SDL_strcmp(name, "rect") == 0) {
        *usage = "[\"rect\", x, y, w, h]";
        if (!br_data_args(cmd, 1, 4, 4, a)) {
            return false;
        }
        br_graphics_rect(ctx, a[0], a[1], a[2], a[3]);
    } else if (SDL_strcmp(name, "round_rect") == 0) {
        *usage = "[\"round_rect\", x, y, w, h, radius]";
        if (!br_data_args(cmd, 1, 5, 5, a)) {
            return false;
        }
        br_graphics_round_rect(ctx, a[0], a[1], a[2], a[3], a[4]);
    } else if (SDL_strcmp(name, "circle") == 0) {
        *usage = "[\"circle\", x, y, radius]";
        if (!br_data_args(cmd, 1, 3, 3, a)) {
            return false;
        }
        br_graphics_circle(ctx, a[0], a[1], a[2]);
    } else if (SDL_strcmp(name, "ellipse") == 0) {
        *usage = "[\"ellipse\", x, y, radius_x, radius_y]";
        if (!br_data_args(cmd, 1, 4, 4, a)) {
            return false;
        }
        br_graphics_ellipse(ctx, a[0], a[1], a[2], a[3]);
    } else if (SDL_strcmp(name, "poly") == 0) {
        *usage = "[\"poly\", [x0, y0, x1, y1, ...], close?]";
        const cJSON *pts = cJSON_GetArrayItem(cmd, 1);
        int n = cJSON_IsArray(pts) ? cJSON_GetArraySize(pts) : 0;
        if (n < 4 || n % 2 != 0) {
            return false;
        }
        br_vec2 *points = br_alloc(sizeof(br_vec2) * (size_t)(n / 2));
        if (points == NULL) {
            return true;
        }
        bool ok = true;
        for (int i = 0; i < n / 2 && ok; i++) {
            const cJSON *x = cJSON_GetArrayItem(pts, i * 2);
            const cJSON *y = cJSON_GetArrayItem(pts, i * 2 + 1);
            ok = cJSON_IsNumber(x) && cJSON_IsNumber(y);
            if (ok) {
                points[i] = br_vec2_make((float)x->valuedouble, (float)y->valuedouble);
            }
        }
        if (ok) {
            br_graphics_poly(ctx, points, (uint32_t)(n / 2), br_data_flag(cmd, 2, true));
        }
        br_free(points);
        return ok;
    } else if (SDL_strcmp(name, "regular_poly") == 0) {
        *usage = "[\"regular_poly\", x, y, radius, sides, rotation?]";
        if (!br_data_args(cmd, 1, 4, 5, a)) {
            return false;
        }
        uint32_t sides = 0;
        if (br_data_value_count(e, a[3], 3, BR_DATA_MAX_SIDES, &sides)) {
            br_graphics_regular_poly(ctx, a[0], a[1], a[2], sides, a[4]);
        }
    } else if (SDL_strcmp(name, "round_poly") == 0) {
        *usage = "[\"round_poly\", x, y, radius, sides, corner_radius, rotation?]";
        if (!br_data_args(cmd, 1, 5, 6, a)) {
            return false;
        }
        uint32_t sides = 0;
        if (br_data_value_count(e, a[3], 3, BR_DATA_MAX_SIDES, &sides)) {
            br_graphics_round_poly(ctx, a[0], a[1], a[2], sides, a[4], a[5]);
        }
    } else if (SDL_strcmp(name, "star") == 0) {
        *usage = "[\"star\", x, y, points, radius, inner_radius?, rotation?]";
        if (!br_data_args(cmd, 1, 4, 6, a)) {
            return false;
        }
        uint32_t points = 0;
        if (br_data_value_count(e, a[2], 2, BR_DATA_MAX_SIDES, &points)) {
            br_graphics_star(ctx, a[0], a[1], points, a[3], a[4], a[5]);
        }
    } else if (SDL_strcmp(name, "chamfer_rect") == 0) {
        *usage = "[\"chamfer_rect\", x, y, w, h, chamfer]";
        if (!br_data_args(cmd, 1, 5, 5, a)) {
            return false;
        }
        br_graphics_chamfer_rect(ctx, a[0], a[1], a[2], a[3], a[4]);
    } else if (SDL_strcmp(name, "fillet_rect") == 0) {
        *usage = "[\"fillet_rect\", x, y, w, h, fillet]";
        if (!br_data_args(cmd, 1, 5, 5, a)) {
            return false;
        }
        br_graphics_fillet_rect(ctx, a[0], a[1], a[2], a[3], a[4]);
    } else if (SDL_strcmp(name, "fill") == 0) {
        *usage = "[\"fill\", colour or fill style]";
        br_fill_style style;
        if (cJSON_GetArraySize(cmd) != 2) {
            return false;
        }
        size_t mark = br_data_path_push_index(e, 1);
        bool ok = br_node_json_fill_style(e, cJSON_GetArrayItem(cmd, 1), &style);
        br_data_path_pop(e, mark);
        if (ok) {
            br_graphics_fill(ctx, &style);
        }
    } else if (SDL_strcmp(name, "stroke") == 0) {
        *usage = "[\"stroke\", colour or { \"width\", \"color\", ... }]";
        br_stroke_style style;
        if (cJSON_GetArraySize(cmd) != 2) {
            return false;
        }
        size_t mark = br_data_path_push_index(e, 1);
        bool ok = br_data_stroke_style(e, cJSON_GetArrayItem(cmd, 1), &style);
        br_data_path_pop(e, mark);
        if (ok) {
            br_graphics_stroke(ctx, &style);
        }
    } else if (SDL_strcmp(name, "cut") == 0) {
        *usage = "[\"cut\"]";
        br_graphics_cut(ctx);
    } else if (SDL_strcmp(name, "texture") == 0) {
        *usage = "[\"texture\", \"path\", tint?, x?, y?, w?, h?]";
        const cJSON *ref = cJSON_GetArrayItem(cmd, 1);
        int size = cJSON_GetArraySize(cmd);
        if (!cJSON_IsString(ref) || size > 7) {
            return false;
        }
        br_color tint = br_color_make(1, 1, 1, 1);
        if (size > 2) {
            size_t mark = br_data_path_push_index(e, 2);
            br_data_value_color(e, cJSON_GetArrayItem(cmd, 2), &tint);
            br_data_path_pop(e, mark);
        }
        for (int i = 3; i < size; i++) {
            const cJSON *item = cJSON_GetArrayItem(cmd, i);
            if (!cJSON_IsNumber(item)) {
                return false;
            }
            a[i - 3] = (float)item->valuedouble;
        }
        size_t mark = br_data_path_push_index(e, 1);
        br_texture t = br_data_texture(e, ref->valuestring);
        br_data_path_pop(e, mark);
        br_graphics_texture(ctx, t, tint, a[0], a[1], a[2], a[3]);
    } else if (SDL_strcmp(name, "save") == 0) {
        *usage = "[\"save\"]";
        br_graphics_save(ctx);
    } else if (SDL_strcmp(name, "restore") == 0) {
        *usage = "[\"restore\"]";
        br_graphics_restore(ctx);
    } else if (SDL_strcmp(name, "translate") == 0) {
        *usage = "[\"translate\", x, y]";
        if (!br_data_args(cmd, 1, 2, 2, a)) {
            return false;
        }
        br_graphics_translate(ctx, a[0], a[1]);
    } else if (SDL_strcmp(name, "rotate") == 0) {
        *usage = "[\"rotate\", radians]";
        if (!br_data_args(cmd, 1, 1, 1, a)) {
            return false;
        }
        br_graphics_rotate(ctx, a[0]);
    } else if (SDL_strcmp(name, "scale") == 0) {
        *usage = "[\"scale\", sx, sy?]";
        if (!br_data_args(cmd, 1, 1, 2, a)) {
            return false;
        }
        br_graphics_scale(ctx, a[0], cJSON_GetArraySize(cmd) == 3 ? a[1] : a[0]);
    } else if (SDL_strcmp(name, "set_transform") == 0) {
        *usage = "[\"set_transform\", a, b, c, d, tx, ty]";
        if (!br_data_args(cmd, 1, 6, 6, a)) {
            return false;
        }
        br_graphics_set_transform(ctx, (br_mat3){ a[0], a[1], a[2], a[3], a[4], a[5] });
    } else {
        *usage = NULL;
        return false;
    }
    return true;
}

static void br_data_commands(br_engine *e, br_graphics_context *ctx, const cJSON *commands)
{
    size_t mark = br_data_path_push_key(e, "commands");
    if (!cJSON_IsArray(commands)) {
        br_data_errorf(e, "expected an array of [\"command\", args...]");
        br_data_path_pop(e, mark);
        return;
    }
    int i = 0;
    const cJSON *cmd = NULL;
    cJSON_ArrayForEach(cmd, commands)
    {
        size_t im = br_data_path_push_index(e, i++);
        const cJSON *name = cJSON_GetArrayItem(cmd, 0);
        const char *usage = NULL;
        if (!cJSON_IsArray(cmd) || !cJSON_IsString(name)) {
            br_data_errorf(e, "expected [\"command\", args...]");
        } else if (!br_data_command(e, ctx, name->valuestring, cmd, &usage)) {
            if (usage == NULL) {
                br_data_errorf(e, "unknown graphics command '%s'", name->valuestring);
            } else {
                br_data_errorf(e, "bad arguments, expected %s", usage);
            }
        }
        br_data_path_pop(e, im);
    }
    br_data_path_pop(e, mark);
}

static bool br_data_graphics_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_graphics_context *ctx = br_graphics_context_of(e, n);
    if (ctx == NULL) {
        return false;
    }
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(json, "commands");
    const cJSON *svg = cJSON_GetObjectItemCaseSensitive(json, "svg");
    if (commands == NULL && svg == NULL) {
        return true;
    }
    br_graphics_clear(ctx);
    if (commands != NULL) {
        br_data_commands(e, ctx, commands);
    }
    if (svg != NULL) {
        size_t mark = br_data_path_push_key(e, "svg");
        if (!cJSON_IsString(svg)) {
            br_data_errorf(e, "expected SVG path data");
        } else if (!br_graphics_svg_path(ctx, svg->valuestring)) {
            br_data_errorf(e, "SVG path data has a syntax error (drawn up to it)");
        }
        br_data_path_pop(e, mark);
        const cJSON *fill = cJSON_GetObjectItemCaseSensitive(json, "fill");
        const cJSON *stroke = cJSON_GetObjectItemCaseSensitive(json, "stroke");
        if (fill != NULL) {
            br_fill_style style;
            size_t fm = br_data_path_push_key(e, "fill");
            if (br_node_json_fill_style(e, fill, &style)) {
                br_graphics_fill(ctx, &style);
            }
            br_data_path_pop(e, fm);
        }
        if (stroke != NULL) {
            br_stroke_style style;
            size_t sm = br_data_path_push_key(e, "stroke");
            if (br_data_stroke_style(e, stroke, &style)) {
                br_graphics_stroke(ctx, &style);
            }
            br_data_path_pop(e, sm);
        }
    }
    return true;
}

void br_node_json_graphics_register(br_engine *e)
{
    uint32_t type = br_graphics_type(e);
    br_node_type_set_from_json(e, type, br_data_graphics_from_json);
    /* commands cannot be read back from a context: saving keeps them as loaded */
    br_data_add_type(e, "graphics", type, NULL, NULL);
}
