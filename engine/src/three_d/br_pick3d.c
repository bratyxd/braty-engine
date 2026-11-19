/* br_pick3d: screen <-> world, ray picking (bounds, then the type's pick: triangles), and 3D
 * pointer events. The events module only knows the view (a 2D node): the view's hit_test says
 * "hit" where a 3D node with 3D listeners is under the pointer, and 2D listeners on the view
 * re-pick and dispatch to per-3D-node listeners, bubbling up the 3D parents. */
#include "three_d/br_three_d_internal.h"

/* ---- screen <-> world */

static br_vec2 br_view3d_local_to_ndc(const br_view3d_state *v, br_vec2 local)
{
    return br_vec2_make(local.x / v->width * 2.0f - 1.0f, 1.0f - local.y / v->height * 2.0f);
}

static br_ray3d br_view3d_local_ray(br_engine *e, br_view3d_state *v, br_vec2 local)
{
    br_view3d_compute_camera(e, v);
    br_mat4 inverse = br_mat4_invert(v->view_projection, NULL);
    br_vec2 ndc = br_view3d_local_to_ndc(v, local);
    return br_ray3d_from_ndc(inverse, ndc.x, ndc.y);
}

br_ray3d br_view3d_screen_to_ray(br_engine *e, br_node view, br_vec2 global_point)
{
    br_ray3d none;
    SDL_zero(none);
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL) {
        return none;
    }
    return br_view3d_local_ray(e, v, br_node_to_local(e, view, global_point));
}

bool br_view3d_world_to_screen(br_engine *e, br_node view, br_vec3 world, br_vec2 *global_out)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL) {
        return false;
    }
    br_view3d_compute_camera(e, v);
    br_vec4 clip = br_mat4_transform_vec4(v->view_projection, br_vec4_make(world.x, world.y, world.z, 1.0f));
    if (clip.w <= 1e-6f) {
        return false;
    }
    br_vec2 local = br_vec2_make((clip.x / clip.w * 0.5f + 0.5f) * v->width, (0.5f - clip.y / clip.w * 0.5f) * v->height);
    if (global_out != NULL) {
        *global_out = br_node_to_global(e, view, local);
    }
    return true;
}

/* ---- raycast */

static bool br_raycast_masked(br_engine *e, br_node3d root, br_ray3d ray, uint32_t mask, br_pick3d_hit *out)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, root)) {
        return false;
    }
    br_three_d_update_world(e, root);
    br_node3d_table *t = &s->nodes;
    br_pick3d_hit best;
    SDL_zero(best);
    best.distance = FLT_MAX;
    bool found = false;
    uint32_t r = root.index;
    uint32_t i = r;
    while (i != BR_NONE3D) {
        bool descend = t->visible[i] != 0;
        br_node3d_type_entry *type = br_array_at(&s->types, t->type[i] - 1);
        if (descend && t->pickable[i] && (t->layer[i] & mask) != 0 && type != NULL && type->desc.pick != NULL) {
            bool ok = false;
            br_mat4 inverse = br_mat4_invert(t->world[i], &ok);
            if (ok) {
                br_ray3d local;
                local.origin = br_mat4_transform_point(inverse, ray.origin);
                local.direction = br_mat4_transform_dir(inverse, ray.direction); /* unnormalised: t stays world t */
                br_pick3d_hit h = best;
                br_node3d n = { i, t->generation[i] };
                void *data = type->desc.data_size > 0 ? br_array_at(&type->data, t->data_index[i]) : NULL;
                if (type->desc.pick(e, n, data, local, &h) && h.distance < best.distance) {
                    best = h;
                    best.node = n;
                    best.point = br_mat4_transform_point(t->world[i], h.point);
                    best.normal = br_vec3_normalize(br_mat4_transform_dir(t->world[i], h.normal));
                    found = true;
                }
            }
        }
        if (descend && t->first_child[i] != BR_NONE3D) {
            i = t->first_child[i];
            continue;
        }
        while (i != r && t->next_sibling[i] == BR_NONE3D) {
            i = t->parent[i];
        }
        i = (i == r) ? BR_NONE3D : t->next_sibling[i];
    }
    if (found && out != NULL) {
        *out = best;
    }
    return found;
}

