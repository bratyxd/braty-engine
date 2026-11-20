/* br_graphics: graphics contexts (path building, shapes, styles, SVG path data) and the
 * "graphics" node type. Triangulation lives in br_graphics_build.c. */
#include "graphics/br_graphics_internal.h"
#include "app/br_engine_internal.h"

#include "textured.frag.h"
#include "textured.vert.h"

/* ---- styles */

br_fill_style br_graphics_style_color(br_color color)
{
    br_fill_style s;
    SDL_zero(s);
    s.kind = BR_GRAPHICS_FILL_COLOR;
    s.color = color;
    s.pattern.matrix = br_mat3_identity();
    return s;
}

br_fill_style br_graphics_style_linear(br_graphics_space space, float x0, float y0, float x1, float y1)
{
    br_fill_style s = br_graphics_style_color(br_color_make(1, 1, 1, 1));
    s.kind = BR_GRAPHICS_FILL_LINEAR;
    s.gradient.space = space;
    s.gradient.start = br_vec2_make(x0, y0);
    s.gradient.end = br_vec2_make(x1, y1);
    return s;
}

br_fill_style br_graphics_style_radial(br_graphics_space space, float cx, float cy, float inner_radius,
                                       float outer_radius)
{
    br_fill_style s = br_graphics_style_color(br_color_make(1, 1, 1, 1));
    s.kind = BR_GRAPHICS_FILL_RADIAL;
    s.gradient.space = space;
    s.gradient.center = br_vec2_make(cx, cy);
    s.gradient.inner_radius = inner_radius;
    s.gradient.outer_radius = outer_radius;
    return s;
}

br_fill_style br_graphics_style_pattern(br_texture texture, br_mat3 matrix)
{
    br_fill_style s = br_graphics_style_color(br_color_make(1, 1, 1, 1));
    s.kind = BR_GRAPHICS_FILL_PATTERN;
    s.pattern.texture = texture;
    s.pattern.matrix = matrix;
    return s;
}

void br_graphics_style_add_stop(br_fill_style *style, float offset, br_color color)
{
    if (style == NULL || style->gradient.stop_count >= BR_GRAPHICS_MAX_STOPS) {
        return;
    }
    br_gradient_stop *stop = &style->gradient.stops[style->gradient.stop_count++];
    stop->offset = br_clampf(offset, 0.0f, 1.0f);
    stop->color = color;
}

br_stroke_style br_graphics_stroke_style(float width, br_color color)
{
    br_stroke_style s;
    SDL_zero(s);
    s.fill = br_graphics_style_color(color);
    s.width = width;
    s.alignment = 0.5f;
    s.cap = BR_GRAPHICS_CAP_BUTT;
    s.join = BR_GRAPHICS_JOIN_MITER;
    s.miter_limit = 10.0f;
    return s;
}

/* ---- gradient textures */

static br_color gradient_color_at(const br_gradient_stop *stops, uint32_t count, float t)
{
    if (count == 0) {
        return br_color_make(1, 1, 1, 1);
    }
    if (t <= stops[0].offset) {
        return stops[0].color;
    }
    for (uint32_t i = 1; i < count; i++) {
        if (t <= stops[i].offset) {
            float span = stops[i].offset - stops[i - 1].offset;
            float f = span > 0.0f ? (t - stops[i - 1].offset) / span : 1.0f;
            return br_color_lerp(stops[i - 1].color, stops[i].color, f);
        }
    }
    return stops[count - 1].color;
}

static void put_color(uint8_t *px, br_color c)
{
    px[0] = (uint8_t)(br_clampf(c.r, 0, 1) * 255.0f + 0.5f);
    px[1] = (uint8_t)(br_clampf(c.g, 0, 1) * 255.0f + 0.5f);
    px[2] = (uint8_t)(br_clampf(c.b, 0, 1) * 255.0f + 0.5f);
    px[3] = (uint8_t)(br_clampf(c.a, 0, 1) * 255.0f + 0.5f);
}

/* Bakes a gradient into a small texture: linear = 256x1 along u, radial = 128x128 over the
 * outer circle's bounding square (edges clamp to the last stop). */
static br_texture bake_gradient(br_engine *e, const br_fill_style *style)
{
    br_gradient_stop stops[BR_GRAPHICS_MAX_STOPS];
    uint32_t count = style->gradient.stop_count;
    for (uint32_t i = 0; i < count; i++) {
        stops[i] = style->gradient.stops[i];
    }
    for (uint32_t i = 1; i < count; i++) { /* insertion sort by offset */
        for (uint32_t j = i; j > 0 && stops[j].offset < stops[j - 1].offset; j--) {
            br_gradient_stop t = stops[j];
            stops[j] = stops[j - 1];
            stops[j - 1] = t;
        }
    }
    bool radial = style->kind == BR_GRAPHICS_FILL_RADIAL;
    uint32_t w = radial ? 128 : 256;
    uint32_t h = radial ? 128 : 1;
    uint8_t *pixels = br_alloc(w * h * 4);
    if (pixels == NULL) {
        return br_texture_white(e);
    }
    float outer = style->gradient.outer_radius > 0.0f ? style->gradient.outer_radius : 1.0f;
    float inner = br_clampf(style->gradient.inner_radius, 0.0f, outer);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float t = ((float)x + 0.5f) / (float)w;
            if (radial) {
                float dx = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f;
                float dy = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
                float d = SDL_sqrtf(dx * dx + dy * dy) * outer;
                t = outer > inner ? (d - inner) / (outer - inner) : 1.0f;
            }
            put_color(&pixels[(y * w + x) * 4], gradient_color_at(stops, count, br_clampf(t, 0.0f, 1.0f)));
        }
    }
    br_texture_style ts;
    SDL_zero(ts); /* linear, clamp */
    br_texture tex = br_texture_create_rgba8(e, w, h, pixels, &ts);
    br_free(pixels);
    return tex;
}

