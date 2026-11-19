/* br_node_json_view3d: JSON for "view3d" (a 2D node) and the 3D content under its root.
 *
 *   { "type": "view3d", "size": [640, 360], "auto_resize": false, "msaa": 4,
 *     "background": "#20242c", "environment": { "sky": "#9db8e8", "ground": "#5a4a3a", "intensity": 0.4 },
 *     "camera": { "position": [0, 2, 6], "target": [0, 1, 0], "fov": 50, "near": 0.1, "far": 200 },
 *     "scene": [
 *       { "type": "light", "kind": "directional", "color": "#fff0dc", "intensity": 2, "shadows": true,
 *         "position": [-4, 8, 4], "target": [0, 0, 0] },
 *       { "type": "mesh", "shape": "plane", "size": [20, 20], "cast_shadows": false,
 *         "material": { "model": "pbr", "color": "#707880", "roughness": 0.9 } },
 *       { "type": "gltf", "source": "models/Fox/Fox.glb", "animation": "Run", "scale": 0.02, "rotation": [0, 90, 0] },
 *       { "type": "group", "position": [2, 0, 0], "children": [ ... ] } ] }
 *
 * 3D nodes take "label", "position", "rotation" (Euler degrees X, Y, Z), "scale" (number or
 * [x, y, z]), "visible" and "children". Colours are written as a colour picker shows them
 * (sRGB) and converted to linear. A gltf node's "animation" is a clip name or index. Shapes and their "size": cube [x, y, z], sphere [r],
 * plane [w, d], cylinder [r, h], cone [r, h], torus [R, r], capsule [r, length].
 */
#include "data/br_data_internal.h"

enum { BR_JSON3D_GROUP, BR_JSON3D_MESH, BR_JSON3D_LIGHT, BR_JSON3D_GLTF };

static const br_data_enum br_json3d_types[] = {
    { "group", BR_JSON3D_GROUP }, { "mesh", BR_JSON3D_MESH }, { "light", BR_JSON3D_LIGHT }, { "gltf", BR_JSON3D_GLTF },
};

static const br_data_enum br_json3d_lights[] = {
    { "directional", BR_LIGHT3D_DIRECTIONAL }, { "point", BR_LIGHT3D_POINT }, { "spot", BR_LIGHT3D_SPOT },
};

static const br_data_enum br_json3d_models[] = {
    { "pbr", BR_MATERIAL3D_PBR }, { "phong", BR_MATERIAL3D_PHONG }, { "flat", BR_MATERIAL3D_FLAT }, { "toon", BR_MATERIAL3D_TOON },
};

static const br_data_enum br_json3d_alpha[] = {
    { "opaque", BR_ALPHA_OPAQUE }, { "mask", BR_ALPHA_MASK }, { "blend", BR_ALPHA_BLEND },
};

/* A colour read as sRGB and returned linear. */
static bool br_json3d_color(br_engine *e, const cJSON *obj, const char *key, br_color *out)
{
    br_color c;
    if (!br_data_get_color(e, obj, key, &c)) {
        return false;
    }
    *out = br_color_srgb_to_linear(c);
    return true;
}

/* "size": a flat number array of up to 3 values (missing ones keep their defaults). */
static void br_json3d_size(br_engine *e, const cJSON *obj, float *size, int count)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "size");
    if (v == NULL) {
        return;
    }
    if (cJSON_IsNumber(v)) {
        size[0] = (float)v->valuedouble;
        return;
    }
    if (!cJSON_IsArray(v)) {
        br_data_error(e, "size", "expected a number or an array of numbers");
        return;
    }
    for (int i = 0; i < count && i < cJSON_GetArraySize(v); i++) {
        const cJSON *item = cJSON_GetArrayItem(v, i);
        if (cJSON_IsNumber(item)) {
            size[i] = (float)item->valuedouble;
        }
    }
}

