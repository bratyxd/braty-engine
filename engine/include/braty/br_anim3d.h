/* br_anim3d.h - 3D animation: clips as flat keyframe arrays, sampling, skinning maths, animator.
 *
 * A clip is plain data (glTF animations are converted into it on load): channels, each with
 * times[] and values[] arrays, one interpolation, and a target (model node index + path). The
 * sampling and joint-matrix functions are pure, so tests and other modules can use them.
 *
 * The animator plays clips on one model instance (a "model3d" node from br_model_instantiate,
 * br_gltf.h). Every animator lives in one dense pool. Sampling, blending and writing node TRS and
 * morph weights run in the unified animation system ("animation.update", br_animation.h); the
 * "gltf.animate" system (LATE_UPDATE) then only computes joint matrices into the mesh3d skin
 * slots. Nothing is allocated per frame.
 */
#ifndef BR_ANIM3D_H
#define BR_ANIM3D_H

#include "braty/br_view3d.h"

/* What a channel animates on its target node. */
typedef enum br_anim3d_path {
    BR_ANIM3D_TRANSLATION,   /* 3 floats */
    BR_ANIM3D_ROTATION,      /* 4 floats: quaternion x, y, z, w */
    BR_ANIM3D_SCALE,         /* 3 floats */
    BR_ANIM3D_WEIGHTS        /* one float per morph target of the node's mesh */
} br_anim3d_path;

typedef enum br_anim3d_interpolation {
    BR_ANIM3D_LINEAR,        /* rotations: slerp along the shortest path */
    BR_ANIM3D_STEP,          /* the previous key's value */
    BR_ANIM3D_CUBIC_SPLINE   /* Hermite with in/out tangents (glTF CUBICSPLINE) */
} br_anim3d_interpolation;

/* One animated property. values holds key_count * components floats; for CUBIC_SPLINE it holds
 * key_count * 3 * components: per key the in-tangent, the value, the out-tangent (glTF order). */
typedef struct br_anim3d_channel {
    uint32_t                target;          /* model node index */
    br_anim3d_path          path;
    br_anim3d_interpolation interpolation;
    uint32_t                key_count;
    uint32_t                components;      /* floats per value */
    float                  *times;           /* seconds, ascending */
    float                  *values;
} br_anim3d_channel;

/* A named set of channels sharing one clock. */
typedef struct br_anim3d_clip {
    char              *name;                 /* "" if unnamed */
    float              duration;             /* last key time of all channels */
    br_anim3d_channel *channels;
    uint32_t           channel_count;
} br_anim3d_clip;

/* ---- pure maths */

/* Samples a channel at time t (clamped to its keys) into out[components]. Rotations come out
 * normalised. */
void br_anim3d_sample(const br_anim3d_channel *channel, float t, float *out);
/* Skinning: out[i] = inverse(mesh_world) * node_worlds[joints[i]] * inverse_binds[i] (glTF joint
 * matrices; what br_mesh3d.joint_matrices expects). Worlds may be relative to any common root. */
void br_anim3d_joint_matrices(br_mat4 mesh_world, const br_mat4 *node_worlds, const uint32_t *joints,
                              const br_mat4 *inverse_binds, uint32_t count, br_mat4 *out);
/* Translation, rotation and scale of an affine matrix (negative determinant flips scale x). */
void br_anim3d_decompose(br_mat4 m, br_vec3 *translation, br_quat *rotation, br_vec3 *scale);

/* ---- animator (one per model instance) */

typedef enum br_anim3d_event {
    BR_ANIM3D_EVENT_LOOP,    /* a looping clip wrapped around */
    BR_ANIM3D_EVENT_END      /* a non-looping clip reached its end (it holds the last pose) */
} br_anim3d_event;

/* Called from the animation system. model_node = the model3d node; clip = clip index. */
typedef void (*br_anim3d_event_fn)(br_engine *e, br_node3d model_node, uint32_t clip, br_anim3d_event event,
                                   void *user);

/* State of one animator: edit fields through br_animator3d_get. */
typedef struct br_animator3d {
    int32_t            clip;            /* playing clip index, -1 = none (rest pose) */
    float              time;            /* seconds into the clip */
    float              speed;           /* time scale (default 1; negative plays backwards) */
    bool               loop;            /* default true */
    bool               paused;
    int32_t            fade_clip;       /* clip being faded out, -1 = none */
    float              fade_time;       /* its time (it keeps running while it fades) */
    float              fade_elapsed;
    float              fade_duration;
    br_anim3d_event_fn on_event;        /* NULL = none */
    void              *event_user;
} br_animator3d;

/* The animator of a model3d node, NULL if n is not one. The pointer moves when animators are
 * created or destroyed: do not keep it. */
br_animator3d *br_animator3d_get(br_engine *e, br_node3d model_node);
/* Plays a clip by name from time 0 (cancels any fade). False if there is no such clip. */
bool br_animator3d_play(br_engine *e, br_node3d model_node, const char *clip);
/* Plays a clip by index from time 0. False if out of range. */
bool br_animator3d_play_index(br_engine *e, br_node3d model_node, uint32_t clip);
/* Starts a clip and blends from the current pose over `seconds` (the old clip keeps playing
 * while it fades out). Same clip as playing: nothing happens. */
bool br_animator3d_cross_fade(br_engine *e, br_node3d model_node, const char *clip, float seconds);
/* Stops playback; the nodes keep their current pose. */
void br_animator3d_stop(br_engine *e, br_node3d model_node);
/* Number of live animators (the size of the dense pool the system walks). */
uint32_t br_animator3d_count(br_engine *e);

#endif
