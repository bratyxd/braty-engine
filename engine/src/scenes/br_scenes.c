/* br_scenes: first-class scenes (load / additive / async / unload / active / switch with fade
 * and loading scene), persistent nodes, scene-scoped systems.
 *
 * A scene = a scene file built under a 2D root container on the stage + an asset group. Loads
 * never call back into a scene: the group is polled (scenes.update, after assets.update), so
 * unloading a scene that is still loading only has to drop its group (the asset core cancels
 * loads nobody else wants) and free its slot. Nodes go first and assets after the node flush
 * (data.release_assets), because dying nodes may still point at a font. */
#include "app/br_engine_internal.h"
#include "data/br_data_internal.h"
#include "data/br_gltf_scene_internal.h" /* a .glb / .gltf path is a scene too */

#define BR_SCENE_NAME_SIZE 64
#define BR_SCENE_PATH_SIZE 512

typedef struct br_scene_rec {
    uint32_t        generation;
    bool            alive;
    bool            loaded;   /* nodes built */
    bool            hold;     /* switch target: built when the transition reaches it */
    br_scene_mode   mode;
    char            name[BR_SCENE_NAME_SIZE];
    char            path[BR_SCENE_PATH_SIZE];
    br_asset_group *group;
    br_node         root;
} br_scene_rec;

/* A system added with br_system_add_for_scene (the engine keeps a pointer to it as the
 * system's user data). Removed with the system when its scene unloads. */
typedef struct br_scoped_system {
    char        *name;
    br_scene     scene;
    br_system_fn fn;
    void        *user;
} br_scoped_system;

typedef struct br_scene_listener {
    br_scene_event_fn fn;
    void             *user;
} br_scene_listener;

typedef enum br_switch_phase {
    BR_SWITCH_NONE,
    BR_SWITCH_OUT,          /* fading to black */
    BR_SWITCH_LOADING_IN,   /* fading from black into the loading scene */
    BR_SWITCH_WAIT,         /* target loading (black, or the loading scene) */
    BR_SWITCH_LOADING_OUT,  /* fading the loading scene to black */
    BR_SWITCH_IN            /* fading from black into the target */
} br_switch_phase;

typedef struct br_scenes_state {
    br_array        records;     /* br_scene_rec, by handle index */
    br_array        order;       /* br_scene, valid scenes in load order */
    br_array        persistent;  /* br_node: br_node_set_persistent, drawn above scenes */
    br_array        unowned;     /* br_node: on the stage while no scene was active, left in place */
    br_array        scoped;      /* br_scoped_system * */
    br_array        listeners;   /* br_scene_listener */
    br_scene        active;
    br_switch_phase phase;
    float           seconds;
    float           alpha;       /* fade overlay, 0..1 */
    br_scene        target;
    br_scene        loading;
    char            loading_name[BR_SCENE_PATH_SIZE];
} br_scenes_state;

static br_scene br_scene_none(void)
{
    br_scene none = { 0, 0 };
    return none;
}

static bool br_scene_same(br_scene a, br_scene b)
{
    return a.index == b.index && a.generation == b.generation;
}

static br_scene_rec *br_scene_get(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_array_at(&e->scenes->records, scene.index);
    if (rec == NULL || !rec->alive || rec->generation != scene.generation || scene.generation == 0) {
        return NULL;
    }
    return rec;
}

static void br_scenes_emit(br_engine *e, br_scene scene, br_scene_event event)
{
    br_scenes_state *s = e->scenes;
    for (uint32_t i = 0; i < s->listeners.count; i++) {
        br_scene_listener *l = br_array_at(&s->listeners, i);
        l->fn(e, scene, event, l->user);
    }
}

/* ---- names: game.json "scenes" keys or file paths */

static void br_scenes_resolve(br_engine *e, const char *name, char *path, char *short_name)
{
    const cJSON *scenes = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "scenes");
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(scenes, name);
    if (cJSON_IsString(entry)) {
        br_str_copy(path, BR_SCENE_PATH_SIZE, entry->valuestring);
        br_str_copy(short_name, BR_SCENE_NAME_SIZE, name);
        return;
    }
    br_str_copy(path, BR_SCENE_PATH_SIZE, name);
    /* "levels/one.scene.json" -> "one" */
    const char *base = SDL_strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    br_str_copy(short_name, BR_SCENE_NAME_SIZE, base);
    char *dot = SDL_strchr(short_name, '.');
    if (dot != NULL && dot != short_name) {
        *dot = '\0';
    }
}

