/* br_node: node table, node types, lifetime (deferred destroy), hierarchy, node fields. */
#include "scene/br_scene_internal.h"

#include <math.h>

/* ---- table access */

br_node br_scene_handle(br_scene_state *s, uint32_t index)
{
    br_node n;
    n.index = index;
    n.generation = br_pool_slot_generation(&s->nodes, index);
    return n;
}

br_node_record *br_scene_record(br_engine *e, br_node n)
{
    if (e->scene == NULL) {
        return NULL;
    }
    /* br_pool_get without the function calls: every node API call goes through here */
    const br_pool *p = &e->scene->nodes;
    if (n.generation == 0 || n.index >= p->items.count) {
        return NULL;
    }
    const uint8_t *alive = p->alive.data;
    const uint32_t *generations = p->generations.data;
    if (alive[n.index] == 0 || generations[n.index] != n.generation) {
        return NULL;
    }
    br_node_record *rec = br_scene_record_at(e->scene, n.index);
    return rec->destroyed ? NULL : rec;
}

br_node_type_entry *br_scene_type_entry(br_scene_state *s, uint32_t type_id)
{
    if (type_id == 0 || type_id > s->types.count) {
        return NULL;
    }
    return (br_node_type_entry *)s->types.data + (type_id - 1);
}

void *br_scene_type_data(br_scene_state *s, uint32_t index)
{
    br_node_record *rec = br_scene_record_at(s, index);
    br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
    if (type == NULL || type->desc.data_size == 0) {
        return NULL;
    }
    return (uint8_t *)type->data.data + (size_t)rec->data_index * type->data.elem_size;
}

static br_node br_node_zero(void)
{
    br_node n = { 0, 0 };
    return n;
}

/* ---- dirty flags */

/* Flags a node and tells every ancestor that something below changed, so the transform
 * system can skip clean subtrees. */
static void br_scene_mark_dirty(br_scene_state *s, uint32_t index, bool transform, bool color)
{
    br_node_record *rec = br_scene_record_at(s, index);
    if (transform) {
        rec->transform_dirty = true;
    }
    if (color) {
        rec->color_dirty = true;
    }
    uint32_t p = rec->parent;
    while (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        if (prec->child_dirty) {
            break;
        }
        prec->child_dirty = true;
        p = prec->parent;
    }
}

static void br_scene_queue_sort(br_scene_state *s, uint32_t index)
{
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->sortable_children && !rec->sort_queued) {
        rec->sort_queued = true;
        br_array_push(&s->sort_queue, &index);
    }
}

/* ---- transforms */

void br_scene_set_rotation(br_node_transform *t, float radians)
{
    if (radians != t->rotation) {
        /* C's cosf/sinf: same results as SDL_cosf/SDL_sinf here, about twice as fast (MinGW) */
        t->rotation_cos = cosf(radians);
        t->rotation_sin = sinf(radians);
    }
    t->rotation = radians;
}

br_mat3 br_scene_compute_local(const br_node_transform *t)
{
    br_mat3 m;
    if (t->skew.x == 0.0f && t->skew.y == 0.0f) {
        /* common case: no trig at all, cos/sin are cached with the rotation */
        float cr = t->rotation_cos;
        float sr = t->rotation_sin;
        m.a = cr * t->scale.x;
        m.b = sr * t->scale.x;
        m.c = -sr * t->scale.y;
        m.d = cr * t->scale.y;
        m.tx = t->position.x - (t->pivot.x * m.a + t->pivot.y * m.c);
        m.ty = t->position.y - (t->pivot.x * m.b + t->pivot.y * m.d);
    } else {
        m = br_mat3_from_transform(t->position.x, t->position.y, t->pivot.x, t->pivot.y, t->scale.x, t->scale.y,
                                   t->rotation, t->skew.x, t->skew.y);
    }
    /* origin: rotate/scale around it without moving the node (Pixi updateLocalTransform):
     * the local origin point stays at position + origin */
    m.tx += t->origin.x - (t->origin.x * m.a + t->origin.y * m.c);
    m.ty += t->origin.y - (t->origin.x * m.b + t->origin.y * m.d);
    return m;
}

br_mat3 br_scene_chain_transform(br_scene_state *s, uint32_t index, bool stop_at_group)
{
    br_mat3 m = br_scene_compute_local(br_scene_transform_at(s, index));
    uint32_t p = br_scene_record_at(s, index)->parent;
    while (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        if (stop_at_group && prec->render_group) {
            break;
        }
        m = br_mat3_mul(br_scene_compute_local(br_scene_transform_at(s, p)), m);
        p = prec->parent;
    }
    return m;
}

