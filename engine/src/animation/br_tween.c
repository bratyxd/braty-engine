/* br_tween: tweens on property paths in one dense array, advanced by one system; sequences are a
 * time cursor that turns "after" / "with" into start delays. */
#include "animation/br_animation_internal.h"

static br_tween_data *br_tween_data_get(br_engine *e, br_tween t)
{
    br_animation_state *s = e->animation;
    uint32_t *dense = s != NULL ? br_pool_get(&s->tween_handles, t.index, t.generation) : NULL;
    if (dense == NULL) {
        return NULL;
    }
    br_tween_data *d = br_array_at(&s->tweens, *dense);
    return d->dead ? NULL : d;
}

static br_tween br_tween_push(br_engine *e, br_tween_data *d)
{
    br_animation_state *s = e->animation;
    br_tween none = { 0, 0 };
    uint32_t *dense = br_pool_alloc(&s->tween_handles, &d->handle, &d->handle_generation);
    if (dense == NULL) {
        return none;
    }
    *dense = s->tweens.count;
    if (br_array_push(&s->tweens, d) == NULL) {
        br_pool_release(&s->tween_handles, d->handle, d->handle_generation);
        return none;
    }
    br_tween t = { d->handle, d->handle_generation };
    return t;
}

static br_tween br_tween_make(br_engine *e, br_anim_target target, const char *property, const float *from,
                              const float *to, float seconds, br_ease_type ease)
{
    br_tween none = { 0, 0 };
    br_tween_data d;
    SDL_zero(d);
    if (e->animation == NULL || property == NULL || !br_anim_resolve(e, target, property, &d.target, &d.prop)) {
        br_log_error("tween: '%s' does not resolve on this node (no such node or property)",
                     property != NULL ? property : "(null)");
        return none;
    }
    if (d.prop.type == BR_ANIM_TEXTURE) {
        br_log_error("tween: '%s' is a texture; animate it with a clip", property);
        return none;
    }
    d.owner = target;
    d.count = br_anim_value_components(d.prop.type);
    d.set = d.prop.property != BR_ANIM_NONE ? br_anim_property_desc_get(e, d.prop.property)->set : NULL;
    if (from != NULL) {
        SDL_memcpy(d.from, from, sizeof(float) * d.count);
        d.has_from = true;
    }
    if (to != NULL) {
        SDL_memcpy(d.to, to, sizeof(float) * d.count);
        d.has_to = true;
    }
    d.duration = SDL_max(seconds, 0.0f);
    d.ease = ease;
    return br_tween_push(e, &d);
}

br_tween br_tween_to(br_engine *e, br_anim_target target, const char *property, const float *to, float seconds,
                     br_ease_type ease)
{
    return br_tween_make(e, target, property, NULL, to, seconds, ease);
}

br_tween br_tween_from(br_engine *e, br_anim_target target, const char *property, const float *from, float seconds,
                       br_ease_type ease)
{
    return br_tween_make(e, target, property, from, NULL, seconds, ease);
}

br_tween br_tween_from_to(br_engine *e, br_anim_target target, const char *property, const float *from,
                          const float *to, float seconds, br_ease_type ease)
{
    return br_tween_make(e, target, property, from, to, seconds, ease);
}

br_tween br_tween_to_f(br_engine *e, br_anim_target target, const char *property, float to, float seconds,
                       br_ease_type ease)
{
    float v[BR_ANIM_MAX_COMPONENTS] = { to };
    return br_tween_make(e, target, property, NULL, v, seconds, ease);
}

br_tween br_tween_delay_call(br_engine *e, float seconds, br_tween_fn fn, void *user)
{
    br_tween none = { 0, 0 };
    if (e->animation == NULL) {
        return none;
    }
    br_tween_data d;
    SDL_zero(d);
    d.delay = SDL_max(seconds, 0.0f);
    d.on_complete = fn;
    d.user = user;
    return br_tween_push(e, &d);
}

void br_tween_set_delay(br_engine *e, br_tween t, float seconds)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL && !d->started) {
        d->delay += SDL_max(seconds, 0.0f);
    }
}

void br_tween_set_repeat(br_engine *e, br_tween t, int32_t count, bool yoyo)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->repeat = count < 0 ? -1 : count;
        d->yoyo = yoyo;
    }
}

void br_tween_set_ease(br_engine *e, br_tween t, br_ease_type ease)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->ease = ease;
    }
}

bool br_tween_set_ease_name(br_engine *e, br_tween t, const char *name)
{
    bool ok = false;
    br_ease_type ease = br_ease_from_name(name, &ok);
    if (!ok) {
        br_log_error("tween: unknown easing '%s' (linear used)", name != NULL ? name : "(null)");
    }
    br_tween_set_ease(e, t, ease);
    return ok;
}

