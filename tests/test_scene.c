/* test_scene - first-class scenes (headless; data in tests/data/scenes, see make_scene_data.py):
 * load/unload back to baseline, shared assets (Review Focus 2), unload during async load
 * (Review Focus 1, 200 runs), additive + active, persistent nodes and adoption, scene systems,
 * switch with fade + loading scene, lazy assets, view3d resources, the old br_scene_file API,
 * asset collection, game.json keys. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <string.h>

static const float DT = 1.0f / 60.0f;

static void step(br_engine *e, int frames)
{
    for (int i = 0; i < frames; i++) {
        br_engine_step(e, DT);
    }
}

/* Steps until no asset is loading (bounded), then twice more (scene builds, deferred releases). */
static void settle(br_engine *e)
{
    for (int i = 0; i < 4000 && br_assets_pending(e) > 0; i++) {
        br_engine_step(e, DT);
        SDL_Delay(i > 20 ? 1 : 0);
    }
    step(e, 2);
}

static br_node find(br_engine *e, br_node root, const char *label)
{
    return br_node_find_by_label(e, root, label, true);
}

/* A system that unloads a scene once, from inside a frame (deferred node destroy). */
typedef struct unload_in_frame {
    br_scene scene;
    bool     done;
} unload_in_frame;

static void unload_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    unload_in_frame *u = user;
    if (!u->done) {
        br_scene_unload(e, u->scene);
        u->done = true;
    }
}

/* ---- load / unload leaves nodes and assets at baseline */

static void test_load_unload_baseline(br_engine *e)
{
    uint32_t nodes = br_node_count(e);
    uint32_t assets = br_assets_count(e);
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    BR_CHECK(br_scene_is_loaded(e, a) && br_scene_progress(e, a) == 1.0f);
    BR_CHECK(strcmp(br_scene_name(e, a), "a") == 0);
    BR_CHECK(br_node_valid(e, find(e, br_scene_root_2d(e, a), "thing"))); /* from the prefab */
    BR_CHECK(br_assets_ref_count(e, "scenes/red.png") == 1);
    BR_CHECK(br_assets_ref_count(e, "scenes/shared.png") == 1); /* "assets" + a sprite: one group reference */
    BR_CHECK(br_assets_ref_count(e, "scenes/green.png") == 1);  /* inside the prefab */
    BR_CHECK(br_node_count(e) == nodes + 5);              /* scene root, file root, 3 sprites */
    const char *owners[4];
    BR_CHECK(br_assets_owners(e, "scenes/red.png", owners, 4) == 1 && strcmp(owners[0], "scene:a") == 0);
    BR_CHECK(strcmp(br_scene_active_name(e), "a") == 0);
    br_assets_report(e);
    br_scene_unload(e, a);
    BR_CHECK(!br_scene_valid(e, a) && !br_scene_valid(e, br_scene_active(e)));
    BR_CHECK(strcmp(br_scene_active_name(e), "") == 0);
    BR_CHECK(br_node_count(e) == nodes);
    BR_CHECK(br_assets_count(e) == assets);

    /* unloading inside a frame: nodes die at once, assets after the next flush */
    unload_in_frame u = { br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE), false };
    br_system_add(e, "test.unload", BR_PHASE_UPDATE, 0, unload_system, &u);
    step(e, 1);
    BR_CHECK(u.done && br_node_count(e) == nodes);
    BR_CHECK(br_assets_count(e) == assets + 3); /* shared, red, green: held until the flush */
    step(e, 1);
    BR_CHECK(br_assets_count(e) == assets);
    br_system_set_enabled(e, "test.unload", false);
}

/* ---- Review Focus 2: an asset shared by two scenes / groups survives one unload */

