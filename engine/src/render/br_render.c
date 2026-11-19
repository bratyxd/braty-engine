/* br_render: the stage walk (collect), masks, render-to-texture, hooks, render context API. */
#include "render/br_render_internal.h"
#include "scene/br_scene_internal.h"

/* Flags for the top node of a br_render_walk call (not inherited by children). */
#define BR_WALK_SKIP_EFFECTS 1u   /* the effects hook is drawing this node: draw it plainly */
#define BR_WALK_FROM_LAYER   2u   /* drawn by its render layer */
#define BR_WALK_AS_MASK      4u   /* drawn as a stencil mask: ignore visible / is-a-mask */

static void br_render_walk(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags);

/* ---- context state */

void br_render_ctx_changed(br_render_ctx *rc)
{
    br_render_state *r = rc->e->render;
    r->serial_counter += 1;
    rc->serial = r->serial_counter;
    rc->open_batch = -1;
}

static void br_render_update_transform(br_render_ctx *rc)
{
    br_rect a = rc->area;
    br_mat4 ortho = br_mat4_ortho(a.x, a.x + a.width, a.y + a.height, a.y, -1.0f, 1.0f);
    rc->transform = br_mat4_mul(ortho, br_mat4_from_mat3(rc->space));
    br_render_ctx_changed(rc);
}

br_mat3 br_render_node_transform(br_render_ctx *rc, br_node n)
{
    if (br_scene_record(rc->e, n) == NULL) {
        return br_mat3_identity();
    }
    return br_scene_transform_at(rc->e->scene, n.index)->world;
}

uint32_t br_render_pack_color(br_color color, float alpha)
{
    float a = br_clampf(color.a * alpha, 0.0f, 1.0f);
    return br_color_to_rgba8(br_color_make(color.r * a, color.g * a, color.b * a, a));
}

uint32_t br_render_node_color(br_render_ctx *rc, br_node n)
{
    br_node_record *rec = br_scene_record(rc->e, n);
    if (rec == NULL) {
        return 0;
    }
    return br_render_pack_color(rec->world_tint, rec->world_alpha);
}

const br_texture_info *br_render_texture_info(br_render_ctx *rc, br_texture t)
{
    uint32_t slot = t.index % BR_RENDER_INFO_CACHE;
    br_texture key = rc->info_keys[slot];
    if (key.generation == 0 || key.index != t.index || key.generation != t.generation) {
        br_texture_get_info(rc->e, t, &rc->info_cache[slot]);
        rc->info_keys[slot] = t;
    }
    return &rc->info_cache[slot];
}

br_blend_mode br_render_blend(br_render_ctx *rc)
{
    return rc->blend;
}

br_blend_mode br_render_set_blend(br_render_ctx *rc, br_blend_mode blend)
{
    br_blend_mode old = rc->blend;
    if (blend == BR_BLEND_INHERIT || blend >= BR_BLEND_BUILTIN_COUNT) {
        blend = BR_BLEND_NORMAL;
    }
    rc->blend = blend;
    return old;
}

float br_render_pixel_ratio(br_render_ctx *rc)
{
    return rc->area.width > 0.0f ? rc->pixel_width / rc->area.width : 1.0f;
}

br_rect br_render_area(br_render_ctx *rc)
{
    return rc->area;
}

br_mat3 br_render_space(br_render_ctx *rc)
{
    return rc->space;
}

br_mat3 br_render_set_space(br_render_ctx *rc, br_mat3 space)
{
    br_mat3 old = rc->space;
    rc->space = space;
    br_render_update_transform(rc);
    return old;
}

static void br_render_set_stencil(br_render_ctx *rc, br_stencil_mode mode, uint8_t ref)
{
    rc->stencil = mode;
    rc->stencil_ref = ref;
    br_render_ctx_changed(rc);
}

/* ---- masks */