/* The texture an instruction with this fill draws with. */
static br_texture fill_texture(br_graphics_context *ctx, const br_fill_style *fill)
{
    if (fill->kind == BR_GRAPHICS_FILL_PATTERN) {
        br_texture_info info;
        br_texture_get_info(ctx->e, fill->pattern.texture, &info);
        if (info.style.address_mode != BR_ADDRESS_REPEAT) {
            br_texture_style st = info.style;
            st.address_mode = BR_ADDRESS_REPEAT;
            br_texture_set_style(ctx->e, fill->pattern.texture, &st);
        }
        return fill->pattern.texture;
    }
    if (fill->kind != BR_GRAPHICS_FILL_LINEAR && fill->kind != BR_GRAPHICS_FILL_RADIAL) {
        return br_texture_white(ctx->e);
    }
    /* reuse an identical gradient already baked for this context */
    for (uint32_t i = 0; i < ctx->instructions.count; i++) {
        const br_gfx_instruction *in = br_array_at(&ctx->instructions, i);
        const br_fill_style *f = &in->style.fill;
        if (in->kind != BR_GFX_TEXTURE && f->kind == fill->kind &&
            SDL_memcmp(&f->gradient, &fill->gradient, sizeof(br_fill_gradient)) == 0) {
            return in->texture;
        }
    }
    br_texture tex = bake_gradient(ctx->e, fill);
    br_array_push(&ctx->owned_textures, &tex);
    return tex;
}

/* ---- contexts */

br_graphics_context *br_graphics_context_create(br_engine *e)
{
    if (e == NULL || e->graphics == NULL) {
        return NULL;
    }
    br_graphics_context *ctx = br_alloc(sizeof(br_graphics_context));
    if (ctx == NULL) {
        return NULL;
    }
    SDL_zerop(ctx);
    ctx->e = e;
    ctx->refs = 1;
    br_array_init(&ctx->instructions, sizeof(br_gfx_instruction));
    br_gfx_path_init(&ctx->path);
    ctx->transform = br_mat3_identity();
    br_array_init(&ctx->stack, sizeof(br_mat3));
    br_array_init(&ctx->owned_textures, sizeof(br_texture));
    br_array_init(&ctx->vertices, sizeof(br_vertex2d));
    br_array_init(&ctx->indices, sizeof(uint16_t));
    br_array_init(&ctx->groups, sizeof(br_gfx_group));
    br_array_init(&ctx->lines, sizeof(br_vertex2d));
    ctx->version = 1;
    br_array_push(&e->graphics->contexts, &ctx);
    return ctx;
}

void br_graphics_clear(br_graphics_context *ctx)
{
    if (ctx == NULL) {
        return;
    }
    for (uint32_t i = 0; i < ctx->instructions.count; i++) {
        br_gfx_instruction *in = br_array_at(&ctx->instructions, i);
        br_gfx_path_free(&in->path);
        br_gfx_path_free(&in->hole);
    }
    br_array_clear(&ctx->instructions);
    for (uint32_t i = 0; i < ctx->owned_textures.count; i++) {
        br_texture_destroy(ctx->e, *(br_texture *)br_array_at(&ctx->owned_textures, i));
    }
    br_array_clear(&ctx->owned_textures);
    br_array_clear(&ctx->path.points);
    br_array_clear(&ctx->path.subpaths);
    ctx->path_used = false;
    ctx->subpath_open = false;
    ctx->cursor = br_vec2_make(0, 0);
    ctx->transform = br_mat3_identity();
    br_array_clear(&ctx->stack);
    ctx->version++;
}

static void context_free(br_graphics_context *ctx)
{
    br_graphics_clear(ctx);
    br_array_free(&ctx->instructions);
    br_gfx_path_free(&ctx->path);
    br_array_free(&ctx->stack);
    br_array_free(&ctx->owned_textures);
    br_array_free(&ctx->vertices);
    br_array_free(&ctx->indices);
    br_array_free(&ctx->groups);
    br_array_free(&ctx->lines);
    br_array *list = &ctx->e->graphics->contexts;
    for (uint32_t i = 0; i < list->count; i++) {
        if (*(br_graphics_context **)br_array_at(list, i) == ctx) {
            br_array_remove_swap(list, i);
            break;
        }
    }
    br_free(ctx);
}

void br_graphics_context_destroy(br_graphics_context *ctx)
{
    if (ctx == NULL || ctx->refs == 0) {
        return;
    }
    ctx->refs--;
    if (ctx->refs == 0) {
        context_free(ctx);
    }
}

/* ---- path building */

static void changed(br_graphics_context *ctx)
{
    ctx->version++;
}

/* After a fill/stroke the next path call starts a fresh path (Pixi v8). */
static void begin_path_if_used(br_graphics_context *ctx)
{
    if (!ctx->path_used) {
        return;
    }
    br_array_clear(&ctx->path.points);
    br_array_clear(&ctx->path.subpaths);
    ctx->path_used = false;
    ctx->subpath_open = false;
    ctx->after_close = false;
    ctx->path_id++;
}

/* Uniform scale of the current transform (curve tessellation density). */
static float transform_scale(const br_graphics_context *ctx)
{
    const br_mat3 *m = &ctx->transform;
    float s = SDL_sqrtf(SDL_fabsf(m->a * m->d - m->b * m->c));
    return s > 0.0f ? s : 1.0f;
}

bool br_gfx_finite(float v)
{
    return !SDL_isnanf(v) && !SDL_isinff(v);
}

static bool point_ok(br_vec2 p)
{
    return br_gfx_finite(p.x) && br_gfx_finite(p.y) && SDL_fabsf(p.x) <= BR_GFX_MAX_COORD &&
           SDL_fabsf(p.y) <= BR_GFX_MAX_COORD;
}

/* Appends an (untransformed) point to the open sub-path. Every point of every path call comes
 * through here, so this is where bad numbers are stopped. */
static void add_point(br_graphics_context *ctx, br_vec2 p)
{
    br_vec2 t = br_mat3_apply(ctx->transform, p);
    if (!point_ok(p) || !point_ok(t)) {
        if (!ctx->bad_point_logged) {
            br_log_error("graphics: dropped a point that is not finite or beyond +-%g", (double)BR_GFX_MAX_COORD);
            ctx->bad_point_logged = true;
        }
        return;
    }
    br_gfx_subpath *s = br_array_at(&ctx->path.subpaths, ctx->path.subpaths.count - 1);
    if (s->count >= BR_GFX_MAX_SUBPATH_POINTS) {
        if (!ctx->full_logged) {
            br_log_error("graphics: a sub-path has more than %u points; the rest are dropped",
                         BR_GFX_MAX_SUBPATH_POINTS);
            ctx->full_logged = true;
        }
        return;
    }
    br_array_push(&ctx->path.points, &t);
    s->count++;
    ctx->cursor = p;
}

