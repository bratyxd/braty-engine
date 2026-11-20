/* Engine systems of the scene: destroy flush, transform update (dirty only), z-sort, culling.
 * Plus bounds and z-sorting helpers shared with the renderer. */
#include "braty/br_render.h"
#include "scene/br_scene_internal.h"

/* ---- z-sorting (stable: equal z keeps the current order) */

typedef struct br_sort_item {
    int      z;
    uint32_t order;
    br_node  handle;
} br_sort_item;

static int br_sort_item_compare(const void *pa, const void *pb)
{
    const br_sort_item *a = pa;
    const br_sort_item *b = pb;
    if (a->z != b->z) {
        return a->z < b->z ? -1 : 1;
    }
    if (a->order != b->order) {
        return a->order < b->order ? -1 : 1;
    }
    return 0;
}

static int br_sort_z_of(br_scene_state *s, uint32_t index)
{
    return br_scene_record_at(s, index)->z_index;
}

/* Makes room for `count` items in the scratch array. */
static bool br_sort_prepare(br_scene_state *s, uint32_t count)
{
    if (s->sort_scratch.elem_size == 0) {
        br_array_init(&s->sort_scratch, sizeof(br_sort_item));
    }
    return br_array_resize(&s->sort_scratch, count);
}

void br_scene_sort_handles_by_z(br_scene_state *s, br_node *handles, uint32_t count)
{
    bool sorted = true;
    for (uint32_t i = 1; i < count && sorted; i++) {
        sorted = br_sort_z_of(s, handles[i - 1].index) <= br_sort_z_of(s, handles[i].index);
    }
    if (sorted || !br_sort_prepare(s, count)) {
        return;
    }
    br_sort_item *items = s->sort_scratch.data;
    for (uint32_t i = 0; i < count; i++) {
        items[i].z = br_sort_z_of(s, handles[i].index);
        items[i].order = i;
        items[i].handle = handles[i];
    }
    SDL_qsort(items, count, sizeof(br_sort_item), br_sort_item_compare);
    for (uint32_t i = 0; i < count; i++) {
        handles[i] = items[i].handle;
    }
}

void br_scene_sort_by_z(br_scene_state *s, uint32_t *indices, uint32_t count)
{
    bool sorted = true;
    for (uint32_t i = 1; i < count && sorted; i++) {
        sorted = br_sort_z_of(s, indices[i - 1]) <= br_sort_z_of(s, indices[i]);
    }
    if (sorted || !br_sort_prepare(s, count)) {
        return;
    }
    br_sort_item *items = s->sort_scratch.data;
    for (uint32_t i = 0; i < count; i++) {
        items[i].z = br_sort_z_of(s, indices[i]);
        items[i].order = i;
        items[i].handle.index = indices[i];
        items[i].handle.generation = 0;
    }
    SDL_qsort(items, count, sizeof(br_sort_item), br_sort_item_compare);
    for (uint32_t i = 0; i < count; i++) {
        indices[i] = items[i].handle.index;
    }
}

/* ---- transform update */

typedef struct br_parent_state {
    br_mat3  world;
    float    alpha;
    br_color tint;
    bool     moved;      /* parent world changed this update */
    bool     recolored;  /* parent world alpha/tint changed */
    bool     is_group;   /* parent is a render group: children are relative to it */
} br_parent_state;

/* One node waiting for the transform update: its index and the state of its parent (an index
 * into transform_parents: siblings share one copy). */
typedef struct br_transform_item {
    uint32_t index;
    uint32_t parent;
} br_transform_item;

/* Explicit stack instead of recursion, so hierarchies of any depth work. Not re-entrant (no
 * callbacks run inside). */
static void br_transform_update(br_scene_state *s, uint32_t index, const br_parent_state *parent)
{
    br_array *stack = &s->transform_stack;
    br_array *parents = &s->transform_parents;
    br_array_clear(stack);
    br_array_clear(parents);
    br_transform_item first = { index, 0 };
    if (br_array_push(parents, parent) == NULL || br_array_push(stack, &first) == NULL) {
        return;
    }
    while (stack->count > 0) {
        br_transform_item item = *(br_transform_item *)br_array_at(stack, stack->count - 1);
        br_array_pop(stack);
        const br_parent_state *ps = br_array_at(parents, item.parent);
        br_node_record *rec = br_scene_record_at(s, item.index);
        br_node_transform *t = br_scene_transform_at(s, item.index);
        if (rec->destroyed) {
            continue;
        }
        bool moved = ps->moved || rec->transform_dirty;
        bool recolored = ps->recolored || rec->color_dirty;
        bool visit_children = moved || recolored || rec->child_dirty;
        if (rec->transform_dirty) {
            t->local = br_scene_compute_local(t);
        }
        if (moved) {
            t->world = ps->is_group ? t->local : br_mat3_mul(ps->world, t->local);
        }
        if (recolored) {
            rec->world_alpha = ps->alpha * rec->alpha;
            rec->world_tint = br_color_multiply(ps->tint, rec->tint);
            rec->world_color = br_render_pack_color(rec->world_tint, rec->world_alpha);
        }
        rec->transform_dirty = false;
        rec->color_dirty = false;
        rec->child_dirty = false;
        if (!visit_children || rec->children.count == 0) {
            continue;
        }
        br_parent_state me;
        me.world = t->world;
        me.alpha = rec->world_alpha;
        me.tint = rec->world_tint;
        /* a render group's children are relative to it: its own movement does not touch them */
        me.moved = moved && !rec->render_group;
        me.recolored = recolored;
        me.is_group = rec->render_group;
        br_transform_item child;
        child.parent = parents->count;
        if (br_array_push(parents, &me) == NULL) {
            return;
        }
        const uint32_t *children = rec->children.data;
        for (uint32_t i = 0; i < rec->children.count; i++) {
            child.index = children[i];
            if (br_array_push(stack, &child) == NULL) {
                return;
            }
        }
    }
}