/* Draws the mask node's geometry with the current stencil mode. */
static void br_render_draw_mask_node(br_render_ctx *rc, uint32_t mask_index)
{
    br_scene_state *s = rc->e->scene;
    br_scene_refresh_subtree(s, mask_index); /* the mask may live outside the stage */
    br_mat3 saved = br_render_set_space(rc, br_scene_group_space(s, mask_index));
    br_render_walk(rc, mask_index, BR_BLEND_NORMAL, BR_WALK_AS_MASK | BR_WALK_SKIP_EFFECTS);
    br_render_set_space(rc, saved);
}

/* A quad covering the whole target (inverse masks). */
static void br_render_draw_full_target(br_render_ctx *rc)
{
    br_mat3 saved = br_render_set_space(rc, br_mat3_identity());
    br_rect a = rc->area;
    br_vertex2d v[4];
    SDL_zeroa(v);
    v[0].x = a.x;
    v[0].y = a.y;
    v[1].x = a.x + a.width;
    v[1].y = a.y;
    v[2].x = a.x + a.width;
    v[2].y = a.y + a.height;
    v[3].x = a.x;
    v[3].y = a.y + a.height;
    br_render_push_quad(rc, br_texture_white(rc->e), v);
    br_render_set_space(rc, saved);
}

typedef struct br_mask_saved {
    br_mask_kind kind;
    bool         inverse;
    uint32_t     mask_index;
    bool         scissor_on;
    SDL_Rect     scissor;
} br_mask_saved;

/* Stencil levels: content inside d nested masks is drawn where stencil == d.
 * Normal:  INCR the mask where == d; content == d+1; DECR the mask where == d+1.
 * Inverse: INCR the whole target, DECR the mask (so outside = d+1); undo in reverse order. */
static bool br_render_push_mask(br_render_ctx *rc, uint32_t index, br_mask_saved *saved)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_node_record *rec = br_scene_record_at(s, index);
    saved->kind = BR_MASK_NONE;
    br_node_record *mrec = br_scene_record(e, rec->mask);
    if (mrec == NULL || (rec->mask_kind != BR_MASK_SCISSOR && rec->mask_kind != BR_MASK_STENCIL)) {
        return true;
    }
    saved->kind = rec->mask_kind;
    saved->inverse = rec->mask_inverse;
    saved->mask_index = rec->mask.index;
    saved->scissor_on = rc->scissor_on;
    saved->scissor = rc->scissor;

    if (saved->kind == BR_MASK_SCISSOR) {
        br_rect bounds = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
        bool has = false;
        br_scene_bounds(s, e, saved->mask_index, br_scene_chain_transform(s, saved->mask_index, false), &bounds, &has);
        float sx = rc->pixel_width / rc->area.width;
        float sy = rc->pixel_height / rc->area.height;
        float x0 = SDL_floorf((bounds.x - rc->area.x) * sx);
        float y0 = SDL_floorf((bounds.y - rc->area.y) * sy);
        float x1 = SDL_ceilf((bounds.x + bounds.width - rc->area.x) * sx);
        float y1 = SDL_ceilf((bounds.y + bounds.height - rc->area.y) * sy);
        br_rect px = br_rect_make(x0, y0, x1 - x0, y1 - y0);
        px = br_rect_intersection(px, br_rect_make(0.0f, 0.0f, rc->pixel_width, rc->pixel_height));
        if (rc->scissor_on) {
            br_rect old = br_rect_make((float)rc->scissor.x, (float)rc->scissor.y, (float)rc->scissor.w,
                                       (float)rc->scissor.h);
            px = br_rect_intersection(px, old);
        }
        if (!has || br_rect_is_empty(px)) {
            return false; /* nothing visible: skip the subtree */
        }
        rc->scissor_on = true;
        rc->scissor.x = (int)px.x;
        rc->scissor.y = (int)px.y;
        rc->scissor.w = (int)px.width;
        rc->scissor.h = (int)px.height;
        br_render_ctx_changed(rc);
        return true;
    }

    if (!rc->has_stencil || rc->stencil_depth == 255) {
        if (!e->render->warned_no_stencil) {
            br_log_warn("stencil mask ignored: target has no stencil buffer or 255 masks are nested");
            e->render->warned_no_stencil = true;
        }
        saved->kind = BR_MASK_NONE;
        return true;
    }
    uint8_t d = rc->stencil_depth;
    br_blend_mode blend = rc->blend;
    if (saved->inverse) {
        br_render_set_stencil(rc, BR_STENCIL_INCR, d);
        br_render_draw_full_target(rc);
        br_render_set_stencil(rc, BR_STENCIL_DECR, (uint8_t)(d + 1));
        br_render_draw_mask_node(rc, saved->mask_index);
    } else {
        br_render_set_stencil(rc, BR_STENCIL_INCR, d);
        br_render_draw_mask_node(rc, saved->mask_index);
    }
    rc->stencil_depth = (uint8_t)(d + 1);
    br_render_set_stencil(rc, BR_STENCIL_TEST, rc->stencil_depth);
    rc->blend = blend;
    return true;
}