static void test_shared_asset(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_scene b = br_scene_load(e, "scenes/b.scene.json", BR_SCENE_ADDITIVE);
    BR_CHECK(br_scene_valid(e, a) && br_scene_valid(e, b) && br_scene_count(e) == 2);
    BR_CHECK(br_assets_ref_count(e, "scenes/shared.png") == 2);
    const char *owners[4];
    BR_CHECK(br_assets_owners(e, "scenes/shared.png", owners, 4) == 2);
    br_scene_unload(e, a);
    BR_CHECK(br_assets_get(e, "scenes/shared.png") != NULL && br_assets_ref_count(e, "scenes/shared.png") == 1);
    BR_CHECK(br_assets_get(e, "scenes/red.png") == NULL); /* only a used it */
    br_node sprite = find(e, br_scene_root_2d(e, b), "shared_b");
    br_texture t = br_sprite_texture(e, sprite);
    BR_CHECK(br_texture_valid(e, t)); /* b's sprite still has its texture */
    br_scene_unload(e, b);
    BR_CHECK(br_assets_get(e, "scenes/shared.png") == NULL);
    BR_CHECK(br_assets_count(e) == assets);
}

/* ---- Review Focus 1: unload while the assets still load, 200 times with varying timing */

static void test_unload_during_async(br_engine *e)
{
    uint32_t nodes = br_node_count(e);
    uint32_t assets = br_assets_count(e);
    uint32_t loaded_before_unload = 0;
    for (int i = 0; i < 200; i++) {
        br_scene s = br_scene_load_async(e, i % 2 == 0 ? "scenes/a.scene.json" : "scenes/b.scene.json",
                                         i % 3 == 0 ? BR_SCENE_SINGLE : BR_SCENE_ADDITIVE);
        BR_CHECK(br_scene_valid(e, s));
        step(e, i % 4);
        if (i % 5 == 0) {
            SDL_Delay((Uint32)(i % 3)); /* let workers get ahead sometimes */
        }
        loaded_before_unload += br_scene_is_loaded(e, s) ? 1u : 0u;
        if (i % 2 == 0) {
            br_scene_unload(e, s); /* between frames */
        } else {
            unload_in_frame u = { s, false };
            char name[32];
            SDL_snprintf(name, sizeof(name), "test.async_unload_%d", i);
            br_system_add(e, name, BR_PHASE_UPDATE, 0, unload_system, &u);
            step(e, 1); /* inside a frame */
            br_system_set_enabled(e, name, false);
            BR_CHECK(u.done);
        }
        BR_CHECK(!br_scene_valid(e, s) && br_scene_root_2d(e, s).generation == 0);
    }
    settle(e);
    printf("  async unload: %u of 200 scenes had finished loading before the unload\n", loaded_before_unload);
    BR_CHECK(br_scene_count(e) == 0);
    BR_CHECK(br_node_count(e) == nodes);
    BR_CHECK(br_assets_count(e) == assets);
    BR_CHECK(br_assets_pending(e) == 0);
}

/* ---- additive scenes, active scene, find, single unloads the others */

static void test_additive_and_active(br_engine *e)
{
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_scene b = br_scene_load(e, "scenes/b.scene.json", BR_SCENE_ADDITIVE);
    BR_CHECK(br_scene_valid(e, br_scene_find(e, "b")) && br_scene_find(e, "b").index == b.index);
    BR_CHECK(br_scene_find(e, "scenes/a.scene.json").index == a.index);
    BR_CHECK(br_scene_active(e).index == a.index); /* additive does not take over */
    BR_CHECK(br_scene_at(e, 0).index == a.index && br_scene_at(e, 1).index == b.index);
    br_scene_set_active(e, b);
    BR_CHECK(strcmp(br_scene_active_name(e), "b") == 0);
    /* b's root is drawn after a's: later scenes are on top */
    br_node stage = br_stage_root(e);
    BR_CHECK(br_node_child_index(e, stage, br_scene_root_2d(e, a)) < br_node_child_index(e, stage, br_scene_root_2d(e, b)));
    br_scene c = br_scene_load(e, "scenes/lazy.scene.json", BR_SCENE_SINGLE);
    BR_CHECK(!br_scene_valid(e, a) && !br_scene_valid(e, b) && br_scene_count(e) == 1);
    BR_CHECK(br_scene_active(e).index == c.index);
    br_scene_unload(e, c);
}

