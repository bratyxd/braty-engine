/* br_node.h - the scene graph: nodes (PixiJS Container), node types, user components.
 *
 * A node is a handle { index, generation } into the engine's node table. Every node has the
 * Container fields (hierarchy, transform, alpha, tint, visibility, z-index, blend mode, masks,
 * culling, interaction fields) and a type. Per-type data (a sprite's texture and anchor, ...)
 * lives in a dense pool per type; get it with br_node_data(e, n, type_id).
 *
 * Handles are checked on every call: a stale or destroyed handle is ignored (getters return
 * zero/defaults, setters do nothing). br_node_destroy is deferred: the node and its subtree
 * stop being valid immediately, but memory is only freed at the start of the next frame, so
 * systems and callbacks that are iterating stay safe.
 *
 * Only nodes reachable from br_stage_root(e) are updated and drawn every frame.
 * Transforms: x right, y down, radians, positive rotation = clockwise (see br_math.h).
 */
#ifndef BR_NODE_H
#define BR_NODE_H

#include "braty/br_core.h"
#include "braty/br_math.h"
#include "cjson/cJSON.h"

typedef struct br_node { uint32_t index; uint32_t generation; } br_node;

/* Opaque render context handed to node render callbacks (see br_render.h). */
typedef struct br_render_ctx br_render_ctx;

/* Type id of plain containers (no per-type data). Type id 0 is never valid. */
#define BR_NODE_TYPE_CONTAINER 1u

/* ---- node types: one descriptor per kind of scene object, registered at engine init */

typedef struct br_node_type_desc {
    const char *name;                 /* "sprite", "graphics", ... also the JSON "type" (copied) */
    size_t      data_size;            /* per-node data stored in a dense pool (0 = none) */
    void (*on_create)(br_engine *e, br_node n, void *data);
    void (*on_destroy)(br_engine *e, br_node n, void *data);
    bool (*local_bounds)(br_engine *e, br_node n, const void *data, br_rect *out);
    void (*render)(br_engine *e, br_node n, const void *data, br_render_ctx *rc);
    bool (*hit_test)(br_engine *e, br_node n, const void *data, br_vec2 local_point);
    bool (*from_json)(br_engine *e, br_node n, void *data, const cJSON *json);
} br_node_type_desc;

/* Registers a node type (any callback may be NULL). Returns its id, 0 on failure (logged). */
uint32_t br_node_type_register(br_engine *e, const br_node_type_desc *desc);
/* Finds a type id by name, 0 if unknown. */
uint32_t br_node_type_find(br_engine *e, const char *name);
/* The descriptor of a type, NULL if unknown. */
const br_node_type_desc *br_node_type_get(br_engine *e, uint32_t type_id);
/* Number of registered types (ids are 1..count). */
uint32_t br_node_type_count(br_engine *e);
/* Installs/replaces a type's from_json callback (the data module adds JSON for built-in types). */
void br_node_type_set_from_json(br_engine *e, uint32_t type_id,
                                bool (*from_json)(br_engine *e, br_node n, void *data, const cJSON *json));

/* ---- lifetime */

/* Creates a detached node of a type (zeroed type data, then on_create). Zero handle on failure. */
br_node br_node_create(br_engine *e, uint32_t type_id);
/* Shortcut for br_node_create(e, BR_NODE_TYPE_CONTAINER). */
br_node br_node_create_container(br_engine *e);
/* Per-type data, NULL if the handle is stale/destroyed or the node has another type. */
void *br_node_data(br_engine *e, br_node n, uint32_t type_id);
/* Type id of a node, 0 if invalid. */
uint32_t br_node_type_of(br_engine *e, br_node n);
/* True if the handle refers to a live node (not destroyed). */
bool br_node_valid(br_engine *e, br_node n);
/* True if both handles are identical. */
bool br_node_equal(br_node a, br_node b);
/* Destroys the node and all its descendants. Handles turn invalid now. Inside br_engine_step
 * memory is freed by the flush at the start of the next frame; outside a step the flush runs
 * now. A destroy from a destroy callback (on_destroy, br_node_on_destroy) joins the
 * flush that is running, which frees it before it returns: callbacks run once per node. */
void br_node_destroy(br_engine *e, br_node n);
/* The stage: the root node the engine updates and draws every frame. */
br_node br_stage_root(br_engine *e);
/* Number of live nodes (stage included). */
uint32_t br_node_count(br_engine *e);

