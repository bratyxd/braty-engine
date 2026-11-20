/* br_gltf.h - glTF 2.0 models (.gltf / .glb): parse once into a br_model asset, instantiate many
 * times into any View3D.
 *
 *   br_model *m = br_model_load(e, "models/Fox/Fox.glb");      (asset cache, ref counted)
 *   br_node3d fox = br_model_instantiate(e, m);                 (a detached "model3d" node)
 *   br_node3d_add_child(e, br_view3d_root(e, view), fox);
 *   br_animator a = br_animator_add(e, br_anim_target_3d(fox)); (br_animation.h)
 *   br_animator_add_model_clips(e, a, m);
 *   br_animator_play(e, a, "Run");
 *
 * Loading (asset loader "model", extensions gltf/glb): the file, its buffers (GLB chunk, data
 * URIs, external .bin) and its images (bufferView, data URI, external file, decoded by
 * stb_image) are read and converted on a worker thread when loaded with br_assets_load_async;
 * the main thread only creates GPU textures (with mipmaps), geometries and materials.
 * Supported: scenes and node hierarchy (TRS or matrix), triangle meshes with every vertex
 * attribute (normals and tangents generated when missing), skins, morph targets (first 8),
 * animations (linear / step / cubic spline), PBR metallic-roughness materials with samplers,
 * uv sets, alpha modes, double sided, cameras, KHR_lights_punctual,
 * KHR_materials_emissive_strength, KHR_materials_unlit, KHR_texture_transform,
 * KHR_materials_transmission (approximated with alpha blending), KHR_mesh_quantization,
 * EXT_mesh_gpu_instancing (one batched_mesh3d per primitive). Compressed glTF (KHR_draco_mesh_compression, EXT_meshopt_compression,
 * KHR_texture_basisu) fails with an error naming the extension. Other unknown extensions load
 * and are logged once per engine (unknown *required* ones fail).
 *
 * Conventions match the 3D core (right-handed, y up, -Z forward for cameras and lights, CCW
 * front faces, glTF uv origin = top-left of the image). Material colours are linear.
 */
#ifndef BR_GLTF_H
#define BR_GLTF_H

#include "braty/br_anim3d.h"
#include "braty/br_camera3d.h"
#include "braty/br_light3d.h"
#include "braty/br_material3d.h"
#include "braty/br_mesh3d.h"

/* One node of the model (glTF node). Indices are -1 when absent. */
typedef struct br_model_node {
    char    *name;            /* "" if unnamed */
    char    *extras;          /* the node's glTF "extras" as JSON text, NULL if none */
    int32_t  parent;          /* model node index, -1 for roots */
    br_vec3  translation;     /* rest pose (matrices are decomposed) */
    br_quat  rotation;
    br_vec3  scale;
    int32_t  mesh;            /* index into meshes */
    int32_t  skin;            /* index into skins */
    int32_t  camera;          /* index into cameras */
    int32_t  light;           /* index into lights (KHR_lights_punctual) */
    float    weights[BR_MESH3D_MAX_MORPH_WEIGHTS]; /* rest morph weights (node, else mesh) */
    uint32_t weight_count;
    br_mat4 *instances;       /* EXT_mesh_gpu_instancing: per-instance matrices in the node's space */
    uint32_t instance_count;  /* 0 = an ordinary node */
} br_model_node;

/* One drawable part of a mesh (glTF primitive). */
typedef struct br_model_primitive {
    br_geometry3d geometry;
    int32_t       material;   /* index into materials, -1 = default material */
} br_model_primitive;

/* A glTF mesh: a range of primitives. */
typedef struct br_model_mesh {
    char    *name;
    uint32_t first_primitive;
    uint32_t primitive_count;
} br_model_mesh;

/* A glTF skin. */
typedef struct br_model_skin {
    char     *name;
    uint32_t *joints;          /* model node indices */
    br_mat4  *inverse_binds;   /* one per joint (identity when the file has none) */
    uint32_t  joint_count;
} br_model_skin;

/* A parsed model. Everything is plain arrays; GPU resources belong to the model and are freed
 * with it (after the last instance is destroyed). */
typedef struct br_model {
    br_model_node      *nodes;
    uint32_t            node_count;
    uint32_t           *roots;          /* nodes of the default scene (or every parentless node) */
    uint32_t            root_count;
    br_model_mesh      *meshes;
    uint32_t            mesh_count;
    br_model_primitive *primitives;
    uint32_t            primitive_count;
    br_material3d      *materials;      /* one per glTF material */
    char              **material_names;
    uint32_t            material_count;
    br_material3d       default_material;
    br_texture         *textures;       /* two per glTF image i: [2i] colour (sRGB, straight alpha; also
                                           made for unused images), [2i + 1] data (linear, straight
                                           alpha); zero when the image does not feed such a slot */
    uint32_t            texture_count;  /* 2 * image count */
    br_model_skin      *skins;
    uint32_t            skin_count;
    br_camera3d        *cameras;
    uint32_t            camera_count;
    br_light3d         *lights;
    uint32_t            light_count;
    br_anim3d_clip     *clips;
    uint32_t            clip_count;
    br_aabb             bounds;         /* rest pose, model space */
    uint32_t            refs;           /* asset reference + one per live instance */
    char               *path;           /* the file it was parsed from (error messages) */
    char               *scene_extras;   /* the glTF scene's "extras" as JSON text, NULL if none */
} br_model;

