/* br_graphics.h - vector graphics (PixiJS Graphics, GraphicsContext, GraphicsPath,
 * FillGradient, FillPattern, strokes, dashes, SVG path data).
 *
 * A br_graphics_context records drawing instructions: build a path with the path/shape calls,
 * then fill or stroke it. One context can be shared by any number of "graphics" nodes; it is
 * triangulated once (on first draw after a change) and every node reuses that geometry with
 * its own transform, tint and alpha.
 *
 *   br_graphics_context *ctx = br_graphics_context_create(e);
 *   br_graphics_rect(ctx, 0, 0, 100, 50);
 *   br_graphics_fill_color(ctx, br_color_from_hex(0xff3355, 1.0f));
 *   br_stroke_style st = br_graphics_stroke_style(4.0f, br_color_from_hex(0xffffff, 1.0f));
 *   br_graphics_stroke(ctx, &st);
 *   br_node a = br_graphics_create(e, ctx);   shares ctx
 *   br_graphics_context_destroy(ctx);         drops your reference; nodes keep theirs
 *
 * Path rules (as in Pixi v8): fill/stroke use the current path and keep it, so
 * rect().fill().stroke() draws both on the same rect; the next path call after a fill/stroke
 * starts a fresh path. Shape calls (rect, circle, ...) add closed sub-paths. Each sub-path is
 * filled on its own (overlaps are not subtracted); use cut() for holes.
 *
 * Coordinates are the context's local space (x right, y down, radians, positive = clockwise).
 * The transform stack applies to points as they are added.
 *
 * Limits (bad data must not hang the game; each is logged): points that are not finite or lie
 * beyond +-1e7 (after the transform) are dropped; a sub-path keeps at most 65536 points; a
 * concave fill (ear clipping, about O(n^2)) with more than 8192 points is not filled; a dashed
 * stroke makes at most 100000 dash pieces.
 */
#ifndef BR_GRAPHICS_H
#define BR_GRAPHICS_H

#include "braty/br_render.h"

/* Max colour stops per gradient, max entries in a dash pattern. */
#define BR_GRAPHICS_MAX_STOPS 8
#define BR_GRAPHICS_MAX_DASHES 8

typedef struct br_graphics_context br_graphics_context;

/* ---- styles */

typedef enum br_graphics_fill_kind {
    BR_GRAPHICS_FILL_COLOR,     /* plain colour */
    BR_GRAPHICS_FILL_LINEAR,    /* linear gradient (gradient field) */
    BR_GRAPHICS_FILL_RADIAL,    /* radial gradient (gradient field) */
    BR_GRAPHICS_FILL_PATTERN    /* repeated texture (pattern field) */
} br_graphics_fill_kind;

/* Where gradient coordinates live. LOCAL: 0..1 across the shape's bounding box (Pixi default). */
typedef enum br_graphics_space { BR_GRAPHICS_SPACE_LOCAL, BR_GRAPHICS_SPACE_GLOBAL } br_graphics_space;

typedef struct br_gradient_stop {
    float    offset;   /* 0..1 */
    br_color color;    /* straight alpha */
} br_gradient_stop;

/* PixiJS FillGradient. Linear: offset 0 at start, 1 at end. Radial: concentric, offset 0 at
 * inner_radius, 1 at outer_radius around center. */
typedef struct br_fill_gradient {
    br_graphics_space space;
    br_vec2           start, end;                     /* linear */
    br_vec2           center;                         /* radial */
    float             inner_radius, outer_radius;     /* radial */
    uint32_t          stop_count;
    br_gradient_stop  stops[BR_GRAPHICS_MAX_STOPS];   /* any order; sorted when baked */
} br_fill_gradient;

/* PixiJS FillPattern: texture repeated over the plane. matrix maps pattern space (texture
 * pixels) to context space; identity = one texel per unit from (0,0). The texture must be a
 * source (not an atlas frame); filling switches its address mode to repeat. */
typedef struct br_fill_pattern {
    br_texture texture;
    br_mat3    matrix;
} br_fill_pattern;

/* How a shape (or a stroke) is painted. color multiplies the gradient/pattern (white = as is). */
typedef struct br_fill_style {
    br_graphics_fill_kind kind;
    br_color              color;      /* straight alpha; alpha = opacity */
    br_fill_gradient      gradient;
    br_fill_pattern       pattern;
} br_fill_style;

