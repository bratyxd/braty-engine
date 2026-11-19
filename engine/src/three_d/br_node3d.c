/* br_node3d: the 3D node table (struct of arrays), the 3D node-type registry with dense data
 * pools, hierarchy (doubly linked sibling lists), transforms, deferred destroy. */
#include "three_d/br_three_d_internal.h"

/* ---- table storage */

static bool br_grow_array(void **ptr, size_t elem_size, uint32_t capacity)
{
    void *p = br_realloc(*ptr, elem_size * capacity);
    if (p == NULL) {
        return false;
    }
    *ptr = p;
    return true;
}

static bool br_node3d_table_grow(br_node3d_table *t)
{
    uint32_t cap = t->capacity > 0 ? t->capacity * 2 : 256;
    bool ok = br_grow_array((void **)&t->generation, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->alive, sizeof(uint8_t), cap) &&
              br_grow_array((void **)&t->type, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->data_index, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->parent, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->first_child, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->last_child, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->next_sibling, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->prev_sibling, sizeof(uint32_t), cap) &&
              br_grow_array((void **)&t->label, sizeof(char *), cap) &&
              br_grow_array((void **)&t->position, sizeof(br_vec3), cap) &&
              br_grow_array((void **)&t->rotation, sizeof(br_quat), cap) &&
              br_grow_array((void **)&t->scale, sizeof(br_vec3), cap) &&
              br_grow_array((void **)&t->world, sizeof(br_mat4), cap) &&
              br_grow_array((void **)&t->visible, sizeof(uint8_t), cap) &&
              br_grow_array((void **)&t->pickable, sizeof(uint8_t), cap) &&
              br_grow_array((void **)&t->layer, sizeof(uint32_t), cap);
    if (!ok) {
        br_log_error("out of memory growing the 3D node table");
        return false;
    }
    for (uint32_t i = t->capacity; i < cap; i++) {
        t->generation[i] = 0;
        t->alive[i] = 0;
        t->label[i] = NULL;
        uint32_t index = cap - 1 - (i - t->capacity); /* push high first so low indices come out first */
        br_array_push(&t->free_slots, &index);
    }
    t->capacity = cap;
    return true;
}

void br_node3d_table_init(br_engine *e)
{
    br_three_d_state *s = e->three_d;
    br_array_init(&s->nodes.free_slots, sizeof(uint32_t));
    br_array_init(&s->nodes.pending, sizeof(uint32_t));
    br_array_init(&s->types, sizeof(br_node3d_type_entry));
    br_array_init(&s->destroy_listeners, sizeof(br_node3d_destroy_entry));
    br_node3d_type_desc group;
    SDL_zero(group);
    group.name = "group3d";
    br_node3d_type_register(e, &group); /* id 1 = BR_NODE3D_TYPE_GROUP */
}

/* ---- handles */

bool br_node3d_index_valid(br_three_d_state *s, br_node3d n)
{
    return s != NULL && n.generation != 0 && n.index < s->nodes.capacity && s->nodes.alive[n.index] &&
           s->nodes.generation[n.index] == n.generation;
}

br_node3d br_node3d_handle(br_three_d_state *s, uint32_t index)
{
    br_node3d n = { 0, 0 };
    if (index != BR_NONE3D && index < s->nodes.capacity && s->nodes.alive[index]) {
        n.index = index;
        n.generation = s->nodes.generation[index];
    }
    return n;
}

static br_node3d_type_entry *br_type_entry(br_three_d_state *s, uint32_t type_id)
{
    if (type_id == 0 || type_id > s->types.count) {
        return NULL;
    }
    return br_array_at(&s->types, type_id - 1);
}

/* ---- types */

