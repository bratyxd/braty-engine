/* br_playtest queries: "label:property" / "label:component.field" / "name:field" paths, get and
 * set, the dump, node lists, expect / wait_until conditions. */
#include "playtest/br_playtest_internal.h"

#include "data/br_data_internal.h"

/* A parsed path: "ship:pilot.intent.x" -> target "ship", parts "pilot", "intent", "x". */
#define BR_PT_MAX_PARTS 4

typedef struct br_pt_path {
    char     target[BR_PLAYTEST_NAME_SIZE];
    char     parts[BR_PT_MAX_PARTS][BR_PLAYTEST_NAME_SIZE];
    uint32_t part_count;
} br_pt_path;

static bool br_pt_parse_path(br_engine *e, const char *text, br_pt_path *out)
{
    SDL_zerop(out);
    const char *colon = text != NULL ? SDL_strchr(text, ':') : NULL;
    if (colon == NULL || colon == text || colon[1] == '\0') {
        return br_playtest_fail(e, "path '%s': expected \"label:property\" or \"label:component.field\"",
                                text != NULL ? text : "");
    }
    size_t target_len = (size_t)(colon - text);
    if (target_len >= sizeof(out->target)) {
        return br_playtest_fail(e, "path '%s': label too long", text);
    }
    SDL_memcpy(out->target, text, target_len);
    const char *p = colon + 1;
    while (*p != '\0') {
        if (out->part_count == BR_PT_MAX_PARTS) {
            return br_playtest_fail(e, "path '%s': too many '.' parts", text);
        }
        const char *dot = SDL_strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : SDL_strlen(p);
        if (len == 0 || len >= BR_PLAYTEST_NAME_SIZE) {
            return br_playtest_fail(e, "path '%s': empty or too long part", text);
        }
        SDL_memcpy(out->parts[out->part_count], p, len);
        out->part_count++;
        p += len;
        if (*p == '.') {
            p++;
        }
    }
    return true;
}

/* ".x" ".y" ".z" ".w" or a number -> array index, -1 if not an element name. */
static int br_pt_element_index(const char *name)
{
    if (SDL_strcmp(name, "x") == 0 || SDL_strcmp(name, "r") == 0) {
        return 0;
    }
    if (SDL_strcmp(name, "y") == 0 || SDL_strcmp(name, "g") == 0) {
        return 1;
    }
    if (SDL_strcmp(name, "z") == 0 || SDL_strcmp(name, "b") == 0) {
        return 2;
    }
    if (SDL_strcmp(name, "w") == 0 || SDL_strcmp(name, "a") == 0) {
        return 3;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        return SDL_atoi(name);
    }
    return -1;
}

/* Applies an optional element part to a value (takes ownership). NULL + error if it does not fit. */
static cJSON *br_pt_pick_element(br_engine *e, cJSON *value, const char *element, const char *path)
{
    if (value == NULL || element == NULL) {
        return value;
    }
    int index = br_pt_element_index(element);
    cJSON *item = cJSON_IsArray(value) && index >= 0 ? cJSON_GetArrayItem(value, index) : NULL;
    if (item == NULL) {
        cJSON_Delete(value);
        br_playtest_fail(e, "path '%s': no element '%s' (elements: x y z w, or 0 1 2...)", path, element);
        return NULL;
    }
    cJSON *copy = cJSON_Duplicate(item, true);
    cJSON_Delete(value);
    return copy;
}

/* ---- fields (components and exposed structs share the br_field_desc layout) */

static const br_field_desc *br_pt_find_field(const br_field_desc *fields, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (SDL_strcmp(fields[i].name, name) == 0) {
            return &fields[i];
        }
    }
    return NULL;
}

static bool br_pt_numbers(const cJSON *value, float *out, int count)
{
    if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != count) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(value, i);
        if (!cJSON_IsNumber(item)) {
            return false;
        }
        out[i] = (float)item->valuedouble;
    }
    return true;
}

