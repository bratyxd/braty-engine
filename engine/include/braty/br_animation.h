/* br_animation.h - one animation system for 2D and 3D: clips, property paths, animators with
 * layers, blend trees, events and state machines. Tweens: br_tween.h. Guide: docs/agents/animation.md.
 *
 *   const br_anim_library *lib = br_anim_library_load(e, "hero.anim.json");
 *   br_animator a = br_animator_add(e, br_anim_target_2d(hero));
 *   br_animator_add_library(e, a, lib);
 *   br_animator_play(e, a, "walk");
 *   br_animator_cross_fade(e, a, "idle", 0.25f);
 *
 * A clip is plain data: tracks of keyframes in flat arrays (times[], values[]). A track targets
 * a property path "<node path>:<property>" relative to the animator's node: "arm/hand:rotation",
 * ":alpha" (the node itself), "#3:rotation" (glTF node 3 of a model3d instance),
 * "hp.value" (component "hp", field "value"). Properties come from a registry of
 * (name, type, get, set) rows per node kind; modules add their own with br_anim_property_register.
 *
 * Every animator lives in one dense pool; one system ("animation.update", LATE_UPDATE -100)
 * advances, samples, blends and writes all of them. Tracks are bound to their targets once per
 * (animator, clip) into index arrays and remember their last key; nothing is allocated per frame.
 * A track whose node or property is missing, or whose node is destroyed later, is dropped with one
 * warning and the animator keeps playing the rest.
 */
#ifndef BR_ANIMATION_H
#define BR_ANIMATION_H

#include "braty/br_app.h"
#include "braty/br_ease.h"
#include "braty/br_gltf.h"
#include "braty/br_node.h"
#include "braty/br_sprites.h"
#include "braty/br_spritesheet.h"
#include "braty/br_view3d.h"

/* Registers the module (property registry, animator and tween systems, anim.json / animgraph.json
 * loaders, the "animator" and "tween" scene-file components). Called by br_builtin.c. */
void br_animation_register(br_engine *e);

#define BR_ANIM_MAX_COMPONENTS 8   /* floats per value (morph weights use 8) */
#define BR_ANIMATOR_MAX_LAYERS 4

/* ---- targets: a 2D or a 3D node */

typedef enum br_anim_target_kind {
    BR_ANIM_TARGET_NONE,
    BR_ANIM_TARGET_2D,       /* br_node */
    BR_ANIM_TARGET_3D        /* br_node3d */
} br_anim_target_kind;

typedef struct br_anim_target {
    br_anim_target_kind kind;
    uint32_t            index;
    uint32_t            generation;
} br_anim_target;

/* A 2D node as a target. */
br_anim_target br_anim_target_2d(br_node n);
/* A 3D node as a target. */
br_anim_target br_anim_target_3d(br_node3d n);
/* The 2D node of a target (zero handle if it is not 2D). */
br_node br_anim_target_node(br_anim_target t);
/* The 3D node of a target (zero handle if it is not 3D). */
br_node3d br_anim_target_node3d(br_anim_target t);
/* True if the target's node is alive. */
bool br_anim_target_valid(br_engine *e, br_anim_target t);
/* The node at `node_path` ("arm/hand", "#3", "" = root) below root; kind NONE if not found.
 * Each segment matches a direct child's label first, then any descendant's. */
br_anim_target br_anim_target_find(br_engine *e, br_anim_target root, const char *node_path);

/* ---- property values and the property registry */

typedef enum br_anim_value_type {
    BR_ANIM_FLOAT,           /* 1 float */
    BR_ANIM_VEC2,            /* 2 floats */
    BR_ANIM_VEC3,            /* 3 floats */
    BR_ANIM_COLOR,           /* r g b a (sRGB, straight alpha) */
    BR_ANIM_QUAT,            /* x y z w, blended with slerp */
    BR_ANIM_BOOL,            /* f[0] >= 0.5; not blended (the heavier contribution wins) */
    BR_ANIM_INT,             /* f[0] rounded; not blended */
    BR_ANIM_TEXTURE,         /* value.texture; not blended (track values index clip->textures) */
    BR_ANIM_WEIGHTS          /* up to 8 floats (morph weights), value.count of them used */
} br_anim_value_type;

