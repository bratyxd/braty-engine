/* br_scene_internal.h - the node table and type/component pools. Engine-private.
 *
 * Other engine modules may include this to read node fields directly (read-only, please:
 * setters keep the dirty flags right). Games use br_node.h.
 *
 * Layout: a br_pool of br_node_record (hierarchy, flags, appearance, masks, interaction) plus a
 * parallel array of br_node_transform with the same index. Per-type data and components are
 * dense arrays with an owner list, so systems iterate them without touching the node table.
 */
#ifndef BR_SCENE_INTERNAL_H
#define BR_SCENE_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_node.h"

#define BR_NO_NODE 0xFFFFFFFFu

typedef struct br_node_record {
    uint32_t parent;             /* node index, BR_NO_NODE if detached */
    br_array children;           /* uint32_t node indices, in draw order */
    uint32_t type_id;
    uint32_t data_index;         /* row in the type's data pool */
    char    *label;              /* NULL = no label */

    bool destroyed;              /* waiting for the flush at the start of the next frame */
    bool transform_dirty;        /* local fields changed since the last transform update */
    bool color_dirty;            /* alpha/tint changed */
    bool child_dirty;            /* a descendant has a dirty flag */
    bool sort_queued;            /* in the sort queue */

    bool visible;
    bool renderable;
    bool sortable_children;
    bool render_group;
    bool cullable;
    bool cullable_children;
    bool has_cull_area;
    br_rect  cull_area;
    uint64_t culled_frame;       /* culled in cull pass number == this */
    int      z_index;
    int      blend_mode;         /* br_blend_mode or >= BR_BLEND_ADVANCED_FIRST */

    float    alpha;
    br_color tint;
    float    world_alpha;
    br_color world_tint;

    br_node  layer;              /* render layer that draws this node (zero = none) */
    br_array layer_members;      /* br_node: nodes attached to this node as a layer */

    br_node      mask;
    br_mask_kind mask_kind;
    bool         mask_inverse;
    uint32_t     mask_users;     /* nodes using this node as their mask: not drawn as content */
    void        *filters;        /* opaque, owned by the filters module */
    bool         cache_as_texture;

    br_event_mode event_mode;
    bool          interactive_children;
    br_hit_area   hit_area;      /* polygon points owned (br_alloc) */
    int           cursor;
} br_node_record;

typedef struct br_node_transform {
    br_vec2 position;
    br_vec2 scale;
    br_vec2 skew;
    br_vec2 pivot;
    br_vec2 origin;
    float   rotation;
    br_mat3 local;
    br_mat3 world;   /* local -> space of the nearest render-group ancestor (the stage if none) */
} br_node_transform;

typedef struct br_node_type_entry {
    br_node_type_desc desc;      /* desc.name points at `name` */
    char    *name;
    br_array data;               /* data_size bytes per node of this type */
    br_array owners;             /* uint32_t node index per data row */
} br_node_type_entry;

typedef struct br_component_entry {
    br_component_desc desc;      /* desc.name / desc.fields point at the copies below */
    char          *name;
    br_field_desc *fields;
    br_array data;               /* desc.size bytes per instance */
    br_array owners;             /* br_node per instance */
    br_array sparse;             /* uint32_t per node index: dense index + 1, 0 = none */
} br_component_entry;

typedef struct br_destroy_listener {
    br_node_destroy_fn fn;
    void              *user;
} br_destroy_listener;

typedef struct br_scene_state {
    br_pool  nodes;              /* br_node_record */
    br_array transforms;         /* br_node_transform, same index as nodes */
    br_array types;              /* br_node_type_entry, type id = index + 1 */
    br_array components;         /* br_component_entry, component id = index + 1 */
    br_array pending_destroy;    /* uint32_t node indices */
    br_array destroy_listeners;  /* br_destroy_listener */
    br_array sort_queue;         /* uint32_t node indices whose children need sorting */
    br_array sort_scratch;       /* br_sort_item */
    br_node  root;
    uint32_t cullable_count;
    uint64_t cull_frame;         /* bumped by every cull pass */
    uint32_t sprite_type;        /* type id of "sprite" */
} br_scene_state;

/* ---- br_node.c */

/* Registers the scene module (called by br_builtin). */
void br_scene_register(br_engine *e);
/* Destroys every node (shutdown callback registered last by br_builtin, so it runs first). */
void br_scene_shutdown_nodes(br_engine *e, void *user);
/* The record of a live node, NULL if stale or destroyed. */
br_node_record *br_scene_record(br_engine *e, br_node n);
/* The record at an index (no checks; index must be a live slot). */
br_node_record *br_scene_record_at(br_scene_state *s, uint32_t index);
/* The transform at an index (no checks). */
br_node_transform *br_scene_transform_at(br_scene_state *s, uint32_t index);
/* The handle of the node at an index. */
br_node br_scene_handle(br_scene_state *s, uint32_t index);
/* The type entry of a type id, NULL if unknown. */
br_node_type_entry *br_scene_type_entry(br_scene_state *s, uint32_t type_id);
/* Per-type data of the node at an index (NULL for types without data). */
void *br_scene_type_data(br_scene_state *s, uint32_t index);
/* Local matrix computed from the transform fields. */
br_mat3 br_scene_compute_local(const br_node_transform *t);
/* Product of local matrices from index up to the stage, or up to (not including) the nearest
 * render-group ancestor when stop_at_group. */
br_mat3 br_scene_chain_transform(br_scene_state *s, uint32_t index, bool stop_at_group);
/* World matrix of the nearest render-group ancestor (identity if none): the space the node's
 * cached `world` is relative to. */
br_mat3 br_scene_group_space(br_scene_state *s, uint32_t index);
/* Recomputes cached world transform/alpha/tint of a subtree from its ancestors (used for
 * nodes drawn outside the stage walk: render-to-texture roots, masks). */
void br_scene_refresh_subtree(br_scene_state *s, uint32_t index);
/* True if the culler skipped the node in the last cull pass. */
bool br_scene_is_culled(br_scene_state *s, uint32_t index);
/* Sorts a list of node indices by z-index, stable. */
void br_scene_sort_by_z(br_scene_state *s, uint32_t *indices, uint32_t count);
/* Sorts a list of node handles by z-index, stable. */
void br_scene_sort_handles_by_z(br_scene_state *s, br_node *handles, uint32_t count);

/* ---- br_scene_systems.c */

/* Adds the transform, sort and cull systems (PRE_RENDER) and the destroy flush (INPUT). */
void br_scene_add_systems(br_engine *e);
/* Runs the deferred destroys now. */
void br_scene_flush_destroyed(br_engine *e);
/* World-space bounds of a subtree given the node's world matrix m. */
void br_scene_bounds(br_scene_state *s, br_engine *e, uint32_t index, br_mat3 m, br_rect *acc, bool *has);

/* ---- br_component.c */

/* Removes every component of a node index (flush). */
void br_component_remove_all(br_scene_state *s, uint32_t index);
/* Frees all component storage (shutdown). */
void br_component_free_all(br_scene_state *s);

/* ---- br_sprite.c */

/* Registers the sprite node type. */
void br_sprite_register(br_engine *e);

#endif
