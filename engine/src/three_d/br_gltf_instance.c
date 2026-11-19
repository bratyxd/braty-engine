/* br_gltf_instance: the "model3d" node type, br_model_instantiate and br_view3d_from_model. */
#include "three_d/br_gltf_internal.h"

#include "assets/br_assets_internal.h" /* reference capture: an instance owns what it loads */

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

/* Builds the 3D nodes of one model node: a group node plus mesh/camera/light children. A batched
 * node gets no mesh children (its mesh is drawn by the instance's batched_mesh3d); a node with
 * EXT_mesh_gpu_instancing gets one batched_mesh3d per primitive. */
static void br_gltf_build_node(br_engine *e, br_gltf_instance *inst, uint32_t i, bool batched,
                               uint32_t *prim_cursor, uint32_t *joint_cursor)
{
    const br_model *m = inst->model;
    const br_model_node *mn = &m->nodes[i];
    br_node3d g = br_node3d_create_group(e);
    inst->nodes[i] = g;
    br_node3d_set_label(e, g, mn->name);
    br_node3d_set_position(e, g, (int32_t)i == inst->sub_root ? br_vec3_make(0.0f, 0.0f, 0.0f) : mn->translation);
    br_node3d_set_rotation(e, g, mn->rotation);
    br_node3d_set_scale(e, g, mn->scale);

    inst->prim_first[i] = *prim_cursor;
    if (mn->mesh >= 0 && !batched) {
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
            if (mn->instance_count > 0) {
                br_node3d batch = br_batched_mesh3d_create(e, prim->geometry, mat);
                for (uint32_t k = 0; k < mn->instance_count; k++) {
                    br_batched_mesh3d_add(e, batch, mn->instances[k], br_color_make(1.0f, 1.0f, 1.0f, 1.0f));
                }
                br_node3d_add_child(e, g, batch);
                continue;
            }
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

/* True if node i is `root` or below it (root -1 = every node). */
static bool br_gltf_in_subtree(const br_model *m, uint32_t i, int32_t root)
{
    if (root < 0) {
        return true;
    }
    uint32_t steps = 0;
    for (int32_t p = (int32_t)i; p >= 0 && steps <= m->node_count; p = m->nodes[p].parent) {
        if (p == root) {
            return true;
        }
        steps++;
    }
    return false;
}

/* A key of the parsed scene extras, NULL if none. */
static const cJSON *br_gltf_scene_extra(const cJSON *doc, const char *key)
{
    return cJSON_IsObject(doc) ? cJSON_GetObjectItemCaseSensitive(doc, key) : NULL;
}

/* Which built nodes are drawn by a shared batched_mesh3d: static (no animated node on the way up,
 * no skin, morph targets, GPU instancing, extras or convention suffix), in the scene, and sharing
 * their mesh with at least one other such node. flags[i] = 1 for those; uses[mesh] = their count. */
static void br_gltf_pick_batched(const br_gltf_instance *inst, const uint8_t *built, const uint8_t *in_scene,
                                 uint8_t *flags, uint32_t *uses)
{
    const br_model *m = inst->model;
    uint8_t *moving = flags; /* the same bytes: "moving" first, then the result */
    for (uint32_t i = 0; i < m->node_count; i++) {
        moving[i] = 0;
    }
    for (uint32_t c = 0; c < m->clip_count; c++) {
        for (uint32_t k = 0; k < m->clips[c].channel_count; k++) {
            uint32_t target = m->clips[c].channels[k].target;
            if (target < m->node_count) {
                moving[target] = 1;
            }
        }
    }
    for (uint32_t k = 0; k < m->node_count; k++) { /* parents before children */
        uint32_t i = inst->order[k];
        int32_t p = m->nodes[i].parent;
        if (p >= 0 && moving[p] && (int32_t)i != inst->sub_root) {
            moving[i] = 1;
        }
    }
    for (uint32_t i = 0; i < m->mesh_count; i++) {
        uses[i] = 0;
    }
    for (uint32_t i = 0; i < m->node_count; i++) {
        const br_model_node *mn = &m->nodes[i];
        bool ok = built[i] && in_scene[i] && !moving[i] && mn->mesh >= 0 && mn->skin < 0 && mn->weight_count == 0 &&
                  mn->instance_count == 0 && mn->extras == NULL && !br_gltf_name_has_convention(mn->name);
        flags[i] = ok ? 1 : 0;
        if (ok) {
            uses[mn->mesh]++;
        }
    }
    for (uint32_t i = 0; i < m->node_count; i++) {
        if (flags[i] && uses[m->nodes[i].mesh] < 2) {
            flags[i] = 0;
        }
    }
}

/* One batched_mesh3d per primitive of every mesh shared by batched nodes, under the instance
 * root, holding each node's rest matrix relative to the instance. */
static void br_gltf_build_batches(br_engine *e, br_gltf_instance *inst, const uint8_t *built, const uint8_t *batched,
                                  const uint32_t *uses)
{
    const br_model *m = inst->model;
    for (uint32_t k = 0; k < m->node_count; k++) {
        uint32_t i = inst->order[k];
        int32_t p = m->nodes[i].parent;
        br_mat4 local = built[i] ? br_node3d_local_matrix(e, inst->nodes[i]) : br_mat4_identity();
        bool chained = p >= 0 && built[p] && (int32_t)i != inst->sub_root;
        inst->model_world[i] = chained ? br_mat4_mul(inst->model_world[p], local) : local;
    }
    br_color white = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    for (uint32_t mesh = 0; mesh < m->mesh_count; mesh++) {
        if (uses[mesh] < 2) {
            continue;
        }
        const br_model_mesh *mm = &m->meshes[mesh];
        for (uint32_t p = 0; p < mm->primitive_count; p++) {
            const br_model_primitive *prim = &m->primitives[mm->first_primitive + p];
            br_material3d mat = prim->material >= 0 ? m->materials[prim->material] : m->default_material;
            br_node3d batch = br_batched_mesh3d_create(e, prim->geometry, mat);
            char label[80];
            SDL_snprintf(label, sizeof(label), "batch:%s", mm->name);
            br_node3d_set_label(e, batch, label);
            for (uint32_t i = 0; i < m->node_count; i++) {
                if (batched[i] && m->nodes[i].mesh == (int32_t)mesh) {
                    br_batched_mesh3d_add(e, batch, inst->model_world[i], white);
                }
            }
            br_node3d_add_child(e, inst->self, batch);
        }
    }
}

/* Starts the autoplay clip: options->autoplay, else the scene extras' "autoplay" (whole-scene
 * instances only: a kit part does not have the nodes the scene's clips animate). */
static void br_gltf_autoplay(br_engine *e, br_gltf_instance *inst, const br_gltf_options *options, const cJSON *extras)
{
    const br_model *m = inst->model;
    const char *name = options != NULL ? options->autoplay : NULL;
    bool first = false;
    if (name == NULL && inst->sub_root < 0) {
        const cJSON *v = br_gltf_scene_extra(extras, "autoplay");
        name = cJSON_IsString(v) ? v->valuestring : NULL;
        first = cJSON_IsTrue(v);
    }
    if (m->clip_count == 0 || (!first && (name == NULL || name[0] == '\0'))) {
        return;
    }
    if (first) {
        br_animator3d_play_index(e, inst->self, 0);
    } else if (!br_animator3d_play(e, inst->self, name)) {
        br_log_warn("glTF '%s': autoplay clip '%s' not found", m->path != NULL ? m->path : "", name);
    }
}

br_node3d br_model_instantiate(br_engine *e, br_model *model)
{
    return br_model_instantiate_ex(e, model, NULL);
}

br_node3d br_model_instantiate_ex(br_engine *e, br_model *model, const br_gltf_options *options)
{
    br_node3d none = { 0, 0 };
    br_gltf_state *s = e->gltf;
    if (s == NULL || model == NULL) {
        return none;
    }
    int32_t sub_root = -1;
    if (options != NULL && options->node != NULL && options->node[0] != '\0') {
        sub_root = br_model_find_node(model, options->node);
        if (sub_root < 0) {
            br_log_error("glTF '%s': no node named '%s'", model->path != NULL ? model->path : "", options->node);
            return none;
        }
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
    uint8_t *scratch = br_alloc(n * 3);
    size_t mesh_n = model->mesh_count > 0 ? model->mesh_count : 1;
    uint32_t *uses = br_alloc(sizeof(uint32_t) * mesh_n);
    if (inst == NULL || scratch == NULL || uses == NULL) {
        br_free(inst);
        br_free(scratch);
        br_free(uses);
        return none;
    }
    model->refs++;
    inst->model = model;
    inst->anim_index = UINT32_MAX;
    inst->sub_root = sub_root;
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
        br_free(scratch);
        br_free(uses);
        return none;
    }
    inst->self = self;
    *(br_gltf_instance **)br_node3d_data(e, self, s->model3d_type) = inst;
    for (uint32_t i = 0; i < joint_total; i++) {
        inst->joints[i] = br_mat4_identity();
    }
    br_gltf_order(model, inst->order, inst->prim_count); /* prim_count is scratch here */

    /* which nodes exist (a sub-asset builds one subtree) and which are in the default scene */
    uint8_t *built = scratch;
    uint8_t *in_scene = scratch + n;
    uint8_t *batched = scratch + 2 * n;
    for (uint32_t i = 0; i < nc; i++) {
        built[i] = br_gltf_in_subtree(model, i, sub_root) ? 1 : 0;
        bool listed = model->nodes[i].parent >= 0 || sub_root >= 0;
        for (uint32_t r = 0; r < model->root_count && !listed; r++) {
            listed = model->roots[r] == i;
        }
        in_scene[i] = listed ? 1 : 0;
        inst->joint_first[i] = UINT32_MAX;
        inst->prim_first[i] = 0;
    }
    cJSON *extras = model->scene_extras != NULL ? cJSON_Parse(model->scene_extras) : NULL;
    bool batching = !(options != NULL && options->no_batching) && !cJSON_IsFalse(br_gltf_scene_extra(extras, "batch"));
    SDL_memset(batched, 0, n);
    SDL_memset(uses, 0, sizeof(uint32_t) * mesh_n);
    if (batching) {
        br_gltf_pick_batched(inst, built, in_scene, batched, uses);
    }

    uint32_t prim_cursor = 0;
    uint32_t joint_cursor = 0;
    for (uint32_t i = 0; i < nc; i++) {
        if (built[i]) {
            br_gltf_build_node(e, inst, i, batched[i] != 0, &prim_cursor, &joint_cursor);
        } else {
            inst->prim_count[i] = 0;
        }
    }
    for (uint32_t i = 0; i < nc; i++) {
        int32_t p = model->nodes[i].parent;
        if (built[i]) {
            bool chained = p >= 0 && built[p] && (int32_t)i != sub_root;
            br_node3d_add_child(e, chained ? inst->nodes[p] : self, inst->nodes[i]);
        }
    }
    /* parentless nodes outside the scene stay in the instance (animations may target them) but hidden */
    for (uint32_t i = 0; i < nc; i++) {
        if (built[i] && !in_scene[i]) {
            br_node3d_set_visible(e, inst->nodes[i], false);
        }
    }
    if (batching) {
        br_gltf_build_batches(e, inst, built, batched, uses);
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
    br_data_gltf_apply_extras(e, inst);
    br_gltf_autoplay(e, inst, options, extras);
    cJSON_Delete(extras);
    br_free(scratch);
    br_free(uses);
    return self;
}

br_node3d br_model3d_camera(br_engine *e, br_node3d n, const char *name)
{
    br_gltf_instance *inst = br_gltf_instance_get(e, n);
    br_node3d none = { 0, 0 };
    for (uint32_t i = 0; inst != NULL && i < inst->model->node_count; i++) {
        const br_model_node *mn = &inst->model->nodes[i];
        if (mn->camera < 0 || !br_node3d_valid(e, inst->nodes[i]) || (name != NULL && SDL_strcmp(mn->name, name) != 0)) {
            continue;
        }
        for (br_node3d c = br_node3d_first_child(e, inst->nodes[i]); br_node3d_valid(e, c);
             c = br_node3d_next_sibling(e, c)) {
            if (br_camera3d_get(e, c) != NULL) {
                return c;
            }
        }
    }
    return none;
}

br_node3d br_gltf_instantiate(br_engine *e, const char *path, br_node3d parent, const br_gltf_options *options)
{
    br_node3d none = { 0, 0 };
    if (path == NULL || e->gltf == NULL) {
        return none;
    }
    /* "kit.glb#Tree": the asset is the whole file, the instance one node of it */
    char file[512];
    const char *hash = SDL_strchr(path, '#');
    size_t len = hash != NULL ? (size_t)(hash - path) : SDL_strlen(path);
    if (len >= sizeof(file)) {
        br_log_error("glTF path too long: '%s'", path);
        return none;
    }
    SDL_memcpy(file, path, len);
    file[len] = '\0';
    br_gltf_options o;
    SDL_zero(o);
    if (options != NULL) {
        o = *options;
    }
    if (hash != NULL && o.node == NULL) {
        o.node = hash + 1;
    }
    /* outside any load the instance owns its reference; inside one, that load captures it */
    bool own = e->assets->capture_depth == 0;
    uint32_t mark = own ? br_assets_capture_begin(e) : 0;
    br_model *model = br_model_load(e, file);
    br_node3d n = model != NULL ? br_model_instantiate_ex(e, model, &o) : none;
    if (!br_node3d_valid(e, n)) {
        if (own) {
            br_assets_capture_end(e, mark, NULL);
        } else if (model != NULL) {
            br_assets_unload(e, file);
        }
        return none;
    }
    if (own) {
        char name[sizeof(file) + 8];
        SDL_snprintf(name, sizeof(name), "gltf:%s", file);
        br_asset_group *group = br_asset_group_create(e, name);
        br_assets_capture_end(e, mark, group);
        br_gltf_state *s = e->gltf;
        if (n.index >= s->owned.count && !br_array_resize(&s->owned, n.index + 1)) {
            br_asset_group_destroy(group); /* out of memory: give the reference back now */
        } else {
            br_gltf_owned *slot = br_array_at(&s->owned, n.index);
            slot->generation = n.generation;
            slot->group = group;
        }
    }
    if (br_node3d_valid(e, parent)) {
        br_node3d_add_child(e, parent, n);
    }
    return n;
}

/* World bounds of the meshes under n (empty box if none). */
static br_aabb br_gltf_subtree_bounds(br_engine *e, br_node3d n)
{
    br_aabb box = br_aabb_empty();
    br_aabb local;
    if (br_node3d_visible(e, n) && br_node3d_local_bounds(e, n, &local) && br_aabb_is_valid(local)) {
        box = br_aabb_transform(local, br_node3d_world_matrix(e, n));
    }
    for (br_node3d c = br_node3d_first_child(e, n); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        if (br_node3d_visible(e, c)) {
            box = br_aabb_union(box, br_gltf_subtree_bounds(e, c));
        }
    }
    return box;
}

void br_gltf_frame_view(br_engine *e, br_node view, br_node3d content, const br_aabb *bounds, bool camera, bool sun)
{
    br_aabb b = bounds != NULL ? *bounds : br_gltf_subtree_bounds(e, content);
    if (!br_aabb_is_valid(b)) {
        b.min = br_vec3_make(-1.0f, -1.0f, -1.0f);
        b.max = br_vec3_make(1.0f, 1.0f, 1.0f);
    }
    br_vec3 center = br_vec3_scale(br_vec3_add(b.min, b.max), 0.5f);
    float radius = br_vec3_length(br_vec3_sub(b.max, b.min)) * 0.5f;
    radius = radius > 0.0f ? radius : 1.0f;

    br_node3d cam_node = br_view3d_camera(e, view);
    br_camera3d *cam = br_camera3d_get(e, cam_node);
    float fov = cam != NULL ? cam->fov_y : 60.0f * BR_DEG_TO_RAD;
    float distance = radius / SDL_sinf(fov * 0.5f) * 1.1f;
    if (camera) {
        if (cam != NULL) {
            cam->near_z = distance * 0.01f;
            cam->far_z = distance * 10.0f;
        }
        br_vec3 dir = br_vec3_normalize(br_vec3_make(0.4f, 0.35f, 1.0f));
        br_node3d_set_position(e, cam_node, br_vec3_add(center, br_vec3_scale(dir, distance)));
        br_node3d_look_at(e, cam_node, center, br_vec3_make(0.0f, 1.0f, 0.0f));
    }
    if (sun) {
        br_light3d light = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
        light.intensity = 2.0f;
        light.cast_shadows = true;
        light.shadow_distance = distance * 3.0f;
        light.shadow_bias = radius * 0.01f;
        light.shadow_normal_bias = radius * 0.01f;
        br_node3d sun_node = br_light3d_create(e, &light);
        br_node3d_set_label(e, sun_node, "default_sun");
        br_node3d_add_child(e, br_view3d_root(e, view), sun_node);
        br_node3d_set_position(e, sun_node, br_vec3_add(center, br_vec3_make(-radius, radius * 2.0f, radius)));
        br_node3d_look_at(e, sun_node, center, br_vec3_make(0.0f, 1.0f, 0.0f));
    }
}

br_node br_view3d_from_model(br_engine *e, br_model *model, float width, float height)
{
    br_node view = br_view3d_create(e, width, height);
    br_node3d inst = br_model_instantiate(e, model);
    br_node3d_add_child(e, br_view3d_root(e, view), inst);
    const br_aabb *bounds = model != NULL && br_aabb_is_valid(model->bounds) ? &model->bounds : NULL;
    br_gltf_frame_view(e, view, inst, bounds, true, true);
    return view;
}
