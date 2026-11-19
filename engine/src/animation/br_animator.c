/* br_animator: the animator pool, playback (play, cross-fade, queue, loop modes), layers with
 * masks, additive layers, blend trees, events, root motion, and the system that samples every
 * animator. */
#include "animation/br_animation_internal.h"

#define BR_ANIM_MAX_SPANS 8   /* time spans one tick may cover (wraps per frame before a shortcut) */

/* A stretch of clip time crossed in one tick: from -> to (either direction). */
typedef struct br_anim_span {
    float from;
    float to;
    bool  include_from;
} br_anim_span;

/* ---- handles and lookup */

static br_array *br_animator_map(br_animation_state *s, br_anim_target_kind kind)
{
    return kind == BR_ANIM_TARGET_3D ? &s->by_node3d : &s->by_node2d;
}

br_animator_data *br_animator_data_get(br_engine *e, br_animator a)
{
    br_animation_state *s = e->animation;
    if (s == NULL) {
        return NULL;
    }
    uint32_t *dense = br_pool_get(&s->handles, a.index, a.generation);
    if (dense == NULL) {
        return NULL;
    }
    br_animator_data *d = br_array_at(&s->animators, *dense);
    return d->root.kind != BR_ANIM_TARGET_NONE ? d : NULL;
}

static br_animator br_animator_handle(const br_animator_data *d)
{
    br_animator a = { d->handle, d->handle_generation };
    return a;
}

br_animator br_animator_get(br_engine *e, br_anim_target root)
{
    br_animator none = { 0, 0 };
    br_animation_state *s = e->animation;
    if (s == NULL || root.kind == BR_ANIM_TARGET_NONE) {
        return none;
    }
    br_array *map = br_animator_map(s, root.kind);
    if (root.index >= map->count) {
        return none;
    }
    uint32_t h = *(uint32_t *)br_array_at(map, root.index);
    if (h == 0) {
        return none;
    }
    br_animator a = { h - 1, br_pool_slot_generation(&s->handles, h - 1) };
    br_animator_data *d = br_animator_data_get(e, a);
    return d != NULL && br_anim_target_equal(d->root, root) ? a : none;
}

bool br_animator_valid(br_engine *e, br_animator a)
{
    return br_animator_data_get(e, a) != NULL;
}

uint32_t br_animator_count(br_engine *e)
{
    return e->animation != NULL ? e->animation->animators.count : 0;
}

br_animator br_animator_add(br_engine *e, br_anim_target root)
{
    br_animator none = { 0, 0 };
    br_animation_state *s = e->animation;
    if (s == NULL || !br_anim_target_valid(e, root)) {
        br_log_error("br_animator_add: the node is not valid");
        return none;
    }
    br_animator existing = br_animator_get(e, root);
    if (existing.generation != 0) {
        return existing;
    }
    br_animator_data d;
    SDL_zero(d);
    d.root = root;
    br_array_init(&d.clips, sizeof(const br_anim_clip *));
    br_array_init(&d.bounds, sizeof(br_anim_bound));
    br_array_init(&d.slots, sizeof(br_anim_slot));
    br_array_init(&d.trees, sizeof(br_anim_tree));
    d.layer_count = 1;
    d.layers[0].weight = 1.0f;
    d.speed = 1.0f;
    d.state = -1;
    uint32_t *dense = br_pool_alloc(&s->handles, &d.handle, &d.handle_generation);
    if (dense == NULL) {
        return none;
    }
    *dense = s->animators.count;
    if (br_array_push(&s->animators, &d) == NULL) {
        br_pool_release(&s->handles, d.handle, d.handle_generation);
        return none;
    }
    br_array *map = br_animator_map(s, root.kind);
    if (root.index >= map->count) {
        br_array_resize(map, root.index + 1);
    }
    if (root.index < map->count) {
        *(uint32_t *)br_array_at(map, root.index) = d.handle + 1;
    }
    return br_animator_handle(&d);
}

static void br_animator_model_unref(br_engine *e, const br_model *model)
{
    br_animation_state *s = e->animation;
    for (uint32_t i = 0; i < s->model_clips.count; i++) {
        br_anim_model_clips *mc = br_array_at(&s->model_clips, i);
        if (mc->model != model) {
            continue;
        }
        mc->refs--;
        if (mc->refs == 0) {
            for (uint32_t k = 0; k < mc->count; k++) {
                br_anim_clip_destroy(mc->clips[k]);
            }
            br_free(mc->clips);
            br_array_remove_swap(&s->model_clips, i);
        }
        return;
    }
}

static void br_animator_free_data(br_engine *e, br_animator_data *d)
{
    for (uint32_t i = 0; i < d->bounds.count; i++) {
        br_anim_bound *b = br_array_at(&d->bounds, i);
        br_free(b->slots);
        br_free(b->hints);
    }
    br_array_free(&d->clips);
    br_array_free(&d->bounds);
    br_array_free(&d->slots);
    br_array_free(&d->trees);
    br_free(d->state_motion);
    if (d->model != NULL) {
        br_animator_model_unref(e, d->model);
    }
}

/* Removes dense entry i now (swap-remove). */
static void br_animator_remove_at(br_engine *e, uint32_t i)
{
    br_animation_state *s = e->animation;
    br_animator_data *d = br_array_at(&s->animators, i);
    br_array *map = br_animator_map(s, d->root.kind);
    if (d->root.kind != BR_ANIM_TARGET_NONE && d->root.index < map->count) {
        uint32_t *h = br_array_at(map, d->root.index);
        if (*h == d->handle + 1) {
            *h = 0;
        }
    }
    br_pool_release(&s->handles, d->handle, d->handle_generation);
    br_animator_free_data(e, d);
    uint32_t last = s->animators.count - 1;
    if (i != last) {
        br_animator_data *moved = br_array_at(&s->animators, last);
        *(uint32_t *)br_pool_slot_data(&s->handles, moved->handle) = i;
    }
    br_array_remove_swap(&s->animators, i);
}

/* Inside the update loop removals only mark the entry (kind NONE); the loop removes it after. */
static void br_animator_remove_data(br_engine *e, br_animator_data *d)
{
    br_animation_state *s = e->animation;
    if (s->updating) {
        br_array *map = br_animator_map(s, d->root.kind);
        if (d->root.index < map->count && *(uint32_t *)br_array_at(map, d->root.index) == d->handle + 1) {
            *(uint32_t *)br_array_at(map, d->root.index) = 0;
        }
        d->root.kind = BR_ANIM_TARGET_NONE;
        return;
    }
    br_animator_remove_at(e, (uint32_t)(d - (br_animator_data *)br_array_at(&s->animators, 0)));
}

void br_animator_remove(br_engine *e, br_animator a)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d != NULL) {
        br_animator_remove_data(e, d);
    }
}

void br_animator_on_node_destroy(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_animation_state *s = e->animation;
    s->destroy_epoch++;
    if (n.index < s->by_node2d.count && *(uint32_t *)br_array_at(&s->by_node2d, n.index) != 0) {
        br_anim_target t = br_anim_target_2d(n);
        br_animator a = br_animator_get(e, t);
        br_animator_remove(e, a);
    }
}

