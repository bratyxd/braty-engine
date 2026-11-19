/* br_gltf_internal.h - state shared by the glTF loader, model instances and the 3D animator. */
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
    uint32_t      anim_index;      /* slot in the animator pool */
} br_gltf_instance;

/* One entry of the dense animator pool. */
typedef struct br_gltf_anim {
    br_animator3d     animator;
    br_gltf_instance *instance;
} br_gltf_anim;

typedef struct br_gltf_state {
    uint32_t model3d_type;
    br_array anims;            /* br_gltf_anim, dense (swap-remove) */
    br_array logged_ext;       /* char[64]: unknown extensions already logged */
} br_gltf_state;

/* br_anim3d.c: the animation system (registered by br_gltf_register). */
void br_anim3d_system(br_engine *e, float dt, void *user);
/* br_gltf_instance.c: registers the model3d node type. */
void br_gltf_instance_register(br_engine *e);
/* The instance of a model3d node, NULL if n is not one. */
br_gltf_instance *br_gltf_instance_get(br_engine *e, br_node3d n);

#endif
