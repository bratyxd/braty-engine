/* br_view3d.h - 3D scenes: the 3D node table, 3D node types, draw submission, View3D, picking.
 *
 * The 3D side mirrors the 2D scene graph:
 *   - 3D nodes (br_node3d, handle = index + generation) live in one struct-of-arrays table per
 *     engine: hierarchy, label, position / rotation (quaternion) / scale, world br_mat4,
 *     visible, layer bits. Stale handles are ignored. br_node3d_destroy is deferred inside a
 *     frame (memory freed at the start of the next frame), immediate outside one.
 *   - Every 3D node has a type (br_node3d_type_desc): name, per-node data in a dense pool,
 *     on_create / on_destroy, local bounds, `collect` (submits draw items for a view) and
 *     `pick` (ray test). mesh3d, batched_mesh3d, light3d and camera3d are registered through
 *     this same registry; other modules add their own types the same way.
 *   - A View3D is a 2D node (type "view3d") that renders the 3D subtree under its root node
 *     with its camera into an HDR target, tone-maps it into a texture and draws that texture as
 *     its quad. 2D filters, masks, alpha and blend modes therefore apply to the whole view.
 *
 * Conventions: right-handed, y up, cameras and lights look down their local -Z. Depth is
 * standard: 0 at the near plane, 1 at the far plane, LESS_OR_EQUAL test, cleared to 1.
 * Colours given to 3D materials and lights are LINEAR (glTF factors are linear).
 *
 * Frame flow: the view's 2D render callback (PRE_RENDER collect) updates world matrices,
 * walks the subtree calling each type's `collect`, culls, sorts (opaque front to back,
 * transparent back to front) and pushes ONE own-passes render callback that runs in RENDER:
 * shadow pass, main HDR pass (opaque, then transparent), bloom, custom post passes, tone map.
 */
#ifndef BR_VIEW3D_H
#define BR_VIEW3D_H

#include "braty/br_events.h"
#include "braty/br_render.h"

/* ---- handles and small 3D types */

typedef struct br_node3d     { uint32_t index; uint32_t generation; } br_node3d;
typedef struct br_geometry3d { uint32_t index; uint32_t generation; } br_geometry3d;
typedef struct br_material3d { uint32_t index; uint32_t generation; } br_material3d;

/* br_aabb and br_ray3d are in br_math.h (with their intersection tests). */

/* Opaque context handed to 3D node types' collect callbacks. */
typedef struct br_render3d_ctx br_render3d_ctx;

/* Result of a pick. Types fill distance/point/normal in the ray's space; the view converts. */
typedef struct br_pick3d_hit {
    br_node3d node;
    float     distance;     /* ray parameter: world units when the ray direction is unit length */
    br_vec3   point;
    br_vec3   normal;
    uint32_t  instance;     /* batched meshes: the instance hit; 0 otherwise */
    uint32_t  triangle;     /* first index of the triangle hit / 3 */
} br_pick3d_hit;

/* Registers the module (node types, systems). Called once by br_builtin.c. */
void br_three_d_register(br_engine *e);

/* ---- 3D node types */

/* Type id of plain transform nodes (no data). Type id 0 is never valid. */
#define BR_NODE3D_TYPE_GROUP 1u

typedef struct br_node3d_type_desc {
    const char *name;       /* "mesh3d", "light3d", ... (copied) */
    size_t      data_size;  /* per-node data in a dense pool (0 = none) */
    void (*on_create)(br_engine *e, br_node3d n, void *data);
    void (*on_destroy)(br_engine *e, br_node3d n, void *data);
    /* Bounds in the node's local space (culling, scene bounds for shadows). */
    bool (*local_bounds)(br_engine *e, br_node3d n, const void *data, br_aabb *out);
    /* Called once per view per frame for visible nodes: submit draw items with
     * br_render3d_submit / br_render3d_submit_custom. */
    void (*collect)(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx);
    /* Ray test in the node's LOCAL space (ray direction not normalised, so t is the world t). */
    bool (*pick)(br_engine *e, br_node3d n, const void *data, br_ray3d local_ray, br_pick3d_hit *hit);
    bool (*from_json)(br_engine *e, br_node3d n, void *data, const cJSON *json);
} br_node3d_type_desc;

/* Registers a 3D node type (any callback may be NULL). Returns its id, 0 on failure. */
uint32_t br_node3d_type_register(br_engine *e, const br_node3d_type_desc *desc);
/* Finds a 3D type id by name, 0 if unknown. */
uint32_t br_node3d_type_find(br_engine *e, const char *name);
/* The descriptor of a 3D type, NULL if unknown. */
const br_node3d_type_desc *br_node3d_type_get(br_engine *e, uint32_t type_id);