static void start_subpath(br_graphics_context *ctx, br_vec2 p, bool convex)
{
    br_gfx_subpath s = { ctx->path.points.count, 0, false, convex };
    br_array_push(&ctx->path.subpaths, &s);
    ctx->subpath_open = true;
    ctx->after_close = false;
    ctx->subpath_start = p;
    add_point(ctx, p);
}

/* Continues the open sub-path, or starts one, with p. After close_path the new sub-path
 * starts at the current point (the closed one's start), as in canvas and SVG. */
static void continue_to(br_graphics_context *ctx, br_vec2 p)
{
    if (ctx->subpath_open) {
        add_point(ctx, p);
    } else if (ctx->after_close) {
        start_subpath(ctx, ctx->cursor, false);
        add_point(ctx, p);
    } else {
        start_subpath(ctx, p, false);
    }
}

/* Segments for a Bezier whose control polygon is `len` long (NaN-safe). */
static uint32_t curve_steps(float len)
{
    float n = SDL_ceilf(SDL_sqrtf(len) * 1.5f);
    if (!(n >= 2.0f)) {
        n = 2.0f;
    }
    if (n > 128.0f) {
        n = 128.0f;
    }
    return (uint32_t)n;
}

/* Caps a side / point count from data. */
static uint32_t cap_sides(uint32_t n, const char *what)
{
    if (n > BR_GFX_MAX_SIDES) {
        br_log_error("graphics: %s %u capped at %u", what, n, BR_GFX_MAX_SIDES);
        return BR_GFX_MAX_SIDES;
    }
    return n;
}

/* Closes the shape being built by a shape call. */
static void end_shape(br_graphics_context *ctx)
{
    if (ctx->path.subpaths.count > 0) {
        br_gfx_subpath *s = br_array_at(&ctx->path.subpaths, ctx->path.subpaths.count - 1);
        s->closed = true;
    }
    ctx->subpath_open = false;
    changed(ctx);
}

/* Points of an elliptical arc (rotated by phi) from angle a1 over `sweep`; the first point
 * only when include_first. */
static void add_arc(br_graphics_context *ctx, br_vec2 c, float rx, float ry, float phi, float a1, float sweep,
                    bool include_first)
{
    uint32_t steps = br_gfx_arc_segments(SDL_max(rx, ry) * transform_scale(ctx), sweep);
    float cp = SDL_cosf(phi);
    float sp = SDL_sinf(phi);
    for (uint32_t i = include_first ? 0 : 1; i <= steps; i++) {
        float a = a1 + sweep * (float)i / (float)steps;
        float ex = rx * SDL_cosf(a);
        float ey = ry * SDL_sinf(a);
        continue_to(ctx, br_vec2_make(c.x + ex * cp - ey * sp, c.y + ex * sp + ey * cp));
    }
}

/* Rounds the corner p0 -> p1 -> p2 with radius r (canvas arcTo); r is clamped to fit
 * half of each edge when clamp is set. */
static void add_corner(br_graphics_context *ctx, br_vec2 p0, br_vec2 p1, br_vec2 p2, float r, bool clamp)
{
    br_vec2 v1 = br_vec2_sub(p0, p1);
    br_vec2 v2 = br_vec2_sub(p2, p1);
    float l1 = br_vec2_length(v1);
    float l2 = br_vec2_length(v2);
    v1 = br_vec2_normalize(v1);
    v2 = br_vec2_normalize(v2);
    float cr = br_vec2_cross(v1, v2);
    if (r <= 0.0f || l1 <= 0.0f || l2 <= 0.0f || SDL_fabsf(cr) < 1e-6f) {
        continue_to(ctx, p1);
        return;
    }
    float angle = SDL_acosf(br_clampf(br_vec2_dot(v1, v2), -1.0f, 1.0f));
    float half_tan = SDL_tanf(angle * 0.5f);
    float t = r / half_tan;
    if (clamp && t > SDL_min(l1, l2) * 0.5f) {
        t = SDL_min(l1, l2) * 0.5f;
        r = t * half_tan;
    }
    br_vec2 t1 = br_vec2_add(p1, br_vec2_scale(v1, t));
    br_vec2 t2 = br_vec2_add(p1, br_vec2_scale(v2, t));
    br_vec2 c = br_vec2_add(p1, br_vec2_scale(br_vec2_normalize(br_vec2_add(v1, v2)), r / SDL_sinf(angle * 0.5f)));
    float a1 = SDL_atan2f(t1.y - c.y, t1.x - c.x);
    float delta = SDL_atan2f(t2.y - c.y, t2.x - c.x) - a1;
    while (delta > BR_PI) {
        delta -= BR_TAU;
    }
    while (delta < -BR_PI) {
        delta += BR_TAU;
    }
    continue_to(ctx, t1);
    add_arc(ctx, c, r, r, 0.0f, a1, delta, false);
}

void br_graphics_move_to(br_graphics_context *ctx, float x, float y)
{
    if (ctx == NULL) {
        return;
    }
    begin_path_if_used(ctx);
    start_subpath(ctx, br_vec2_make(x, y), false);
    changed(ctx);
}

void br_graphics_line_to(br_graphics_context *ctx, float x, float y)
{
    if (ctx == NULL) {
        return;
    }
    begin_path_if_used(ctx);
    continue_to(ctx, br_vec2_make(x, y));
    changed(ctx);
}

/* Starts at the cursor when there is no open sub-path but a current point exists. */
static void ensure_current(br_graphics_context *ctx, br_vec2 fallback)
{
    begin_path_if_used(ctx);
    if (!ctx->subpath_open) {
        start_subpath(ctx, ctx->path.points.count > 0 ? ctx->cursor : fallback, false);
    }
}

void br_graphics_bezier_curve_to(br_graphics_context *ctx, float cp1x, float cp1y, float cp2x, float cp2y,
                                 float x, float y)
{
    if (ctx == NULL) {
        return;
    }
    ensure_current(ctx, br_vec2_make(cp1x, cp1y));
    br_vec2 p0 = ctx->cursor;
    br_vec2 p1 = br_vec2_make(cp1x, cp1y);
    br_vec2 p2 = br_vec2_make(cp2x, cp2y);
    br_vec2 p3 = br_vec2_make(x, y);
    /* adaptive: more segments for a longer control polygon */
    float len = (br_vec2_distance(p0, p1) + br_vec2_distance(p1, p2) + br_vec2_distance(p2, p3)) * transform_scale(ctx);
    uint32_t steps = curve_steps(len);
    for (uint32_t i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        float b0 = u * u * u;
        float b1 = 3.0f * u * u * t;
        float b2 = 3.0f * u * t * t;
        float b3 = t * t * t;
        add_point(ctx, br_vec2_make(b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
                                    b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y));
    }
    changed(ctx);
}

