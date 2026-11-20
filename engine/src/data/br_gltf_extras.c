/* br_gltf_extras: glTF node extras -> 3D components and authoring conventions.
 *
 * Applied to every node of a fresh instance (br_model_instantiate_ex calls it):
 *   - name suffixes: "-col" (collider), "-colonly" (collider, not drawn), "-trigger" (trigger
 *     collider, not drawn), "-norender" (not drawn)
 *   - convention keys: collider, trigger, no_render, visible, layer, cast_shadows (batch is read
 *     when the instance is built)
 *   - { "components": { "health": { "max": 5 } } }, flat "health.max": 5 (Blender custom
 *     properties), "health": true / { ... } -> registered components on the node's group node.
 * Errors name the file, the node and the field ("level.glb: node 'Coin'.extras.spin.speed:
 * expected a number"); bad values are skipped and the rest still applies.
 */
#include "data/br_gltf_scene_internal.h"
#include "three_d/br_gltf_internal.h"

/* Keys with an engine meaning, never read as components. */
static const char *br_gltf_convention_keys = " components collider trigger no_render visible layer cast_shadows batch ";

static bool br_gltf_is_convention(const char *key)
{
    char word[BR_DATA_NAME_SIZE + 2];
    SDL_snprintf(word, sizeof(word), " %s ", key);
    return SDL_strstr(br_gltf_convention_keys, word) != NULL;
}

/* true / false, or 1 / 0 (Blender exports older boolean properties as integers). */
static bool br_gltf_bool(const cJSON *v, bool *out)
{
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    if (cJSON_IsNumber(v) && (v->valuedouble == 0.0 || v->valuedouble == 1.0)) {
        *out = v->valuedouble != 0.0;
        return true;
    }
    return false;
}

static bool br_gltf_vec3(br_engine *e, const cJSON *v, br_vec3 *out)
{
    float f[3] = { 0.0f, 0.0f, 0.0f };
    bool ok = cJSON_IsArray(v) && cJSON_GetArraySize(v) == 3;
    for (int i = 0; ok && i < 3; i++) {
        const cJSON *item = cJSON_GetArrayItem(v, i);
        ok = cJSON_IsNumber(item);
        f[i] = ok ? (float)item->valuedouble : 0.0f;
    }
    if (!ok) {
        br_data_errorf(e, "expected [x, y, z]");
        return false;
    }
    *out = br_vec3_make(f[0], f[1], f[2]);
    return true;
}

/* One value into one field of a component row (errors at the current JSON path). */
static bool br_gltf_read_field(br_engine *e, uint8_t *base, const br_field_desc *field, const cJSON *value,
                               br_node3d search_root)
{
    void *p = base + field->offset;
    switch (field->type) {
    case BR_FIELD_BOOL: {
        bool b = false;
        if (!br_gltf_bool(value, &b)) {
            br_data_errorf(e, "expected true or false");
            return false;
        }
        SDL_memcpy(p, &b, sizeof(b));
        return true;
    }
    case BR_FIELD_INT: {
        int whole = 0;
        if (!br_data_value_int(e, value, INT32_MIN, INT32_MAX, &whole)) {
            return false;
        }
        int32_t i = (int32_t)whole;
        SDL_memcpy(p, &i, sizeof(i));
        return true;
    }
    case BR_FIELD_FLOAT: {
        float f;
        if (!br_data_value_float(e, value, &f)) {
            return false;
        }
        SDL_memcpy(p, &f, sizeof(f));
        return true;
    }
    case BR_FIELD_VEC2: {
        br_vec2 v;
        if (!br_data_value_vec2(e, value, &v)) {
            return false;
        }
        SDL_memcpy(p, &v, sizeof(v));
        return true;
    }
    case BR_FIELD_VEC3: {
        br_vec3 v;
        if (!br_gltf_vec3(e, value, &v)) {
            return false;
        }
        SDL_memcpy(p, &v, sizeof(v));
        return true;
    }
    case BR_FIELD_COLOR: {
        br_color c;
        if (!br_data_value_color(e, value, &c)) {
            return false;
        }
        SDL_memcpy(p, &c, sizeof(c));
        return true;
    }
    case BR_FIELD_STRING:
    case BR_FIELD_ASSET:
        if (!cJSON_IsString(value)) {
            br_data_errorf(e, field->type == BR_FIELD_ASSET ? "expected an asset path" : "expected a string");
            return false;
        }
        if (!br_str_copy(p, BR_FIELD_STRING_SIZE, value->valuestring)) {
            br_data_errorf(e, "string longer than %d bytes", BR_FIELD_STRING_SIZE - 1);
            return false;
        }
        return true;
    case BR_FIELD_NODE: {
        /* a 3D component's node field holds a br_node3d (same layout as br_node) */
        br_node3d target = { 0, 0 };
        if (!cJSON_IsNull(value)) {
            if (!cJSON_IsString(value)) {
                br_data_errorf(e, "expected a node name (string) or null");
                return false;
            }
            target = br_node3d_find_by_label(e, search_root, value->valuestring);
            if (!br_node3d_valid(e, target)) {
                br_data_errorf(e, "no node named '%s'", value->valuestring);
                return false;
            }
        }
        SDL_memcpy(p, &target, sizeof(target));
        return true;
    }
    }
    br_data_errorf(e, "unknown field type %d", (int)field->type);
    return false;
}

