/* br_material3d: material resources (plain parameter structs in a generational pool). */
#include "three_d/br_three_d_internal.h"

br_material3d_desc br_material3d_default(br_material3d_model model)
{
    br_material3d_desc d;
    SDL_zero(d);
    d.model = model;
    d.base_color = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    d.metallic = 0.0f;
    d.roughness = 0.5f;
    d.emissive = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    d.emissive_strength = 1.0f;
    d.normal_scale = 1.0f;
    d.occlusion_strength = 1.0f;
    d.alpha_mode = BR_ALPHA_OPAQUE;
    d.alpha_cutoff = 0.5f;
    d.receive_shadows = true;
    d.specular = br_color_make(0.5f, 0.5f, 0.5f, 1.0f);
    d.shininess = 32.0f;
    d.toon_bands = 3;
    return d;
}

br_material3d br_material3d_create(br_engine *e, const br_material3d_desc *desc)
{
    br_material3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    if (s == NULL || desc == NULL) {
        return none;
    }
    if (desc->model == BR_MATERIAL3D_CUSTOM && desc->custom_fragment_shader == NULL) {
        br_log_error("br_material3d_create: custom material without a fragment shader");
        return none;
    }
    if (desc->custom_uniform_size > BR_MATERIAL3D_CUSTOM_UNIFORM_SIZE ||
        desc->custom_texture_count > BR_MATERIAL3D_CUSTOM_TEXTURES) {
        br_log_error("br_material3d_create: custom uniforms or textures over the limit");
        return none;
    }
    br_material3d m;
    br_material3d_desc *d = br_pool_alloc(&s->materials, &m.index, &m.generation);
    if (d == NULL) {
        return none;
    }
    *d = *desc;
    return m;
}

void br_material3d_destroy(br_engine *e, br_material3d m)
{
    if (e->three_d != NULL) {
        br_pool_release(&e->three_d->materials, m.index, m.generation);
    }
}

void br_material3d_free_all(br_engine *e)
{
    br_pool_free(&e->three_d->materials);
}

bool br_material3d_valid(br_engine *e, br_material3d m)
{
    return br_material3d_params(e, m) != NULL;
}

br_material3d_desc *br_material3d_params(br_engine *e, br_material3d m)
{
    if (e->three_d == NULL) {
        return NULL;
    }
    return br_pool_get(&e->three_d->materials, m.index, m.generation);
}

void br_material3d_set_texture(br_engine *e, br_material3d m, br_material3d_slot slot, br_texture texture)
{
    br_material3d_desc *d = br_material3d_params(e, m);
    if (d == NULL || (int)slot < 0 || slot >= BR_MATERIAL3D_SLOT_COUNT) {
        return;
    }
    SDL_zero(d->textures[slot]);
    d->textures[slot].texture = texture;
}