void br_scene_refresh_subtree_uncolored(br_scene_state *s, uint32_t index)
{
    br_node_record *rec = br_scene_record_at(s, index);
    float alpha = rec->alpha;
    br_color tint = rec->tint;
    rec->alpha = 1.0f;
    rec->tint = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    br_parent_state parent;
    parent.world = br_mat3_identity();
    parent.alpha = 1.0f;
    parent.tint = rec->tint;
    parent.moved = true;
    parent.recolored = true;
    parent.is_group = false;
    uint32_t p = rec->parent;
    if (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        parent.is_group = prec->render_group;
        if (!prec->render_group) {
            parent.world = br_scene_chain_transform(s, p, true);
        }
    }
    br_transform_update(s, index, &parent);
    rec = br_scene_record_at(s, index); /* no callbacks ran: same pointer, fetched for clarity */
    rec->alpha = alpha;
    rec->tint = tint;
}

void br_scene_refresh_subtree(br_scene_state *s, uint32_t index)
{
    br_parent_state parent;
    parent.world = br_mat3_identity();
    parent.alpha = 1.0f;
    parent.tint = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    parent.moved = true;
    parent.recolored = true;
    parent.is_group = false;
    uint32_t p = br_scene_record_at(s, index)->parent;
    if (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        parent.is_group = prec->render_group;
        if (!prec->render_group) {
            parent.world = br_scene_chain_transform(s, p, true);
        }
        /* alpha/tint are absolute: multiply the whole chain */
        while (p != BR_NO_NODE) {
            prec = br_scene_record_at(s, p);
            parent.alpha *= prec->alpha;
            parent.tint = br_color_multiply(parent.tint, prec->tint);
            p = prec->parent;
        }
    }
    br_transform_update(s, index, &parent);
}

static void br_transform_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_scene_state *s = e->scene;
    br_parent_state top;
    top.world = br_mat3_identity();
    top.alpha = 1.0f;
    top.tint = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    top.moved = false;
    top.recolored = false;
    top.is_group = false;
    br_transform_update(s, s->root.index, &top);
}

/* ---- z-sort */

static void br_sort_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_scene_state *s = e->scene;
    for (uint32_t i = 0; i < s->sort_queue.count; i++) {
        uint32_t index = *(uint32_t *)br_array_at(&s->sort_queue, i);
        if (!br_pool_slot_alive(&s->nodes, index)) {
            continue;
        }
        br_node_record *rec = br_scene_record_at(s, index);
        rec->sort_queued = false;
        if (rec->destroyed || !rec->sortable_children) {
            continue;
        }
        br_scene_sort_by_z(s, rec->children.data, rec->children.count);
        br_scene_sort_handles_by_z(s, rec->layer_members.data, rec->layer_members.count);
    }
    br_array_clear(&s->sort_queue);
}

/* ---- bounds and culling */

/* A node waiting for the bounds walk, with its world matrix. */
typedef struct br_bounds_item {
    uint32_t index;
    br_mat3  m;
} br_bounds_item;

void br_scene_bounds(br_scene_state *s, br_engine *e, uint32_t index, br_mat3 m, br_rect *acc, bool *has)
{
    /* explicit stack (any depth). local_bounds callbacks may ask for bounds again: each call
     * only pops what it pushed above `base` */
    br_array *stack = &s->bounds_stack;
    uint32_t base = stack->count;
    br_bounds_item first = { index, m };
    if (br_array_push(stack, &first) == NULL) {
        return;
    }
    while (stack->count > base) {
        br_bounds_item item = *(br_bounds_item *)br_array_at(stack, stack->count - 1);
        br_array_pop(stack);
        br_node_record *rec = br_scene_record_at(s, item.index);
        if (rec->destroyed || !rec->visible) {
            continue;
        }
        br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
        br_rect local;
        if (type->desc.local_bounds != NULL &&
            type->desc.local_bounds(e, br_scene_handle(s, item.index), br_scene_type_data(s, item.index), &local)) {
            br_rect r = br_rect_transform_bounds(local, item.m);
            *acc = *has ? br_rect_union(*acc, r) : r;
            *has = true;
        }
        rec = br_scene_record_at(s, item.index);
        for (uint32_t i = 0; i < rec->children.count; i++) {
            uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
            if (br_scene_record_at(s, child)->mask_users > 0) {
                continue; /* masks clip, they do not add to their parent's bounds */
            }
            br_bounds_item next;
            next.index = child;
            next.m = br_mat3_mul(item.m, br_scene_compute_local(br_scene_transform_at(s, child)));
            if (br_array_push(stack, &next) == NULL) {
                stack->count = base;
                return;
            }
        }
    }
}