void br_graphics_quadratic_curve_to(br_graphics_context *ctx, float cpx, float cpy, float x, float y)
{
    if (ctx == NULL) {
        return;
    }
    ensure_current(ctx, br_vec2_make(cpx, cpy));
    br_vec2 p0 = ctx->cursor;
    br_vec2 p1 = br_vec2_make(cpx, cpy);
    br_vec2 p2 = br_vec2_make(x, y);
    float len = (br_vec2_distance(p0, p1) + br_vec2_distance(p1, p2)) * transform_scale(ctx);
    uint32_t steps = curve_steps(len);
    for (uint32_t i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        add_point(ctx, br_vec2_make(u * u * p0.x + 2.0f * u * t * p1.x + t * t * p2.x,
                                    u * u * p0.y + 2.0f * u * t * p1.y + t * t * p2.y));
    }
    changed(ctx);
}

void br_graphics_arc(br_graphics_context *ctx, float cx, float cy, float radius, float start_angle, float end_angle,
                     bool counterclockwise)
{
    if (ctx == NULL) {
        return;
    }
    begin_path_if_used(ctx);
    float sweep = end_angle - start_angle;
    if (!counterclockwise) {
        if (sweep >= BR_TAU) {
            sweep = BR_TAU;
        } else {
            sweep = SDL_fmodf(sweep, BR_TAU);
            if (sweep < 0.0f) {
                sweep += BR_TAU;
            }
        }
    } else {
        if (sweep <= -BR_TAU) {
            sweep = -BR_TAU;
        } else {
            sweep = SDL_fmodf(sweep, BR_TAU);
            if (sweep > 0.0f) {
                sweep -= BR_TAU;
            }
        }
    }
    add_arc(ctx, br_vec2_make(cx, cy), radius, radius, 0.0f, start_angle, sweep, true);
    changed(ctx);
}

void br_graphics_arc_to(br_graphics_context *ctx, float x1, float y1, float x2, float y2, float radius)
{
    if (ctx == NULL) {
        return;
    }
    ensure_current(ctx, br_vec2_make(x1, y1));
    add_corner(ctx, ctx->cursor, br_vec2_make(x1, y1), br_vec2_make(x2, y2), radius, false);
    changed(ctx);
}

static float vec_angle(float ux, float uy, float vx, float vy)
{
    return SDL_atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
}

void br_graphics_arc_to_svg(br_graphics_context *ctx, float rx, float ry, float x_axis_rotation, bool large_arc,
                            bool sweep, float x, float y)
{
    if (ctx == NULL) {
        return;
    }
    ensure_current(ctx, br_vec2_make(x, y));
    br_vec2 p1 = ctx->cursor;
    br_vec2 p2 = br_vec2_make(x, y);
    rx = SDL_fabsf(rx);
    ry = SDL_fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f) {
        continue_to(ctx, p2);
        changed(ctx);
        return;
    }
    if (br_vec2_distance(p1, p2) < 1e-6f) {
        return;
    }
    /* SVG 1.1 appendix F.6.5: endpoint to centre parameterisation */
    float cp = SDL_cosf(x_axis_rotation);
    float sp = SDL_sinf(x_axis_rotation);
    float dx = (p1.x - p2.x) * 0.5f;
    float dy = (p1.y - p2.y) * 0.5f;
    float x1p = cp * dx + sp * dy;
    float y1p = -sp * dx + cp * dy;
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        rx *= SDL_sqrtf(lambda);
        ry *= SDL_sqrtf(lambda);
    }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float coef = den > 0.0f ? SDL_sqrtf(SDL_max(0.0f, num / den)) : 0.0f;
    if (large_arc == sweep) {
        coef = -coef;
    }
    float cxp = coef * rx * y1p / ry;
    float cyp = -coef * ry * x1p / rx;
    br_vec2 c = br_vec2_make(cp * cxp - sp * cyp + (p1.x + p2.x) * 0.5f, sp * cxp + cp * cyp + (p1.y + p2.y) * 0.5f);
    float ux = (x1p - cxp) / rx;
    float uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx;
    float vy = (-y1p - cyp) / ry;
    float theta1 = vec_angle(1.0f, 0.0f, ux, uy);
    float dtheta = vec_angle(ux, uy, vx, vy);
    if (!sweep && dtheta > 0.0f) {
        dtheta -= BR_TAU;
    } else if (sweep && dtheta < 0.0f) {
        dtheta += BR_TAU;
    }
    add_arc(ctx, c, rx, ry, x_axis_rotation, theta1, dtheta, false);
    ctx->cursor = p2; /* exact end point, not the tessellated one */
    changed(ctx);
}

void br_graphics_close_path(br_graphics_context *ctx)
{
    if (ctx == NULL || !ctx->subpath_open) {
        return;
    }
    br_gfx_subpath *s = br_array_at(&ctx->path.subpaths, ctx->path.subpaths.count - 1);
    s->closed = true;
    ctx->subpath_open = false;
    ctx->after_close = true;
    ctx->cursor = ctx->subpath_start;
    changed(ctx);
}

/* ---- shapes */

static void begin_shape(br_graphics_context *ctx, br_vec2 first, bool convex)
{
    begin_path_if_used(ctx);
    ctx->subpath_open = false;
    start_subpath(ctx, first, convex);
}

void br_graphics_rect(br_graphics_context *ctx, float x, float y, float w, float h)
{
    if (ctx == NULL) {
        return;
    }
    begin_shape(ctx, br_vec2_make(x, y), true);
    add_point(ctx, br_vec2_make(x + w, y));
    add_point(ctx, br_vec2_make(x + w, y + h));
    add_point(ctx, br_vec2_make(x, y + h));
    end_shape(ctx);
}