void br_animator_on_node3d_destroy(br_engine *e, br_node3d n, void *user)
{
    (void)user;
    br_animation_state *s = e->animation;
    s->destroy_epoch++;
    if (n.index < s->by_node3d.count && *(uint32_t *)br_array_at(&s->by_node3d, n.index) != 0) {
        br_animator a = br_animator_get(e, br_anim_target_3d(n));
        br_animator_remove(e, a);
    }
}

void br_animator_free_all(br_engine *e)
{
    br_animation_state *s = e->animation;
    while (s->animators.count > 0) {
        br_animator_remove_at(e, s->animators.count - 1);
    }
    for (uint32_t i = 0; i < s->model_clips.count; i++) {
        br_anim_model_clips *mc = br_array_at(&s->model_clips, i);
        for (uint32_t k = 0; k < mc->count; k++) {
            br_anim_clip_destroy(mc->clips[k]);
        }
        br_free(mc->clips);
    }
    br_array_clear(&s->model_clips);
}

/* ---- clips */

bool br_animator_add_clip(br_engine *e, br_animator a, const br_anim_clip *clip)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL || clip == NULL) {
        return false;
    }
    br_anim_bound b;
    SDL_zero(b);
    b.clip = clip;
    if (br_array_push(&d->clips, &clip) == NULL || br_array_push(&d->bounds, &b) == NULL) {
        return false;
    }
    return true;
}

uint32_t br_animator_add_library(br_engine *e, br_animator a, const br_anim_library *lib)
{
    uint32_t n = 0;
    for (uint32_t i = 0; lib != NULL && i < lib->count; i++) {
        n += br_animator_add_clip(e, a, lib->clips[i]) ? 1 : 0;
    }
    return n;
}

uint32_t br_animator_add_model_clips(br_engine *e, br_animator a, const br_model *model)
{
    br_animation_state *s = e->animation;
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL || model == NULL) {
        return 0;
    }
    if (d->model != NULL && d->model != model) {
        br_log_error("br_animator_add_model_clips: an animator takes the clips of one model");
        return 0;
    }
    br_anim_model_clips *mc = NULL;
    for (uint32_t i = 0; i < s->model_clips.count && mc == NULL; i++) {
        br_anim_model_clips *c = br_array_at(&s->model_clips, i);
        mc = c->model == model ? c : NULL;
    }
    if (mc == NULL) {
        br_anim_model_clips fresh;
        SDL_zero(fresh);
        fresh.model = model;
        fresh.clips = br_alloc(sizeof(br_anim_clip *) * (model->clip_count > 0 ? model->clip_count : 1));
        for (uint32_t i = 0; fresh.clips != NULL && i < model->clip_count; i++) {
            br_anim_clip *c = br_anim_clip_from_gltf(model, i);
            if (c != NULL) {
                fresh.clips[fresh.count++] = c;
            }
        }
        mc = br_array_push(&s->model_clips, &fresh);
        if (mc == NULL) {
            return 0;
        }
        d = br_animator_data_get(e, a);
    }
    if (d->model == NULL) {
        d->model = model;
        mc->refs++;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < mc->count; i++) {
        n += br_animator_add_clip(e, a, mc->clips[i]) ? 1 : 0;
    }
    return n;
}

int32_t br_animator_find_motion(br_animator_data *d, const char *name)
{
    if (name == NULL) {
        return INT32_MIN;
    }
    for (uint32_t i = 0; i < d->clips.count; i++) {
        const br_anim_clip *c = *(const br_anim_clip **)br_array_at(&d->clips, i);
        if (SDL_strcmp(c->name, name) == 0) {
            return (int32_t)i;
        }
    }
    for (uint32_t i = 0; i < d->trees.count; i++) {
        const br_anim_tree *t = br_array_at(&d->trees, i);
        if (SDL_strcmp(t->name, name) == 0) {
            return -(int32_t)i - 1;
        }
    }
    return INT32_MIN;
}

static const br_anim_clip *br_animator_clip(br_animator_data *d, uint32_t k)
{
    return *(const br_anim_clip **)br_array_at(&d->clips, k);
}

/* ---- binding: tracks -> slots, once per (animator, clip) */

static bool br_animator_slot_allowed(br_engine *e, const br_animator_data *d, br_anim_target t, uint32_t layer)
{
    const br_anim_layer *l = &d->layers[layer];
    if (l->mask_count == 0) {
        return true;
    }
    for (uint32_t m = 0; m < l->mask_count; m++) {
        if (br_anim_target_is_under(e, t, l->masks[m])) {
            return true;
        }
    }
    return false;
}

static uint32_t br_animator_layer_mask(br_engine *e, const br_animator_data *d, br_anim_target t)
{
    uint32_t mask = 0;
    for (uint32_t l = 0; l < BR_ANIMATOR_MAX_LAYERS; l++) {
        if (br_animator_slot_allowed(e, d, t, l)) {
            mask |= 1u << l;
        }
    }
    return mask;
}

static bool br_anim_prop_equal(const br_anim_prop_ref *a, const br_anim_prop_ref *b)
{
    return a->property == b->property && a->component == b->component && a->offset == b->offset;
}

static bool br_animator_is_position(br_engine *e, const br_anim_prop_ref *prop, br_anim_target_kind kind)
{
    return prop->property != BR_ANIM_NONE && prop->property == br_anim_property_find(e, kind, "position");
}

/* The slot a track writes, created on first use; BR_ANIM_NONE (with one warning) if the path does
 * not resolve. */
static uint32_t br_animator_slot_for(br_engine *e, br_animator_data *d, const br_anim_clip *clip, const br_anim_track *track)
{
    br_anim_target node;
    br_anim_prop_ref prop;
    if (!br_anim_resolve(e, d->root, track->path, &node, &prop)) {
        br_log_warn("animation: clip '%s': track '%s' has no such node or property; track dropped", clip->name,
                    track->path);
        return BR_ANIM_NONE;
    }
    if (prop.property == BR_ANIM_NONE && br_component_get(e, br_anim_target_node(node), prop.component) == NULL) {
        br_log_warn("animation: clip '%s': track '%s': the node has no such component; track dropped", clip->name,
                    track->path);
        return BR_ANIM_NONE;
    }
    uint32_t need = br_anim_value_components(prop.type);
    if (prop.type != BR_ANIM_WEIGHTS && track->components < need) {
        br_log_warn("animation: clip '%s': track '%s' has %u values per key, the property needs %u; track dropped",
                    clip->name, track->path, track->components, need);
        return BR_ANIM_NONE;
    }
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        if (br_anim_target_equal(slot->target, node) && br_anim_prop_equal(&slot->prop, &prop)) {
            if (prop.type == BR_ANIM_WEIGHTS) {
                slot->count = SDL_max(slot->count, track->components);
            }
            return i;
        }
    }
    br_anim_slot slot;
    SDL_zero(slot);
    slot.target = node;
    slot.prop = prop;
    slot.count = prop.type == BR_ANIM_WEIGHTS ? track->components : need;
    slot.layer_mask = br_animator_layer_mask(e, d, node);
    slot.root = d->root_motion && br_anim_target_equal(node, d->root_node) && br_animator_is_position(e, &prop, node.kind);
    br_anim_value v;
    br_anim_prop_get(e, node, &prop, &v);
    SDL_memcpy(slot.rest, v.f, sizeof(slot.rest));
    slot.rest_texture = v.texture;
    slot.set = prop.property != BR_ANIM_NONE ? br_anim_property_get_desc(e, prop.property)->set : NULL;
    if (br_array_push(&d->slots, &slot) == NULL) {
        return BR_ANIM_NONE;
    }
    return d->slots.count - 1;
}

