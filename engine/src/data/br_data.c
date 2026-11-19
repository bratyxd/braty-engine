/* br_data: module state, error reporting with JSON paths, the JSON type table and the value
 * helpers every from_json callback uses. */
#include "data/br_data_internal.h"

#include <stdarg.h>

/* ---- errors and the JSON path */

void br_data_errorf(br_engine *e, const char *fmt, ...)
{
    br_data_state *s = e->data;
    char message[512];
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    const char *file = s->file[0] != '\0' ? s->file : "<json>";
    if (s->path[0] != '\0') {
        SDL_snprintf(s->last_error, sizeof(s->last_error), "%s: %s: %s", file, s->path, message);
    } else {
        SDL_snprintf(s->last_error, sizeof(s->last_error), "%s: %s", file, message);
    }
    s->error_count++;
    br_log_error("%s", s->last_error);
}

void br_data_warnf(br_engine *e, const char *fmt, ...)
{
    br_data_state *s = e->data;
    char message[512];
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    const char *file = s->file[0] != '\0' ? s->file : "<json>";
    br_log_warn("%s: %s: %s", file, s->path, message);
}

void br_data_error(br_engine *e, const char *key, const char *message)
{
    size_t mark = key != NULL ? br_data_path_push_key(e, key) : e->data->path_len;
    br_data_errorf(e, "%s", message);
    br_data_path_pop(e, mark);
}

static void br_data_path_append(br_data_state *s, const char *text)
{
    size_t len = SDL_strlen(text);
    if (s->path_len + len + 1 > sizeof(s->path)) {
        return; /* too deep to name: keep the prefix */
    }
    SDL_memcpy(s->path + s->path_len, text, len + 1);
    s->path_len += len;
}

size_t br_data_path_push_key(br_engine *e, const char *key)
{
    br_data_state *s = e->data;
    size_t mark = s->path_len;
    if (s->path_len > 0) {
        br_data_path_append(s, ".");
    }
    br_data_path_append(s, key);
    return mark;
}

size_t br_data_path_push_index(br_engine *e, int index)
{
    br_data_state *s = e->data;
    size_t mark = s->path_len;
    char text[24];
    SDL_snprintf(text, sizeof(text), "[%d]", index);
    br_data_path_append(s, text);
    return mark;
}

void br_data_path_pop(br_engine *e, size_t mark)
{
    br_data_state *s = e->data;
    if (mark <= s->path_len) {
        s->path_len = mark;
        s->path[mark] = '\0';
    }
}

void br_data_begin_file(br_engine *e, const char *file, br_data_scope *saved)
{
    br_data_state *s = e->data;
    SDL_memcpy(saved->file, s->file, sizeof(s->file));
    SDL_memcpy(saved->path, s->path, sizeof(s->path));
    saved->path_len = s->path_len;
    br_str_copy(s->file, sizeof(s->file), file != NULL ? file : "");
    br_data_path_pop(e, 0);
}

void br_data_end_file(br_engine *e, const br_data_scope *saved)
{
    br_data_state *s = e->data;
    SDL_memcpy(s->file, saved->file, sizeof(s->file));
    SDL_memcpy(s->path, saved->path, sizeof(s->path));
    s->path_len = saved->path_len;
}

uint32_t br_scene_file_error_count(br_engine *e)
{
    return e->data != NULL ? e->data->error_count : 0;
}

const char *br_scene_file_last_error(br_engine *e)
{
    return e->data != NULL ? e->data->last_error : "";
}

/* ---- JSON parsing */

