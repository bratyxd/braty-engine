/* br_three_d_internal.h - private state of the 3D module (Task 9). */
#ifndef BR_THREE_D_INTERNAL_H
#define BR_THREE_D_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_camera3d.h"
#include "braty/br_light3d.h"
#include "braty/br_material3d.h"
#include "braty/br_mesh3d.h"
#include "braty/br_post3d.h"
#include "braty/br_view3d.h"

#include <float.h>

#define BR_NONE3D 0xFFFFFFFFu
#define BR_SHADOW3D_TILES 8          /* atlas = 4 x 2 tiles: 0-3 cascades, 4-7 spot lights */
#define BR_BLOOM3D_MAX_LEVELS 6

/* ---- node table (struct of arrays, indexed by br_node3d.index) */

typedef struct br_node3d_table {
    uint32_t  capacity;
    uint32_t  live;
    uint32_t *generation;     /* current generation of each slot (0 = never used) */
    uint8_t  *alive;          /* 1 = valid handle */
    uint32_t *type;
    uint32_t *data_index;     /* index into the type's dense pool */
    uint32_t *parent;
    uint32_t *first_child;
    uint32_t *last_child;
    uint32_t *next_sibling;
    uint32_t *prev_sibling;
    char    **label;
    br_vec3  *position;
    br_quat  *rotation;
    br_vec3  *scale;
    br_mat4  *world;          /* cached, updated by br_three_d_update_world */
    uint8_t  *visible;
    uint8_t  *pickable;
    uint32_t *layer;
    br_array  free_slots;     /* uint32_t: released indices */
    br_array  pending;        /* uint32_t: destroyed this frame, freed at the next flush */
    br_array  walk;           /* uint32_t: scratch stack of br_node3d_find_by_label */
    /* br_node3d_child_at cursor: the last (parent, i) -> child answer, valid while epoch matches */
    uint32_t  epoch;          /* bumped by every link / unlink */
    uint32_t  cursor_epoch;
    uint32_t  cursor_parent;
    uint32_t  cursor_index;
    uint32_t  cursor_child;
    uint32_t  count_epoch;    /* br_node3d_child_count answer for count_parent, same rule */
    uint32_t  count_parent;
    uint32_t  count_value;
} br_node3d_table;

/* One registered 3D node type with its dense data pool. */
typedef struct br_node3d_type_entry {
    br_node3d_type_desc desc;  /* name points at `name` below */
    char               *name;
    br_array            data;   /* data_size bytes per node */
    br_array            owners; /* uint32_t node index per data element */
} br_node3d_type_entry;

typedef struct br_node3d_destroy_entry {
    br_node3d_destroy_fn fn;
    void                *user;
} br_node3d_destroy_entry;

typedef struct br_listener3d {
    br_node3d     node;
    br_event_type type;
    br_event3d_fn fn;
    void         *user;
    bool          removed;
} br_listener3d;

/* ---- resources */

typedef struct br_geometry3d_data {
    br_vertex3d   *vertices;
    uint32_t       vertex_count;
    uint32_t      *indices;
    uint32_t       index_count;
    uint32_t       morph_target_count;
    br_aabb        bounds;
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    SDL_GPUBuffer *morph_buffer;   /* storage: 2 float4 per target per vertex */
} br_geometry3d_data;

typedef struct br_batched3d {
    br_array instances;      /* br_instance3d, local to the node */
    br_aabb  bounds;
    bool     cast_shadows;
} br_batched3d;

/* Data of a batched_mesh3d node (the store is heap-allocated so draw data never moves). */
typedef struct br_batched_mesh3d_data {
    br_geometry3d geometry;
    br_material3d material;
    br_batched3d *store;
} br_batched_mesh3d_data;

/* ---- per-frame draw items of a view */

typedef struct br_item3d {
    uint32_t            passes;          /* br_pass3d bits */
    float               depth;           /* camera distance along the view axis */
    br_geometry3d       geometry;
    br_material3d       material;
    uint32_t            instance_offset; /* bytes into the transient vertex buffer */
    uint32_t            instance_count;
    uint32_t            joint_offset;    /* matrices into the view's joint buffer */
    uint32_t            joint_count;
    float               morph[BR_MESH3D_MAX_MORPH_WEIGHTS];
    uint32_t            morph_count;
    br_draw3d_custom_fn custom_fn;       /* non-NULL = custom item */
    void               *custom_user;
} br_item3d;

/* Fragment uniform block b0 of 3D shaders (mesh3d_common.hlsli). */
typedef struct br_frame3d_uniforms {
    float   camera_position[4];
    float   camera_forward[4];
    float   sky[4];
    float   ground[4];
    float   counts[4];
    float   cascade_far[4];
    br_mat4 shadow_matrix[BR_SHADOW3D_TILES];
    float   shadow_rect[BR_SHADOW3D_TILES][4];
    float   light_position[BR_LIGHT3D_MAX][4];
    float   light_direction[BR_LIGHT3D_MAX][4];
    float   light_color[BR_LIGHT3D_MAX][4];
    float   light_params[BR_LIGHT3D_MAX][4];
} br_frame3d_uniforms;

/* A light found during collect. */
typedef struct br_light3d_entry {
    br_light3d light;
    br_mat4    world;
    float      score;   /* importance when over BR_LIGHT3D_MAX: FLT_MAX directional, else intensity / (1 + d^2) */
} br_light3d_entry;