uint32_t br_node3d_type_register(br_engine *e, const br_node3d_type_desc *desc)
{
    br_three_d_state *s = e->three_d;
    if (s == NULL || desc == NULL || desc->name == NULL) {
        br_log_error("br_node3d_type_register: missing module or name");
        return 0;
    }
    if (br_node3d_type_find(e, desc->name) != 0) {
        br_log_error("3D node type '%s' already registered", desc->name);
        return 0;
    }
    br_node3d_type_entry entry;
    SDL_zero(entry);
    entry.desc = *desc;
    entry.name = br_strdup(desc->name);
    entry.desc.name = entry.name;
    br_array_init(&entry.data, desc->data_size > 0 ? (uint32_t)desc->data_size : 1);
    br_array_init(&entry.owners, sizeof(uint32_t));
    if (br_array_push(&s->types, &entry) == NULL) {
        br_free(entry.name);
        return 0;
    }
    return s->types.count;
}

uint32_t br_node3d_type_find(br_engine *e, const char *name)
{
    br_three_d_state *s = e->three_d;
    for (uint32_t i = 0; s != NULL && name != NULL && i < s->types.count; i++) {
        br_node3d_type_entry *t = br_array_at(&s->types, i);
        if (br_str_equal(t->name, name)) {
            return i + 1;
        }
    }
    return 0;
}

const br_node3d_type_desc *br_node3d_type_get(br_engine *e, uint32_t type_id)
{
    br_node3d_type_entry *t = br_type_entry(e->three_d, type_id);
    return t != NULL ? &t->desc : NULL;
}

/* ---- lifetime */

br_node3d br_node3d_create(br_engine *e, uint32_t type_id)
{
    br_node3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    br_node3d_type_entry *type = s != NULL ? br_type_entry(s, type_id) : NULL;
    if (type == NULL) {
        br_log_error("br_node3d_create: unknown 3D node type %u", type_id);
        return none;
    }
    br_node3d_table *t = &s->nodes;
    if (t->free_slots.count == 0 && !br_node3d_table_grow(t)) {
        return none;
    }
    uint32_t index = *(uint32_t *)br_array_at(&t->free_slots, t->free_slots.count - 1);
    br_array_pop(&t->free_slots);
    t->generation[index] += 1;
    if (t->generation[index] == 0) {
        t->generation[index] = 1;
    }
    t->alive[index] = 1;
    t->type[index] = type_id;
    t->data_index[index] = BR_NONE3D;
    t->parent[index] = BR_NONE3D;
    t->first_child[index] = BR_NONE3D;
    t->last_child[index] = BR_NONE3D;
    t->next_sibling[index] = BR_NONE3D;
    t->prev_sibling[index] = BR_NONE3D;
    t->label[index] = NULL;
    t->position[index] = br_vec3_make(0.0f, 0.0f, 0.0f);
    t->rotation[index] = br_quat_identity();
    t->scale[index] = br_vec3_make(1.0f, 1.0f, 1.0f);
    t->world[index] = br_mat4_identity();
    t->visible[index] = 1;
    t->pickable[index] = 1;
    t->layer[index] = 1;
    t->live++;
    br_node3d n = { index, t->generation[index] };
    if (type->desc.data_size > 0) {
        t->data_index[index] = type->data.count;
        if (br_array_push(&type->data, NULL) == NULL || br_array_push(&type->owners, &index) == NULL) {
            br_log_error("out of memory creating a 3D node");
            t->alive[index] = 0;
            t->live--;
            br_array_push(&t->free_slots, &index);
            return none;
        }
        void *data = br_array_at(&type->data, t->data_index[index]);
        SDL_memset(data, 0, type->desc.data_size);
        if (type->desc.on_create != NULL) {
            type->desc.on_create(e, n, br_array_at(&type->data, t->data_index[index]));
        }
    } else if (type->desc.on_create != NULL) {
        type->desc.on_create(e, n, NULL);
    }
    return n;
}

br_node3d br_node3d_create_group(br_engine *e)
{
    return br_node3d_create(e, BR_NODE3D_TYPE_GROUP);
}

void *br_node3d_data(br_engine *e, br_node3d n, uint32_t type_id)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n) || s->nodes.type[n.index] != type_id) {
        return NULL;
    }
    br_node3d_type_entry *type = br_type_entry(s, type_id);
    if (type == NULL || type->desc.data_size == 0) {
        return NULL;
    }
    return br_array_at(&type->data, s->nodes.data_index[n.index]);
}

uint32_t br_node3d_type_of(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? s->nodes.type[n.index] : 0;
}