/* ---- ownership of stage nodes */

static bool br_scenes_listed(const br_array *list, br_node n)
{
    for (uint32_t i = 0; i < list->count; i++) {
        if (br_node_equal(*(br_node *)br_array_at(list, i), n)) {
            return true;
        }
    }
    return false;
}

static bool br_scenes_is_persistent(br_scenes_state *s, br_node n)
{
    return br_scenes_listed(&s->persistent, n) || br_scenes_listed(&s->unowned, n);
}

/* Drops dead handles from a node list. */
static void br_scenes_prune(br_engine *e, br_array *list)
{
    for (uint32_t i = list->count; i > 0; i--) {
        if (!br_node_valid(e, *(br_node *)br_array_at(list, i - 1))) {
            br_array_remove_swap(list, i - 1);
        }
    }
}

static bool br_scenes_is_root(br_engine *e, br_node n)
{
    br_scenes_state *s = e->scenes;
    for (uint32_t i = 0; i < s->records.count; i++) {
        br_scene_rec *rec = br_array_at(&s->records, i);
        if (rec->alive && br_node_equal(rec->root, n)) {
            return true;
        }
    }
    return false;
}

/* Stage children that are neither scene roots nor persistent, in stage order. */
static void br_scenes_orphans(br_engine *e, br_array *out)
{
    br_node stage = br_scene_root(e);
    uint32_t count = br_node_child_count(e, stage);
    for (uint32_t i = 0; i < count; i++) {
        br_node child = br_node_child_at(e, stage, i);
        if (br_node_valid(e, child) && !br_scenes_is_root(e, child) && !br_scenes_is_persistent(e->scenes, child)) {
            br_array_push(out, &child);
        }
    }
}

/* Moves stage orphans into the active scene, or (no active scene) marks them as nobody's. */
static void br_scenes_adopt(br_engine *e)
{
    br_scenes_state *s = e->scenes;
    br_scene_rec *active = br_scene_get(e, s->active);
    br_array orphans;
    br_array_init(&orphans, sizeof(br_node));
    br_scenes_orphans(e, &orphans);
    for (uint32_t i = 0; i < orphans.count; i++) {
        br_node n = *(br_node *)br_array_at(&orphans, i);
        if (active != NULL && active->loaded && br_node_valid(e, active->root)) {
            br_node_add_child(e, active->root, n);
        } else {
            br_array_push(&s->unowned, &n); /* on the stage while no scene was active: nobody's */
        }
    }
    br_array_free(&orphans);
}

/* Persistent nodes draw above scenes: re-append them after a new scene root. */
static void br_scenes_raise_persistent(br_engine *e)
{
    br_scenes_state *s = e->scenes;
    br_node stage = br_scene_root(e);
    for (uint32_t i = 0; i < s->persistent.count; i++) {
        br_node n = *(br_node *)br_array_at(&s->persistent, i);
        if (br_node_valid(e, n) && br_node_equal(br_node_parent(e, n), stage)) {
            br_node_add_child(e, stage, n);
        }
    }
}

void br_node_set_persistent(br_engine *e, br_node n, bool persistent)
{
    br_scenes_state *s = e->scenes;
    if (!br_node_valid(e, n)) {
        return;
    }
    for (uint32_t i = 0; i < s->persistent.count; i++) {
        if (br_node_equal(*(br_node *)br_array_at(&s->persistent, i), n)) {
            if (!persistent) {
                br_array_remove_swap(&s->persistent, i); /* back on the stage: adopted by the active scene */
            }
            return;
        }
    }
    if (!persistent) {
        return;
    }
    br_node stage = br_scene_root(e);
    if (!br_node_equal(br_node_parent(e, n), stage)) {
        br_node_reparent(e, n, stage); /* out of its scene, same place on screen */
    }
    br_array_push(&s->persistent, &n);
}