br_mat3 br_scene_group_space(br_scene_state *s, uint32_t index)
{
    uint32_t p = br_scene_record_at(s, index)->parent;
    while (p != BR_NO_NODE) {
        br_node_record *prec = br_scene_record_at(s, p);
        if (prec->render_group) {
            return br_scene_chain_transform(s, p, false);
        }
        p = prec->parent;
    }
    return br_mat3_identity();
}

/* ---- node types */

uint32_t br_node_type_register(br_engine *e, const br_node_type_desc *desc)
{
    br_scene_state *s = e->scene;
    if (desc == NULL || desc->name == NULL || desc->name[0] == 0) {
        br_log_error("br_node_type_register: a type needs a name");
        return 0;
    }
    if (br_node_type_find(e, desc->name) != 0) {
        br_log_error("node type '%s' is already registered", desc->name);
        return 0;
    }
    br_node_type_entry entry;
    SDL_zero(entry);
    entry.desc = *desc;
    entry.name = br_strdup(desc->name);
    entry.desc.name = entry.name;
    br_array_init(&entry.data, desc->data_size > 0 ? (uint32_t)desc->data_size : 1);
    br_array_init(&entry.owners, sizeof(uint32_t));
    if (entry.name == NULL || br_array_push(&s->types, &entry) == NULL) {
        br_free(entry.name);
        return 0;
    }
    return s->types.count;
}

uint32_t br_node_type_find(br_engine *e, const char *name)
{
    br_scene_state *s = e->scene;
    for (uint32_t i = 0; i < s->types.count; i++) {
        br_node_type_entry *t = br_array_at(&s->types, i);
        if (br_str_equal(t->name, name)) {
            return i + 1;
        }
    }
    return 0;
}

const br_node_type_desc *br_node_type_get(br_engine *e, uint32_t type_id)
{
    br_node_type_entry *t = br_scene_type_entry(e->scene, type_id);
    return t != NULL ? &t->desc : NULL;
}

uint32_t br_node_type_count(br_engine *e)
{
    return e->scene->types.count;
}

void br_node_type_set_from_json(br_engine *e, uint32_t type_id,
                                bool (*from_json)(br_engine *e, br_node n, void *data, const cJSON *json))
{
    br_node_type_entry *t = br_scene_type_entry(e->scene, type_id);
    if (t != NULL) {
        t->desc.from_json = from_json;
    }
}

/* ---- lifetime */

br_node br_node_create(br_engine *e, uint32_t type_id)
{
    br_scene_state *s = e->scene;
    br_node_type_entry *type = br_scene_type_entry(s, type_id);
    if (type == NULL) {
        br_log_error("br_node_create: unknown node type %u", type_id);
        return br_node_zero();
    }
    br_node n;
    br_node_record *rec = br_pool_alloc(&s->nodes, &n.index, &n.generation);
    if (rec == NULL) {
        return br_node_zero();
    }
    if (s->transforms.count < br_pool_capacity(&s->nodes) &&
        !br_array_resize(&s->transforms, br_pool_capacity(&s->nodes))) {
        br_pool_release(&s->nodes, n.index, n.generation);
        return br_node_zero();
    }
    rec->parent = BR_NO_NODE;
    br_array_init(&rec->children, sizeof(uint32_t));
    br_array_init(&rec->layer_members, sizeof(br_node));
    rec->type_id = type_id;
    rec->transform_dirty = true;
    rec->color_dirty = true;
    rec->visible = true;
    rec->renderable = true;
    rec->layer_bits = 1u;
    rec->cullable_children = true;
    rec->alpha = 1.0f;
    rec->tint = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    rec->world_alpha = 1.0f;
    rec->world_tint = rec->tint;
    rec->world_color = 0xFFFFFFFFu;
    rec->interactive_children = true;
    rec->cursor = -1;

    br_node_transform *t = br_scene_transform_at(s, n.index);
    SDL_zerop(t);
    t->scale = br_vec2_make(1.0f, 1.0f);
    t->local = br_mat3_identity();
    t->world = br_mat3_identity();
    t->rotation_cos = 1.0f;

    void *data = NULL;
    if (type->desc.data_size > 0) {
        rec->data_index = type->data.count;
        data = br_array_push(&type->data, NULL);
        br_array_push(&type->owners, &n.index);
    }
    if (type->desc.on_create != NULL) {
        type->desc.on_create(e, n, data);
    }
    return n;
}

br_node br_node_create_container(br_engine *e)
{
    return br_node_create(e, BR_NODE_TYPE_CONTAINER);
}

void *br_node_data(br_engine *e, br_node n, uint32_t type_id)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || rec->type_id != type_id) {
        return NULL;
    }
    return br_scene_type_data(e->scene, n.index);
}

uint32_t br_node_type_of(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->type_id : 0;
}

bool br_node_valid(br_engine *e, br_node n)
{
    return br_scene_record(e, n) != NULL;
}