/* ---- persistent nodes, nodes added to the stage belong to the active scene */

static void test_persistent_nodes(br_engine *e)
{
    br_node before = br_node_create_container(e); /* on the stage before any scene: nobody's */
    br_node_add_child(e, br_stage_root(e), before);
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_node hud = br_node_create_container(e);
    br_node_add_child(e, br_scene_root_2d(e, a), hud);
    br_node_set_position(e, hud, 5.0f, 6.0f);
    br_node_set_persistent(e, hud, true);
    BR_CHECK(br_node_persistent(e, hud) && br_node_equal(br_node_parent(e, hud), br_stage_root(e)));
    br_node spawned = br_node_create_container(e); /* added to the stage while a is active */
    br_node_add_child(e, br_stage_root(e), spawned);
    step(e, 1);
    BR_CHECK(br_node_equal(br_node_parent(e, spawned), br_scene_root_2d(e, a)));
    BR_CHECK(br_node_equal(br_node_parent(e, before), br_stage_root(e)));
    br_scene b = br_scene_load(e, "scenes/b.scene.json", BR_SCENE_SINGLE);
    BR_CHECK(!br_scene_valid(e, a));
    BR_CHECK(br_node_valid(e, hud) && !br_node_valid(e, spawned) && br_node_valid(e, before));
    BR_CHECK(br_node_position(e, hud).x == 5.0f);
    /* persistent nodes draw above the new scene */
    br_node stage = br_stage_root(e);
    BR_CHECK(br_node_child_index(e, stage, hud) > br_node_child_index(e, stage, br_scene_root_2d(e, b)));
    br_scene_unload(e, b);
    BR_CHECK(br_node_valid(e, hud));
    br_node_destroy(e, hud);
    br_node_destroy(e, before);
}

/* ---- scene-scoped systems stop with their scene */

static void count_system(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    (*(int *)user)++;
}

static void test_scene_systems(br_engine *e)
{
    int count = 0;
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_system_add_for_scene(e, a, "test.scoped", BR_PHASE_UPDATE, 0, count_system, &count);
    step(e, 3);
    BR_CHECK(count == 3);
    br_scene_unload(e, a);
    step(e, 3);
    BR_CHECK(count == 3);
    /* the name is free again for another scene; a loading scene does not run it yet */
    br_scene b = br_scene_load_async(e, "scenes/b.scene.json", BR_SCENE_SINGLE);
    br_system_add_for_scene(e, b, "test.scoped", BR_PHASE_LATE_UPDATE, 42, count_system, &count);
    /* ... with its own phase and priority (the first scene's system was removed, not reused) */
    bool fresh = false;
    for (uint32_t i = 0; i < br_system_count(e); i++) {
        br_system_info info;
        if (br_system_get_info(e, i, &info) && strcmp(info.name, "test.scoped") == 0) {
            fresh = info.phase == BR_PHASE_LATE_UPDATE && info.priority == 42;
        }
    }
    BR_CHECK(fresh);
    settle(e);
    BR_CHECK(br_scene_is_loaded(e, b));
    int at_load = count;
    step(e, 2);
    BR_CHECK(count == at_load + 2);
    br_scene_unload(e, b);
    step(e, 2);
    BR_CHECK(count == at_load + 2);
}

/* ---- switch: fade out, loading scene, background load, fade in */

typedef struct event_log {
    char text[512];
} event_log;

static void on_scene_event(br_engine *e, br_scene scene, br_scene_event event, void *user)
{
    event_log *log = user;
    static const char *names[] = { "loaded", "activated", "unloading" };
    char line[64];
    SDL_snprintf(line, sizeof(line), "%s:%s ", names[event], br_scene_name(e, scene));
    SDL_strlcat(log->text, line, sizeof(log->text));
}