void br_graphics_round_rect(br_graphics_context *ctx, float x, float y, float w, float h, float radius)
{
    if (ctx == NULL) {
        return;
    }
    float r = SDL_min(radius, SDL_min(SDL_fabsf(w), SDL_fabsf(h)) * 0.5f);
    if (r <= 0.0f) {
        br_graphics_rect(ctx, x, y, w, h);
        return;
    }
    begin_shape(ctx, br_vec2_make(x, y + r), true);
    add_arc(ctx, br_vec2_make(x + r, y + r), r, r, 0.0f, BR_PI, BR_PI * 0.5f, true);
    add_arc(ctx, br_vec2_make(x + w - r, y + r), r, r, 0.0f, BR_PI * 1.5f, BR_PI * 0.5f, true);
    add_arc(ctx, br_vec2_make(x + w - r, y + h - r), r, r, 0.0f, 0.0f, BR_PI * 0.5f, true);
    add_arc(ctx, br_vec2_make(x + r, y + h - r), r, r, 0.0f, BR_PI * 0.5f, BR_PI * 0.5f, true);
    end_shape(ctx);
}

void br_graphics_ellipse(br_graphics_context *ctx, float x, float y, float radius_x, float radius_y)
{
    if (ctx == NULL || !(radius_x > 0.0f) || !(radius_y > 0.0f)) {
        return;
    }
    uint32_t n = br_gfx_arc_segments(SDL_max(radius_x, radius_y) * transform_scale(ctx), BR_TAU);
    if (n < 8) {
        n = 8;
    }
    begin_shape(ctx, br_vec2_make(x + radius_x, y), true);
    for (uint32_t i = 1; i < n; i++) {
        float a = BR_TAU * (float)i / (float)n;
        add_point(ctx, br_vec2_make(x + SDL_cosf(a) * radius_x, y + SDL_sinf(a) * radius_y));
    }
    end_shape(ctx);
}

void br_graphics_circle(br_graphics_context *ctx, float x, float y, float radius)
{
    br_graphics_ellipse(ctx, x, y, radius, radius);
}

void br_graphics_poly(br_graphics_context *ctx, const br_vec2 *points, uint32_t count, bool close)
{
    if (ctx == NULL || points == NULL || count == 0) {
        return;
    }
    begin_shape(ctx, points[0], false);
    for (uint32_t i = 1; i < count; i++) {
        add_point(ctx, points[i]);
    }
    if (close) {
        end_shape(ctx);
    } else {
        changed(ctx); /* stays open: line_to continues it */
    }
}

/* Vertices of a regular polygon (first one straight up, then clockwise). */
static br_vec2 regular_vertex(float x, float y, float radius, uint32_t sides, float rotation, uint32_t i)
{
    float a = -BR_PI * 0.5f + rotation + BR_TAU * (float)i / (float)sides;
    return br_vec2_make(x + SDL_cosf(a) * radius, y + SDL_sinf(a) * radius);
}

void br_graphics_regular_poly(br_graphics_context *ctx, float x, float y, float radius, uint32_t sides,
                              float rotation)
{
    if (ctx == NULL || sides < 3) {
        return;
    }
    sides = cap_sides(sides, "regular_poly sides");
    begin_shape(ctx, regular_vertex(x, y, radius, sides, rotation, 0), true);
    for (uint32_t i = 1; i < sides; i++) {
        add_point(ctx, regular_vertex(x, y, radius, sides, rotation, i));
    }
    end_shape(ctx);
}

void br_graphics_round_poly(br_graphics_context *ctx, float x, float y, float radius, uint32_t sides,
                            float corner_radius, float rotation)
{
    if (ctx == NULL || sides < 3) {
        return;
    }
    sides = cap_sides(sides, "round_poly sides");
    br_vec2 first = regular_vertex(x, y, radius, sides, rotation, 0);
    br_vec2 last = regular_vertex(x, y, radius, sides, rotation, sides - 1);
    begin_shape(ctx, br_vec2_lerp(last, first, 0.5f), true);
    for (uint32_t i = 0; i < sides; i++) {
        br_vec2 prev = regular_vertex(x, y, radius, sides, rotation, (i + sides - 1) % sides);
        br_vec2 at = regular_vertex(x, y, radius, sides, rotation, i);
        br_vec2 next = regular_vertex(x, y, radius, sides, rotation, (i + 1) % sides);
        add_corner(ctx, prev, at, next, corner_radius, true);
    }
    end_shape(ctx);
}

void br_graphics_star(br_graphics_context *ctx, float x, float y, uint32_t points, float radius, float inner_radius,
                      float rotation)
{
    if (ctx == NULL || points < 2) {
        return;
    }
    points = cap_sides(points, "star points");
    float inner = inner_radius > 0.0f ? inner_radius : radius * 0.5f;
    uint32_t n = points * 2;
    for (uint32_t i = 0; i < n; i++) {
        float r = i % 2 == 0 ? radius : inner;
        float a = -BR_PI * 0.5f + rotation + BR_PI * (float)i / (float)points;
        br_vec2 p = br_vec2_make(x + SDL_cosf(a) * r, y + SDL_sinf(a) * r);
        if (i == 0) {
            begin_shape(ctx, p, false);
        } else {
            add_point(ctx, p);
        }
    }
    end_shape(ctx);
}

void br_graphics_chamfer_rect(br_graphics_context *ctx, float x, float y, float w, float h, float chamfer)
{
    if (ctx == NULL) {
        return;
    }
    float c = SDL_min(chamfer, SDL_min(w, h) * 0.5f);
    if (c <= 0.0f) {
        br_graphics_rect(ctx, x, y, w, h);
        return;
    }
    begin_shape(ctx, br_vec2_make(x + c, y), true);
    add_point(ctx, br_vec2_make(x + w - c, y));
    add_point(ctx, br_vec2_make(x + w, y + c));
    add_point(ctx, br_vec2_make(x + w, y + h - c));
    add_point(ctx, br_vec2_make(x + w - c, y + h));
    add_point(ctx, br_vec2_make(x + c, y + h));
    add_point(ctx, br_vec2_make(x, y + h - c));
    add_point(ctx, br_vec2_make(x, y + c));
    end_shape(ctx);
}

