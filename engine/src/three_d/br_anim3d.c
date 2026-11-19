/* br_anim3d: keyframe sampling, skinning maths, the animator API and the animation system. */
#include "three_d/br_gltf_internal.h"

#include <SDL3/SDL_timer.h>

/* ---- pure maths */

static void br_anim3d_normalize4(float *q)
{
    float len = SDL_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 0.0f) {
        for (int i = 0; i < 4; i++) {
            q[i] /= len;
        }
    }
}

void br_anim3d_sample(const br_anim3d_channel *ch, float t, float *out)
{
    uint32_t n = ch->key_count;
    uint32_t c = ch->components;
    if (n == 0 || c == 0) {
        return;
    }
    bool cubic = ch->interpolation == BR_ANIM3D_CUBIC_SPLINE;
    uint32_t stride = cubic ? c * 3 : c; /* floats per key */
    uint32_t value = cubic ? c : 0;      /* the value inside a key (after the in-tangent) */
    bool rotation = ch->path == BR_ANIM3D_ROTATION;

    if (n == 1 || t <= ch->times[0] || t >= ch->times[n - 1]) {
        uint32_t k = (n > 1 && t >= ch->times[n - 1]) ? n - 1 : 0;
        SDL_memcpy(out, ch->values + k * stride + value, sizeof(float) * c);
        if (rotation) {
            br_anim3d_normalize4(out);
        }
        return;
    }
    /* binary search: times[lo] <= t < times[hi], hi = lo + 1 */
    uint32_t lo = 0;
    uint32_t hi = n - 1;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (ch->times[mid] <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    float dt = ch->times[hi] - ch->times[lo];
    float u = dt > 0.0f ? (t - ch->times[lo]) / dt : 0.0f;
    const float *a = ch->values + lo * stride;
    const float *b = ch->values + hi * stride;

    if (ch->interpolation == BR_ANIM3D_STEP) {
        SDL_memcpy(out, a + value, sizeof(float) * c);
    } else if (cubic) {
        /* Hermite: p0 = value(lo), m0 = out-tangent(lo) * dt, p1 = value(hi), m1 = in-tangent(hi) * dt */
        float u2 = u * u;
        float u3 = u2 * u;
        float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        float h10 = u3 - 2.0f * u2 + u;
        float h01 = -2.0f * u3 + 3.0f * u2;
        float h11 = u3 - u2;
        for (uint32_t i = 0; i < c; i++) {
            float p0 = a[c + i];
            float m0 = a[2 * c + i] * dt;
            float p1 = b[c + i];
            float m1 = b[i] * dt;
            out[i] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }
    } else if (rotation) {
        br_quat qa = { a[0], a[1], a[2], a[3] };
        br_quat qb = { b[0], b[1], b[2], b[3] };
        br_quat q = br_quat_slerp(qa, qb, u); /* takes the short way round */
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    } else {
        for (uint32_t i = 0; i < c; i++) {
            out[i] = a[i] + (b[i] - a[i]) * u;
        }
    }
    if (rotation) {
        br_anim3d_normalize4(out);
    }
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

/* ---- animator API */

static br_gltf_anim *br_anim3d_slot(br_engine *e, br_node3d n)
{
    br_gltf_instance *inst = br_gltf_instance_get(e, n);
    if (inst == NULL || inst->anim_index == UINT32_MAX) {
        return NULL;
    }
    return br_array_at(&e->gltf->anims, inst->anim_index);
}

br_animator3d *br_animator3d_get(br_engine *e, br_node3d model_node)
{
    br_gltf_anim *slot = br_anim3d_slot(e, model_node);
    return slot != NULL ? &slot->animator : NULL;
}

bool br_animator3d_play_index(br_engine *e, br_node3d model_node, uint32_t clip)
{
    br_gltf_anim *slot = br_anim3d_slot(e, model_node);
    if (slot == NULL || clip >= slot->instance->model->clip_count) {
        return false;
    }
    br_animator3d *a = &slot->animator;
    a->clip = (int32_t)clip;
    a->time = a->speed < 0.0f ? slot->instance->model->clips[clip].duration : 0.0f;
    a->fade_clip = -1;
    return true;
}

static int32_t br_anim3d_find(br_engine *e, br_node3d model_node, const char *clip)
{
    br_gltf_anim *slot = br_anim3d_slot(e, model_node);
    if (slot == NULL) {
        br_log_error("animator3d: node is not a model instance");
        return -1;
    }
    int32_t index = br_model_find_clip(slot->instance->model, clip);
    if (index < 0) {
        br_log_error("animator3d: no clip named '%s'", clip != NULL ? clip : "(null)");
    }
    return index;
}

bool br_animator3d_play(br_engine *e, br_node3d model_node, const char *clip)
{
    int32_t index = br_anim3d_find(e, model_node, clip);
    return index >= 0 && br_animator3d_play_index(e, model_node, (uint32_t)index);
}

bool br_animator3d_cross_fade(br_engine *e, br_node3d model_node, const char *clip, float seconds)
{
    int32_t index = br_anim3d_find(e, model_node, clip);
    if (index < 0) {
        return false;
    }
    br_animator3d *a = br_animator3d_get(e, model_node);
    if (a->clip == index) {
        return true;
    }
    if (a->clip < 0 || seconds <= 0.0f) {
        return br_animator3d_play_index(e, model_node, (uint32_t)index);
    }
    a->fade_clip = a->clip;
    a->fade_time = a->time;
    a->fade_elapsed = 0.0f;
    a->fade_duration = seconds;
    a->clip = index;
    a->time = 0.0f;
    return true;
}

void br_animator3d_stop(br_engine *e, br_node3d model_node)
{
    br_animator3d *a = br_animator3d_get(e, model_node);
    if (a != NULL) {
        a->clip = -1;
        a->fade_clip = -1;
    }
}

uint32_t br_animator3d_count(br_engine *e)
{
    return e->gltf != NULL ? e->gltf->anims.count : 0;
}

/* ---- the system */

/* Moves a clip clock; returns true when it wrapped (loop) or reached an end (once). */
static bool br_anim3d_tick(float *time, float step, float duration, bool loop)
{
    if (duration <= 0.0f) {
        *time = 0.0f;
        return false;
    }
    float before = *time;
    float t = before + step;
    if (loop) {
        bool wrapped = t >= duration || t < 0.0f;
        t = SDL_fmodf(t, duration);
        if (t < 0.0f) {
            t += duration;
        }
        *time = t;
        return wrapped;
    }
    t = br_clampf(t, 0.0f, duration);
    *time = t;
    return t != before && (t == duration || t == 0.0f);
}

static void br_anim3d_rest(const br_model *m, br_gltf_pose *pose)
{
    for (uint32_t i = 0; i < m->node_count; i++) {
        const br_model_node *node = &m->nodes[i];
        pose->translation[i] = node->translation;
        pose->rotation[i] = node->rotation;
        pose->scale[i] = node->scale;
        SDL_memcpy(pose->weights + i * BR_MESH3D_MAX_MORPH_WEIGHTS, node->weights, sizeof(node->weights));
    }
}

static void br_anim3d_sample_clip(const br_anim3d_clip *clip, float t, br_gltf_pose *pose)
{
    float v[BR_MESH3D_MAX_MORPH_WEIGHTS];
    for (uint32_t c = 0; c < clip->channel_count; c++) {
        const br_anim3d_channel *ch = &clip->channels[c];
        uint32_t i = ch->target;
        br_anim3d_sample(ch, t, v);
        if (ch->path == BR_ANIM3D_TRANSLATION) {
            pose->translation[i] = br_vec3_make(v[0], v[1], v[2]);
        } else if (ch->path == BR_ANIM3D_ROTATION) {
            br_quat q = { v[0], v[1], v[2], v[3] };
            pose->rotation[i] = q;
        } else if (ch->path == BR_ANIM3D_SCALE) {
            pose->scale[i] = br_vec3_make(v[0], v[1], v[2]);
        } else {
            SDL_memcpy(pose->weights + i * BR_MESH3D_MAX_MORPH_WEIGHTS, v, sizeof(float) * ch->components);
        }
    }
}

/* pose = blend(from, pose, w): w = 0 is `from`, w = 1 keeps `pose`. */
static void br_anim3d_blend(uint32_t node_count, const br_gltf_pose *from, br_gltf_pose *pose, float w)
{
    for (uint32_t i = 0; i < node_count; i++) {
        pose->translation[i] = br_vec3_lerp(from->translation[i], pose->translation[i], w);
        pose->rotation[i] = br_quat_slerp(from->rotation[i], pose->rotation[i], w);
        pose->scale[i] = br_vec3_lerp(from->scale[i], pose->scale[i], w);
    }
    for (uint32_t i = 0; i < node_count * BR_MESH3D_MAX_MORPH_WEIGHTS; i++) {
        pose->weights[i] = br_lerpf(from->weights[i], pose->weights[i], w);
    }
}

/* Writes the channels' targets of a clip from the pose into the 3D nodes / mesh morph slots. */
static void br_anim3d_write(br_engine *e, br_gltf_instance *inst, const br_anim3d_clip *clip)
{
    const br_model *m = inst->model;
    for (uint32_t c = 0; c < clip->channel_count; c++) {
        const br_anim3d_channel *ch = &clip->channels[c];
        uint32_t i = ch->target;
        br_node3d n = inst->nodes[i];
        if (ch->path == BR_ANIM3D_TRANSLATION) {
            br_node3d_set_position(e, n, inst->pose.translation[i]);
        } else if (ch->path == BR_ANIM3D_ROTATION) {
            br_node3d_set_rotation(e, n, inst->pose.rotation[i]);
        } else if (ch->path == BR_ANIM3D_SCALE) {
            br_node3d_set_scale(e, n, inst->pose.scale[i]);
        } else {
            for (uint32_t p = 0; p < inst->prim_count[i]; p++) {
                br_mesh3d *mesh = br_mesh3d_get(e, inst->prim_nodes[inst->prim_first[i] + p]);
                if (mesh != NULL) {
                    SDL_memcpy(mesh->morph_weights, inst->pose.weights + i * BR_MESH3D_MAX_MORPH_WEIGHTS,
                               sizeof(mesh->morph_weights));
                    mesh->morph_weight_count = m->nodes[i].weight_count;
                }
            }
        }
    }
}

/* Joint matrices of every skinned node, from the nodes' current local transforms. */
static void br_anim3d_skin(br_engine *e, br_gltf_instance *inst)
{
    const br_model *m = inst->model;
    if (inst->joint_total == 0) {
        return;
    }
    for (uint32_t k = 0; k < m->node_count; k++) {
        uint32_t i = inst->order[k];
        br_mat4 local = br_node3d_local_matrix(e, inst->nodes[i]);
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
    (void)user;
    br_gltf_state *s = e->gltf;
    uint64_t start = SDL_GetTicksNS();
    for (uint32_t idx = 0; idx < s->anims.count; idx++) {
        br_gltf_anim *slot = br_array_at(&s->anims, idx);
        br_gltf_instance *inst = slot->instance;
        if (!br_node3d_valid(e, inst->self)) {
            continue; /* destroyed this frame, freed at the next flush */
        }
        const br_model *m = inst->model;
        br_animator3d *a = &slot->animator;
        bool fire = false;
        br_anim3d_event event = BR_ANIM3D_EVENT_LOOP;
        int32_t event_clip = a->clip;
        if (!a->paused && a->clip >= 0) {
            float step = dt * a->speed;
            fire = br_anim3d_tick(&a->time, step, m->clips[a->clip].duration, a->loop);
            event = a->loop ? BR_ANIM3D_EVENT_LOOP : BR_ANIM3D_EVENT_END;
            if (a->fade_clip >= 0) {
                br_anim3d_tick(&a->fade_time, step, m->clips[a->fade_clip].duration, a->loop);
                a->fade_elapsed += dt;
                if (a->fade_elapsed >= a->fade_duration) {
                    a->fade_clip = -1;
                }
            }
        }
        if (a->clip >= 0 || a->fade_clip >= 0) {
            br_anim3d_rest(m, &inst->pose);
            if (a->clip >= 0) {
                br_anim3d_sample_clip(&m->clips[a->clip], a->time, &inst->pose);
            }
            if (a->fade_clip >= 0) {
                br_anim3d_rest(m, &inst->fade_pose);
                br_anim3d_sample_clip(&m->clips[a->fade_clip], a->fade_time, &inst->fade_pose);
                br_anim3d_blend(m->node_count, &inst->fade_pose, &inst->pose, a->fade_elapsed / a->fade_duration);
                br_anim3d_write(e, inst, &m->clips[a->fade_clip]);
            }
            if (a->clip >= 0) {
                br_anim3d_write(e, inst, &m->clips[a->clip]);
            }
        }
        br_anim3d_skin(e, inst);
        if (fire && a->on_event != NULL) {
            /* the callback may play clips or create instances (the pool may move): re-fetch after */
            a->on_event(e, inst->self, (uint32_t)event_clip, event, a->event_user);
        }
    }
    s->animate_ns = SDL_GetTicksNS() - start;
}