/* A property value. Floats first; `count` = floats in use; `texture` for BR_ANIM_TEXTURE. */
typedef struct br_anim_value {
    float      f[BR_ANIM_MAX_COMPONENTS];
    uint32_t   count;
    br_texture texture;
} br_anim_value;

typedef void (*br_anim_get_fn)(br_engine *e, br_anim_target target, br_anim_value *out);
typedef void (*br_anim_set_fn)(br_engine *e, br_anim_target target, const br_anim_value *value);

/* One animatable property of a node kind. Setters must ignore nodes they do not apply to. */
typedef struct br_anim_property_desc {
    const char         *name;   /* "position", "material.roughness", "mymodule.glow" */
    br_anim_target_kind kind;   /* 2D or 3D nodes */
    br_anim_value_type  type;
    br_anim_get_fn      get;
    br_anim_set_fn      set;
} br_anim_property_desc;

/* Adds a property row (the name is copied); a row with the same kind and name is replaced.
 * Returns its id, UINT32_MAX on failure. */
uint32_t br_anim_property_register(br_engine *e, const br_anim_property_desc *desc);
/* Id of a property row, UINT32_MAX if none. */
uint32_t br_anim_property_find(br_engine *e, br_anim_target_kind kind, const char *name);
/* The row of an id, NULL if out of range. */
const br_anim_property_desc *br_anim_property_get_desc(br_engine *e, uint32_t id);
/* Number of rows (ids are 0..count-1). */
uint32_t br_anim_property_count(br_engine *e);
/* Floats a value of `type` uses (8 for weights). */
uint32_t br_anim_value_components(br_anim_value_type type);
/* Reads a property path ("arm:rotation", "hp.value") relative to root. False if it does not resolve. */
bool br_anim_get(br_engine *e, br_anim_target root, const char *path, br_anim_value *out);
/* Writes a property path relative to root. False if it does not resolve. */
bool br_anim_set(br_engine *e, br_anim_target root, const char *path, const br_anim_value *value);

/* ---- clips: tracks of flat keyframe arrays */

typedef enum br_anim_interpolation {
    BR_ANIM_STEP,            /* hold the previous key */
    BR_ANIM_LINEAR,          /* lerp (slerp for quaternions), u shaped by the track's ease */
    BR_ANIM_CUBIC,           /* Hermite, keys hold in-tangent, value, out-tangent (glTF CUBICSPLINE) */
    BR_ANIM_BEZIER           /* keys hold in-handle, value, out-handle (absolute values), u eased */
} br_anim_interpolation;

typedef enum br_anim_loop {
    BR_ANIM_LOOP,            /* wrap around */
    BR_ANIM_ONCE,            /* play to the end, then stop writing (the last pose stays) */
    BR_ANIM_PING_PONG,       /* forwards, backwards, ... */
    BR_ANIM_CLAMP            /* play to the end and keep holding it */
} br_anim_loop;

/* One animated property. values: key_count * components floats (STEP, LINEAR), or
 * key_count * 3 * components (CUBIC, BEZIER: per key in, value, out). */
typedef struct br_anim_track {
    char                 *path;          /* "<node path>:<property>" */
    br_anim_interpolation interpolation;
    br_ease_type          ease;          /* shapes u inside each segment (LINEAR, BEZIER) */
    uint32_t              components;
    uint32_t              key_count;
    const float          *times;         /* seconds, ascending */
    const float          *values;
    bool                  owns_keys;     /* false: times/values are borrowed (glTF clips) */
} br_anim_track;

/* A named moment in a clip; the animator reports it once each time playback passes it. */
typedef struct br_anim_marker {
    float time;
    char *name;
} br_anim_marker;