static br_anim_bound *br_animator_bind(br_engine *e, br_animator_data *d, uint32_t k)
{
    br_anim_bound *b = br_array_at(&d->bounds, k);
    if (b->bound) {
        return b;
    }
    const br_anim_clip *clip = b->clip;
    uint32_t n = clip->track_count > 0 ? clip->track_count : 1;
    uint32_t *slots = br_alloc(sizeof(uint32_t) * n);
    uint32_t *hints = br_alloc(sizeof(uint32_t) * n);
    if (slots == NULL || hints == NULL) {
        br_free(slots);
        br_free(hints);
        return NULL;
    }
    for (uint32_t i = 0; i < clip->track_count; i++) {
        slots[i] = br_animator_slot_for(e, d, clip, &clip->tracks[i]);
    }
    b = br_array_at(&d->bounds, k);
    b->slots = slots;
    b->hints = hints;
    b->bound = true;
    return b;
}

/* ---- parameters and blend trees */

uint32_t br_animator_param_index(br_animator_data *d, const char *name, br_anim_param_type type, bool create)
{
    for (uint32_t i = 0; name != NULL && i < d->param_count; i++) {
        if (SDL_strcmp(d->params[i].name, name) == 0) {
            return i;
        }
    }
    if (!create || name == NULL || d->param_count >= BR_ANIMATOR_MAX_PARAMS) {
        if (create) {
            br_log_error("animator: more than %d parameters", BR_ANIMATOR_MAX_PARAMS);
        }
        return BR_ANIM_NONE;
    }
    br_anim_param *p = &d->params[d->param_count];
    SDL_strlcpy(p->name, name, sizeof(p->name));
    p->type = type;
    p->value = 0.0f;
    return d->param_count++;
}

static void br_animator_set_param(br_engine *e, br_animator a, const char *name, br_anim_param_type type, float value)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL) {
        return;
    }
    uint32_t i = br_animator_param_index(d, name, type, true);
    if (i != BR_ANIM_NONE) {
        d->params[i].value = value;
    }
}

void br_animator_set_float(br_engine *e, br_animator a, const char *name, float value)
{
    br_animator_set_param(e, a, name, BR_ANIM_PARAM_FLOAT, value);
}

void br_animator_set_bool(br_engine *e, br_animator a, const char *name, bool value)
{
    br_animator_set_param(e, a, name, BR_ANIM_PARAM_BOOL, value ? 1.0f : 0.0f);
}

void br_animator_set_trigger(br_engine *e, br_animator a, const char *name)
{
    br_animator_set_param(e, a, name, BR_ANIM_PARAM_TRIGGER, 1.0f);
}

float br_animator_get_float(br_engine *e, br_animator a, const char *name)
{
    br_animator_data *d = br_animator_data_get(e, a);
    uint32_t i = d != NULL ? br_animator_param_index(d, name, BR_ANIM_PARAM_FLOAT, false) : BR_ANIM_NONE;
    return i != BR_ANIM_NONE ? d->params[i].value : 0.0f;
}

static bool br_animator_add_tree(br_engine *e, br_animator a, const char *name, uint32_t dims, const char *px,
                                 const char *py, const char *const *clips, const float *pos, uint32_t count)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL || name == NULL || clips == NULL || pos == NULL || count == 0 || count > BR_ANIM_TREE_MAX) {
        br_log_error("animator: a blend tree needs 1..%d clips", BR_ANIM_TREE_MAX);
        return false;
    }
    br_anim_tree t;
    SDL_zero(t);
    SDL_strlcpy(t.name, name, sizeof(t.name));
    t.dims = dims;
    t.param[0] = br_animator_param_index(d, px, BR_ANIM_PARAM_FLOAT, true);
    t.param[1] = dims == 2 ? br_animator_param_index(d, py, BR_ANIM_PARAM_FLOAT, true) : 0;
    if (t.param[0] == BR_ANIM_NONE || t.param[1] == BR_ANIM_NONE) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        int32_t m = br_animator_find_motion(d, clips[i]);
        if (m < 0) {
            br_log_error("animator: blend tree '%s': no clip named '%s'", name, clips[i] != NULL ? clips[i] : "(null)");
            return false;
        }
        /* 1D: keep the children sorted by threshold (insertion) */
        uint32_t at = t.count;
        while (dims == 1 && at > 0 && t.pos[at - 1][0] > pos[i]) {
            t.clips[at] = t.clips[at - 1];
            t.pos[at][0] = t.pos[at - 1][0];
            at--;
        }
        t.clips[at] = (uint32_t)m;
        t.pos[at][0] = pos[i * dims];
        t.pos[at][1] = dims == 2 ? pos[i * dims + 1] : 0.0f;
        t.count++;
    }
    return br_array_push(&d->trees, &t) != NULL;
}

bool br_animator_add_blend1d(br_engine *e, br_animator a, const char *name, const char *parameter,
                             const char *const *clips, const float *thresholds, uint32_t count)
{
    return br_animator_add_tree(e, a, name, 1, parameter, NULL, clips, thresholds, count);
}

bool br_animator_add_blend2d(br_engine *e, br_animator a, const char *name, const char *param_x,
                             const char *param_y, const char *const *clips, const br_vec2 *positions,
                             uint32_t count)
{
    float pos[BR_ANIM_TREE_MAX * 2];
    for (uint32_t i = 0; positions != NULL && i < count && i < BR_ANIM_TREE_MAX; i++) {
        pos[i * 2] = positions[i].x;
        pos[i * 2 + 1] = positions[i].y;
    }
    return br_animator_add_tree(e, a, name, 2, param_x, param_y, clips, positions != NULL ? pos : NULL, count);
}

/* Child weights from the parameters: 1D lerps between the two neighbours, 2D uses inverse
 * squared distances (an exact hit takes everything). Weights sum to 1. */
