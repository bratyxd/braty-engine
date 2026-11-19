/* br_animated_sprite: animated sprites (a sprite node + the "sprites.animated" component,
 * advanced by one system over the dense component array) and the GIF frame-set loader. */
#include "sprites/br_sprites_internal.h"
#include "braty/br_assets.h"

#include "stb/stb_image.h"

static br_anim_state *br_anim_get(br_engine *e, br_node n)
{
    return br_component_get(e, n, e->sprites->anim_component);
}

/* Shows frame `index`: sets the sprite texture (and the frame's default anchor if asked). */
static void br_anim_show(br_engine *e, br_node n, const br_anim_state *a)
{
    br_texture t = a->frames[a->current].texture;
    br_sprite_set_texture(e, n, t);
    if (a->update_anchor) {
        br_texture_info info;
        br_texture_get_info(e, t, &info);
        br_sprite_set_anchor(e, n, info.frame.default_anchor.x, info.frame.default_anchor.y);
    }
}

/* The owned frame array of node index i (slot in frames_by_node), growing the table. */
static br_anim_frame **br_anim_owned_slot(br_engine *e, uint32_t index)
{
    br_array *owned = &e->sprites->frames_by_node;
    if (index >= owned->count && !br_array_resize(owned, index + 1)) {
        return NULL;
    }
    return br_array_at(owned, index);
}

static void br_anim_node_destroyed(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_array *owned = &e->sprites->frames_by_node;
    if (n.index < owned->count) {
        br_anim_frame **slot = br_array_at(owned, n.index);
        br_free(*slot);
        *slot = NULL;
    }
}

bool br_animated_sprite_set_frames(br_engine *e, br_node n, const br_anim_frame *frames, uint32_t count)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a == NULL || frames == NULL || count == 0) {
        br_log_error("br_animated_sprite_set_frames: not an animated sprite or no frames");
        return false;
    }
    br_anim_frame **slot = br_anim_owned_slot(e, n.index);
    br_anim_frame *copy = br_alloc(sizeof(br_anim_frame) * count);
    if (slot == NULL || copy == NULL) {
        br_free(copy);
        return false;
    }
    SDL_memcpy(copy, frames, sizeof(br_anim_frame) * count);
    br_free(*slot);
    *slot = copy;
    a->frames = copy;
    a->count = count;
    a->current = 0;
    a->time = 0.0f;
    br_anim_show(e, n, a);
    return true;
}

br_node br_animated_sprite_create(br_engine *e, const br_anim_frame *frames, uint32_t count)
{
    br_node none = { 0, 0 };
    if (frames == NULL || count == 0) {
        br_log_error("br_animated_sprite_create: no frames");
        return none;
    }
    br_node n = br_sprite_create(e, frames[0].texture);
    br_anim_state *a = br_component_add(e, n, e->sprites->anim_component);
    if (a == NULL) {
        br_node_destroy(e, n);
        return none;
    }
    a->speed = 1.0f;
    a->loop = true;
    if (!br_animated_sprite_set_frames(e, n, frames, count)) {
        br_node_destroy(e, n);
        return none;
    }
    return n;
}

br_node br_animated_sprite_create_uniform(br_engine *e, const br_texture *textures, uint32_t count, float fps)
{
    br_node none = { 0, 0 };
    if (textures == NULL || count == 0 || fps <= 0.0f) {
        br_log_error("br_animated_sprite_create_uniform: no textures or fps <= 0");
        return none;
    }
    br_anim_frame *frames = br_alloc(sizeof(br_anim_frame) * count);
    if (frames == NULL) {
        return none;
    }
    for (uint32_t i = 0; i < count; i++) {
        frames[i].texture = textures[i];
        frames[i].duration = 1.0f / fps;
    }
    br_node n = br_animated_sprite_create(e, frames, count);
    br_free(frames);
    return n;
}

bool br_animated_sprite_is(br_engine *e, br_node n)
{
    return br_anim_get(e, n) != NULL;
}

void br_animated_sprite_play(br_engine *e, br_node n)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->playing = true;
    }
}