/* Writes a JSON value into one field of a struct at `base`. */
static bool br_pt_write_field(br_engine *e, const br_field_desc *field, void *base, const cJSON *value, const char *path)
{
    uint8_t *p = (uint8_t *)base + field->offset;
    float f[4];
    switch (field->type) {
    case BR_FIELD_BOOL:
        if (cJSON_IsBool(value)) {
            bool b = cJSON_IsTrue(value);
            SDL_memcpy(p, &b, sizeof(b));
            return true;
        }
        break;
    case BR_FIELD_INT:
        if (cJSON_IsNumber(value)) {
            int32_t i = (int32_t)value->valuedouble;
            SDL_memcpy(p, &i, sizeof(i));
            return true;
        }
        break;
    case BR_FIELD_FLOAT:
        if (cJSON_IsNumber(value)) {
            float x = (float)value->valuedouble;
            SDL_memcpy(p, &x, sizeof(x));
            return true;
        }
        break;
    case BR_FIELD_VEC2:
        if (br_pt_numbers(value, f, 2)) {
            br_vec2 v = br_vec2_make(f[0], f[1]);
            SDL_memcpy(p, &v, sizeof(v));
            return true;
        }
        break;
    case BR_FIELD_VEC3:
        if (br_pt_numbers(value, f, 3)) {
            br_vec3 v = br_vec3_make(f[0], f[1], f[2]);
            SDL_memcpy(p, &v, sizeof(v));
            return true;
        }
        break;
    case BR_FIELD_COLOR: {
        br_color c;
        if (cJSON_IsString(value) && br_color_parse(value->valuestring, &c)) {
            SDL_memcpy(p, &c, sizeof(c));
            return true;
        }
        if (br_pt_numbers(value, f, 4)) {
            c = br_color_make(f[0], f[1], f[2], f[3]);
            SDL_memcpy(p, &c, sizeof(c));
            return true;
        }
        break;
    }
    case BR_FIELD_STRING:
    case BR_FIELD_ASSET:
        if (cJSON_IsString(value) && SDL_strlen(value->valuestring) < BR_FIELD_STRING_SIZE) {
            br_str_copy((char *)p, BR_FIELD_STRING_SIZE, value->valuestring);
            return true;
        }
        break;
    case BR_FIELD_NODE: {
        br_node n = { 0, 0 };
        if (cJSON_IsString(value)) {
            n = br_node_find_by_label(e, br_scene_root(e), value->valuestring, true);
        }
        if (cJSON_IsNull(value) || br_node_valid(e, n)) {
            SDL_memcpy(p, &n, sizeof(n));
            return true;
        }
        break;
    }
    }
    return br_playtest_fail(e, "path '%s': value does not fit field '%s' (bool, number, [x,y], [x,y,z], "
                               "\"#rrggbb\", string or node label)",
                            path, field->name);
}

static br_playtest_exposed *br_pt_find_exposed(br_engine *e, const char *name)
{
    br_playtest_state *s = e->playtest;
    for (uint32_t i = 0; i < s->exposed.count; i++) {
        br_playtest_exposed *x = br_array_at(&s->exposed, i);
        if (SDL_strcmp(x->name, name) == 0) {
            return x;
        }
    }
    return NULL;
}

/* "name:field" on an exposed struct, or "label:component.field": the field and its struct. */
static bool br_pt_resolve_field(br_engine *e, const br_pt_path *p, const char *path, const br_field_desc **field,
                                void **base, uint32_t *next_part, bool *is_field)
{
    *is_field = false;
    br_playtest_exposed *x = br_pt_find_exposed(e, p->target);
    if (x != NULL) {
        *field = br_pt_find_field(x->fields, x->field_count, p->parts[0]);
        if (*field == NULL) {
            return br_playtest_fail(e, "path '%s': '%s' has no field '%s'", path, x->name, p->parts[0]);
        }
        *base = x->base;
        *next_part = 1;
        *is_field = true;
        return true;
    }
    uint32_t id = br_component_find(e, p->parts[0]);
    if (id == 0) {
        return true; /* not a component: a node property */
    }
    br_node n = br_node_find_by_label(e, br_scene_root(e), p->target, true);
    *base = br_component_get(e, n, id);
    if (!br_node_valid(e, n)) {
        return br_playtest_fail(e, "path '%s': no node labelled '%s'", path, p->target);
    }
    if (*base == NULL) {
        return br_playtest_fail(e, "path '%s': node '%s' has no component '%s'", path, p->target, p->parts[0]);
    }
    const br_component_desc *desc = br_component_get_desc(e, id);
    if (p->part_count < 2) {
        return br_playtest_fail(e, "path '%s': name a field of '%s'", path, p->parts[0]);
    }
    *field = br_pt_find_field(desc->fields, desc->field_count, p->parts[1]);
    if (*field == NULL) {
        return br_playtest_fail(e, "path '%s': component '%s' has no field '%s'", path, p->parts[0], p->parts[1]);
    }
    *next_part = 2;
    *is_field = true;
    return true;
}