void br_graphics_fillet_rect(br_graphics_context *ctx, float x, float y, float w, float h, float fillet)
{
    if (ctx == NULL) {
        return;
    }
    if (fillet >= 0.0f) {
        br_graphics_round_rect(ctx, x, y, w, h, fillet);
        return;
    }
    float r = SDL_min(-fillet, SDL_min(w, h) * 0.5f);
    br_vec2 corners[4] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    begin_shape(ctx, br_vec2_make(x, y + r), false);
    for (uint32_t k = 0; k < 4; k++) {
        /* scooped corner: quarter circle around the corner itself, curving inwards */
        add_arc(ctx, corners[k], r, r, 0.0f, BR_PI * 0.5f * (float)(k + 1), -BR_PI * 0.5f, k > 0);
    }
    end_shape(ctx);
}

/* ---- painting */

/* Replaces stroke numbers that would poison the geometry (NaN width, alignment, dashes). */
static void sanitize_stroke(br_stroke_style *s)
{
    if (!br_gfx_finite(s->width) || s->width < 0.0f) {
        s->width = 0.0f;
    }
    if (!br_gfx_finite(s->alignment)) {
        s->alignment = 0.5f;
    }
    if (!br_gfx_finite(s->miter_limit)) {
        s->miter_limit = 10.0f;
    }
    if (!br_gfx_finite(s->dash_offset)) {
        s->dash_offset = 0.0f;
    }
    if (s->dash_count > BR_GRAPHICS_MAX_DASHES) {
        s->dash_count = BR_GRAPHICS_MAX_DASHES;
    }
    for (uint32_t i = 0; i < s->dash_count; i++) {
        if (!br_gfx_finite(s->dash[i])) {
            s->dash_count = 0; /* solid */
        }
    }
}

static void push_instruction(br_graphics_context *ctx, br_gfx_kind kind, const br_stroke_style *style)
{
    if (ctx->path.subpaths.count == 0) {
        return; /* nothing to paint */
    }
    br_gfx_instruction in;
    SDL_zero(in);
    in.kind = kind;
    in.path_id = ctx->path_id;
    in.style = *style;
    sanitize_stroke(&in.style);
    in.texture = fill_texture(ctx, &style->fill);
    br_gfx_path_copy(&in.path, &ctx->path);
    br_gfx_path_init(&in.hole);
    br_array_push(&ctx->instructions, &in);
    ctx->path_used = true;
    changed(ctx);
}

void br_graphics_fill(br_graphics_context *ctx, const br_fill_style *style)
{
    if (ctx == NULL || style == NULL) {
        return;
    }
    br_stroke_style s;
    SDL_zero(s);
    s.fill = *style;
    push_instruction(ctx, BR_GFX_FILL, &s);
}

void br_graphics_fill_color(br_graphics_context *ctx, br_color color)
{
    br_fill_style s = br_graphics_style_color(color);
    br_graphics_fill(ctx, &s);
}

void br_graphics_stroke(br_graphics_context *ctx, const br_stroke_style *style)
{
    if (ctx == NULL || style == NULL) {
        return;
    }
    push_instruction(ctx, BR_GFX_STROKE, style);
}

void br_graphics_cut(br_graphics_context *ctx)
{
    if (ctx == NULL || ctx->instructions.count == 0 || ctx->path.subpaths.count == 0 || ctx->path_used) {
        return;
    }
    uint32_t n = ctx->instructions.count;
    br_gfx_instruction *last = br_array_at(&ctx->instructions, n - 1);
    if (last->kind != BR_GFX_TEXTURE) {
        br_gfx_path_append(&last->hole, &ctx->path);
        if (n >= 2) {
            br_gfx_instruction *before = br_array_at(&ctx->instructions, n - 2);
            if (before->kind != BR_GFX_TEXTURE && before->path_id == last->path_id) {
                br_gfx_path_append(&before->hole, &ctx->path);
            }
        }
    }
    ctx->path_used = true;
    changed(ctx);
}

void br_graphics_texture(br_graphics_context *ctx, br_texture texture, br_color tint, float x, float y, float w,
                         float h)
{
    if (ctx == NULL) {
        return;
    }
    br_texture_info info;
    br_texture_get_info(ctx->e, texture, &info);
    if (w <= 0.0f || h <= 0.0f) {
        w = info.width;
        h = info.height;
    }
    br_gfx_instruction in;
    SDL_zero(in);
    in.kind = BR_GFX_TEXTURE;
    in.path_id = ctx->path_id;
    in.style.fill = br_graphics_style_color(tint);
    in.texture = texture;
    in.quad[0] = br_mat3_apply(ctx->transform, br_vec2_make(x, y));
    in.quad[1] = br_mat3_apply(ctx->transform, br_vec2_make(x + w, y));
    in.quad[2] = br_mat3_apply(ctx->transform, br_vec2_make(x + w, y + h));
    in.quad[3] = br_mat3_apply(ctx->transform, br_vec2_make(x, y + h));
    br_gfx_path_init(&in.path);
    br_gfx_path_init(&in.hole);
    br_array_push(&ctx->instructions, &in);
    changed(ctx);
}

/* ---- transform stack */

void br_graphics_save(br_graphics_context *ctx)
{
    if (ctx != NULL) {
        br_array_push(&ctx->stack, &ctx->transform);
    }
}

void br_graphics_restore(br_graphics_context *ctx)
{
    if (ctx == NULL || ctx->stack.count == 0) {
        return;
    }
    ctx->transform = *(br_mat3 *)br_array_at(&ctx->stack, ctx->stack.count - 1);
    br_array_pop(&ctx->stack);
}

void br_graphics_translate(br_graphics_context *ctx, float x, float y)
{
    if (ctx != NULL) {
        ctx->transform = br_mat3_mul(ctx->transform, br_mat3_translation(x, y));
    }
}

void br_graphics_rotate(br_graphics_context *ctx, float radians)
{
    if (ctx != NULL) {
        ctx->transform = br_mat3_mul(ctx->transform, br_mat3_rotation(radians));
    }
}

void br_graphics_scale(br_graphics_context *ctx, float sx, float sy)
{
    if (ctx != NULL) {
        ctx->transform = br_mat3_mul(ctx->transform, br_mat3_scaling(sx, sy));
    }
}

void br_graphics_set_transform(br_graphics_context *ctx, br_mat3 m)
{
    if (ctx != NULL) {
        ctx->transform = m;
    }
}

/* ---- SVG path data */