/* Called for every node when its memory is actually freed (handle already invalid; use
 * n.index to clean per-node tables). */
typedef void (*br_node_destroy_fn)(br_engine *e, br_node n, void *user);
/* Adds a callback run for every destroyed node (modules use it to drop per-node data). */
void br_node_on_destroy(br_engine *e, br_node_destroy_fn fn, void *user);

/* ---- hierarchy (PixiJS Container API) */

/* Appends child to parent (removing it from its old parent first). False on invalid/cyclic. */
bool br_node_add_child(br_engine *e, br_node parent, br_node child);
/* Inserts child at index (clamped to the child count). */
bool br_node_add_child_at(br_engine *e, br_node parent, br_node child, uint32_t index);
/* Removes child from parent (the child stays alive, detached). False if not a child. */
bool br_node_remove_child(br_engine *e, br_node parent, br_node child);
/* Removes the node from its parent, if any. */
void br_node_remove_from_parent(br_engine *e, br_node n);
/* Removes all children (they stay alive, detached). */
void br_node_remove_children(br_engine *e, br_node parent);
/* Moves n under new_parent keeping its world transform (Pixi reparentChild). */
bool br_node_reparent(br_engine *e, br_node n, br_node new_parent);
/* Swaps the positions of two children of the same parent. */
bool br_node_swap_children(br_engine *e, br_node parent, br_node a, br_node b);
/* Index of child within parent, -1 if not a child. */
int br_node_child_index(br_engine *e, br_node parent, br_node child);
/* Moves child to index within its parent (clamped). */
bool br_node_set_child_index(br_engine *e, br_node parent, br_node child, uint32_t index);
/* Number of children. */
uint32_t br_node_child_count(br_engine *e, br_node n);
/* The i-th child (zero handle if out of range). Destroyed children stay listed until the frame ends. */
br_node br_node_child_at(br_engine *e, br_node n, uint32_t i);
/* The parent (zero handle if detached). */
br_node br_node_parent(br_engine *e, br_node n);
/* True if `ancestor` is n or one of n's ancestors. */
bool br_node_is_descendant_of(br_engine *e, br_node n, br_node ancestor);
/* Sets the label (copied; NULL clears). */
void br_node_set_label(br_engine *e, br_node n, const char *label);
/* The label, "" if none. */
const char *br_node_label(br_engine *e, br_node n);
/* First child (or descendant when deep) with this label, depth-first. Zero handle if none. */
br_node br_node_find_by_label(br_engine *e, br_node root, const char *label, bool deep);

/* ---- transform (PixiJS position, scale, rotation, skew, pivot, origin; setters take what the
 * getters return, as br_node3d_*) */

/* Sets the position in the parent's space. */
void br_node_set_position(br_engine *e, br_node n, br_vec2 position);
/* The position. */
br_vec2 br_node_position(br_engine *e, br_node n);
/* Sets the scale (default 1,1). */
void br_node_set_scale(br_engine *e, br_node n, br_vec2 scale);
/* The scale. */
br_vec2 br_node_scale(br_engine *e, br_node n);
/* Sets the rotation in radians (clockwise on screen). */
void br_node_set_rotation(br_engine *e, br_node n, float radians);
/* The rotation in radians. */
float br_node_rotation(br_engine *e, br_node n);
/* Sets the skew in radians. */
void br_node_set_skew(br_engine *e, br_node n, br_vec2 skew);
/* The skew. */
br_vec2 br_node_skew(br_engine *e, br_node n);
/* Sets the pivot: the local point that lands on the position, also the rotation/scale centre. */
void br_node_set_pivot(br_engine *e, br_node n, br_vec2 pivot);
/* The pivot. */
br_vec2 br_node_pivot(br_engine *e, br_node n);
/* Sets the origin: rotation/scale centre that does not move the node (Pixi origin). */
void br_node_set_origin(br_engine *e, br_node n, br_vec2 origin);
/* The origin. */
br_vec2 br_node_origin(br_engine *e, br_node n);
/* Local matrix (from the fields above, always current). */
br_mat3 br_node_local_transform(br_engine *e, br_node n);
/* Matrix from local to world (stage) space, computed from the ancestors now (always current). */
br_mat3 br_node_world_transform(br_engine *e, br_node n);
/* Local point -> world point. */
br_vec2 br_node_to_global(br_engine *e, br_node n, br_vec2 local_point);
/* World point -> local point. */
br_vec2 br_node_to_local(br_engine *e, br_node n, br_vec2 world_point);

