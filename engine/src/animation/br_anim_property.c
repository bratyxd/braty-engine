/* br_anim_property: the property registry, the built-in rows (2D nodes, sprites, 3D nodes,
 * materials, morph weights), component fields, and "<node path>:<property>" resolution. */
#include "animation/br_animation_internal.h"

/* ---- targets */

br_anim_target br_anim_target_2d(br_node n)
{
    br_anim_target t = { BR_ANIM_TARGET_2D, n.index, n.generation };
    return t;
}

br_anim_target br_anim_target_3d(br_node3d n)
{
    br_anim_target t = { BR_ANIM_TARGET_3D, n.index, n.generation };
    return t;
}

br_node br_anim_target_node(br_anim_target t)
{
    br_node n = { 0, 0 };
    if (t.kind == BR_ANIM_TARGET_2D) {
        n.index = t.index;
        n.generation = t.generation;
    }
    return n;
}

br_node3d br_anim_target_node3d(br_anim_target t)
{
    br_node3d n = { 0, 0 };
    if (t.kind == BR_ANIM_TARGET_3D) {
        n.index = t.index;
        n.generation = t.generation;
    }
    return n;
}

bool br_anim_target_valid(br_engine *e, br_anim_target t)
{
    if (t.kind == BR_ANIM_TARGET_2D) {
        return br_node_valid(e, br_anim_target_node(t));
    }
    if (t.kind == BR_ANIM_TARGET_3D) {
        return br_node3d_valid(e, br_anim_target_node3d(t));
    }
    return false;
}

bool br_anim_target_equal(br_anim_target a, br_anim_target b)
{
    return a.kind == b.kind && a.index == b.index && a.generation == b.generation;
}

bool br_anim_target_is_under(br_engine *e, br_anim_target t, br_anim_target ancestor)
{
    if (t.kind != ancestor.kind) {
        return false;
    }
    if (br_anim_target_equal(t, ancestor)) {
        return true;
    }
    if (t.kind == BR_ANIM_TARGET_2D) {
        return br_node_is_descendant_of(e, br_anim_target_node(t), br_anim_target_node(ancestor));
    }
    br_node3d target = br_anim_target_node3d(ancestor);
    for (br_node3d p = br_node3d_parent(e, br_anim_target_node3d(t)); br_node3d_valid(e, p); p = br_node3d_parent(e, p)) {
        if (p.index == target.index && p.generation == target.generation) {
            return true;
        }
    }
    return false;
}

/* One path segment below `cur`: "#i" (glTF node i of a model3d), a direct child's label, else a
 * descendant's label. */
static br_anim_target br_anim_child(br_engine *e, br_anim_target cur, const char *seg)
{
    br_anim_target none = { BR_ANIM_TARGET_NONE, 0, 0 };
    if (cur.kind == BR_ANIM_TARGET_2D) {
        br_node n = br_anim_target_node(cur);
        uint32_t count = br_node_child_count(e, n);
        for (uint32_t i = 0; i < count; i++) {
            br_node c = br_node_child_at(e, n, i);
            const char *label = br_node_label(e, c);
            if (br_node_valid(e, c) && label != NULL && SDL_strcmp(label, seg) == 0) {
                return br_anim_target_2d(c);
            }
        }
        br_node deep = br_node_find_by_label(e, n, seg, true);
        return br_node_valid(e, deep) ? br_anim_target_2d(deep) : none;
    }
    br_node3d n = br_anim_target_node3d(cur);
    if (seg[0] == '#') {
        br_node3d g = br_model3d_node(e, n, (uint32_t)SDL_atoi(seg + 1));
        return br_node3d_valid(e, g) ? br_anim_target_3d(g) : none;
    }
    br_node3d deep = br_node3d_find_by_label(e, n, seg, true); /* children first, then deeper */
    return br_node3d_valid(e, deep) ? br_anim_target_3d(deep) : none;
}