typedef struct br_anim_clip {
    char           *name;
    float           duration;      /* grows to the last key time as tracks are added */
    br_anim_loop    loop;          /* default loop mode when played */
    br_anim_track  *tracks;
    uint32_t        track_count;
    br_anim_marker *markers;
    uint32_t        marker_count;
    br_texture     *textures;      /* BR_ANIM_TEXTURE track values are indices into this */
    uint32_t        texture_count;
} br_anim_clip;

/* An empty clip (duration may be 0: the tracks set it). Free with br_anim_clip_destroy. */
br_anim_clip *br_anim_clip_create(const char *name, float duration, br_anim_loop loop);
/* Frees a clip, its tracks and markers (borrowed keys are not freed). NULL is fine. */
void br_anim_clip_destroy(br_anim_clip *clip);
/* Adds a track, copying times/values. Returns it (valid until the next add) or NULL. */
br_anim_track *br_anim_clip_add_track(br_anim_clip *clip, const char *path, br_anim_interpolation interpolation,
                                      uint32_t components, uint32_t key_count, const float *times,
                                      const float *values);
/* Same without copying: the arrays must outlive the clip. */
br_anim_track *br_anim_clip_add_track_borrowed(br_anim_clip *clip, const char *path,
                                               br_anim_interpolation interpolation, uint32_t components,
                                               uint32_t key_count, const float *times, const float *values);
/* Adds a marker (event) at `time` seconds. */
bool br_anim_clip_add_marker(br_anim_clip *clip, float time, const char *name);
/* Adds a texture to the clip's table and returns its index (for texture track values). */
uint32_t br_anim_clip_add_texture(br_anim_clip *clip, br_texture texture);
/* Samples a track at time t (clamped to its keys) into out[components]. key_hint (may be NULL)
 * caches the last segment between calls. quaternion: slerp + normalise (rotation tracks). */
void br_anim_track_sample(const br_anim_track *track, float t, bool quaternion, uint32_t *key_hint, float *out);

/* A flipbook clip: one STEP track ":sprite.texture" over the frames (durations in seconds). */
br_anim_clip *br_anim_clip_from_frames(const char *name, const br_anim_frame *frames, uint32_t count);
/* A flipbook clip from a spritesheet animation ("walk") at `fps` frames per second. NULL if none. */
br_anim_clip *br_anim_clip_from_spritesheet(br_engine *e, const br_spritesheet *sheet, const char *animation,
                                            float fps);
/* Clip `index` of a glTF model as a br_anim_clip. Keys are borrowed from the model (it must
 * outlive the clip); paths are "#<node>:position|rotation|scale|morph_weights". */
br_anim_clip *br_anim_clip_from_gltf(const br_model *model, uint32_t index);

/* ---- libraries: *.anim.json (format: docs/agents/animation.md) */

typedef struct br_anim_library {
    br_anim_clip **clips;
    uint32_t       count;
} br_anim_library;

/* Loads (or returns the cached) library asset ("anim", *.anim.json). NULL on failure (logged). */
const br_anim_library *br_anim_library_load(br_engine *e, const char *path);
/* Parses a library from JSON text (name is used in errors). Free with br_anim_library_free. */
br_anim_library *br_anim_library_parse(br_engine *e, const char *name, const char *json, size_t size);
/* Frees a library made by br_anim_library_parse. */
void br_anim_library_free(br_anim_library *lib);
/* A clip by name, NULL if none. */
const br_anim_clip *br_anim_library_find(const br_anim_library *lib, const char *name);

/* ---- animators */

typedef struct br_animator { uint32_t index; uint32_t generation; } br_animator;

/* Adds an animator to a node (or returns the one it has). Removed when the node is destroyed. */
br_animator br_animator_add(br_engine *e, br_anim_target root);
/* The animator of a node, zero handle if none. */
br_animator br_animator_get(br_engine *e, br_anim_target root);
/* Removes an animator (the nodes keep their current pose). */
void br_animator_remove(br_engine *e, br_animator a);
/* True if the handle is alive. */
bool br_animator_valid(br_engine *e, br_animator a);
/* Number of live animators (the dense pool the system walks). */
uint32_t br_animator_count(br_engine *e);
/* Binds again the tracks that were dropped (node or property missing, node destroyed), e.g.
 * after attaching a weapon node the clips animate. Returns how many tracks now resolve. */