static const br_field_desc *br_gltf_find_field(const br_component_desc *desc, const char *name)
{
    for (uint32_t i = 0; i < desc->field_count; i++) {
        if (SDL_strcmp(desc->fields[i].name, name) == 0) {
            return &desc->fields[i];
        }
    }
    return NULL;
}

/* Reads one field by name into n's component (added if missing). */
static bool br_gltf_set_field(br_engine *e, br_node3d n, uint32_t id, const char *field_name, const cJSON *value,
                              br_node3d search_root)
{
    const br_component_desc *desc = br_component_get_desc(e, id);
    const br_field_desc *field = br_gltf_find_field(desc, field_name);
    if (field == NULL) {
        br_data_errorf(e, "component '%s' has no field '%s'", desc->name, field_name);
        return false;
    }
    uint8_t *base = br_component3d_add(e, n, id);
    if (base == NULL) {
        br_data_errorf(e, "cannot add component '%s'", desc->name);
        return false;
    }
    return br_gltf_read_field(e, base, field, value, search_root);
}

bool br_data_component3d_apply(br_engine *e, br_node3d n, const char *name, const cJSON *fields, br_node3d search_root,
                               bool lenient)
{
    uint32_t id = br_component_find(e, name);
    if (id == 0) {
        if (lenient) {
            br_data_warnf(e, "unknown component '%s' (not registered; ignored)", name);
            return true;
        }
        br_data_errorf(e, "unknown component '%s' (register it before loading)", name);
        return false;
    }
    bool defaults = fields == NULL || cJSON_IsNull(fields) || cJSON_IsTrue(fields);
    if (!defaults && !cJSON_IsObject(fields)) {
        br_data_errorf(e, "expected true or an object of fields");
        return false;
    }
    if (br_component3d_add(e, n, id) == NULL) {
        br_data_errorf(e, "cannot add component '%s'", name);
        return false;
    }
    if (defaults) {
        return true;
    }
    bool ok = true;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, fields)
    {
        size_t mark = br_data_path_push_key(e, item->string);
        ok = br_gltf_set_field(e, n, id, item->string, item, search_root) && ok;
        br_data_path_pop(e, mark);
    }
    return ok;
}

/* A flat key: "health.max" (component health, field max; the last dot splits, so dotted
 * component names work) or "health" (true / { fields }). Keys naming no registered component are
 * custom properties for something else and are skipped. */