bool br_node_persistent(br_engine *e, br_node n)
{
    return br_node_valid(e, n) && br_scenes_listed(&e->scenes->persistent, n);
}

/* ---- loading */

static br_scene br_scenes_alloc(br_engine *e, const char *path, const char *name, br_scene_mode mode)
{
    br_scenes_state *s = e->scenes;
    uint32_t index = s->records.count;
    for (uint32_t i = 0; i < s->records.count; i++) {
        br_scene_rec *rec = br_array_at(&s->records, i);
        if (!rec->alive && i > 0) {
            index = i;
            break;
        }
    }
    if (index == 0) {
        br_scene_rec unused; /* index 0 stays unused: a zero handle is never valid */
        SDL_zero(unused);
        br_array_push(&s->records, &unused);
        index = 1;
    }
    if (index >= s->records.count) {
        br_scene_rec fresh;
        SDL_zero(fresh);
        br_array_push(&s->records, &fresh);
    }
    br_scene_rec *rec = br_array_at(&s->records, index);
    uint32_t generation = rec->generation + 1;
    SDL_zerop(rec);
    rec->generation = generation;
    rec->alive = true;
    rec->mode = mode;
    br_str_copy(rec->path, sizeof(rec->path), path);
    br_str_copy(rec->name, sizeof(rec->name), name);
    char group_name[BR_SCENE_NAME_SIZE + 8];
    SDL_snprintf(group_name, sizeof(group_name), "scene:%s", name);
    rec->group = br_asset_group_create(e, group_name);
    br_scene handle = { index, generation };
    br_array_push(&s->order, &handle);
    return handle;
}

static void br_scenes_add_entry(br_engine *e, const char *path, bool lazy, void *user)
{
    (void)e;
    if (lazy) {
        br_asset_group_add_lazy(user, path);
    } else {
        br_asset_group_add(user, path);
    }
}

typedef struct br_scenes_collect {
    br_engine      *engine;
    br_asset_group *group;
} br_scenes_collect;

static void br_scenes_add_referenced(const char *path, void *user)
{
    br_scenes_collect *c = user;
    char resolved[BR_SCENE_PATH_SIZE];
    if (!br_assets_resolve(c->engine, path, resolved, sizeof(resolved))) {
        return;
    }
    if (br_assets_get(c->engine, resolved) != NULL || br_file_exists(c->engine, resolved)) {
        br_asset_group_add(c->group, path);
    }
}

/* Reads the scene file and lists its assets in the group: the root's "assets" (paths or
 * bundles, preload / lazy), then every path its nodes, prefabs and components name. */
static bool br_scenes_collect_assets(br_engine *e, br_scene_rec *rec)
{
    void *bytes = NULL;
    size_t size = 0;
    cJSON *doc = NULL;
    if (br_data_is_model_path(rec->path)) {
        doc = br_data_model_scene_json(e, rec->path, true); /* the view3d node the file stands for */
    } else if (!br_file_read_all(e, rec->path, &bytes, &size)) {
        br_log_error("scene '%s': cannot read '%s'", rec->name, rec->path);
        return false;
    } else {
        doc = br_data_parse(e, rec->path, bytes, size);
        br_free(bytes);
    }
    if (!cJSON_IsObject(doc)) {
        if (doc != NULL) {
            br_log_error("scene '%s': '%s' must be one node object", rec->name, rec->path);
        }
        cJSON_Delete(doc);
        return false;
    }
    br_data_scope saved;
    br_data_begin_file(e, rec->path, &saved);
    cJSON *assets = cJSON_DetachItemFromObjectCaseSensitive(doc, "assets");
    if (cJSON_IsArray(assets)) {
        size_t mark = br_data_path_push_key(e, "assets");
        int i = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, assets)
        {
            size_t item_mark = br_data_path_push_index(e, i);
            br_data_assets_entry(e, item, br_scenes_add_entry, rec->group);
            br_data_path_pop(e, item_mark);
            i++;
        }
        br_data_path_pop(e, mark);
    }
    cJSON_Delete(assets);
    br_scenes_collect c = { e, rec->group };
    br_data_collect_expanded(e, doc, br_scenes_add_referenced, &c);
    br_data_end_file(e, &saved);
    cJSON_Delete(doc);
    return true;
}