static void br_render_pop_mask(br_render_ctx *rc, const br_mask_saved *saved)
{
    if (saved->kind == BR_MASK_SCISSOR) {
        rc->scissor_on = saved->scissor_on;
        rc->scissor = saved->scissor;
        br_render_ctx_changed(rc);
        return;
    }
    if (saved->kind != BR_MASK_STENCIL) {
        return;
    }
    uint8_t d = (uint8_t)(rc->stencil_depth - 1);
    br_blend_mode blend = rc->blend;
    if (saved->inverse) {
        br_render_set_stencil(rc, BR_STENCIL_INCR, d);
        br_render_draw_mask_node(rc, saved->mask_index);
        br_render_set_stencil(rc, BR_STENCIL_DECR, (uint8_t)(d + 1));
        br_render_draw_full_target(rc);
    } else {
        br_render_set_stencil(rc, BR_STENCIL_DECR, (uint8_t)(d + 1));
        br_render_draw_mask_node(rc, saved->mask_index);
    }
    rc->stencil_depth = d;
    br_render_set_stencil(rc, d > 0 ? BR_STENCIL_TEST : BR_STENCIL_OFF, d);
    rc->blend = blend;
}

/* ---- the walk */

static bool br_render_record_needs_effects(br_engine *e, const br_node_record *rec)
{
    bool effect_mask = (rec->mask_kind == BR_MASK_ALPHA || rec->mask_kind == BR_MASK_COLOR) &&
                       br_scene_record(e, rec->mask) != NULL;
    return rec->filters != NULL || rec->cache_as_texture || effect_mask || rec->blend_mode >= BR_BLEND_ADVANCED_FIRST;
}

