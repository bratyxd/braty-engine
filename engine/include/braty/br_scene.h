/* br_scene.h - scenes as first-class objects (Unity SceneManager shaped): load, additive load,
 * async load with progress, unload, active scene, transitions, persistent nodes, scene systems.
 *
 * A scene is a scene file (*.scene.json, docs/DATA_FORMATS.md) loaded into a 2D root node on
 * the stage, plus an asset group (br_asset_group.h) holding every asset the file references:
 * its "assets" list and every path its nodes, prefabs and components name. Loading a scene
 * loads them, unloading it releases them; assets shared with other scenes stay loaded.
 *
 *   br_scene title = br_scene_load(e, "title", BR_SCENE_SINGLE);       (name from game.json "scenes", or a path)
 *   br_scene_load(e, "scenes/hud.scene.json", BR_SCENE_ADDITIVE);       (on top, keeps the others)
 *   br_scene_switch(e, "level1", BR_TRANSITION_FADE, 0.4f);              (fade, load in background, fade in)
 *   br_node hud = br_node_find_by_label(e, br_scene_root_2d(e, title), "score", true);
 *
 * Ownership: a scene owns its root's subtree. Nodes you add to the stage itself while a scene is
 * active move into the active scene's 2D root at the start of the next frame (they draw with it
 * and die with it), unless br_node_set_persistent marked them. Nodes that were on the stage
 * before the first scene was loaded belong to no scene.
 *
 * Handles are like node handles: { index, generation }; a stale handle is ignored everywhere.
 * The older br_scene_file_* functions (br_scene_file.h) still work: they build a file under any
 * parent and nothing else (the subtree's root owns its asset references). Use br_scene for
 * screens and levels, br_scene_file / br_prefab for pieces inside them.
 */
#ifndef BR_SCENE_H
#define BR_SCENE_H

#include "braty/br_app.h"
#include "braty/br_asset_group.h"
#include "braty/br_node.h"
#include "braty/br_view3d.h"

typedef struct br_scene { uint32_t index; uint32_t generation; } br_scene;

typedef enum br_scene_mode {
    BR_SCENE_SINGLE,    /* unloads every other scene once this one is loaded, and makes it active */
    BR_SCENE_ADDITIVE   /* added on top of the loaded scenes */
} br_scene_mode;

typedef enum br_transition {
    BR_TRANSITION_NONE,
    BR_TRANSITION_FADE  /* to black, switch, back from black */
} br_transition;

typedef enum br_scene_event {
    BR_SCENE_EVENT_LOADED,     /* nodes built (the root exists) */
    BR_SCENE_EVENT_ACTIVATED,  /* became the active scene */
    BR_SCENE_EVENT_UNLOADING   /* about to be destroyed: nodes still valid */
} br_scene_event;

/* Called synchronously for every scene event (from the call that caused it, or from the
 * scenes.update system for async loads and transitions). */
typedef void (*br_scene_event_fn)(br_engine *e, br_scene scene, br_scene_event event, void *user);

/* Loads a scene now: its assets, then its nodes. `name` is a key of game.json "scenes" or a
 * scene file path. Zero handle if the file is missing or not JSON (logged). */
br_scene br_scene_load(br_engine *e, const char *name, br_scene_mode mode);
/* Starts loading in the background and returns at once; the nodes are built (and SINGLE unloads
 * the others) in the frame the last asset arrives. Poll br_scene_is_loaded / _progress. */
br_scene br_scene_load_async(br_engine *e, const char *name, br_scene_mode mode);
/* Destroys the scene's nodes and releases its assets. Safe while it is still loading (the loads
 * are cancelled or released as they finish). The handle turns stale. */
void br_scene_unload(br_engine *e, br_scene scene);
/* True if the handle refers to a scene that is loaded or loading. */
bool br_scene_valid(br_engine *e, br_scene scene);
/* True once the nodes are built. */
bool br_scene_is_loaded(br_engine *e, br_scene scene);
/* Share of its assets loaded, 0..1 (1 once loaded). */
float br_scene_progress(br_engine *e, br_scene scene);
/* Makes a loaded scene the active one (nodes added to the stage go into it). */
void br_scene_set_active(br_engine *e, br_scene scene);
/* The active scene (zero handle if none). */
br_scene br_scene_active(br_engine *e);
/* Name of the active scene, "" if none (play-test "current scene"). */
const char *br_scene_active_name(br_engine *e);
/* The scene's name: its game.json "scenes" key, else the file name without extensions. */
const char *br_scene_name(br_engine *e, br_scene scene);
/* The scene file path. */
const char *br_scene_path(br_engine *e, br_scene scene);
/* The first valid scene with this name (or path); zero handle if none. */
br_scene br_scene_find(br_engine *e, const char *name);
/* Number of scenes (loaded or loading), and the i-th in load order. */
uint32_t br_scene_count(br_engine *e);
br_scene br_scene_at(br_engine *e, uint32_t i);
/* The 2D root on the stage (zero handle while loading). The scene file's root is its child. */
br_node br_scene_root_2d(br_engine *e, br_scene scene);
/* The 3D root: the root of the first view3d in the scene, or of a full-window view3d created
 * now at the bottom of the scene. Zero handle while loading. */
br_node3d br_scene_root_3d(br_engine *e, br_scene scene);
/* The scene's asset group (owned by the scene; add lazy assets to it, read its paths). */
br_asset_group *br_scene_assets(br_engine *e, br_scene scene);
/* Adds a callback for scene events. */
void br_scene_on_event(br_engine *e, br_scene_event_fn fn, void *user);

/* Switches to another scene with a transition: fade to black over `seconds`, unload every scene
 * (persistent nodes stay), show the loading scene if one is set, load the target in the
 * background, fade in. Returns the target (loading). Ignored (zero handle) while another
 * switch runs. */
br_scene br_scene_switch(br_engine *e, const char *name, br_transition transition, float seconds);
/* Scene shown while br_scene_switch loads the next one (NULL = none). Its nodes can read
 * br_scene_progress(e, br_scene_switch_target(e)) for a progress bar. */
void br_scene_set_loading_scene(br_engine *e, const char *name);
/* The scene a running switch is loading, zero handle if no switch runs. */
br_scene br_scene_switch_target(br_engine *e);
/* True while a switch runs. */
bool br_scene_switching(br_engine *e);

/* Keeps n (and its subtree) alive across scene unloads (Unity DontDestroyOnLoad). A node inside
 * a scene moves to the stage, keeping its world transform. */
void br_node_set_persistent(br_engine *e, br_node n, bool persistent);
/* True if marked persistent. */
bool br_node_persistent(br_engine *e, br_node n);

/* br_system_add for a system that runs only while `scene` is loaded and stops for good when it
 * is unloaded. The name may be reused by a later scene. */
void br_system_add_for_scene(br_engine *e, br_scene scene, const char *name, br_phase phase, int priority,
                             br_system_fn fn, void *user);

/* Registers the module on an engine (called from br_builtin.c). */
void br_scenes_register(br_engine *e);

#endif