static void br_scenes_set_active_internal(br_engine *e, br_scene scene)
{
    br_scenes_state *s = e->scenes;
    if (br_scene_same(s->active, scene)) {
        return;
    }
    br_scenes_adopt(e); /* what was added under the old active scene stays with it */
    s->active = scene;
    br_scenes_emit(e, scene, BR_SCENE_EVENT_ACTIVATED);
}

static void br_scenes_unload_others(br_engine *e, br_scene keep)
{
    br_scenes_state *s = e->scenes;
    for (uint32_t i = s->order.count; i > 0; i--) {
        if (i > s->order.count) {
            continue; /* a callback unloaded several */
        }
        br_scene other = *(br_scene *)br_array_at(&s->order, i - 1);
        if (!br_scene_same(other, keep) && !br_scene_same(other, s->target)) {
            br_scene_unload(e, other);
        }
    }
}

/* Builds the nodes of a scene whose assets are loaded. */
static void br_scenes_build(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL || rec->loaded) {
        return;
    }
    br_scenes_adopt(e); /* stage orphans go to the scene active before this one */
    br_node root = br_node_create_container(e);
    char label[BR_SCENE_NAME_SIZE + 8];
    SDL_snprintf(label, sizeof(label), "scene:%s", rec->name);
    br_node_set_label(e, root, label);
    br_node_add_child(e, br_scene_root(e), root);
    rec->root = root;
    rec->loaded = true;
    char path[BR_SCENE_PATH_SIZE];
    br_str_copy(path, sizeof(path), rec->path);
    br_asset_group *group = rec->group;
    br_scene_mode mode = rec->mode;
    /* rec may move: nodes made by the file may load scenes */
    if (br_data_is_model_path(path)) {
        br_data_load_model_owned(e, path, root, group);
    } else {
        br_data_load_owned(e, path, root, group);
    }
    br_scenes_raise_persistent(e);
    br_log_info("scene '%s': loaded", br_scene_name(e, scene));
    br_scenes_emit(e, scene, BR_SCENE_EVENT_LOADED);
    if (!br_scene_valid(e, scene)) {
        return; /* unloaded by a listener */
    }
    if (mode == BR_SCENE_SINGLE) {
        br_scenes_unload_others(e, scene);
        br_scenes_set_active_internal(e, scene);
    } else if (br_scene_get(e, e->scenes->active) == NULL) {
        br_scenes_set_active_internal(e, scene);
    }
}

static br_scene br_scenes_start(br_engine *e, const char *name, br_scene_mode mode)
{
    if (name == NULL || name[0] == '\0') {
        br_log_error("br_scene_load: empty name");
        return br_scene_none();
    }
    char path[BR_SCENE_PATH_SIZE];
    char short_name[BR_SCENE_NAME_SIZE];
    br_scenes_resolve(e, name, path, short_name);
    br_scene scene = br_scenes_alloc(e, path, short_name, mode);
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL || !br_scenes_collect_assets(e, rec)) {
        br_scene_unload(e, scene);
        return br_scene_none();
    }
    return scene;
}

br_scene br_scene_load(br_engine *e, const char *name, br_scene_mode mode)
{
    br_scene scene = br_scenes_start(e, name, mode);
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL) {
        return scene;
    }
    br_asset_group_load(rec->group); /* failures are logged; the scene still loads what it can */
    br_scenes_build(e, scene);
    return scene;
}

br_scene br_scene_load_async(br_engine *e, const char *name, br_scene_mode mode)
{
    br_scene scene = br_scenes_start(e, name, mode);
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec != NULL) {
        br_asset_group_load_async(rec->group, NULL, NULL, NULL);
    }
    return scene;
}

/* Gives a group back after the node flush (dying nodes may still use its assets). */
static void br_scenes_release_group(br_engine *e, br_asset_group *group)
{
    if (e->stepping) {
        br_array_push(&e->data->release, &group); /* data.release_assets, right after the flush */
        return;
    }
    br_data_release_pending(e); /* outside a frame the flush already happened */
    br_asset_group_destroy(group);
}

