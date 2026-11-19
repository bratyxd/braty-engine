/* br_scene_file: build node trees from scene JSON, component fields, label references, save,
 * asset collection, named event handlers. */
#include "data/br_data_internal.h"

static br_node br_data_zero_node(void)
{
    br_node n = { 0, 0 };
    return n;
}

/* ---- stored JSON (what a node was loaded from; saving starts from it) */

void br_data_store_json(br_engine *e, br_node n, const cJSON *json)
{
    br_data_state *s = e->data;
    if (n.index >= s->stored.count && !br_array_resize(&s->stored, n.index + 1)) {
        return;
    }
    br_data_stored *st = br_array_at(&s->stored, n.index);
    cJSON_Delete(st->json);
    st->json = cJSON_Duplicate(json, true);
    st->generation = n.generation;
    if (st->json != NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(st->json, "children");
        cJSON_DeleteItemFromObjectCaseSensitive(st->json, "prefab");
    }
}

const cJSON *br_data_stored_json(br_engine *e, br_node n)
{
    br_data_stored *st = br_array_at(&e->data->stored, n.index);
    if (st == NULL || st->generation != n.generation) {
        return NULL;
    }
    return st->json;
}

/* ---- label references */

void br_data_add_ref(br_engine *e, const br_data_ref *ref)
{
    br_array_push(&e->data->refs, ref);
}

static br_node br_data_find_label(br_engine *e, br_node root, const char *label)
{
    if (br_node_valid(e, root)) {
        if (SDL_strcmp(br_node_label(e, root), label) == 0) {
            return root;
        }
        br_node found = br_node_find_by_label(e, root, label, true);
        if (br_node_valid(e, found)) {
            return found;
        }
    }
    return br_node_find_by_label(e, br_scene_root(e), label, true);
}

void br_data_resolve_refs(br_engine *e, br_node root)
{
    br_data_state *s = e->data;
    char saved_path[BR_DATA_PATH_SIZE];
    size_t saved_len = s->path_len;
    SDL_memcpy(saved_path, s->path, sizeof(saved_path));
    for (uint32_t i = 0; i < s->refs.count; i++) {
        br_data_ref *ref = br_array_at(&s->refs, i);
        if (!br_node_valid(e, ref->node)) {
            continue;
        }
        br_node target = br_data_find_label(e, root, ref->label);
        if (!br_node_valid(e, target)) {
            br_str_copy(s->path, sizeof(s->path), ref->path);
            s->path_len = SDL_strlen(s->path);
            br_data_errorf(e, "no node labelled '%s'", ref->label);
            continue;
        }
        if (ref->kind == BR_DATA_REF_FIELD) {
            uint8_t *data = br_component_get(e, ref->node, ref->component_id);
            if (data != NULL) {
                SDL_memcpy(data + ref->offset, &target, sizeof(br_node));
            }
        } else if (ref->kind == BR_DATA_REF_MASK) {
            br_node_set_mask(e, ref->node, target, ref->mask_kind, ref->inverse);
        } else {
            br_node_layer_attach(e, target, ref->node);
        }
    }
    br_array_clear(&s->refs);
    SDL_memcpy(s->path, saved_path, sizeof(saved_path));
    s->path_len = saved_len;
}

/* ---- component fields */

static bool br_data_value_vec3(br_engine *e, const cJSON *v, br_vec3 *out)
{
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 3) {
        float f[3];
        bool numbers = true;
        for (int i = 0; i < 3; i++) {
            const cJSON *item = cJSON_GetArrayItem(v, i);
            numbers = numbers && cJSON_IsNumber(item);
            f[i] = numbers ? (float)item->valuedouble : 0.0f;
        }
        if (numbers) {
            *out = br_vec3_make(f[0], f[1], f[2]);
            return true;
        }
    }
    br_data_errorf(e, "expected [x, y, z]");
    return false;
}

