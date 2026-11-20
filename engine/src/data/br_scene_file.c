/* br_scene_file: build node trees from scene JSON, component fields, label references, save,
 * asset collection, named event handlers. */
#include "data/br_data_internal.h"
#include "scene/br_scene_internal.h" /* br_field_extra */

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
        if (ref->kind == BR_DATA_REF_FIELD3D) {
            br_node3d target3d = br_data_find_node3d(e, root, ref->label);
            uint8_t *data = br_component_get(e, ref->node, ref->component_id);
            if (!br_node3d_valid(e, target3d)) {
                br_str_copy(s->path, sizeof(s->path), ref->path);
                s->path_len = SDL_strlen(s->path);
                br_data_errorf(e, "no 3D node labelled '%s'", ref->label);
            } else if (data != NULL) {
                SDL_memcpy(data + ref->offset, &target3d, sizeof(target3d));
            }
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

/* n numbers from a JSON array of exactly n (errors with `expected`). */
static bool br_data_value_floats(br_engine *e, const cJSON *v, float *out, int n, const char *expected)
{
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != n) {
        br_data_errorf(e, "expected %s", expected);
        return false;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(v, i);
        if (!cJSON_IsNumber(item)) {
            br_data_errorf(e, "expected %s", expected);
            return false;
        }
        if (!br_data_value_float(e, item, &out[i])) {
            return false;
        }
    }
    return true;
}

/* An enum value: one of the field's names, or its index (any int32 when it has no names: an
 * enum in a struct exposed to play-tests). */
static bool br_data_value_enum(br_engine *e, const br_field_extra *x, const cJSON *v, int32_t *out)
{
    uint32_t count = x != NULL ? x->enum_count : 0;
    if (cJSON_IsString(v)) {
        for (uint32_t i = 0; i < count; i++) {
            if (SDL_strcmp(x->enum_names[i], v->valuestring) == 0) {
                *out = (int32_t)i;
                return true;
            }
        }
    } else if (cJSON_IsNumber(v)) {
        int i = 0;
        if (!br_data_value_int(e, v, count > 0 ? 0 : INT32_MIN, count > 0 ? (int)count - 1 : INT32_MAX, &i)) {
            return false;
        }
        *out = (int32_t)i;
        return true;
    }
    char names[256] = "";
    for (uint32_t i = 0; i < count; i++) {
        SDL_strlcat(names, i > 0 ? ", " : "", sizeof(names));
        SDL_strlcat(names, x->enum_names[i], sizeof(names));
    }
    br_data_errorf(e, count > 0 ? "expected one of: %s" : "expected a whole number (enum without names)%s", names);
    return false;
}