uint32_t br_animator_rebind(br_engine *e, br_animator a);
/* Makes a clip playable by name (the clip must outlive the animator). */
bool br_animator_add_clip(br_engine *e, br_animator a, const br_anim_clip *clip);
/* Adds every clip of a library. Returns how many. */
uint32_t br_animator_add_library(br_engine *e, br_animator a, const br_anim_library *lib);
/* Adds every glTF clip of a model, in the model's order (converted once per model, keys shared).
 * Put the animator on the model3d node. Returns how many. */
uint32_t br_animator_add_model_clips(br_engine *e, br_animator a, const br_model *model);

/* Plays a clip or blend tree on the base layer from its start (cancels fades and the queue). */
bool br_animator_play(br_engine *e, br_animator a, const char *motion);
/* Starts a motion on the base layer and blends to it over `seconds` (the old one keeps playing).
 * The fade starts from the pose on screen: a fade that is still running, or the end pose of a
 * finished ONCE clip, is frozen and faded out. Nothing happens while `motion` is already playing
 * or fading in, so it can be called every frame. */
bool br_animator_cross_fade(br_engine *e, br_animator a, const char *motion, float seconds);
/* Plays `motion` after the current base-layer motion ends (or wraps), fading over fade_seconds. */
bool br_animator_queue(br_engine *e, br_animator a, const char *motion, float fade_seconds);
/* Stops every layer; the nodes keep their current pose. */
void br_animator_stop(br_engine *e, br_animator a);
/* Overall speed (default 1; negative plays backwards). */
void br_animator_set_speed(br_engine *e, br_animator a, float speed);
/* Pauses (the pose is still written) or resumes. */
void br_animator_set_paused(br_engine *e, br_animator a, bool paused);
/* Loop mode of the motion playing on a layer (default: the clip's). */
void br_animator_set_loop(br_engine *e, br_animator a, uint32_t layer, br_anim_loop loop);
/* Jumps the motion playing on a layer to `seconds` (blend trees: normalised 0..1). */
void br_animator_set_time(br_engine *e, br_animator a, uint32_t layer, float seconds);
/* Time of the motion playing on a layer (seconds; blend trees: normalised). */
float br_animator_time(br_engine *e, br_animator a, uint32_t layer);
/* Name of the motion playing on a layer, NULL if none. */
const char *br_animator_current(br_engine *e, br_animator a, uint32_t layer);
/* True when a ONCE/CLAMP motion on the layer has reached its end. */
bool br_animator_finished(br_engine *e, br_animator a, uint32_t layer);
/* Cross-fade progress 0..1 on a layer (1 = no fade running). */
float br_animator_fade_progress(br_engine *e, br_animator a, uint32_t layer);

/* ---- layers (0 = base layer; layers above blend over it in order) */

/* Configures layer 1..3: weight 0..1, additive (adds the clip's change from its first key) or
 * override. Layers between existing ones are created with weight 0. */
bool br_animator_set_layer(br_engine *e, br_animator a, uint32_t layer, float weight, bool additive);
/* Limits a layer to a node and its descendants ("Spine", "upper"); call again to add more. */
bool br_animator_layer_add_mask(br_engine *e, br_animator a, uint32_t layer, const char *node_path);
/* Plays a motion on a layer, fading over `seconds` (0 = at once). */
bool br_animator_play_layer(br_engine *e, br_animator a, uint32_t layer, const char *motion, float seconds);
/* Stops a layer's motion. */
void br_animator_stop_layer(br_engine *e, br_animator a, uint32_t layer);

/* ---- parameters and blend trees */

