/* br_component: user components as dense arrays (sparse set: node index -> dense row). */
#include "scene/br_scene_internal.h"

static br_component_entry *br_component_entry_get(br_engine *e, uint32_t component_id)
{
    if (component_id == 0 || e->scene == NULL) {
        return NULL;
    }
    return br_array_at(&e->scene->components, component_id - 1);
}

size_t br_field_type_size(br_field_type type)
{
    switch (type) {
    case BR_FIELD_BOOL: return sizeof(bool);
    case BR_FIELD_INT: return sizeof(int32_t);
    case BR_FIELD_FLOAT: return sizeof(float);
    case BR_FIELD_VEC2: return sizeof(br_vec2);
    case BR_FIELD_VEC3: return sizeof(br_vec3);
    case BR_FIELD_COLOR: return sizeof(br_color);
    case BR_FIELD_STRING: return BR_FIELD_STRING_SIZE;
    case BR_FIELD_ASSET: return BR_FIELD_STRING_SIZE;
    case BR_FIELD_NODE: return sizeof(br_node);
    case BR_FIELD_TEXT: return sizeof(char *);
    case BR_FIELD_ENUM: return sizeof(int32_t);
    case BR_FIELD_VEC4: return sizeof(br_vec4);
    case BR_FIELD_QUAT: return sizeof(br_quat);
    case BR_FIELD_NODE3D: return sizeof(br_node3d);
    case BR_FIELD_ARRAY: return 0; /* depends on element_type and capacity */
    }
    return 0;
}

bool br_component_set_text(char **field, const char *text)
{
    char *copy = NULL;
    if (text != NULL) {
        copy = br_strdup(text);
        if (copy == NULL) {
            return false;
        }
    }
    br_free(*field);
    *field = copy;
    return true;
}

/* Why a field cannot be stored in a struct of `size` bytes, NULL if it can (arrays are checked
 * by br_component_set_array). */
static const char *br_field_problem(const br_field_desc *f, size_t size)
{
    if (f->name == NULL) {
        return "has no name";
    }
    size_t bytes = br_field_type_size(f->type);
    if (bytes == 0 && f->type != BR_FIELD_ARRAY) {
        return "has an unknown type";
    }
    if (f->offset > size || bytes > size - f->offset) {
        return "does not fit in the struct";
    }
    return NULL;
}

void br_component_release_row(const br_component_desc *desc, void *row)
{
    for (uint32_t i = 0; i < desc->field_count; i++) {
        if (desc->fields[i].type == BR_FIELD_TEXT) {
            char **text = (char **)((uint8_t *)row + desc->fields[i].offset);
            br_free(*text);
            *text = NULL;
        }
    }
}

static void br_field_extra_free_names(br_field_extra *x)
{
    for (uint32_t i = 0; x->enum_names != NULL && i < x->enum_count; i++) {
        br_free(x->enum_names[i]);
    }
    br_free(x->enum_names);
    x->enum_names = NULL;
    x->enum_count = 0;
}