static void br_anim_tree_weights(const br_animator_data *d, br_anim_tree *t)
{
    SDL_memset(t->weights, 0, sizeof(t->weights));
    float x = d->params[t->param[0]].value;
    if (t->dims == 1) {
        if (x <= t->pos[0][0]) {
            t->weights[0] = 1.0f;
            return;
        }
        for (uint32_t i = 0; i + 1 < t->count; i++) {
            float a = t->pos[i][0];
            float b = t->pos[i + 1][0];
            if (x <= b) {
                float u = b > a ? (x - a) / (b - a) : 1.0f;
                t->weights[i] = 1.0f - u;
                t->weights[i + 1] = u;
                return;
            }
        }
        t->weights[t->count - 1] = 1.0f;
        return;
    }
    // ponytail: inverse-distance weights, not Unity's gradient-band interpolation; add it if blends between close samples look mushy.
    float y = d->params[t->param[1]].value;
    float sum = 0.0f;
    for (uint32_t i = 0; i < t->count; i++) {
        float dx = x - t->pos[i][0];
        float dy = y - t->pos[i][1];
        float dist2 = dx * dx + dy * dy;
        if (dist2 < 1e-8f) {
            SDL_memset(t->weights, 0, sizeof(t->weights));
            t->weights[i] = 1.0f;
            return;
        }
        t->weights[i] = 1.0f / dist2;
        sum += t->weights[i];
    }
    for (uint32_t i = 0; i < t->count; i++) {
        t->weights[i] /= sum;
    }
}

float br_animator_motion_duration(br_animator_data *d, int32_t motion)
{
    if (motion >= 0) {
        return br_animator_clip(d, (uint32_t)motion)->duration;
    }
    br_anim_tree *t = br_array_at(&d->trees, (uint32_t)(-motion - 1));
    br_anim_tree_weights(d, t);
    float sum = 0.0f;
    for (uint32_t i = 0; i < t->count; i++) {
        sum += t->weights[i] * br_animator_clip(d, t->clips[i])->duration;
    }
    return sum;
}

float br_animator_blend_weight(br_engine *e, br_animator a, const char *tree, uint32_t child)
{
    br_animator_data *d = br_animator_data_get(e, a);
    int32_t m = d != NULL ? br_animator_find_motion(d, tree) : INT32_MIN;
    if (m >= 0 || m == INT32_MIN) {
        return 0.0f;
    }
    br_anim_tree *t = br_array_at(&d->trees, (uint32_t)(-m - 1));
    br_anim_tree_weights(d, t);
    return child < t->count ? t->weights[child] : 0.0f;
}

/* ---- playback */

static bool br_animator_layer_ok(br_animator_data *d, uint32_t layer)
{
    return d != NULL && layer < d->layer_count;
}

bool br_animator_start(br_engine *e, br_animator_data *d, uint32_t layer, int32_t motion, float fade)
{
    br_anim_layer *l = &d->layers[layer];
    if (motion >= 0) {
        br_animator_bind(e, d, (uint32_t)motion); /* rest values are read before anything moves */
    } else {
        br_anim_tree *t = br_array_at(&d->trees, (uint32_t)(-motion - 1));
        for (uint32_t i = 0; i < t->count; i++) {
            br_animator_bind(e, d, t->clips[i]);
        }
    }
    if (fade > 0.0f && l->cur.active) {
        l->prev = l->cur;
        l->fade_elapsed = 0.0f;
        l->fade_duration = fade;
    } else {
        l->prev.active = false;
    }
    br_anim_player *p = &l->cur;
    SDL_zerop(p);
    p->active = true;
    p->motion = motion;
    p->speed = 1.0f;
    p->loop = motion >= 0 ? br_animator_clip(d, (uint32_t)motion)->loop : BR_ANIM_LOOP;
    p->dir = 1.0f;
    p->fresh = true;
    p->time = d->speed < 0.0f ? (motion >= 0 ? br_animator_motion_duration(d, motion) : 1.0f) : 0.0f;
    return true;
}

static bool br_animator_play_on(br_engine *e, br_animator a, uint32_t layer, const char *motion, float fade, bool clear_queue)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (!br_animator_layer_ok(d, layer)) {
        return false;
    }
    int32_t m = br_animator_find_motion(d, motion);
    if (m == INT32_MIN) {
        br_log_error("animator: no clip or blend tree named '%s'", motion != NULL ? motion : "(null)");
        return false;
    }
    if (clear_queue) {
        d->layers[layer].queue_count = 0;
    }
    return br_animator_start(e, d, layer, m, fade);
}

bool br_animator_play(br_engine *e, br_animator a, const char *motion)
{
    return br_animator_play_on(e, a, 0, motion, 0.0f, true);
}

bool br_animator_cross_fade(br_engine *e, br_animator a, const char *motion, float seconds)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL) {
        return false;
    }
    int32_t m = br_animator_find_motion(d, motion);
    const br_anim_layer *l = &d->layers[0];
    if (m != INT32_MIN && l->cur.active && l->cur.motion == m && !l->prev.active) {
        return true; /* already playing it */
    }
    return br_animator_play_on(e, a, 0, motion, seconds, true);
}

bool br_animator_queue(br_engine *e, br_animator a, const char *motion, float fade_seconds)
{
    br_animator_data *d = br_animator_data_get(e, a);
    int32_t m = d != NULL ? br_animator_find_motion(d, motion) : INT32_MIN;
    if (m == INT32_MIN || d->layers[0].queue_count >= BR_ANIMATOR_MAX_QUEUE) {
        br_log_error("animator: cannot queue '%s' (unknown, or the queue holds %d)", motion != NULL ? motion : "(null)",
                     BR_ANIMATOR_MAX_QUEUE);
        return false;
    }
    br_anim_layer *l = &d->layers[0];
    if (!l->cur.active) {
        return br_animator_start(e, d, 0, m, 0.0f);
    }
    l->queue[l->queue_count].motion = m;
    l->queue[l->queue_count].fade = fade_seconds;
    l->queue_count++;
    return true;
}

void br_animator_stop(br_engine *e, br_animator a)
{
    br_animator_data *d = br_animator_data_get(e, a);
    for (uint32_t l = 0; d != NULL && l < d->layer_count; l++) {
        d->layers[l].cur.active = false;
        d->layers[l].prev.active = false;
        d->layers[l].queue_count = 0;
    }
}

void br_animator_set_speed(br_engine *e, br_animator a, float speed)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d != NULL) {
        d->speed = speed;
    }
}

void br_animator_set_paused(br_engine *e, br_animator a, bool paused)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d != NULL) {
        d->paused = paused;
    }
}

void br_animator_set_loop(br_engine *e, br_animator a, uint32_t layer, br_anim_loop loop)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (br_animator_layer_ok(d, layer)) {
        d->layers[layer].cur.loop = loop;
        d->layers[layer].prev.loop = loop;
    }
}

void br_animator_set_time(br_engine *e, br_animator a, uint32_t layer, float seconds)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (br_animator_layer_ok(d, layer)) {
        d->layers[layer].cur.time = seconds;
        d->layers[layer].cur.ended = false;
    }
}

float br_animator_time(br_engine *e, br_animator a, uint32_t layer)
{
    br_animator_data *d = br_animator_data_get(e, a);
    return br_animator_layer_ok(d, layer) ? d->layers[layer].cur.time : 0.0f;
}