static br_material3d br_json3d_material(br_engine *e, const cJSON *json)
{
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(json, "material");
    int model = BR_MATERIAL3D_PBR;
    if (m != NULL) {
        br_data_get_enum(e, m, "model", br_json3d_models, BR_ARRAY_COUNT(br_json3d_models), &model);
    }
    br_material3d_desc d = br_material3d_default((br_material3d_model)model);
    if (m != NULL) {
        size_t mark = br_data_path_push_key(e, "material");
        int alpha = (int)d.alpha_mode;
        br_json3d_color(e, m, "color", &d.base_color);
        br_data_get_float(e, m, "metallic", &d.metallic);
        br_data_get_float(e, m, "roughness", &d.roughness);
        br_json3d_color(e, m, "emissive", &d.emissive);
        br_data_get_float(e, m, "emissive_strength", &d.emissive_strength);
        br_json3d_color(e, m, "specular", &d.specular);
        br_data_get_float(e, m, "shininess", &d.shininess);
        br_data_get_bool(e, m, "double_sided", &d.double_sided);
        br_data_get_enum(e, m, "alpha", br_json3d_alpha, BR_ARRAY_COUNT(br_json3d_alpha), &alpha);
        d.alpha_mode = (br_alpha_mode)alpha;
        br_data_get_texture(e, m, "texture", &d.textures[BR_MATERIAL3D_BASE_COLOR].texture);
        br_data_path_pop(e, mark);
    }
    return br_material3d_create(e, &d);
}

void br_data_mesh3d_destroyed(br_engine *e, br_node3d n, void *user)
{
    (void)user;
    if (e->data == NULL) {
        return;
    }
    br_data_mesh3d *m = br_array_at(&e->data->meshes3d, n.index);
    if (m != NULL && m->generation == n.generation && m->generation != 0) {
        br_geometry3d_destroy(e, m->geometry);
        br_material3d_destroy(e, m->material);
        SDL_zerop(m);
    }
}

/* Remembers what a JSON mesh created so it is freed with its node. */
static void br_json3d_track(br_engine *e, br_node3d n, br_geometry3d g, br_material3d m)
{
    br_data_state *s = e->data;
    if (!br_node3d_valid(e, n)) {
        br_geometry3d_destroy(e, g);
        br_material3d_destroy(e, m);
        return;
    }
    if (n.index >= s->meshes3d.count && !br_array_resize(&s->meshes3d, n.index + 1)) {
        return;
    }
    br_data_mesh3d *slot = br_array_at(&s->meshes3d, n.index);
    slot->generation = n.generation;
    slot->geometry = g;
    slot->material = m;
}

static br_node3d br_json3d_mesh(br_engine *e, const cJSON *json)
{
    char shape[16] = "cube";
    br_data_get_string(e, json, "shape", shape, sizeof(shape));
    float s[3] = { 1.0f, 1.0f, 1.0f };
    br_geometry3d g;
    /* every mesh object creates its own geometry and material; br_json3d_track frees them with the node */
    if (SDL_strcmp(shape, "cube") == 0) {
        br_json3d_size(e, json, s, 3);
        g = br_geometry3d_cube(e, br_vec3_make(s[0], s[1], s[2]));
    } else if (SDL_strcmp(shape, "sphere") == 0) {
        s[0] = 0.5f;
        br_json3d_size(e, json, s, 1);
        g = br_geometry3d_sphere(e, s[0], 32, 16);
    } else if (SDL_strcmp(shape, "plane") == 0) {
        br_json3d_size(e, json, s, 2);
        g = br_geometry3d_plane(e, s[0], s[1], 1, 1);
    } else if (SDL_strcmp(shape, "cylinder") == 0) {
        s[0] = 0.5f;
        br_json3d_size(e, json, s, 2);
        g = br_geometry3d_cylinder(e, s[0], s[0], s[1], 32, 1);
    } else if (SDL_strcmp(shape, "cone") == 0) {
        s[0] = 0.5f;
        br_json3d_size(e, json, s, 2);
        g = br_geometry3d_cone(e, s[0], s[1], 32);
    } else if (SDL_strcmp(shape, "torus") == 0) {
        s[0] = 0.5f;
        s[1] = 0.2f;
        br_json3d_size(e, json, s, 2);
        g = br_geometry3d_torus(e, s[0], s[1], 48, 16);
    } else if (SDL_strcmp(shape, "capsule") == 0) {
        s[0] = 0.5f;
        br_json3d_size(e, json, s, 2);
        g = br_geometry3d_capsule(e, s[0], s[1], 24, 8);
    } else {
        br_data_error(e, "shape", "expected cube, sphere, plane, cylinder, cone, torus or capsule");
        br_node3d none = { 0, 0 };
        return none;
    }
    br_material3d material = br_json3d_material(e, json);
    br_node3d n = br_mesh3d_create(e, g, material);
    br_json3d_track(e, n, g, material);
    br_mesh3d *m = br_mesh3d_get(e, n);
    if (m != NULL) {
        br_data_get_bool(e, json, "cast_shadows", &m->cast_shadows);
    }
    return n;
}