bool br_view3d_raycast(br_engine *e, br_node3d root, br_ray3d ray, br_pick3d_hit *hit)
{
    return br_raycast_masked(e, root, ray, 0xFFFFFFFFu, hit);
}

static bool br_view3d_pick_local(br_engine *e, br_view3d_state *v, br_vec2 local, br_pick3d_hit *hit)
{
    if (local.x < 0.0f || local.y < 0.0f || local.x >= v->width || local.y >= v->height) {
        return false;
    }
    return br_raycast_masked(e, v->root, br_view3d_local_ray(e, v, local), v->culling_mask, hit);
}

bool br_view3d_pick(br_engine *e, br_node view, br_vec2 global_point, br_pick3d_hit *hit)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL) {
        return false;
    }
    return br_view3d_pick_local(e, v, br_node_to_local(e, view, global_point), hit);
}

/* ---- 3D listeners */

void br_node3d_on(br_engine *e, br_node3d n, br_event_type type, br_event3d_fn fn, void *user)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n) || fn == NULL) {
        return;
    }
    br_listener3d l;
    SDL_zero(l);
    l.node = n;
    l.type = type;
    l.fn = fn;
    l.user = user;
    br_array_push(&s->listeners, &l);
}

static void br_listeners3d_compact(br_three_d_state *s)
{
    if (s->dispatch_depth > 0) {
        return;
    }
    for (uint32_t i = s->listeners.count; i > 0; i--) {
        br_listener3d *l = br_array_at(&s->listeners, i - 1);
        if (l->removed) {
            br_array_remove_ordered(&s->listeners, i - 1);
        }
    }
}

void br_node3d_off(br_engine *e, br_node3d n, br_event_type type, br_event3d_fn fn, void *user)
{
    br_three_d_state *s = e->three_d;
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->listeners.count; i++) {
        br_listener3d *l = br_array_at(&s->listeners, i);
        if (l->node.index == n.index && l->node.generation == n.generation && l->type == type && l->fn == fn &&
            l->user == user) {
            l->removed = true;
        }
    }
    br_listeners3d_compact(s);
}

void br_pick3d_node_destroyed(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    for (uint32_t i = 0; i < s->listeners.count; i++) {
        br_listener3d *l = br_array_at(&s->listeners, i);
        if (l->node.index == n.index && l->node.generation == n.generation) {
            l->removed = true;
        }
    }
    br_listeners3d_compact(s);
}

static bool br_node3d_has_listener(br_three_d_state *s, uint32_t index)
{
    for (uint32_t i = 0; i < s->listeners.count; i++) {
        const br_listener3d *l = br_array_at(&s->listeners, i);
        if (!l->removed && l->node.index == index && l->node.generation == s->nodes.generation[index]) {
            return true;
        }
    }
    return false;
}

/* True if n or one of its ancestors listens to anything. */
static bool br_node3d_listened(br_three_d_state *s, br_node3d n)
{
    if (!br_node3d_index_valid(s, n)) {
        return false;
    }
    for (uint32_t i = n.index; i != BR_NONE3D; i = s->nodes.parent[i]) {
        if (br_node3d_has_listener(s, i)) {
            return true;
        }
    }
    return false;
}

/* Runs type listeners on target, then on each 3D ancestor (bubbling) until stopped. */
static void br_dispatch3d(br_engine *e, br_view3d_state *v, br_event *source, br_event_type type, br_node3d target,
                          const br_pick3d_hit *hit)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, target)) {
        return;
    }
    br_event3d ev;
    SDL_zero(ev);
    ev.source = source;
    ev.view = v->node;
    ev.target = target;
    if (hit != NULL) {
        ev.hit = *hit;
    }
    br_event_type original = source->type;
    source->type = type; /* listeners read the 3D type from ev->source->type too */
    s->dispatch_depth++;
    br_node3d current = target;
    while (br_node3d_index_valid(s, current) && !ev.propagation_stopped) {
        ev.current_target = current;
        uint32_t count = s->listeners.count; /* rows added during dispatch run next time */
        for (uint32_t i = 0; i < count; i++) {
            br_listener3d l = *(br_listener3d *)br_array_at(&s->listeners, i);
            if (!l.removed && l.type == type && l.node.index == current.index && l.node.generation == current.generation) {
                l.fn(e, &ev, l.user);
            }
        }
        current = br_node3d_handle(s, br_node3d_index_valid(s, current) ? s->nodes.parent[current.index] : BR_NONE3D);
    }
    s->dispatch_depth--;
    source->type = original;
    br_listeners3d_compact(s);
}