static bool br_scoped_find(br_engine *e, const char *name, const void *user);

void br_scene_unload(br_engine *e, br_scene scene)
{
    br_scenes_state *s = e->scenes;
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL) {
        return;
    }
    if (rec->loaded) {
        if (br_scene_same(s->active, scene)) {
            br_scenes_adopt(e);
        }
        br_scenes_emit(e, scene, BR_SCENE_EVENT_UNLOADING);
        rec = br_scene_get(e, scene);
        if (rec == NULL) {
            return; /* a listener unloaded it already */
        }
    }
    for (uint32_t i = s->scoped.count; i > 0; i--) {
        br_scoped_system *sys = *(br_scoped_system **)br_array_at(&s->scoped, i - 1);
        if (br_scene_same(sys->scene, scene)) {
            /* the engine stops calling it at once (deferred removal inside a frame), so the
             * record can go now. Only our own system: the game may have removed it and added
             * another one under the same name since. */
            if (br_scoped_find(e, sys->name, sys)) {
                br_system_remove(e, sys->name);
            }
            br_free(sys->name);
            br_free(sys);
            br_array_remove_swap(&s->scoped, i - 1);
        }
    }
    br_node root = rec->root;
    br_asset_group *group = rec->group;
    rec->alive = false;
    rec->loaded = false;
    rec->group = NULL;
    rec->root.index = 0;
    rec->root.generation = 0;
    for (uint32_t i = 0; i < s->order.count; i++) {
        if (br_scene_same(*(br_scene *)br_array_at(&s->order, i), scene)) {
            br_array_remove_ordered(&s->order, i);
            break;
        }
    }
    if (br_scene_same(s->active, scene)) {
        s->active = br_scene_none();
        for (uint32_t i = s->order.count; i > 0; i--) {
            br_scene other = *(br_scene *)br_array_at(&s->order, i - 1);
            br_scene_rec *other_rec = br_scene_get(e, other);
            if (other_rec != NULL && other_rec->loaded) {
                s->active = other;
                br_scenes_emit(e, other, BR_SCENE_EVENT_ACTIVATED);
                break;
            }
        }
    }
    if (br_node_valid(e, root)) {
        br_node_destroy(e, root);
    }
    br_scenes_release_group(e, group);
}

/* ---- queries */

bool br_scene_valid(br_engine *e, br_scene scene)
{
    return br_scene_get(e, scene) != NULL;
}

bool br_scene_is_loaded(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    return rec != NULL && rec->loaded;
}

float br_scene_progress(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL) {
        return 0.0f;
    }
    return rec->loaded ? 1.0f : br_asset_group_progress(rec->group);
}

void br_scene_set_active(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL || !rec->loaded) {
        br_log_warn("br_scene_set_active: the scene is not loaded");
        return;
    }
    br_scenes_set_active_internal(e, scene);
}

br_scene br_scene_active(br_engine *e)
{
    return br_scene_get(e, e->scenes->active) != NULL ? e->scenes->active : br_scene_none();
}

const char *br_scene_active_name(br_engine *e)
{
    return br_scene_name(e, br_scene_active(e));
}

const char *br_scene_name(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    return rec != NULL ? rec->name : "";
}

const char *br_scene_path(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    return rec != NULL ? rec->path : "";
}

br_scene br_scene_find(br_engine *e, const char *name)
{
    br_scenes_state *s = e->scenes;
    for (uint32_t i = 0; name != NULL && i < s->order.count; i++) {
        br_scene scene = *(br_scene *)br_array_at(&s->order, i);
        br_scene_rec *rec = br_scene_get(e, scene);
        if (rec != NULL && (SDL_strcmp(rec->name, name) == 0 || SDL_strcmp(rec->path, name) == 0)) {
            return scene;
        }
    }
    return br_scene_none();
}

uint32_t br_scene_count(br_engine *e)
{
    return e->scenes->order.count;
}

br_scene br_scene_at(br_engine *e, uint32_t i)
{
    br_scene *scene = br_array_at(&e->scenes->order, i);
    return scene != NULL ? *scene : br_scene_none();
}