/* ---- appearance */

/* Blend modes. Textures and vertex colours are premultiplied; *_NPM are for straight-alpha
 * sources (custom shaders). INHERIT (default) takes the parent's mode. */
typedef enum br_blend_mode {
    BR_BLEND_INHERIT,
    BR_BLEND_NORMAL,
    BR_BLEND_ADD,
    BR_BLEND_MULTIPLY,
    BR_BLEND_SCREEN,
    BR_BLEND_ERASE,
    BR_BLEND_NONE,
    BR_BLEND_NORMAL_NPM,
    BR_BLEND_ADD_NPM,
    BR_BLEND_SCREEN_NPM,
    BR_BLEND_BUILTIN_COUNT
} br_blend_mode;
/* Values >= this are advanced blend modes owned by the filters module (Task 8): nodes using
 * them are drawn through the effects hook (br_render_set_effects_hook). */
#define BR_BLEND_ADVANCED_FIRST 32

/* Sets alpha (0..1), multiplied down the hierarchy. */
void br_node_set_alpha(br_engine *e, br_node n, float alpha);
/* The node's own alpha. */
float br_node_alpha(br_engine *e, br_node n);
/* Sets the tint (rgb used, a ignored), multiplied down the hierarchy. Default white. */
void br_node_set_tint(br_engine *e, br_node n, br_color tint);
/* The node's own tint. */
br_color br_node_tint(br_engine *e, br_node n);
/* Alpha after multiplying all ancestors (as of the last transform update). */
float br_node_world_alpha(br_engine *e, br_node n);
/* Tint after multiplying all ancestors (as of the last transform update). */
br_color br_node_world_tint(br_engine *e, br_node n);
/* Hides/shows the node and its subtree. */
void br_node_set_visible(br_engine *e, br_node n, bool visible);
/* True if visible. */
bool br_node_visible(br_engine *e, br_node n);
/* Like visible, but bounds still count it (Pixi renderable). */
void br_node_set_renderable(br_engine *e, br_node n, bool renderable);
/* True if renderable. */
bool br_node_renderable(br_engine *e, br_node n);
/* Draw order among siblings when the parent has sortable_children (stable, ascending). */
void br_node_set_z_index(br_engine *e, br_node n, int z_index);
/* The z-index. */
int br_node_z_index(br_engine *e, br_node n);
/* Sorts children by z-index when it changes. */
void br_node_set_sortable_children(br_engine *e, br_node n, bool sortable);
/* True if children are sorted by z-index. */
bool br_node_sortable_children(br_engine *e, br_node n);
/* Sets the blend mode (br_blend_mode, or >= BR_BLEND_ADVANCED_FIRST for Task 8 modes). */
void br_node_set_blend_mode(br_engine *e, br_node n, int blend_mode);
/* The blend mode as set (BR_BLEND_INHERIT by default). */
int br_node_blend_mode(br_engine *e, br_node n);

/* ---- bounds and culling (PixiJS getBounds, Culler) */

/* World-space axis-aligned bounds of the node and its visible descendants (zero rect if empty). */
br_rect br_node_get_bounds(br_engine *e, br_node n);
/* Bounds of the node and its visible descendants in the node's own local space. */
br_rect br_node_get_local_bounds(br_engine *e, br_node n);
/* Culled nodes (bounds outside the screen) are skipped by rendering. Default false. */
void br_node_set_cullable(br_engine *e, br_node n, bool cullable);
/* True if cullable. */
bool br_node_cullable(br_engine *e, br_node n);
/* When false, the culler does not look at descendants (saves time). Default true. */
void br_node_set_cullable_children(br_engine *e, br_node n, bool cullable_children);
/* Uses this local rect instead of the measured bounds for culling. */
void br_node_set_cull_area(br_engine *e, br_node n, br_rect local_area);
/* Goes back to measured bounds for culling. */
void br_node_clear_cull_area(br_engine *e, br_node n);
/* True if the culler skipped the node in the last frame. */
bool br_node_is_culled(br_engine *e, br_node n);

/* ---- render groups and render layers */