bool br_node3d_valid(br_engine *e, br_node3d n)
{
    return br_node3d_index_valid(e->three_d, n);
}

uint32_t br_node3d_count(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->nodes.live : 0;
}

static void br_node3d_unlink(br_node3d_table *t, uint32_t i)
{
    uint32_t p = t->parent[i];
    if (p == BR_NONE3D) {
        return;
    }
    uint32_t prev = t->prev_sibling[i];
    uint32_t next = t->next_sibling[i];
    if (prev != BR_NONE3D) {
        t->next_sibling[prev] = next;
    } else {
        t->first_child[p] = next;
    }
    if (next != BR_NONE3D) {
        t->prev_sibling[next] = prev;
    } else {
        t->last_child[p] = prev;
    }
    t->parent[i] = BR_NONE3D;
    t->prev_sibling[i] = BR_NONE3D;
    t->next_sibling[i] = BR_NONE3D;
}

/* Frees one slot: type data (swap-remove), label, listeners. The node is already detached. */
static void br_node3d_free_slot(br_engine *e, uint32_t index)
{
    br_three_d_state *s = e->three_d;
    br_node3d_table *t = &s->nodes;
    br_node3d n = { index, t->generation[index] };
    br_node3d_type_entry *type = br_type_entry(s, t->type[index]);
    if (type != NULL && type->desc.data_size > 0 && t->data_index[index] != BR_NONE3D) {
        uint32_t di = t->data_index[index];
        if (type->desc.on_destroy != NULL) {
            type->desc.on_destroy(e, n, br_array_at(&type->data, di));
        }
        uint32_t last = type->data.count - 1;
        if (di != last) {
            uint32_t moved = *(uint32_t *)br_array_at(&type->owners, last);
            t->data_index[moved] = di;
        }
        br_array_remove_swap(&type->data, di);
        br_array_remove_swap(&type->owners, di);
    } else if (type != NULL && type->desc.on_destroy != NULL) {
        type->desc.on_destroy(e, n, NULL);
    }
    br_free(t->label[index]);
    t->label[index] = NULL;
    t->alive[index] = 0;
    t->type[index] = 0;
    for (uint32_t i = 0; i < s->destroy_listeners.count; i++) {
        br_node3d_destroy_entry *l = br_array_at(&s->destroy_listeners, i);
        l->fn(e, n, l->user);
    }
    br_pick3d_node_destroyed(e, n);
    br_array_push(&t->free_slots, &index);
}

/* Marks index and its subtree dead and appends them to `out` (parents before children). */
static void br_node3d_collect_subtree(br_node3d_table *t, uint32_t index, br_array *out)
{
    uint32_t first = out->count;
    br_array_push(out, &index);
    for (uint32_t k = first; k < out->count; k++) {
        uint32_t i = *(uint32_t *)br_array_at(out, k);
        if (t->alive[i]) {
            t->alive[i] = 0;
            t->live--;
        }
        for (uint32_t c = t->first_child[i]; c != BR_NONE3D; c = t->next_sibling[c]) {
            br_array_push(out, &c);
        }
    }
}

void br_node3d_destroy(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return;
    }
    br_node3d_table *t = &s->nodes;
    br_node3d_unlink(t, n.index);
    uint32_t first = t->pending.count;
    br_node3d_collect_subtree(t, n.index, &t->pending);
    /* handles are invalid now; memory goes at the next flush (or now, outside a frame) */
    for (uint32_t k = first; k < t->pending.count; k++) {
        uint32_t i = *(uint32_t *)br_array_at(&t->pending, k);
        t->alive[i] = 0;
    }
    if (!e->stepping && s->dispatch_depth == 0) {
        br_node3d_flush(e);
    }
}

void br_node3d_flush(br_engine *e)
{
    br_three_d_state *s = e->three_d;
    br_node3d_table *t = &s->nodes;
    /* on_destroy callbacks may destroy more nodes: those append to pending and are freed too */
    for (uint32_t k = 0; k < t->pending.count; k++) {
        uint32_t i = *(uint32_t *)br_array_at(&t->pending, k);
        br_node3d_free_slot(e, i);
    }
    br_array_clear(&t->pending);
}