bool br_scene_file_read_field(br_engine *e, br_node owner, uint32_t component_id, const br_field_desc *field,
                              const cJSON *value)
{
    uint8_t *base = br_component_get(e, owner, component_id);
    if (base == NULL || field == NULL || value == NULL) {
        return false;
    }
    void *p = base + field->offset;
    switch (field->type) {
    case BR_FIELD_BOOL:
        if (!cJSON_IsBool(value)) {
            br_data_errorf(e, "expected true or false");
            return false;
        }
        *(bool *)p = cJSON_IsTrue(value);
        return true;
    case BR_FIELD_INT:
        if (!cJSON_IsNumber(value) || value->valuedouble != (double)(int32_t)value->valuedouble) {
            br_data_errorf(e, "expected a whole number");
            return false;
        }
        *(int32_t *)p = (int32_t)value->valuedouble;
        return true;
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
        if (!br_data_value_vec3(e, value, &v)) {
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
        if (cJSON_IsNull(value)) {
            br_node zero = br_data_zero_node();
            SDL_memcpy(p, &zero, sizeof(zero));
            return true;
        }
        if (!cJSON_IsString(value)) {
            br_data_errorf(e, "expected a node label (string) or null");
            return false;
        }
        if (e->data->load_depth > 0) {
            br_data_ref ref;
            SDL_zero(ref);
            ref.kind = BR_DATA_REF_FIELD;
            ref.node = owner;
            ref.component_id = component_id;
            ref.offset = field->offset;
            br_str_copy(ref.label, sizeof(ref.label), value->valuestring);
            br_str_copy(ref.path, sizeof(ref.path), e->data->path);
            br_data_add_ref(e, &ref);
            return true;
        }
        br_node target = br_node_find_by_label(e, br_scene_root(e), value->valuestring, true);
        if (!br_node_valid(e, target)) {
            br_data_errorf(e, "no node labelled '%s'", value->valuestring);
            return false;
        }
        SDL_memcpy(p, &target, sizeof(target));
        return true;
    }
    }
    br_data_errorf(e, "unknown field type %d", (int)field->type);
    return false;
}

cJSON *br_scene_file_write_field(br_engine *e, const br_field_desc *field, const void *component)
{
    const uint8_t *p = (const uint8_t *)component + field->offset;
    switch (field->type) {
    case BR_FIELD_BOOL: {
        bool b;
        SDL_memcpy(&b, p, sizeof(b));
        return cJSON_CreateBool(b);
    }
    case BR_FIELD_INT: {
        int32_t i;
        SDL_memcpy(&i, p, sizeof(i));
        return cJSON_CreateNumber(i);
    }
    case BR_FIELD_FLOAT: {
        float f;
        SDL_memcpy(&f, p, sizeof(f));
        return br_data_number(f);
    }
    case BR_FIELD_VEC2: {
        br_vec2 v;
        SDL_memcpy(&v, p, sizeof(v));
        return br_data_vec2_json(v);
    }
    case BR_FIELD_VEC3: {
        br_vec3 v;
        SDL_memcpy(&v, p, sizeof(v));
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, br_data_number(v.x));
        cJSON_AddItemToArray(a, br_data_number(v.y));
        cJSON_AddItemToArray(a, br_data_number(v.z));
        return a;
    }
    case BR_FIELD_COLOR: {
        br_color c;
        SDL_memcpy(&c, p, sizeof(c));
        return br_data_color_json(c);
    }
    case BR_FIELD_STRING:
    case BR_FIELD_ASSET: {
        char text[BR_FIELD_STRING_SIZE];
        SDL_memcpy(text, p, sizeof(text));
        text[BR_FIELD_STRING_SIZE - 1] = '\0';
        return cJSON_CreateString(text);
    }
    case BR_FIELD_NODE: {
        br_node n;
        SDL_memcpy(&n, p, sizeof(n));
        const char *label = br_node_label(e, n);
        if (!br_node_valid(e, n) || label[0] == '\0') {
            return cJSON_CreateNull(); /* unlabelled targets cannot be referenced from a file */
        }
        return cJSON_CreateString(label);
    }
    }
    return NULL;
}

bool br_scene_file_apply_component(br_engine *e, br_node n, const char *name, const cJSON *fields)
{
    uint32_t id = br_component_find(e, name);
    if (id == 0) {
        br_data_errorf(e, "unknown component '%s' (register it before loading)", name);
        return false;
    }
    if (br_component_add(e, n, id) == NULL) {
        br_data_errorf(e, "cannot add component '%s'", name);
        return false;
    }
    if (fields == NULL || cJSON_IsNull(fields)) {
        return true;
    }
    if (!cJSON_IsObject(fields)) {
        br_data_errorf(e, "expected an object of fields");
        return false;
    }
    const br_component_desc *desc = br_component_get_desc(e, id);
    bool ok = true;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, fields)
    {
        const br_field_desc *field = NULL;
        for (uint32_t i = 0; i < desc->field_count; i++) {
            if (SDL_strcmp(desc->fields[i].name, item->string) == 0) {
                field = &desc->fields[i];
            }
        }
        size_t mark = br_data_path_push_key(e, item->string);
        if (field == NULL) {
            br_data_errorf(e, "component '%s' has no field '%s'", name, item->string);
            ok = false;
        } else if (!br_scene_file_read_field(e, n, id, field, item)) {
            ok = false;
        }
        br_data_path_pop(e, mark);
    }
    return ok;
}

