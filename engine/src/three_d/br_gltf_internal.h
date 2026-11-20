/* br_gltf_internal.h - state shared by the glTF loader, model instances and the skinning pass. */
#ifndef BR_GLTF_INTERNAL_H
#define BR_GLTF_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"

/* A pose of every model node: TRS + morph weights (BR_MESH3D_MAX_MORPH_WEIGHTS per node). */
typedef struct br_gltf_pose {
    br_vec3 *translation;
    br_quat *rotation;
    br_vec3 *scale;
    float   *weights;
} br_gltf_pose;

/* One copy of a batched model node inside a shared batched_mesh3d. */
typedef struct br_gltf_batch_slot {
    uint32_t  node;      /* model node */
    br_node3d batch;     /* the batched_mesh3d */
    uint32_t  instance;  /* index in its instances */
    br_mat4   rest;      /* matrix while shown */
    bool      shown;
} br_gltf_batch_slot;

/* One instance of a model (heap, the "model3d" node data points at it). All arrays are made at
 * instantiate time; nothing is allocated per frame. */
typedef struct br_gltf_instance {
    br_model     *model;
    br_node3d     self;            /* the model3d node */
    br_node3d    *nodes;           /* per model node: its group node */
    uint32_t     *order;           /* model nodes, parents before children */
    uint32_t     *prim_first;      /* per model node: first entry in prim_nodes */
    uint32_t     *prim_count;      /* per model node: mesh3d nodes built for its mesh */
    br_node3d    *prim_nodes;      /* mesh3d nodes */
    uint32_t      prim_node_count;
    uint32_t     *joint_first;     /* per model node with a skin: first matrix in joints */
    br_mat4      *joints;          /* skin slots of the mesh3d nodes point in here */
    uint32_t      joint_total;
    br_mat4      *model_world;     /* scratch: node matrices relative to the instance */
    br_gltf_pose  pose;            /* the playing clip */
    br_gltf_pose  fade_pose;       /* the clip fading out */
    uint32_t      anim_index;      /* slot in the skinning pool */
    int32_t       sub_root;        /* model node of a sub-asset instance ("kit.glb#Tree"), -1 = whole scene */
    br_gltf_batch_slot *batch_slots; /* batched copies, follow their group's destroy / visibility */
    uint32_t      batch_slot_count;
} br_gltf_instance;

/* One entry of the dense skinning pool (every model instance). */
typedef struct br_gltf_anim {
    br_gltf_instance *instance;
} br_gltf_anim;

/* Model instances alive (the profiler's "3D models"). */
uint32_t br_gltf_instance_count(br_engine *e);

/* The 3D rows of one component kind (sparse set: 3D node index -> dense row + 1). */
typedef struct br_component3d_set {
    br_array data;     /* rows of desc->size bytes */
    br_array owners;   /* br_node3d */
    br_array sparse;   /* uint32_t by 3D node index */
} br_component3d_set;

/* An asset group owned by a 3D node (br_gltf_instantiate outside a load). */
typedef struct br_gltf_owned {
    uint32_t        generation;   /* of the 3D node; 0 = unused */
    br_asset_group *group;
} br_gltf_owned;

typedef struct br_gltf_state {
    uint32_t model3d_type;
    br_array anims;            /* br_gltf_anim, dense (swap-remove) */
    br_array logged_ext;       /* char[64]: unknown extensions already logged */
    br_array components3d;     /* br_component3d_set by component id - 1 (grown on first use) */
    br_array owned;            /* br_gltf_owned by 3D node index */
    uint32_t collider_id;      /* "gltf.collider" */
    /* br_gltf_set_load_options (strings copied; "" = unset) */
    char     load_camera[64];
    char     load_autoplay[64];
    bool     load_autoplay_set;
    bool     load_no_batching;
    bool     load_camera_as_br_camera;
} br_gltf_state;

/* br_anim3d.c: the animation system (registered by br_gltf_register). */
void br_anim3d_system(br_engine *e, float dt, void *user);
/* br_gltf_instance.c: registers the model3d node type. */
void br_gltf_instance_register(br_engine *e);
/* Collapses the batched copies whose group node died or is hidden (and restores shown ones). */
void br_gltf_sync_batches(br_engine *e, br_gltf_instance *inst);
/* The instance of a model3d node, NULL if n is not one. */
br_gltf_instance *br_gltf_instance_get(br_engine *e, br_node3d n);
/* Frames `bounds` (NULL = the world bounds of the visible meshes under content) with the view's
 * camera (camera) and/or adds a sun with shadows sized to it under the view root (sun): what
 * br_view3d_from_model does. */
void br_gltf_frame_view(br_engine *e, br_node view, br_node3d content, const br_aabb *bounds, bool camera, bool sun);
/* br_gltf_components.c: registers gltf.collider and the 3D-node destroy listener. */
void br_gltf_components_register(br_engine *e);
/* Frees the 3D component arrays and the owned groups' table (shutdown). */
void br_gltf_components_free(br_engine *e);
/* True if a node name ends with a convention suffix (-col, -colonly, -trigger, -norender). */
bool br_gltf_name_has_convention(const char *name);
/* data/br_gltf_extras.c: applies node extras and name conventions of a fresh instance. */
void br_data_gltf_apply_extras(br_engine *e, br_gltf_instance *inst);

#endif