static void br_render_walk(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_render_state *r = e->render;
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->destroyed || br_scene_is_culled(s, index)) {
        return;
    }
    bool as_mask = (flags & BR_WALK_AS_MASK) != 0;
    /* a mask node is not drawn as content, but an explicit offscreen render of it (the top node
     * of br_render_subtree_to_texture) is allowed: alpha/colour masks render their mask that way */
    bool hidden_mask = rec->mask_users > 0 && (flags & BR_WALK_SKIP_EFFECTS) == 0;
    if (!as_mask && (!rec->visible || !rec->renderable || hidden_mask)) {
        return;
    }
    if ((flags & BR_WALK_FROM_LAYER) == 0 && br_scene_record(e, rec->layer) != NULL) {
        return; /* its render layer draws it */
    }
    br_node n = br_scene_handle(s, index);
    br_blend_mode blend = inherited;
    if (rec->blend_mode != BR_BLEND_INHERIT && rec->blend_mode < BR_BLEND_BUILTIN_COUNT) {
        blend = (br_blend_mode)rec->blend_mode;
    }
    if ((flags & BR_WALK_SKIP_EFFECTS) == 0 && r->effects_fn != NULL && br_render_record_needs_effects(e, rec)) {
        rc->blend = blend;
        if (r->effects_fn(e, n, rc, r->effects_user)) {
            return;
        }
        rec = br_scene_record_at(s, index);
    }

    br_mask_saved mask;
    if (!as_mask && !br_render_push_mask(rc, index, &mask)) {
        return;
    }
    if (as_mask) {
        mask.kind = BR_MASK_NONE;
    }
    rec = br_scene_record_at(s, index);
    br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
    if (type->desc.render != NULL) {
        rc->blend = blend;
        type->desc.render(e, n, br_scene_type_data(s, index), rc);
        r->building.nodes_drawn += 1;
        rec = br_scene_record_at(s, index);
    }

    br_mat3 saved_space = rc->space;
    if (rec->render_group) {
        br_render_set_space(rc, br_mat3_mul(rc->space, br_scene_transform_at(s, index)->world));
    }
    for (uint32_t i = 0; i < rec->children.count; i++) {
        uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
        br_render_walk(rc, child, blend, 0);
        rec = br_scene_record_at(s, index);
    }
    if (rec->render_group) {
        br_render_set_space(rc, saved_space);
    }
    for (uint32_t i = 0; i < rec->layer_members.count; i++) {
        br_node member = *(br_node *)br_array_at(&rec->layer_members, i);
        if (br_scene_record(e, member) == NULL) {
            continue;
        }
        br_mat3 saved = br_render_set_space(rc, br_scene_group_space(s, member.index));
        br_render_walk(rc, member.index, blend, BR_WALK_FROM_LAYER);
        br_render_set_space(rc, saved);
        rec = br_scene_record_at(s, index);
    }
    br_render_pop_mask(rc, &mask);
}

/* ---- targets */

/* Points the context at a target showing world rect `area`. */
static void br_render_ctx_set_target(br_render_ctx *rc, br_texture target, br_rect area, float pixel_width,
                                     float pixel_height, bool has_stencil)
{
    rc->target = target;
    rc->area = area;
    rc->pixel_width = pixel_width;
    rc->pixel_height = pixel_height;
    rc->has_stencil = has_stencil;
    rc->stencil = BR_STENCIL_OFF;
    rc->stencil_ref = 0;
    rc->stencil_depth = 0;
    rc->scissor_on = false;
    rc->space = br_mat3_identity();
    br_render_update_transform(rc);
}

static bool br_render_is_on_stage(br_scene_state *s, uint32_t index)
{
    while (index != BR_NO_NODE) {
        if (index == s->root.index) {
            return true;
        }
        index = br_scene_record_at(s, index)->parent;
    }
    return false;
}

void br_render_subtree_to_texture(br_render_ctx *rc, br_node n, br_texture target, br_rect area, bool clear,
                                  br_color clear_color)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_render_target *rt = br_texture_render_target(e, target);
    if (br_scene_record(e, n) == NULL || rt == NULL || area.width <= 0.0f || area.height <= 0.0f) {
        return;
    }
    if (!br_render_is_on_stage(s, n.index)) {
        br_scene_refresh_subtree(s, n.index); /* the transform system only walks the stage */
    }
    br_render_ctx saved = *rc;
    br_batch_push_pass(rc, target, clear, clear_color);
    br_render_ctx_set_target(rc, target, area, (float)rt->width, (float)rt->height, rt->depth_stencil != NULL);
    br_render_set_space(rc, br_scene_group_space(s, n.index));
    br_render_walk(rc, n.index, BR_BLEND_NORMAL, BR_WALK_SKIP_EFFECTS);

    *rc = saved;
    br_batch_push_pass(rc, saved.target, false, clear_color);
    br_render_ctx_changed(rc);
}

void br_render_to_texture(br_engine *e, br_node n, br_texture target)
{
    br_rtt_job job = { n, target };
    br_array_push(&e->render->rtt_jobs, &job);
}

/* ---- hooks, stats */

void br_render_set_effects_hook(br_engine *e, br_render_effects_fn fn, void *user)
{
    e->render->effects_fn = fn;
    e->render->effects_user = user;
}