void br_animated_sprite_stop(br_engine *e, br_node n)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->playing = false;
    }
}

static void br_anim_goto(br_engine *e, br_node n, uint32_t frame, bool play)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a == NULL) {
        return;
    }
    a->current = frame < a->count ? frame : a->count - 1;
    a->time = 0.0f;
    a->playing = play;
    br_anim_show(e, n, a);
}

void br_animated_sprite_goto_and_play(br_engine *e, br_node n, uint32_t frame)
{
    br_anim_goto(e, n, frame, true);
}

void br_animated_sprite_goto_and_stop(br_engine *e, br_node n, uint32_t frame)
{
    br_anim_goto(e, n, frame, false);
}

void br_animated_sprite_set_speed(br_engine *e, br_node n, float speed)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->speed = speed;
    }
}

void br_animated_sprite_set_loop(br_engine *e, br_node n, bool loop)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->loop = loop;
    }
}

void br_animated_sprite_set_update_anchor(br_engine *e, br_node n, bool update_anchor)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->update_anchor = update_anchor;
    }
}

void br_animated_sprite_set_callbacks(br_engine *e, br_node n, br_anim_fn on_complete, br_anim_fn on_frame_change,
                                      br_anim_fn on_loop, void *user)
{
    br_anim_state *a = br_anim_get(e, n);
    if (a != NULL) {
        a->on_complete = on_complete;
        a->on_frame_change = on_frame_change;
        a->on_loop = on_loop;
        a->user = user;
    }
}

uint32_t br_animated_sprite_current_frame(br_engine *e, br_node n)
{
    br_anim_state *a = br_anim_get(e, n);
    return a != NULL ? a->current : 0;
}

uint32_t br_animated_sprite_total_frames(br_engine *e, br_node n)
{
    br_anim_state *a = br_anim_get(e, n);
    return a != NULL ? a->count : 0;
}

bool br_animated_sprite_playing(br_engine *e, br_node n)
{
    br_anim_state *a = br_anim_get(e, n);
    return a != NULL && a->playing;
}

/* ---- the animate system */

/* What happened to one sprite this frame (callbacks fire after the state is updated). */
typedef struct br_anim_events {
    bool changed;
    bool looped;
    bool completed;
} br_anim_events;

/* Advances one animation by dt seconds. Frames with duration <= 0 are skipped. */
static br_anim_events br_anim_advance(br_anim_state *a, float dt)
{
    br_anim_events ev = { false, false, false };
    if (!a->playing || a->count == 0) {
        return ev;
    }
    uint32_t start = a->current;
    a->time += dt * a->speed;
    /* bounded: a huge dt or all-zero durations cannot spin forever */
    for (uint32_t steps = 0; steps < 100000 && a->playing; steps++) {
        float duration = a->frames[a->current].duration;
        if (a->speed >= 0.0f && a->time >= duration) {
            a->time -= duration;
            if (a->current + 1 < a->count) {
                a->current += 1;
            } else if (a->loop) {
                a->current = 0;
                ev.looped = true;
            } else {
                a->time = duration;
                a->playing = false;
                ev.completed = true;
            }
        } else if (a->speed < 0.0f && a->time < 0.0f) {
            if (a->current > 0) {
                a->current -= 1;
            } else if (a->loop) {
                a->current = a->count - 1;
                ev.looped = true;
            } else {
                a->time = 0.0f;
                a->playing = false;
                ev.completed = true;
                break;
            }
            a->time += a->frames[a->current].duration;
        } else {
            break;
        }
    }
    ev.changed = a->current != start;
    return ev;
}

