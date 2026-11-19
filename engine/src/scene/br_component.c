/* br_component: user components as dense arrays (sparse set: node index -> dense row). */
#include "scene/br_scene_internal.h"

static br_component_entry *br_component_entry_get(br_engine *e, uint32_t component_id)
{
    if (component_id == 0 || e->scene == NULL) {
        return NULL;
    }
    return br_array_at(&e->scene->components, component_id - 1);
}

uint32_t br_component_register(br_engine *e, const br_component_desc *desc)
{
    br_scene_state *s = e->scene;
    if (desc == NULL || desc->name == NULL || desc->size == 0) {
        br_log_error("br_component_register: a component needs a name and a size");
        return 0;
    }
    if (br_component_find(e, desc->name) != 0) {
        br_log_error("component '%s' is already registered", desc->name);
        return 0;
    }
    br_component_entry entry;
    SDL_zero(entry);
    entry.desc = *desc;
    entry.name = br_strdup(desc->name);
    entry.desc.name = entry.name;
    if (desc->field_count > 0 && desc->fields != NULL) {
        entry.fields = br_alloc(sizeof(br_field_desc) * desc->field_count);
        if (entry.fields == NULL) {
            br_free(entry.name);
            return 0;
        }
        for (uint32_t i = 0; i < desc->field_count; i++) {
            entry.fields[i] = desc->fields[i];
            entry.fields[i].name = br_strdup(desc->fields[i].name);
        }
    } else {
        entry.desc.field_count = 0;
    }
    entry.desc.fields = entry.fields;
    br_array_init(&entry.data, (uint32_t)desc->size);
    br_array_init(&entry.owners, sizeof(br_node));
    br_array_init(&entry.sparse, sizeof(uint32_t));
    br_array_push(&s->components, &entry);
    return s->components.count;
}

uint32_t br_component_find(br_engine *e, const char *name)
{
    br_scene_state *s = e->scene;
    for (uint32_t i = 0; i < s->components.count; i++) {
        br_component_entry *c = br_array_at(&s->components, i);
        if (br_str_equal(c->name, name)) {
            return i + 1;
        }
    }
    return 0;
}

const br_component_desc *br_component_get_desc(br_engine *e, uint32_t component_id)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    return c != NULL ? &c->desc : NULL;
}

/* Dense row + 1 of node `index`, 0 if it has no such component. */
static uint32_t br_component_slot(br_component_entry *c, uint32_t index)
{
    uint32_t *slot = br_array_at(&c->sparse, index);
    return slot != NULL ? *slot : 0;
}

void *br_component_add(br_engine *e, br_node n, uint32_t component_id)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    if (c == NULL || br_scene_record(e, n) == NULL) {
        br_log_warn("br_component_add: invalid node or component %u", component_id);
        return NULL;
    }
    uint32_t slot = br_component_slot(c, n.index);
    if (slot != 0) {
        return br_array_at(&c->data, slot - 1);
    }
    if (c->sparse.count <= n.index && !br_array_resize(&c->sparse, n.index + 1)) {
        return NULL;
    }
    void *data = br_array_push(&c->data, NULL);
    if (data == NULL) {
        return NULL;
    }
    br_array_push(&c->owners, &n);
    *(uint32_t *)br_array_at(&c->sparse, n.index) = c->data.count;
    return data;
}

void *br_component_get(br_engine *e, br_node n, uint32_t component_id)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    if (c == NULL || br_scene_record(e, n) == NULL) {
        return NULL;
    }
    uint32_t slot = br_component_slot(c, n.index);
    return slot != 0 ? br_array_at(&c->data, slot - 1) : NULL;
}

static void br_component_remove_index(br_component_entry *c, uint32_t index)
{
    uint32_t slot = br_component_slot(c, index);
    if (slot == 0) {
        return;
    }
    uint32_t row = slot - 1;
    uint32_t last = c->data.count - 1;
    if (row != last) {
        /* swap-remove: the last instance moves into the hole, fix its owner's sparse entry */
        SDL_memcpy(br_array_at(&c->data, row), br_array_at(&c->data, last), c->desc.size);
        br_node moved = *(br_node *)br_array_at(&c->owners, last);
        *(br_node *)br_array_at(&c->owners, row) = moved;
        *(uint32_t *)br_array_at(&c->sparse, moved.index) = row + 1;
    }
    br_array_pop(&c->data);
    br_array_pop(&c->owners);
    *(uint32_t *)br_array_at(&c->sparse, index) = 0;
}

void br_component_remove(br_engine *e, br_node n, uint32_t component_id)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    if (c != NULL && br_scene_record(e, n) != NULL) {
        br_component_remove_index(c, n.index);
    }
}

uint32_t br_component_count(br_engine *e, uint32_t component_id)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    return c != NULL ? c->data.count : 0;
}

void *br_component_at(br_engine *e, uint32_t component_id, uint32_t i)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    return c != NULL ? br_array_at(&c->data, i) : NULL;
}

br_node br_component_owner_at(br_engine *e, uint32_t component_id, uint32_t i)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    br_node none = { 0, 0 };
    br_node *owner = c != NULL ? br_array_at(&c->owners, i) : NULL;
    return owner != NULL ? *owner : none;
}

void br_component_remove_all(br_scene_state *s, uint32_t index)
{
    for (uint32_t i = 0; i < s->components.count; i++) {
        br_component_remove_index(br_array_at(&s->components, i), index);
    }
}

void br_component_free_all(br_scene_state *s)
{
    for (uint32_t i = 0; i < s->components.count; i++) {
        br_component_entry *c = br_array_at(&s->components, i);
        for (uint32_t f = 0; f < c->desc.field_count; f++) {
            br_free((void *)c->fields[f].name);
        }
        br_free(c->fields);
        br_free(c->name);
        br_array_free(&c->data);
        br_array_free(&c->owners);
        br_array_free(&c->sparse);
    }
    br_array_free(&s->components);
}