/* ---- node properties */

static const char *br_pt_type_name(br_engine *e, br_node n)
{
    uint32_t type_id = br_node_type_of(e, n);
    const br_data_type *t = br_data_type_of(e, type_id);
    if (t != NULL) {
        return t->name;
    }
    const br_node_type_desc *desc = br_node_type_get(e, type_id);
    return desc != NULL ? desc->name : "?";
}

/* A built-in property or a key of the node's scene-file form; NULL + error if unknown. */
static cJSON *br_pt_node_property(br_engine *e, br_node n, const char *name, const char *path)
{
    if (SDL_strcmp(name, "position") == 0) {
        return br_data_vec2_json(br_node_position(e, n));
    }
    if (SDL_strcmp(name, "x") == 0) {
        return br_data_number(br_node_position(e, n).x);
    }
    if (SDL_strcmp(name, "y") == 0) {
        return br_data_number(br_node_position(e, n).y);
    }
    if (SDL_strcmp(name, "scale") == 0) {
        return br_data_vec2_json(br_node_scale(e, n));
    }
    if (SDL_strcmp(name, "rotation") == 0) {
        return br_data_number(br_node_rotation(e, n));
    }
    if (SDL_strcmp(name, "alpha") == 0) {
        return br_data_number(br_node_alpha(e, n));
    }
    if (SDL_strcmp(name, "visible") == 0) {
        return cJSON_CreateBool(br_node_visible(e, n));
    }
    if (SDL_strcmp(name, "tint") == 0) {
        return br_data_color_json(br_node_tint(e, n));
    }
    if (SDL_strcmp(name, "z_index") == 0) {
        return cJSON_CreateNumber(br_node_z_index(e, n));
    }
    if (SDL_strcmp(name, "label") == 0) {
        return cJSON_CreateString(br_node_label(e, n));
    }
    if (SDL_strcmp(name, "type") == 0) {
        return cJSON_CreateString(br_pt_type_name(e, n));
    }
    if (SDL_strcmp(name, "children") == 0) {
        return cJSON_CreateNumber(br_node_child_count(e, n));
    }
    if (SDL_strcmp(name, "world_position") == 0) {
        return br_data_vec2_json(br_node_to_global(e, n, br_vec2_make(0.0f, 0.0f)));
    }
    if (SDL_strcmp(name, "bounds") == 0) {
        br_rect r = br_node_get_bounds(e, n);
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, br_data_number(r.x));
        cJSON_AddItemToArray(a, br_data_number(r.y));
        cJSON_AddItemToArray(a, br_data_number(r.width));
        cJSON_AddItemToArray(a, br_data_number(r.height));
        return a;
    }
    /* anything else the scene file form has: "text", "texture", "style", ... */
    // ponytail: builds the node's whole JSON (subtree included) for one key; fine for queries, not for per-frame use.
    cJSON *json = br_scene_file_to_json(e, n);
    cJSON *item = cJSON_DetachItemFromObjectCaseSensitive(json, name);
    cJSON_Delete(json);
    if (item == NULL) {
        br_playtest_fail(e, "path '%s': unknown property '%s' (built-ins: position x y scale rotation alpha visible "
                            "tint z_index label type children world_position bounds exists; or a component name)",
                         path, name);
    }
    return item;
}

