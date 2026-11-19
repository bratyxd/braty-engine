/* br_gltf_instance: the "model3d" node type, br_model_instantiate and br_view3d_from_model. */
#include "three_d/br_gltf_internal.h"

static void br_gltf_free_pose(br_gltf_pose *p)
{
    br_free(p->translation);
    br_free(p->rotation);
    br_free(p->scale);
    br_free(p->weights);
}

static bool br_gltf_alloc_pose(br_gltf_pose *p, uint32_t nodes)
{
    size_t n = nodes > 0 ? nodes : 1;
    p->translation = br_alloc(sizeof(br_vec3) * n);
    p->rotation = br_alloc(sizeof(br_quat) * n);
    p->scale = br_alloc(sizeof(br_vec3) * n);
    p->weights = br_alloc(sizeof(float) * BR_MESH3D_MAX_MORPH_WEIGHTS * n);
    return p->translation != NULL && p->rotation != NULL && p->scale != NULL && p->weights != NULL;
}

static void br_gltf_instance_free(br_engine *e, br_gltf_instance *inst)
{
    br_free(inst->nodes);
    br_free(inst->order);
    br_free(inst->prim_first);
    br_free(inst->prim_count);
    br_free(inst->prim_nodes);
    br_free(inst->joint_first);
    br_free(inst->joints);
    br_free(inst->model_world);
    br_gltf_free_pose(&inst->pose);
    br_gltf_free_pose(&inst->fade_pose);
    br_model_release(e, inst->model);
    br_free(inst);
}

/* model3d on_destroy: leave the animator pool (swap-remove), free the instance. */
static void br_gltf_model3d_destroy(br_engine *e, br_node3d n, void *data)
{
    (void)n;
    br_gltf_instance *inst = *(br_gltf_instance **)data;
    br_gltf_state *s = e->gltf;
    if (inst == NULL) {
        return;
    }
    if (s != NULL && inst->anim_index != UINT32_MAX) {
        uint32_t last = s->anims.count - 1;
        if (inst->anim_index != last) {
            br_gltf_anim *moved = br_array_at(&s->anims, last);
            moved->instance->anim_index = inst->anim_index;
        }
        br_array_remove_swap(&s->anims, inst->anim_index);
    }
    br_gltf_instance_free(e, inst);
}

void br_gltf_instance_register(br_engine *e)
{
    br_node3d_type_desc desc;
    SDL_zero(desc);
    desc.name = "model3d";
    desc.data_size = sizeof(br_gltf_instance *);
    desc.on_destroy = br_gltf_model3d_destroy;
    e->gltf->model3d_type = br_node3d_type_register(e, &desc);
}

uint32_t br_model3d_type(br_engine *e)
{
    return e->gltf != NULL ? e->gltf->model3d_type : 0;
}

br_gltf_instance *br_gltf_instance_get(br_engine *e, br_node3d n)
{
    if (e->gltf == NULL) {
        return NULL;
    }
    br_gltf_instance **data = br_node3d_data(e, n, e->gltf->model3d_type);
    return data != NULL ? *data : NULL;
}

br_model *br_model3d_model(br_engine *e, br_node3d n)
{
    br_gltf_instance *inst = br_gltf_instance_get(e, n);
    return inst != NULL ? inst->model : NULL;
}

br_node3d br_model3d_node(br_engine *e, br_node3d n, uint32_t index)
{
    br_gltf_instance *inst = br_gltf_instance_get(e, n);
    br_node3d none = { 0, 0 };
    return inst != NULL && index < inst->model->node_count ? inst->nodes[index] : none;
}

/* Model nodes sorted so parents come before children (by depth). */
static void br_gltf_order(const br_model *m, uint32_t *order, uint32_t *depth)
{
    uint32_t max_depth = 0;
    for (uint32_t i = 0; i < m->node_count; i++) {
        uint32_t d = 0;
        for (int32_t p = m->nodes[i].parent; p >= 0 && d <= m->node_count; p = m->nodes[p].parent) {
            d++;
        }
        depth[i] = d;
        max_depth = d > max_depth ? d : max_depth;
    }
    uint32_t k = 0;
    for (uint32_t d = 0; d <= max_depth; d++) {
        for (uint32_t i = 0; i < m->node_count; i++) {
            if (depth[i] == d) {
                order[k++] = i;
            }
        }
    }
}