cJSON *br_data_parse(br_engine *e, const char *source, const char *text, size_t size)
{
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(text, size, &end, false);
    if (json != NULL) {
        return json;
    }
    int line = 1;
    int column = 1;
    for (const char *p = text; end != NULL && p < end && p < text + size; p++) {
        if (*p == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    br_data_scope saved;
    br_data_begin_file(e, source, &saved);
    br_data_errorf(e, "JSON syntax error at line %d, column %d", line, column);
    br_data_end_file(e, &saved);
    return NULL;
}

/* ---- JSON writing */

cJSON *br_data_number(float value)
{
    /* "%.6g" reads better ("0.1" not "0.100000001"); fall back to 9 digits when it would not
     * read back as the same float */
    char text[32];
    SDL_snprintf(text, sizeof(text), "%.6g", (double)value);
    double d = SDL_strtod(text, NULL);
    if ((float)d != value) {
        SDL_snprintf(text, sizeof(text), "%.9g", (double)value);
        d = SDL_strtod(text, NULL);
    }
    return cJSON_CreateNumber(d);
}

void br_data_json_set(cJSON *obj, const char *key, cJSON *value)
{
    if (value == NULL) {
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(obj, key) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    } else {
        cJSON_AddItemToObject(obj, key, value);
    }
}

cJSON *br_data_vec2_json(br_vec2 v)
{
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, br_data_number(v.x));
    cJSON_AddItemToArray(a, br_data_number(v.y));
    return a;
}

cJSON *br_data_color_json(br_color c)
{
    char text[16];
    uint32_t rgb = br_color_to_hex(c);
    if (c.a >= 1.0f) {
        SDL_snprintf(text, sizeof(text), "#%06x", (unsigned)rgb);
    } else {
        unsigned a = (unsigned)(br_clampf(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
        SDL_snprintf(text, sizeof(text), "#%06x%02x", (unsigned)rgb, a);
    }
    return cJSON_CreateString(text);
}

/* ---- the JSON type table */

void br_data_add_type(br_engine *e, const char *name, uint32_t type_id, br_data_create_fn create,
                      br_data_write_fn write)
{
    if (type_id == 0) {
        return; /* the module that owns the type is not there */
    }
    br_data_type t;
    SDL_zero(t);
    br_str_copy(t.name, sizeof(t.name), name);
    t.type_id = type_id;
    t.create = create;
    t.write = write;
    br_array_push(&e->data->types, &t);
}

const br_data_type *br_data_find_type(br_engine *e, const char *name)
{
    br_array *types = &e->data->types;
    for (uint32_t i = 0; i < types->count; i++) {
        br_data_type *t = br_array_at(types, i);
        if (SDL_strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

const br_data_type *br_data_type_of(br_engine *e, uint32_t type_id)
{
    br_array *types = &e->data->types;
    for (uint32_t i = 0; i < types->count; i++) {
        br_data_type *t = br_array_at(types, i);
        if (t->type_id == type_id) {
            return t;
        }
    }
    return NULL;
}

/* ---- value helpers: one value at the current path */

bool br_data_value_float(br_engine *e, const cJSON *v, float *out)
{
    if (!cJSON_IsNumber(v)) {
        br_data_errorf(e, "expected a number");
        return false;
    }
    *out = (float)v->valuedouble;
    return true;
}

bool br_data_value_vec2(br_engine *e, const cJSON *v, br_vec2 *out)
{
    if (cJSON_IsNumber(v)) {
        *out = br_vec2_make((float)v->valuedouble, (float)v->valuedouble);
        return true;
    }
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 2 && cJSON_IsNumber(cJSON_GetArrayItem(v, 0)) &&
        cJSON_IsNumber(cJSON_GetArrayItem(v, 1))) {
        *out = br_vec2_make((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble);
        return true;
    }
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(v, "x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(v, "y");
    if (cJSON_IsObject(v) && cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
        *out = br_vec2_make((float)x->valuedouble, (float)y->valuedouble);
        return true;
    }
    br_data_errorf(e, "expected [x, y]");
    return false;
}

bool br_data_value_color(br_engine *e, const cJSON *v, br_color *out)
{
    if (cJSON_IsString(v)) {
        if (br_color_parse(v->valuestring, out)) {
            return true;
        }
        br_data_errorf(e, "'%s' is not a colour (use \"#rrggbb\", \"#rrggbbaa\" or a CSS name)", v->valuestring);
        return false;
    }
    if (cJSON_IsNumber(v)) {
        *out = br_color_from_hex((uint32_t)v->valuedouble, 1.0f);
        return true;
    }
    int n = cJSON_IsArray(v) ? cJSON_GetArraySize(v) : 0;
    if (n == 3 || n == 4) {
        float c[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (int i = 0; i < n; i++) {
            const cJSON *item = cJSON_GetArrayItem(v, i);
            if (!cJSON_IsNumber(item)) {
                br_data_errorf(e, "expected a colour [r, g, b, a] of numbers 0..1");
                return false;
            }
            c[i] = (float)item->valuedouble;
        }
        *out = br_color_make(c[0], c[1], c[2], c[3]);
        return true;
    }
    br_data_errorf(e, "expected a colour (\"#rrggbb\", a CSS name or [r, g, b, a])");
    return false;
}

/* ---- value helpers: obj[key] */

bool br_data_get_bool(br_engine *e, const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!cJSON_IsBool(v)) {
        br_data_error(e, key, "expected true or false");
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

bool br_data_get_int(br_engine *e, const cJSON *obj, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(int)v->valuedouble) {
        br_data_error(e, key, "expected a whole number");
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

bool br_data_get_float(br_engine *e, const cJSON *obj, const char *key, float *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    size_t mark = br_data_path_push_key(e, key);
    bool ok = br_data_value_float(e, v, out);
    br_data_path_pop(e, mark);
    return ok;
}

bool br_data_get_vec2(br_engine *e, const cJSON *obj, const char *key, br_vec2 *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    size_t mark = br_data_path_push_key(e, key);
    bool ok = br_data_value_vec2(e, v, out);
    br_data_path_pop(e, mark);
    return ok;
}

bool br_data_get_vec3(br_engine *e, const cJSON *obj, const char *key, br_vec3 *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 3 && cJSON_IsNumber(cJSON_GetArrayItem(v, 0)) &&
        cJSON_IsNumber(cJSON_GetArrayItem(v, 1)) && cJSON_IsNumber(cJSON_GetArrayItem(v, 2))) {
        *out = br_vec3_make((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
                            (float)cJSON_GetArrayItem(v, 2)->valuedouble);
        return true;
    }
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(v, "x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(v, "y");
    const cJSON *z = cJSON_GetObjectItemCaseSensitive(v, "z");
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsNumber(z)) {
        *out = br_vec3_make((float)x->valuedouble, (float)y->valuedouble, (float)z->valuedouble);
        return true;
    }
    br_data_error(e, key, "expected [x, y, z]");
    return false;
}

bool br_data_get_color(br_engine *e, const cJSON *obj, const char *key, br_color *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    size_t mark = br_data_path_push_key(e, key);
    bool ok = br_data_value_color(e, v, out);
    br_data_path_pop(e, mark);
    return ok;
}

bool br_data_get_string(br_engine *e, const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!cJSON_IsString(v)) {
        br_data_error(e, key, "expected a string");
        return false;
    }
    if (!br_str_copy(out, out_size, v->valuestring)) {
        size_t mark = br_data_path_push_key(e, key);
        br_data_errorf(e, "string longer than %u bytes", (unsigned)(out_size - 1));
        br_data_path_pop(e, mark);
        return false;
    }
    return true;
}

bool br_data_get_rect(br_engine *e, const cJSON *obj, const char *key, br_rect *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 4) {
        float f[4];
        bool numbers = true;
        for (int i = 0; i < 4; i++) {
            const cJSON *item = cJSON_GetArrayItem(v, i);
            numbers = numbers && cJSON_IsNumber(item);
            f[i] = numbers ? (float)item->valuedouble : 0.0f;
        }
        if (numbers) {
            *out = br_rect_make(f[0], f[1], f[2], f[3]);
            return true;
        }
    }
    br_data_error(e, key, "expected [x, y, width, height]");
    return false;
}

bool br_data_get_enum(br_engine *e, const cJSON *obj, const char *key, const br_data_enum *table, uint32_t count,
                      int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!cJSON_IsString(v)) {
        br_data_error(e, key, "expected a name (string)");
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (SDL_strcmp(table[i].name, v->valuestring) == 0) {
            *out = table[i].value;
            return true;
        }
    }
    size_t mark = br_data_path_push_key(e, key);
    br_data_errorf(e, "unknown value '%s'", v->valuestring);
    br_data_path_pop(e, mark);
    return false;
}

const char *br_data_enum_name(const br_data_enum *table, uint32_t count, int value)
{
    for (uint32_t i = 0; i < count; i++) {
        if (table[i].value == value) {
            return table[i].name;
        }
    }
    return NULL;
}

/* ---- textures by path or spritesheet frame */

static br_spritesheet *br_data_sheet(br_engine *e, const char *path)
{
    const char *type = br_assets_type(e, path);
    if (type != NULL && SDL_strcmp(type, "spritesheet") == 0) {
        return br_assets_get(e, path); /* already loaded: no new reference */
    }
    return br_spritesheet_load(e, path);
}

/* A frame of any loaded spritesheet called `name`; zero handle if none. */
static br_texture br_data_find_frame(br_engine *e, const char *name)
{
    br_texture none = { 0, 0 };
    uint32_t count = br_assets_count(e);
    if (count == 0) {
        return none;
    }
    br_asset_info *list = br_alloc(sizeof(br_asset_info) * count);
    if (list == NULL) {
        return none;
    }
    count = br_assets_list(e, list, count);
    br_texture found = none;
    for (uint32_t i = 0; i < count && found.generation == 0; i++) {
        if (list[i].type != NULL && SDL_strcmp(list[i].type, "spritesheet") == 0) {
            br_spritesheet *sheet = br_assets_get(e, list[i].path);
            if (sheet != NULL) {
                found = br_spritesheet_frame(e, sheet, name);
            }
        }
    }
    br_free(list);
    return found;
}

br_texture br_data_texture(br_engine *e, const char *ref)
{
    // ponytail: loads take asset references that destroying the scene does not drop; Phase 2 Task 16 (asset groups) owns release.
    const char *hash = SDL_strchr(ref, '#');
    if (hash != NULL) {
        char sheet_path[BR_DATA_PATH_SIZE];
        size_t len = (size_t)(hash - ref);
        if (len >= sizeof(sheet_path)) {
            br_data_errorf(e, "texture reference too long");
            return br_texture_missing(e);
        }
        SDL_memcpy(sheet_path, ref, len);
        sheet_path[len] = '\0';
        br_spritesheet *sheet = br_data_sheet(e, sheet_path);
        if (sheet == NULL) {
            br_data_errorf(e, "spritesheet '%s' could not be loaded", sheet_path);
            return br_texture_missing(e);
        }
        br_texture t = br_spritesheet_frame(e, sheet, hash + 1);
        if (t.generation == 0) {
            br_data_errorf(e, "no frame '%s' in spritesheet '%s'", hash + 1, sheet_path);
            return br_texture_missing(e);
        }
        return t;
    }
    br_texture frame = br_data_find_frame(e, ref);
    if (frame.generation != 0) {
        return frame;
    }
    char resolved[BR_DATA_PATH_SIZE];
    bool cached = br_assets_resolve(e, ref, resolved, sizeof(resolved)) && br_assets_get(e, resolved) != NULL;
    if (!cached && !br_file_exists(e, resolved)) {
        br_data_errorf(e, "texture '%s' not found (no such file and no loaded spritesheet frame)", ref);
        return br_texture_missing(e);
    }
    return br_texture_load(e, ref);
}

bool br_data_get_texture(br_engine *e, const cJSON *obj, const char *key, br_texture *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return false;
    }
    size_t mark = br_data_path_push_key(e, key);
    bool ok = false;
    if (!cJSON_IsString(v)) {
        br_data_errorf(e, "expected a texture path or \"sheet.json#frame\"");
    } else {
        *out = br_data_texture(e, v->valuestring);
        ok = true;
    }
    br_data_path_pop(e, mark);
    return ok;
}

/* ---- module */

static void br_data_on_node_destroy(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_data_stored *st = br_array_at(&e->data->stored, n.index);
    if (st != NULL && st->json != NULL) {
        cJSON_Delete(st->json);
        st->json = NULL;
        st->generation = 0;
    }
}

static void br_data_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_data_state *s = e->data;
    for (uint32_t i = 0; i < s->stored.count; i++) {
        br_data_stored *st = br_array_at(&s->stored, i);
        cJSON_Delete(st->json);
    }
    br_data_prefabs_free(e);
    br_array_free(&s->stored);
    br_array_free(&s->types);
    br_array_free(&s->refs);
    br_array_free(&s->watches);
    br_map_free(&s->handlers);
    br_map_free(&s->prefabs);
    cJSON_Delete(s->game_json);
    br_free(s);
    e->data = NULL;
}

void br_data_register(br_engine *e)
{
    br_data_state *s = br_alloc(sizeof(br_data_state));
    if (s == NULL) {
        return;
    }
    e->data = s;
    br_array_init(&s->types, sizeof(br_data_type));
    br_array_init(&s->stored, sizeof(br_data_stored));
    br_array_init(&s->refs, sizeof(br_data_ref));
    br_array_init(&s->watches, sizeof(br_data_watch));
    br_map_init(&s->handlers, sizeof(br_data_handler));
    br_map_init(&s->prefabs, sizeof(cJSON *));
    s->poll_frames = 30;
#ifndef NDEBUG
    s->hot_reload = true; /* debug builds only: release games do not poll the disk */
#endif
    br_scene_on_node_destroy(e, br_data_on_node_destroy, NULL);

    br_node_json_container_register(e);
    br_node_json_sprite_register(e);
    br_node_json_nine_slice_register(e);
    br_node_json_tiling_sprite_register(e);
    br_node_json_graphics_register(e);
    br_node_json_mesh_register(e);
    br_node_json_particle_container_register(e);
    br_node_json_text_register(e);
    br_data_reload_register(e);
    br_engine_on_shutdown(e, br_data_shutdown, NULL);
}
