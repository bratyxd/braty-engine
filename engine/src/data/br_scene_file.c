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
    return br_node_find_by_label(e, br_stage_root(e), label, true);
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
        float f[3] = { 0.0f, 0.0f, 0.0f };
        bool numbers = true;
        for (int i = 0; i < 3; i++) {
            numbers = numbers && cJSON_IsNumber(cJSON_GetArrayItem(v, i));
        }
        if (numbers) {
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++) {
                ok = br_data_value_float(e, cJSON_GetArrayItem(v, i), &f[i]);
            }
            if (ok) {
                *out = br_vec3_make(f[0], f[1], f[2]);
            }
            return ok;
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
    case BR_FIELD_INT: {
        int i = 0;
        if (!br_data_value_int(e, value, INT32_MIN, INT32_MAX, &i)) {
            return false;
        }
        int32_t i32 = (int32_t)i;
        SDL_memcpy(p, &i32, sizeof(i32));
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
        br_node target = br_node_find_by_label(e, br_stage_root(e), value->valuestring, true);
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
    const br_component_desc *desc = br_component_desc_get(e, id);
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

bool br_data_assets_entry(br_engine *e, const cJSON *item, br_data_asset_entry_fn fn, void *user)
{
    const char *name = NULL;
    bool lazy = false;
    if (cJSON_IsString(item)) {
        name = item->valuestring;
    } else if (cJSON_IsObject(item)) {
        /* { "path": "a.png", "load": "lazy" } or { "bundle": "music", "load": "preload" } */
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
        const cJSON *bundle = cJSON_GetObjectItemCaseSensitive(item, "bundle");
        const cJSON *load = cJSON_GetObjectItemCaseSensitive(item, "load");
        name = cJSON_IsString(path) ? path->valuestring : cJSON_IsString(bundle) ? bundle->valuestring : NULL;
        if (load != NULL) {
            if (cJSON_IsString(load) && SDL_strcmp(load->valuestring, "lazy") == 0) {
                lazy = true;
            } else if (!cJSON_IsString(load) || SDL_strcmp(load->valuestring, "preload") != 0) {
                br_data_error(e, "load", "expected \"preload\" or \"lazy\"");
                return false;
            }
        }
        if (cJSON_IsString(bundle) && br_assets_bundle_size(e, bundle->valuestring) == 0) {
            br_data_error(e, "bundle", "unknown bundle (add the manifest first)");
            return false;
        }
    }
    if (name == NULL) {
        br_data_errorf(e, "expected an asset path, a bundle name or { \"path\" | \"bundle\", \"load\" }");
        return false;
    }
    /* a manifest bundle name lists its assets; anything else is one asset path */
    uint32_t bundle_size = br_assets_bundle_size(e, name);
    if (bundle_size > 0) {
        for (uint32_t i = 0; i < bundle_size; i++) {
            fn(e, br_assets_bundle_path(e, name, i), lazy, user);
        }
    } else {
        fn(e, name, lazy, user);
    }
    return true;
}

static void br_data_preload_one(br_engine *e, const char *path, bool lazy, void *user)
{
    (void)user;
    if (!lazy && br_assets_load(e, path) == NULL) {
        br_data_errorf(e, "asset '%s' could not be loaded", path);
    }
}

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
        br_data_assets_entry(e, item, br_data_preload_one, NULL);
        br_data_path_pop(e, item_mark);
        i++;
    }
    br_data_path_pop(e, mark);
}

/* ---- unknown keys: a typo in a data file ("posiiton") would otherwise be ignored silently */

/* Keys every node object may have (section 2.1 of docs/DATA_FORMATS.md). */
static const char *br_data_container_keys =
    " type label position scale skew pivot origin rotation angle alpha tint visible renderable z_index"
    " sortable_children cullable cullable_children render_group cache_as_texture cull_area blend mask layer"
    " filters filter_resolution filter_area event_mode interactive_children cursor hit_area on components"
    " children assets prefab ";

/* Type-specific keys per JSON type name (section 2.2). */
typedef struct br_data_known_keys {
    const char *type;
    const char *keys;   /* space-separated, with a space at both ends */
} br_data_known_keys;