void br_render_add_overlay(br_engine *e, br_render_overlay_fn fn, void *user)
{
    br_overlay_entry entry = { fn, user };
    br_array_push(&e->render->overlays, &entry);
}

void br_render_get_stats(br_engine *e, br_render_stats *out)
{
    *out = e->render->stats;
}

/* ---- systems */

static void br_render_collect_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_render_state *r = e->render;
    br_batch_reset(e);
    int w, h, pw, ph;
    br_engine_window_size(e, &w, &h);
    br_engine_pixel_size(e, &pw, &ph);
    if (br_engine_gpu(e) == NULL || w <= 0 || h <= 0 || pw <= 0 || ph <= 0) {
        br_array_clear(&r->rtt_jobs);
        return;
    }
    br_texture_white(e);   /* make the built-in textures now: replay must not create textures */
    br_texture_missing(e);
    br_texture frame = { 0, 0 };
    br_render_ctx rc;
    SDL_zero(rc);
    rc.e = e;
    rc.open_batch = -1;
    rc.blend = BR_BLEND_NORMAL;
    br_rect window_area = br_rect_make(0.0f, 0.0f, (float)w, (float)h);
    br_render_ctx_set_target(&rc, frame, window_area, (float)pw, (float)ph, true);

    for (uint32_t i = 0; i < r->rtt_jobs.count; i++) {
        br_rtt_job *job = br_array_at(&r->rtt_jobs, i);
        br_texture_info info;
        if (br_texture_get_info(e, job->target, &info)) {
            br_rect area = br_rect_make(0.0f, 0.0f, info.width, info.height);
            br_render_subtree_to_texture(&rc, job->node, job->target, area, true, br_color_make(0, 0, 0, 0));
        }
    }
    br_array_clear(&r->rtt_jobs);

    br_batch_push_pass(&rc, frame, false, br_color_make(0, 0, 0, 0));
    br_render_walk(&rc, e->scene->root.index, BR_BLEND_NORMAL, 0);
    for (uint32_t i = 0; i < r->overlays.count; i++) {
        br_overlay_entry *o = br_array_at(&r->overlays, i);
        br_render_set_space(&rc, br_mat3_identity());
        rc.blend = BR_BLEND_NORMAL;
        o->fn(e, &rc, o->user);
    }
    br_batch_finish(e);
    r->collected = true;
}

static void br_render_submit_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_render_state *r = e->render;
    if (r->collected) {
        br_batch_replay(e);
    }
    r->collected = false;
}

static void br_render_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_render_state *r = e->render;
    br_texture_shutdown(e);
    br_array_free(&r->cmds);
    br_array_free(&r->customs);
    br_array_free(&r->vertices);
    br_array_free(&r->indices);
    br_array_free(&r->uniforms);
    br_array_free(&r->rtt_jobs);
    br_array_free(&r->overlays);
    br_free(r);
    e->render = NULL;
}

void br_render_register(br_engine *e)
{
    br_render_state *r = br_alloc(sizeof(br_render_state));
    if (r == NULL) {
        return;
    }
    e->render = r;
    br_texture_init(e);
    br_array_init(&r->cmds, sizeof(br_draw_cmd));
    br_array_init(&r->customs, sizeof(br_custom_cmd));
    br_array_init(&r->vertices, sizeof(br_batch_vertex));
    br_array_init(&r->indices, sizeof(uint16_t));
    br_array_init(&r->uniforms, 1);
    br_array_init(&r->rtt_jobs, sizeof(br_rtt_job));
    br_array_init(&r->overlays, sizeof(br_overlay_entry));
    br_engine_on_shutdown(e, br_render_shutdown, NULL);
    br_sprite_register(e);
    br_system_add(e, "render.collect", BR_PHASE_PRE_RENDER, 1000, br_render_collect_system, NULL);
    br_system_add(e, "render.submit", BR_PHASE_RENDER, 0, br_render_submit_system, NULL);
}