void br_node3d_table_free(br_engine *e)
{
    br_three_d_state *s = e->three_d;
    br_node3d_table *t = &s->nodes;
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (t->alive[i]) {
            br_node3d_unlink(t, i);
            t->alive[i] = 0;
            t->live--;
            br_array_push(&t->pending, &i);
        }
    }
    br_node3d_flush(e);
    for (uint32_t i = 0; i < s->types.count; i++) {
        br_node3d_type_entry *type = br_array_at(&s->types, i);
        br_array_free(&type->data);
        br_array_free(&type->owners);
        br_free(type->name);
    }
    br_array_free(&s->types);
    br_array_free(&s->destroy_listeners);
    br_free(t->generation);
    br_free(t->alive);
    br_free(t->type);
    br_free(t->data_index);
    br_free(t->parent);
    br_free(t->first_child);
    br_free(t->last_child);
    br_free(t->next_sibling);
    br_free(t->prev_sibling);
    br_free(t->label);
    br_free(t->position);
    br_free(t->rotation);
    br_free(t->scale);
    br_free(t->world);
    br_free(t->visible);
    br_free(t->pickable);
    br_free(t->layer);
    br_array_free(&t->free_slots);
    br_array_free(&t->pending);
    SDL_zero(*t);
}

void br_node3d_on_destroy(br_engine *e, br_node3d_destroy_fn fn, void *user)
{
    if (e->three_d != NULL && fn != NULL) {
        br_node3d_destroy_entry entry = { fn, user };
        br_array_push(&e->three_d->destroy_listeners, &entry);
    }
}

/* ---- hierarchy */

bool br_node3d_add_child(br_engine *e, br_node3d parent, br_node3d child)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, parent) || !br_node3d_index_valid(s, child)) {
        return false;
    }
    br_node3d_table *t = &s->nodes;
    for (uint32_t p = parent.index; p != BR_NONE3D; p = t->parent[p]) {
        if (p == child.index) {
            br_log_error("br_node3d_add_child: would create a cycle");
            return false;
        }
    }
    br_node3d_unlink(t, child.index);
    uint32_t c = child.index;
    uint32_t p = parent.index;
    t->parent[c] = p;
    t->prev_sibling[c] = t->last_child[p];
    t->next_sibling[c] = BR_NONE3D;
    if (t->last_child[p] != BR_NONE3D) {
        t->next_sibling[t->last_child[p]] = c;
    } else {
        t->first_child[p] = c;
    }
    t->last_child[p] = c;
    return true;
}

void br_node3d_remove_from_parent(br_engine *e, br_node3d n)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        br_node3d_unlink(&e->three_d->nodes, n.index);
    }
}

br_node3d br_node3d_parent(br_engine *e, br_node3d n)
{
    br_node3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? br_node3d_handle(s, s->nodes.parent[n.index]) : none;
}

br_node3d br_node3d_first_child(br_engine *e, br_node3d n)
{
    br_node3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? br_node3d_handle(s, s->nodes.first_child[n.index]) : none;
}

br_node3d br_node3d_next_sibling(br_engine *e, br_node3d n)
{
    br_node3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? br_node3d_handle(s, s->nodes.next_sibling[n.index]) : none;
}

uint32_t br_node3d_child_count(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    uint32_t count = 0;
    if (br_node3d_index_valid(s, n)) {
        for (uint32_t c = s->nodes.first_child[n.index]; c != BR_NONE3D; c = s->nodes.next_sibling[c]) {
            count++;
        }
    }
    return count;
}

void br_node3d_set_label(br_engine *e, br_node3d n, const char *label)
{
    br_three_d_state *s = e->three_d;
    if (br_node3d_index_valid(s, n)) {
        br_free(s->nodes.label[n.index]);
        s->nodes.label[n.index] = label != NULL ? br_strdup(label) : NULL;
    }
}

const char *br_node3d_label(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    if (br_node3d_index_valid(s, n) && s->nodes.label[n.index] != NULL) {
        return s->nodes.label[n.index];
    }
    return "";
}