br_node br_scene_root_2d(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    br_node none = { 0, 0 };
    return rec != NULL ? rec->root : none;
}

/* The first view3d node in a subtree, depth-first. */
static br_node br_scenes_find_view(br_engine *e, br_node n, uint32_t view_type)
{
    if (br_node_type_of(e, n) == view_type) {
        return n;
    }
    uint32_t count = br_node_child_count(e, n);
    for (uint32_t i = 0; i < count; i++) {
        br_node child = br_node_child_at(e, n, i);
        if (br_node_valid(e, child)) {
            br_node found = br_scenes_find_view(e, child, view_type);
            if (br_node_valid(e, found)) {
                return found;
            }
        }
    }
    br_node none = { 0, 0 };
    return none;
}

br_node3d br_scene_root_3d(br_engine *e, br_scene scene)
{
    br_node3d none = { 0, 0 };
    br_scene_rec *rec = br_scene_get(e, scene);
    if (rec == NULL || !rec->loaded) {
        return none;
    }
    uint32_t view_type = br_view3d_type(e);
    br_node view = br_scenes_find_view(e, rec->root, view_type);
    if (!br_node_valid(e, view)) {
        int w = 0;
        int h = 0;
        br_engine_window_size(e, &w, &h);
        view = br_view3d_create(e, w > 0 ? (float)w : 1280.0f, h > 0 ? (float)h : 720.0f);
        br_view3d_set_auto_resize(e, view, true);
        br_node_add_child_at(e, rec->root, view, 0); /* under the scene's 2D content */
    }
    return br_view3d_root(e, view);
}

br_asset_group *br_scene_assets(br_engine *e, br_scene scene)
{
    br_scene_rec *rec = br_scene_get(e, scene);
    return rec != NULL ? rec->group : NULL;
}

void br_scene_on_event(br_engine *e, br_scene_event_fn fn, void *user)
{
    if (fn != NULL) {
        br_scene_listener l = { fn, user };
        br_array_push(&e->scenes->listeners, &l);
    }
}

/* ---- scene-scoped systems */

/* True if a live system called `name` exists; with `user` non-NULL, only one with that user data. */
static bool br_scoped_find(br_engine *e, const char *name, const void *user)
{
    br_array *lists[2] = { &e->systems, &e->pending_systems };
    for (uint32_t l = 0; l < 2; l++) {
        for (uint32_t i = 0; i < lists[l]->count; i++) {
            const br_system_entry *entry = br_array_at(lists[l], i);
            if (!entry->removed && SDL_strcmp(entry->name, name) == 0 && (user == NULL || entry->user == user)) {
                return true;
            }
        }
    }
    return false;
}

static void br_scoped_run(br_engine *e, float dt, void *user)
{
    br_scoped_system *sys = user;
    if (br_scene_is_loaded(e, sys->scene)) {
        sys->fn(e, dt, sys->user);
    }
}

void br_system_add_for_scene(br_engine *e, br_scene scene, const char *name, br_phase phase, int priority,
                             br_system_fn fn, void *user)
{
    br_scenes_state *s = e->scenes;
    if (name == NULL || fn == NULL || !br_scene_valid(e, scene)) {
        br_log_error("br_system_add_for_scene: invalid arguments or scene");
        return;
    }
    for (uint32_t i = 0; i < s->scoped.count; i++) {
        br_scoped_system *sys = *(br_scoped_system **)br_array_at(&s->scoped, i);
        if (SDL_strcmp(sys->name, name) == 0) {
            br_log_error("system '%s' already exists (scene '%s')", name, br_scene_name(e, sys->scene));
            return;
        }
    }
    if (br_scoped_find(e, name, NULL)) {
        /* an engine or game system: recording it would remove that system when the scene unloads */
        br_log_error("br_system_add_for_scene: system '%s' already exists; not added", name);
        return;
    }
    br_scoped_system *sys = br_alloc(sizeof(br_scoped_system));
    if (sys == NULL) {
        return;
    }
    sys->name = br_strdup(name);
    sys->scene = scene;
    sys->fn = fn;
    sys->user = user;
    br_array_push(&s->scoped, &sys);
    br_system_add(e, name, phase, priority, br_scoped_run, sys);
}