bool br_node_equal(br_node a, br_node b)
{
    return a.index == b.index && a.generation == b.generation;
}

static void br_scene_mark_destroyed(br_scene_state *s, uint32_t index)
{
    br_node_record *rec = br_scene_record_at(s, index);
    rec->destroyed = true;
    br_array_push(&s->pending_destroy, &index);
    for (uint32_t i = 0; i < rec->children.count; i++) {
        uint32_t *child = br_array_at(&rec->children, i);
        if (!br_scene_record_at(s, *child)->destroyed) {
            br_scene_mark_destroyed(s, *child);
        }
    }
}

void br_node_destroy(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return;
    }
    if (br_node_equal(n, e->scene->root)) {
        br_log_error("br_node_destroy: the stage cannot be destroyed");
        return;
    }
    br_scene_mark_destroyed(e->scene, n.index);
    if (!e->stepping) {
        br_scene_flush_destroyed(e);
    }
}

static void br_remove_index(br_array *list, uint32_t value)
{
    for (uint32_t i = 0; i < list->count; i++) {
        uint32_t *v = br_array_at(list, i);
        if (*v == value) {
            br_array_remove_ordered(list, i);
            return;
        }
    }
}

static void br_remove_handle(br_array *list, br_node value)
{
    for (uint32_t i = 0; i < list->count; i++) {
        br_node *v = br_array_at(list, i);
        if (br_node_equal(*v, value)) {
            br_array_remove_ordered(list, i);
            return;
        }
    }
}

/* Frees one destroyed node: callbacks first (they may still read the node), then tables. */
static void br_scene_free_node(br_engine *e, uint32_t index)
{
    br_scene_state *s = e->scene;
    br_node n = br_scene_handle(s, index);
    br_node_record *rec = br_scene_record_at(s, index);
    br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
    if (type->desc.on_destroy != NULL) {
        type->desc.on_destroy(e, n, br_scene_type_data(s, index));
    }
    for (uint32_t i = 0; i < s->destroy_listeners.count; i++) {
        br_destroy_listener *l = br_array_at(&s->destroy_listeners, i);
        l->fn(e, n, l->user);
    }
    br_component_remove_all(s, index);

    /* callbacks may have created nodes (pool growth moves records): fetch again */
    rec = br_scene_record_at(s, index);
    type = br_scene_type_entry(s, rec->type_id);
    if (type->desc.data_size > 0) {
        uint32_t last = type->data.count - 1;
        if (rec->data_index != last) {
            SDL_memcpy(br_array_at(&type->data, rec->data_index), br_array_at(&type->data, last), type->desc.data_size);
            uint32_t *moved_owner = br_array_at(&type->owners, last);
            uint32_t moved = *moved_owner;
            *(uint32_t *)br_array_at(&type->owners, rec->data_index) = moved;
            br_scene_record_at(s, moved)->data_index = rec->data_index;
        }
        br_array_pop(&type->data);
        br_array_pop(&type->owners);
    }
    if (rec->parent != BR_NO_NODE && br_pool_slot_alive(&s->nodes, rec->parent)) {
        br_node_record *prec = br_scene_record_at(s, rec->parent);
        if (!prec->destroyed) {
            br_remove_index(&prec->children, index);
        }
    }
    br_node_record *layer = br_scene_record(e, rec->layer);
    if (layer != NULL) {
        br_remove_handle(&layer->layer_members, n);
    }
    br_node_record *mask = br_scene_record(e, rec->mask);
    if (mask != NULL && mask->mask_users > 0) {
        mask->mask_users -= 1;
    }
    for (uint32_t i = 0; i < rec->layer_members.count; i++) {
        br_node_record *member = br_scene_record(e, *(br_node *)br_array_at(&rec->layer_members, i));
        if (member != NULL) {
            member->layer = br_node_zero();
        }
    }
    if (rec->cullable) {
        br_scene_cullable_remove(s, n.index);
    }
    br_free(rec->label);
    br_free((void *)rec->hit_area.points);
    br_array_free(&rec->children);
    br_array_free(&rec->layer_members);
    br_pool_release(&s->nodes, n.index, n.generation);
}

void br_scene_flush_destroyed(br_engine *e)
{
    br_scene_state *s = e->scene;
    /* count is re-read: destroy callbacks may destroy more nodes */
    for (uint32_t i = 0; i < s->pending_destroy.count; i++) {
        uint32_t index = *(uint32_t *)br_array_at(&s->pending_destroy, i);
        br_scene_free_node(e, index);
    }
    br_array_clear(&s->pending_destroy);
}

br_node br_scene_root(br_engine *e)
{
    return e->scene->root;
}

uint32_t br_scene_node_count(br_engine *e)
{
    return e->scene->nodes.alive_count;
}