static uint32_t br_node3d_find_index(br_node3d_table *t, uint32_t i, const char *label)
{
    if (t->label[i] != NULL && br_str_equal(t->label[i], label)) {
        return i;
    }
    for (uint32_t c = t->first_child[i]; c != BR_NONE3D; c = t->next_sibling[c]) {
        uint32_t found = br_node3d_find_index(t, c, label);
        if (found != BR_NONE3D) {
            return found;
        }
    }
    return BR_NONE3D;
}

br_node3d br_node3d_find_by_label(br_engine *e, br_node3d root, const char *label)
{
    br_node3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, root) || label == NULL) {
        return none;
    }
    return br_node3d_handle(s, br_node3d_find_index(&s->nodes, root.index, label));
}

/* ---- transform and flags */

void br_node3d_set_position(br_engine *e, br_node3d n, br_vec3 position)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.position[n.index] = position;
    }
}

br_vec3 br_node3d_position(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? s->nodes.position[n.index] : br_vec3_make(0.0f, 0.0f, 0.0f);
}

void br_node3d_set_rotation(br_engine *e, br_node3d n, br_quat rotation)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.rotation[n.index] = br_quat_normalize(rotation);
    }
}

br_quat br_node3d_rotation(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? s->nodes.rotation[n.index] : br_quat_identity();
}

void br_node3d_set_euler(br_engine *e, br_node3d n, float x, float y, float z)
{
    br_node3d_set_rotation(e, n, br_quat_from_euler(x, y, z));
}

void br_node3d_set_scale(br_engine *e, br_node3d n, br_vec3 scale)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.scale[n.index] = scale;
    }
}

br_vec3 br_node3d_scale(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? s->nodes.scale[n.index] : br_vec3_make(1.0f, 1.0f, 1.0f);
}

/* Quaternion of an orthonormal basis (columns x, y, z). */
static br_quat br_quat_from_basis(br_vec3 x, br_vec3 y, br_vec3 z)
{
    br_quat q;
    float trace = x.x + y.y + z.z;
    if (trace > 0.0f) {
        float k = 0.5f / SDL_sqrtf(trace + 1.0f);
        q.w = 0.25f / k;
        q.x = (y.z - z.y) * k;
        q.y = (z.x - x.z) * k;
        q.z = (x.y - y.x) * k;
    } else if (x.x > y.y && x.x > z.z) {
        float k = 2.0f * SDL_sqrtf(1.0f + x.x - y.y - z.z);
        q.w = (y.z - z.y) / k;
        q.x = 0.25f * k;
        q.y = (y.x + x.y) / k;
        q.z = (z.x + x.z) / k;
    } else if (y.y > z.z) {
        float k = 2.0f * SDL_sqrtf(1.0f + y.y - x.x - z.z);
        q.w = (z.x - x.z) / k;
        q.x = (y.x + x.y) / k;
        q.y = 0.25f * k;
        q.z = (z.y + y.z) / k;
    } else {
        float k = 2.0f * SDL_sqrtf(1.0f + z.z - x.x - y.y);
        q.w = (x.y - y.x) / k;
        q.x = (z.x + x.z) / k;
        q.y = (z.y + y.z) / k;
        q.z = 0.25f * k;
    }
    return br_quat_normalize(q);
}

void br_node3d_look_at(br_engine *e, br_node3d n, br_vec3 target, br_vec3 up)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return;
    }
    br_mat4 world = br_node3d_world_matrix(e, n);
    br_vec3 eye = br_vec3_make(world.m[12], world.m[13], world.m[14]);
    br_vec3 back = br_vec3_normalize(br_vec3_sub(eye, target)); /* local +Z points away from the target */
    if (br_vec3_length(back) == 0.0f) {
        return;
    }
    br_vec3 right = br_vec3_cross(up, back);
    if (br_vec3_length(right) < 1e-6f) {
        right = br_vec3_cross(br_vec3_make(0.0f, 0.0f, 1.0f), back); /* looking straight along up */
        if (br_vec3_length(right) < 1e-6f) {
            right = br_vec3_make(1.0f, 0.0f, 0.0f);
        }
    }
    right = br_vec3_normalize(right);
    br_vec3 real_up = br_vec3_cross(back, right);
    br_quat world_rotation = br_quat_from_basis(right, real_up, back);
    /* convert to the parent's space */
    uint32_t p = s->nodes.parent[n.index];
    if (p != BR_NONE3D) {
        br_quat parent_rotation = br_quat_identity();
        for (uint32_t a = p; a != BR_NONE3D; a = s->nodes.parent[a]) {
            parent_rotation = br_quat_mul(s->nodes.rotation[a], parent_rotation);
        }
        world_rotation = br_quat_mul(br_quat_conjugate(parent_rotation), world_rotation);
    }
    s->nodes.rotation[n.index] = br_quat_normalize(world_rotation);
}