cJSON *br_playtest_query_get(br_engine *e, const char *path)
{
    br_pt_path p;
    if (!br_pt_parse_path(e, path, &p)) {
        return NULL;
    }
    const br_field_desc *field = NULL;
    void *base = NULL;
    uint32_t next = 0;
    bool is_field = false;
    if (!br_pt_resolve_field(e, &p, path, &field, &base, &next, &is_field)) {
        return NULL;
    }
    cJSON *value = NULL;
    if (is_field) {
        value = br_scene_file_write_field(e, field, base);
    } else {
        br_node n = br_node_find_by_label(e, br_scene_root(e), p.target, true);
        if (SDL_strcmp(p.parts[0], "exists") == 0) {
            return cJSON_CreateBool(br_node_valid(e, n));
        }
        if (!br_node_valid(e, n)) {
            br_playtest_fail(e, "path '%s': no node labelled '%s'", path, p.target);
            return NULL;
        }
        value = br_pt_node_property(e, n, p.parts[0], path);
        next = 1;
    }
    if (next < p.part_count) {
        if (next + 1 < p.part_count) {
            cJSON_Delete(value);
            br_playtest_fail(e, "path '%s': too many parts", path);
            return NULL;
        }
        value = br_pt_pick_element(e, value, p.parts[next], path);
    }
    return value;
}

/* Replaces one element of an array value: "ship:position.x" = 10 -> [10, y]. */
static cJSON *br_pt_with_element(br_engine *e, const char *path, const char *element, const cJSON *value)
{
    char whole[256];
    SDL_snprintf(whole, sizeof(whole), "%s", path);
    char *last_dot = SDL_strrchr(whole, '.');
    if (last_dot != NULL) {
        *last_dot = '\0';
    }
    cJSON *current = br_playtest_query_get(e, whole);
    int index = br_pt_element_index(element);
    if (current == NULL || !cJSON_IsArray(current) || index < 0 || index >= cJSON_GetArraySize(current)) {
        cJSON_Delete(current);
        br_playtest_fail(e, "path '%s': cannot set element '%s'", path, element);
        return NULL;
    }
    cJSON_ReplaceItemInArray(current, index, cJSON_Duplicate(value, true));
    return current;
}

bool br_playtest_query_set(br_engine *e, const char *path, const cJSON *value)
{
    br_pt_path p;
    if (value == NULL) {
        return br_playtest_fail(e, "set '%s': missing value", path != NULL ? path : "");
    }
    if (!br_pt_parse_path(e, path, &p)) {
        return false;
    }
    const br_field_desc *field = NULL;
    void *base = NULL;
    uint32_t next = 0;
    bool is_field = false;
    if (!br_pt_resolve_field(e, &p, path, &field, &base, &next, &is_field)) {
        return false;
    }
    if (is_field && next < p.part_count) {
        cJSON *whole = br_pt_with_element(e, path, p.parts[next], value);
        bool ok = whole != NULL && br_pt_write_field(e, field, base, whole, path);
        cJSON_Delete(whole);
        return ok;
    }
    if (is_field) {
        return br_pt_write_field(e, field, base, value, path);
    }
    br_node n = br_node_find_by_label(e, br_scene_root(e), p.target, true);
    if (!br_node_valid(e, n)) {
        return br_playtest_fail(e, "path '%s': no node labelled '%s'", path, p.target);
    }
    if (p.part_count > 2) {
        return br_playtest_fail(e, "path '%s': too many parts", path);
    }
    /* only properties that can be read can be written (catches typos the reader would only warn about) */
    cJSON *probe = br_pt_node_property(e, n, p.parts[0], path);
    if (probe == NULL) {
        return false;
    }
    cJSON_Delete(probe);
    /* x / y and "property.element" become the whole vector */
    cJSON *whole = NULL;
    const char *key = p.parts[0];
    if (SDL_strcmp(key, "x") == 0 || SDL_strcmp(key, "y") == 0) {
        whole = br_data_vec2_json(br_node_position(e, n));
        cJSON_ReplaceItemInArray(whole, key[0] == 'x' ? 0 : 1, cJSON_Duplicate(value, true));
        key = "position";
    } else if (p.part_count == 2) {
        whole = br_pt_with_element(e, path, p.parts[1], value);
        if (whole == NULL) {
            return false;
        }
    }
    /* the scene-file reader applies it: every key a scene file knows can be set */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, key, whole != NULL ? whole : cJSON_Duplicate(value, true));
    uint32_t errors = e->data->error_count;
    br_data_apply_node(e, n, obj, false);
    cJSON_Delete(obj);
    if (e->data->error_count != errors) {
        return br_playtest_fail(e, "path '%s': %s", path, e->data->last_error);
    }
    return true;
}