void br_scene_on_node_destroy(br_engine *e, br_node_destroy_fn fn, void *user)
{
    br_destroy_listener l = { fn, user };
    br_array_push(&e->scene->destroy_listeners, &l);
}

/* ---- hierarchy */

static void br_scene_detach(br_scene_state *s, uint32_t index)
{
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->parent == BR_NO_NODE) {
        return;
    }
    br_remove_index(&br_scene_record_at(s, rec->parent)->children, index);
    rec->parent = BR_NO_NODE;
    rec->transform_dirty = true;
    rec->color_dirty = true;
}

bool br_node_is_descendant_of(br_engine *e, br_node n, br_node ancestor)
{
    if (br_scene_record(e, n) == NULL || br_scene_record(e, ancestor) == NULL) {
        return false;
    }
    uint32_t i = n.index;
    while (i != BR_NO_NODE) {
        if (i == ancestor.index) {
            return true;
        }
        i = br_scene_record_at(e->scene, i)->parent;
    }
    return false;
}

bool br_node_add_child_at(br_engine *e, br_node parent, br_node child, uint32_t index)
{
    br_scene_state *s = e->scene;
    br_node_record *prec = br_scene_record(e, parent);
    br_node_record *crec = br_scene_record(e, child);
    if (prec == NULL || crec == NULL) {
        br_log_warn("br_node_add_child: invalid node");
        return false;
    }
    if (br_node_equal(child, s->root) || br_node_is_descendant_of(e, parent, child)) {
        br_log_error("br_node_add_child: would create a cycle");
        return false;
    }
    br_scene_detach(s, child.index);
    if (index > prec->children.count) {
        index = prec->children.count;
    }
    br_array_insert(&prec->children, index, &child.index);
    crec->parent = parent.index;
    br_scene_mark_dirty(s, child.index, true, true);
    br_scene_queue_sort(s, parent.index);
    return true;
}

bool br_node_add_child(br_engine *e, br_node parent, br_node child)
{
    return br_node_add_child_at(e, parent, child, 0xFFFFFFFFu);
}

bool br_node_remove_child(br_engine *e, br_node parent, br_node child)
{
    br_node_record *crec = br_scene_record(e, child);
    if (br_scene_record(e, parent) == NULL || crec == NULL || crec->parent != parent.index) {
        return false;
    }
    br_scene_detach(e->scene, child.index);
    return true;
}

void br_node_remove_from_parent(br_engine *e, br_node n)
{
    if (br_scene_record(e, n) != NULL) {
        br_scene_detach(e->scene, n.index);
    }
}

void br_node_remove_children(br_engine *e, br_node parent)
{
    br_node_record *prec = br_scene_record(e, parent);
    if (prec == NULL) {
        return;
    }
    while (prec->children.count > 0) {
        uint32_t child = *(uint32_t *)br_array_at(&prec->children, prec->children.count - 1);
        br_scene_detach(e->scene, child);
    }
}

bool br_node_reparent(br_engine *e, br_node n, br_node new_parent)
{
    br_scene_state *s = e->scene;
    if (br_scene_record(e, n) == NULL || br_scene_record(e, new_parent) == NULL) {
        return false;
    }
    br_mat3 world = br_node_world_transform(e, n);
    br_mat3 parent_world = br_node_world_transform(e, new_parent);
    if (!br_node_add_child(e, new_parent, n)) {
        return false;
    }
    br_mat3 local = br_mat3_mul(br_mat3_invert(parent_world, NULL), world);
    br_node_transform *t = br_scene_transform_at(s, n.index);
    br_vec2 position;
    float rotation = 0.0f;
    br_mat3_decompose(local, &position, &t->scale, &rotation, &t->skew);
    br_scene_set_rotation(t, rotation);
    /* decompose assumes no pivot/origin: undo their translation (see br_scene_compute_local) */
    br_mat3 m = br_mat3_from_transform(0.0f, 0.0f, 0.0f, 0.0f, t->scale.x, t->scale.y, t->rotation, t->skew.x, t->skew.y);
    br_vec2 pivot_offset = br_vec2_make(t->pivot.x * m.a + t->pivot.y * m.c, t->pivot.x * m.b + t->pivot.y * m.d);
    br_vec2 origin_offset = br_vec2_make(t->origin.x * m.a + t->origin.y * m.c, t->origin.x * m.b + t->origin.y * m.d);
    t->position.x = local.tx + pivot_offset.x + origin_offset.x - t->origin.x;
    t->position.y = local.ty + pivot_offset.y + origin_offset.y - t->origin.y;
    br_scene_mark_dirty(s, n.index, true, false);
    return true;
}

