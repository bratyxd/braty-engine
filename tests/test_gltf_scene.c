/* test_gltf_scene - glTF as scene content, headless, on tests/data/level.gltf (written by
 * tests/data/make_level_gltf.py): extras -> 3D components (every field type, nested and flat
 * forms, node references by name, bad values reported and skipped), name conventions, automatic
 * batching and EXT_mesh_gpu_instancing, cameras / lights / autoplay, material extensions,
 * sub-asset addressing ("level.gltf#Windmill"), a .gltf as a br_scene (unload returns every count
 * to its baseline), as a prefab (N instances share one parsed model) and as view3d JSON content. */
#include "braty/braty.h"
#include "br_test.h"

typedef struct spin {
    float speed;
} spin;

typedef struct pickup {
    int32_t value;
    char    kind[BR_FIELD_STRING_SIZE];
} pickup;

typedef struct spawn_point {
    bool used;
} spawn_point;

typedef struct all_fields {
    bool      b;
    int32_t   i;
    float     f;
    br_vec2   v2;
    br_vec3   v3;
    br_color  c;
    char      s[BR_FIELD_STRING_SIZE];
    char      a[BR_FIELD_STRING_SIZE];
    br_node3d n;
} all_fields;

static uint32_t g_spin;
static uint32_t g_pickup;
static uint32_t g_spawn;
static uint32_t g_all;

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

static void register_components(br_engine *e)
{
    static const br_field_desc spin_fields[] = { { "speed", BR_FIELD_FLOAT, offsetof(spin, speed) } };
    static const br_field_desc pickup_fields[] = {
        { "value", BR_FIELD_INT, offsetof(pickup, value) },
        { "kind", BR_FIELD_STRING, offsetof(pickup, kind) },
    };
    static const br_field_desc spawn_fields[] = { { "used", BR_FIELD_BOOL, offsetof(spawn_point, used) } };
    static const br_field_desc all[] = {
        { "b", BR_FIELD_BOOL, offsetof(all_fields, b) },    { "i", BR_FIELD_INT, offsetof(all_fields, i) },
        { "f", BR_FIELD_FLOAT, offsetof(all_fields, f) },   { "v2", BR_FIELD_VEC2, offsetof(all_fields, v2) },
        { "v3", BR_FIELD_VEC3, offsetof(all_fields, v3) },  { "c", BR_FIELD_COLOR, offsetof(all_fields, c) },
        { "s", BR_FIELD_STRING, offsetof(all_fields, s) },  { "a", BR_FIELD_ASSET, offsetof(all_fields, a) },
        { "n", BR_FIELD_NODE, offsetof(all_fields, n) },
    };
    br_component_desc d1 = { "spin", sizeof(spin), spin_fields, BR_ARRAY_COUNT(spin_fields) };
    br_component_desc d2 = { "pickup", sizeof(pickup), pickup_fields, BR_ARRAY_COUNT(pickup_fields) };
    br_component_desc d3 = { "spawn_point", sizeof(spawn_point), spawn_fields, BR_ARRAY_COUNT(spawn_fields) };
    br_component_desc d4 = { "all", sizeof(all_fields), all, BR_ARRAY_COUNT(all) };
    g_spin = br_component_register(e, &d1);
    g_pickup = br_component_register(e, &d2);
    g_spawn = br_component_register(e, &d3);
    g_all = br_component_register(e, &d4);
    BR_CHECK(g_spin != 0 && g_pickup != 0 && g_spawn != 0 && g_all != 0);
}

static br_node3d find(br_engine *e, br_node3d root, const char *label)
{
    br_node3d n = br_node3d_find_by_label(e, root, label);
    BR_CHECK(br_node3d_valid(e, n));
    return n;
}

/* Counts children of n of a 3D type (and their instances for batched meshes). */
static uint32_t count_children(br_engine *e, br_node3d n, uint32_t type, uint32_t *instances)
{
    uint32_t count = 0;
    for (br_node3d c = br_node3d_first_child(e, n); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        if (br_node3d_type_of(e, c) == type) {
            count++;
            uint32_t k = 0;
            if (instances != NULL && br_batched_mesh3d_instances(e, c, &k) != NULL) {
                *instances += k;
            }
        }
    }
    return count;
}