/* ---- building nodes */

static void br_data_preload_assets(br_engine *e, const cJSON *assets)
{
    if (!cJSON_IsArray(assets)) {
        br_data_error(e, "assets", "expected an array of asset paths");
        return;
    }
    size_t mark = br_data_path_push_key(e, "assets");
    int i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, assets)
    {
        size_t item_mark = br_data_path_push_index(e, i);
        if (!cJSON_IsString(item)) {
            br_data_errorf(e, "expected an asset path");
        } else if (br_assets_load(e, item->valuestring) == NULL) {
            br_data_errorf(e, "asset '%s' could not be loaded", item->valuestring);
        }
        br_data_path_pop(e, item_mark);
        i++;
    }
    br_data_path_pop(e, mark);
}

void br_data_apply_node(br_engine *e, br_node n, const cJSON *json, bool build_children)
{
    // ponytail: unknown keys (typos like "posiiton") are ignored silently; a per-type list of known keys would let us warn.
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
    if (assets != NULL) {
        br_data_preload_assets(e, assets); /* before anything that names them */
    }
    br_node_json_container_read(e, n, json);
    uint32_t type_id = br_node_type_of(e, n);
    const br_node_type_desc *desc = br_node_type_get(e, type_id);
    if (desc != NULL && desc->from_json != NULL && type_id != BR_NODE_TYPE_CONTAINER) {
        desc->from_json(e, n, br_node_data(e, n, type_id), json);
    }

    const cJSON *components = cJSON_GetObjectItemCaseSensitive(json, "components");
    if (components != NULL) {
        size_t mark = br_data_path_push_key(e, "components");
        if (!cJSON_IsObject(components)) {
            br_data_errorf(e, "expected an object { \"name\": { fields } }");
        } else {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, components)
            {
                size_t item_mark = br_data_path_push_key(e, item->string);
                br_scene_file_apply_component(e, n, item->string, item);
                br_data_path_pop(e, item_mark);
            }
        }
        br_data_path_pop(e, mark);
    }

    const cJSON *children = cJSON_GetObjectItemCaseSensitive(json, "children");
    if (build_children && children != NULL) {
        size_t mark = br_data_path_push_key(e, "children");
        if (!cJSON_IsArray(children)) {
            br_data_errorf(e, "expected an array of nodes");
        } else {
            int i = 0;
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, children)
            {
                size_t item_mark = br_data_path_push_index(e, i);
                br_data_build_node(e, item, n);
                br_data_path_pop(e, item_mark);
                i++;
            }
        }
        br_data_path_pop(e, mark);
    }
}

br_node br_data_build_node(br_engine *e, const cJSON *json, br_node parent)
{
    if (!cJSON_IsObject(json)) {
        br_data_errorf(e, "a node must be an object; skipped");
        return br_data_zero_node();
    }
    cJSON *expanded = NULL;
    if (cJSON_GetObjectItemCaseSensitive(json, "prefab") != NULL) {
        expanded = br_data_expand_prefab(e, json);
        if (expanded == NULL) {
            return br_data_zero_node(); /* error already reported */
        }
        json = expanded;
    }
    const char *type_name = "container";
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (type != NULL) {
        if (!cJSON_IsString(type)) {
            br_data_error(e, "type", "expected a node type name; node skipped");
            cJSON_Delete(expanded);
            return br_data_zero_node();
        }
        type_name = type->valuestring;
    }
    const br_data_type *t = br_data_find_type(e, type_name);
    uint32_t type_id = t != NULL ? t->type_id : br_node_type_find(e, type_name);
    if (type_id == 0) {
        size_t mark = br_data_path_push_key(e, "type");
        br_data_errorf(e, "unknown node type '%s'; node skipped", type_name);
        br_data_path_pop(e, mark);
        cJSON_Delete(expanded);
        return br_data_zero_node();
    }
    br_node n = t != NULL && t->create != NULL ? t->create(e, json) : br_node_create(e, type_id);
    if (!br_node_valid(e, n)) {
        br_data_errorf(e, "could not create a '%s' node; skipped", type_name);
        cJSON_Delete(expanded);
        return br_data_zero_node();
    }
    br_data_store_json(e, n, json);
    if (br_node_valid(e, parent)) {
        br_node_add_child(e, parent, n);
    }
    br_data_apply_node(e, n, json, true);
    cJSON_Delete(expanded);
    return n;
}

br_node br_scene_file_parse_node(br_engine *e, const cJSON *json, br_node parent)
{
    br_data_state *s = e->data;
    s->load_depth++;
    br_node n = br_data_build_node(e, json, parent);
    s->load_depth--;
    if (s->load_depth == 0) {
        br_data_resolve_refs(e, n);
    }
    return n;
}