void br_tween_set_on_complete(br_engine *e, br_tween t, br_tween_fn fn, void *user)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->on_complete = fn;
        d->user = user;
    }
}

void br_tween_set_tag(br_engine *e, br_tween t, uint32_t tag)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->tag = tag;
    }
}

void br_tween_set_unscaled(br_engine *e, br_tween t, bool unscaled)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->unscaled = unscaled;
    }
}

bool br_tween_unscaled(br_engine *e, br_tween t)
{
    br_tween_data *d = br_tween_data_get(e, t);
    return d != NULL && d->unscaled;
}

void br_tween_set_paused(br_engine *e, br_tween t, bool paused)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        d->paused = paused;
    }
}

bool br_tween_paused(br_engine *e, br_tween t)
{
    br_tween_data *d = br_tween_data_get(e, t);
    return d != NULL && d->paused;
}

/* ---- removal */

static void br_tween_remove_at(br_engine *e, uint32_t i)
{
    br_animation_state *s = e->animation;
    br_tween_data *d = br_array_at(&s->tweens, i);
    br_pool_release(&s->tween_handles, d->handle, d->handle_generation);
    uint32_t last = s->tweens.count - 1;
    if (i != last) {
        br_tween_data *moved = br_array_at(&s->tweens, last);
        *(uint32_t *)br_pool_slot_data(&s->tween_handles, moved->handle) = i;
    }
    br_array_remove_swap(&s->tweens, i);
}

/* Marks a tween dead; outside the system it is removed at once. */
static void br_tween_kill_data(br_engine *e, br_tween_data *d)
{
    br_animation_state *s = e->animation;
    d->dead = true;
    if (!s->updating) {
        br_tween_remove_at(e, (uint32_t)(d - (br_tween_data *)br_array_at(&s->tweens, 0)));
    }
}

void br_tween_kill(br_engine *e, br_tween t)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d != NULL) {
        br_tween_kill_data(e, d);
    }
}

uint32_t br_tween_kill_target(br_engine *e, br_anim_target target)
{
    br_animation_state *s = e->animation;
    uint32_t n = 0;
    for (uint32_t i = s != NULL ? s->tweens.count : 0; i > 0; i--) {
        br_tween_data *d = br_array_at(&s->tweens, i - 1);
        bool hit = br_anim_target_equal(d->target, target) || br_anim_target_equal(d->owner, target);
        if (!d->dead && hit) {
            br_tween_kill_data(e, d);
            n++;
        }
    }
    return n;
}

uint32_t br_tween_kill_tag(br_engine *e, uint32_t tag)
{
    br_animation_state *s = e->animation;
    uint32_t n = 0;
    for (uint32_t i = s != NULL ? s->tweens.count : 0; tag != 0 && i > 0; i--) {
        br_tween_data *d = br_array_at(&s->tweens, i - 1);
        if (!d->dead && d->tag == tag) {
            br_tween_kill_data(e, d);
            n++;
        }
    }
    return n;
}

bool br_tween_alive(br_engine *e, br_tween t)
{
    return br_tween_data_get(e, t) != NULL;
}

uint32_t br_tween_count(br_engine *e)
{
    br_animation_state *s = e->animation;
    uint32_t n = 0;
    for (uint32_t i = 0; s != NULL && i < s->tweens.count; i++) {
        n += ((br_tween_data *)br_array_at(&s->tweens, i))->dead ? 0 : 1;
    }
    return n;
}

void br_tween_free_all(br_engine *e)
{
    br_animation_state *s = e->animation;
    while (s->tweens.count > 0) {
        br_tween_remove_at(e, s->tweens.count - 1);
    }
}

/* ---- sequences */

br_tween_sequence br_tween_sequence_begin(uint32_t tag)
{
    br_tween_sequence seq = { 0.0f, 0.0f, tag };
    return seq;
}

/* A tween's own length: its delay plus every pass (an endless one counts one pass). */
static float br_tween_length(const br_tween_data *d)
{
    float passes = d->repeat > 0 ? (float)(d->repeat + 1) : 1.0f;
    return d->delay + d->duration * passes;
}

void br_tween_sequence_add(br_engine *e, br_tween_sequence *seq, br_tween t)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d == NULL || seq == NULL) {
        return;
    }
    float length = br_tween_length(d);
    d->delay += seq->cursor;
    d->tag = seq->tag != 0 ? seq->tag : d->tag;
    seq->group_start = seq->cursor;
    seq->cursor += length;
}

void br_tween_sequence_join(br_engine *e, br_tween_sequence *seq, br_tween t)
{
    br_tween_data *d = br_tween_data_get(e, t);
    if (d == NULL || seq == NULL) {
        return;
    }
    float length = br_tween_length(d);
    d->delay += seq->group_start;
    d->tag = seq->tag != 0 ? seq->tag : d->tag;
    seq->cursor = SDL_max(seq->cursor, seq->group_start + length);
}