/* Sets a float parameter (created on first use; names up to 31 bytes, longer ones are reported
 * and ignored). Blend trees and state machines read them. A parameter's type comes from the
 * graph's "parameters", else from the first setter; a setter of another type sets the value and
 * warns once. */
void br_animator_set_float(br_engine *e, br_animator a, const char *name, float value);
/* Sets a bool parameter. */
void br_animator_set_bool(br_engine *e, br_animator a, const char *name, bool value);
/* Sets a trigger (a bool that a state-machine transition consumes, declared in the graph or not). */
void br_animator_set_trigger(br_engine *e, br_animator a, const char *name);
/* A parameter's value (bools/triggers as 0/1), 0 if unknown. */
float br_animator_get_float(br_engine *e, br_animator a, const char *name);
/* A 1D blend tree playable by name: clips placed at thresholds of `parameter` (up to 8). */
bool br_animator_add_blend1d(br_engine *e, br_animator a, const char *name, const char *parameter,
                             const char *const *clips, const float *thresholds, uint32_t count);
/* A 2D blend tree: clips at positions in (param_x, param_y) space, inverse-distance weights. */
bool br_animator_add_blend2d(br_engine *e, br_animator a, const char *name, const char *param_x,
                             const char *param_y, const char *const *clips, const br_vec2 *positions,
                             uint32_t count);
/* The weight of a blend tree's child clip this frame (tests, debugging); 0 if none. */
float br_animator_blend_weight(br_engine *e, br_animator a, const char *tree, uint32_t child);

/* ---- state machines (*.animgraph.json, format: docs/agents/animation.md) */

typedef struct br_anim_graph br_anim_graph;

/* Loads (or returns the cached) graph asset ("animgraph", *.animgraph.json). NULL on failure. */
const br_anim_graph *br_anim_graph_load(br_engine *e, const char *path);
/* Parses a graph from JSON text. Free with br_anim_graph_free. */
br_anim_graph *br_anim_graph_parse(br_engine *e, const char *name, const char *json, size_t size);
/* Frees a parsed graph. */
void br_anim_graph_free(br_anim_graph *graph);
/* Runs a state machine on the base layer (add the clips first; the graph must outlive the
 * animator). Starts in the entry state. Blend states become blend trees named after the state
 * (a clip of the same name does not replace them). NULL removes it. */
bool br_animator_set_graph(br_engine *e, br_animator a, const br_anim_graph *graph);
/* Name of the current state, NULL without a graph. */
const char *br_animator_state(br_engine *e, br_animator a);

/* ---- events */

/* A marker reached this frame. */
typedef struct br_anim_event {
    br_animator    animator;
    br_anim_target root;       /* the animator's node */
    const char    *name;       /* the marker name (valid until the next animation update) */
    const char    *clip;
    float          time;       /* the marker time in the clip */
} br_anim_event;

typedef void (*br_anim_event_fn)(br_engine *e, const br_anim_event *event, void *user);

/* Events of the last animation update (any system after LATE_UPDATE -100 can read them). */
const br_anim_event *br_animation_events(br_engine *e, uint32_t *count);
/* Calls fn for each of the animator's events, after all animators were sampled. NULL removes it. */
void br_animator_set_event_callback(br_engine *e, br_animator a, br_anim_event_fn fn, void *user);

/* ---- root motion */

/* Extracts root motion from the position track of node_path ("" = the root, "#0", "Hips"): the
 * animator stops writing that node's horizontal position (2D: x and y, 3D: x and z) and reports
 * the track's movement with br_animator_root_motion, for the game to apply to the node itself or
 * to a parent. Base layer only; a clip change, a set_time or a loop wrap adds only the clip's own
 * movement. NULL turns it off. */
bool br_animator_set_root_motion(br_engine *e, br_animator a, const char *node_path);
/* The root movement extracted in the last update (in the root node's parent space). */
br_vec3 br_animator_root_motion(br_engine *e, br_animator a);

/* ---- statistics */

/* Animators sampled and tracks written in the last update. */
void br_animation_stats(br_engine *e, uint32_t *animators, uint32_t *tracks);

#endif