static br_node3d br_json3d_light(br_engine *e, const cJSON *json)
{
    int kind = BR_LIGHT3D_DIRECTIONAL;
    br_data_get_enum(e, json, "kind", br_json3d_lights, BR_ARRAY_COUNT(br_json3d_lights), &kind);
    br_light3d l = br_light3d_default((br_light3d_kind)kind);
    br_json3d_color(e, json, "color", &l.color);
    br_data_get_float(e, json, "intensity", &l.intensity);
    br_data_get_float(e, json, "range", &l.range);
    br_data_get_bool(e, json, "shadows", &l.cast_shadows);
    br_data_get_float(e, json, "shadow_distance", &l.shadow_distance);
    int cascades = (int)l.cascade_count;
    if (br_data_get_int(e, json, "cascades", &cascades)) {
        l.cascade_count = (uint32_t)(cascades < 1 ? 1 : cascades > BR_SHADOW3D_MAX_CASCADES ? BR_SHADOW3D_MAX_CASCADES : cascades);
    }
    float degrees = 0.0f;
    if (br_data_get_float(e, json, "inner_angle", &degrees)) {
        l.inner_angle = degrees * BR_DEG_TO_RAD;
    }
    if (br_data_get_float(e, json, "outer_angle", &degrees)) {
        l.outer_angle = degrees * BR_DEG_TO_RAD;
    }
    return br_light3d_create(e, &l);
}

static br_node3d br_json3d_gltf(br_engine *e, const cJSON *json)
{
    br_node3d none = { 0, 0 };
    char source[BR_DATA_PATH_SIZE];
    if (!br_data_get_string(e, json, "source", source, sizeof(source))) {
        br_data_errorf(e, "a gltf node needs \"source\": \"models/x.glb\"; skipped");
        return none;
    }
    /* the asset reference belongs to the load in progress (released with the scene / file root) */
    br_model *model = br_model_load(e, source);
    if (model == NULL) {
        br_data_error(e, "source", "model could not be loaded; skipped");
        return none;
    }
    br_node3d n = br_model_instantiate(e, model);
    /* "animation": a clip name, or a clip index for unnamed clips */
    const cJSON *anim = cJSON_GetObjectItemCaseSensitive(json, "animation");
    char clip[BR_DATA_NAME_SIZE];
    if (cJSON_IsNumber(anim)) {
        if (anim->valueint < 0 || !br_animator3d_play_index(e, n, (uint32_t)anim->valueint)) {
            br_data_error(e, "animation", "clip index out of range");
        }
    } else if (br_data_get_string(e, json, "animation", clip, sizeof(clip)) && !br_animator3d_play(e, n, clip)) {
        br_data_error(e, "animation", "no clip with that name in the model");
    }
    br_animator3d *a = br_animator3d_get(e, n);
    if (a != NULL) {
        br_data_get_float(e, json, "speed", &a->speed);
        br_data_get_bool(e, json, "loop", &a->loop);
        br_data_get_float(e, json, "time", &a->time);
    }
    return n;
}

static void br_json3d_transform(br_engine *e, br_node3d n, const cJSON *json)
{
    char label[BR_DATA_NAME_SIZE];
    if (br_data_get_string(e, json, "label", label, sizeof(label))) {
        br_node3d_set_label(e, n, label);
    }
    br_vec3 v;
    if (br_data_get_vec3(e, json, "position", &v)) {
        br_node3d_set_position(e, n, v);
    }
    if (br_data_get_vec3(e, json, "rotation", &v)) {
        br_node3d_set_euler(e, n, v.x * BR_DEG_TO_RAD, v.y * BR_DEG_TO_RAD, v.z * BR_DEG_TO_RAD);
    }
    const cJSON *scale = cJSON_GetObjectItemCaseSensitive(json, "scale");
    if (cJSON_IsNumber(scale)) {
        float f = (float)scale->valuedouble;
        br_node3d_set_scale(e, n, br_vec3_make(f, f, f));
    } else if (br_data_get_vec3(e, json, "scale", &v)) {
        br_node3d_set_scale(e, n, v);
    }
    if (br_data_get_vec3(e, json, "target", &v)) {
        br_node3d_look_at(e, n, v, br_vec3_make(0.0f, 1.0f, 0.0f));
    }
    bool visible = true;
    if (br_data_get_bool(e, json, "visible", &visible)) {
        br_node3d_set_visible(e, n, visible);
    }
}

static void br_json3d_children(br_engine *e, br_node3d parent, const cJSON *list, const char *key);