static void br_gltf_flat_key(br_engine *e, br_node3d n, const cJSON *item, br_node3d search_root)
{
    const char *key = item->string;
    size_t mark = br_data_path_push_key(e, key);
    uint32_t whole = br_component_find(e, key);
    if (whole != 0) {
        if (!cJSON_IsFalse(item)) {
            br_data_component3d_apply(e, n, key, item, search_root, false);
        }
        br_data_path_pop(e, mark);
        return;
    }
    const char *dot = SDL_strrchr(key, '.');
    char component[BR_DATA_NAME_SIZE];
    size_t len = dot != NULL ? (size_t)(dot - key) : 0;
    if (dot != NULL && len > 0 && len < sizeof(component)) {
        SDL_memcpy(component, key, len);
        component[len] = '\0';
        uint32_t id = br_component_find(e, component);
        if (id != 0) {
            br_gltf_set_field(e, n, id, dot + 1, item, search_root);
        }
    }
    br_data_path_pop(e, mark);
}

static bool br_gltf_ends_with(const char *s, const char *suffix)
{
    size_t a = SDL_strlen(s);
    size_t b = SDL_strlen(suffix);
    return a > b && SDL_strcmp(s + a - b, suffix) == 0;
}

/* True for the content children of a glTF node group (mesh3d, batched_mesh3d, camera, light). */
static bool br_gltf_is_content(br_engine *e, br_node3d c)
{
    uint32_t type = br_node3d_type_of(e, c);
    return type != BR_NODE3D_TYPE_GROUP && type != br_model3d_type(e);
}

static void br_gltf_hide_meshes(br_engine *e, br_node3d g)
{
    for (br_node3d c = br_node3d_first_child(e, g); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        uint32_t type = br_node3d_type_of(e, c);
        if (type == br_mesh3d_type(e) || type == br_batched_mesh3d_type(e)) {
            br_node3d_set_visible(e, c, false);
            br_node3d_set_pickable(e, c, false);
        }
    }
}

static void br_gltf_set_layer(br_engine *e, br_node3d g, uint32_t layer)
{
    br_node3d_set_layer(e, g, layer);
    for (br_node3d c = br_node3d_first_child(e, g); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        if (br_gltf_is_content(e, c)) {
            br_node3d_set_layer(e, c, layer);
        }
    }
}

static void br_gltf_set_shadows(br_engine *e, br_node3d g, bool cast)
{
    for (br_node3d c = br_node3d_first_child(e, g); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        br_mesh3d *mesh = br_mesh3d_get(e, c);
        br_light3d *light = br_light3d_get(e, c);
        if (mesh != NULL) {
            mesh->cast_shadows = cast;
        } else if (light != NULL) {
            light->cast_shadows = cast;
        } else if (br_node3d_type_of(e, c) == br_batched_mesh3d_type(e)) {
            br_batched_mesh3d_set_cast_shadows(e, c, cast);
        }
    }
}

/* A box collider from the node's mesh bounds (a unit cube for empties). */
static void br_gltf_add_collider(br_engine *e, const br_gltf_instance *inst, uint32_t i, bool trigger)
{
    br_gltf_collider *c = br_component3d_add(e, inst->nodes[i], br_gltf_collider_component(e));
    if (c == NULL) {
        return;
    }
    const br_model *m = inst->model;
    br_aabb box = br_aabb_empty();
    if (m->nodes[i].mesh >= 0) {
        const br_model_mesh *mesh = &m->meshes[m->nodes[i].mesh];
        for (uint32_t p = 0; p < mesh->primitive_count; p++) {
            box = br_aabb_union(box, br_geometry3d_bounds(e, m->primitives[mesh->first_primitive + p].geometry));
        }
    }
    if (!br_aabb_is_valid(box)) {
        box.min = br_vec3_make(-0.5f, -0.5f, -0.5f);
        box.max = br_vec3_make(0.5f, 0.5f, 0.5f);
    }
    c->center = br_vec3_scale(br_vec3_add(box.min, box.max), 0.5f);
    c->size = br_vec3_sub(box.max, box.min);
    c->trigger = c->trigger || trigger;
}

/* Reads a convention key's boolean (errors for anything else). */
static bool br_gltf_flag(br_engine *e, const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!br_gltf_bool(v, out)) {
        br_data_error(e, key, "expected true or false");
        return false;
    }
    return true;
}