uint32_t br_component_register(br_engine *e, const br_component_desc *desc)
{
    br_scene_state *s = e->scene;
    if (desc == NULL || desc->name == NULL || desc->size == 0) {
        br_log_error("br_component_register: a component needs a name and a size");
        return 0;
    }
    /* scene files and glTF extras write at base + offset: a field must lie inside the struct */
    for (uint32_t i = 0; desc->fields != NULL && i < desc->field_count; i++) {
        const br_field_desc *f = &desc->fields[i];
        const char *problem = br_field_problem(f, desc->size);
        if (problem != NULL) {
            br_log_error("component '%s': field %u ('%s') %s (%u bytes)", desc->name, i, f->name != NULL ? f->name : "",
                         problem, (unsigned)desc->size);
            return 0;
        }
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
        entry.extras = br_alloc(sizeof(br_field_extra) * desc->field_count);
        if (entry.fields == NULL || entry.extras == NULL) {
            br_free(entry.fields);
            br_free(entry.extras);
            br_free(entry.name);
            return 0;
        }
        SDL_memset(entry.extras, 0, sizeof(br_field_extra) * desc->field_count);
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

/* The registered field `name` of a component and its extra description (logged when missing). */
static br_field_desc *br_component_field_named(br_engine *e, uint32_t component_id, const char *name,
                                               br_field_extra **extra, const char *caller)
{
    br_component_entry *c = br_component_entry_get(e, component_id);
    for (uint32_t i = 0; c != NULL && name != NULL && i < c->desc.field_count; i++) {
        if (SDL_strcmp(c->fields[i].name, name) == 0) {
            *extra = &c->extras[i];
            return &c->fields[i];
        }
    }
    br_log_error("%s: component %u has no field '%s'", caller, component_id, name != NULL ? name : "");
    return NULL;
}

bool br_component_set_enum(br_engine *e, uint32_t component_id, const char *field, const char *const *names,
                           uint32_t count)
{
    br_field_extra *x = NULL;
    br_field_desc *f = br_component_field_named(e, component_id, field, &x, "br_component_set_enum");
    if (f == NULL) {
        return false;
    }
    if (f->type != BR_FIELD_ENUM && f->type != BR_FIELD_ARRAY) {
        br_log_error("br_component_set_enum: field '%s' is not BR_FIELD_ENUM or BR_FIELD_ARRAY", field);
        return false;
    }
    bool ok = names != NULL && count > 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        ok = names[i] != NULL;
    }
    if (!ok) {
        br_log_error("br_component_set_enum: field '%s' needs at least one name and no NULL names", field);
        return false;
    }
    br_field_extra_free_names(x);
    x->enum_names = br_alloc(sizeof(char *) * count);
    if (x->enum_names == NULL) {
        return false;
    }
    x->enum_count = count;
    for (uint32_t i = 0; i < count; i++) {
        x->enum_names[i] = br_strdup(names[i]);
        ok = ok && x->enum_names[i] != NULL;
    }
    if (!ok) {
        br_field_extra_free_names(x);
    }
    return ok;
}

bool br_component_set_array(br_engine *e, uint32_t component_id, const char *field, br_field_type element_type,
                            uint32_t capacity, size_t count_offset)
{
    br_field_extra *x = NULL;
    br_field_desc *f = br_component_field_named(e, component_id, field, &x, "br_component_set_array");
    if (f == NULL) {
        return false;
    }
    size_t size = br_component_desc_get(e, component_id)->size;
    size_t bytes = br_field_type_size(element_type);
    const char *problem = NULL;
    if (f->type != BR_FIELD_ARRAY) {
        problem = "is not BR_FIELD_ARRAY";
    } else if (bytes == 0 || element_type == BR_FIELD_TEXT) {
        problem = "needs an element type other than text and array";
    } else if (capacity == 0 || bytes > (size - f->offset) / capacity) {
        problem = "has no capacity or its elements do not fit in the struct";
    } else if (count_offset > size || sizeof(uint32_t) > size - count_offset ||
               (count_offset + sizeof(uint32_t) > f->offset && count_offset < f->offset + bytes * capacity)) {
        problem = "has its uint32_t count outside the struct or inside the elements";
    }
    if (problem != NULL) {
        br_log_error("br_component_set_array: field '%s' %s", field, problem);
        return false;
    }
    x->element_type = element_type;
    x->capacity = capacity;
    x->count_offset = count_offset;
    return true;
}

const br_field_extra *br_component_field_extra(br_engine *e, const br_field_desc *field)
{
    br_scene_state *s = e->scene;
    for (uint32_t i = 0; s != NULL && i < s->components.count; i++) {
        br_component_entry *c = br_array_at(&s->components, i);
        uintptr_t first = (uintptr_t)c->fields;
        uintptr_t at = (uintptr_t)field;
        if (c->desc.field_count > 0 && at >= first && at < first + sizeof(br_field_desc) * c->desc.field_count) {
            return &c->extras[(at - first) / sizeof(br_field_desc)];
        }
    }
    return NULL;
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

const br_component_desc *br_component_desc_get(br_engine *e, uint32_t component_id)
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
    br_component_release_row(&c->desc, br_array_at(&c->data, row));
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
        for (uint32_t r = 0; r < c->data.count; r++) {
            br_component_release_row(&c->desc, br_array_at(&c->data, r));
        }
        for (uint32_t f = 0; f < c->desc.field_count; f++) {
            br_free((void *)c->fields[f].name);
            br_field_extra_free_names(&c->extras[f]);
        }
        br_free(c->extras);
        br_free(c->fields);
        br_free(c->name);
        br_array_free(&c->data);
        br_array_free(&c->owners);
        br_array_free(&c->sparse);
    }
    br_array_free(&s->components);
}