/* Batched instances under n's direct children that are collapsed (hidden: zero scale). */
static uint32_t count_collapsed(br_engine *e, br_node3d n)
{
    uint32_t count = 0;
    for (br_node3d c = br_node3d_first_child(e, n); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        uint32_t k = 0;
        br_instance3d *inst = br_batched_mesh3d_instances(e, c, &k);
        for (uint32_t i = 0; inst != NULL && i < k; i++) {
            const float *m = inst[i].model.m;
            count += m[0] == 0.0f && m[5] == 0.0f && m[10] == 0.0f ? 1u : 0u;
        }
    }
    return count;
}

/* Mesh3d nodes anywhere under n. */
static uint32_t count_meshes(br_engine *e, br_node3d n)
{
    uint32_t count = br_node3d_type_of(e, n) == br_mesh3d_type(e) ? 1u : 0u;
    for (br_node3d c = br_node3d_first_child(e, n); br_node3d_valid(e, c); c = br_node3d_next_sibling(e, c)) {
        count += count_meshes(e, c);
    }
    return count;
}

static void test_extras(br_engine *e, br_node3d inst)
{
    /* nested "components" object */
    br_node3d coin1 = find(e, inst, "Coin_01");
    spin *s = br_component3d_get(e, coin1, g_spin);
    pickup *p = br_component3d_get(e, coin1, g_pickup);
    BR_CHECK(s != NULL && near_eq(s->speed, 3.0f));
    BR_CHECK(p != NULL && p->value == 25 && SDL_strcmp(p->kind, "big") == 0);
    /* flat Blender custom properties */
    br_node3d coin2 = find(e, inst, "Coin_02");
    s = br_component3d_get(e, coin2, g_spin);
    p = br_component3d_get(e, coin2, g_pickup);
    BR_CHECK(s != NULL && near_eq(s->speed, 2.0f));
    BR_CHECK(p != NULL && p->value == 10 && SDL_strcmp(p->kind, "small") == 0);
    BR_CHECK(br_component3d_get(e, find(e, inst, "SpawnPoint"), g_spawn) != NULL);

    /* every field type, nested */
    all_fields *a = br_component3d_get(e, find(e, inst, "Props"), g_all);
    BR_CHECK(a != NULL);
    if (a != NULL) {
        BR_CHECK(a->b && a->i == 7 && near_eq(a->f, 1.5f));
        BR_CHECK(near_eq(a->v2.x, 1.0f) && near_eq(a->v2.y, 2.0f));
        BR_CHECK(near_eq(a->v3.x, 1.0f) && near_eq(a->v3.y, 2.0f) && near_eq(a->v3.z, 3.0f));
        BR_CHECK(near_eq(a->c.r, 1.0f) && near_eq(a->c.g, 0.0f) && near_eq(a->c.a, 1.0f));
        BR_CHECK(SDL_strcmp(a->s, "hello") == 0 && SDL_strcmp(a->a, "tiny.gltf") == 0);
        br_node3d tree3 = find(e, inst, "Tree_03");
        BR_CHECK(a->n.index == tree3.index && a->n.generation == tree3.generation);
    }
    /* every field type, flat (bool as 1, colour as [r, g, b]) */
    a = br_component3d_get(e, find(e, inst, "FlatProps"), g_all);
    BR_CHECK(a != NULL);
    if (a != NULL) {
        BR_CHECK(a->b && a->i == 3 && near_eq(a->f, 0.25f));
        BR_CHECK(near_eq(a->v2.x, 5.0f) && near_eq(a->v3.z, 9.0f) && near_eq(a->c.g, 1.0f) && near_eq(a->c.r, 0.0f));
        BR_CHECK(SDL_strcmp(a->s, "flat") == 0 && SDL_strcmp(a->a, "level.gltf") == 0);
        BR_CHECK(a->n.index == find(e, inst, "Windmill").index);
    }
    /* bad values: skipped (zero), the rest of the node still applied */
    a = br_component3d_get(e, find(e, inst, "BadValues"), g_all);
    BR_CHECK(a != NULL && a->i == 0 && near_eq(a->v3.y, 0.0f) && !br_node3d_valid(e, a->n));
    BR_CHECK(br_component3d_get(e, find(e, inst, "BadValues"), g_spin) == NULL);

    /* iteration: the four coins */
    uint32_t coins = 0;
    for (uint32_t i = 0; i < br_component3d_count(e, g_spin); i++) {
        br_node3d owner = br_component3d_owner_at(e, g_spin, i);
        const spin *row = br_component3d_at(e, g_spin, i);
        if (br_node3d_valid(e, owner) && SDL_strncmp(br_node3d_label(e, owner), "Coin_", 5) == 0 && row->speed > 0.0f) {
            coins++;
        }
    }
    BR_CHECK(coins == 4);

    /* conventions: colliders, trigger, no render, layer, shadows, visibility */
    uint32_t col = br_gltf_collider_component(e);
    br_gltf_collider *ground = br_component3d_get(e, find(e, inst, "Ground-col"), col);
    BR_CHECK(ground != NULL && !ground->trigger && near_eq(ground->size.x, 24.0f) && near_eq(ground->size.y, 0.4f) &&
             near_eq(ground->center.y, -0.2f));
    BR_CHECK(br_node3d_visible(e, br_node3d_first_child(e, find(e, inst, "Ground-col"))));
    br_node3d gate = find(e, inst, "Gate-trigger");
    br_gltf_collider *trigger = br_component3d_get(e, gate, col);
    BR_CHECK(trigger != NULL && trigger->trigger && near_eq(trigger->size.y, 2.0f));
    BR_CHECK(!br_node3d_visible(e, br_node3d_first_child(e, gate)));
    br_node3d helper = find(e, inst, "Helper-norender");
    BR_CHECK(br_component3d_get(e, helper, col) == NULL && !br_node3d_visible(e, br_node3d_first_child(e, helper)));
    br_node3d layered = find(e, inst, "Layered");
    br_node3d layered_mesh = br_node3d_first_child(e, layered);
    BR_CHECK(br_node3d_layer(e, layered) == 2 && br_node3d_layer(e, layered_mesh) == 2);
    BR_CHECK(br_mesh3d_get(e, layered_mesh) != NULL && !br_mesh3d_get(e, layered_mesh)->cast_shadows);
    BR_CHECK(!br_node3d_visible(e, find(e, inst, "Hidden")));
    br_light3d *sun = br_light3d_get(e, br_node3d_first_child(e, find(e, inst, "Sun")));
    BR_CHECK(sun != NULL && sun->kind == BR_LIGHT3D_DIRECTIONAL && sun->cast_shadows);
}