br_anim_target br_anim_target_find(br_engine *e, br_anim_target root, const char *node_path)
{
    br_anim_target none = { BR_ANIM_TARGET_NONE, 0, 0 };
    if (!br_anim_target_valid(e, root)) {
        return none;
    }
    br_anim_target cur = root;
    const char *p = node_path != NULL ? node_path : "";
    while (*p != '\0') {
        char seg[128];
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        if (len >= sizeof(seg)) {
            return none;
        }
        SDL_memcpy(seg, p, len);
        seg[len] = '\0';
        p += len;
        if (*p == '/') {
            p++;
        }
        if (len == 0) {
            continue;
        }
        cur = br_anim_child(e, cur, seg);
        if (cur.kind == BR_ANIM_TARGET_NONE) {
            return none;
        }
    }
    return cur;
}

/* ---- the registry */

uint32_t br_anim_value_components(br_anim_value_type type)
{
    switch (type) {
    case BR_ANIM_VEC2: return 2;
    case BR_ANIM_VEC3: return 3;
    case BR_ANIM_COLOR: return 4;
    case BR_ANIM_QUAT: return 4;
    case BR_ANIM_WEIGHTS: return BR_ANIM_MAX_COMPONENTS;
    default: return 1;
    }
}

uint32_t br_anim_property_find(br_engine *e, br_anim_target_kind kind, const char *name)
{
    br_animation_state *s = e->animation;
    if (s == NULL || name == NULL) {
        return BR_ANIM_NONE;
    }
    for (uint32_t i = 0; i < s->properties.count; i++) {
        const br_anim_property_row *row = br_array_at(&s->properties, i);
        if (row->desc.kind == kind && SDL_strcmp(row->desc.name, name) == 0) {
            return i;
        }
    }
    return BR_ANIM_NONE;
}

uint32_t br_anim_property_register(br_engine *e, const br_anim_property_desc *desc)
{
    br_animation_state *s = br_anim_state(e);
    if (s == NULL || desc == NULL || desc->name == NULL || desc->get == NULL || desc->set == NULL ||
        desc->kind == BR_ANIM_TARGET_NONE) {
        br_log_error("br_anim_property_register: name, kind, get and set are required");
        return BR_ANIM_NONE;
    }
    br_anim_property_row row;
    row.desc = *desc;
    row.desc.name = br_strdup(desc->name);
    if (row.desc.name == NULL) {
        return BR_ANIM_NONE;
    }
    uint32_t id = br_anim_property_find(e, desc->kind, desc->name);
    if (id != BR_ANIM_NONE) {
        br_anim_property_row *old = br_array_at(&s->properties, id);
        br_free((char *)old->desc.name);
        *old = row;
        return id;
    }
    if (br_array_push(&s->properties, &row) == NULL) {
        br_free((char *)row.desc.name);
        return BR_ANIM_NONE;
    }
    return s->properties.count - 1;
}

const br_anim_property_desc *br_anim_property_get_desc(br_engine *e, uint32_t id)
{
    br_animation_state *s = e->animation;
    if (s == NULL || id >= s->properties.count) {
        return NULL;
    }
    const br_anim_property_row *row = br_array_at(&s->properties, id);
    return &row->desc;
}

uint32_t br_anim_property_count(br_engine *e)
{
    return e->animation != NULL ? e->animation->properties.count : 0;
}

/* ---- component fields ("component.field", 2D nodes) */