static void test_switch(br_engine *e, event_log *log)
{
    uint32_t assets = br_assets_count(e);
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_scene_set_loading_scene(e, "scenes/loading.scene.json");
    log->text[0] = '\0';
    br_scene target = br_scene_switch(e, "scenes/b.scene.json", BR_TRANSITION_FADE, 0.1f);
    BR_CHECK(br_scene_valid(e, target) && br_scene_switching(e) && !br_scene_is_loaded(e, target));
    BR_CHECK(br_scene_switch_target(e).index == target.index);
    BR_CHECK(br_scene_switch(e, "scenes/a.scene.json", BR_TRANSITION_FADE, 0.1f).generation == 0); /* one at a time */
    step(e, 3);
    BR_CHECK(br_scene_valid(e, a)); /* still fading out */
    bool saw_loading_active = false;
    int frames = 0;
    while (br_scene_switching(e) && frames < 600) {
        br_engine_step(e, DT);
        saw_loading_active = saw_loading_active || strcmp(br_scene_active_name(e), "loading") == 0;
        frames++;
    }
    BR_CHECK(!br_scene_switching(e));
    BR_CHECK(saw_loading_active);
    BR_CHECK(!br_scene_valid(e, a) && br_scene_is_loaded(e, target) && br_scene_count(e) == 1);
    BR_CHECK(br_scene_active(e).index == target.index);
    BR_CHECK(strstr(log->text, "unloading:a") != NULL && strstr(log->text, "loaded:loading") != NULL &&
             strstr(log->text, "unloading:loading") != NULL && strstr(log->text, "loaded:b") != NULL);
    printf("  switch: %d frames; events: %s\n", frames, log->text);
    br_scene_set_loading_scene(e, NULL);
    br_scene_unload(e, target);
    step(e, 2);
    BR_CHECK(br_assets_count(e) == assets);
}

/* ---- a scene system named like an existing system is refused, and unloading the scene
 * leaves that system alone */