br_node br_scene_file_load_json(br_engine *e, const char *json, size_t size, const char *source_name,
                                br_node parent)
{
    cJSON *doc = br_data_parse(e, source_name, json, size);
    if (doc == NULL) {
        return br_data_zero_node();
    }
    br_data_scope saved;
    br_data_begin_file(e, source_name, &saved);
    br_node n = br_scene_file_parse_node(e, doc, parent);
    br_data_end_file(e, &saved);
    cJSON_Delete(doc);
    return n;
}

static bool br_data_read_file(br_engine *e, const char *path, void **bytes, size_t *size)
{
    if (br_file_read_all(e, path, bytes, size)) {
        return true;
    }
    br_data_scope saved;
    br_data_begin_file(e, path, &saved);
    br_data_errorf(e, "cannot read the file");
    br_data_end_file(e, &saved);
    return false;
}

br_node br_scene_file_load(br_engine *e, const char *path, br_node parent)
{
    void *bytes = NULL;
    size_t size = 0;
    if (!br_data_read_file(e, path, &bytes, &size)) {
        return br_data_zero_node();
    }
    br_node n = br_scene_file_load_json(e, bytes, size, path, parent);
    br_free(bytes);
    if (br_node_valid(e, n)) {
        br_data_watch_add(e, path, n);
        br_log_info("scene file: loaded '%s'", path);
    }
    return n;
}

bool br_scene_file_reload(br_engine *e, const char *path, br_node root)
{
    if (!br_node_valid(e, root)) {
        return false;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (!br_data_read_file(e, path, &bytes, &size)) {
        return false;
    }
    cJSON *doc = br_data_parse(e, path, bytes, size);
    br_free(bytes);
    if (doc == NULL) {
        return false; /* keep the old scene when the edit does not parse */
    }
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        return false;
    }
    uint32_t count = br_node_child_count(e, root);
    for (uint32_t i = count; i > 0; i--) {
        br_node child = br_node_child_at(e, root, i - 1);
        if (br_node_valid(e, child)) {
            br_node_destroy(e, child);
        }
    }
    br_data_scope saved;
    br_data_begin_file(e, path, &saved);
    br_data_state *s = e->data;
    s->load_depth++;
    br_data_store_json(e, root, doc);
    br_data_apply_node(e, root, doc, true);
    s->load_depth--;
    if (s->load_depth == 0) {
        br_data_resolve_refs(e, root);
    }
    br_data_end_file(e, &saved);
    cJSON_Delete(doc);
    br_log_info("scene file: reloaded '%s'", path);
    return true;
}

/* ---- saving */

/* Engine-internal components are named "<module>.<name>" and are rebuilt by their node types. */
static bool br_data_component_saved(const br_component_desc *desc)
{
    return SDL_strchr(desc->name, '.') == NULL;
}

static cJSON *br_data_node_to_json(br_engine *e, br_node n)
{
    uint32_t type_id = br_node_type_of(e, n);
    const cJSON *stored = br_data_stored_json(e, n);
    cJSON *out = cJSON_CreateObject();

    /* "type" first, then whatever the file had, in its order */
    const cJSON *stored_type = cJSON_GetObjectItemCaseSensitive(stored, "type");
    const br_data_type *t = br_data_type_of(e, type_id);
    const br_node_type_desc *desc = br_node_type_get(e, type_id);
    const char *type_name = cJSON_IsString(stored_type) ? stored_type->valuestring
                            : t != NULL                 ? t->name
                                                        : desc->name;
    cJSON_AddStringToObject(out, "type", type_name);
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, stored)
    {
        if (SDL_strcmp(item->string, "type") != 0 && SDL_strcmp(item->string, "components") != 0) {
            cJSON_AddItemToObject(out, item->string, cJSON_Duplicate(item, true));
        }
    }

    br_node_json_container_write(e, n, out);
    const br_data_type *writer = t;
    if (cJSON_IsString(stored_type)) {
        const br_data_type *alias = br_data_find_type(e, stored_type->valuestring);
        writer = alias != NULL ? alias : t;
    }
    if (writer != NULL && writer->write != NULL) {
        writer->write(e, n, out);
    }

    cJSON *components = cJSON_CreateObject();
    for (uint32_t id = 1; br_component_get_desc(e, id) != NULL; id++) {
        const br_component_desc *cd = br_component_get_desc(e, id);
        const void *data = br_component_get(e, n, id);
        if (data == NULL || !br_data_component_saved(cd)) {
            continue;
        }
        cJSON *fields = cJSON_CreateObject();
        for (uint32_t f = 0; f < cd->field_count; f++) {
            cJSON *v = br_scene_file_write_field(e, &cd->fields[f], data);
            if (v != NULL) {
                cJSON_AddItemToObject(fields, cd->fields[f].name, v);
            }
        }
        cJSON_AddItemToObject(components, cd->name, fields);
    }
    if (cJSON_GetArraySize(components) > 0) {
        cJSON_AddItemToObject(out, "components", components);
    } else {
        cJSON_Delete(components);
    }

    cJSON *children = cJSON_CreateArray();
    uint32_t count = br_node_child_count(e, n);
    for (uint32_t i = 0; i < count; i++) {
        br_node child = br_node_child_at(e, n, i);
        if (br_node_valid(e, child)) {
            cJSON_AddItemToArray(children, br_data_node_to_json(e, child));
        }
    }
    if (cJSON_GetArraySize(children) > 0) {
        cJSON_AddItemToObject(out, "children", children);
    } else {
        cJSON_Delete(children);
    }
    return out;
}