/* ---- dump and list */

cJSON *br_playtest_query_dump(br_engine *e)
{
    cJSON *dump = cJSON_CreateObject();
    cJSON_AddNumberToObject(dump, "frame", (double)e->ticker.frame);
    cJSON_AddNumberToObject(dump, "time", e->ticker.elapsed);
    cJSON_AddNumberToObject(dump, "nodes", br_scene_node_count(e));
    /* exposed game state first: it is what an agent usually wants */
    cJSON *state = cJSON_CreateObject();
    br_playtest_state *s = e->playtest;
    for (uint32_t i = 0; i < s->exposed.count; i++) {
        br_playtest_exposed *x = br_array_at(&s->exposed, i);
        cJSON *fields = cJSON_CreateObject();
        for (uint32_t f = 0; f < x->field_count; f++) {
            cJSON_AddItemToObject(fields, x->fields[f].name, br_scene_file_write_field(e, &x->fields[f], x->base));
        }
        cJSON_AddItemToObject(state, x->name, fields);
    }
    cJSON_AddItemToObject(dump, "state", state);
    cJSON *scene = br_scene_file_to_json(e, br_scene_root(e));
    cJSON_AddItemToObject(dump, "scene", scene != NULL ? scene : cJSON_CreateNull());
    return dump;
}

static void br_pt_list_add(br_engine *e, cJSON *out, br_node n)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "label", br_node_label(e, n));
    cJSON_AddStringToObject(item, "type", br_pt_type_name(e, n));
    cJSON_AddItemToObject(item, "position", br_data_vec2_json(br_node_position(e, n)));
    cJSON_AddItemToArray(out, item);
}

static void br_pt_list_walk(br_engine *e, br_node n, const char *type, cJSON *out)
{
    if (type == NULL || SDL_strcmp(br_pt_type_name(e, n), type) == 0) {
        br_pt_list_add(e, out, n);
    }
    uint32_t count = br_node_child_count(e, n);
    for (uint32_t i = 0; i < count; i++) {
        br_pt_list_walk(e, br_node_child_at(e, n, i), type, out);
    }
}

cJSON *br_playtest_query_list(br_engine *e, const char *type, const char *component)
{
    cJSON *out = cJSON_CreateArray();
    if (component != NULL) {
        uint32_t id = br_component_find(e, component);
        for (uint32_t i = 0; id != 0 && i < br_component_count(e, id); i++) {
            br_node n = br_component_owner_at(e, id, i);
            if (br_node_valid(e, n) && (type == NULL || SDL_strcmp(br_pt_type_name(e, n), type) == 0)) {
                br_pt_list_add(e, out, n);
            }
        }
        return out;
    }
    br_pt_list_walk(e, br_scene_root(e), type, out);
    return out;
}

/* ---- conditions */

