/* br_gltf_scene_internal.h - glTF files as scene content: the hooks the scenes and prefab code
 * call for .glb / .gltf paths, and 3D component fields read from JSON (glTF extras, view3d scene
 * JSON). Implemented in br_gltf_scene.c and br_gltf_extras.c. */
#ifndef BR_GLTF_SCENE_INTERNAL_H
#define BR_GLTF_SCENE_INTERNAL_H

#include "data/br_data_internal.h"

/* True for "x.glb", "x.gltf" and "x.glb#Node" (case-insensitive extension). */
bool br_data_is_model_path(const char *path);
/* The node JSON a model path stands for: a view3d holding the model. scene = true: a full-window
 * view using the file's first camera (a framing camera if it has none) and a sun if it has no
 * lights, with the glTF scene's extras (background, msaa) applied; false: the same as a 2D
 * prefab (a 640 x 360 view, framing camera, sun). Caller deletes. NULL if out of memory. */
cJSON *br_data_model_scene_json(br_engine *e, const char *path, bool scene);
/* br_data_load_owned for a model path (br_scene_load of a .glb): builds that JSON under parent,
 * references going to owner. */
br_node br_data_load_model_owned(br_engine *e, const char *path, br_node parent, br_asset_group *owner);

/* Adds component `name` to a 3D node and fills it from { field: value } (true / null = defaults).
 * BR_FIELD_NODE values name a node under search_root. Errors at the current JSON path; an
 * unknown component is a warning (glTF files carry data for other tools) when lenient, else an
 * error. False on any error. */
bool br_data_component3d_apply(br_engine *e, br_node3d n, const char *name, const cJSON *fields, br_node3d search_root,
                               bool lenient);

#endif
