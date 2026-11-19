/* br_gltf_scene: a .glb / .gltf path used where a scene file or a prefab is expected.
 *
 * The file stands for this node JSON (so everything a scene file does - asset collection, the
 * scene's group, label references, errors - works unchanged):
 *
 *   { "type": "view3d", "label": "island", "auto_resize": true, "msaa": 4, "background": ...,
 *     "scene": [ { "type": "gltf", "source": "levels/island.glb", "camera": true, "default_light": true } ] }
 *
 * br_scene_load gets a full-window view (glTF scene extras "background", "msaa", "sky",
 * "ground", "ambient" set it up); br_prefab_instantiate gets a 640 x 360 view (overrides apply
 * to it, "size" included).
 */
#include "data/br_gltf_scene_internal.h"

bool br_data_is_model_path(const char *path)
{
    if (path == NULL) {
        return false;
    }
    const char *hash = SDL_strchr(path, '#');
    size_t len = hash != NULL ? (size_t)(hash - path) : SDL_strlen(path);
    return (len > 4 && SDL_strncasecmp(path + len - 4, ".glb", 4) == 0) ||
           (len > 5 && SDL_strncasecmp(path + len - 5, ".gltf", 5) == 0);
}

/* The parsed scene extras of a cached model (NULL if not loaded yet or none). */
static cJSON *br_gltf_scene_extras(br_engine *e, const char *path)
{
    char file[BR_DATA_PATH_SIZE];
    const char *hash = SDL_strchr(path, '#');
    size_t len = hash != NULL ? (size_t)(hash - path) : SDL_strlen(path);
    if (len >= sizeof(file)) {
        return NULL;
    }
    SDL_memcpy(file, path, len);
    file[len] = '\0';
    const char *type = br_assets_type(e, file);
    const br_model *m = type != NULL && SDL_strcmp(type, "model") == 0 ? br_assets_get(e, file) : NULL;
    return m != NULL && m->scene_extras != NULL ? cJSON_Parse(m->scene_extras) : NULL;
}

/* Copies extras[key] into view[as] when it has the expected JSON type. */
static void br_gltf_copy_extra(cJSON *view, const cJSON *extras, const char *key, const char *as, bool number)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(extras, key);
    if (v != NULL && (number ? cJSON_IsNumber(v) : (cJSON_IsString(v) || cJSON_IsArray(v)))) {
        cJSON_AddItemToObject(view, as, cJSON_Duplicate(v, true));
    }
}

cJSON *br_data_model_scene_json(br_engine *e, const char *path, bool scene)
{
    cJSON *view = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    if (view == NULL || content == NULL || list == NULL) {
        cJSON_Delete(view);
        cJSON_Delete(content);
        cJSON_Delete(list);
        return NULL;
    }
    /* label: the file name without directory and extension ("levels/island.glb" -> "island") */
    char label[BR_DATA_NAME_SIZE];
    const char *base = SDL_strrchr(path, '/');
    br_str_copy(label, sizeof(label), base != NULL ? base + 1 : path);
    char *dot = SDL_strchr(label, '.');
    if (dot != NULL && dot != label) {
        *dot = '\0';
    }
    cJSON_AddStringToObject(view, "type", "view3d");
    cJSON_AddStringToObject(view, "label", label);
    if (scene) {
        int w = 0;
        int h = 0;
        br_engine_window_size(e, &w, &h);
        cJSON *size = cJSON_AddArrayToObject(view, "size");
        cJSON_AddItemToArray(size, cJSON_CreateNumber(w > 0 ? w : 1280));
        cJSON_AddItemToArray(size, cJSON_CreateNumber(h > 0 ? h : 720));
        cJSON_AddBoolToObject(view, "auto_resize", true);
        cJSON *extras = br_gltf_scene_extras(e, path);
        if (cJSON_IsObject(extras)) {
            br_gltf_copy_extra(view, extras, "background", "background", false);
            br_gltf_copy_extra(view, extras, "msaa", "msaa", true);
            if (cJSON_HasObjectItem(extras, "sky") || cJSON_HasObjectItem(extras, "ground") ||
                cJSON_HasObjectItem(extras, "ambient")) {
                cJSON *env = cJSON_AddObjectToObject(view, "environment");
                br_gltf_copy_extra(env, extras, "sky", "sky", false);
                br_gltf_copy_extra(env, extras, "ground", "ground", false);
                br_gltf_copy_extra(env, extras, "ambient", "intensity", true);
            }
        }
        cJSON_Delete(extras);
    }
    cJSON_AddStringToObject(content, "type", "gltf");
    cJSON_AddStringToObject(content, "source", path);
    cJSON_AddBoolToObject(content, "camera", true);
    cJSON_AddBoolToObject(content, "default_light", true);
    cJSON_AddItemToArray(list, content);
    cJSON_AddItemToObject(view, "scene", list);
    return view;
}

br_node br_data_load_model_owned(br_engine *e, const char *path, br_node parent, br_asset_group *owner)
{
    br_node none = { 0, 0 };
    cJSON *json = br_data_model_scene_json(e, path, true);
    if (json == NULL) {
        return none;
    }
    br_asset_group *saved_owner = e->data->owner_group;
    e->data->owner_group = owner;
    br_data_scope saved;
    br_data_begin_file(e, path, &saved);
    br_node n = br_scene_file_parse_node(e, json, parent);
    br_data_end_file(e, &saved);
    e->data->owner_group = saved_owner;
    cJSON_Delete(json);
    if (br_node_valid(e, n)) {
        br_log_info("scene file: loaded glTF '%s'", path);
    }
    return n;
}