static void br_json3d_node(br_engine *e, br_node3d parent, const cJSON *json)
{
    if (!cJSON_IsObject(json)) {
        br_data_errorf(e, "a 3D node must be an object; skipped");
        return;
    }
    int type = BR_JSON3D_GROUP;
    br_data_get_enum(e, json, "type", br_json3d_types, BR_ARRAY_COUNT(br_json3d_types), &type);
    br_node3d n;
    if (type == BR_JSON3D_MESH) {
        n = br_json3d_mesh(e, json);
    } else if (type == BR_JSON3D_LIGHT) {
        n = br_json3d_light(e, json);
    } else if (type == BR_JSON3D_GLTF) {
        n = br_json3d_gltf(e, json);
    } else {
        n = br_node3d_create_group(e);
    }
    if (!br_node3d_valid(e, n)) {
        return;
    }
    br_node3d_add_child(e, parent, n);
    br_json3d_transform(e, n, json);
    br_json3d_children(e, n, cJSON_GetObjectItemCaseSensitive(json, "children"), "children");
}

static void br_json3d_children(br_engine *e, br_node3d parent, const cJSON *list, const char *key)
{
    if (list == NULL) {
        return;
    }
    size_t mark = br_data_path_push_key(e, key);
    if (!cJSON_IsArray(list)) {
        br_data_errorf(e, "expected an array of 3D nodes");
    } else {
        int i = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, list)
        {
            size_t item_mark = br_data_path_push_index(e, i);
            br_json3d_node(e, parent, item);
            br_data_path_pop(e, item_mark);
            i++;
        }
    }
    br_data_path_pop(e, mark);
}

static br_node br_data_view3d_create(br_engine *e, const cJSON *json)
{
    br_vec2 size = br_vec2_make(640.0f, 360.0f);
    br_data_get_vec2(e, json, "size", &size);
    return br_view3d_create(e, size.x, size.y);
}

static bool br_data_view3d_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_vec2 size;
    if (br_data_get_vec2(e, json, "size", &size)) {
        br_view3d_set_size(e, n, size.x, size.y);
    }
    bool flag = false;
    if (br_data_get_bool(e, json, "auto_resize", &flag)) {
        br_view3d_set_auto_resize(e, n, flag);
    }
    int msaa = 1;
    if (br_data_get_int(e, json, "msaa", &msaa)) {
        br_view3d_set_msaa(e, n, (uint32_t)(msaa > 0 ? msaa : 1));
    }
    br_color c;
    if (br_json3d_color(e, json, "background", &c)) {
        br_view3d_set_background(e, n, c);
    }
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(json, "environment");
    if (env != NULL) {
        size_t mark = br_data_path_push_key(e, "environment");
        br_color sky = br_color_make(0.4f, 0.45f, 0.55f, 1.0f);
        br_color ground = br_color_make(0.15f, 0.12f, 0.1f, 1.0f);
        float intensity = 0.4f;
        br_json3d_color(e, env, "sky", &sky);
        br_json3d_color(e, env, "ground", &ground);
        br_data_get_float(e, env, "intensity", &intensity);
        br_view3d_set_environment(e, n, sky, ground, intensity);
        br_data_path_pop(e, mark);
    }
    br_node3d camera = br_view3d_camera(e, n);
    const cJSON *cam = cJSON_GetObjectItemCaseSensitive(json, "camera");
    if (cam != NULL) {
        size_t mark = br_data_path_push_key(e, "camera");
        br_camera3d *cd = br_camera3d_get(e, camera);
        float fov = 0.0f;
        if (cd != NULL && br_data_get_float(e, cam, "fov", &fov)) {
            cd->fov_y = fov * BR_DEG_TO_RAD;
        }
        if (cd != NULL) {
            br_data_get_float(e, cam, "near", &cd->near_z);
            br_data_get_float(e, cam, "far", &cd->far_z);
        }
        br_json3d_transform(e, camera, cam);
        br_data_path_pop(e, mark);
    }
    /* rebuild the 3D content (hot reload applies the JSON again): keep only the camera */
    br_node3d root = br_view3d_root(e, n);
    br_node3d child = br_node3d_first_child(e, root);
    while (br_node3d_valid(e, child)) {
        br_node3d next = br_node3d_next_sibling(e, child);
        if (child.index != camera.index || child.generation != camera.generation) {
            br_node3d_destroy(e, child);
        }
        child = next;
    }
    br_json3d_children(e, root, cJSON_GetObjectItemCaseSensitive(json, "scene"), "scene");
    return true;
}

void br_node_json_view3d_register(br_engine *e)
{
    uint32_t type = br_view3d_type(e);
    br_node_type_set_from_json(e, type, br_data_view3d_from_json);
    br_data_add_type(e, "view3d", type, br_data_view3d_create, NULL);
}
