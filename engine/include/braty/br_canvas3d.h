/* br_canvas3d.h - 2D content in the 3D world (Phase 2 Task 15).
 *
 * A canvas3d is a 3D node (type "canvas3d") that hosts a 2D subtree: br_canvas3d_root gives a
 * detached 2D container; anything you put under it (sprites, nine-slice, graphics, text, UI with
 * pointer listeners) is drawn on the canvas plane in the 3D scene.
 *
 *   - Mode GEOMETRY (default): the subtree's batched triangles are replayed inside the 3D pass
 *     with the canvas' world matrix, depth tested, so they stay crisp at any distance (use an
 *     SDF font for text: br_text3d_create does) and sort with the other transparent items.
 *     Filters, alpha/colour masks and stencil/scissor masks are not applied in this mode.
 *   - Mode TEXTURE: the subtree is rendered into a render texture each frame (filters and masks
 *     work there; put them on children of the root) and that texture is drawn on the plane.
 *
 * Plane: 2D x right / y down maps to local +X / -Y; the canvas faces +Z. pixels_per_unit 2D
 * units make one world unit; the 2D point (pivot.x * width, pivot.y * height) sits on the node.
 * Billboards turn the plane toward the camera of the view being drawn (full or about world Y).
 * Lit canvases take the view's ambient and lights (no shadows); unlit ones show their colours.
 * Colours are sRGB like all 2D content and converted to linear for the 3D pass.
 *
 * Pointer events: a click on the canvas (3D picking through the View3D) is mapped to the 2D
 * point on the plane, hit-tested against the subtree and dispatched as a normal 2D event, so
 * br_node_on listeners on a button inside a canvas work (DOWN, UP, MOVE, OVER, OUT, CLICK, TAP).
 */
#ifndef BR_CANVAS3D_H
#define BR_CANVAS3D_H

#include "braty/br_app.h"
#include "braty/br_sprites.h"
#include "braty/br_text.h"
#include "braty/br_view3d.h"

typedef enum br_canvas3d_mode {
    BR_CANVAS3D_GEOMETRY,
    BR_CANVAS3D_TEXTURE
} br_canvas3d_mode;

typedef enum br_canvas3d_billboard {
    BR_CANVAS3D_BILLBOARD_NONE,
    BR_CANVAS3D_BILLBOARD_FULL,     /* faces the camera (rotates with it) */
    BR_CANVAS3D_BILLBOARD_Y_AXIS    /* turns about world Y only (signs, trees) */
} br_canvas3d_billboard;

/* Canvas settings (edit in place through br_canvas3d_get). */
typedef struct br_canvas3d_desc {
    float                 width;            /* 2D units of the content (picking, texture size) */
    float                 height;
    float                 pixels_per_unit;  /* 2D units per world unit (default 100) */
    br_vec2               pivot;            /* 0..1 of width/height placed at the node (default 0.5, 0.5) */
    br_canvas3d_mode      mode;
    br_canvas3d_billboard billboard;
    bool                  lit;              /* scene ambient + lights (default false) */
    bool                  double_sided;     /* default true; false hides the back */
    float                 alpha_cutoff;     /* > 0: alpha-tested in the opaque pass (depth write) */
    br_color              tint;             /* sRGB, straight alpha; multiplies the content */
    float                 resolution;       /* texture pixels per 2D unit (texture mode, default 2) */
} br_canvas3d_desc;

/* Defaults for content of width x height 2D units. */
br_canvas3d_desc br_canvas3d_default(float width, float height);

/* Registers the module (node type, systems). Called once by br_builtin.c. */
void br_canvas3d_register(br_engine *e);
/* Type id of "canvas3d". */
uint32_t br_canvas3d_type(br_engine *e);
/* Creates a detached canvas node (desc NULL = br_canvas3d_default(100, 100)). */
br_node3d br_canvas3d_create(br_engine *e, const br_canvas3d_desc *desc);
/* The settings, NULL if n is not a live canvas. Stable until n is destroyed. */
br_canvas3d_desc *br_canvas3d_get(br_engine *e, br_node3d n);
/* The 2D container whose subtree the canvas shows (detached from the stage). */
br_node br_canvas3d_root(br_engine *e, br_node3d n);
/* The first child of the root (the content of the convenience creators below). */
br_node br_canvas3d_content(br_engine *e, br_node3d n);

/* A canvas showing one sprite of the texture, centred on the node. */
br_node3d br_sprite3d_create(br_engine *e, br_texture texture, float pixels_per_unit);
/* A canvas showing text, centred. style->font NULL = the default font as an SDF font (sharp
 * at any distance). The canvas is sized to the text. */
br_node3d br_text3d_create(br_engine *e, const char *utf8, const br_text_style *style, float pixels_per_unit);
/* A canvas showing a nine-slice of width x height 2D units, centred. */
br_node3d br_nine_slice3d_create(br_engine *e, br_texture texture, float left, float top, float right, float bottom,
                                 float width, float height, float pixels_per_unit);
/* The engine's SDF version of the default font (created on first use, freed at shutdown). */
br_font *br_canvas3d_sdf_font(br_engine *e);

/* ---- pure maths (tests) */

/* Matrix from the canvas' 2D space (root coordinates) to its local 3D plane. */
br_mat4 br_canvas3d_plane_matrix(const br_canvas3d_desc *desc);
/* Intersects a world ray with a canvas whose plane-space world matrix is `world`. On a hit
 * inside the content rect, writes the 2D point (root coordinates) and the ray parameter. */
bool br_canvas3d_ray_to_local(const br_canvas3d_desc *desc, br_mat4 world, br_ray3d ray, br_vec2 *local, float *t);
/* World matrix of a billboarded canvas: node world matrix + the camera's world matrix. */
br_mat4 br_canvas3d_billboard_matrix(br_canvas3d_billboard mode, br_mat4 node_world, br_mat4 camera_world);

#endif