static bool br_pt_compare(br_engine *e, const cJSON *a, const char *op, const cJSON *b, bool *ok)
{
    bool equal_op = SDL_strcmp(op, "==") == 0 || SDL_strcmp(op, "!=") == 0;
    if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) {
        double x = a->valuedouble;
        double y = b->valuedouble;
        bool eq = SDL_fabs(x - y) <= 1e-4 * (1.0 + SDL_fabs(y)); /* floats from JSON round trips */
        if (SDL_strcmp(op, "==") == 0) {
            *ok = eq;
        } else if (SDL_strcmp(op, "!=") == 0) {
            *ok = !eq;
        } else if (SDL_strcmp(op, "<") == 0) {
            *ok = x < y;
        } else if (SDL_strcmp(op, "<=") == 0) {
            *ok = x <= y || eq;
        } else if (SDL_strcmp(op, ">") == 0) {
            *ok = x > y;
        } else if (SDL_strcmp(op, ">=") == 0) {
            *ok = x >= y || eq;
        } else {
            return br_playtest_fail(e, "unknown op '%s' (== != < <= > >= contains)", op);
        }
        return true;
    }
    if (SDL_strcmp(op, "contains") == 0 && cJSON_IsString(a) && cJSON_IsString(b)) {
        *ok = SDL_strstr(a->valuestring, b->valuestring) != NULL;
        return true;
    }
    if (equal_op) {
        bool eq = cJSON_Compare(a, b, true);
        if (!eq && cJSON_IsArray(a) && cJSON_IsArray(b) && cJSON_GetArraySize(a) == cJSON_GetArraySize(b)) {
            /* number arrays compare element by element with the same tolerance */
            eq = true;
            for (int i = 0; eq && i < cJSON_GetArraySize(a); i++) {
                bool same = false;
                if (!br_pt_compare(e, cJSON_GetArrayItem(a, i), "==", cJSON_GetArrayItem(b, i), &same)) {
                    return false;
                }
                eq = same;
            }
        }
        *ok = op[0] == '=' ? eq : !eq;
        return true;
    }
    return br_playtest_fail(e, "op '%s' needs numbers (or two strings for contains)", op);
}

bool br_playtest_query_condition(br_engine *e, const cJSON *cmd, bool *ok, char *detail, size_t detail_size)
{
    const cJSON *log = cJSON_GetObjectItemCaseSensitive(cmd, "log");
    if (cJSON_IsString(log)) {
        br_playtest_hook_log(e);
        br_playtest_state *s = e->playtest;
        uint32_t have = s->log_count < BR_PLAYTEST_LOG_LINES ? s->log_count : BR_PLAYTEST_LOG_LINES;
        *ok = false;
        for (uint32_t i = s->log_count - have; i < s->log_count && !*ok; i++) {
            *ok = SDL_strstr(s->log_lines[i % BR_PLAYTEST_LOG_LINES], log->valuestring) != NULL;
        }
        SDL_snprintf(detail, detail_size, "no log line contains '%s' (last %u lines kept)", log->valuestring, have);
        return true;
    }
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(cmd, "path");
    const cJSON *want = cJSON_GetObjectItemCaseSensitive(cmd, "value");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(cmd, "op");
    if (!cJSON_IsString(path) || want == NULL) {
        return br_playtest_fail(e, "a condition needs \"path\" and \"value\" (optional \"op\"), or \"log\"");
    }
    cJSON *have = br_playtest_query_get(e, path->valuestring);
    if (have == NULL) {
        return false;
    }
    const char *op_text = cJSON_IsString(op) ? op->valuestring : "==";
    bool evaluated = br_pt_compare(e, have, op_text, want, ok);
    char *have_text = cJSON_PrintUnformatted(have);
    char *want_text = cJSON_PrintUnformatted(want);
    SDL_snprintf(detail, detail_size, "%s is %s, expected %s %s", path->valuestring, have_text, op_text, want_text);
    cJSON_free(have_text);
    cJSON_free(want_text);
    cJSON_Delete(have);
    return evaluated;
}

bool br_playtest_query_click_point(br_engine *e, const char *label, float *x, float *y)
{
    br_node n = br_node_find_by_label(e, br_scene_root(e), label, true);
    if (!br_node_valid(e, n)) {
        return br_playtest_fail(e, "click: no node labelled '%s'", label);
    }
    br_rect r = br_node_get_bounds(e, n);
    if (r.width <= 0.0f || r.height <= 0.0f) {
        return br_playtest_fail(e, "click: node '%s' has empty bounds (not drawn yet?)", label);
    }
    /* the stage is in window coordinates (no camera yet), so bounds are where the pointer goes */
    *x = r.x + r.width * 0.5f;
    *y = r.y + r.height * 0.5f;
    return true;
}