int br_node_child_index(br_engine *e, br_node parent, br_node child)
{
    br_node_record *prec = br_scene_record(e, parent);
    if (prec == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < prec->children.count; i++) {
        if (*(uint32_t *)br_array_at(&prec->children, i) == child.index) {
            return (int)i;
        }
    }
    return -1;
}

bool br_node_swap_children(br_engine *e, br_node parent, br_node a, br_node b)
{
    int ia = br_node_child_index(e, parent, a);
    int ib = br_node_child_index(e, parent, b);
    if (ia < 0 || ib < 0) {
        return false;
    }
    br_node_record *prec = br_scene_record(e, parent);
    *(uint32_t *)br_array_at(&prec->children, (uint32_t)ia) = b.index;
    *(uint32_t *)br_array_at(&prec->children, (uint32_t)ib) = a.index;
    return true;
}

bool br_node_set_child_index(br_engine *e, br_node parent, br_node child, uint32_t index)
{
    int current = br_node_child_index(e, parent, child);
    if (current < 0) {
        return false;
    }
    br_node_record *prec = br_scene_record(e, parent);
    br_array_remove_ordered(&prec->children, (uint32_t)current);
    if (index > prec->children.count) {
        index = prec->children.count;
    }
    br_array_insert(&prec->children, index, &child.index);
    return true;
}

uint32_t br_node_child_count(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->children.count : 0;
}

br_node br_node_child_at(br_engine *e, br_node n, uint32_t i)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || i >= rec->children.count) {
        return br_node_zero();
    }
    return br_scene_handle(e->scene, *(uint32_t *)br_array_at(&rec->children, i));
}

br_node br_node_parent(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || rec->parent == BR_NO_NODE) {
        return br_node_zero();
    }
    return br_scene_handle(e->scene, rec->parent);
}

void br_node_set_label(br_engine *e, br_node n, const char *label)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return;
    }
    br_free(rec->label);
    rec->label = br_strdup(label);
}

const char *br_node_label(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->label != NULL ? rec->label : "";
}

static uint32_t br_find_label(br_scene_state *s, uint32_t index, const char *label, bool deep)
{
    br_node_record *rec = br_scene_record_at(s, index);
    for (uint32_t i = 0; i < rec->children.count; i++) {
        uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
        br_node_record *crec = br_scene_record_at(s, child);
        if (!crec->destroyed && crec->label != NULL && SDL_strcmp(crec->label, label) == 0) {
            return child;
        }
    }
    if (deep) {
        for (uint32_t i = 0; i < rec->children.count; i++) {
            uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
            if (br_scene_record_at(s, child)->destroyed) {
                continue;
            }
            uint32_t found = br_find_label(s, child, label, true);
            if (found != BR_NO_NODE) {
                return found;
            }
        }
    }
    return BR_NO_NODE;
}

br_node br_node_find_by_label(br_engine *e, br_node root, const char *label, bool deep)
{
    if (br_scene_record(e, root) == NULL || label == NULL) {
        return br_node_zero();
    }
    uint32_t found = br_find_label(e->scene, root.index, label, deep);
    return found != BR_NO_NODE ? br_scene_handle(e->scene, found) : br_node_zero();
}

/* ---- transform fields */

static br_node_transform *br_node_transform_for(br_engine *e, br_node n)
{
    if (br_scene_record(e, n) == NULL) {
        return NULL;
    }
    return br_scene_transform_at(e->scene, n.index);
}

/* Shared tail of every transform setter. */
static void br_node_transform_changed(br_engine *e, br_node n)
{
    br_scene_mark_dirty(e->scene, n.index, true, false);
}

void br_node_set_position(br_engine *e, br_node n, float x, float y)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        t->position = br_vec2_make(x, y);
        br_node_transform_changed(e, n);
    }
}

br_vec2 br_node_position(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->position : br_vec2_make(0.0f, 0.0f);
}

void br_node_set_scale(br_engine *e, br_node n, float x, float y)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        t->scale = br_vec2_make(x, y);
        br_node_transform_changed(e, n);
    }
}

br_vec2 br_node_scale(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->scale : br_vec2_make(1.0f, 1.0f);
}

void br_node_set_rotation(br_engine *e, br_node n, float radians)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        br_scene_set_rotation(t, radians);
        br_node_transform_changed(e, n);
    }
}

float br_node_rotation(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->rotation : 0.0f;
}

void br_node_set_skew(br_engine *e, br_node n, float x, float y)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        t->skew = br_vec2_make(x, y);
        br_node_transform_changed(e, n);
    }
}

br_vec2 br_node_skew(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->skew : br_vec2_make(0.0f, 0.0f);
}

void br_node_set_pivot(br_engine *e, br_node n, float x, float y)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        t->pivot = br_vec2_make(x, y);
        br_node_transform_changed(e, n);
    }
}