static void test_batching(br_engine *e, br_model *model, br_node3d inst)
{
    /* 8 trees x 2 primitives -> 2 batched meshes of 8 instances under the instance root */
    uint32_t instances = 0;
    BR_CHECK(count_children(e, inst, br_batched_mesh3d_type(e), &instances) == 2 && instances == 16);
    BR_CHECK(br_node3d_valid(e, br_node3d_find_by_label(e, inst, "batch:TreeMesh")));
    BR_CHECK(br_node3d_first_child(e, find(e, inst, "Tree_01")).index == 0); /* group stays, mesh moved */
    /* nodes with extras / "batch": false / animated parents keep their own mesh3d */
    BR_CHECK(br_mesh3d_get(e, br_node3d_first_child(e, find(e, inst, "Loose_01"))) != NULL);
    BR_CHECK(br_mesh3d_get(e, br_node3d_first_child(e, find(e, inst, "Blades"))) != NULL);
    /* EXT_mesh_gpu_instancing: one batched mesh with the 6 rocks */
    instances = 0;
    BR_CHECK(count_children(e, find(e, inst, "Rocks"), br_batched_mesh3d_type(e), &instances) == 1 && instances == 6);

    br_gltf_options options;
    SDL_zero(options);
    options.no_batching = true;
    br_node3d plain = br_model_instantiate_ex(e, model, &options);
    uint32_t meshes_batched = count_meshes(e, inst);
    uint32_t meshes_plain = count_meshes(e, plain);
    br_log_info("test_gltf_scene: mesh3d nodes %u -> %u with batching (+2 batched_mesh3d)", meshes_plain, meshes_batched);
    BR_CHECK(meshes_plain == meshes_batched + 16);
    BR_CHECK(count_children(e, plain, br_batched_mesh3d_type(e), NULL) == 0);
    br_node3d_destroy(e, plain);

    /* R6: hiding or destroying a batched node's group hides its copy in the batch (one per
     * primitive: the tree has two) */
    br_node3d copy = br_model_instantiate(e, model);
    br_node3d_set_visible(e, find(e, copy, "Tree_01"), false);
    br_node3d_destroy(e, find(e, copy, "Tree_02"));
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(count_collapsed(e, copy) == 4);
    br_node3d_set_visible(e, find(e, copy, "Tree_01"), true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(count_collapsed(e, copy) == 2);
    br_node3d_destroy(e, copy);
}

static void test_import(br_engine *e, br_model *model, br_node3d inst)
{
    /* cameras: the first one, and one by node name */
    br_camera3d *main_cam = br_camera3d_get(e, br_model3d_camera(e, inst, NULL));
    BR_CHECK(main_cam != NULL && main_cam->projection == BR_PROJECTION_PERSPECTIVE && near_eq(main_cam->fov_y, 0.75f));
    br_camera3d *top = br_camera3d_get(e, br_model3d_camera(e, inst, "TopCam"));
    BR_CHECK(top != NULL && top->projection == BR_PROJECTION_ORTHOGRAPHIC);
    BR_CHECK(br_model3d_camera(e, inst, "Nope").index == 0);
    /* lights */
    BR_CHECK(model->light_count == 2);
    br_light3d *lamp = br_light3d_get(e, br_node3d_first_child(e, find(e, inst, "Lamp")));
    BR_CHECK(lamp != NULL && lamp->kind == BR_LIGHT3D_POINT && near_eq(lamp->range, 8.0f) && !lamp->cast_shadows);
    /* the scene extras' autoplay started "Spin" on the instance's animator */
    br_animator3d *a = br_animator3d_get(e, inst);
    BR_CHECK(a != NULL && a->clip == br_model_find_clip(model, "Spin"));
    br_quat before = br_node3d_rotation(e, find(e, inst, "Blades"));
    br_engine_step(e, 0.5f);
    br_quat after = br_node3d_rotation(e, find(e, inst, "Blades"));
    BR_CHECK(SDL_fabsf(br_quat_dot(before, after)) < 0.99f);
    /* material extensions */
    for (uint32_t i = 0; i < model->material_count; i++) {
        br_material3d_desc *d = br_material3d_params(e, model->materials[i]);
        if (SDL_strcmp(model->material_names[i], "Glass") == 0) {
            BR_CHECK(d->alpha_mode == BR_ALPHA_BLEND && near_eq(d->base_color.a, 1.0f - 0.7f * 0.8f));
        } else if (SDL_strcmp(model->material_names[i], "Sail") == 0) {
            BR_CHECK(d->model == BR_MATERIAL3D_FLAT);
        } else if (SDL_strcmp(model->material_names[i], "Gold") == 0) {
            BR_CHECK(near_eq(d->emissive_strength, 0.6f));
        }
    }
}

static void test_sub_assets(br_engine *e, br_model *model)
{
    uint32_t refs = br_assets_ref_count(e, "level.gltf");
    uint32_t nodes3d = br_node3d_count(e);
    br_node3d mill = br_gltf_instantiate(e, "level.gltf#Windmill", (br_node3d){ 0, 0 }, NULL);
    BR_CHECK(br_model3d_model(e, mill) == model && br_assets_ref_count(e, "level.gltf") == refs + 1);
    br_node3d root = br_node3d_first_child(e, mill);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, root), "Windmill") == 0 && br_node3d_next_sibling(e, root).index == 0);
    br_vec3 at = br_node3d_position(e, root);
    BR_CHECK(near_eq(at.x, 0.0f) && near_eq(at.z, 0.0f)); /* placed where the caller puts the instance */
    BR_CHECK(br_node3d_valid(e, br_node3d_find_by_label(e, mill, "Blades")));
    BR_CHECK(br_node3d_find_by_label(e, mill, "Tree_01").index == 0);
    br_node3d_destroy(e, mill); /* outside a frame: freed now, with the reference it owned */
    BR_CHECK(br_assets_ref_count(e, "level.gltf") == refs && br_node3d_count(e) == nodes3d);
    /* a missing node name fails cleanly */
    BR_CHECK(br_gltf_instantiate(e, "level.gltf#Nope", (br_node3d){ 0, 0 }, NULL).index == 0);
    BR_CHECK(br_assets_ref_count(e, "level.gltf") == refs && br_node3d_count(e) == nodes3d);
    /* br_assets: the fragment names the same cached file */
    BR_CHECK(br_assets_load(e, "level.gltf#Tree_01") == model && br_assets_ref_count(e, "level.gltf") == refs + 1);
    br_assets_unload(e, "level.gltf#Tree_01");
    BR_CHECK(br_assets_ref_count(e, "level.gltf") == refs);
}