static const char *br_animator_motion_name(br_animator_data *d, int32_t motion)
{
    if (motion >= 0) {
        return br_animator_clip(d, (uint32_t)motion)->name;
    }
    return ((br_anim_tree *)br_array_at(&d->trees, (uint32_t)(-motion - 1)))->name;
}

const char *br_animator_current(br_engine *e, br_animator a, uint32_t layer)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (!br_animator_layer_ok(d, layer) || !d->layers[layer].cur.active) {
        return NULL;
    }
    return br_animator_motion_name(d, d->layers[layer].cur.motion);
}

bool br_animator_finished(br_engine *e, br_animator a, uint32_t layer)
{
    br_animator_data *d = br_animator_data_get(e, a);
    return br_animator_layer_ok(d, layer) && d->layers[layer].cur.ended;
}

float br_animator_fade_progress(br_engine *e, br_animator a, uint32_t layer)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (!br_animator_layer_ok(d, layer) || !d->layers[layer].prev.active || d->layers[layer].fade_duration <= 0.0f) {
        return 1.0f;
    }
    return br_clamp01f(d->layers[layer].fade_elapsed / d->layers[layer].fade_duration);
}

/* ---- layers */

bool br_animator_set_layer(br_engine *e, br_animator a, uint32_t layer, float weight, bool additive)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL || layer == 0 || layer >= BR_ANIMATOR_MAX_LAYERS) {
        br_log_error("br_animator_set_layer: layers 1..%d can be configured", BR_ANIMATOR_MAX_LAYERS - 1);
        return false;
    }
    if (layer >= d->layer_count) {
        d->layer_count = layer + 1;
    }
    d->layers[layer].weight = br_clamp01f(weight);
    d->layers[layer].additive = additive;
    return true;
}

bool br_animator_layer_add_mask(br_engine *e, br_animator a, uint32_t layer, const char *node_path)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL || layer >= BR_ANIMATOR_MAX_LAYERS || d->layers[layer].mask_count >= BR_ANIMATOR_MAX_MASKS) {
        return false;
    }
    br_anim_target t = br_anim_target_find(e, d->root, node_path);
    if (t.kind == BR_ANIM_TARGET_NONE) {
        br_log_error("br_animator_layer_add_mask: no node '%s'", node_path != NULL ? node_path : "(null)");
        return false;
    }
    br_anim_layer *l = &d->layers[layer];
    l->masks[l->mask_count++] = t;
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        slot->layer_mask = br_animator_layer_mask(e, d, slot->target);
    }
    return true;
}

bool br_animator_play_layer(br_engine *e, br_animator a, uint32_t layer, const char *motion, float seconds)
{
    return br_animator_play_on(e, a, layer, motion, seconds, true);
}

void br_animator_stop_layer(br_engine *e, br_animator a, uint32_t layer)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (br_animator_layer_ok(d, layer)) {
        d->layers[layer].cur.active = false;
        d->layers[layer].prev.active = false;
    }
}

/* ---- events and root motion */

void br_animator_set_event_callback(br_engine *e, br_animator a, br_anim_event_fn fn, void *user)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d != NULL) {
        d->on_event = fn;
        d->event_user = user;
    }
}

const br_anim_event *br_animation_events(br_engine *e, uint32_t *count)
{
    br_animation_state *s = e->animation;
    *count = s != NULL ? s->events.count : 0;
    return s != NULL && s->events.count > 0 ? br_array_at(&s->events, 0) : NULL;
}

bool br_animator_set_root_motion(br_engine *e, br_animator a, const char *node_path)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL) {
        return false;
    }
    d->root_motion = false;
    if (node_path != NULL) {
        d->root_node = br_anim_target_find(e, d->root, node_path);
        if (d->root_node.kind == BR_ANIM_TARGET_NONE) {
            br_log_error("br_animator_set_root_motion: no node '%s'", node_path);
            return false;
        }
        d->root_motion = true;
    }
    d->root_has_prev = false;
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        slot->root = d->root_motion && br_anim_target_equal(slot->target, d->root_node) &&
                     br_animator_is_position(e, &slot->prop, slot->target.kind);
    }
    return true;
}

br_vec3 br_animator_root_motion(br_engine *e, br_animator a)
{
    br_animator_data *d = br_animator_data_get(e, a);
    return d != NULL ? d->root_delta : br_vec3_make(0.0f, 0.0f, 0.0f);
}

void br_animation_stats(br_engine *e, uint32_t *animators, uint32_t *tracks)
{
    br_animation_state *s = e->animation;
    if (animators != NULL) {
        *animators = s != NULL ? s->stat_animators : 0;
    }
    if (tracks != NULL) {
        *tracks = s != NULL ? s->stat_tracks : 0;
    }
}

/* ---- the clock */

static uint32_t br_anim_add_span(br_anim_span *spans, uint32_t n, float from, float to, bool include_from)
{
    if (n < BR_ANIM_MAX_SPANS) {
        spans[n].from = from;
        spans[n].to = to;
        spans[n].include_from = include_from;
        n++;
    }
    return n;
}

/* Advances a player by `step` seconds of a motion `duration` long, following its loop mode.
 * Writes the stretches of time it crossed (for markers). */
static uint32_t br_anim_tick(br_anim_player *p, float duration, float step, br_anim_span *spans)
{
    uint32_t n = 0;
    bool include = p->fresh;
    p->fresh = false;
    p->wrapped = false;
    p->ended_now = false;
    if (duration <= 0.0f) {
        p->time = 0.0f;
        p->ended_now = !p->ended && (p->loop == BR_ANIM_ONCE || p->loop == BR_ANIM_CLAMP);
        p->ended = p->ended || p->ended_now;
        return include ? br_anim_add_span(spans, n, 0.0f, 0.0f, true) : 0;
    }
    float t = br_clampf(p->time, 0.0f, duration);
    if (step == 0.0f || p->ended) {
        return include ? br_anim_add_span(spans, n, t, t, true) : 0;
    }
    float dir = (step > 0.0f ? 1.0f : -1.0f) * p->dir;
    float remaining = SDL_fabsf(step);
    if (p->loop == BR_ANIM_LOOP) {
        /* one fmod (not a subtraction per wrap) keeps the clock bit-identical to the Phase 1 animator */
        float raw = t + dir * remaining;
        if (raw >= 0.0f && raw < duration) {
            p->time = raw;
            return br_anim_add_span(spans, n, t, raw, include);
        }
        p->wrapped = true;
        float nt = SDL_fmodf(raw, duration);
        if (nt < 0.0f) {
            nt += duration;
        }
        float start = dir > 0.0f ? 0.0f : duration;
        float end = dir > 0.0f ? duration : 0.0f;
        float crossings = dir > 0.0f ? SDL_floorf(raw / duration) : SDL_floorf(-raw / duration) + 1.0f;
        n = br_anim_add_span(spans, n, t, end, include);
        // ponytail: a step longer than ~8 passes skips the markers of the extra passes; fine for frame steps.
        for (float k = 1.0f; k < crossings && n + 1 < BR_ANIM_MAX_SPANS; k += 1.0f) {
            n = br_anim_add_span(spans, n, start, end, true);
        }
        n = br_anim_add_span(spans, n, start, nt, true);
        p->time = nt;
        return n;
    }
    while (remaining > 0.0f) {
        float room = dir > 0.0f ? duration - t : t;
        if (remaining < room) {
            float nt = t + dir * remaining;
            n = br_anim_add_span(spans, n, t, nt, include);
            t = nt;
            break;
        }
        float boundary = dir > 0.0f ? duration : 0.0f;
        n = br_anim_add_span(spans, n, t, boundary, include);
        remaining -= room;
        t = boundary;
        if (p->loop != BR_ANIM_PING_PONG) {
            p->ended = true; /* ONCE / CLAMP */
            p->ended_now = true;
            break;
        }
        p->wrapped = true;
        p->dir = -p->dir;
        dir = -dir;
        include = false;
        if (n >= BR_ANIM_MAX_SPANS) {
            /* the markers of the extra passes are skipped (see LOOP above) */
            float rest = SDL_fmodf(remaining, 2.0f * duration);
            if (rest > duration) {
                rest -= duration;
                p->dir = -p->dir;
                dir = -dir;
            }
            t = dir > 0.0f ? rest : duration - rest;
            break;
        }
    }
    p->time = t;
    return n;
}