br_vec2 br_node_pivot(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->pivot : br_vec2_make(0.0f, 0.0f);
}

void br_node_set_origin(br_engine *e, br_node n, float x, float y)
{
    br_node_transform *t = br_node_transform_for(e, n);
    if (t != NULL) {
        t->origin = br_vec2_make(x, y);
        br_node_transform_changed(e, n);
    }
}

br_vec2 br_node_origin(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? t->origin : br_vec2_make(0.0f, 0.0f);
}

br_mat3 br_node_local_transform(br_engine *e, br_node n)
{
    br_node_transform *t = br_node_transform_for(e, n);
    return t != NULL ? br_scene_compute_local(t) : br_mat3_identity();
}

br_mat3 br_node_world_transform(br_engine *e, br_node n)
{
    if (br_scene_record(e, n) == NULL) {
        return br_mat3_identity();
    }
    return br_scene_chain_transform(e->scene, n.index, false);
}

br_vec2 br_node_to_global(br_engine *e, br_node n, br_vec2 local_point)
{
    return br_mat3_apply(br_node_world_transform(e, n), local_point);
}

br_vec2 br_node_to_local(br_engine *e, br_node n, br_vec2 world_point)
{
    return br_mat3_apply_inverse(br_node_world_transform(e, n), world_point);
}

/* ---- appearance */

void br_node_set_alpha(br_engine *e, br_node n, float alpha)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->alpha = alpha;
        br_scene_mark_dirty(e->scene, n.index, false, true);
    }
}

float br_node_alpha(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->alpha : 0.0f;
}

void br_node_set_tint(br_engine *e, br_node n, br_color tint)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->tint = br_color_make(tint.r, tint.g, tint.b, 1.0f);
        br_scene_mark_dirty(e->scene, n.index, false, true);
    }
}

br_color br_node_tint(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->tint : br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
}

float br_node_world_alpha(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->world_alpha : 0.0f;
}

br_color br_node_world_tint(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->world_tint : br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
}

void br_node_set_visible(br_engine *e, br_node n, bool visible)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->visible = visible;
    }
}

bool br_node_visible(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->visible;
}

void br_node_set_renderable(br_engine *e, br_node n, bool renderable)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->renderable = renderable;
    }
}

bool br_node_renderable(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->renderable;
}

void br_node_set_z_index(br_engine *e, br_node n, int z_index)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || rec->z_index == z_index) {
        return;
    }
    rec->z_index = z_index;
    if (rec->parent != BR_NO_NODE) {
        br_scene_queue_sort(e->scene, rec->parent);
    }
    br_node_record *layer = br_scene_record(e, rec->layer);
    if (layer != NULL) {
        br_scene_queue_sort(e->scene, rec->layer.index);
    }
}

int br_node_z_index(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->z_index : 0;
}

void br_node_set_sortable_children(br_engine *e, br_node n, bool sortable)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->sortable_children = sortable;
        br_scene_queue_sort(e->scene, n.index);
    }
}

bool br_node_sortable_children(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->sortable_children;
}

void br_node_set_blend_mode(br_engine *e, br_node n, int blend_mode)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->blend_mode = blend_mode;
    }
}

int br_node_blend_mode(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->blend_mode : BR_BLEND_INHERIT;
}

/* ---- bounds and culling */

br_rect br_node_get_bounds(br_engine *e, br_node n)
{
    br_rect r = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    bool has = false;
    if (br_scene_record(e, n) != NULL) {
        br_scene_bounds(e->scene, e, n.index, br_node_world_transform(e, n), &r, &has);
    }
    return r;
}

br_rect br_node_get_local_bounds(br_engine *e, br_node n)
{
    br_rect r = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    bool has = false;
    if (br_scene_record(e, n) != NULL) {
        br_scene_bounds(e->scene, e, n.index, br_mat3_identity(), &r, &has);
    }
    return r;
}

void br_node_set_cullable(br_engine *e, br_node n, bool cullable)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || rec->cullable == cullable) {
        return;
    }
    rec->cullable = cullable;
    if (cullable) {
        br_scene_cullable_add(e->scene, n.index);
    } else {
        br_scene_cullable_remove(e->scene, n.index);
    }
}

bool br_node_cullable(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->cullable;
}

void br_node_set_cullable_children(br_engine *e, br_node n, bool cullable_children)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->cullable_children = cullable_children;
    }
}

void br_node_set_cull_area(br_engine *e, br_node n, br_rect local_area)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->cull_area = local_area;
        rec->has_cull_area = true;
    }
}

void br_node_clear_cull_area(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->has_cull_area = false;
    }
}

bool br_node_is_culled(br_engine *e, br_node n)
{
    return br_scene_record(e, n) != NULL && br_scene_is_culled(e->scene, n.index);
}