/* ---- switching with a transition */

br_scene br_scene_switch(br_engine *e, const char *name, br_transition transition, float seconds)
{
    br_scenes_state *s = e->scenes;
    if (s->phase != BR_SWITCH_NONE) {
        br_log_warn("br_scene_switch '%s': another switch is running; ignored", name != NULL ? name : "");
        return br_scene_none();
    }
    br_scene target = br_scenes_start(e, name, BR_SCENE_ADDITIVE);
    br_scene_rec *rec = br_scene_get(e, target);
    if (rec == NULL) {
        return target;
    }
    rec->hold = true;
    br_asset_group_load_async(rec->group, NULL, NULL, NULL); /* loads while the old scene fades out */
    s->target = target;
    s->seconds = transition == BR_TRANSITION_FADE && seconds > 0.0f ? seconds : 0.0f;
    s->phase = BR_SWITCH_OUT;
    return target;
}

void br_scene_set_loading_scene(br_engine *e, const char *name)
{
    br_str_copy(e->scenes->loading_name, sizeof(e->scenes->loading_name), name != NULL ? name : "");
}

br_scene br_scene_switch_target(br_engine *e)
{
    return e->scenes->phase != BR_SWITCH_NONE ? e->scenes->target : br_scene_none();
}

bool br_scene_switching(br_engine *e)
{
    return e->scenes->phase != BR_SWITCH_NONE;
}

/* Fade towards `to` (0 or 1); true when there. */
static bool br_scenes_fade(br_scenes_state *s, float dt, float to)
{
    float step = s->seconds > 0.0f ? dt / s->seconds : 1.0f;
    if (to > s->alpha) {
        s->alpha = s->alpha + step >= to ? to : s->alpha + step;
    } else {
        s->alpha = s->alpha - step <= to ? to : s->alpha - step;
    }
    return s->alpha == to;
}

static void br_scenes_finish_target(br_engine *e)
{
    br_scenes_state *s = e->scenes;
    br_scene_rec *rec = br_scene_get(e, s->target);
    if (rec == NULL) {
        return;
    }
    rec->hold = false;
    rec->mode = BR_SCENE_SINGLE; /* building makes it active and unloads the rest */
    br_scenes_build(e, s->target);
}

static void br_scenes_switch_update(br_engine *e, float dt)
{
    br_scenes_state *s = e->scenes;
    br_scene_rec *target = br_scene_get(e, s->target);
    bool target_ready = target != NULL && br_asset_group_is_loaded(target->group);
    switch (s->phase) {
    case BR_SWITCH_NONE:
        break;
    case BR_SWITCH_OUT:
        if (br_scenes_fade(s, dt, 1.0f)) {
            if (target == NULL) {
                s->phase = BR_SWITCH_IN; /* the target was unloaded meanwhile: keep what is there */
                break;
            }
            br_scenes_unload_others(e, s->target);
            s->phase = BR_SWITCH_WAIT;
            if (s->loading_name[0] != '\0' && br_scene_valid(e, s->target)) {
                s->loading = br_scene_load(e, s->loading_name, BR_SCENE_ADDITIVE);
                if (br_scene_is_loaded(e, s->loading)) {
                    br_scene_set_active(e, s->loading);
                    s->phase = BR_SWITCH_LOADING_IN;
                }
            }
        }
        break;
    case BR_SWITCH_LOADING_IN:
        if (br_scenes_fade(s, dt, 0.0f)) {
            s->phase = BR_SWITCH_WAIT;
        }
        break;
    case BR_SWITCH_WAIT:
        if (target == NULL) {
            br_scene_unload(e, s->loading); /* the target was unloaded: show whatever is left */
            s->phase = BR_SWITCH_IN;
        } else if (target_ready) {
            if (br_scene_valid(e, s->loading) && s->alpha < 1.0f) {
                s->phase = BR_SWITCH_LOADING_OUT;
            } else {
                br_scene_unload(e, s->loading);
                br_scenes_finish_target(e);
                s->phase = BR_SWITCH_IN;
            }
        }
        break;
    case BR_SWITCH_LOADING_OUT:
        if (br_scenes_fade(s, dt, 1.0f)) {
            br_scene_unload(e, s->loading);
            br_scenes_finish_target(e);
            s->phase = BR_SWITCH_IN;
        }
        break;
    case BR_SWITCH_IN:
        if (br_scenes_fade(s, dt, 0.0f)) {
            s->phase = BR_SWITCH_NONE;
            s->target = br_scene_none();
            s->loading = br_scene_none();
        }
        break;
    }
}