/* ---- events */

static void br_anim_push_event(br_engine *e, br_animator_data *d, const br_anim_clip *clip, const br_anim_marker *m)
{
    br_anim_event ev;
    ev.animator = br_animator_handle(d);
    ev.root = d->root;
    ev.name = m->name;
    ev.clip = clip->name;
    ev.time = m->time;
    br_array_push(&e->animation->events, &ev);
}

/* Markers of `clip` inside the spans (scaled from the player's time units by `scale`). */
static void br_anim_fire(br_engine *e, br_animator_data *d, const br_anim_clip *clip, const br_anim_span *spans,
                         uint32_t n, float scale)
{
    for (uint32_t s = 0; s < n; s++) {
        float a = spans[s].from * scale;
        float b = spans[s].to * scale;
        for (uint32_t k = 0; k < clip->marker_count; k++) {
            float m = clip->markers[k].time;
            bool hit;
            if (b >= a) {
                hit = (spans[s].include_from ? m >= a : m > a) && m <= b;
            } else {
                hit = (spans[s].include_from ? m <= a : m < a) && m >= b;
            }
            if (hit) {
                br_anim_push_event(e, d, clip, &clip->markers[k]);
            }
        }
    }
}

static void br_anim_fire_player(br_engine *e, br_animator_data *d, const br_anim_player *p, const br_anim_span *spans,
                                uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (p->motion >= 0) {
        br_anim_fire(e, d, br_animator_clip(d, (uint32_t)p->motion), spans, n, 1.0f);
        return;
    }
    /* blend trees report the markers of their heaviest child */
    const br_anim_tree *t = br_array_at(&d->trees, (uint32_t)(-p->motion - 1));
    uint32_t best = 0;
    for (uint32_t i = 1; i < t->count; i++) {
        best = t->weights[i] > t->weights[best] ? i : best;
    }
    const br_anim_clip *clip = br_animator_clip(d, t->clips[best]);
    br_anim_fire(e, d, clip, spans, n, clip->duration);
}

/* ---- sampling and blending */

static bool br_anim_discrete(br_anim_value_type type)
{
    return type == BR_ANIM_BOOL || type == BR_ANIM_INT || type == BR_ANIM_TEXTURE;
}

static void br_anim_blend(br_anim_value_type type, float *dst, const float *src, uint32_t count, float k)
{
    if (type == BR_ANIM_QUAT) {
        br_quat a = { dst[0], dst[1], dst[2], dst[3] };
        br_quat b = { src[0], src[1], src[2], src[3] };
        br_quat q = br_quat_slerp(a, b, k);
        dst[0] = q.x;
        dst[1] = q.y;
        dst[2] = q.z;
        dst[3] = q.w;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = dst[i] + (src[i] - dst[i]) * k;
    }
}

/* The value below the current layer: final once some layer wrote the slot, else its rest value. */
static void br_anim_slot_base(br_anim_slot *slot)
{
    if (!slot->touched) {
        SDL_memcpy(slot->final.f, slot->rest, sizeof(float) * slot->count);
        slot->final.texture = slot->rest_texture;
        slot->touched = true;
    }
}

/* Adds one sampled value with weight w to the slot's layer blend. */
static void br_anim_accumulate(br_anim_slot *slot, const float *v, br_texture texture, float w, bool additive)
{
    br_anim_value_type type = slot->prop.type;
    if (br_anim_discrete(type)) {
        if (!additive && (slot->acc_weight <= 0.0f || w > slot->acc_max)) {
            slot->acc[0] = v[0];
            slot->acc_texture = texture;
            slot->acc_max = w;
        }
        slot->acc_weight += w;
        return;
    }
    if (additive) {
        if (type == BR_ANIM_QUAT) {
            br_quat acc = { slot->acc[0], slot->acc[1], slot->acc[2], slot->acc[3] };
            if (slot->acc_weight <= 0.0f) {
                acc = br_quat_identity();
            }
            br_quat q = { v[0], v[1], v[2], v[3] };
            acc = br_quat_mul(acc, br_quat_slerp(br_quat_identity(), q, w));
            slot->acc[0] = acc.x;
            slot->acc[1] = acc.y;
            slot->acc[2] = acc.z;
            slot->acc[3] = acc.w;
        } else {
            for (uint32_t i = 0; i < slot->count; i++) {
                slot->acc[i] = (slot->acc_weight <= 0.0f ? 0.0f : slot->acc[i]) + v[i] * w;
            }
        }
        slot->acc_weight += w;
        return;
    }
    if (slot->acc_weight <= 0.0f) {
        SDL_memcpy(slot->acc, v, sizeof(float) * slot->count);
    } else {
        br_anim_blend(type, slot->acc, v, slot->count, w / (slot->acc_weight + w));
    }
    slot->acc_weight += w;
}

/* Samples clip k at time t into the slots of layer `layer` with weight w. direct: the base layer
 * plays this clip alone, so samples go straight into the final values. */