static void test_level(br_engine *e)
{
    uint32_t errors = br_scene_file_error_count(e);
    br_node3d inst = br_gltf_instantiate(e, "level.gltf", (br_node3d){ 0, 0 }, NULL);
    BR_CHECK(br_node3d_valid(e, inst));
    /* BadValues: all.i, all.v3, all.n, all.nosuch, "spin": 5 (unknown "not_registered" only warns) */
    BR_CHECK(br_scene_file_error_count(e) == errors + 5);
    BR_CHECK(SDL_strstr(br_scene_file_last_error(e), "level.gltf: node 'BadValues'.extras.") != NULL);
    br_model *model = br_model3d_model(e, inst);
    BR_CHECK(model != NULL && br_assets_ref_count(e, "level.gltf") == 1);
    if (model == NULL) {
        return;
    }
    test_extras(e, inst);
    test_batching(e, model, inst);
    test_import(e, model, inst);
    test_sub_assets(e, model);
    br_node3d_destroy(e, inst);
    BR_CHECK(br_assets_get(e, "level.gltf") == NULL && br_component3d_count(e, g_spin) == 0);
}

typedef struct counts {
    uint32_t nodes;
    uint32_t nodes3d;
    uint32_t refs;
    uint32_t spins;
    uint32_t animators;
} counts;