static const char *svg_skip(const char *s)
{
    while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return s;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool br_gfx_parse_number(const char **s, float *out)
{
    const char *p = svg_skip(*s);
    double sign = 1.0;
    if (*p == '+' || *p == '-') {
        sign = *p == '-' ? -1.0 : 1.0;
        p++;
    }
    double mantissa = 0.0;
    int digits = 0;
    int scale = 0; /* decimal exponent of the digits read */
    for (; is_digit(*p); p++, digits++) {
        mantissa = mantissa * 10.0 + (double)(*p - '0');
    }
    if (*p == '.') {
        p++;
        for (; is_digit(*p); p++, digits++) {
            mantissa = mantissa * 10.0 + (double)(*p - '0');
            scale--;
        }
    }
    if (digits == 0) {
        return false;
    }
    /* an exponent only if digits follow ("2e" or "2em" stop before the 'e') */
    const char *q = p;
    if (*q == 'e' || *q == 'E') {
        q++;
        int esign = 1;
        if (*q == '+' || *q == '-') {
            esign = *q == '-' ? -1 : 1;
            q++;
        }
        if (is_digit(*q)) {
            int exponent = 0;
            for (; is_digit(*q); q++) {
                if (exponent < 100000) {
                    exponent = exponent * 10 + (*q - '0');
                }
            }
            scale += esign * exponent;
            p = q;
        }
    }
    double v = sign * mantissa * SDL_pow(10.0, (double)scale);
    if (!br_gfx_finite((float)v)) {
        return false;
    }
    *out = (float)v;
    *s = p;
    return true;
}

/* Arc flags are single characters and may be written without separators ("a5 5 0 016 0"). */
static bool svg_flag(const char **s, float *out)
{
    const char *p = svg_skip(*s);
    if (*p != '0' && *p != '1') {
        return false;
    }
    *out = *p == '1' ? 1.0f : 0.0f;
    *s = p + 1;
    return true;
}

static int svg_arg_count(char up)
{
    switch (up) {
    case 'M': case 'L': case 'T': return 2;
    case 'H': case 'V': return 1;
    case 'C': return 6;
    case 'S': case 'Q': return 4;
    case 'A': return 7;
    case 'Z': return 0;
    default: return -1;
    }
}

bool br_graphics_svg_path(br_graphics_context *ctx, const char *d)
{
    if (ctx == NULL || d == NULL) {
        return false;
    }
    const char *s = d;
    char cmd = 0;
    char prev = 0;
    br_vec2 cur = ctx->cursor;
    br_vec2 start = cur;
    br_vec2 last_cubic = cur;  /* second control point of the previous C/S */
    br_vec2 last_quad = cur;   /* control point of the previous Q/T */
    for (;;) {
        s = svg_skip(s);
        if (*s == '\0') {
            return true;
        }
        if (SDL_isalpha((unsigned char)*s)) {
            cmd = *s++;
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            br_log_error("graphics: svg path: number without a command at offset %d", (int)(s - d));
            return false;
        }
        char up = (char)SDL_toupper((unsigned char)cmd);
        bool rel = cmd != up;
        int need = svg_arg_count(up);
        if (need < 0) {
            br_log_error("graphics: svg path: unknown command '%c' at offset %d", cmd, (int)(s - d - 1));
            return false;
        }
        float v[7];
        for (int i = 0; i < need; i++) {
            bool ok = (up == 'A' && (i == 3 || i == 4)) ? svg_flag(&s, &v[i]) : br_gfx_parse_number(&s, &v[i]);
            if (!ok) {
                br_log_error("graphics: svg path: '%c' needs %d numbers (offset %d)", cmd, need, (int)(s - d));
                return false;
            }
        }
        br_vec2 base = rel ? cur : br_vec2_make(0, 0);
        if (up == 'M') {
            cur = br_vec2_make(base.x + v[0], base.y + v[1]);
            br_graphics_move_to(ctx, cur.x, cur.y);
            start = cur;
            cmd = rel ? 'l' : 'L'; /* further pairs are line-tos */
        } else if (up == 'L' || up == 'H' || up == 'V') {
            if (up == 'L') {
                cur = br_vec2_make(base.x + v[0], base.y + v[1]);
            } else if (up == 'H') {
                cur.x = base.x + v[0];
            } else {
                cur.y = base.y + v[0];
            }
            br_graphics_line_to(ctx, cur.x, cur.y);
        } else if (up == 'C' || up == 'S') {
            br_vec2 c1;
            br_vec2 c2;
            br_vec2 p;
            if (up == 'C') {
                c1 = br_vec2_make(base.x + v[0], base.y + v[1]);
                c2 = br_vec2_make(base.x + v[2], base.y + v[3]);
                p = br_vec2_make(base.x + v[4], base.y + v[5]);
            } else {
                bool reflect = prev == 'C' || prev == 'S';
                c1 = reflect ? br_vec2_sub(br_vec2_scale(cur, 2.0f), last_cubic) : cur;
                c2 = br_vec2_make(base.x + v[0], base.y + v[1]);
                p = br_vec2_make(base.x + v[2], base.y + v[3]);
            }
            br_graphics_bezier_curve_to(ctx, c1.x, c1.y, c2.x, c2.y, p.x, p.y);
            last_cubic = c2;
            cur = p;
        } else if (up == 'Q' || up == 'T') {
            br_vec2 c;
            br_vec2 p;
            if (up == 'Q') {
                c = br_vec2_make(base.x + v[0], base.y + v[1]);
                p = br_vec2_make(base.x + v[2], base.y + v[3]);
            } else {
                bool reflect = prev == 'Q' || prev == 'T';
                c = reflect ? br_vec2_sub(br_vec2_scale(cur, 2.0f), last_quad) : cur;
                p = br_vec2_make(base.x + v[0], base.y + v[1]);
            }
            br_graphics_quadratic_curve_to(ctx, c.x, c.y, p.x, p.y);
            last_quad = c;
            cur = p;
        } else if (up == 'A') {
            br_vec2 p = br_vec2_make(base.x + v[5], base.y + v[6]);
            br_graphics_arc_to_svg(ctx, v[0], v[1], v[2] * BR_DEG_TO_RAD, v[3] != 0.0f, v[4] != 0.0f, p.x, p.y);
            cur = p;
        } else {
            br_graphics_close_path(ctx);
            cur = start;
        }
        prev = up;
    }
}

/* ---- the node type */

typedef struct br_graphics_node {
    br_graphics_context *ctx;
} br_graphics_node;

static void graphics_on_create(br_engine *e, br_node n, void *data)
{
    (void)n;
    br_graphics_node *g = data;
    g->ctx = br_graphics_context_create(e);
}

static void graphics_on_destroy(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_graphics_node *g = data;
    br_graphics_context_destroy(g->ctx);
    g->ctx = NULL;
}

/* Per-channel multiply of two packed premultiplied colours. */
static uint32_t multiply_color(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        uint32_t ca = (a >> shift) & 0xffu;
        uint32_t cb = (b >> shift) & 0xffu;
        out |= ((ca * cb + 127u) / 255u) << shift;
    }
    return out;
}