typedef enum br_graphics_cap { BR_GRAPHICS_CAP_BUTT, BR_GRAPHICS_CAP_ROUND, BR_GRAPHICS_CAP_SQUARE } br_graphics_cap;
typedef enum br_graphics_join { BR_GRAPHICS_JOIN_MITER, BR_GRAPHICS_JOIN_BEVEL, BR_GRAPHICS_JOIN_ROUND } br_graphics_join;

/* PixiJS StrokeStyle. Make one with br_graphics_stroke_style and change fields. */
typedef struct br_stroke_style {
    br_fill_style    fill;          /* colour / gradient / pattern of the stroke */
    float            width;
    float            alignment;     /* 0 = inside, 0.5 = centred, 1 = outside (closed shapes) */
    br_graphics_cap  cap;           /* open paths and dashes */
    br_graphics_join join;
    float            miter_limit;   /* miter length / half width above which a miter becomes a bevel; <= 0 = 10 */
    bool             pixel_line;    /* always exactly 1 pixel wide, any scale (width, cap, join ignored) */
    uint32_t         dash_count;    /* 0 = solid; on, off, on, ... lengths (odd counts repeat twice, as SVG) */
    float            dash[BR_GRAPHICS_MAX_DASHES];
    float            dash_offset;   /* distance into the pattern where the stroke starts */
} br_stroke_style;

/* A plain colour fill. */
br_fill_style br_graphics_style_color(br_color color);
/* A linear gradient from (x0,y0) to (x1,y1) in `space`; add stops with br_graphics_style_add_stop. */
br_fill_style br_graphics_style_linear(br_graphics_space space, float x0, float y0, float x1, float y1);
/* A concentric radial gradient in `space`. */
br_fill_style br_graphics_style_radial(br_graphics_space space, float cx, float cy, float inner_radius,
                                       float outer_radius);
/* A repeated texture; matrix maps texture pixels to context space. */
br_fill_style br_graphics_style_pattern(br_texture texture, br_mat3 matrix);
/* Adds a colour stop to a gradient style (ignored when full). */
void br_graphics_style_add_stop(br_fill_style *style, float offset, br_color color);
/* Default stroke: centred, butt caps, miter joins, limit 10, solid. */
br_stroke_style br_graphics_stroke_style(float width, br_color color);

/* ---- contexts (PixiJS GraphicsContext) */

/* Creates an empty context with one reference (yours). */
br_graphics_context *br_graphics_context_create(br_engine *e);
/* Drops your reference; the context is freed when no graphics node uses it either. */
void br_graphics_context_destroy(br_graphics_context *ctx);
/* Removes every instruction and the current path; resets the transform stack. */
void br_graphics_clear(br_graphics_context *ctx);

/* ---- path building (PixiJS GraphicsPath) */

/* Starts a new sub-path at (x, y). */
void br_graphics_move_to(br_graphics_context *ctx, float x, float y);
/* Straight line to (x, y). After close_path it starts at the closed sub-path's start (canvas,
 * SVG "Z L"); with no open sub-path otherwise it acts as a move_to. */
void br_graphics_line_to(br_graphics_context *ctx, float x, float y);
/* Cubic Bezier from the current point. */
void br_graphics_bezier_curve_to(br_graphics_context *ctx, float cp1x, float cp1y, float cp2x, float cp2y,
                                 float x, float y);
/* Quadratic Bezier from the current point. */
void br_graphics_quadratic_curve_to(br_graphics_context *ctx, float cpx, float cpy, float x, float y);
/* Circular arc (canvas arc): lines to its start, then sweeps clockwise unless counterclockwise. */
void br_graphics_arc(br_graphics_context *ctx, float cx, float cy, float radius, float start_angle, float end_angle,
                     bool counterclockwise);
/* Canvas arcTo: rounds the corner current point -> (x1,y1) -> (x2,y2) with `radius`. */
void br_graphics_arc_to(br_graphics_context *ctx, float x1, float y1, float x2, float y2, float radius);
/* SVG elliptical arc from the current point to (x, y); x_axis_rotation in radians. */
void br_graphics_arc_to_svg(br_graphics_context *ctx, float rx, float ry, float x_axis_rotation, bool large_arc,
                            bool sweep, float x, float y);
/* Closes the current sub-path (line back to its start). */
void br_graphics_close_path(br_graphics_context *ctx);
/* Appends SVG path data ("M10 10 L 90 10 A 5 5 0 0 1 ... Z"; all commands, relative and
 * absolute). False (logged) on a syntax error; what parsed before the error is kept. */
bool br_graphics_svg_path(br_graphics_context *ctx, const char *d);