/* ---- 3D nodes: lifetime and hierarchy */

/* Creates a detached 3D node (zeroed type data, then on_create). Zero handle on failure. */
br_node3d br_node3d_create(br_engine *e, uint32_t type_id);
/* Shortcut for br_node3d_create(e, BR_NODE3D_TYPE_GROUP). */
br_node3d br_node3d_create_group(br_engine *e);
/* Per-type data, NULL if stale or of another type. Pointers die when that type's pool grows. */
void *br_node3d_data(br_engine *e, br_node3d n, uint32_t type_id);
/* Type id of a node, 0 if invalid. */
uint32_t br_node3d_type_of(br_engine *e, br_node3d n);
/* True if the handle refers to a live node. */
bool br_node3d_valid(br_engine *e, br_node3d n);
/* Destroys n and its subtree. Deferred inside a frame (handles invalid at once). */
void br_node3d_destroy(br_engine *e, br_node3d n);
/* Number of live 3D nodes. */
uint32_t br_node3d_count(br_engine *e);
/* Appends child to parent (detaching it first). False on invalid handles or cycles. */
bool br_node3d_add_child(br_engine *e, br_node3d parent, br_node3d child);
/* Detaches n from its parent (n stays alive). */
void br_node3d_remove_from_parent(br_engine *e, br_node3d n);
/* The parent (zero handle if none). */
br_node3d br_node3d_parent(br_engine *e, br_node3d n);
/* First child (zero handle if none). Iterate with br_node3d_next_sibling. */
br_node3d br_node3d_first_child(br_engine *e, br_node3d n);
/* Next sibling (zero handle at the end). */
br_node3d br_node3d_next_sibling(br_engine *e, br_node3d n);
/* Number of children. */
uint32_t br_node3d_child_count(br_engine *e, br_node3d n);
/* Sets the label (copied; NULL clears). */
void br_node3d_set_label(br_engine *e, br_node3d n, const char *label);
/* The label, "" if none. */
const char *br_node3d_label(br_engine *e, br_node3d n);
/* First descendant of root (root included) with this label, depth-first. Zero handle if none. */
br_node3d br_node3d_find_by_label(br_engine *e, br_node3d root, const char *label);

/* ---- 3D nodes: transform and flags */

/* Sets the position in the parent's space. */
void br_node3d_set_position(br_engine *e, br_node3d n, br_vec3 position);
/* The position. */
br_vec3 br_node3d_position(br_engine *e, br_node3d n);
/* Sets the rotation (normalised here). */
void br_node3d_set_rotation(br_engine *e, br_node3d n, br_quat rotation);
/* The rotation. */
br_quat br_node3d_rotation(br_engine *e, br_node3d n);
/* Sets the rotation from Euler angles in radians (X, then Y, then Z). */
void br_node3d_set_euler(br_engine *e, br_node3d n, float x, float y, float z);
/* Sets the scale (default 1,1,1). */
void br_node3d_set_scale(br_engine *e, br_node3d n, br_vec3 scale);
/* The scale. */
br_vec3 br_node3d_scale(br_engine *e, br_node3d n);
/* Rotates n so its -Z axis points at a world-space target (cameras, lights). */
void br_node3d_look_at(br_engine *e, br_node3d n, br_vec3 target, br_vec3 up);
/* Local matrix (translation * rotation * scale). */
br_mat4 br_node3d_local_matrix(br_engine *e, br_node3d n);
/* World matrix computed from the ancestors now (always current). */
br_mat4 br_node3d_world_matrix(br_engine *e, br_node3d n);
/* World-space position (translation of the world matrix). */
br_vec3 br_node3d_world_position(br_engine *e, br_node3d n);
/* Hides/shows n and its subtree. */
void br_node3d_set_visible(br_engine *e, br_node3d n, bool visible);
/* True if visible. */
bool br_node3d_visible(br_engine *e, br_node3d n);
/* Layer bits (default 1). A view draws a node only if (layer & view culling mask) != 0. */
void br_node3d_set_layer(br_engine *e, br_node3d n, uint32_t layer);
/* The layer bits. */
uint32_t br_node3d_layer(br_engine *e, br_node3d n);
/* Whether picking considers n (default true). */
void br_node3d_set_pickable(br_engine *e, br_node3d n, bool pickable);
/* True if pickable. */
bool br_node3d_pickable(br_engine *e, br_node3d n);
/* Local bounds from the type (false if the type has none). */
bool br_node3d_local_bounds(br_engine *e, br_node3d n, br_aabb *out);