/* br_data_read_field with the field's extra description (NULL if it has none). */
static bool br_data_read_value(br_engine *e, const br_field_desc *field, const br_field_extra *x, uint8_t *base,
                               const cJSON *value, br_data_node_field_fn node_fn, void *user)
{
    void *p = base + field->offset;
    float f[4];
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
    case BR_FIELD_FLOAT:
        if (!br_data_value_float(e, value, &f[0])) {
            return false;
        }
        SDL_memcpy(p, &f[0], sizeof(float));
        return true;
    case BR_FIELD_VEC2: {
        br_vec2 v;
        if (!br_data_value_vec2(e, value, &v)) {
            return false;
        }
        SDL_memcpy(p, &v, sizeof(v));
        return true;
    }
    case BR_FIELD_VEC3: {
        if (!br_data_value_floats(e, value, f, 3, "[x, y, z]")) {
            return false;
        }
        br_vec3 v = br_vec3_make(f[0], f[1], f[2]);
        SDL_memcpy(p, &v, sizeof(v));
        return true;
    }
    case BR_FIELD_VEC4:
    case BR_FIELD_QUAT:
        if (!br_data_value_floats(e, value, f, 4, "[x, y, z, w]")) {
            return false;
        }
        SDL_memcpy(p, f, sizeof(float) * 4); /* br_vec4 and br_quat are both x y z w */
        return true;
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
            br_data_errorf(e, "string longer than %d bytes (BR_FIELD_TEXT holds any length)", BR_FIELD_STRING_SIZE - 1);
            return false;
        }
        return true;
    case BR_FIELD_TEXT: {
        if (!cJSON_IsString(value) && !cJSON_IsNull(value)) {
            br_data_errorf(e, "expected a string or null");
            return false;
        }
        char *text = NULL;
        SDL_memcpy(&text, p, sizeof(text));
        bool ok = br_component_set_text(&text, cJSON_IsString(value) ? value->valuestring : NULL);
        SDL_memcpy(p, &text, sizeof(text));
        if (!ok) {
            br_data_errorf(e, "out of memory");
        }
        return ok;
    }
    case BR_FIELD_ENUM: {
        int32_t i = 0;
        if (!br_data_value_enum(e, x, value, &i)) {
            return false;
        }
        SDL_memcpy(p, &i, sizeof(i));
        return true;
    }
    case BR_FIELD_NODE:
    case BR_FIELD_NODE3D:
        if (cJSON_IsNull(value)) {
            br_node zero = br_data_zero_node(); /* br_node3d has the same layout */
            SDL_memcpy(p, &zero, sizeof(zero));
            return true;
        }
        if (!cJSON_IsString(value)) {
            br_data_errorf(e, "expected a node label (string) or null");
            return false;
        }
        return node_fn(e, field, base, value->valuestring, user);
    case BR_FIELD_ARRAY: {
        if (x == NULL || x->capacity == 0) {
            br_data_errorf(e, "array field '%s' has no element type (br_component_set_array)", field->name);
            return false;
        }
        int n = cJSON_IsArray(value) ? cJSON_GetArraySize(value) : -1;
        if (n < 0) {
            br_data_errorf(e, "expected an array");
            return false;
        }
        if ((uint32_t)n > x->capacity) {
            br_data_errorf(e, "%d elements, the field holds at most %u", n, (unsigned)x->capacity);
            return false;
        }
        br_field_desc element = *field;
        element.type = x->element_type;
        size_t size = br_field_type_size(x->element_type);
        bool ok = true;
        for (int i = 0; i < n; i++) {
            element.offset = field->offset + (size_t)i * size;
            size_t mark = br_data_path_push_index(e, i);
            ok = br_data_read_value(e, &element, x, base, cJSON_GetArrayItem(value, i), node_fn, user) && ok;
            br_data_path_pop(e, mark);
        }
        uint32_t count = (uint32_t)n;
        SDL_memcpy(base + x->count_offset, &count, sizeof(count));
        return ok;
    }
    }
    br_data_errorf(e, "unknown field type %d", (int)field->type);
    return false;
}

bool br_data_read_field(br_engine *e, const br_field_desc *field, uint8_t *base, const cJSON *value,
                        br_data_node_field_fn node_fn, void *user)
{
    return br_data_read_value(e, field, br_component_field_extra(e, field), base, value, node_fn, user);
}

/* The 3D node labelled `label` in a view3d at or below `root` (depth first), zero if none. */
static br_node3d br_data_find_node3d_in(br_engine *e, br_node root, const char *label)
{
    if (br_node_type_of(e, root) == br_view3d_type(e)) {
        br_node3d found = br_node3d_find_by_label(e, br_view3d_root(e, root), label, true);
        if (br_node3d_valid(e, found)) {
            return found;
        }
    }
    for (uint32_t i = 0; i < br_node_child_count(e, root); i++) {
        br_node3d found = br_data_find_node3d_in(e, br_node_child_at(e, root, i), label);
        if (br_node3d_valid(e, found)) {
            return found;
        }
    }
    br_node3d none = { 0, 0 };
    return none;
}

br_node3d br_data_find_node3d(br_engine *e, br_node root, const char *label)
{
    br_node3d found = { 0, 0 };
    if (br_node_valid(e, root)) {
        found = br_data_find_node3d_in(e, root, label);
    }
    return br_node3d_valid(e, found) ? found : br_data_find_node3d_in(e, br_stage_root(e), label);
}

/* Node fields of 2D components: labels resolve at the end of a file load (targets may come later
 * in the file), or right away outside a load. `user` is a br_data_ref with node and component. */
bool br_data_label_node_field(br_engine *e, const br_field_desc *field, uint8_t *base, const char *label,
                                     void *user)
{
    if (e->data->load_depth > 0) {
        br_data_ref ref = *(const br_data_ref *)user;
        ref.kind = field->type == BR_FIELD_NODE3D ? BR_DATA_REF_FIELD3D : BR_DATA_REF_FIELD;
        ref.offset = field->offset;
        br_str_copy(ref.label, sizeof(ref.label), label);
        br_str_copy(ref.path, sizeof(ref.path), e->data->path);
        br_data_add_ref(e, &ref);
        return true;
    }
    if (field->type == BR_FIELD_NODE3D) {
        br_node3d target = br_data_find_node3d(e, br_stage_root(e), label);
        if (!br_node3d_valid(e, target)) {
            br_data_errorf(e, "no 3D node labelled '%s'", label);
            return false;
        }
        SDL_memcpy(base + field->offset, &target, sizeof(target));
        return true;
    }
    br_node target = br_node_find_by_label(e, br_stage_root(e), label, true);
    if (!br_node_valid(e, target)) {
        br_data_errorf(e, "no node labelled '%s'", label);
        return false;
    }
    SDL_memcpy(base + field->offset, &target, sizeof(target));
    return true;
}