static bool br_anim_resolve_field(br_engine *e, const char *name, br_anim_prop_ref *out)
{
    const char *dot = SDL_strchr(name, '.');
    if (dot == NULL || dot == name || (size_t)(dot - name) >= 64) {
        return false;
    }
    char comp[64];
    SDL_memcpy(comp, name, (size_t)(dot - name));
    comp[dot - name] = '\0';
    uint32_t id = br_component_find(e, comp);
    const br_component_desc *desc = id != 0 ? br_component_get_desc(e, id) : NULL;
    if (desc == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < desc->field_count; i++) {
        const br_field_desc *f = &desc->fields[i];
        if (SDL_strcmp(f->name, dot + 1) != 0) {
            continue;
        }
        br_anim_value_type type;
        switch (f->type) {
        case BR_FIELD_FLOAT: type = BR_ANIM_FLOAT; break;
        case BR_FIELD_INT: type = BR_ANIM_INT; break;
        case BR_FIELD_BOOL: type = BR_ANIM_BOOL; break;
        case BR_FIELD_VEC2: type = BR_ANIM_VEC2; break;
        case BR_FIELD_VEC3: type = BR_ANIM_VEC3; break;
        case BR_FIELD_COLOR: type = BR_ANIM_COLOR; break;
        default: return false; /* strings, assets, node references are not animatable */
        }
        out->property = BR_ANIM_NONE;
        out->component = id;
        out->offset = (uint32_t)f->offset;
        out->type = type;
        return true;
    }
    return false;
}

bool br_anim_resolve(br_engine *e, br_anim_target root, const char *path, br_anim_target *node,
                     br_anim_prop_ref *prop)
{
    const char *colon = SDL_strrchr(path, ':');
    const char *name = colon != NULL ? colon + 1 : path;
    char node_path[256] = "";
    if (colon != NULL) {
        size_t len = (size_t)(colon - path);
        if (len >= sizeof(node_path)) {
            return false;
        }
        SDL_memcpy(node_path, path, len);
        node_path[len] = '\0';
    }
    *node = br_anim_target_find(e, root, node_path);
    if (node->kind == BR_ANIM_TARGET_NONE) {
        return false;
    }
    uint32_t id = br_anim_property_find(e, node->kind, name);
    if (id != BR_ANIM_NONE) {
        prop->property = id;
        prop->component = 0;
        prop->offset = 0;
        prop->type = br_anim_property_get_desc(e, id)->type;
        return true;
    }
    return node->kind == BR_ANIM_TARGET_2D && br_anim_resolve_field(e, name, prop);
}

void br_anim_prop_get(br_engine *e, br_anim_target t, const br_anim_prop_ref *prop, br_anim_value *out)
{
    SDL_zerop(out);
    out->count = br_anim_value_components(prop->type);
    if (prop->property != BR_ANIM_NONE) {
        const br_anim_property_desc *d = br_anim_property_get_desc(e, prop->property);
        if (d != NULL) {
            d->get(e, t, out);
        }
        return;
    }
    const uint8_t *c = br_component_get(e, br_anim_target_node(t), prop->component);
    if (c == NULL) {
        return;
    }
    c += prop->offset;
    if (prop->type == BR_ANIM_INT) {
        int32_t v;
        SDL_memcpy(&v, c, sizeof(v));
        out->f[0] = (float)v;
    } else if (prop->type == BR_ANIM_BOOL) {
        bool v;
        SDL_memcpy(&v, c, sizeof(v));
        out->f[0] = v ? 1.0f : 0.0f;
    } else {
        SDL_memcpy(out->f, c, sizeof(float) * out->count);
    }
}

void br_anim_prop_set(br_engine *e, br_anim_target t, const br_anim_prop_ref *prop, const br_anim_value *value)
{
    if (prop->property != BR_ANIM_NONE) {
        const br_anim_property_desc *d = br_anim_property_get_desc(e, prop->property);
        if (d != NULL) {
            d->set(e, t, value);
        }
        return;
    }
    uint8_t *c = br_component_get(e, br_anim_target_node(t), prop->component);
    if (c == NULL) {
        return;
    }
    c += prop->offset;
    if (prop->type == BR_ANIM_INT) {
        int32_t v = (int32_t)SDL_floorf(value->f[0] + 0.5f);
        SDL_memcpy(c, &v, sizeof(v));
    } else if (prop->type == BR_ANIM_BOOL) {
        bool v = value->f[0] >= 0.5f;
        SDL_memcpy(c, &v, sizeof(v));
    } else {
        SDL_memcpy(c, value->f, sizeof(float) * br_anim_value_components(prop->type));
    }
}