static counts take_counts(br_engine *e)
{
    counts c = { br_scene_node_count(e), br_node3d_count(e), br_assets_ref_count(e, "level.gltf"),
                 br_component3d_count(e, g_spin), br_animator3d_count(e) };
    return c;
}

static bool same_counts(counts a, counts b)
{
    return a.nodes == b.nodes && a.nodes3d == b.nodes3d && a.refs == b.refs && a.spins == b.spins &&
           a.animators == b.animators;
}

static void test_as_scene(br_engine *e)
{
    br_engine_step(e, 1.0f / 60.0f);
    counts base = take_counts(e);
    br_scene s = br_scene_load(e, "level.gltf", BR_SCENE_SINGLE);
    BR_CHECK(br_scene_is_loaded(e, s) && SDL_strcmp(br_scene_name(e, s), "level") == 0);
    BR_CHECK(br_asset_group_count(br_scene_assets(e, s)) == 1 && br_assets_ref_count(e, "level.gltf") == 1);
    br_node3d root = br_scene_root_3d(e, s);
    br_node view = br_node_find_by_label(e, br_scene_root_2d(e, s), "level", true);
    BR_CHECK(br_node_type_of(e, view) == br_view3d_type(e) && br_view3d_root(e, view).index == root.index);
    br_node3d coin = find(e, root, "Coin_01");
    BR_CHECK(br_component3d_get(e, coin, g_spin) != NULL);
    /* the file's first camera is the view's camera, following the view's aspect */
    br_node3d cam = br_view3d_camera(e, view);
    br_camera3d *cd = br_camera3d_get(e, cam);
    BR_CHECK(cd != NULL && near_eq(cd->fov_y, 0.75f) && cd->aspect == 0.0f);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, br_node3d_parent(e, cam)), "MainCam") == 0);
    BR_CHECK(br_node3d_find_by_label(e, root, "default_sun").index == 0); /* the file has lights */
    br_engine_step(e, 1.0f / 60.0f);
    br_scene_unload(e, s);
    br_engine_step(e, 1.0f / 60.0f); /* nodes flushed, then the group released */
    br_engine_step(e, 1.0f / 60.0f);
    counts after = take_counts(e);
    BR_CHECK(same_counts(base, after));
    BR_CHECK(br_assets_get(e, "level.gltf") == NULL);

    /* load options: another camera, no batching, no autoplay */
    br_gltf_options options;
    SDL_zero(options);
    options.camera = "TopCam";
    options.no_batching = true;
    options.autoplay = "";
    br_gltf_set_load_options(e, &options);
    s = br_scene_load(e, "level.gltf", BR_SCENE_SINGLE);
    br_gltf_set_load_options(e, NULL);
    view = br_node_find_by_label(e, br_scene_root_2d(e, s), "level", true);
    cd = br_camera3d_get(e, br_view3d_camera(e, view));
    BR_CHECK(cd != NULL && cd->projection == BR_PROJECTION_ORTHOGRAPHIC);
    br_node3d inst = br_node3d_first_child(e, br_scene_root_3d(e, s));
    while (br_node3d_valid(e, inst) && br_model3d_model(e, inst) == NULL) {
        inst = br_node3d_next_sibling(e, inst);
    }
    BR_CHECK(count_children(e, inst, br_batched_mesh3d_type(e), NULL) == 0);
    BR_CHECK(br_animator3d_get(e, inst) != NULL && br_animator3d_get(e, inst)->clip == -1);
    br_scene_unload(e, s);
    br_engine_step(e, 1.0f / 60.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(same_counts(base, take_counts(e)));
}

