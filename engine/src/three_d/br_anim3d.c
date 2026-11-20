/* br_anim3d: the Phase 1 glTF animator API (br_animator3d_*) on top of the unified animation
 * system, skinning maths, and the skinning pass ("gltf.animate").
 *
 * Playing a clip through br_animator3d_* gives the model3d node a unified br_animator holding the
 * model's clips (br_animator_add_model_clips); "animation.update" samples it like any other
 * animator. The legacy br_animator3d struct stays the user-facing state: br_anim3d_sync_in copies
 * the user's edits (speed, loop, paused, time) into the animator before sampling, sync_out copies
 * time and fade state back and fires the LOOP / END callbacks. "gltf.animate" (LATE_UPDATE 0, after
 * the animators) only computes joint matrices into the skin slots and syncs batched copies with
 * their group nodes (br_gltf_sync_batches).
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

/* ---- animator API */

static br_gltf_anim *br_anim3d_slot(br_engine *e, br_node3d n)
{
    br_gltf_instance *inst = br_gltf_instance_get(e, n);
    if (inst == NULL || inst->anim_index == UINT32_MAX) {
        return NULL;
    }
    return br_array_at(&e->gltf->anims, inst->anim_index);
}

/* The unified animator behind a legacy one (made with the model's clips on first use). */
static br_animator_data *br_anim3d_unified(br_engine *e, br_gltf_anim *slot, bool create)
{
    br_anim_target t = br_anim_target_3d(slot->instance->self);
    br_animator a = br_animator_get(e, t);
    if (a.generation == 0 && create) {
        a = br_animator_add(e, t);
        br_animator_add_model_clips(e, a, slot->instance->model);
    }
    br_animator_data *d = br_animator_data_get(e, a);
    if (d != NULL && create) {
        d->legacy.active = true;
    }
    return d;
}

static br_anim_loop br_anim3d_loop(const br_animator3d *a)
{
    return a->loop ? BR_ANIM_LOOP : BR_ANIM_CLAMP;
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
    br_animator_data *d = br_anim3d_unified(e, slot, true);
    if (d == NULL || clip >= d->clips.count) {
        return false;
    }
    a = br_animator3d_get(e, model_node);
    d->speed = a->speed;
    d->layers[0].queue_count = 0;
    br_animator_start(e, d, 0, (int32_t)clip, 0.0f);
    d->layers[0].cur.loop = br_anim3d_loop(a);
    d->layers[0].cur.time = a->time;
    d->legacy.time = a->time;
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
    br_animator_data *d = br_anim3d_unified(e, br_anim3d_slot(e, model_node), true);
    if (d == NULL) {
        return false;
    }
    a = br_animator3d_get(e, model_node);
    a->fade_clip = a->clip;
    a->fade_time = a->time;
    a->fade_elapsed = 0.0f;
    a->fade_duration = seconds;
    a->clip = index;
    a->time = 0.0f;
    d->layers[0].queue_count = 0;
    br_animator_start(e, d, 0, index, seconds);
    d->layers[0].cur.loop = br_anim3d_loop(a);
    d->layers[0].cur.time = 0.0f;
    d->legacy.time = 0.0f;
    return true;
}

void br_animator3d_stop(br_engine *e, br_node3d model_node)
{
    br_animator3d *a = br_animator3d_get(e, model_node);
    if (a == NULL) {
        return;
    }
    a->clip = -1;
    a->fade_clip = -1;
    br_animator_data *d = br_anim3d_unified(e, br_anim3d_slot(e, model_node), false);
    if (d != NULL) {
        d->layers[0].cur.active = false;
        d->layers[0].prev.active = false;
    }
}

uint32_t br_animator3d_count(br_engine *e)
{
    return e->gltf != NULL ? e->gltf->anims.count : 0;
}

/* ---- legacy state <-> unified animator */

void br_anim3d_sync_in(br_engine *e)
{
    if (e->gltf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < e->gltf->anims.count; i++) {
        br_gltf_anim *slot = br_array_at(&e->gltf->anims, i);
        if (!br_node3d_valid(e, slot->instance->self)) {
            continue;
        }
        br_animator_data *d = br_anim3d_unified(e, slot, false);
        if (d == NULL || !d->legacy.active) {
            continue;
        }
        const br_animator3d *a = &slot->animator;
        br_anim_layer *l = &d->layers[0];
        d->speed = a->speed;
        d->paused = a->paused;
        l->cur.loop = br_anim3d_loop(a);
        l->prev.loop = l->cur.loop;
        if (a->clip >= 0 && l->cur.active && a->time != d->legacy.time) {
            l->cur.time = a->time; /* the user moved the clock */
            l->cur.ended = false;
        }
    }
}

typedef struct br_anim3d_fired {
    br_anim3d_event_fn fn;
    void              *user;
    br_node3d          node;
    uint32_t           clip;
    br_anim3d_event    event;
} br_anim3d_fired;

void br_anim3d_sync_out(br_engine *e)
{
    if (e->gltf == NULL || e->gltf->anims.count == 0) {
        return;
    }
    br_anim3d_fired *fired = br_frame_alloc(e, sizeof(br_anim3d_fired) * e->gltf->anims.count);
    uint32_t fired_count = 0;
    for (uint32_t i = 0; i < e->gltf->anims.count; i++) {
        br_gltf_anim *slot = br_array_at(&e->gltf->anims, i);
        if (!br_node3d_valid(e, slot->instance->self)) {
            continue;
        }
        br_animator_data *d = br_anim3d_unified(e, slot, false);
        if (d == NULL || !d->legacy.active) {
            continue;
        }
        br_animator3d *a = &slot->animator;
        const br_anim_layer *l = &d->layers[0];
        if (l->cur.active) {
            a->time = l->cur.time;
        }
        d->legacy.time = a->time;
        if (l->prev.active) {
            a->fade_clip = l->prev.motion;
            a->fade_time = l->prev.time;
            a->fade_elapsed = l->fade_elapsed;
            a->fade_duration = l->fade_duration;
        } else {
            a->fade_clip = -1;
        }
        bool fire = a->clip >= 0 && l->cur.active && (l->cur.wrapped || l->cur.ended_now);
        if (fire && a->on_event != NULL && fired != NULL) {
            br_anim3d_fired *f = &fired[fired_count++];
            f->fn = a->on_event;
            f->user = a->event_user;
            f->node = slot->instance->self;
            f->clip = (uint32_t)a->clip;
            f->event = a->loop ? BR_ANIM3D_EVENT_LOOP : BR_ANIM3D_EVENT_END;
        }
    }
    /* callbacks may play clips or create instances (the pools may move): after the loop */
    for (uint32_t i = 0; i < fired_count; i++) {
        fired[i].fn(e, fired[i].node, fired[i].clip, fired[i].event, fired[i].user);
    }
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

