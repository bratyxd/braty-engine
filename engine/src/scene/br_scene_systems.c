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

static void br_transform_update(br_scene_state *s, uint32_t index, const br_parent_state *parent)
{
    br_node_record *rec = br_scene_record_at(s, index);
    br_node_transform *t = br_scene_transform_at(s, index);
    if (rec->destroyed) {
        return;
    }
    bool moved = parent->moved || rec->transform_dirty;
    bool recolored = parent->recolored || rec->color_dirty;
    bool visit_children = moved || recolored || rec->child_dirty;
    if (rec->transform_dirty) {
        t->local = br_scene_compute_local(t);
    }
    if (moved) {
        t->world = parent->is_group ? t->local : br_mat3_mul(parent->world, t->local);
    }
    if (recolored) {
        rec->world_alpha = parent->alpha * rec->alpha;
        rec->world_tint = br_color_multiply(parent->tint, rec->tint);
        rec->world_color = br_render_pack_color(rec->world_tint, rec->world_alpha);
    }
    rec->transform_dirty = false;
    rec->color_dirty = false;
    rec->child_dirty = false;
    if (!visit_children || rec->children.count == 0) {
        return;
    }
    br_parent_state me;
    me.world = t->world;
    me.alpha = rec->world_alpha;
    me.tint = rec->world_tint;
    /* a render group's children are relative to it: its own movement does not touch them */
    me.moved = moved && !rec->render_group;
    me.recolored = recolored;
    me.is_group = rec->render_group;
    const uint32_t *children = rec->children.data;
    for (uint32_t i = 0; i < rec->children.count; i++) {
        br_transform_update(s, children[i], &me);
    }
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

void br_scene_bounds(br_scene_state *s, br_engine *e, uint32_t index, br_mat3 m, br_rect *acc, bool *has)
{
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->destroyed || !rec->visible) {
        return;
    }
    br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
    br_rect local;
    if (type->desc.local_bounds != NULL &&
        type->desc.local_bounds(e, br_scene_handle(s, index), br_scene_type_data(s, index), &local)) {
        br_rect r = br_rect_transform_bounds(local, m);
        *acc = *has ? br_rect_union(*acc, r) : r;
        *has = true;
    }
    rec = br_scene_record_at(s, index);
    for (uint32_t i = 0; i < rec->children.count; i++) {
        uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
        if (br_scene_record_at(s, child)->mask_users > 0) {
            continue; /* masks clip, they do not add to their parent's bounds */
        }
        br_mat3 child_m = br_mat3_mul(m, br_scene_compute_local(br_scene_transform_at(s, child)));
        br_scene_bounds(s, e, child, child_m, acc, has);
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
    if (w <= 0 || h <= 0) {
        return;
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

void br_scene_add_systems(br_engine *e)
{
    br_system_add(e, "scene.flush_destroyed", BR_PHASE_INPUT, -1000000, br_flush_system, NULL);
    br_system_add(e, "scene.transform", BR_PHASE_PRE_RENDER, -1000, br_transform_system, NULL);
    br_system_add(e, "scene.sort", BR_PHASE_PRE_RENDER, -900, br_sort_system, NULL);
    br_system_add(e, "scene.cull", BR_PHASE_PRE_RENDER, -800, br_cull_system, NULL);
}