/* Everything a view owns. Heap-allocated: its address is the render callback's user pointer. */
typedef struct br_view3d_state {
    br_node   node;
    br_node3d root;
    br_node3d own_root;            /* created with the view, destroyed with it (root may be another) */
    br_node3d camera;
    float     width;
    float     height;
    bool      auto_resize;
    float     resolution;          /* 0 = window pixel density */
    uint32_t  msaa;
    br_color  background;
    br_color  sky;
    br_color  ground;
    float     ambient_intensity;
    uint32_t  culling_mask;
    uint32_t  shadow_tile_size;
    br_post3d_settings post;
    br_post3d_pass     passes[BR_POST3D_MAX_PASSES];
    uint32_t           pass_count;

    /* GPU targets (recreated when the pixel size or settings change) */
    uint32_t          pixel_width;
    uint32_t          pixel_height;
    uint32_t          samples;     /* actual MSAA samples of hdr */
    br_render_target *hdr;
    br_render_target *post_temp[2]; /* ping-pong for custom passes */
    br_render_target *bloom[BR_BLOOM3D_MAX_LEVELS];
    uint32_t          bloom_count;
    br_render_target *shadow_atlas;
    uint32_t          shadow_atlas_tile;
    br_texture        output;      /* 8-bit tone-mapped render texture */
    SDL_GPUBuffer    *joint_buffer;
    uint32_t          joint_capacity;   /* matrices */
    SDL_GPUTransferBuffer *joint_transfer;

    /* this frame (filled by collect, read by the RENDER callback) */
    uint64_t            collected_frame;
    bool                collected;
    br_array            items;       /* br_item3d */
    br_array            opaque;      /* uint32_t item indices, sorted */
    br_array            transparent;
    br_array            shadow;
    br_array            joints;      /* br_mat4 for this frame's skinned items */
    br_array            lights;      /* br_light3d_entry */
    br_mat4             view;
    br_mat4             projection;
    br_mat4             view_projection;
    br_vec3             camera_position;
    br_vec3             camera_forward;
    br_aabb             caster_bounds;
    br_frame3d_uniforms frame;
    uint32_t            shadow_tiles; /* bit i = tile i rendered this frame */
    br_mat4             tile_view[BR_SHADOW3D_TILES];
    br_mat4             tile_projection[BR_SHADOW3D_TILES];
    br_view3d_stats     stats;

    /* pointer events */
    br_node3d hovered;
    br_node3d pressed;
} br_view3d_state;

/* Data of a "view3d" 2D node. */
typedef struct br_view3d_node {
    br_view3d_state *state;
} br_view3d_node;

struct br_render3d_ctx {
    br_engine       *e;
    br_view3d_state *view;
    br_plane         frustum[6];  /* br_frustum_planes of the view-projection */
};

typedef struct br_three_d_state {
    br_node3d_table nodes;
    br_array        types;            /* br_node3d_type_entry; type id = index + 1 */
    br_array        destroy_listeners;
    br_array        listeners;        /* br_listener3d */
    uint32_t        dispatch_depth;   /* > 0 while 3D listeners run */
    bool            flushing;         /* br_node3d_flush is running: nested destroys only append */
    br_pool         geometries;       /* br_geometry3d_data */
    br_pool         materials;        /* br_material3d_desc */
    br_array        views;            /* br_view3d_state * */
    uint32_t        view_type;
    uint32_t        mesh_type;
    uint32_t        batched_type;
    uint32_t        light_type;
    uint32_t        camera_type;
    SDL_GPUBuffer  *dummy_storage;    /* bound when a deform draw has no joints / morphs */
} br_three_d_state;

/* ---- shared between the module's files */

/* br_node3d.c */
void br_node3d_table_init(br_engine *e);
void br_node3d_table_free(br_engine *e);
/* Destroys every live 3D node (on_destroy and destroy listeners run). Registered by br_builtin
 * to run early at shutdown, while the modules that listen for 3D node deaths still exist. */
void br_three_d_shutdown_nodes(br_engine *e, void *user);
void br_node3d_flush(br_engine *e);
bool br_node3d_index_valid(br_three_d_state *s, br_node3d n);
br_node3d br_node3d_handle(br_three_d_state *s, uint32_t index);
/* Recomputes cached world matrices of root's subtree (root's own parent chain included). */
void br_three_d_update_world(br_engine *e, br_node3d root);

/* br_geometry3d.c / br_material3d.c */
br_geometry3d_data *br_geometry3d_data_get(br_engine *e, br_geometry3d g);
void br_geometry3d_free_all(br_engine *e);
void br_material3d_free_all(br_engine *e);

/* br_node_types3d.c */
void br_node_types3d_register(br_engine *e);
void br_render3d_add_light(br_render3d_ctx *ctx, const br_light3d *light, br_mat4 world);
/* br_render3d_submit with instance matrices multiplied by *parent (NULL = as given). */
void br_render3d_submit_with_parent(br_render3d_ctx *ctx, const br_draw3d *draw, const br_mat4 *parent);

/* br_view3d.c / br_render3d.c / br_pick3d.c */
void br_view3d_register_type(br_engine *e);
br_view3d_state *br_view3d_state_get(br_engine *e, br_node view);
void br_view3d_compute_camera(br_engine *e, br_view3d_state *v);
void br_render3d_collect(br_engine *e, br_view3d_state *v);
void br_render3d_draw(br_engine *e, const br_render_callback_info *info, void *user);
void br_render3d_release_targets(br_engine *e, br_view3d_state *v);
bool br_pick3d_view_hit_test(br_engine *e, br_view3d_state *v, br_vec2 local_point);
void br_pick3d_attach_view(br_engine *e, br_view3d_state *v);
void br_pick3d_node_destroyed(br_engine *e, br_node3d n);

#endif
