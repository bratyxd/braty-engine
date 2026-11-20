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
    /* hot: read for every node by the transform, cull and render walks (first two cache lines) */
    uint32_t parent;             /* node index, BR_NO_NODE if detached */
    uint32_t type_id;
    uint32_t data_index;         /* row in the type's data pool */
    uint32_t world_color;        /* br_render_pack_color(world_tint, world_alpha), kept with them */
    br_array children;           /* uint32_t node indices, in draw order */
    br_array layer_members;      /* br_node: nodes attached to this node as a layer */
    uint64_t culled_frame;       /* culled in cull pass number == this */
    br_node  layer;              /* render layer that draws this node (zero = none) */
    br_node  mask;
    void    *filters;            /* opaque, owned by the filters module */
    uint32_t mask_users;         /* nodes using this node as their mask: not drawn as content */
    br_mask_kind mask_kind;
    br_blend_mode blend_mode;

    bool destroyed;              /* waiting for the flush at the start of the next frame */
    bool transform_dirty;        /* local fields changed since the last transform update */
    bool color_dirty;            /* alpha/tint changed */
    bool child_dirty;            /* a descendant has a dirty flag */
    bool visible;
    bool renderable;
    bool render_group;
    bool cullable;
    bool cullable_children;
    bool cache_as_texture;

    /* colour: touched when alpha/tint change */
    float    alpha;
    br_color tint;
    float    world_alpha;
    br_color world_tint;

    /* cold */
    char    *label;              /* NULL = no label */
    bool     sort_queued;        /* in the sort queue */
    bool     compact_queued;     /* children / layer members lose destroyed entries in this flush */
    bool     sortable_children;
    bool     has_cull_area;
    bool     mask_inverse;
    br_rect  cull_area;
    uint32_t cull_slot;          /* position in br_scene_state.cullables while cullable */
    int      z_index;
    uint32_t layer_bits;         /* camera layers (br_node_set_layer_bits), 0 = the parent's */

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
    float   rotation_cos;  /* cos/sin of rotation, updated whenever rotation is set (br_scene_set_rotation) */
    float   rotation_sin;
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
    bool     flushing;           /* br_scene_flush_destroyed is running (destroys inside it join it) */
    br_array destroy_listeners;  /* br_destroy_listener */
    br_array sort_queue;         /* uint32_t node indices whose children need sorting */
    br_array sort_scratch;       /* br_sort_item */
    br_node  root;
    br_array cullables;          /* uint32_t indices of nodes with `cullable` set (record.cull_slot = position) */
    uint64_t cull_frame;         /* bumped by every cull pass */
    uint32_t sprite_type;        /* type id of "sprite" */
    bool     has_cull_view;      /* cameras are active: cull against cull_view, not the window */
    br_rect  cull_view;          /* stage rect the cameras show (union) */
    /* explicit stacks of the tree walks (no recursion per level: hierarchies of any depth) */
    br_array walk_stack;         /* uint32_t: label search */
    br_array transform_stack;    /* br_transform_item (br_scene_systems.c) */
    br_array transform_parents;  /* br_parent_state: parent state shared by the queued siblings */
    br_array bounds_stack;       /* br_bounds_item (br_scene_systems.c) */
} br_scene_state;

/* ---- br_node.c */

/* Registers the scene module (called by br_builtin). */
void br_scene_register(br_engine *e);
/* Destroys every node (shutdown callback registered last by br_builtin, so it runs first). */
void br_scene_shutdown_nodes(br_engine *e, void *user);
/* The record of a live node, NULL if stale or destroyed. */
br_node_record *br_scene_record(br_engine *e, br_node n);
/* The record at an index (no checks; index must be a live slot). Pointers stay valid until a
 * node is created. Inline: the systems and the render walk call it for every node. */
static inline br_node_record *br_scene_record_at(br_scene_state *s, uint32_t index)
{
    return (br_node_record *)s->nodes.items.data + index;
}
/* The transform at an index (no checks). */
static inline br_node_transform *br_scene_transform_at(br_scene_state *s, uint32_t index)
{
    return (br_node_transform *)s->transforms.data + index;
}
/* The handle of the node at an index. */
br_node br_scene_handle(br_scene_state *s, uint32_t index);
/* The type entry of a type id, NULL if unknown. */
br_node_type_entry *br_scene_type_entry(br_scene_state *s, uint32_t type_id);
/* Per-type data of the node at an index (NULL for types without data). */
void *br_scene_type_data(br_scene_state *s, uint32_t index);
/* Sets rotation and its cached cos/sin (the only way rotation should be written). */
void br_scene_set_rotation(br_node_transform *t, float radians);
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
/* Like br_scene_refresh_subtree, but colours as if the node and its ancestors had alpha 1 and a
 * white tint (cache_as_texture renders its content that way). Call br_scene_refresh_subtree
 * afterwards to put the real colours back. */
void br_scene_refresh_subtree_uncolored(br_scene_state *s, uint32_t index);
/* Cull against this stage rect instead of the window (NULL = the window again). Set by the
 * camera module every frame while cameras exist. */
void br_scene_set_cull_view(br_scene_state *s, const br_rect *view);
/* True if the culler skipped the node in the last cull pass. */
bool br_scene_is_culled(br_scene_state *s, uint32_t index);
/* Adds a node index to / removes it from the cullable list (keeps cull_slot right). */
void br_scene_cullable_add(br_scene_state *s, uint32_t index);
void br_scene_cullable_remove(br_scene_state *s, uint32_t index);
/* Sorts a list of node indices by z-index, stable. */
void br_scene_sort_by_z(br_scene_state *s, uint32_t *indices, uint32_t count);
/* Sorts a list of node handles by z-index, stable. */
void br_scene_sort_handles_by_z(br_scene_state *s, br_node *handles, uint32_t count);

/* ---- br_scene_systems.c */

/* Sets up the walk stacks (transform_stack, bounds_stack). */
void br_scene_walks_init(br_scene_state *s);
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
