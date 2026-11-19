/* br_scene_file.h - scene files (*.scene.json): load, save, hot reload, JSON helpers.
 *
 * A scene file is one node object; children nest:
 *
 *   { "type": "container", "label": "level",
 *     "assets": ["atlas.json"],                         (root only: loaded first)
 *     "children": [
 *       { "type": "sprite", "label": "hero", "texture": "atlas.json#hero.png",
 *         "position": [100, 200], "anchor": [0.5, 0.5], "tint": "#ffcc00",
 *         "components": { "velocity": { "x": 40, "y": 0 }, "target": { "node": "door" } } },
 *       { "prefab": "enemy.prefab.json", "position": [300, 200] } ] }
 *
 * Every node type's JSON comes from its from_json callback (br_node_type_set_from_json); the
 * data module installs them for all built-in types. Container fields (transform, alpha, tint,
 * mask, filters, events, ...) work on every node. The full format: docs/DATA_FORMATS.md.
 *
 * Errors never stop a load: each is logged as "file: json.path: reason" (for example
 * "level.scene.json: children[2].components.velocity.x: expected a number"), a bad value is
 * skipped, a bad node is skipped with its subtree, the rest still loads.
 *
 * Node references (component fields of type BR_FIELD_NODE, "mask", "layer") name a label; they
 * are resolved after the whole file is built, inside the loaded tree first, then the stage.
 */
#ifndef BR_SCENE_FILE_H
#define BR_SCENE_FILE_H

#include "braty/br_events.h"
#include "braty/br_render.h"

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_data_register(br_engine *e);

/* ---- loading and saving */

/* Loads a scene file (path relative to the asset root) as a new subtree under parent (zero
 * handle = detached). Returns its root, zero handle if the file is missing or not JSON.
 * Debug builds watch the file and reload it in place when it changes (hot reload). */
br_node br_scene_file_load(br_engine *e, const char *path, br_node parent);
/* Same from JSON text in memory; source_name is used in error messages. Not watched. */
br_node br_scene_file_load_json(br_engine *e, const char *json, size_t size, const char *source_name,
                                br_node parent);
/* Re-reads a scene file into an existing root: its children are destroyed and rebuilt, the
 * root's own fields and components re-applied (what hot reload does). */
bool br_scene_file_reload(br_engine *e, const char *path, br_node root);
/* The subtree as JSON (live transforms and components, stored type data). Free with cJSON_Delete. */
cJSON *br_scene_file_to_json(br_engine *e, br_node root);
/* Writes the subtree as a scene file (path as for br_file_write_all). */
bool br_scene_file_save(br_engine *e, br_node root, const char *path);

/* ---- building blocks (scenes, prefabs and tools reuse these) */

/* Builds one node object and its children under parent; resolves label references at the end.
 * Handles { "prefab": ... } objects. Zero handle if the node was skipped. */
br_node br_scene_file_parse_node(br_engine *e, const cJSON *json, br_node parent);
/* Adds (or updates) component `name` on n from a { field: value } object. False if unknown. */
bool br_scene_file_apply_component(br_engine *e, br_node n, const char *name, const cJSON *fields);
/* Writes one JSON value into a component field. BR_FIELD_NODE takes a label (resolved later
 * during a load, at once against the stage otherwise). False (logged) on a type mismatch. */
bool br_scene_file_read_field(br_engine *e, br_node owner, uint32_t component_id, const br_field_desc *field,
                              const cJSON *value);
/* One component field as JSON (nodes by label). NULL if it cannot be written. */
cJSON *br_scene_file_write_field(br_engine *e, const br_field_desc *field, const void *component);

/* Called once per asset path a JSON tree references. */
typedef void (*br_asset_path_fn)(const char *path, void *user);
/* Lists the asset paths a scene/prefab JSON references ("assets", textures, fonts, prefabs,
 * BR_FIELD_ASSET component fields). Spritesheet frame references give the sheet path. */
void br_scene_file_collect_assets(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user);

/* Names an event handler so scene files can attach it: "on": { "click": "start_game" }. */
void br_scene_file_add_handler(br_engine *e, const char *name, br_event_listener_fn fn, void *user);

/* ---- hot reload (on by default in debug builds, off when NDEBUG is defined) */

/* Turns polling of loaded scene files on/off; poll_frames = frames between checks (0 = 30). */
void br_scene_file_set_hot_reload(br_engine *e, bool enabled, uint32_t poll_frames);
/* Checks every watched file now and reloads the changed ones. Returns how many reloaded. */
uint32_t br_scene_file_poll(br_engine *e);

/* ---- errors */

/* Errors reported since the engine started (loads, prefabs, game.json). */
uint32_t br_scene_file_error_count(br_engine *e);
/* The last error message ("" if none). */
const char *br_scene_file_last_error(br_engine *e);

/* ---- value helpers for from_json callbacks (your own node types use them too).
 * Each reads obj[key]: absent = false and *out untouched (no error); wrong type = false and an
 * error at the JSON path "<node path>.<key>". */

/* true / false. */
bool br_data_get_bool(br_engine *e, const cJSON *obj, const char *key, bool *out);
/* A whole number. */
bool br_data_get_int(br_engine *e, const cJSON *obj, const char *key, int *out);
/* A number. */
bool br_data_get_float(br_engine *e, const cJSON *obj, const char *key, float *out);
/* [x, y], { "x": .., "y": .. } or one number for both. */
bool br_data_get_vec2(br_engine *e, const cJSON *obj, const char *key, br_vec2 *out);
/* [x, y, z] or { "x", "y", "z" }. */
bool br_data_get_vec3(br_engine *e, const cJSON *obj, const char *key, br_vec3 *out);
/* "#rrggbb", "#rrggbbaa", CSS names ("red"), 0xRRGGBB numbers or [r, g, b(, a)] in 0..1. */
bool br_data_get_color(br_engine *e, const cJSON *obj, const char *key, br_color *out);
/* A string copied into out (truncation is an error). */
bool br_data_get_string(br_engine *e, const cJSON *obj, const char *key, char *out, size_t out_size);
/* A texture: "images/a.png" (asset path), "sheet.json#frame" (spritesheet frame), or a frame
 * name found in any loaded spritesheet. Missing ones log and give the magenta texture. */
bool br_data_get_texture(br_engine *e, const cJSON *obj, const char *key, br_texture *out);
/* [x, y, width, height]. */
bool br_data_get_rect(br_engine *e, const cJSON *obj, const char *key, br_rect *out);

/* One name of a string-valued enum. */
typedef struct br_data_enum {
    const char *name;
    int         value;
} br_data_enum;
/* A string from `table` (an error lists nothing more than the bad name). */
bool br_data_get_enum(br_engine *e, const cJSON *obj, const char *key, const br_data_enum *table, uint32_t count,
                      int *out);
/* The name of value in table, NULL if absent. */
const char *br_data_enum_name(const br_data_enum *table, uint32_t count, int value);
/* Resolves a texture reference string (see br_data_get_texture). */
br_texture br_data_texture(br_engine *e, const char *ref);
/* Reports an error at the current JSON path (for your from_json callbacks). */
void br_data_error(br_engine *e, const char *key, const char *message);

#endif