static void br_anim_sample_clip(br_engine *e, br_animator_data *d, uint32_t layer, uint32_t k, float t, float w,
                                bool direct)
{
    if (w <= 0.0f) {
        return;
    }
    br_anim_bound *b = br_animator_bind(e, d, k);
    if (b == NULL) {
        return;
    }
    const br_anim_clip *clip = b->clip;
    bool additive = d->layers[layer].additive;
    uint32_t bit = 1u << layer;
    br_anim_slot *slots = d->slots.data;
    for (uint32_t i = 0; i < clip->track_count; i++) {
        uint32_t si = b->slots[i];
        if (si == BR_ANIM_NONE) {
            continue;
        }
        br_anim_slot *slot = &slots[si];
        if (slot->dead || (slot->layer_mask & bit) == 0) {
            continue;
        }
        const br_anim_track *track = &clip->tracks[i];
        bool quat = slot->prop.type == BR_ANIM_QUAT;
        float v[BR_ANIM_MAX_COMPONENTS];
        br_anim_track_sample(track, t, quat, &b->hints[i], v);
        br_texture tex = { 0, 0 };
        if (slot->prop.type == BR_ANIM_TEXTURE) {
            uint32_t ti = (uint32_t)SDL_max(0.0f, v[0]);
            tex = ti < clip->texture_count ? clip->textures[ti] : tex;
        }
        if (direct) {
            SDL_memcpy(slot->final.f, v, sizeof(float) * slot->count);
            slot->final.texture = tex;
            slot->touched = true;
            continue;
        }
        if (additive && !br_anim_discrete(slot->prop.type)) {
            /* additive: the change from the clip's first key */
            float r[BR_ANIM_MAX_COMPONENTS] = { 0 };
            br_anim_track_sample(track, track->times[0], quat, NULL, r);
            if (quat) {
                br_quat q = { v[0], v[1], v[2], v[3] };
                br_quat ref = { r[0], r[1], r[2], r[3] };
                br_quat dq = br_quat_mul(q, br_quat_conjugate(ref));
                v[0] = dq.x;
                v[1] = dq.y;
                v[2] = dq.z;
                v[3] = dq.w;
            } else {
                for (uint32_t c = 0; c < slot->count; c++) {
                    v[c] -= r[c];
                }
            }
        }
        br_anim_accumulate(slot, v, tex, w, additive);
    }
}

static void br_anim_sample_player(br_engine *e, br_animator_data *d, uint32_t layer, const br_anim_player *p, float w)
{
    if (!p->active || w <= 0.0f) {
        return;
    }
    if (p->motion >= 0) {
        br_anim_sample_clip(e, d, layer, (uint32_t)p->motion, p->time, w, false);
        return;
    }
    br_anim_tree *t = br_array_at(&d->trees, (uint32_t)(-p->motion - 1));
    br_anim_tree_weights(d, t);
    for (uint32_t i = 0; i < t->count; i++) {
        float child_duration = br_animator_clip(d, t->clips[i])->duration;
        br_anim_sample_clip(e, d, layer, t->clips[i], p->time * child_duration, w * t->weights[i], false);
    }
}

/* Folds a layer's blend into the slots' final values. */
static void br_anim_combine(br_animator_data *d, uint32_t layer)
{
    const br_anim_layer *l = &d->layers[layer];
    float lw = layer == 0 ? 1.0f : l->weight;
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        if (slot->acc_weight <= 0.0f) {
            continue;
        }
        br_anim_value_type type = slot->prop.type;
        br_anim_slot_base(slot);
        if (l->additive) {
            if (type == BR_ANIM_QUAT) {
                br_quat f = { slot->final.f[0], slot->final.f[1], slot->final.f[2], slot->final.f[3] };
                br_quat a = { slot->acc[0], slot->acc[1], slot->acc[2], slot->acc[3] };
                br_quat q = br_quat_normalize(br_quat_mul(f, br_quat_slerp(br_quat_identity(), a, lw)));
                slot->final.f[0] = q.x;
                slot->final.f[1] = q.y;
                slot->final.f[2] = q.z;
                slot->final.f[3] = q.w;
            } else if (!br_anim_discrete(type)) {
                for (uint32_t c = 0; c < slot->count; c++) {
                    slot->final.f[c] += slot->acc[c] * lw;
                }
            }
            continue;
        }
        /* a layer covering a slot only partly (weights < 1) blends from what is below it */
        float amount = SDL_min(slot->acc_weight, 1.0f) * lw;
        if (br_anim_discrete(type)) {
            if (amount >= 0.5f) {
                slot->final.f[0] = slot->acc[0];
                slot->final.texture = slot->acc_texture;
            }
        } else if (amount >= 1.0f) {
            SDL_memcpy(slot->final.f, slot->acc, sizeof(float) * slot->count);
        } else {
            br_anim_blend(type, slot->final.f, slot->acc, slot->count, amount);
        }
    }
}

static void br_anim_reset_acc(br_animator_data *d)
{
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        slot->acc_weight = 0.0f;
        slot->acc_max = 0.0f;
    }
}

/* acc = blend(faded pose, new pose, f), where a pose missing a slot uses what is below the layer
 * (rest / lower layers; nothing for additive layers). */
static void br_anim_blend_fade(br_animator_data *d, uint32_t layer, float f)
{
    bool additive = d->layers[layer].additive;
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = br_array_at(&d->slots, i);
        if (!slot->has_faded && slot->acc_weight <= 0.0f) {
            continue;
        }
        float below[BR_ANIM_MAX_COMPONENTS] = { 0 };
        br_anim_slot_base(slot);
        if (!additive) {
            SDL_memcpy(below, slot->final.f, sizeof(float) * slot->count);
        } else if (slot->prop.type == BR_ANIM_QUAT) {
            below[3] = 1.0f;
        }
        float a[BR_ANIM_MAX_COMPONENTS];
        float b[BR_ANIM_MAX_COMPONENTS];
        SDL_memcpy(a, slot->has_faded ? slot->faded : below, sizeof(a));
        SDL_memcpy(b, slot->acc_weight > 0.0f ? slot->acc : below, sizeof(b));
        br_texture ta = slot->has_faded ? slot->faded_texture : slot->final.texture;
        br_texture tb = slot->acc_weight > 0.0f ? slot->acc_texture : slot->final.texture;
        if (br_anim_discrete(slot->prop.type)) {
            SDL_memcpy(slot->acc, f >= 0.5f ? b : a, sizeof(slot->acc));
            slot->acc_texture = f >= 0.5f ? tb : ta;
        } else {
            br_anim_blend(slot->prop.type, a, b, slot->count, f);
            SDL_memcpy(slot->acc, a, sizeof(slot->acc));
        }
        slot->acc_weight = 1.0f;
    }
}

static float br_anim_player_duration(br_animator_data *d, const br_anim_player *p)
{
    return p->motion >= 0 ? br_animator_motion_duration(d, p->motion) : 1.0f;
}

/* Clock step of a player in its own time units (trees: normalised by their weighted duration). */
static float br_anim_player_step(br_animator_data *d, const br_anim_player *p, float step)
{
    if (p->motion >= 0) {
        return step * p->speed;
    }
    float duration = br_animator_motion_duration(d, p->motion);
    return duration > 0.0f ? step * p->speed / duration : 0.0f;
}