/* Fade overlay: a black quad over the whole window. */
static void br_scenes_overlay(br_engine *e, br_render_ctx *rc, void *user)
{
    (void)user;
    br_scenes_state *s = e->scenes;
    if (s == NULL || s->alpha <= 0.0f) {
        return;
    }
    br_rect area = br_render_area(rc);
    uint32_t color = br_render_pack_color(br_color_make(0.0f, 0.0f, 0.0f, 1.0f), s->alpha);
    br_vertex2d v[4] = {
        { area.x, area.y, 0.0f, 0.0f, color },
        { area.x + area.width, area.y, 1.0f, 0.0f, color },
        { area.x + area.width, area.y + area.height, 1.0f, 1.0f, color },
        { area.x, area.y + area.height, 0.0f, 1.0f, color },
    };
    br_render_push_quad(rc, br_texture_white(e), v);
}

/* ---- per frame */

static void br_scenes_update(br_engine *e, float dt, void *user)
{
    (void)user;
    br_scenes_state *s = e->scenes;
    br_scenes_prune(e, &s->persistent);
    br_scenes_prune(e, &s->unowned);
    if (s->order.count > 0) {
        br_scenes_adopt(e);
    }
    /* async loads whose assets arrived: build (in load order, on a copy: builds may unload) */
    br_array ready;
    br_array_init(&ready, sizeof(br_scene));
    for (uint32_t i = 0; i < s->order.count; i++) {
        br_scene scene = *(br_scene *)br_array_at(&s->order, i);
        br_scene_rec *rec = br_scene_get(e, scene);
        if (rec != NULL && !rec->loaded && !rec->hold && br_asset_group_is_loaded(rec->group)) {
            br_array_push(&ready, &scene);
        }
    }
    for (uint32_t i = 0; i < ready.count; i++) {
        br_scenes_build(e, *(br_scene *)br_array_at(&ready, i));
    }
    br_array_free(&ready);
    br_scenes_switch_update(e, dt);
}

static void br_scenes_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_scenes_state *s = e->scenes;
    for (uint32_t i = 0; i < s->records.count; i++) {
        br_scene_rec *rec = br_array_at(&s->records, i);
        if (rec->alive) {
            br_asset_group_destroy(rec->group); /* nodes are already gone (shutdown destroys them first) */
        }
    }
    for (uint32_t i = 0; i < s->scoped.count; i++) {
        br_scoped_system *sys = *(br_scoped_system **)br_array_at(&s->scoped, i);
        br_free(sys->name);
        br_free(sys);
    }
    br_array_free(&s->records);
    br_array_free(&s->order);
    br_array_free(&s->persistent);
    br_array_free(&s->unowned);
    br_array_free(&s->scoped);
    br_array_free(&s->listeners);
    br_free(s);
    e->scenes = NULL;
}

void br_scenes_register(br_engine *e)
{
    br_scenes_state *s = br_alloc(sizeof(br_scenes_state));
    if (s == NULL) {
        return;
    }
    SDL_zerop(s);
    e->scenes = s;
    br_array_init(&s->records, sizeof(br_scene_rec));
    br_array_init(&s->order, sizeof(br_scene));
    br_array_init(&s->persistent, sizeof(br_node));
    br_array_init(&s->unowned, sizeof(br_node));
    br_array_init(&s->scoped, sizeof(br_scoped_system *));
    br_array_init(&s->listeners, sizeof(br_scene_listener));
    /* after assets.update (1000): groups are polled before scenes look at them */
    br_system_add(e, "scenes.update", BR_PHASE_INPUT, 1001, br_scenes_update, NULL);
    br_render_add_overlay(e, br_scenes_overlay, NULL);
    br_engine_on_shutdown(e, br_scenes_shutdown, NULL);
}
