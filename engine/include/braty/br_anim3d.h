/* br_anim3d.h - glTF clip data as flat keyframe arrays, sampling and skinning maths.
 *
 * A clip is plain data (glTF animations are converted into it on load): channels, each with
 * times[] and values[] arrays, one interpolation, and a target (model node index + path). The
 * sampling and joint-matrix functions are pure, so tests and other modules can use them.
 *
 * Playing clips on a model instance is the unified animator's job (br_animation.h):
 *   br_animator a = br_animator_add(e, br_anim_target_3d(instance));
 *   br_animator_add_model_clips(e, a, br_model3d_model(e, instance));
 *   br_animator_play(e, a, "Run");
 * The "gltf.animate" system (LATE_UPDATE) then computes joint matrices into the mesh3d skin slots.
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

#endif