static const br_data_known_keys br_data_type_keys[] = {
    { "container", " " },
    { "sprite", " texture anchor round_pixels width height " },
    { "animated_sprite",
      " texture anchor round_pixels width height animation gif frames fps speed loop playing frame update_anchor " },
    { "nine_slice", " texture borders width height anchor edge_fill center_fill hide_center " },
    { "nine_slice_sprite", " texture borders width height anchor edge_fill center_fill hide_center " },
    { "tiling_sprite", " texture width height anchor tile_position tile_scale tile_rotation clamp_margin " },
    { "graphics", " commands svg fill stroke " },
    { "mesh", " texture positions uvs colors indices plane perspective rope " },
    { "mesh_simple", " texture positions uvs colors indices plane perspective rope " },
    { "mesh_plane", " texture positions uvs colors indices plane perspective rope " },
    { "perspective_mesh", " texture positions uvs colors indices plane perspective rope " },
    { "mesh_rope", " texture positions uvs colors indices plane perspective rope " },
    { "particle_container", " texture dynamic particles " },
    { "text", " text font size style anchor resolution " },
    { "bitmap_text", " text font size style anchor resolution " },
    { "rich_text", " text font size style anchor resolution tag_styles " },
};

static const char *br_data_text_style_keys =
    " font font_family font_size bold italic fill gradient stroke drop_shadow align word_wrap wrap_width"
    " break_words line_height leading letter_spacing padding white_space baseline ";

static bool br_data_key_listed(const char *list, const char *key)
{
    char word[BR_DATA_NAME_SIZE + 2];
    SDL_snprintf(word, sizeof(word), " %s ", key);
    return SDL_strstr(list, word) != NULL;
}

/* Warns (does not fail) for every key of obj that neither list names. */
static void br_data_warn_keys(br_engine *e, const cJSON *obj, const char *list, const char *extra)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, obj)
    {
        if (br_data_key_listed(list, item->string) || (extra != NULL && br_data_key_listed(extra, item->string))) {
            continue;
        }
        size_t mark = br_data_path_push_key(e, item->string);
        br_data_warnf(e, "unknown key (ignored; a typo?)");
        br_data_path_pop(e, mark);
    }
}

/* Node keys are checked for the built-in types only: other types (yours, view3d) read keys we
 * cannot know about. */
static void br_data_check_keys(br_engine *e, const cJSON *json)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const char *type_name = cJSON_IsString(type) ? type->valuestring : "container";
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(br_data_type_keys); i++) {
        if (SDL_strcmp(br_data_type_keys[i].type, type_name) == 0) {
            br_data_warn_keys(e, json, br_data_container_keys, br_data_type_keys[i].keys);
            const cJSON *style = cJSON_GetObjectItemCaseSensitive(json, "style");
            if (cJSON_IsObject(style)) {
                size_t mark = br_data_path_push_key(e, "style");
                br_data_warn_keys(e, style, br_data_text_style_keys, NULL);
                br_data_path_pop(e, mark);
            }
            return;
        }
    }
}