br_mat4 br_node3d_local_matrix(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return br_mat4_identity();
    }
    return br_mat4_from_trs(s->nodes.position[n.index], s->nodes.rotation[n.index], s->nodes.scale[n.index]);
}

br_mat4 br_node3d_world_matrix(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return br_mat4_identity();
    }
    br_node3d_table *t = &s->nodes;
    br_mat4 m = br_mat4_identity();
    for (uint32_t i = n.index; i != BR_NONE3D; i = t->parent[i]) {
        m = br_mat4_mul(br_mat4_from_trs(t->position[i], t->rotation[i], t->scale[i]), m);
    }
    return m;
}

br_vec3 br_node3d_world_position(br_engine *e, br_node3d n)
{
    br_mat4 m = br_node3d_world_matrix(e, n);
    return br_vec3_make(m.m[12], m.m[13], m.m[14]);
}

void br_node3d_set_visible(br_engine *e, br_node3d n, bool visible)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.visible[n.index] = visible ? 1 : 0;
    }
}

bool br_node3d_visible(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) && s->nodes.visible[n.index];
}

void br_node3d_set_layer(br_engine *e, br_node3d n, uint32_t layer)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.layer[n.index] = layer;
    }
}

uint32_t br_node3d_layer(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) ? s->nodes.layer[n.index] : 0;
}

void br_node3d_set_pickable(br_engine *e, br_node3d n, bool pickable)
{
    if (br_node3d_index_valid(e->three_d, n)) {
        e->three_d->nodes.pickable[n.index] = pickable ? 1 : 0;
    }
}

bool br_node3d_pickable(br_engine *e, br_node3d n)
{
    br_three_d_state *s = e->three_d;
    return br_node3d_index_valid(s, n) && s->nodes.pickable[n.index];
}

bool br_node3d_local_bounds(br_engine *e, br_node3d n, br_aabb *out)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, n)) {
        return false;
    }
    br_node3d_type_entry *type = br_type_entry(s, s->nodes.type[n.index]);
    if (type == NULL || type->desc.local_bounds == NULL) {
        return false;
    }
    return type->desc.local_bounds(e, n, br_node3d_data(e, n, s->nodes.type[n.index]), out);
}

/* ---- world matrices */

void br_three_d_update_world(br_engine *e, br_node3d root)
{
    br_three_d_state *s = e->three_d;
    if (!br_node3d_index_valid(s, root)) {
        return;
    }
    br_node3d_table *t = &s->nodes;
    uint32_t r = root.index;
    br_mat4 parent_world = br_mat4_identity();
    if (t->parent[r] != BR_NONE3D) {
        parent_world = br_node3d_world_matrix(e, br_node3d_handle(s, t->parent[r]));
    }
    t->world[r] = br_mat4_mul(parent_world, br_mat4_from_trs(t->position[r], t->rotation[r], t->scale[r]));
    /* pre-order walk without recursion: parents are always computed before their children */
    uint32_t i = t->first_child[r];
    while (i != BR_NONE3D) {
        t->world[i] = br_mat4_mul(t->world[t->parent[i]], br_mat4_from_trs(t->position[i], t->rotation[i], t->scale[i]));
        if (t->first_child[i] != BR_NONE3D) {
            i = t->first_child[i];
            continue;
        }
        while (i != r && t->next_sibling[i] == BR_NONE3D) {
            i = t->parent[i];
        }
        i = (i == r) ? BR_NONE3D : t->next_sibling[i];
    }
}