/* ---- render groups and layers */

void br_node_set_render_group(br_engine *e, br_node n, bool is_render_group)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || rec->render_group == is_render_group) {
        return;
    }
    rec->render_group = is_render_group;
    /* children's cached world switches between "relative to this node" and "relative to the
     * group above": recompute them */
    for (uint32_t i = 0; i < rec->children.count; i++) {
        br_scene_mark_dirty(e->scene, *(uint32_t *)br_array_at(&rec->children, i), true, false);
    }
}

bool br_node_is_render_group(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->render_group;
}

bool br_node_layer_attach(br_engine *e, br_node layer, br_node n)
{
    br_node_record *lrec = br_scene_record(e, layer);
    br_node_record *rec = br_scene_record(e, n);
    if (lrec == NULL || rec == NULL || br_node_equal(layer, n)) {
        return false;
    }
    br_node_layer_detach(e, n);
    br_array_push(&lrec->layer_members, &n);
    rec = br_scene_record(e, n);
    rec->layer = layer;
    br_scene_queue_sort(e->scene, layer.index);
    return true;
}

void br_node_layer_detach(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return;
    }
    br_node_record *lrec = br_scene_record(e, rec->layer);
    if (lrec != NULL) {
        br_remove_handle(&lrec->layer_members, n);
    }
    rec->layer = br_node_zero();
}

br_node br_node_layer(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL || br_scene_record(e, rec->layer) == NULL) {
        return br_node_zero();
    }
    return rec->layer;
}

void br_node_set_layer_bits(br_engine *e, br_node n, uint32_t bits)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->layer_bits = bits;
    }
}

uint32_t br_node_layer_bits(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->layer_bits : 0u;
}

/* ---- masks and effects */

void br_node_set_mask(br_engine *e, br_node n, br_node mask, br_mask_kind kind, bool inverse)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return;
    }
    br_node_record *old = br_scene_record(e, rec->mask);
    if (old != NULL && old->mask_users > 0) {
        old->mask_users -= 1;
    }
    br_node_record *mrec = br_scene_record(e, mask);
    if (mrec == NULL || kind == BR_MASK_NONE || br_node_equal(mask, n)) {
        rec->mask = br_node_zero();
        rec->mask_kind = BR_MASK_NONE;
        rec->mask_inverse = false;
        return;
    }
    mrec->mask_users += 1;
    rec->mask = mask;
    rec->mask_kind = kind;
    rec->mask_inverse = inverse;
}

br_node br_node_mask(br_engine *e, br_node n, br_mask_kind *kind, bool *inverse)
{
    br_node_record *rec = br_scene_record(e, n);
    bool has = rec != NULL && br_scene_record(e, rec->mask) != NULL;
    if (kind != NULL) {
        *kind = has ? rec->mask_kind : BR_MASK_NONE;
    }
    if (inverse != NULL) {
        *inverse = has && rec->mask_inverse;
    }
    return has ? rec->mask : br_node_zero();
}

void br_node_set_filters(br_engine *e, br_node n, void *filters)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->filters = filters;
    }
}

void *br_node_filters(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->filters : NULL;
}

void br_node_set_cache_as_texture(br_engine *e, br_node n, bool cache)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->cache_as_texture = cache;
    }
}

bool br_node_cache_as_texture(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->cache_as_texture;
}

bool br_node_needs_effects(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return false;
    }
    bool effect_mask = (rec->mask_kind == BR_MASK_ALPHA || rec->mask_kind == BR_MASK_COLOR) &&
                       br_scene_record(e, rec->mask) != NULL;
    return rec->filters != NULL || rec->cache_as_texture || effect_mask ||
           rec->blend_mode >= BR_BLEND_ADVANCED_FIRST;
}

/* ---- interaction */

void br_node_set_event_mode(br_engine *e, br_node n, br_event_mode mode)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->event_mode = mode;
    }
}

br_event_mode br_node_event_mode(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->event_mode : BR_EVENT_MODE_NONE;
}

void br_node_set_interactive_children(br_engine *e, br_node n, bool interactive_children)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->interactive_children = interactive_children;
    }
}

bool br_node_interactive_children(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL && rec->interactive_children;
}

void br_node_set_hit_area(br_engine *e, br_node n, const br_hit_area *area)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return;
    }
    br_free((void *)rec->hit_area.points);
    SDL_zero(rec->hit_area);
    if (area == NULL || area->type == BR_HIT_AREA_NONE) {
        return;
    }
    rec->hit_area = *area;
    rec->hit_area.points = NULL;
    rec->hit_area.point_count = 0;
    if (area->type == BR_HIT_AREA_POLYGON && area->point_count > 0 && area->points != NULL) {
        br_vec2 *points = br_alloc(sizeof(br_vec2) * area->point_count);
        if (points != NULL) {
            SDL_memcpy(points, area->points, sizeof(br_vec2) * area->point_count);
            rec->hit_area.points = points;
            rec->hit_area.point_count = area->point_count;
        }
    }
}