void br_data_apply_node(br_engine *e, br_node n, const cJSON *json, bool build_children)
{
    br_data_check_keys(e, json);
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

static br_node br_data_build_node_at(br_engine *e, const cJSON *json, br_node parent);

br_node br_data_build_node(br_engine *e, const cJSON *json, br_node parent)
{
    br_data_state *s = e->data;
    if (s->node_depth >= BR_DATA_MAX_NODE_DEPTH) {
        br_data_errorf(e, "nodes nest deeper than %d levels; skipped", BR_DATA_MAX_NODE_DEPTH);
        return br_data_zero_node();
    }
    uint32_t prefab_mark = s->prefab_depth; /* the prefabs this node pushes stay until its subtree is built */
    s->node_depth++;
    br_node n = br_data_build_node_at(e, json, parent);
    s->node_depth--;
    s->prefab_depth = prefab_mark;
    return n;
}

static br_node br_data_build_node_at(br_engine *e, const cJSON *json, br_node parent)
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

/* The group a scene-file root keeps its references in (created on first use). */
static br_asset_group *br_data_root_group(br_engine *e, br_node root)
{
    br_data_stored *st = br_array_at(&e->data->stored, root.index);
    if (st == NULL || st->generation != root.generation) {
        return NULL;
    }
    if (st->assets == NULL) {
        char name[BR_DATA_PATH_SIZE + 8];
        SDL_snprintf(name, sizeof(name), "file:%s", e->data->file);
        st->assets = br_asset_group_create(e, name);
    }
    return st->assets;
}

/* Ends the capture of an outermost load: the references go to the br_scene that asked for the
 * load, else to the root (released when it dies), else (nothing built) they are dropped. */
static void br_data_capture_end(br_engine *e, uint32_t mark, br_node root)
{
    br_asset_group *owner = e->data->owner_group;
    if (owner == NULL && br_assets_capture_count(e, mark) > 0 && br_node_valid(e, root)) {
        owner = br_data_root_group(e, root);
    }
    br_assets_capture_end(e, mark, owner);
}

br_node br_scene_file_parse_node(br_engine *e, const cJSON *json, br_node parent)
{
    br_data_state *s = e->data;
    bool outermost = s->load_depth == 0;
    uint32_t mark = outermost ? br_assets_capture_begin(e) : 0;
    s->load_depth++;
    br_node n = br_data_build_node(e, json, parent);
    s->load_depth--;
    if (s->load_depth == 0) {
        br_data_resolve_refs(e, n);
    }
    if (outermost) {
        br_data_capture_end(e, mark, n);
    }
    return n;
}

br_node br_data_load_owned(br_engine *e, const char *path, br_node parent, br_asset_group *owner)
{
    br_asset_group *saved = e->data->owner_group;
    e->data->owner_group = owner;
    br_node n = br_scene_file_load(e, path, parent);
    e->data->owner_group = saved;
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
    bool outermost = s->load_depth == 0;
    /* the rebuilt tree takes new references; the old ones go after it (shared assets stay loaded) */
    br_data_stored *st = br_array_at(&s->stored, root.index);
    br_asset_group *old_assets = NULL;
    if (outermost && st != NULL && st->generation == root.generation) {
        old_assets = st->assets;
        st->assets = NULL;
    }
    uint32_t mark = outermost ? br_assets_capture_begin(e) : 0;
    s->load_depth++;
    br_data_store_json(e, root, doc);
    br_data_apply_node(e, root, doc, true);
    s->load_depth--;
    if (s->load_depth == 0) {
        br_data_resolve_refs(e, root);
    }
    if (outermost) {
        br_data_capture_end(e, mark, root);
    }
    if (old_assets != NULL && e->stepping) {
        /* the old children die at the next node flush and may still point at a font until then */
        br_array_push(&s->release, &old_assets);
    } else {
        br_asset_group_destroy(old_assets);
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
    for (uint32_t id = 1; br_component_desc_get(e, id) != NULL; id++) {
        const br_component_desc *cd = br_component_desc_get(e, id);
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

static bool br_data_is_asset_key(const char *key, const char *value)
{
    /* "source": a glTF model of a view3d "gltf" node */
    static const char *keys[] = { "texture", "map", "font", "gif", "prefab", "svg_file", "source" };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(keys); i++) {
        if (SDL_strcmp(keys[i], key) == 0) {
            return true;
        }
    }
    /* "animation": "sheet.json#walk" names a spritesheet; a glTF clip name ("Run") is no asset */
    return SDL_strcmp(key, "animation") == 0 && SDL_strchr(value, '#') != NULL;
}

/* Collection state: the callback, and whether prefab files are opened (each once per call, so
 * prefabs used many times or in a cycle cost one read). */
typedef struct br_data_collector {
    br_asset_path_fn fn;
    void            *user;
    bool             expand_prefabs;
    int              depth;
    br_map           visited;  /* prefab path -> bool */
} br_data_collector;

static void br_data_collect(br_engine *e, const cJSON *json, br_data_collector *c);

static void br_data_collect_prefab(br_engine *e, const char *path, br_data_collector *c)
{
    bool yes = true;
    if (c->depth >= BR_PREFAB_MAX_DEPTH || br_map_get(&c->visited, path) != NULL) {
        return; /* seen already, or a chain too deep (the build reports cycles and depth) */
    }
    br_map_set(&c->visited, path, &yes);
    void *bytes = NULL;
    size_t size = 0;
    if (!br_file_read_all(e, path, &bytes, &size)) {
        return; /* the load reports the missing prefab */
    }
    cJSON *doc = cJSON_ParseWithLength(bytes, size);
    br_free(bytes);
    if (doc != NULL) {
        c->depth++;
        br_data_collect(e, doc, c);
        c->depth--;
        cJSON_Delete(doc);
    }
}

/* One "assets" list entry: paths and bundle names, preload or lazy, each reported once. */
static void br_data_collect_entry(br_engine *e, const char *path, bool lazy, void *user)
{
    (void)e;
    (void)lazy;
    br_data_collector *c = user;
    c->fn(path, c->user);
}

static void br_data_collect_report(br_engine *e, const char *key, const char *path, br_data_collector *c)
{
    if (c->expand_prefabs && SDL_strcmp(key, "prefab") == 0) {
        br_data_collect_prefab(e, path, c);
        return;
    }
    br_data_report_path(path, c->fn, c->user);
}

static void br_data_collect(br_engine *e, const cJSON *json, br_data_collector *c)
{
    br_asset_path_fn fn = c->fn;
    void *user = c->user;
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
        if (key != NULL && cJSON_IsString(item) && br_data_is_asset_key(key, item->valuestring)) {
            br_data_collect_report(e, key, item->valuestring, c);
        } else if (key != NULL && cJSON_IsArray(item) && SDL_strcmp(key, "assets") == 0) {
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, item)
            {
                if (cJSON_IsString(entry) || cJSON_IsObject(entry)) {
                    br_data_assets_entry(e, entry, br_data_collect_entry, c);
                }
            }
        } else if (key != NULL && cJSON_IsArray(item) && SDL_strcmp(key, "frames") == 0) {
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, item)
            {
                if (cJSON_IsString(entry)) {
                    br_data_report_path(entry->valuestring, fn, user);
                } else {
                    br_data_collect(e, entry, c);
                }
            }
        } else if (key != NULL && cJSON_IsObject(item) && SDL_strcmp(key, "components") == 0) {
            const cJSON *component = NULL;
            cJSON_ArrayForEach(component, item)
            {
                const br_component_desc *desc = br_component_desc_get(e, br_component_find(e, component->string));
                for (uint32_t f = 0; desc != NULL && f < desc->field_count; f++) {
                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(component, desc->fields[f].name);
                    if (desc->fields[f].type == BR_FIELD_ASSET && cJSON_IsString(v)) {
                        fn(v->valuestring, user);
                    }
                }
            }
        } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
            br_data_collect(e, item, c);
        }
    }
}

void br_scene_file_collect_assets(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user)
{
    if (json != NULL && fn != NULL) {
        br_data_collector c;
        SDL_zero(c);
        c.fn = fn;
        c.user = user;
        br_map_init(&c.visited, sizeof(bool));
        br_data_collect(e, json, &c);
        br_map_free(&c.visited);
    }
}

void br_data_collect_expanded(br_engine *e, const cJSON *json, br_asset_path_fn fn, void *user)
{
    if (json != NULL && fn != NULL) {
        br_data_collector c;
        SDL_zero(c);
        c.fn = fn;
        c.user = user;
        c.expand_prefabs = true;
        br_map_init(&c.visited, sizeof(bool));
        br_data_collect(e, json, &c);
        br_map_free(&c.visited);
    }
}

/* ---- named handlers */

void br_scene_file_add_handler(br_engine *e, const char *name, br_event_listener_fn fn, void *user)
{
    br_data_handler h = { fn, user };
    br_map_set(&e->data->handlers, name, &h);
}