cJSON *br_scene_file_to_json(br_engine *e, br_node root)
{
    if (!br_node_valid(e, root)) {
        return NULL;
    }
    return br_data_node_to_json(e, root);
}

bool br_scene_file_save(br_engine *e, br_node root, const char *path)
{
    cJSON *json = br_scene_file_to_json(e, root);
    if (json == NULL) {
        br_log_error("br_scene_file_save: invalid root node");
        return false;
    }
    char *text = cJSON_Print(json);
    cJSON_Delete(json);
    if (text == NULL) {
        return false;
    }
    bool ok = br_file_write_all(e, path, text, SDL_strlen(text));
    cJSON_free(text);
    return ok;
}

/* ---- asset collection */

static void br_data_report_path(const char *path, br_asset_path_fn fn, void *user)
{
    const char *hash = SDL_strchr(path, '#');
    if (hash == NULL) {
        fn(path, user);
        return;
    }
    char sheet[BR_DATA_PATH_SIZE];
    size_t len = (size_t)(hash - path);
    if (len < sizeof(sheet)) {
        SDL_memcpy(sheet, path, len);
        sheet[len] = '\0';
        fn(sheet, user);
    }
}

static bool br_data_is_asset_key(const char *key)
{
    static const char *keys[] = { "texture", "map", "font", "gif", "animation", "prefab", "svg_file" };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(keys); i++) {
        if (SDL_strcmp(keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void br_data_collect(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user)
{
    if (cJSON_IsArray(json)) {
        /* graphics commands: ["texture", "path", ...] */
        const cJSON *first = cJSON_GetArrayItem(json, 0);
        const cJSON *second = cJSON_GetArrayItem(json, 1);
        if (cJSON_IsString(first) && SDL_strcmp(first->valuestring, "texture") == 0 && cJSON_IsString(second)) {
            br_data_report_path(second->valuestring, fn, user);
        }
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, json)
    {
        const char *key = item->string;
        if (key != NULL && cJSON_IsString(item) && br_data_is_asset_key(key)) {
            br_data_report_path(item->valuestring, fn, user);
        } else if (key != NULL && cJSON_IsArray(item) &&
                   (SDL_strcmp(key, "assets") == 0 || SDL_strcmp(key, "frames") == 0)) {
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, item)
            {
                if (cJSON_IsString(entry)) {
                    br_data_report_path(entry->valuestring, fn, user);
                } else {
                    br_data_collect(e, entry, fn, user);
                }
            }
        } else if (key != NULL && cJSON_IsObject(item) && SDL_strcmp(key, "components") == 0) {
            const cJSON *component = NULL;
            cJSON_ArrayForEach(component, item)
            {
                const br_component_desc *desc = br_component_get_desc(e, br_component_find(e, component->string));
                for (uint32_t f = 0; desc != NULL && f < desc->field_count; f++) {
                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(component, desc->fields[f].name);
                    if (desc->fields[f].type == BR_FIELD_ASSET && cJSON_IsString(v)) {
                        fn(v->valuestring, user);
                    }
                }
            }
        } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
            br_data_collect(e, item, fn, user);
        }
    }
}

void br_scene_file_collect_assets(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user)
{
    if (json != NULL && fn != NULL) {
        br_data_collect(e, json, fn, user);
    }
}

/* ---- named handlers */

void br_scene_file_add_handler(br_engine *e, const char *name, br_event_listener_fn fn, void *user)
{
    br_data_handler h = { fn, user };
    br_map_set(&e->data->handlers, name, &h);
}
