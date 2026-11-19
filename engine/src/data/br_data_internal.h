/* br_data_internal.h - state of the data module (scene files, prefabs, game.json, hot reload). */
#ifndef BR_DATA_INTERNAL_H
#define BR_DATA_INTERNAL_H

#include "app/br_engine_internal.h"
#include "assets/br_assets_internal.h" /* reference capture: a scene load owns what it loads */
#include "braty/braty.h"

#define BR_DATA_PATH_SIZE 512
#define BR_DATA_NAME_SIZE 64

/* Creates a node for a JSON "type" whose creation needs data (rope meshes, rich text, ...). */
typedef br_node (*br_data_create_fn)(br_engine *e, const cJSON *json);
/* Writes a node's live type fields into its saved JSON object (overwriting stored ones). */
typedef void (*br_data_write_fn)(br_engine *e, br_node n, cJSON *out);

/* One JSON "type" name. Several names may map to one node type ("animated_sprite" -> sprite). */
typedef struct br_data_type {
    char              name[BR_DATA_NAME_SIZE];
    uint32_t          type_id;
    br_data_create_fn create;   /* NULL = br_node_create */
    br_data_write_fn  write;    /* NULL = stored JSON only */
} br_data_type;

/* The JSON a node was loaded from (children removed), kept for saving. Indexed by node index.
 * A scene file's root also owns the asset references its load took (released when it dies). */
typedef struct br_data_stored {
    uint32_t        generation;
    cJSON          *json;
    br_asset_group *assets;   /* NULL if the load took no references (or a br_scene owns them) */
} br_data_stored;

/* Geometry and material a scene-JSON "mesh" created for one 3D node (freed when it dies). */
typedef struct br_data_mesh3d {
    uint32_t       generation;   /* of the 3D node; 0 = slot unused */
    br_geometry3d  geometry;
    br_material3d  material;
} br_data_mesh3d;

typedef enum br_data_ref_kind {
    BR_DATA_REF_FIELD,   /* component field of type BR_FIELD_NODE */
    BR_DATA_REF_MASK,    /* "mask": the mask node */
    BR_DATA_REF_LAYER    /* "layer": the render layer to attach to */
} br_data_ref_kind;

/* A label reference resolved once the whole file is built (targets may come later in the file). */
typedef struct br_data_ref {
    br_data_ref_kind kind;
    br_node          node;
    uint32_t         component_id;
    size_t           offset;
    br_mask_kind     mask_kind;
    bool             inverse;
    char             label[BR_DATA_NAME_SIZE];
    char             path[BR_DATA_PATH_SIZE];
} br_data_ref;

/* A named event handler for "on": { "click": "name" }. */
typedef struct br_data_handler {
    br_event_listener_fn fn;
    void                *user;
} br_data_handler;

/* A scene file loaded from disk, watched for hot reload. */
typedef struct br_data_watch {
    char    path[BR_DATA_PATH_SIZE];
    br_node root;
    int64_t modify_time;
} br_data_watch;

typedef struct br_data_state {
    /* error context: source file and JSON path of the value being read */
    char     file[BR_DATA_PATH_SIZE];
    char     path[BR_DATA_PATH_SIZE];
    size_t   path_len;
    char     last_error[1024];
    uint32_t error_count;

    br_array types;        /* br_data_type */
    br_array stored;       /* br_data_stored, by node index */
    br_array refs;         /* br_data_ref, pending during a load */
    int      load_depth;   /* > 0 while a file is being built (refs are deferred) */
    br_map   handlers;     /* name -> br_data_handler */
    br_map   prefabs;      /* path -> cJSON * (parsed prefab files) */

    br_array watches;      /* br_data_watch */
    bool     hot_reload;
    uint32_t poll_frames;
    uint32_t frame_counter;

    cJSON   *game_json;    /* game.json taken over by br_game_config_apply */

    br_asset_group *owner_group;  /* set by the scenes module: loads give their references to it */
    br_array release;             /* br_asset_group * of destroyed roots, freed after the node flush */
    br_array meshes3d;            /* br_data_mesh3d, by 3D node index */
} br_data_state;

/* ---- br_data.c: errors, JSON path, type table */

/* Formats an error at the current JSON path ("file: path: message"), logs and remembers it. */
void br_data_errorf(br_engine *e, const char *fmt, ...);
/* Logs a warning at the current JSON path (not counted as an error). */
void br_data_warnf(br_engine *e, const char *fmt, ...);
/* Appends ".key" to the JSON path; returns the length to restore with br_data_path_pop. */
size_t br_data_path_push_key(br_engine *e, const char *key);
/* Appends "[index]" to the JSON path. */
size_t br_data_path_push_index(br_engine *e, int index);
/* Restores the JSON path to a length returned by a push. */
void br_data_path_pop(br_engine *e, size_t mark);
/* Error context saved while a nested file (a prefab) is read. */
typedef struct br_data_scope {
    char   file[BR_DATA_PATH_SIZE];
    char   path[BR_DATA_PATH_SIZE];
    size_t path_len;
} br_data_scope;
/* Sets the source name and clears the path (start of a file); saves the old context. */
void br_data_begin_file(br_engine *e, const char *file, br_data_scope *saved);
/* Restores what br_data_begin_file replaced. */
void br_data_end_file(br_engine *e, const br_data_scope *saved);
/* One value at the current path: a number / [x, y] / a colour (errors at the current path). */
bool br_data_value_float(br_engine *e, const cJSON *v, float *out);
bool br_data_value_vec2(br_engine *e, const cJSON *v, br_vec2 *out);
bool br_data_value_color(br_engine *e, const cJSON *v, br_color *out);