/* Builds the 3D nodes of one model node: a group node plus mesh/camera/light children. */
static void br_gltf_build_node(br_engine *e, br_gltf_instance *inst, uint32_t i, uint32_t *prim_cursor,
                               uint32_t *joint_cursor)
{
    const br_model *m = inst->model;
    const br_model_node *mn = &m->nodes[i];
    br_node3d g = br_node3d_create_group(e);
    inst->nodes[i] = g;
    br_node3d_set_label(e, g, mn->name);
    br_node3d_set_position(e, g, mn->translation);
    br_node3d_set_rotation(e, g, mn->rotation);
    br_node3d_set_scale(e, g, mn->scale);

    inst->prim_first[i] = *prim_cursor;
    inst->joint_first[i] = UINT32_MAX;
    if (mn->mesh >= 0) {
        const br_model_mesh *mesh = &m->meshes[mn->mesh];
        const br_mat4 *joints = NULL;
        uint32_t joint_count = 0;
        if (mn->skin >= 0) {
            inst->joint_first[i] = *joint_cursor;
            joints = inst->joints + *joint_cursor;
            joint_count = m->skins[mn->skin].joint_count;
            *joint_cursor += joint_count;
        }
        for (uint32_t p = 0; p < mesh->primitive_count; p++) {
            const br_model_primitive *prim = &m->primitives[mesh->first_primitive + p];
            br_material3d mat = prim->material >= 0 ? m->materials[prim->material] : m->default_material;
            br_node3d mesh_node = br_mesh3d_create(e, prim->geometry, mat);
            br_mesh3d *md = br_mesh3d_get(e, mesh_node);
            if (md != NULL) {
                md->joint_matrices = joint_count > 0 ? joints : NULL;
                md->joint_count = joint_count;
                SDL_memcpy(md->morph_weights, mn->weights, sizeof(md->morph_weights));
                md->morph_weight_count = mn->weight_count;
            }
            br_node3d_add_child(e, g, mesh_node);
            inst->prim_nodes[(*prim_cursor)++] = mesh_node;
        }
    }
    inst->prim_count[i] = *prim_cursor - inst->prim_first[i];
    if (mn->camera >= 0) {
        br_node3d_add_child(e, g, br_camera3d_create(e, &m->cameras[mn->camera]));
    }
    if (mn->light >= 0) {
        br_node3d_add_child(e, g, br_light3d_create(e, &m->lights[mn->light]));
    }
}

