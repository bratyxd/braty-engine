/* br_scene_reload: hot reload. Scene files loaded from disk are watched; a system polls their
 * modification time (br_file_get_modify_time) every N frames and reloads changed ones in place. */
#include "data/br_data_internal.h"

int64_t br_data_modify_time(br_engine *e, const char *path)
{
    int64_t time = 0;
    br_file_get_modify_time(e, path, &time); /* 0 when there is no loose file (packed scenes never reload) */
    return time;
}

void br_data_watch_add(br_engine *e, const char *path, br_node root)
{
    br_data_state *s = e->data;
    for (uint32_t i = 0; i < s->watches.count; i++) {
        br_data_watch *w = br_array_at(&s->watches, i);
        if (br_node_equal(w->root, root)) {
            br_str_copy(w->path, sizeof(w->path), path);
            w->modify_time = br_data_modify_time(e, path);
            return;
        }
    }
    br_data_watch w;
    SDL_zero(w);
    br_str_copy(w.path, sizeof(w.path), path);
    w.root = root;
    w.modify_time = br_data_modify_time(e, path);
    br_array_push(&s->watches, &w);
}

uint32_t br_scene_file_poll(br_engine *e)
{
    br_data_state *s = e->data;
    uint32_t reloaded = 0;
    for (uint32_t i = 0; i < s->watches.count; i++) {
        br_data_watch *w = br_array_at(&s->watches, i);
        if (!br_node_valid(e, w->root)) {
            br_array_remove_swap(&s->watches, i);
            i--;
            continue;
        }
        int64_t t = br_data_modify_time(e, w->path);
        if (t == 0 || t == w->modify_time) {
            continue;
        }
        w->modify_time = t;
        char path[BR_DATA_PATH_SIZE];
        br_node root = w->root;
        br_str_copy(path, sizeof(path), w->path);
        // ponytail: edited prefab files are re-read only when a scene that uses them changes; watch prefab mtimes too if that bites.
        br_data_prefabs_free(e);
        if (br_scene_file_reload(e, path, root)) {
            reloaded++;
        }
    }
    return reloaded;
}

void br_scene_file_set_hot_reload(br_engine *e, bool enabled, uint32_t poll_frames)
{
    e->data->hot_reload = enabled;
    e->data->poll_frames = poll_frames > 0 ? poll_frames : 30;
}

static void br_data_reload_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_data_state *s = e->data;
    if (!s->hot_reload || s->watches.count == 0) {
        return;
    }
    s->frame_counter++;
    if (s->frame_counter < s->poll_frames) {
        return;
    }
    s->frame_counter = 0;
    br_scene_file_poll(e);
}

void br_data_reload_register(br_engine *e)
{
    br_system_add(e, "data.hot_reload", BR_PHASE_INPUT, 0, br_data_reload_system, NULL);
}