/* Adds a JSON type name for a node type (with optional factory and live writer). */
void br_data_add_type(br_engine *e, const char *name, uint32_t type_id, br_data_create_fn create,
                      br_data_write_fn write);
/* Finds a JSON type name (registered aliases first, then node type names). */
const br_data_type *br_data_find_type(br_engine *e, const char *name);
/* The first JSON type entry of a node type (for saving), NULL if none. */
const br_data_type *br_data_type_of(br_engine *e, uint32_t type_id);

/* Parses a JSON document; logs "file:line:col: syntax error" on failure. */
cJSON *br_data_parse(br_engine *e, const char *source, const char *text, size_t size);

/* Number for saving: floats written short when that reads back exactly. */
cJSON *br_data_number(float value);
/* Replaces obj[key] or adds it. Takes ownership of value. */
void br_data_json_set(cJSON *obj, const char *key, cJSON *value);
/* [x, y] */
cJSON *br_data_vec2_json(br_vec2 v);
/* "#rrggbb" or "#rrggbbaa" */
cJSON *br_data_color_json(br_color c);

/* ---- per-type JSON (br_node_json_<type>.c) */

void br_node_json_container_register(br_engine *e);
void br_node_json_sprite_register(br_engine *e);
void br_node_json_nine_slice_register(br_engine *e);
void br_node_json_tiling_sprite_register(br_engine *e);
void br_node_json_graphics_register(br_engine *e);
void br_node_json_mesh_register(br_engine *e);
void br_node_json_particle_container_register(br_engine *e);
void br_node_json_text_register(br_engine *e);
void br_node_json_view3d_register(br_engine *e);

/* Container (all-node) fields: transform, appearance, mask, filters, events. */
void br_node_json_container_read(br_engine *e, br_node n, const cJSON *json);
/* Writes the live container fields of n into out. */
void br_node_json_container_write(br_engine *e, br_node n, cJSON *out);
/* Text style object (shared by the three text types). */
void br_node_json_text_style(br_engine *e, const cJSON *json, br_text_style *style);
/* Fill style: colour string/number or { color, alpha, linear/radial/texture ... }. */
bool br_node_json_fill_style(br_engine *e, const cJSON *json, br_fill_style *out);

/* ---- br_scene_file.c */

/* Queues a label reference (resolved at the end of the outermost load). */
void br_data_add_ref(br_engine *e, const br_data_ref *ref);
/* Remembers the JSON a node came from (copied, children dropped). */
void br_data_store_json(br_engine *e, br_node n, const cJSON *json);
/* The stored JSON of a node, NULL if none. */
const cJSON *br_data_stored_json(br_engine *e, br_node n);
/* Builds one node and its subtree from JSON (no ref resolution). Zero handle if skipped. */
br_node br_data_build_node(br_engine *e, const cJSON *json, br_node parent);
/* Applies a node object (fields, components, children) to an existing node. */
void br_data_apply_node(br_engine *e, br_node n, const cJSON *json, bool build_children);
/* Resolves queued references against root (then the stage) and clears the queue. */
void br_data_resolve_refs(br_engine *e, br_node root);

/* Loads a scene file whose asset references go into `owner` (a br_scene's group) instead of a
 * group of the root node. Hot reload watches it as usual. */
br_node br_data_load_owned(br_engine *e, const char *path, br_node parent, br_asset_group *owner);
/* Frees the asset groups of scene-file roots destroyed so far (runs after the node flush). */
void br_data_release_pending(br_engine *e);
/* br_scene_file_collect_assets that also looks inside prefab files (each path once per call
 * order is not deduplicated; the caller does). */
void br_data_collect_expanded(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user);
/* Expands a scene file "assets" list entry: calls fn(path, lazy) per asset (bundle names give
 * each bundle asset). False (logged at the current JSON path) if the entry is malformed. */
typedef void (*br_data_asset_entry_fn)(br_engine *e, const char *path, bool lazy, void *user);
bool br_data_assets_entry(br_engine *e, const cJSON *item, br_data_asset_entry_fn fn, void *user);

/* ---- br_node_json_view3d.c */

/* Frees what scene JSON created for a dying 3D node (node3d destroy listener). */
void br_data_mesh3d_destroyed(br_engine *e, br_node3d n, void *user);

/* ---- br_game_config.c (br_app_init uses these for br_app_desc.config) */

/* br_game_config_apply without the start scene; takes over config->json. */
void br_game_config_apply_settings(br_engine *e, br_game_config *config);
/* Loads config->start_scene under the stage; zero handle if none or it failed. */
br_node br_game_config_load_start_scene(br_engine *e, const br_game_config *config);

/* ---- br_scene_reload.c */

/* Starts watching a scene file loaded into root (hot reload). */
void br_data_watch_add(br_engine *e, const char *path, br_node root);
/* Registers the hot-reload system. */
void br_data_reload_register(br_engine *e);
/* Modification time of a file under the asset root (0 if unknown). */
int64_t br_data_modify_time(br_engine *e, const char *path);

/* ---- br_prefab.c */

/* Expands { "prefab": path, ...overrides } into a new JSON object (caller deletes), NULL on error. */
cJSON *br_data_expand_prefab(br_engine *e, const cJSON *json);
/* Frees cached prefab documents. */
void br_data_prefabs_free(br_engine *e);

#endif
