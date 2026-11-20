/* br_prefab: prefab files (cached parsed JSON) expanded with overrides into node JSON. */
#include "data/br_data_internal.h"
#include "data/br_gltf_scene_internal.h" /* a .glb / .gltf path is a prefab too */

static const cJSON *br_data_prefab_doc(br_engine *e, const char *path)
{
    br_data_state *s = e->data;
    cJSON **cached = br_map_get(&s->prefabs, path);
    if (cached != NULL) {
        return *cached;
    }
    if (br_data_is_model_path(path)) {
        cJSON *view = br_data_model_scene_json(e, path, false); /* a view3d wrapping the model */
        if (view != NULL) {
            br_map_set(&s->prefabs, path, &view);
        }
        return view;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (!br_file_read_all(e, path, &bytes, &size)) {
        br_data_errorf(e, "cannot read prefab '%s'", path);
        return NULL;
    }
    cJSON *doc = br_data_parse(e, path, bytes, size);
    br_free(bytes);
    if (doc == NULL) {
        br_data_errorf(e, "prefab '%s' is not valid JSON", path);
        return NULL;
    }
    if (!cJSON_IsObject(doc)) {
        br_data_errorf(e, "prefab '%s' must be one node object", path);
        cJSON_Delete(doc);
        return NULL;
    }
    br_map_set(&s->prefabs, path, &doc);
    return doc;
}

/* Merges overrides into target (see br_prefab.h for the rules). */
static void br_data_merge(cJSON *target, const cJSON *overrides)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, overrides)
    {
        if (SDL_strcmp(item->string, "prefab") == 0) {
            continue;
        }
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(target, item->string);
        if (cJSON_IsObject(existing) && cJSON_IsObject(item)) {
            br_data_merge(existing, item);
        } else if (cJSON_IsArray(existing) && cJSON_IsArray(item) && SDL_strcmp(item->string, "children") == 0) {
            const cJSON *child = NULL;
            cJSON_ArrayForEach(child, item)
            {
                cJSON_AddItemToArray(existing, cJSON_Duplicate(child, true));
            }
        } else {
            br_data_json_set(target, item->string, cJSON_Duplicate(item, true));
        }
    }
}

/* Pushes path onto the stack of prefabs being built. False (logged with the chain) if it is
 * already there - a prefab that includes itself, through children or "based on" - or if the
 * stack is full. */
static bool br_data_prefab_push(br_engine *e, const char *path)
{
    br_data_state *s = e->data;
    for (uint32_t i = 0; i < s->prefab_depth; i++) {
        if (SDL_strcmp(s->prefab_stack[i], path) == 0) {
            char chain[400] = "";
            for (uint32_t k = i; k < s->prefab_depth; k++) {
                SDL_strlcat(chain, s->prefab_stack[k], sizeof(chain));
                SDL_strlcat(chain, " -> ", sizeof(chain));
            }
            SDL_strlcat(chain, path, sizeof(chain));
            size_t mark = br_data_path_push_key(e, "prefab");
            br_data_errorf(e, "prefab '%s' includes itself (%s); node skipped", path, chain);
            br_data_path_pop(e, mark);
            return false;
        }
    }
    if (s->prefab_depth >= BR_PREFAB_MAX_DEPTH) {
        br_data_error(e, "prefab", "prefabs nest more than 16 deep; node skipped");
        return false;
    }
    if (!br_str_copy(s->prefab_stack[s->prefab_depth], BR_DATA_PATH_SIZE, path)) {
        br_data_error(e, "prefab", "prefab path too long; node skipped");
        return false;
    }
    s->prefab_depth++;
    return true;
}

cJSON *br_data_expand_prefab(br_engine *e, const cJSON *json)
{
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "prefab");
    if (!cJSON_IsString(path)) {
        br_data_error(e, "prefab", "expected a prefab file path; node skipped");
        return NULL;
    }
    if (!br_data_prefab_push(e, path->valuestring)) {
        return NULL;
    }
    const cJSON *doc = br_data_prefab_doc(e, path->valuestring);
    if (doc == NULL) {
        return NULL;
    }
    cJSON *out = NULL;
    if (cJSON_GetObjectItemCaseSensitive(doc, "prefab") != NULL) {
        out = br_data_expand_prefab(e, doc); /* a prefab based on another prefab */
    } else {
        out = cJSON_Duplicate(doc, true);
    }
    if (out != NULL) {
        br_data_merge(out, json);
    }
    return out;
}

void br_data_prefabs_free(br_engine *e)
{
    br_data_state *s = e->data;
    uint32_t slot = 0;
    const char *key = NULL;
    void *value = NULL;
    while (br_map_next(&s->prefabs, &slot, &key, &value)) {
        cJSON *doc = NULL;
        SDL_memcpy(&doc, value, sizeof(doc));
        cJSON_Delete(doc);
    }
    br_map_clear(&s->prefabs);
}

void br_prefab_clear_cache(br_engine *e)
{
    br_data_prefabs_free(e);
}

br_node br_prefab_instantiate_at(br_engine *e, const char *path, br_node parent, br_vec2 position)
{
    br_node n = br_prefab_instantiate(e, path, parent, NULL);
    br_node_set_position(e, n, position);
    return n;
}

br_node br_prefab_instantiate(br_engine *e, const char *path, br_node parent, const cJSON *overrides)
{
    cJSON *json = overrides != NULL ? cJSON_Duplicate(overrides, true) : cJSON_CreateObject();
    if (!cJSON_IsObject(json)) {
        br_log_error("br_prefab_instantiate: overrides must be a JSON object");
        cJSON_Delete(json);
        br_node none = { 0, 0 };
        return none;
    }
    br_data_json_set(json, "prefab", cJSON_CreateString(path));
    br_data_scope saved;
    br_data_begin_file(e, path, &saved);
    br_node n = br_scene_file_parse_node(e, json, parent);
    br_data_end_file(e, &saved);
    cJSON_Delete(json);
    return n;
}