/* How an instance is built (all fields optional; zero = defaults). */
typedef struct br_gltf_options {
    const char *node;         /* only this glTF node and its subtree, its translation reset to 0
                                 (a kit part, "kit.glb#Tree"); NULL = the whole scene */
    const char *autoplay;     /* clip started at once; NULL = the scene extras' "autoplay" (a clip
                                 name, or true = the first clip); "" = none */
    bool        no_batching;  /* keep one mesh3d per node even for repeated static meshes */
    const char *camera;       /* scene loads / view3d JSON "camera": true: the glTF camera (by node
                                 name) that becomes the view's camera; NULL = the first one */
    bool        camera_as_br_camera; /* scene loads: the view becomes a br_camera (br_camera_adopt_view3d)
                                 labelled after the glTF camera's node ("camera" for a framing one),
                                 so it can follow and shake; the view is drawn once, through it */
} br_gltf_options;

/* Registers the module (model loader, model3d node type, animation system). Called by br_builtin.c. */
void br_gltf_register(br_engine *e);

/* ---- models */

/* Loads (or returns the cached) model asset and takes an asset reference (br_assets_unload drops
 * it). NULL on failure (logged). */
br_model *br_model_load(br_engine *e, const char *path);
/* Parses a model from memory without the asset cache (path names external files and is used in
 * errors). Free with br_model_release. NULL on failure (logged). */
br_model *br_model_parse(br_engine *e, const char *path, const void *bytes, size_t size);
/* Drops one reference (br_model_parse's, or an instance's); frees the model at zero. */
void br_model_release(br_engine *e, br_model *model);
/* Index of a clip by name, -1 if none. */
int32_t br_model_find_clip(const br_model *model, const char *name);
/* Index of a node by name, -1 if none. */
int32_t br_model_find_node(const br_model *model, const char *name);

/* ---- instances */

/* Type id of "model3d" (the root node of an instance). */
uint32_t br_model3d_type(br_engine *e);
/* Builds an instance: a detached "model3d" node holding the model's scene (group nodes named
 * like the glTF nodes; mesh3d / camera3d / light3d children for their content) and an
 * animator. The instance keeps the model alive. Zero handle on failure.
 * Same as br_model_instantiate_ex(e, model, NULL): repeated static meshes are batched and node
 * extras become components (see br_model_instantiate_ex). */
br_node3d br_model_instantiate(br_engine *e, br_model *model);
/* Builds an instance with options (NULL = defaults). Static nodes (not animated, no skin or
 * morph targets, no extras, no convention name suffix) that share a mesh are drawn as ONE
 * batched_mesh3d per primitive (child of the instance root); their group nodes still exist but
 * moving them moves nothing. Node extras are applied: components (registered with
 * br_component_register) go on the node's group node as 3D components, and the convention keys
 * of docs/agents/gltf.md (collider, trigger, no_render, visible, layer, cast_shadows, batch).
 * Errors name the file, node and field; bad values are skipped. Zero handle on failure. */
br_node3d br_model_instantiate_ex(br_engine *e, br_model *model, const br_gltf_options *options);
/* Loads (cached) and instantiates "models/kit.glb" or "models/kit.glb#Tree" (one node and its
 * subtree) under parent (zero = detached). The instance owns the asset reference, released when
 * it is destroyed (inside a scene / scene-file load, that load owns it). Zero handle on failure
 * (missing file or node name, logged). */
br_node3d br_gltf_instantiate(br_engine *e, const char *path, br_node3d parent, const br_gltf_options *options);
/* Options for every .glb / .gltf loaded as a scene (br_scene_load) or a 2D prefab: camera, autoplay
 * and no_batching (node is ignored; camera_as_br_camera is for scene loads only). Copied; NULL goes back to the defaults. The file's scene extras
 * ("autoplay", "batch") apply when these leave them unset. */
void br_gltf_set_load_options(br_engine *e, const br_gltf_options *options);
/* The camera3d node made for a glTF camera of an instance: the camera on the node called `name`,
 * or the first camera (name NULL). Zero handle if none. */
br_node3d br_model3d_camera(br_engine *e, br_node3d n, const char *name);
/* The model of an instance, NULL if n is not a model3d node. */
br_model *br_model3d_model(br_engine *e, br_node3d n);
/* The 3D node built for model node `index` of an instance (zero handle if out of range). */
br_node3d br_model3d_node(br_engine *e, br_node3d n, uint32_t index);
/* PixiJS View3D.from: a view of width x height with the model instantiated under its root, a
 * sun with shadows, and the camera framing the model's bounds. Returns the 2D view node. */
br_node br_view3d_from_model(br_engine *e, br_model *model, float width, float height);

/* "gltf.collider": added for nodes named "*-col", "*-colonly", "*-trigger" or with extras
 * "collider" / "trigger". A box in the node's space (from its mesh bounds, else a unit cube). */
typedef struct br_gltf_collider {
    br_vec3 center;
    br_vec3 size;        /* full extents */
    bool    trigger;     /* "*-trigger" / extras "trigger": true */
} br_gltf_collider;
/* Component id of "gltf.collider". */
uint32_t br_gltf_collider_component(br_engine *e);

#endif