bool br_anim_get(br_engine *e, br_anim_target root, const char *path, br_anim_value *out)
{
    br_anim_target node;
    br_anim_prop_ref prop;
    if (path == NULL || out == NULL || !br_anim_resolve(e, root, path, &node, &prop)) {
        return false;
    }
    br_anim_prop_get(e, node, &prop, out);
    return true;
}

bool br_anim_set(br_engine *e, br_anim_target root, const char *path, const br_anim_value *value)
{
    br_anim_target node;
    br_anim_prop_ref prop;
    if (path == NULL || value == NULL || !br_anim_resolve(e, root, path, &node, &prop)) {
        return false;
    }
    br_anim_prop_set(e, node, &prop, value);
    return true;
}

/* ---- built-in rows: 2D nodes */

static br_node br_n(br_anim_target t)
{
    return br_anim_target_node(t);
}

static void get_position(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec2 p = br_node_position(e, br_n(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
}
static void set_position(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_position(e, br_n(t), br_vec2_make(v->f[0], v->f[1]));
}
static void get_x(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node_position(e, br_n(t)).x;
}
static void set_x(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_position(e, br_n(t), br_vec2_make(v->f[0], br_node_position(e, br_n(t)).y));
}
static void get_y(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node_position(e, br_n(t)).y;
}
static void set_y(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_position(e, br_n(t), br_vec2_make(br_node_position(e, br_n(t)).x, v->f[0]));
}
static void get_scale(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec2 p = br_node_scale(e, br_n(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
}
static void set_scale(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_scale(e, br_n(t), br_vec2_make(v->f[0], v->f[1]));
}
static void get_rotation(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node_rotation(e, br_n(t));
}
static void set_rotation(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_rotation(e, br_n(t), v->f[0]);
}
static void get_skew(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec2 p = br_node_skew(e, br_n(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
}
static void set_skew(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_skew(e, br_n(t), br_vec2_make(v->f[0], v->f[1]));
}
static void get_pivot(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec2 p = br_node_pivot(e, br_n(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
}
static void set_pivot(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_pivot(e, br_n(t), br_vec2_make(v->f[0], v->f[1]));
}
static void get_alpha(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node_alpha(e, br_n(t));
}
static void set_alpha(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_alpha(e, br_n(t), v->f[0]);
}
static void get_tint(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_color c = br_node_tint(e, br_n(t));
    v->f[0] = c.r;
    v->f[1] = c.g;
    v->f[2] = c.b;
    v->f[3] = c.a;
}
static void set_tint(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_color c = { v->f[0], v->f[1], v->f[2], v->f[3] };
    br_node_set_tint(e, br_n(t), c);
}
static void get_visible(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node_visible(e, br_n(t)) ? 1.0f : 0.0f;
}
static void set_visible(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_visible(e, br_n(t), v->f[0] >= 0.5f);
}
static void get_z_index(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = (float)br_node_z_index(e, br_n(t));
}
static void set_z_index(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node_set_z_index(e, br_n(t), (int)SDL_floorf(v->f[0] + 0.5f));
}
static void get_sprite_texture(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->texture = br_sprite_texture(e, br_n(t));
}
static void set_sprite_texture(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    if (br_node_data(e, br_n(t), br_sprite_type(e)) != NULL) {
        br_sprite_set_texture(e, br_n(t), v->texture);
    }
}
static void get_sprite_frame(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = (float)br_animated_sprite_current_frame(e, br_n(t));
}
static void set_sprite_frame(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    if (br_animated_sprite_is(e, br_n(t))) {
        br_animated_sprite_goto_and_stop(e, br_n(t), (uint32_t)SDL_max(0.0f, SDL_floorf(v->f[0] + 0.5f)));
    }
}

/* ---- built-in rows: 3D nodes, materials, morph weights */

static br_node3d br_n3(br_anim_target t)
{
    return br_anim_target_node3d(t);
}

static void get3_position(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec3 p = br_node3d_position(e, br_n3(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
    v->f[2] = p.z;
}
static void set3_position(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node3d_set_position(e, br_n3(t), br_vec3_make(v->f[0], v->f[1], v->f[2]));
}
static void get3_rotation(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_quat q = br_node3d_rotation(e, br_n3(t));
    v->f[0] = q.x;
    v->f[1] = q.y;
    v->f[2] = q.z;
    v->f[3] = q.w;
}
static void set3_rotation(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_quat q = { v->f[0], v->f[1], v->f[2], v->f[3] };
    br_node3d_set_rotation(e, br_n3(t), q);
}
static void get3_scale(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_vec3 p = br_node3d_scale(e, br_n3(t));
    v->f[0] = p.x;
    v->f[1] = p.y;
    v->f[2] = p.z;
}
static void set3_scale(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node3d_set_scale(e, br_n3(t), br_vec3_make(v->f[0], v->f[1], v->f[2]));
}
static void get3_visible(br_engine *e, br_anim_target t, br_anim_value *v)
{
    v->f[0] = br_node3d_visible(e, br_n3(t)) ? 1.0f : 0.0f;
}
static void set3_visible(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node3d_set_visible(e, br_n3(t), v->f[0] >= 0.5f);
}

/* The mesh of a node: the node itself, else its first mesh3d child (glTF node groups). */
static br_mesh3d *br_anim_mesh(br_engine *e, br_node3d n)
{
    br_mesh3d *m = br_mesh3d_get(e, n);
    for (uint32_t c_i = 0; m == NULL && c_i < br_node3d_child_count(e, n); c_i++) {
        br_node3d c = br_node3d_child_at(e, n, c_i);
        m = br_mesh3d_get(e, c);
    }
    return m;
}

static br_material3d_desc *br_anim_material(br_engine *e, br_anim_target t)
{
    br_mesh3d *m = br_anim_mesh(e, br_n3(t));
    return m != NULL ? br_material3d_params(e, m->material) : NULL;
}

/* Material colours are linear in the material, sRGB in animation data (like every data file). */
static void br_anim_color_out(br_color linear, br_anim_value *v)
{
    br_color c = br_color_to_srgb(linear);
    v->f[0] = c.r;
    v->f[1] = c.g;
    v->f[2] = c.b;
    v->f[3] = linear.a;
}
static br_color br_anim_color_in(const br_anim_value *v)
{
    br_color c = { v->f[0], v->f[1], v->f[2], v->f[3] };
    return br_color_to_linear(c);
}

static void get_mat_base_color(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        br_anim_color_out(m->base_color, v);
    }
}
static void set_mat_base_color(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        m->base_color = br_anim_color_in(v);
    }
}
static void get_mat_emissive(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        br_anim_color_out(m->emissive, v);
    }
}
static void set_mat_emissive(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        m->emissive = br_anim_color_in(v);
    }
}
static void get_mat_roughness(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    v->f[0] = m != NULL ? m->roughness : 0.0f;
}
static void set_mat_roughness(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        m->roughness = v->f[0];
    }
}
static void get_mat_metallic(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    v->f[0] = m != NULL ? m->metallic : 0.0f;
}
static void set_mat_metallic(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        m->metallic = v->f[0];
    }
}
static void get_mat_alpha(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    v->f[0] = m != NULL ? m->base_color.a : 1.0f;
}
static void set_mat_alpha(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_material3d_desc *m = br_anim_material(e, t);
    if (m != NULL) {
        m->base_color.a = v->f[0];
    }
}

static void get_morph(br_engine *e, br_anim_target t, br_anim_value *v)
{
    br_mesh3d *m = br_anim_mesh(e, br_n3(t));
    if (m != NULL) {
        SDL_memcpy(v->f, m->morph_weights, sizeof(m->morph_weights));
        v->count = m->morph_weight_count;
    }
}
/* Writes the node's mesh, or every mesh3d child (a glTF node with several primitives). */
static void set_morph(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    br_node3d n = br_n3(t);
    uint32_t count = SDL_min(v->count, (uint32_t)BR_MESH3D_MAX_MORPH_WEIGHTS);
    br_mesh3d *own = br_mesh3d_get(e, n);
    if (own != NULL) {
        SDL_memcpy(own->morph_weights, v->f, sizeof(float) * count);
        own->morph_weight_count = count;
        return;
    }
    for (uint32_t c_i = 0; c_i < br_node3d_child_count(e, n); c_i++) {
        br_node3d c = br_node3d_child_at(e, n, c_i);
        br_mesh3d *m = br_mesh3d_get(e, c);
        if (m != NULL) {
            SDL_memcpy(m->morph_weights, v->f, sizeof(float) * count);
            m->morph_weight_count = count;
        }
    }
}

static void br_anim_builtin(br_engine *e, const char *name, br_anim_target_kind kind, br_anim_value_type type,
                            br_anim_get_fn get, br_anim_set_fn set)
{
    br_anim_property_desc d = { name, kind, type, get, set };
    br_anim_property_register(e, &d);
}

void br_anim_property_register_builtins(br_engine *e)
{
    const br_anim_target_kind k2 = BR_ANIM_TARGET_2D;
    const br_anim_target_kind k3 = BR_ANIM_TARGET_3D;
    br_anim_builtin(e, "position", k2, BR_ANIM_VEC2, get_position, set_position);
    br_anim_builtin(e, "x", k2, BR_ANIM_FLOAT, get_x, set_x);
    br_anim_builtin(e, "y", k2, BR_ANIM_FLOAT, get_y, set_y);
    br_anim_builtin(e, "scale", k2, BR_ANIM_VEC2, get_scale, set_scale);
    br_anim_builtin(e, "rotation", k2, BR_ANIM_FLOAT, get_rotation, set_rotation);
    br_anim_builtin(e, "skew", k2, BR_ANIM_VEC2, get_skew, set_skew);
    br_anim_builtin(e, "pivot", k2, BR_ANIM_VEC2, get_pivot, set_pivot);
    br_anim_builtin(e, "alpha", k2, BR_ANIM_FLOAT, get_alpha, set_alpha);
    br_anim_builtin(e, "tint", k2, BR_ANIM_COLOR, get_tint, set_tint);
    br_anim_builtin(e, "visible", k2, BR_ANIM_BOOL, get_visible, set_visible);
    br_anim_builtin(e, "z_index", k2, BR_ANIM_INT, get_z_index, set_z_index);
    br_anim_builtin(e, "sprite.texture", k2, BR_ANIM_TEXTURE, get_sprite_texture, set_sprite_texture);
    br_anim_builtin(e, "sprite.frame", k2, BR_ANIM_INT, get_sprite_frame, set_sprite_frame);
    br_anim_builtin(e, "position", k3, BR_ANIM_VEC3, get3_position, set3_position);
    br_anim_builtin(e, "rotation", k3, BR_ANIM_QUAT, get3_rotation, set3_rotation);
    br_anim_builtin(e, "scale", k3, BR_ANIM_VEC3, get3_scale, set3_scale);
    br_anim_builtin(e, "visible", k3, BR_ANIM_BOOL, get3_visible, set3_visible);
    br_anim_builtin(e, "material.base_color", k3, BR_ANIM_COLOR, get_mat_base_color, set_mat_base_color);
    br_anim_builtin(e, "material.emissive", k3, BR_ANIM_COLOR, get_mat_emissive, set_mat_emissive);
    br_anim_builtin(e, "material.roughness", k3, BR_ANIM_FLOAT, get_mat_roughness, set_mat_roughness);
    br_anim_builtin(e, "material.metallic", k3, BR_ANIM_FLOAT, get_mat_metallic, set_mat_metallic);
    br_anim_builtin(e, "material.alpha", k3, BR_ANIM_FLOAT, get_mat_alpha, set_mat_alpha);
    br_anim_builtin(e, "morph_weights", k3, BR_ANIM_WEIGHTS, get_morph, set_morph);
}