/* The 2D listener on every view: re-picks and forwards to 3D listeners. */
static void br_view3d_event(br_engine *e, br_event *ev, void *user)
{
    br_view3d_state *v = user;
    br_three_d_state *s = e->three_d;
    if (!br_node_equal(ev->target, v->node) || s->listeners.count == 0) {
        return;
    }
    br_pick3d_hit hit;
    SDL_zero(hit);
    bool found = br_view3d_pick(e, v->node, ev->global, &hit);
    br_node3d none = { 0, 0 };
    br_node3d target = found ? hit.node : none;
    switch (ev->type) {
    case BR_EVENT_POINTER_OVER:
    case BR_EVENT_POINTER_MOVE:
        if (target.index != v->hovered.index || target.generation != v->hovered.generation) {
            br_dispatch3d(e, v, ev, BR_EVENT_POINTER_OUT, v->hovered, NULL);
            v->hovered = target;
            br_dispatch3d(e, v, ev, BR_EVENT_POINTER_OVER, target, &hit);
        }
        if (ev->type == BR_EVENT_POINTER_MOVE) {
            br_dispatch3d(e, v, ev, BR_EVENT_POINTER_MOVE, target, &hit);
        }
        break;
    case BR_EVENT_POINTER_OUT:
        br_dispatch3d(e, v, ev, BR_EVENT_POINTER_OUT, v->hovered, NULL);
        v->hovered = none;
        break;
    case BR_EVENT_POINTER_DOWN:
        v->pressed = target;
        br_dispatch3d(e, v, ev, BR_EVENT_POINTER_DOWN, target, &hit);
        break;
    case BR_EVENT_POINTER_UP:
        br_dispatch3d(e, v, ev, BR_EVENT_POINTER_UP, target, &hit);
        if (br_node3d_index_valid(s, v->pressed) && v->pressed.index != target.index) {
            br_dispatch3d(e, v, ev, BR_EVENT_POINTER_UP_OUTSIDE, v->pressed, NULL);
        }
        break;
    case BR_EVENT_POINTER_UP_OUTSIDE:
        br_dispatch3d(e, v, ev, BR_EVENT_POINTER_UP_OUTSIDE, v->pressed, NULL);
        v->pressed = none;
        break;
    case BR_EVENT_CLICK:
    case BR_EVENT_RIGHT_CLICK:
    case BR_EVENT_TAP:
        if (br_node3d_index_valid(s, target) && target.index == v->pressed.index &&
            target.generation == v->pressed.generation) {
            br_dispatch3d(e, v, ev, ev->type, target, &hit);
        }
        v->pressed = none;
        break;
    case BR_EVENT_WHEEL:
        br_dispatch3d(e, v, ev, BR_EVENT_WHEEL, target, &hit);
        break;
    default:
        break;
    }
}

void br_pick3d_attach_view(br_engine *e, br_view3d_state *v)
{
    static const br_event_type types[] = { BR_EVENT_POINTER_OVER, BR_EVENT_POINTER_MOVE, BR_EVENT_POINTER_OUT,
                                           BR_EVENT_POINTER_DOWN, BR_EVENT_POINTER_UP, BR_EVENT_POINTER_UP_OUTSIDE,
                                           BR_EVENT_CLICK, BR_EVENT_RIGHT_CLICK, BR_EVENT_TAP, BR_EVENT_WHEEL };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(types); i++) {
        br_node_on(e, v->node, types[i], br_view3d_event, v);
    }
}

bool br_pick3d_view_hit_test(br_engine *e, br_view3d_state *v, br_vec2 local_point)
{
    br_three_d_state *s = e->three_d;
    if (s->listeners.count == 0) {
        return false; /* nothing listens: let the pointer through to 2D nodes below */
    }
    br_pick3d_hit hit;
    if (!br_view3d_pick_local(e, v, local_point, &hit)) {
        return false;
    }
    /* keep 3D hover/out working: once something here is hovered, the view keeps the pointer
     * while it is over any 3D node */
    return br_node3d_listened(s, hit.node) || br_node3d_index_valid(s, v->hovered);
}
