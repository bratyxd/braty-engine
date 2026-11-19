/* br_node_json_particle_container: JSON for "particle_container". Emitters are game code; a
 * scene file sets the texture, which property groups are dynamic and optional start particles.
 *
 *   { "type": "particle_container", "texture": "atlas.json#spark.png",
 *     "dynamic": ["position", "rotation"],
 *     "particles": [ { "position": [10, 20], "scale": 0.5, "tint": "orange", "alpha": 0.8 } ] }
 */
#include "data/br_data_internal.h"

static const br_data_enum br_property_names[] = {
    { "vertex", BR_PARTICLE_VERTEX }, { "position", BR_PARTICLE_POSITION }, { "rotation", BR_PARTICLE_ROTATION },
    { "uvs", BR_PARTICLE_UVS },       { "color", BR_PARTICLE_COLOR },
};

static void br_data_particle(br_engine *e, const cJSON *json, br_particle *p)
{
    br_vec2 v;
    br_color c;
    if (br_data_get_vec2(e, json, "position", &v)) {
        p->x = v.x;
        p->y = v.y;
    }
    if (br_data_get_vec2(e, json, "scale", &v)) {
        p->scale_x = v.x;
        p->scale_y = v.y;
    }
    if (br_data_get_vec2(e, json, "anchor", &v)) {
        p->anchor_x = v.x;
        p->anchor_y = v.y;
    }
    br_data_get_float(e, json, "rotation", &p->rotation);
    if (br_data_get_color(e, json, "tint", &c)) {
        p->tint = br_color_to_hex(c);
    }
    br_data_get_float(e, json, "alpha", &p->alpha);
    br_data_get_texture(e, json, "texture", &p->texture);
}

static bool br_data_particles_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_texture texture;
    if (br_data_get_texture(e, json, "texture", &texture)) {
        br_particle_container_set_texture(e, n, texture);
    }
    const cJSON *dynamic = cJSON_GetObjectItemCaseSensitive(json, "dynamic");
    if (dynamic != NULL) {
        size_t mark = br_data_path_push_key(e, "dynamic");
        uint32_t mask = 0;
        int i = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, dynamic)
        {
            bool found = false;
            for (uint32_t k = 0; cJSON_IsString(item) && k < BR_ARRAY_COUNT(br_property_names); k++) {
                if (SDL_strcmp(br_property_names[k].name, item->valuestring) == 0) {
                    mask |= (uint32_t)br_property_names[k].value;
                    found = true;
                }
            }
            if (!found) {
                size_t im = br_data_path_push_index(e, i);
                br_data_errorf(e, "expected one of vertex, position, rotation, uvs, color");
                br_data_path_pop(e, im);
            }
            i++;
        }
        if (!cJSON_IsArray(dynamic)) {
            br_data_errorf(e, "expected an array of property group names");
        } else {
            br_particle_container_set_dynamic(e, n, mask);
        }
        br_data_path_pop(e, mark);
    }
    const cJSON *particles = cJSON_GetObjectItemCaseSensitive(json, "particles");
    if (particles != NULL) {
        size_t mark = br_data_path_push_key(e, "particles");
        int count = cJSON_IsArray(particles) ? cJSON_GetArraySize(particles) : 0;
        br_particle_container_clear(e, n);
        if (count > 0) {
            br_particle_container_add(e, n, (uint32_t)count);
        } else if (!cJSON_IsArray(particles)) {
            br_data_errorf(e, "expected an array of particles");
        }
        for (int i = 0; i < count; i++) {
            uint32_t total = 0;
            br_particle *list = br_particle_container_particles(e, n, &total);
            size_t im = br_data_path_push_index(e, i);
            if (list != NULL && (uint32_t)i < total) {
                br_data_particle(e, cJSON_GetArrayItem(particles, i), &list[i]);
            }
            br_data_path_pop(e, im);
        }
        br_particle_container_update(e, n);
        br_data_path_pop(e, mark);
    }
    return true;
}

void br_node_json_particle_container_register(br_engine *e)
{
    uint32_t type = br_particle_container_type(e);
    br_node_type_set_from_json(e, type, br_data_particles_from_json);
    br_data_add_type(e, "particle_container", type, NULL, NULL);
}
