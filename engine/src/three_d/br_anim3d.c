/* br_anim3d: glTF clip sampling and skinning maths, and the skinning pass ("gltf.animate").
 *
 * Clips on a model instance are played by the unified animator (br_animator_add_model_clips);
 * "gltf.animate" (LATE_UPDATE 0, after the animators) only computes joint matrices into the skin
 * slots and syncs batched copies with their group nodes (br_gltf_sync_batches).
 */
#include "animation/br_animation_internal.h"
#include "three_d/br_gltf_internal.h"


/* ---- pure maths */

void br_anim3d_sample(const br_anim3d_channel *ch, float t, float *out)
{
    /* a view of the channel as a unified track: one sampler for every clip in the engine */
    br_anim_track track;
    SDL_zero(track);
    track.interpolation = BR_ANIM_LINEAR;
    if (ch->interpolation == BR_ANIM3D_STEP) {
        track.interpolation = BR_ANIM_STEP;
    } else if (ch->interpolation == BR_ANIM3D_CUBIC_SPLINE) {
        track.interpolation = BR_ANIM_CUBIC;
    }
    track.ease = BR_EASE_LINEAR;
    track.components = ch->components;
    track.key_count = ch->key_count;
    track.times = ch->times;
    track.values = ch->values;
    br_anim_track_sample(&track, t, ch->path == BR_ANIM3D_ROTATION, NULL, out);
}

void br_anim3d_joint_matrices(br_mat4 mesh_world, const br_mat4 *node_worlds, const uint32_t *joints,
                              const br_mat4 *inverse_binds, uint32_t count, br_mat4 *out)
{
    bool ok = true;
    br_mat4 inverse_mesh = br_mat4_invert(mesh_world, &ok);
    if (!ok) {
        inverse_mesh = br_mat4_identity();
    }
    for (uint32_t i = 0; i < count; i++) {
        out[i] = br_mat4_mul(inverse_mesh, br_mat4_mul(node_worlds[joints[i]], inverse_binds[i]));
    }
}

void br_anim3d_decompose(br_mat4 m, br_vec3 *translation, br_quat *rotation, br_vec3 *scale)
{
    const float *f = m.m; /* column-major: column c at f[c * 4] */
    *translation = br_vec3_make(f[12], f[13], f[14]);
    br_vec3 c0 = br_vec3_make(f[0], f[1], f[2]);
    br_vec3 c1 = br_vec3_make(f[4], f[5], f[6]);
    br_vec3 c2 = br_vec3_make(f[8], f[9], f[10]);
    float sx = br_vec3_length(c0);
    float sy = br_vec3_length(c1);
    float sz = br_vec3_length(c2);
    if (br_vec3_dot(br_vec3_cross(c0, c1), c2) < 0.0f) {
        sx = -sx; /* mirrored: put the flip on x */
    }
    *scale = br_vec3_make(sx, sy, sz);
    if (sx == 0.0f || sy == 0.0f || sz == 0.0f) {
        *rotation = br_quat_identity();
        return;
    }
    c0 = br_vec3_scale(c0, 1.0f / sx);
    c1 = br_vec3_scale(c1, 1.0f / sy);
    c2 = br_vec3_scale(c2, 1.0f / sz);
    /* rotation matrix r[row][col] -> quaternion (Shepperd: pick the largest diagonal term) */
    float r00 = c0.x, r10 = c0.y, r20 = c0.z;
    float r01 = c1.x, r11 = c1.y, r21 = c1.z;
    float r02 = c2.x, r12 = c2.y, r22 = c2.z;
    float trace = r00 + r11 + r22;
    br_quat q;
    if (trace > 0.0f) {
        float s = SDL_sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = SDL_sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = SDL_sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        float s = SDL_sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }
    *rotation = br_quat_normalize(q);
}

/* ---- the skinning pass */

/* Joint matrices of every skinned node, from the nodes' current local transforms. */
static void br_anim3d_skin(br_engine *e, br_gltf_instance *inst)
{
    const br_model *m = inst->model;
    if (inst->joint_total == 0) {
        return;
    }
    for (uint32_t k = 0; k < m->node_count; k++) {
        uint32_t i = inst->order[k];
        br_mat4 local = br_node3d_local_transform(e, inst->nodes[i]);
        int32_t p = m->nodes[i].parent;
        inst->model_world[i] = p >= 0 ? br_mat4_mul(inst->model_world[p], local) : local;
    }
    for (uint32_t i = 0; i < m->node_count; i++) {
        if (inst->joint_first[i] == UINT32_MAX) {
            continue;
        }
        const br_model_skin *skin = &m->skins[m->nodes[i].skin];
        br_anim3d_joint_matrices(inst->model_world[i], inst->model_world, skin->joints, skin->inverse_binds,
                                 skin->joint_count, inst->joints + inst->joint_first[i]);
    }
}

void br_anim3d_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_gltf_state *s = e->gltf;
    for (uint32_t idx = 0; idx < s->anims.count; idx++) {
        br_gltf_anim *slot = br_array_at(&s->anims, idx);
        if (br_node3d_valid(e, slot->instance->self)) {
            br_anim3d_skin(e, slot->instance); /* skins follow whatever moved the joints this frame */
            br_gltf_sync_batches(e, slot->instance);
        }
    }
}