br_node3d br_model_instantiate(br_engine *e, br_model *model)
{
    br_node3d none = { 0, 0 };
    br_gltf_state *s = e->gltf;
    if (s == NULL || model == NULL) {
        return none;
    }
    uint32_t nc = model->node_count;
    size_t n = nc > 0 ? nc : 1;
    uint32_t prim_total = 0;
    uint32_t joint_total = 0;
    for (uint32_t i = 0; i < nc; i++) {
        const br_model_node *mn = &model->nodes[i];
        if (mn->mesh >= 0) {
            prim_total += model->meshes[mn->mesh].primitive_count;
            joint_total += mn->skin >= 0 ? model->skins[mn->skin].joint_count : 0;
        }
    }
    br_gltf_instance *inst = br_alloc(sizeof(br_gltf_instance));
    if (inst == NULL) {
        return none;
    }
    model->refs++;
    inst->model = model;
    inst->anim_index = UINT32_MAX;
    inst->nodes = br_alloc(sizeof(br_node3d) * n);
    inst->order = br_alloc(sizeof(uint32_t) * n);
    inst->prim_first = br_alloc(sizeof(uint32_t) * n);
    inst->prim_count = br_alloc(sizeof(uint32_t) * n);
    inst->joint_first = br_alloc(sizeof(uint32_t) * n);
    inst->prim_nodes = br_alloc(sizeof(br_node3d) * (prim_total > 0 ? prim_total : 1));
    inst->joints = br_alloc(sizeof(br_mat4) * (joint_total > 0 ? joint_total : 1));
    inst->model_world = br_alloc(sizeof(br_mat4) * n);
    inst->prim_node_count = prim_total;
    inst->joint_total = joint_total;
    bool ok = inst->nodes != NULL && inst->order != NULL && inst->prim_first != NULL && inst->prim_count != NULL &&
              inst->joint_first != NULL && inst->prim_nodes != NULL && inst->joints != NULL &&
              inst->model_world != NULL && br_gltf_alloc_pose(&inst->pose, nc) &&
              br_gltf_alloc_pose(&inst->fade_pose, nc);
    br_node3d self = ok ? br_node3d_create(e, s->model3d_type) : none;
    if (!br_node3d_valid(e, self)) {
        br_log_error("model: cannot create an instance");
        br_gltf_instance_free(e, inst);
        return none;
    }
    inst->self = self;
    *(br_gltf_instance **)br_node3d_data(e, self, s->model3d_type) = inst;
    for (uint32_t i = 0; i < joint_total; i++) {
        inst->joints[i] = br_mat4_identity();
    }
    br_gltf_order(model, inst->order, inst->prim_count); /* prim_count is scratch here */

    uint32_t prim_cursor = 0;
    uint32_t joint_cursor = 0;
    for (uint32_t i = 0; i < nc; i++) {
        br_gltf_build_node(e, inst, i, &prim_cursor, &joint_cursor);
    }
    for (uint32_t i = 0; i < nc; i++) {
        int32_t p = model->nodes[i].parent;
        br_node3d_add_child(e, p >= 0 ? inst->nodes[p] : self, inst->nodes[i]);
    }
    /* parentless nodes outside the scene stay in the instance (animations may target them) but hidden */
    for (uint32_t i = 0; i < nc; i++) {
        bool in_scene = model->nodes[i].parent >= 0;
        for (uint32_t r = 0; r < model->root_count && !in_scene; r++) {
            in_scene = model->roots[r] == i;
        }
        if (!in_scene) {
            br_node3d_set_visible(e, inst->nodes[i], false);
        }
    }

    br_gltf_anim slot;
    SDL_zero(slot);
    slot.animator.clip = -1;
    slot.animator.fade_clip = -1;
    slot.animator.speed = 1.0f;
    slot.animator.loop = true;
    slot.instance = inst;
    if (br_array_push(&s->anims, &slot) != NULL) {
        inst->anim_index = s->anims.count - 1;
    }
    return self;
}

br_node br_view3d_from_model(br_engine *e, br_model *model, float width, float height)
{
    br_node view = br_view3d_create(e, width, height);
    br_node3d root = br_view3d_root(e, view);
    br_node3d inst = br_model_instantiate(e, model);
    br_node3d_add_child(e, root, inst);

    br_aabb b = model != NULL && br_aabb_is_valid(model->bounds) ? model->bounds
                                                                   : (br_aabb){ { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f } };
    br_vec3 center = br_vec3_scale(br_vec3_add(b.min, b.max), 0.5f);
    float radius = br_vec3_length(br_vec3_sub(b.max, b.min)) * 0.5f;
    radius = radius > 0.0f ? radius : 1.0f;

    br_node3d camera = br_view3d_camera(e, view);
    br_camera3d *cam = br_camera3d_get(e, camera);
    float fov = cam != NULL ? cam->fov_y : 60.0f * BR_DEG_TO_RAD;
    float distance = radius / SDL_sinf(fov * 0.5f) * 1.1f;
    if (cam != NULL) {
        cam->near_z = distance * 0.01f;
        cam->far_z = distance * 10.0f;
    }
    br_vec3 dir = br_vec3_normalize(br_vec3_make(0.4f, 0.35f, 1.0f));
    br_node3d_set_position(e, camera, br_vec3_add(center, br_vec3_scale(dir, distance)));
    br_node3d_look_at(e, camera, center, br_vec3_make(0.0f, 1.0f, 0.0f));

    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 2.0f;
    sun.cast_shadows = true;
    sun.shadow_distance = distance * 3.0f;
    sun.shadow_bias = radius * 0.01f;
    sun.shadow_normal_bias = radius * 0.01f;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_add_child(e, root, sun_node);
    br_node3d_set_position(e, sun_node, br_vec3_add(center, br_vec3_make(-radius, radius * 2.0f, radius)));
    br_node3d_look_at(e, sun_node, center, br_vec3_make(0.0f, 1.0f, 0.0f));
    return view;
}