/* A render group's descendants are transformed relative to it and the group transform is
 * applied on the GPU: moving a group (camera, scrolling layer) costs one matrix. */
void br_node_set_render_group(br_engine *e, br_node n, bool is_render_group);
/* True if the node is a render group. */
bool br_node_is_render_group(br_engine *e, br_node n);
/* Draws n (with its subtree) where `layer` is in the draw order instead of where n sits in the
 * hierarchy (Pixi RenderLayer.attach). Any node can be a layer. Transforms still come from n's parent.
 * False (logged) if layer is n or inside n's subtree (the walk would never end). */
bool br_node_layer_attach(br_engine *e, br_node layer, br_node n);
/* Detaches n from its render layer. */
void br_node_layer_detach(br_engine *e, br_node n);
/* The render layer n is attached to (zero handle if none). */
br_node br_node_layer(br_engine *e, br_node n);

/* ---- camera layers (br_camera.h): which cameras draw a node */

/* Layer bits. 0 (the default) = inherit the parent's; the stage counts as layer 1. A camera
 * draws a node only if (its bits & culling_mask) != 0; children are still visited, so a UI
 * container with bits 2 puts its whole subtree on layer 2. */
void br_node_set_layer_bits(br_engine *e, br_node n, uint32_t bits);
/* The node's own layer bits (0 = inherited, also for a stale handle). */
uint32_t br_node_layer_bits(br_engine *e, br_node n);

/* ---- masks (scissor and stencil here; alpha and colour masks are drawn by Task 8) */

typedef enum br_mask_kind {
    BR_MASK_NONE,
    BR_MASK_SCISSOR,   /* axis-aligned world bounds of the mask node clip the subtree */
    BR_MASK_STENCIL,   /* the mask node's geometry (any node, nested, inverse) clips the subtree */
    BR_MASK_ALPHA,     /* Task 8, through the effects hook */
    BR_MASK_COLOR      /* Task 8, through the effects hook */
} br_mask_kind;

/* Masks n's subtree with `mask` (which is then no longer drawn as content). Zero mask or
 * BR_MASK_NONE removes the mask. inverse: draw only outside the mask (stencil/alpha).
 * n inside the mask's own subtree (or n == mask) is rejected (logged): n ends up unmasked.
 * The renderer also ignores masks met while it draws a mask's geometry. */
void br_node_set_mask(br_engine *e, br_node n, br_node mask, br_mask_kind kind, bool inverse);
/* The mask node (zero handle if none); kind and inverse may be NULL. */
br_node br_node_mask(br_engine *e, br_node n, br_mask_kind *kind, bool *inverse);

/* ---- effect slots (owned by Task 8; set here so traversal can route nodes to the hook) */

/* Stores an opaque filter list owned by the filters module (NULL = none). */
void br_node_set_filters(br_engine *e, br_node n, void *filters);
/* The opaque filter list, NULL if none. */
void *br_node_filters(br_engine *e, br_node n);
/* Marks the subtree to be rendered once into a texture and reused (Pixi cacheAsTexture). */
void br_node_set_cache_as_texture(br_engine *e, br_node n, bool cache);
/* True if cache_as_texture is set. */
bool br_node_cache_as_texture(br_engine *e, br_node n);
/* True if rendering routes this node through the effects hook (filters, cache, alpha/colour
 * mask, advanced blend mode). */
bool br_node_needs_effects(br_engine *e, br_node n);

/* ---- interaction fields (read by the events module, Task 7) */

typedef enum br_event_mode {
    BR_EVENT_MODE_PASSIVE,   /* default: not a target, children are still tested */
    BR_EVENT_MODE_NONE,      /* ignores events, children too */
    BR_EVENT_MODE_AUTO,      /* target only if the parent is interactive */
    BR_EVENT_MODE_STATIC,    /* target, no global move */
    BR_EVENT_MODE_DYNAMIC    /* target, also gets ticker-driven move events */
} br_event_mode;

typedef enum br_hit_area_type {
    BR_HIT_AREA_NONE,        /* use the node type's hit_test / bounds */
    BR_HIT_AREA_RECT,
    BR_HIT_AREA_CIRCLE,
    BR_HIT_AREA_ELLIPSE,
    BR_HIT_AREA_ROUNDED_RECT,
    BR_HIT_AREA_TRIANGLE,
    BR_HIT_AREA_POLYGON
} br_hit_area_type;