static void br_anim_system(br_engine *e, float dt, void *user)
{
    (void)user;
    uint32_t id = e->sprites->anim_component;
    /* count re-read every step: callbacks may add or remove animated sprites */
    for (uint32_t i = 0; i < br_component_count(e, id); i++) {
        br_node n = br_component_owner_at(e, id, i);
        br_anim_state *a = br_component_at(e, id, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        br_anim_events ev = br_anim_advance(a, dt);
        if (!ev.changed && !ev.looped && !ev.completed) {
            continue;
        }
        if (ev.changed) {
            br_anim_show(e, n, a);
        }
        /* copy before calling out: callbacks may move the component array */
        br_anim_state copy = *a;
        if (ev.changed && copy.on_frame_change != NULL) {
            copy.on_frame_change(e, n, copy.user);
        }
        if (ev.looped && copy.on_loop != NULL) {
            copy.on_loop(e, n, copy.user);
        }
        if (ev.completed && copy.on_complete != NULL) {
            copy.on_complete(e, n, copy.user);
        }
    }
}

/* ---- GIF loader: every frame becomes its own texture */

static void br_frame_set_free(br_engine *e, br_frame_set *fs)
{
    for (uint32_t i = 0; i < fs->count; i++) {
        br_texture_destroy(e, fs->frames[i].texture);
    }
    br_free(fs->frames);
    br_free(fs);
}

static void *br_gif_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    if (size > 0x7fffffff) {
        br_log_error("gif '%s': file too large", path);
        return NULL;
    }
    int *delays = NULL;
    int w = 0;
    int h = 0;
    int frames = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_gif_from_memory(bytes, (int)size, &delays, &w, &h, &frames, &channels, 4);
    if (pixels == NULL || frames <= 0) {
        br_log_error("gif '%s': cannot decode: %s", path, stbi_failure_reason());
        stbi_image_free(pixels);
        stbi_image_free(delays);
        return NULL;
    }
    br_frame_set *fs = br_alloc(sizeof(br_frame_set));
    br_anim_frame *list = br_alloc(sizeof(br_anim_frame) * (size_t)frames);
    bool ok = fs != NULL && list != NULL;
    if (ok) {
        fs->frames = list;
        fs->width = (uint32_t)w;
        fs->height = (uint32_t)h;
        size_t frame_bytes = (size_t)w * (size_t)h * 4;
        for (int i = 0; i < frames && ok; i++) {
            list[i].texture = br_texture_create_rgba8(e, (uint32_t)w, (uint32_t)h, pixels + frame_bytes * (size_t)i, NULL);
            /* browsers treat delays under 20 ms as 100 ms; so do we */
            int ms = delays != NULL ? delays[i] : 100;
            list[i].duration = (float)(ms < 20 ? 100 : ms) / 1000.0f;
            ok = list[i].texture.generation != 0;
            fs->count = (uint32_t)i + 1;
        }
    }
    stbi_image_free(pixels);
    stbi_image_free(delays);
    if (!ok) {
        br_log_error("gif '%s': out of memory or texture creation failed", path);
        if (fs != NULL) {
            fs->frames = list;
            br_frame_set_free(e, fs);
        } else {
            br_free(list);
        }
        return NULL;
    }
    return fs;
}

static void br_gif_unload(br_engine *e, void *asset)
{
    br_frame_set_free(e, asset);
}

const br_frame_set *br_frame_set_load(br_engine *e, const char *path)
{
    br_frame_set *fs = br_assets_load(e, path);
    if (fs != NULL && !br_str_equal(br_assets_type(e, path), "frames")) {
        br_log_error("br_frame_set_load: '%s' is not a frame set", path);
        return NULL;
    }
    return fs;
}

/* ---- module glue */

void br_animated_sprite_register(br_engine *e)
{
    br_component_desc desc = { "sprites.animated", sizeof(br_anim_state), NULL, 0 };
    e->sprites->anim_component = br_component_register(e, &desc);
    br_scene_on_node_destroy(e, br_anim_node_destroyed, NULL);
    br_system_add(e, "sprites.animated", BR_PHASE_UPDATE, 1000, br_anim_system, NULL);

    br_asset_loader_desc loader;
    SDL_zero(loader);
    loader.type_name = "frames";
    loader.extensions = "gif";
    loader.load = br_gif_load;
    loader.unload = br_gif_unload;
    br_assets_register_loader(e, &loader);
}

void br_animated_sprite_shutdown(br_engine *e)
{
    br_array *owned = &e->sprites->frames_by_node;
    for (uint32_t i = 0; i < owned->count; i++) {
        br_free(*(br_anim_frame **)br_array_at(owned, i));
    }
}