static void draw_pixel_lines(br_engine *e, br_graphics_context *ctx, const br_gfx_group *grp, br_mat3 m, uint32_t tint,
                             br_render_ctx *rc)
{
    br_array *scratch = &e->graphics->scratch;
    if (!br_array_resize(scratch, grp->line_count)) {
        return;
    }
    br_vertex2d *out = scratch->data;
    const br_vertex2d *src = (const br_vertex2d *)ctx->lines.data + grp->first_line;
    for (uint32_t i = 0; i < grp->line_count; i++) {
        br_vec2 p = br_mat3_apply(m, br_vec2_make(src[i].x, src[i].y));
        out[i] = src[i];
        out[i].x = p.x;
        out[i].y = p.y;
        out[i].color = multiply_color(src[i].color, tint);
    }
    br_custom_draw draw;
    SDL_zero(draw);
    draw.vertex_shader = &br_shader_textured_vert;
    draw.fragment_shader = &br_shader_textured_frag;
    draw.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
    draw.layout.buffer_count = 1;
    draw.layout.strides[0] = sizeof(br_vertex2d);
    draw.layout.attribute_count = 3;
    draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    draw.vertices = out;
    draw.vertex_bytes = grp->line_count * (uint32_t)sizeof(br_vertex2d);
    draw.textures[0] = br_texture_white(e);
    draw.texture_count = 1;
    br_render_push_custom(rc, &draw);
}

static void graphics_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_graphics_node *g = data;
    br_graphics_context *ctx = g->ctx;
    uint32_t tint = br_render_node_color(rc, n);
    if (ctx == NULL || tint == 0) {
        return;
    }
    br_gfx_build(ctx);
    br_mat3 m = br_render_node_transform(rc, n);
    br_array *scratch = &e->graphics->scratch;
    const br_vertex2d *src = ctx->vertices.data;
    const uint16_t *indices = ctx->indices.data;
    for (uint32_t gi = 0; gi < ctx->groups.count; gi++) {
        const br_gfx_group *grp = br_array_at(&ctx->groups, gi);
        if (grp->line_count > 0) {
            draw_pixel_lines(e, ctx, grp, m, tint, rc); /* in instruction order, between triangles */
            continue;
        }
        if (!br_array_resize(scratch, grp->vertex_count)) {
            return;
        }
        br_vertex2d *out = scratch->data;
        for (uint32_t i = 0; i < grp->vertex_count; i++) {
            const br_vertex2d *v = &src[grp->first_vertex + i];
            br_vec2 p = br_mat3_apply(m, br_vec2_make(v->x, v->y));
            out[i] = *v;
            out[i].x = p.x;
            out[i].y = p.y;
            out[i].color = multiply_color(v->color, tint);
        }
        br_render_push_triangles(rc, grp->texture, out, grp->vertex_count, &indices[grp->first_index],
                                 grp->index_count);
    }
}

static bool graphics_local_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_graphics_node *g = data;
    if (g->ctx == NULL) {
        return false;
    }
    br_gfx_build(g->ctx);
    *out = g->ctx->bounds;
    return g->ctx->has_bounds;
}

static bool graphics_hit_test(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    (void)e;
    (void)n;
    const br_graphics_node *g = data;
    return br_graphics_context_contains(g->ctx, p);
}

br_node br_graphics_create(br_engine *e, br_graphics_context *ctx)
{
    br_node n = br_node_create(e, br_graphics_type(e));
    if (ctx != NULL) {
        br_graphics_set_context(e, n, ctx);
    }
    return n;
}

br_graphics_context *br_graphics_context_of(br_engine *e, br_node graphics)
{
    br_graphics_node *g = br_node_data(e, graphics, br_graphics_type(e));
    return g != NULL ? g->ctx : NULL;
}

void br_graphics_set_context(br_engine *e, br_node graphics, br_graphics_context *ctx)
{
    br_graphics_node *g = br_node_data(e, graphics, br_graphics_type(e));
    if (g == NULL || g->ctx == ctx) {
        return;
    }
    br_graphics_context *next = ctx;
    if (next != NULL) {
        next->refs++;
    } else {
        next = br_graphics_context_create(e);
    }
    br_graphics_context_destroy(g->ctx);
    g->ctx = next;
}

uint32_t br_graphics_type(br_engine *e)
{
    return e->graphics != NULL ? e->graphics->type : 0;
}

static void graphics_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_graphics_state *s = e->graphics;
    /* contexts the game never destroyed */
    while (s->contexts.count > 0) {
        br_graphics_context *ctx = *(br_graphics_context **)br_array_at(&s->contexts, s->contexts.count - 1);
        context_free(ctx);
    }
    br_array_free(&s->contexts);
    br_array_free(&s->scratch);
    br_free(s);
    e->graphics = NULL;
}

void br_graphics_register(br_engine *e)
{
    br_graphics_state *s = br_alloc(sizeof(br_graphics_state));
    if (s == NULL) {
        br_log_error("graphics: out of memory");
        return;
    }
    SDL_zerop(s);
    br_array_init(&s->contexts, sizeof(br_graphics_context *));
    br_array_init(&s->scratch, sizeof(br_vertex2d));
    e->graphics = s;
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "graphics";
    desc.data_size = sizeof(br_graphics_node);
    desc.on_create = graphics_on_create;
    desc.on_destroy = graphics_on_destroy;
    desc.local_bounds = graphics_local_bounds;
    desc.render = graphics_render;
    desc.hit_test = graphics_hit_test;
    s->type = br_node_type_register(e, &desc);
    br_engine_on_shutdown(e, graphics_shutdown, NULL);
}