static bool system_exists(br_engine *e, const char *name)
{
    for (uint32_t i = 0; i < br_system_count(e); i++) {
        br_system_info info;
        if (br_system_get_info(e, i, &info) && strcmp(info.name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_scoped_name_collision(br_engine *e)
{
    int game = 0;
    int scoped = 0;
    br_system_add(e, "test.game_system", BR_PHASE_UPDATE, 0, count_system, &game);
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_system_add_for_scene(e, a, "test.game_system", BR_PHASE_UPDATE, 0, count_system, &scoped);
    br_system_add_for_scene(e, a, "assets.update", BR_PHASE_UPDATE, 0, count_system, &scoped);
    step(e, 2);
    BR_CHECK(game == 2 && scoped == 0); /* both refused */
    br_scene_unload(e, a);
    step(e, 2);
    BR_CHECK(system_exists(e, "test.game_system") && system_exists(e, "assets.update"));
    BR_CHECK(game == 4);
    br_system_remove(e, "test.game_system");
}

/* ---- the switch target unloaded during the fade-out: the old scene stays */

static void test_switch_target_unloaded(br_engine *e)
{
    br_scene a = br_scene_load(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    br_scene_switch(e, "scenes/b.scene.json", BR_TRANSITION_FADE, 0.1f);
    step(e, 2);
    br_scene_unload(e, br_scene_switch_target(e));
    for (int i = 0; i < 600 && br_scene_switching(e); i++) {
        br_engine_step(e, DT);
    }
    BR_CHECK(!br_scene_switching(e));
    BR_CHECK(br_scene_is_loaded(e, a) && br_scene_count(e) == 1);
    br_scene_unload(e, a);
    step(e, 2);
}

/* ---- lazy vs preload */

static void test_lazy_assets(br_engine *e)
{
    br_scene s = br_scene_load(e, "scenes/lazy.scene.json", BR_SCENE_SINGLE);
    BR_CHECK(br_assets_get(e, "scenes/red.png") != NULL);   /* preload */
    BR_CHECK(br_assets_get(e, "scenes/extra.png") == NULL); /* lazy: listed, not loaded */
    br_asset_group *g = br_scene_assets(e, s);
    BR_CHECK(br_asset_group_count(g) == 2);
    BR_CHECK(br_asset_group_get(g, "scenes/extra.png") != NULL); /* loaded on first use */
    BR_CHECK(br_assets_ref_count(e, "scenes/extra.png") == 1);
    br_scene_unload(e, s);
    BR_CHECK(br_assets_get(e, "scenes/extra.png") == NULL && br_assets_get(e, "scenes/red.png") == NULL);
}

/* ---- view3d scene JSON: meshes, materials and the model are freed with the scene */

static void test_view3d_resources(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    uint32_t nodes3d = br_node3d_count(e);
    br_scene s = br_scene_load(e, "scenes/view.scene.json", BR_SCENE_SINGLE);
    br_node view = find(e, br_scene_root_2d(e, s), "view");
    BR_CHECK(br_node_valid(e, view));
    BR_CHECK(br_scene_root_3d(e, s).index == br_view3d_root(e, view).index); /* the file's view */
    br_node3d cube = br_node3d_find_by_label(e, br_view3d_root(e, view), "cube", true);
    br_mesh3d *m = br_mesh3d_get(e, cube);
    BR_CHECK(m != NULL);
    br_geometry3d geometry = m != NULL ? m->geometry : (br_geometry3d){ 0, 0 };
    br_material3d material = m != NULL ? m->material : (br_material3d){ 0, 0 };
    BR_CHECK(br_geometry3d_valid(e, geometry) && br_material3d_valid(e, material));
    BR_CHECK(br_assets_ref_count(e, "tiny.gltf") == 1 && br_assets_ref_count(e, "scenes/red.png") == 1);
    br_scene_unload(e, s);
    step(e, 2);
    BR_CHECK(!br_geometry3d_valid(e, geometry) && !br_material3d_valid(e, material));
    BR_CHECK(br_node3d_count(e) == nodes3d);
    BR_CHECK(br_assets_count(e) == assets);

    /* a scene without a view gets one for br_scene_root_3d */
    br_scene b = br_scene_load(e, "scenes/b.scene.json", BR_SCENE_SINGLE);
    br_node3d root3d = br_scene_root_3d(e, b);
    BR_CHECK(br_node3d_valid(e, root3d));
    BR_CHECK(br_scene_root_3d(e, b).index == root3d.index); /* the same view the second time */
    br_scene_unload(e, b);
    BR_CHECK(!br_node3d_valid(e, root3d));
}

/* ---- the old API: a scene file root and a prefab instance own what their build loaded */

static void test_scene_file_roots(br_engine *e)
{
    uint32_t assets = br_assets_count(e);
    br_node root = br_scene_file_load(e, "scenes/a.scene.json", br_node_create_container(e));
    BR_CHECK(br_node_valid(e, root) && br_assets_ref_count(e, "scenes/red.png") == 1);
    const char *owners[2];
    BR_CHECK(br_assets_owners(e, "scenes/red.png", owners, 2) == 1 && strcmp(owners[0], "file:scenes/a.scene.json") == 0);
    br_node prefab = br_prefab_instantiate_at(e, "scenes/thing.prefab.json", br_stage_root(e), br_vec2_make(7.0f, 8.0f));
    BR_CHECK(br_node_position(e, prefab).x == 7.0f && br_node_position(e, prefab).y == 8.0f);
    BR_CHECK(br_assets_ref_count(e, "scenes/green.png") == 2); /* the file's prefab + this instance */
    br_node_destroy(e, prefab);
    step(e, 1); /* released after the flush */
    BR_CHECK(br_assets_ref_count(e, "scenes/green.png") == 1);
    br_node_destroy(e, br_node_parent(e, root));
    step(e, 1);
    BR_CHECK(br_assets_count(e) == assets);
    /* repeated loads do not pile up references (API_REVIEW item 6: 2 -> 9 over a soak) */
    for (int i = 0; i < 5; i++) {
        br_node r = br_scene_file_load(e, "scenes/a.scene.json", br_stage_root(e));
        BR_CHECK(br_assets_ref_count(e, "scenes/shared.png") == 1);
        br_node_destroy(e, r);
        step(e, 1);
    }
    BR_CHECK(br_assets_count(e) == assets);
}

/* ---- asset collection covers models, fonts, sounds; glTF clip names are no assets */

typedef struct path_list {
    char text[1024];
} path_list;

static void on_path(const char *path, void *user)
{
    path_list *list = user;
    SDL_strlcat(list->text, path, sizeof(list->text));
    SDL_strlcat(list->text, " ", sizeof(list->text));
}

typedef struct sfx_component {
    char sound[BR_FIELD_STRING_SIZE];
} sfx_component;

static void test_collect(br_engine *e)
{
    static const br_field_desc fields[] = { { "sound", BR_FIELD_ASSET, offsetof(sfx_component, sound) } };
    br_component_desc desc = { "sfx", sizeof(sfx_component), fields, 1 };
    br_component_register(e, &desc);
    const char *json =
        "{ \"assets\": [\"x.json\", { \"path\": \"lazy.png\", \"load\": \"lazy\" }], \"children\": ["
        " { \"type\": \"text\", \"style\": { \"font\": \"fonts/a.ttf\" } },"
        " { \"type\": \"bitmap_text\", \"font\": \"fonts/b.fnt\" },"
        " { \"type\": \"animated_sprite\", \"animation\": \"sheet.json#walk\" },"
        " { \"components\": { \"sfx\": { \"sound\": \"sounds/hit.wav\" } } },"
        " { \"type\": \"view3d\", \"scene\": [ { \"type\": \"gltf\", \"source\": \"models/fox.glb\", \"animation\": \"Run\" } ] } ] }";
    cJSON *doc = cJSON_Parse(json);
    path_list list;
    list.text[0] = '\0';
    br_scene_file_collect_assets(e, doc, on_path, &list);
    cJSON_Delete(doc);
    BR_CHECK(strstr(list.text, "x.json ") != NULL && strstr(list.text, "lazy.png ") != NULL);
    BR_CHECK(strstr(list.text, "fonts/a.ttf ") != NULL && strstr(list.text, "fonts/b.fnt ") != NULL);
    BR_CHECK(strstr(list.text, "sheet.json ") != NULL && strstr(list.text, "sounds/hit.wav ") != NULL);
    BR_CHECK(strstr(list.text, "models/fox.glb ") != NULL);
    BR_CHECK(strstr(list.text, "Run") == NULL);
    printf("  collected: %s\n", list.text);
}

/* ---- game.json: input "threshold"/"context" and "scenes" are known keys; scenes by name */

static int g_unknown_key_warnings = 0;
static SDL_LogOutputFunction g_default_log = NULL;
static void *g_default_log_user = NULL;

static void count_warnings(void *unused, int category, SDL_LogPriority priority, const char *message)
{
    (void)unused;
    if (strstr(message, "unknown key") != NULL) {
        g_unknown_key_warnings++;
    }
    g_default_log(g_default_log_user, category, priority, message);
}

static void test_game_config_keys(void)
{
    const char *json = "{ \"scenes\": { \"first\": \"scenes/a.scene.json\" },"
                       "  \"input\": { \"jump\": { \"keys\": [\"Space\"], \"threshold\": 0.5, \"context\": \"menu\" } } }";
    SDL_GetLogOutputFunction(&g_default_log, &g_default_log_user);
    SDL_SetLogOutputFunction(count_warnings, NULL);
    br_game_config config;
    BR_CHECK(br_game_config_parse(json, strlen(json), "game.json", &config));
    SDL_SetLogOutputFunction(g_default_log, g_default_log_user);
    BR_CHECK(g_unknown_key_warnings == 0);

    br_engine *e = br_engine_create("test_scene_config", BRATY_SOURCE_DIR "/tests/data");
    br_game_config_apply(e, &config); /* the engine takes the JSON; no start scene */
    br_scene s = br_scene_load(e, "first", BR_SCENE_SINGLE);
    BR_CHECK(br_scene_is_loaded(e, s) && strcmp(br_scene_name(e, s), "first") == 0);
    BR_CHECK(strcmp(br_scene_path(e, s), "scenes/a.scene.json") == 0);
    BR_CHECK(br_scene_find(e, "first").index == s.index);
    br_engine_destroy(e);
}

/* ---- game.json "profiler" and "window.logical_size" */

static void test_game_config_profiler_logical(void)
{
    const char *json = "{ \"window\": { \"logical_size\": [640, 360] },"
                       "  \"profiler\": { \"hud\": true, \"panel\": true } }";
    g_unknown_key_warnings = 0;
    SDL_GetLogOutputFunction(&g_default_log, &g_default_log_user);
    SDL_SetLogOutputFunction(count_warnings, NULL);
    br_game_config config;
    BR_CHECK(br_game_config_parse(json, strlen(json), "game.json", &config));
    SDL_SetLogOutputFunction(g_default_log, g_default_log_user);
    BR_CHECK(g_unknown_key_warnings == 0);
    BR_CHECK(config.logical_width == 640.0f && config.logical_height == 360.0f);

    br_engine *e = br_engine_create("test_scene_config_logical", BRATY_SOURCE_DIR "/tests/data");
    br_camera_set_headless_screen(e, 1280.0f, 720.0f);
    br_game_config_apply(e, &config);
    BR_CHECK(br_profiler_hud(e) && br_profiler_is_open(e));
    br_engine_step(e, DT);
    /* a 640 x 360 design shown 2x in a 1280 x 720 window: window centre = design centre */
    br_vec2 p = br_camera_screen_to_stage(e, br_vec2_make(640.0f, 360.0f));
    BR_CHECK(SDL_fabsf(p.x - 320.0f) < 0.01f && SDL_fabsf(p.y - 180.0f) < 0.01f);
    p = br_camera_screen_to_stage(e, br_vec2_make(0.0f, 0.0f));
    BR_CHECK(SDL_fabsf(p.x) < 0.01f && SDL_fabsf(p.y) < 0.01f);
    br_engine_destroy(e);

    const char *bad = "{ \"window\": { \"logical_size\": [640] } }";
    BR_CHECK(br_game_config_parse(bad, strlen(bad), "game.json", &config));
    BR_CHECK(config.logical_width == 0.0f);
    br_game_config_free(&config);
}

/* ---- deterministic mode: async loads finish one asset per frame, in file order */

static void test_deterministic_order(void)
{
    br_engine *e = br_engine_create("test_scene_det", BRATY_SOURCE_DIR "/tests/data");
    br_app_set_deterministic(e, true, 1);
    br_scene s = br_scene_load_async(e, "scenes/a.scene.json", BR_SCENE_SINGLE);
    float progress[6];
    for (int i = 0; i < 6; i++) {
        br_engine_step(e, DT);
        progress[i] = br_scene_progress(e, s);
    }
    /* 3 assets: shared.png ("assets"), red.png, green.png (prefab) */
    BR_CHECK(br_asset_group_count(br_scene_assets(e, s)) == 3);
    BR_CHECK(strcmp(br_asset_group_path(br_scene_assets(e, s), 0), "scenes/shared.png") == 0);
    BR_CHECK(progress[0] > 0.3f && progress[0] < 0.4f && progress[1] > 0.6f && progress[1] < 0.7f);
    BR_CHECK(br_scene_is_loaded(e, s));
    br_engine_destroy(e);
}

int main(void)
{
    br_engine *e = br_engine_create("test_scene", BRATY_SOURCE_DIR "/tests/data"); /* the source tree (CMake) */
    event_log log;
    log.text[0] = '\0';
    br_scene_on_event(e, on_scene_event, &log);
    test_load_unload_baseline(e);
    test_shared_asset(e);
    test_unload_during_async(e);
    test_additive_and_active(e);
    test_persistent_nodes(e);
    test_scene_systems(e);
    test_switch(e, &log);
    test_scoped_name_collision(e);
    test_switch_target_unloaded(e);
    test_lazy_assets(e);
    test_view3d_resources(e);
    test_scene_file_roots(e);
    test_collect(e);
    br_engine_destroy(e);
    test_game_config_keys();
    test_game_config_profiler_logical();
    test_deterministic_order();
    return br_test_finish();
}
