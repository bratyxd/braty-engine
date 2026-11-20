/* br_graphics_internal.h - graphics context data and the geometry builder (engine-private). */
#ifndef BR_GRAPHICS_INTERNAL_H
#define BR_GRAPHICS_INTERNAL_H

#include "braty/br_graphics.h"

#include <SDL3/SDL.h>

/* One sub-path: points[first .. first + count) of its path. */
typedef struct br_gfx_subpath {
    uint32_t first;
    uint32_t count;
    bool     closed;
    bool     convex;   /* built by a convex shape builder: fan-triangulated, no ear clipping */
} br_gfx_subpath;

/* Flattened path: points (br_vec2, context space) and sub-paths (br_gfx_subpath). */
typedef struct br_gfx_path {
    br_array points;
    br_array subpaths;
} br_gfx_path;

typedef enum br_gfx_kind { BR_GFX_FILL, BR_GFX_STROKE, BR_GFX_TEXTURE } br_gfx_kind;

typedef struct br_gfx_instruction {
    br_gfx_kind     kind;
    uint32_t        path_id;   /* instructions made from the same path share this */
    br_gfx_path     path;      /* owned copy */
    br_gfx_path     hole;      /* owned, empty unless cut() was used */
    br_stroke_style style;     /* fills use style.fill */
    br_texture      texture;   /* white, baked gradient, pattern texture, or the TEXTURE image */
    br_vec2         quad[4];   /* TEXTURE: corners top-left, top-right, bottom-right, bottom-left */
} br_gfx_instruction;

/* Consecutive triangles sharing one texture (at most 65535 vertices), or consecutive pixel
 * lines (line_count > 0). Groups are drawn in instruction order. */
typedef struct br_gfx_group {
    br_texture texture;
    uint32_t   first_vertex;
    uint32_t   vertex_count;
    uint32_t   first_index;
    uint32_t   index_count;   /* indices are relative to first_vertex */
    uint32_t   first_line;    /* pixel lines: vertices [first_line, + line_count) of `lines` */
    uint32_t   line_count;
} br_gfx_group;

struct br_graphics_context {
    br_engine  *e;
    uint32_t    refs;            /* creator + every node using it */
    br_array    instructions;    /* br_gfx_instruction */
    br_gfx_path path;            /* the path being built */
    uint32_t    path_id;
    bool        path_used;       /* filled/stroked: the next path call starts a new path */
    bool        subpath_open;    /* the last sub-path of `path` takes line_to points */
    br_vec2     cursor;          /* current point, untransformed */
    br_vec2     subpath_start;   /* untransformed start of the open sub-path */
    br_mat3     transform;
    br_array    stack;           /* br_mat3 */
    br_array    owned_textures;  /* br_texture: baked gradients */
    uint32_t    version;         /* bumped by every change */
    uint32_t    built_version;   /* version the cache below was built from */
    /* cache, context space */
    br_array    vertices;        /* br_vertex2d, colours premultiplied */
    br_array    indices;         /* uint16_t */
    br_array    groups;          /* br_gfx_group */
    br_array    lines;           /* br_vertex2d pairs (pixel lines) */
    br_rect     bounds;
    bool        has_bounds;
    bool        bad_point_logged;   /* a non-finite / huge point was dropped (logged once) */
    bool        full_logged;        /* a sub-path hit BR_GFX_MAX_SUBPATH_POINTS (logged once) */
    bool        after_close;        /* close_path ran: line_to continues from the sub-path start */
};

typedef struct br_graphics_state {
    uint32_t type;
    br_array contexts;   /* br_graphics_context*, every live context (freed at shutdown) */
    br_array scratch;    /* br_vertex2d, per-node transformed vertices */
} br_graphics_state;

/* Limits that keep bad data (scene JSON, SVG strings) from hanging or exhausting memory.
 * Points outside +-BR_GFX_MAX_COORD or not finite are dropped where they enter (add_point). */
#define BR_GFX_MAX_COORD 1.0e7f
#define BR_GFX_MAX_SUBPATH_POINTS 65536u   /* points per sub-path; more are dropped */
#define BR_GFX_MAX_SIDES 4096u             /* regular_poly / round_poly sides, star points */
#define BR_GFX_MAX_EAR_CLIP_POINTS 8192u   /* concave fills (ear clipping is about O(n^2)) */
#define BR_GFX_MAX_DASH_PIECES 100000u     /* dash pieces per stroke */

/* True for a finite float (false for NaN and +-inf). */
bool br_gfx_finite(float v);
/* Parses one SVG-grammar number at *s (leading separators skipped): sign, digits, '.',
 * digits, exponent. No nan / inf / hex, independent of the C locale. Advances *s. */
bool br_gfx_parse_number(const char **s, float *out);

/* Path helpers. */
void br_gfx_path_init(br_gfx_path *p);
void br_gfx_path_free(br_gfx_path *p);
void br_gfx_path_copy(br_gfx_path *dst, const br_gfx_path *src);
void br_gfx_path_append(br_gfx_path *dst, const br_gfx_path *src);

/* Segments for an arc of `radius` sweeping `sweep` radians (chord error <= 0.25 units). */
uint32_t br_gfx_arc_segments(float radius, float sweep);
/* Triangulates one polygon with holes (ear clipping). Appends positions (br_vec2) to
 * out_points and triangle indices (uint32_t, relative to the first appended point) to
 * out_indices. Holes may have any winding. */
void br_gfx_triangulate(const br_vec2 *outer, uint32_t outer_count, const br_gfx_path *holes, br_array *out_points,
                        br_array *out_indices);
/* Builds stroke triangles for one polyline (same output convention as br_gfx_triangulate). */
void br_gfx_stroke_polyline(const br_vec2 *points, uint32_t count, bool closed, const br_stroke_style *style,
                            br_array *out_points, br_array *out_indices);
/* Splits a polyline into dash pieces (open sub-paths appended to out). */
void br_gfx_dash_polyline(const br_vec2 *points, uint32_t count, bool closed, const br_stroke_style *style,
                          br_gfx_path *out);
/* Rebuilds the geometry cache if the context changed since the last build. */
void br_gfx_build(br_graphics_context *ctx);

#endif