/* ---- shapes: each adds one closed sub-path */

/* Axis-aligned rectangle. */
void br_graphics_rect(br_graphics_context *ctx, float x, float y, float w, float h);
/* Rectangle with circular corners. */
void br_graphics_round_rect(br_graphics_context *ctx, float x, float y, float w, float h, float radius);
/* Circle. */
void br_graphics_circle(br_graphics_context *ctx, float x, float y, float radius);
/* Ellipse centred at (x, y). */
void br_graphics_ellipse(br_graphics_context *ctx, float x, float y, float radius_x, float radius_y);
/* Polygon through points; close = false leaves it open (for strokes). */
void br_graphics_poly(br_graphics_context *ctx, const br_vec2 *points, uint32_t count, bool close);
/* Regular polygon, first vertex straight up (then turned by rotation). sides is capped at 4096. */
void br_graphics_regular_poly(br_graphics_context *ctx, float x, float y, float radius, uint32_t sides,
                              float rotation);
/* Regular polygon with rounded corners. */
void br_graphics_round_poly(br_graphics_context *ctx, float x, float y, float radius, uint32_t sides,
                            float corner_radius, float rotation);
/* Star with `points` tips (capped at 4096); inner_radius <= 0 means radius / 2. */
void br_graphics_star(br_graphics_context *ctx, float x, float y, uint32_t points, float radius, float inner_radius,
                      float rotation);
/* Rectangle with corners cut off at 45 degrees. */
void br_graphics_chamfer_rect(br_graphics_context *ctx, float x, float y, float w, float h, float chamfer);
/* Rectangle with rounded (fillet > 0) or scooped-in (fillet < 0) corners. */
void br_graphics_fillet_rect(br_graphics_context *ctx, float x, float y, float w, float h, float fillet);

/* ---- painting */

/* Fills the current path with a style. */
void br_graphics_fill(br_graphics_context *ctx, const br_fill_style *style);
/* Fills the current path with a colour. */
void br_graphics_fill_color(br_graphics_context *ctx, br_color color);
/* Strokes the current path. */
void br_graphics_stroke(br_graphics_context *ctx, const br_stroke_style *style);
/* Uses the current path as holes in the previous fill (and its stroke when both came from one path). */
void br_graphics_cut(br_graphics_context *ctx);
/* Draws a texture (frame aware) into the rect x,y,w,h (w or h <= 0 = texture size), tinted. */
void br_graphics_texture(br_graphics_context *ctx, br_texture texture, br_color tint, float x, float y, float w,
                         float h);

/* ---- transform stack (applies to points added afterwards) */

/* Pushes the current transform. */
void br_graphics_save(br_graphics_context *ctx);
/* Pops the transform pushed by the matching save. */
void br_graphics_restore(br_graphics_context *ctx);
/* Appends a translation. */
void br_graphics_translate(br_graphics_context *ctx, float x, float y);
/* Appends a rotation (radians). */
void br_graphics_rotate(br_graphics_context *ctx, float radians);
/* Appends a scale. */
void br_graphics_scale(br_graphics_context *ctx, float sx, float sy);
/* Replaces the current transform. */
void br_graphics_set_transform(br_graphics_context *ctx, br_mat3 m);

/* ---- geometry queries (triangulates if needed) */

typedef struct br_graphics_geometry_info {
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t line_count;      /* pixel-line segments */
    float    area;            /* sum of triangle areas (overlaps counted twice) */
    br_rect  bounds;          /* zero rect when empty */
} br_graphics_geometry_info;
/* Fills *out with the context's triangulated geometry numbers. */
void br_graphics_context_get_geometry(br_graphics_context *ctx, br_graphics_geometry_info *out);
/* True if local point p is inside any filled or stroked triangle (holes excluded). */
bool br_graphics_context_contains(br_graphics_context *ctx, br_vec2 p);

/* ---- the "graphics" node type (PixiJS Graphics) */

/* Creates a graphics node drawing ctx (shared; NULL = a new context owned by the node). */
br_node br_graphics_create(br_engine *e, br_graphics_context *ctx);
/* The node's context (draw into it directly), NULL for non-graphics nodes. */
br_graphics_context *br_graphics_context_of(br_engine *e, br_node graphics);
/* Points the node at another context (NULL = a fresh owned one). */
void br_graphics_set_context(br_engine *e, br_node graphics, br_graphics_context *ctx);
/* Node type id of "graphics". */
uint32_t br_graphics_type(br_engine *e);

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_graphics_register(br_engine *e);

#endif