void br_tween_sequence_wait(br_tween_sequence *seq, float seconds)
{
    if (seq != NULL) {
        seq->cursor += SDL_max(seconds, 0.0f);
    }
}

br_tween br_tween_sequence_call(br_engine *e, br_tween_sequence *seq, br_tween_fn fn, void *user)
{
    br_tween t = br_tween_delay_call(e, seq != NULL ? seq->cursor : 0.0f, fn, user);
    br_tween_set_tag(e, t, seq != NULL ? seq->tag : 0);
    return t;
}

/* ---- the system */

/* Applies the tween at its current pass and time. */
static void br_tween_apply(br_engine *e, br_tween_data *d)
{
    float u = d->duration > 0.0f ? br_clamp01f(d->elapsed / d->duration) : 1.0f;
    if (d->yoyo && (d->pass & 1u) != 0) {
        u = 1.0f - u;
    }
    float k = br_ease_apply(d->ease, u);
    br_anim_value v;
    SDL_zero(v);
    v.count = d->count;
    if (d->prop.type == BR_ANIM_QUAT) {
        br_quat a = { d->from[0], d->from[1], d->from[2], d->from[3] };
        br_quat b = { d->to[0], d->to[1], d->to[2], d->to[3] };
        br_quat q = br_quat_slerp(a, b, k);
        v.f[0] = q.x;
        v.f[1] = q.y;
        v.f[2] = q.z;
        v.f[3] = q.w;
    } else {
        for (uint32_t i = 0; i < d->count; i++) {
            v.f[i] = d->from[i] + (d->to[i] - d->from[i]) * k;
        }
    }
    if (d->set != NULL) {
        d->set(e, d->target, &v);
    } else {
        br_anim_prop_set(e, d->target, &d->prop, &v); /* component field */
    }
}

void br_tween_system(br_engine *e, float dt, void *user)
{
    (void)user;
    br_animation_state *s = e->animation;
    uint32_t count = s->tweens.count; /* tweens made by callbacks start next frame */
    /* targets are re-checked only after some node was destroyed */
    bool check_targets = s->tween_epoch != s->destroy_epoch;
    s->tween_epoch = s->destroy_epoch;
    s->updating = true;
    float unscaled = br_ticker_unscaled_delta(e);
    for (uint32_t i = 0; i < count; i++) {
        br_tween_data *d = br_array_at(&s->tweens, i);
        if (d->dead) {
            continue;
        }
        float frame_dt = d->unscaled ? unscaled : dt;
        bool has_target = d->target.kind != BR_ANIM_TARGET_NONE;
        if (has_target && check_targets && !br_anim_target_valid(e, d->target)) {
            d->dead = true; /* the node was destroyed: the tween goes with it */
            continue;
        }
        if (d->paused) {
            continue;
        }
        float step = frame_dt;
        if (d->delay > 0.0f) {
            d->delay -= frame_dt;
            if (d->delay > 0.0f) {
                continue;
            }
            step = -d->delay;
            d->delay = 0.0f;
        }
        if (has_target && !d->started) {
            br_anim_value cur;
            br_anim_prop_get(e, d->target, &d->prop, &cur);
            if (!d->has_from) {
                SDL_memcpy(d->from, cur.f, sizeof(float) * d->count);
            }
            if (!d->has_to) {
                SDL_memcpy(d->to, cur.f, sizeof(float) * d->count);
            }
        }
        d->started = true;
        d->elapsed += step;
        bool done = false;
        if (d->elapsed >= d->duration) {
            float whole = d->duration > 0.0f ? SDL_floorf(d->elapsed / d->duration) : 1.0f;
            uint32_t passes = (uint32_t)SDL_min(whole, 1.0e6f);
            if (d->repeat < 0 && d->duration > 0.0f) {
                d->pass += passes;
                d->elapsed -= (float)passes * d->duration;
            } else if (d->repeat >= 0 && (int64_t)passes <= (int64_t)d->repeat && d->duration > 0.0f) {
                d->repeat -= (int32_t)passes;
                d->pass += passes;
                d->elapsed -= (float)passes * d->duration;
            } else {
                d->pass += d->repeat > 0 ? (uint32_t)d->repeat : 0u;
                d->repeat = 0;
                d->elapsed = d->duration;
                done = true;
            }
        }
        if (has_target) {
            br_tween_apply(e, d);
        }
        if (done) {
            d->dead = true;
            if (d->on_complete != NULL) {
                br_tween handle = { d->handle, d->handle_generation };
                d->on_complete(e, handle, d->user); /* may add tweens: d is not used after this */
            }
        }
    }
    s->updating = false;
    for (uint32_t i = s->tweens.count; i > 0; i--) {
        if (((br_tween_data *)br_array_at(&s->tweens, i - 1))->dead) {
            br_tween_remove_at(e, i - 1);
        }
    }
}