/* Called for every 3D node when its memory is freed (handle already invalid). */
typedef void (*br_node3d_destroy_fn)(br_engine *e, br_node3d n, void *user);
/* Adds a callback run for every destroyed 3D node (per-node side tables of other modules). */
void br_node3d_on_destroy(br_engine *e, br_node3d_destroy_fn fn, void *user);

/* ---- draw submission (inside a type's collect callback) */

/* Passes a draw item takes part in. */
typedef enum br_pass3d {
    BR_PASS3D_SHADOW      = 1,   /* depth into the shadow maps */
    BR_PASS3D_OPAQUE      = 2,   /* main pass, sorted front to back, depth write */
    BR_PASS3D_TRANSPARENT = 4    /* main pass after opaque, sorted back to front, no depth write */
} br_pass3d;

/* Per-instance data (vertex buffer slot 1, per instance). color multiplies the base colour. */
typedef struct br_instance3d {
    br_mat4  model;   /* world matrix of the instance */
    br_color color;   /* linear, straight alpha */
} br_instance3d;

/* A mesh draw: geometry + material + instances. Instances and joints are copied at submit. */
typedef struct br_draw3d {
    br_geometry3d        geometry;
    br_material3d        material;
    const br_instance3d *instances;
    uint32_t             instance_count;
    br_aabb              world_bounds;     /* of all instances: culling and sorting */
    bool                 cast_shadows;
    const br_mat4       *joint_matrices;   /* skinning: NULL = not skinned */
    uint32_t             joint_count;
    const float         *morph_weights;    /* morph targets of the geometry: NULL = none */
    uint32_t             morph_weight_count;
} br_draw3d;
/* Submits a mesh draw. The material's alpha mode picks opaque or transparent; cast_shadows
 * adds the shadow pass. Items outside the camera frustum skip the main passes. */
void br_render3d_submit(br_render3d_ctx *ctx, const br_draw3d *draw);

/* What a custom draw sees when it runs (RENDER phase, inside the pass). */
typedef struct br_draw3d_custom_info {
    SDL_GPUCommandBuffer *cmd;
    SDL_GPURenderPass    *pass;
    br_pass3d             pass_kind;      /* which pass is running */
    br_mat4               view;           /* camera view (shadow pass: the light's view) */
    br_mat4               projection;
    br_mat4               view_projection;
    br_vec3               camera_position;
    SDL_GPUTextureFormat  color_format;
    SDL_GPUTextureFormat  depth_format;
    SDL_GPUSampleCount    sample_count;
} br_draw3d_custom_info;
typedef void (*br_draw3d_custom_fn)(br_engine *e, const br_draw3d_custom_info *info, void *user);
/* Submits a custom draw into the passes in `passes` (br_pass3d bits). sort_point (world) orders
 * it among the other items. `user` must stay valid until the RENDER phase of this frame ends;
 * allocate transient GPU data here (collect), never inside fn. */
void br_render3d_submit_custom(br_render3d_ctx *ctx, uint32_t passes, br_vec3 sort_point,
                               br_draw3d_custom_fn fn, void *user);
/* Fills formats, sample count, depth test/write and blend of a pipeline for the running pass
 * (opaque: no blend + depth write; transparent: premultiplied blend, no depth write;
 * shadow: writes depth into an R32F colour target). */
void br_render3d_pipeline_setup(const br_draw3d_custom_info *info, br_pipeline_desc *desc);

/* World matrix of a node as of this frame's update (valid during collect). */
br_mat4 br_render3d_world(br_render3d_ctx *ctx, br_node3d n);
/* Camera view-projection of the view being collected. */
br_mat4 br_render3d_view_projection(br_render3d_ctx *ctx);
/* Camera world position of the view being collected. */
br_vec3 br_render3d_camera_position(br_render3d_ctx *ctx);
/* True if a world box is inside the camera frustum. */
bool br_render3d_visible(br_render3d_ctx *ctx, br_aabb world_bounds);
/* The 2D view node being collected. */
br_node br_render3d_view(br_render3d_ctx *ctx);

/* ---- View3D (a 2D node) */

/* Creates a view of width x height 2D units (a detached 2D node, add it to the stage). It
 * owns a root 3D node and a default perspective camera (60 degrees, under the root). Its
 * event mode is STATIC: it is hit only where a 3D node with 3D listeners is under the pointer. */