static void br_gltf_apply_node(br_engine *e, br_gltf_instance *inst, uint32_t i)
{
    const br_model_node *mn = &inst->model->nodes[i];
    br_node3d g = inst->nodes[i];
    bool collider = br_gltf_ends_with(mn->name, "-col") || br_gltf_ends_with(mn->name, "-colonly");
    bool trigger = br_gltf_ends_with(mn->name, "-trigger");
    bool no_render = br_gltf_ends_with(mn->name, "-colonly") || br_gltf_ends_with(mn->name, "-norender");
    cJSON *extras = NULL;
    if (mn->extras != NULL) {
        // ponytail: extras text is parsed per instance (cheap for gameplay-sized objects); cache a cJSON per model node if kits with huge extras show up in profiles.
        extras = cJSON_Parse(mn->extras);
        if (!cJSON_IsObject(extras)) {
            cJSON_Delete(extras); /* a number or string as extras carries nothing we read */
            extras = NULL;
        }
    }
    if (extras != NULL) {
        bool flag = false;
        const cJSON *col = cJSON_GetObjectItemCaseSensitive(extras, "collider");
        if (col != NULL && !cJSON_IsFalse(col) && !(cJSON_IsNumber(col) && col->valuedouble == 0.0)) {
            // ponytail: every collider is a box from the mesh bounds; read "sphere" / "capsule" shape names when physics needs them.
            collider = true;
        }
        if (br_gltf_flag(e, extras, "trigger", &flag) && flag) {
            trigger = true;
        }
        if (br_gltf_flag(e, extras, "no_render", &flag)) {
            no_render = flag;
        }
        if (br_gltf_flag(e, extras, "visible", &flag)) {
            br_node3d_set_visible(e, g, flag);
        }
        int layer = 0;
        if (br_data_get_int(e, extras, "layer", &layer)) {
            if (layer < 0) {
                br_data_error(e, "layer", "expected layer bits >= 0");
            } else {
                br_gltf_set_layer(e, g, (uint32_t)layer);
            }
        }
        if (br_gltf_flag(e, extras, "cast_shadows", &flag)) {
            br_gltf_set_shadows(e, g, flag);
        }
    }
    if (trigger) {
        no_render = true; /* trigger volumes are invisible */
    }
    if (collider || trigger) {
        br_gltf_add_collider(e, inst, i, trigger);
    }
    if (no_render) {
        br_gltf_hide_meshes(e, g);
    }
    if (extras == NULL) {
        return;
    }
    const cJSON *components = cJSON_GetObjectItemCaseSensitive(extras, "components");
    if (components != NULL) {
        size_t mark = br_data_path_push_key(e, "components");
        if (!cJSON_IsObject(components)) {
            br_data_errorf(e, "expected an object { \"name\": { fields } }");
        } else {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, components)
            {
                size_t item_mark = br_data_path_push_key(e, item->string);
                br_data_component3d_apply(e, g, item->string, item, inst->self, true);
                br_data_path_pop(e, item_mark);
            }
        }
        br_data_path_pop(e, mark);
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, extras)
    {
        if (!br_gltf_is_convention(item->string)) {
            br_gltf_flat_key(e, g, item, inst->self);
        }
    }
    cJSON_Delete(extras);
}

void br_data_gltf_apply_extras(br_engine *e, br_gltf_instance *inst)
{
    if (e->data == NULL) {
        return;
    }
    const br_model *m = inst->model;
    br_data_scope saved;
    br_data_begin_file(e, m->path != NULL ? m->path : "<model>", &saved);
    for (uint32_t i = 0; i < m->node_count; i++) {
        const br_model_node *mn = &m->nodes[i];
        if (!br_node3d_valid(e, inst->nodes[i]) || (mn->extras == NULL && !br_gltf_name_has_convention(mn->name))) {
            continue;
        }
        char key[BR_DATA_NAME_SIZE + 16];
        SDL_snprintf(key, sizeof(key), "node '%s'", mn->name);
        size_t mark = br_data_path_push_key(e, key);
        size_t extras_mark = br_data_path_push_key(e, "extras");
        br_gltf_apply_node(e, inst, i);
        br_data_path_pop(e, extras_mark);
        br_data_path_pop(e, mark);
    }
    br_data_end_file(e, &saved);
}