br_hit_area br_node_hit_area(br_engine *e, br_node n)
{
    br_hit_area none;
    SDL_zero(none);
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->hit_area : none;
}

bool br_hit_area_contains(const br_hit_area *area, br_vec2 p)
{
    switch (area->type) {
    case BR_HIT_AREA_RECT:
        return br_rect_contains(area->rect, p.x, p.y);
    case BR_HIT_AREA_CIRCLE:
        return br_circle_contains(area->circle, p.x, p.y);
    case BR_HIT_AREA_ELLIPSE:
        return br_ellipse_contains(area->ellipse, p.x, p.y);
    case BR_HIT_AREA_ROUNDED_RECT:
        return br_rounded_rect_contains(area->rounded_rect, p.x, p.y);
    case BR_HIT_AREA_TRIANGLE:
        return br_triangle_contains(area->triangle, p.x, p.y);
    case BR_HIT_AREA_POLYGON: {
        br_polygon poly = { area->points, area->point_count };
        return br_polygon_contains(poly, p.x, p.y);
    }
    case BR_HIT_AREA_NONE:
    default:
        return false;
    }
}

void br_node_set_cursor(br_engine *e, br_node n, int cursor)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec != NULL) {
        rec->cursor = cursor;
    }
}

int br_node_cursor(br_engine *e, br_node n)
{
    br_node_record *rec = br_scene_record(e, n);
    return rec != NULL ? rec->cursor : -1;
}

bool br_node_type_hit_test(br_engine *e, br_node n, br_vec2 local_point)
{
    br_node_record *rec = br_scene_record(e, n);
    if (rec == NULL) {
        return false;
    }
    br_node_type_entry *type = br_scene_type_entry(e->scene, rec->type_id);
    if (type->desc.hit_test == NULL) {
        return false;
    }
    return type->desc.hit_test(e, n, br_scene_type_data(e->scene, n.index), local_point);
}

/* ---- module lifetime */

void br_scene_shutdown_nodes(br_engine *e, void *user)
{
    (void)user;
    br_scene_state *s = e->scene;
    for (uint32_t i = 0; i < br_pool_capacity(&s->nodes); i++) {
        if (br_pool_slot_alive(&s->nodes, i)) {
            br_node_record *rec = br_scene_record_at(s, i);
            if (!rec->destroyed) {
                rec->destroyed = true;
                br_array_push(&s->pending_destroy, &i);
            }
        }
    }
    br_scene_flush_destroyed(e);
}

static void br_scene_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_scene_state *s = e->scene;
    br_scene_shutdown_nodes(e, NULL); /* no-op if br_builtin's hook already ran */
    for (uint32_t i = 0; i < s->types.count; i++) {
        br_node_type_entry *t = br_array_at(&s->types, i);
        br_free(t->name);
        br_array_free(&t->data);
        br_array_free(&t->owners);
    }
    br_component_free_all(s);
    br_array_free(&s->types);
    br_pool_free(&s->nodes);
    br_array_free(&s->transforms);
    br_array_free(&s->pending_destroy);
    br_array_free(&s->destroy_listeners);
    br_array_free(&s->sort_queue);
    br_array_free(&s->sort_scratch);
    br_array_free(&s->cullables);
    br_free(s);
    e->scene = NULL;
}

void br_scene_register(br_engine *e)
{
    br_scene_state *s = br_alloc(sizeof(br_scene_state));
    if (s == NULL) {
        return;
    }
    e->scene = s;
    br_pool_init(&s->nodes, sizeof(br_node_record));
    br_array_init(&s->transforms, sizeof(br_node_transform));
    br_array_init(&s->types, sizeof(br_node_type_entry));
    br_array_init(&s->components, sizeof(br_component_entry));
    br_array_init(&s->pending_destroy, sizeof(uint32_t));
    br_array_init(&s->destroy_listeners, sizeof(br_destroy_listener));
    br_array_init(&s->sort_queue, sizeof(uint32_t));
    br_array_init(&s->cullables, sizeof(uint32_t));
    br_engine_on_shutdown(e, br_scene_shutdown, NULL);

    br_node_type_desc container;
    SDL_zero(container);
    container.name = "container";
    br_node_type_register(e, &container); /* id 1 = BR_NODE_TYPE_CONTAINER */
    s->root = br_node_create(e, BR_NODE_TYPE_CONTAINER);
    br_node_set_label(e, s->root, "stage");
    br_scene_add_systems(e);
}