br_node br_view3d_create(br_engine *e, float width, float height);
/* Type id of "view3d". */
uint32_t br_view3d_type(br_engine *e);
/* The root 3D node: everything under it is drawn. */
br_node3d br_view3d_root(br_engine *e, br_node view);
/* Uses a camera3d node (anywhere in the table, usually under the root). */
void br_view3d_set_camera(br_engine *e, br_node view, br_node3d camera);
/* The camera node. */
br_node3d br_view3d_camera(br_engine *e, br_node view);
/* Sets the size in 2D units (targets are recreated at the next frame). */
void br_view3d_set_size(br_engine *e, br_node view, float width, float height);
/* The size in 2D units. */
br_vec2 br_view3d_size(br_engine *e, br_node view);
/* When on, the size follows the window size every frame. */
void br_view3d_set_auto_resize(br_engine *e, br_node view, bool auto_resize);
/* Target pixels per 2D unit (0 = window pixel density, the default). */
void br_view3d_set_resolution(br_engine *e, br_node view, float resolution);
/* MSAA sample count for the HDR pass: 1 (default), 2, 4 or 8 (clamped to what the GPU supports). */
void br_view3d_set_msaa(br_engine *e, br_node view, uint32_t samples);
/* Background (linear). Alpha 0 shows the 2D content under the view. */
void br_view3d_set_background(br_engine *e, br_node view, br_color color);
/* Hemisphere ambient light: sky colour from above, ground colour from below (linear). */
void br_view3d_set_environment(br_engine *e, br_node view, br_color sky, br_color ground, float intensity);
/* Only nodes with (layer & mask) != 0 are drawn (default all bits). */
void br_view3d_set_culling_mask(br_engine *e, br_node view, uint32_t mask);
/* Shadow map tile size in pixels (default 1024; atlas = 4 x 2 tiles). */
void br_view3d_set_shadow_resolution(br_engine *e, br_node view, uint32_t tile_size);
/* The tone-mapped output (a render texture, usable as a sprite or material texture elsewhere). */
br_texture br_view3d_texture(br_engine *e, br_node view);

/* ---- screen <-> world, picking, 3D pointer events */

/* World ray through a stage (global 2D) point, starting on the near plane. Direction is unit length. */
br_ray3d br_view3d_screen_to_ray(br_engine *e, br_node view, br_vec2 global_point);
/* Projects a world point to stage (global 2D) coordinates. False if behind the camera. */
bool br_view3d_world_to_screen(br_engine *e, br_node view, br_vec3 world, br_vec2 *global_out);
/* Closest pickable node under a stage point: ray vs AABB, then triangles. False if none. */
bool br_view3d_pick(br_engine *e, br_node view, br_vec2 global_point, br_pick3d_hit *hit);
/* Closest pickable node under root hit by a world ray. */
bool br_view3d_raycast(br_engine *e, br_node3d root, br_ray3d ray, br_pick3d_hit *hit);

/* A pointer event delivered to a 3D node (from the view's 2D event). */
typedef struct br_event3d {
    br_event     *source;          /* the 2D event the view received (type, button, global...) */
    br_node       view;
    br_node3d     target;          /* the node hit */
    br_node3d     current_target;  /* the node whose listener runs (bubbles up 3D parents) */
    br_pick3d_hit hit;             /* world-space hit (zero for OUT) */
    bool          propagation_stopped;
} br_event3d;
typedef void (*br_event3d_fn)(br_engine *e, br_event3d *ev, void *user);
/* Listens for a pointer event type on a 3D node. Handled types: DOWN, UP, UP_OUTSIDE (to the
 * node pressed), MOVE, OVER, OUT (per 3D node, from moves), CLICK, RIGHT_CLICK, TAP, WHEEL. */
void br_node3d_on(br_engine *e, br_node3d n, br_event_type type, br_event3d_fn fn, void *user);
/* Removes listeners matching (type, fn, user). */
void br_node3d_off(br_engine *e, br_node3d n, br_event_type type, br_event3d_fn fn, void *user);

/* ---- stats */

typedef struct br_view3d_stats {
    uint32_t items_opaque;
    uint32_t items_transparent;
    uint32_t items_shadow;
    uint32_t instances;      /* instances drawn in the main passes */
    uint32_t draw_calls;     /* main + shadow passes */
    uint32_t lights;
    uint32_t shadow_tiles;
} br_view3d_stats;
/* Numbers of the view's last collected frame. */
void br_view3d_get_stats(br_engine *e, br_node view, br_view3d_stats *out);

#endif