bool br_scene_file_read_field(br_engine *e, br_node owner, uint32_t component_id, const br_field_desc *field,
                              const cJSON *value)
{
    uint8_t *base = br_component_get(e, owner, component_id);
    if (base == NULL || field == NULL || value == NULL) {
        return false;
    }
    br_data_ref ref;
    SDL_zero(ref);
    ref.node = owner;
    ref.component_id = component_id;
    return br_data_read_field(e, field, base, value, br_data_label_node_field, &ref);
}

static cJSON *br_data_write_value(br_engine *e, const br_field_desc *field, const br_field_extra *x,
                                  const void *component)
{
    const uint8_t *p = (const uint8_t *)component + field->offset;
    float f[4];
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
    case BR_FIELD_FLOAT:
        SDL_memcpy(f, p, sizeof(float));
        return br_data_number(f[0]);
    case BR_FIELD_VEC2: {
        br_vec2 v;
        SDL_memcpy(&v, p, sizeof(v));
        return br_data_vec2_json(v);
    }
    case BR_FIELD_VEC3:
    case BR_FIELD_VEC4:
    case BR_FIELD_QUAT: {
        int n = field->type == BR_FIELD_VEC3 ? 3 : 4;
        SDL_memcpy(f, p, sizeof(float) * (size_t)n);
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(a, br_data_number(f[i]));
        }
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
    case BR_FIELD_TEXT: {
        const char *text = NULL;
        SDL_memcpy(&text, p, sizeof(text));
        return cJSON_CreateString(text != NULL ? text : "");
    }
    case BR_FIELD_ENUM: {
        int32_t i;
        SDL_memcpy(&i, p, sizeof(i));
        if (x != NULL && i >= 0 && (uint32_t)i < x->enum_count) {
            return cJSON_CreateString(x->enum_names[i]);
        }
        return cJSON_CreateNumber(i); /* set from code to a value without a name */
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
    case BR_FIELD_NODE3D: {
        br_node3d n;
        SDL_memcpy(&n, p, sizeof(n));
        if (!br_node3d_valid(e, n) || br_node3d_label(e, n)[0] == '\0') {
            return cJSON_CreateNull();
        }
        return cJSON_CreateString(br_node3d_label(e, n));
    }
    case BR_FIELD_ARRAY: {
        if (x == NULL || x->capacity == 0) {
            return cJSON_CreateArray(); /* not described: nothing can be read either */
        }
        uint32_t count;
        SDL_memcpy(&count, (const uint8_t *)component + x->count_offset, sizeof(count));
        count = count < x->capacity ? count : x->capacity; /* the count is game memory */
        br_field_desc element = *field;
        element.type = x->element_type;
        size_t size = br_field_type_size(x->element_type);
        cJSON *a = cJSON_CreateArray();
        for (uint32_t i = 0; i < count; i++) {
            element.offset = field->offset + (size_t)i * size;
            cJSON_AddItemToArray(a, br_data_write_value(e, &element, x, component));
        }
        return a;
    }
    }
    return NULL;
}

cJSON *br_scene_file_write_field(br_engine *e, const br_field_desc *field, const void *component)
{
    return br_data_write_value(e, field, br_component_field_extra(e, field), component);
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
    { "text", " text text_key font size style anchor resolution " },
    { "bitmap_text", " text text_key font size style anchor resolution " },
    { "rich_text", " text text_key font size style anchor resolution tag_styles " },
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
                    const br_field_extra *x = br_component_field_extra(e, &desc->fields[f]);
                    if (desc->fields[f].type == BR_FIELD_ARRAY && x != NULL && x->element_type == BR_FIELD_ASSET) {
                        const cJSON *path = NULL;
                        cJSON_ArrayForEach(path, v)
                        {
                            if (cJSON_IsString(path)) {
                                fn(path->valuestring, user);
                            }
                        }
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