static void br_anim_update_layer(br_engine *e, br_animator_data *d, uint32_t layer, float step, float fade_step)
{
    br_anim_layer *l = &d->layers[layer];
    /* the queue moves on when the current motion ended or turned around */
    if (l->cur.active && (l->cur.ended || l->cur.wrapped) && l->queue_count > 0) {
        br_anim_queued next = l->queue[0];
        for (uint32_t i = 1; i < l->queue_count; i++) {
            l->queue[i - 1] = l->queue[i];
        }
        l->queue_count--;
        br_animator_start(e, d, layer, next.motion, next.fade);
    } else if (l->cur.active && l->cur.ended && l->cur.loop == BR_ANIM_ONCE) {
        l->cur.active = false; /* ONCE: the end pose was written last frame; stop writing */
    }
    if (l->prev.active) {
        l->fade_elapsed += fade_step;
        if (l->fade_elapsed >= l->fade_duration) {
            l->prev.active = false;
        }
    }
    float f = l->prev.active && l->fade_duration > 0.0f ? l->fade_elapsed / l->fade_duration : 1.0f;
    bool contributes = layer == 0 || l->weight > 0.0f;
    br_anim_span spans[BR_ANIM_MAX_SPANS];
    if (l->prev.active) {
        uint32_t n = br_anim_tick(&l->prev, br_anim_player_duration(d, &l->prev), br_anim_player_step(d, &l->prev, step), spans);
        if (contributes && f < 1.0f) {
            br_anim_fire_player(e, d, &l->prev, spans, n);
        }
    }
    if (l->cur.active) {
        uint32_t n = br_anim_tick(&l->cur, br_anim_player_duration(d, &l->cur), br_anim_player_step(d, &l->cur, step), spans);
        if (contributes) {
            br_anim_fire_player(e, d, &l->cur, spans, n);
        }
    }
    if (!contributes || (!l->cur.active && !l->prev.active)) {
        return;
    }
    if (l->prev.active) {
        /* cross-fade: the old motion's pose, then the new one's, then blend the two by f */
        br_anim_reset_acc(d);
        br_anim_sample_player(e, d, layer, &l->prev, 1.0f);
        for (uint32_t i = 0; i < d->slots.count; i++) {
            br_anim_slot *slot = br_array_at(&d->slots, i);
            slot->has_faded = slot->acc_weight > 0.0f;
            if (slot->has_faded) {
                SDL_memcpy(slot->faded, slot->acc, sizeof(float) * slot->count);
                slot->faded_texture = slot->acc_texture;
            }
        }
        br_anim_reset_acc(d);
        br_anim_sample_player(e, d, layer, &l->cur, 1.0f);
        br_anim_blend_fade(d, layer, f);
    } else if (layer == 0 && l->cur.motion >= 0) {
        br_anim_sample_clip(e, d, 0, (uint32_t)l->cur.motion, l->cur.time, 1.0f, true); /* the common case */
        return;
    } else {
        br_anim_reset_acc(d);
        br_anim_sample_player(e, d, layer, &l->cur, 1.0f);
    }
    br_anim_combine(d, layer);
}

/* Root motion: report the root slot's movement, keep the node in place horizontally. */
static void br_anim_root_motion(br_animator_data *d, br_anim_slot *slot)
{
    bool three_d = slot->target.kind == BR_ANIM_TARGET_3D;
    float now[3] = { slot->final.f[0], slot->final.f[1], three_d ? slot->final.f[2] : 0.0f };
    if (d->root_has_prev && !d->layers[0].cur.wrapped) {
        // ponytail: the frame a loop wraps reports no motion (one-frame hitch); split the delta at the wrap if it shows.
        d->root_delta.x += now[0] - d->root_prev[0];
        if (three_d) {
            d->root_delta.z += now[2] - d->root_prev[2];
        } else {
            d->root_delta.y += now[1] - d->root_prev[1];
        }
    }
    SDL_memcpy(d->root_prev, now, sizeof(now));
    d->root_has_prev = true;
    slot->final.f[0] = slot->rest[0];
    if (three_d) {
        slot->final.f[2] = slot->rest[2];
    } else {
        slot->final.f[1] = slot->rest[1];
    }
}

static void br_animator_update(br_engine *e, br_animator_data *d, float dt)
{
    br_animation_state *s = e->animation;
    if (d->graph != NULL) {
        br_anim_graph_update(e, d);
    }
    float step = d->paused ? 0.0f : dt * d->speed;
    float fade_step = d->paused ? 0.0f : dt;
    d->root_delta = br_vec3_make(0.0f, 0.0f, 0.0f);
    for (uint32_t l = 0; l < d->layer_count; l++) {
        br_anim_update_layer(e, d, l, step, fade_step);
    }
    /* targets are re-checked only after some node was destroyed (Review Focus 5 at no per-frame cost) */
    bool check_targets = d->destroy_epoch != s->destroy_epoch;
    d->destroy_epoch = s->destroy_epoch;
    br_anim_slot *slots = d->slots.data;
    for (uint32_t i = 0; i < d->slots.count; i++) {
        br_anim_slot *slot = &slots[i];
        if (!slot->touched || slot->dead) {
            continue;
        }
        slot->touched = false; /* final is valid only while touched (see br_anim_slot_base) */
        if (check_targets && !br_anim_target_valid(e, slot->target)) {
            slot->dead = true;
            br_log_warn("animation: a node animated by an animator was destroyed; its tracks are dropped");
            continue;
        }
        if (slot->root && d->root_motion) {
            br_anim_root_motion(d, slot);
        }
        slot->final.count = slot->count;
        if (slot->set != NULL) {
            slot->set(e, slot->target, &slot->final);
        } else {
            br_anim_prop_set(e, slot->target, &slot->prop, &slot->final); /* component field */
        }
        s->stat_tracks++;
    }
}

void br_animator_system(br_engine *e, float dt, void *user)
{
    (void)user;
    br_animation_state *s = e->animation;
    br_anim3d_sync_in(e);
    br_array_clear(&s->events);
    s->stat_animators = 0;
    s->stat_tracks = 0;
    s->updating = true;
    for (uint32_t i = 0; i < s->animators.count; i++) {
        br_animator_data *d = br_array_at(&s->animators, i);
        if (d->root.kind == BR_ANIM_TARGET_NONE) {
            continue;
        }
        if (!br_anim_target_valid(e, d->root)) {
            br_animator_remove_data(e, d); /* destroyed this frame; listeners may not have run yet */
            continue;
        }
        br_animator_update(e, d, dt);
        s->stat_animators++;
    }
    s->updating = false;
    for (uint32_t i = s->animators.count; i > 0; i--) {
        br_animator_data *d = br_array_at(&s->animators, i - 1);
        if (d->root.kind == BR_ANIM_TARGET_NONE) {
            br_animator_remove_at(e, i - 1);
        }
    }
    br_anim3d_sync_out(e);
    /* callbacks last: they may play clips or add animators (the pool may move) */
    for (uint32_t i = 0; i < s->events.count; i++) {
        br_anim_event ev = *(br_anim_event *)br_array_at(&s->events, i);
        br_animator_data *d = br_animator_data_get(e, ev.animator);
        if (d != NULL && d->on_event != NULL) {
            d->on_event(e, &ev, d->event_user);
        }
    }
}