static void test_as_prefab(br_engine *e)
{
    counts base = take_counts(e);
    br_node views[3];
    br_model *first = NULL;
    for (int i = 0; i < 3; i++) {
        views[i] = br_prefab_instantiate_at(e, "level.gltf#Coin_02", br_scene_root(e), br_vec2_make(100.0f * i, 0.0f));
        BR_CHECK(br_node_type_of(e, views[i]) == br_view3d_type(e));
        br_node3d inst = br_node3d_first_child(e, br_view3d_root(e, views[i]));
        while (br_node3d_valid(e, inst) && br_model3d_model(e, inst) == NULL) {
            inst = br_node3d_next_sibling(e, inst);
        }
        br_model *m = br_model3d_model(e, inst);
        BR_CHECK(m != NULL && (first == NULL || m == first)); /* one parsed model for every instance */
        first = m;
        br_node3d coin = find(e, inst, "Coin_02");
        BR_CHECK(br_component3d_get(e, coin, g_spin) != NULL);
        /* no camera in a coin: framed; no light: a default sun */
        BR_CHECK(br_node3d_valid(e, br_node3d_find_by_label(e, br_view3d_root(e, views[i]), "default_sun")));
    }
    BR_CHECK(br_assets_ref_count(e, "level.gltf") == 3);
    /* 3D parent: br_gltf_instantiate shares the model too */
    br_node3d holder = br_node3d_create_group(e);
    br_node3d a = br_gltf_instantiate(e, "level.gltf#Tree_02", holder, NULL);
    br_node3d b = br_gltf_instantiate(e, "level.gltf#Tree_05", holder, NULL);
    BR_CHECK(br_model3d_model(e, a) == first && br_model3d_model(e, b) == first);
    BR_CHECK(br_node3d_child_count(e, holder) == 2 && br_assets_ref_count(e, "level.gltf") == 5);
    br_node3d_destroy(e, holder);
    for (int i = 0; i < 3; i++) {
        br_node_destroy(e, views[i]);
    }
    br_engine_step(e, 1.0f / 60.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(same_counts(base, take_counts(e)));
    BR_CHECK(br_assets_get(e, "level.gltf") == NULL);
}

/* view3d scene JSON: a kit part, 3D components with a forward node reference, a named camera. */
static void test_scene_json(br_engine *e)
{
    const char *json = "{ \"type\": \"view3d\", \"label\": \"v\", \"scene\": ["
                       "  { \"type\": \"group\", \"label\": \"aim\", \"components\": { \"all\": { \"i\": 4, \"n\": \"coin\" } } },"
                       "  { \"type\": \"gltf\", \"label\": \"coin\", \"source\": \"level.gltf#Coin_03\", \"position\": [1, 0, 0],"
                       "    \"components\": { \"spin\": { \"speed\": 9 } } },"
                       "  { \"type\": \"gltf\", \"label\": \"lvl\", \"source\": \"level.gltf\", \"camera\": \"TopCam\", \"batch\": false } ] }";
    uint32_t errors = br_scene_file_error_count(e);
    br_node view = br_scene_file_load_json(e, json, SDL_strlen(json), "kit.scene.json", br_scene_root(e));
    BR_CHECK(br_scene_file_error_count(e) == errors + 5); /* the level's BadValues node again */
    br_node3d root = br_view3d_root(e, view);
    br_node3d coin = find(e, root, "coin");
    spin *s = br_component3d_get(e, coin, g_spin);
    BR_CHECK(s != NULL && near_eq(s->speed, 9.0f));
    BR_CHECK(br_component3d_get(e, find(e, coin, "Coin_03"), g_spin) != NULL); /* its extras too */
    all_fields *a = br_component3d_get(e, find(e, root, "aim"), g_all);
    BR_CHECK(a != NULL && a->i == 4 && a->n.index == coin.index && a->n.generation == coin.generation);
    br_camera3d *cam = br_camera3d_get(e, br_view3d_camera(e, view));
    BR_CHECK(cam != NULL && cam->projection == BR_PROJECTION_ORTHOGRAPHIC);
    BR_CHECK(count_children(e, find(e, root, "lvl"), br_batched_mesh3d_type(e), NULL) == 0);
    /* unknown components in scene JSON are errors (unlike glTF extras) */
    const char *bad = "{ \"type\": \"view3d\", \"scene\": [ { \"type\": \"group\", \"components\": { \"nope\": {} } },"
                      " { \"type\": \"gltf\", \"source\": \"level.gltf#Missing\" } ] }";
    errors = br_scene_file_error_count(e);
    br_node view2 = br_scene_file_load_json(e, bad, SDL_strlen(bad), "bad.scene.json", br_scene_root(e));
    BR_CHECK(br_scene_file_error_count(e) == errors + 2);
    br_node_destroy(e, view2);
    br_node_destroy(e, view);
    br_engine_step(e, 1.0f / 60.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_assets_get(e, "level.gltf") == NULL);
}

int main(void)
{
    br_engine *e = br_engine_create("test_gltf_scene", BRATY_SOURCE_DIR "/tests/data"); /* the source tree (CMake) */
    BR_CHECK(e != NULL);
    if (e != NULL) {
        register_components(e);
        test_level(e);
        test_as_scene(e);
        test_as_prefab(e);
        test_scene_json(e);
        br_engine_destroy(e);
    }
    return br_test_finish();
}