/* A hit-area shape in the node's local space. For polygons, points are copied by the setter. */
typedef struct br_hit_area {
    br_hit_area_type type;
    br_rect          rect;
    br_circle        circle;
    br_ellipse       ellipse;
    br_rounded_rect  rounded_rect;
    br_triangle      triangle;
    const br_vec2   *points;        /* polygon points */
    uint32_t         point_count;
} br_hit_area;

/* Sets how the node takes part in pointer events. */
void br_node_set_event_mode(br_engine *e, br_node n, br_event_mode mode);
/* The event mode (default passive). */
br_event_mode br_node_event_mode(br_engine *e, br_node n);
/* When false, children are not hit-tested. Default true. */
void br_node_set_interactive_children(br_engine *e, br_node n, bool interactive_children);
/* True if children are hit-tested. */
bool br_node_interactive_children(br_engine *e, br_node n);
/* Sets the hit area (copied, polygon points too). NULL or type NONE clears it. */
void br_node_set_hit_area(br_engine *e, br_node n, const br_hit_area *area);
/* The hit area (type NONE if unset). Polygon points belong to the node. */
br_hit_area br_node_hit_area(br_engine *e, br_node n);
/* True if the local point is inside the shape (false for type NONE). */
bool br_hit_area_contains(const br_hit_area *area, br_vec2 local_point);
/* Cursor while hovered: an SDL_SystemCursor value, or -1 (default) for none. */
void br_node_set_cursor(br_engine *e, br_node n, int cursor);
/* The cursor, -1 if none. */
int br_node_cursor(br_engine *e, br_node n);
/* Runs the node type's hit_test callback on a local point (false if the type has none). */
bool br_node_type_hit_test(br_engine *e, br_node n, br_vec2 local_point);

/* ---- user components: dense arrays, described field by field so scene files can fill them */

#define BR_FIELD_STRING_SIZE 64   /* BR_FIELD_STRING and BR_FIELD_ASSET are char[64] in the struct */

typedef enum br_field_type {
    BR_FIELD_BOOL,     /* bool */
    BR_FIELD_INT,      /* int32_t */
    BR_FIELD_FLOAT,    /* float */
    BR_FIELD_VEC2,     /* br_vec2 */
    BR_FIELD_VEC3,     /* br_vec3 */
    BR_FIELD_COLOR,    /* br_color */
    BR_FIELD_STRING,   /* char[BR_FIELD_STRING_SIZE] */
    BR_FIELD_ASSET,    /* char[BR_FIELD_STRING_SIZE], an asset path */
    BR_FIELD_NODE      /* br_node (scene files resolve it by label) */
} br_field_type;

typedef struct br_field_desc { const char *name; br_field_type type; size_t offset; } br_field_desc;

typedef struct br_component_desc {
    const char *name; size_t size; const br_field_desc *fields; uint32_t field_count;
} br_component_desc;

/* Registers a component (desc, name and field list are copied). Returns its id, 0 on failure
 * (logged): duplicate name, size 0, or a field without a name, of unknown type or not inside size. */
uint32_t br_component_register(br_engine *e, const br_component_desc *desc);
/* Finds a component id by name, 0 if unknown. */
uint32_t br_component_find(br_engine *e, const char *name);
/* The registered descriptor, NULL if unknown. */
const br_component_desc *br_component_get_desc(br_engine *e, uint32_t component_id);
/* Adds a zeroed component to n (returns the existing one if present). NULL on failure. */
void *br_component_add(br_engine *e, br_node n, uint32_t component_id);
/* The component data of n, NULL if absent. Pointers die when the component array grows or shrinks. */
void *br_component_get(br_engine *e, br_node n, uint32_t component_id);
/* Removes the component from n (swap-remove: the last element moves into its slot). */
void br_component_remove(br_engine *e, br_node n, uint32_t component_id);
/* Number of instances (for iteration with br_component_at / br_component_owner_at). */
uint32_t br_component_count(br_engine *e, uint32_t component_id);
/* The i-th instance's data. */
void *br_component_at(br_engine *e, uint32_t component_id, uint32_t i);
/* The node owning the i-th instance (may be invalid if destroyed this frame: check br_node_valid). */
br_node br_component_owner_at(br_engine *e, uint32_t component_id, uint32_t i);

#endif