void br_scene_set_cull_view(br_scene_state *s, const br_rect *view)
{
    s->has_cull_view = view != NULL;
    if (view != NULL) {
        s->cull_view = *view;
    }
}

bool br_scene_is_culled(br_scene_state *s, uint32_t index)
{
    return s->cull_frame != 0 && br_scene_record_at(s, index)->culled_frame == s->cull_frame;
}

void br_scene_cullable_add(br_scene_state *s, uint32_t index)
{
    br_scene_record_at(s, index)->cull_slot = s->cullables.count;
    br_array_push(&s->cullables, &index);
}

void br_scene_cullable_remove(br_scene_state *s, uint32_t index)
{
    /* swap-remove: the last entry takes the removed one's place */
    uint32_t *list = s->cullables.data;
    uint32_t slot = br_scene_record_at(s, index)->cull_slot;
    if (slot >= s->cullables.count || list[slot] != index) {
        return;
    }
    uint32_t last = list[s->cullables.count - 1];
    list[slot] = last;
    br_scene_record_at(s, last)->cull_slot = slot;
    br_array_pop(&s->cullables);
}

/* True if the culler reaches the node: on the stage, it and its ancestors visible and every
 * ancestor has cullable_children. Also returns the space its cached world matrix is in. */
static bool br_cull_reachable(br_scene_state *s, uint32_t index, br_mat3 *group_space)
{
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->destroyed || !rec->visible) {
        return false;
    }
    uint32_t group = BR_NO_NODE;
    uint32_t p = rec->parent;
    while (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        if (!prec->visible || !prec->cullable_children) {
            return false;
        }
        if (prec->render_group && group == BR_NO_NODE) {
            group = p;
        }
        index = p;
        p = prec->parent;
    }
    if (index != s->root.index) {
        return false;
    }
    *group_space = group != BR_NO_NODE ? br_scene_chain_transform(s, group, false) : br_mat3_identity();
    return true;
}

static void br_cull_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_scene_state *s = e->scene;
    int w, h;
    br_engine_window_size(e, &w, &h);
    s->cull_frame += 1;
    if (!s->has_cull_view && (w <= 0 || h <= 0)) {
        return; /* no window and no camera / headless screen (br_render_debug_set_screen) */
    }
    br_rect screen = s->has_cull_view ? s->cull_view : br_rect_make(0.0f, 0.0f, (float)w, (float)h);
    /* only the cullable nodes are visited (not the whole tree). A cullable node inside a culled
     * one is tested on its own bounds; it is not drawn either way. */
    for (uint32_t i = 0; i < s->cullables.count; i++) {
        uint32_t index = ((const uint32_t *)s->cullables.data)[i];
        br_mat3 group_space;
        if (!br_cull_reachable(s, index, &group_space)) {
            continue;
        }
        br_node_record *rec = br_scene_record_at(s, index);
        br_mat3 world = br_mat3_mul(group_space, br_scene_transform_at(s, index)->world);
        br_rect bounds = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
        bool has = false;
        if (rec->has_cull_area) {
            bounds = br_rect_transform_bounds(rec->cull_area, world);
            has = true;
        } else {
            br_scene_bounds(s, e, index, world, &bounds, &has);
        }
        if (has && !br_rect_intersects(bounds, screen)) {
            br_scene_record_at(s, index)->culled_frame = s->cull_frame;
        }
    }
}

static void br_flush_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_scene_flush_destroyed(e);
}

void br_scene_walks_init(br_scene_state *s)
{
    br_array_init(&s->transform_stack, sizeof(br_transform_item));
    br_array_init(&s->transform_parents, sizeof(br_parent_state));
    br_array_init(&s->bounds_stack, sizeof(br_bounds_item));
}

void br_scene_add_systems(br_engine *e)
{
    br_system_add(e, "scene.flush_destroyed", BR_PHASE_INPUT, -1000000, br_flush_system, NULL);
    br_system_add(e, "scene.transform", BR_PHASE_PRE_RENDER, -1000, br_transform_system, NULL);
    br_system_add(e, "scene.sort", BR_PHASE_PRE_RENDER, -900, br_sort_system, NULL);
    br_system_add(e, "scene.cull", BR_PHASE_PRE_RENDER, -800, br_cull_system, NULL);
}
