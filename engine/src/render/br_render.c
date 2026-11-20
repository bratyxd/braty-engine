/* br_render: the stage walk (collect), masks, render-to-texture, hooks, render context API. */
#include "render/br_render_internal.h"
#include "scene/br_scene_internal.h"
#include "braty/br_profiler.h"

/* Flags for the top node of a br_render_walk call (not inherited by children). */
#define BR_WALK_SKIP_EFFECTS 1u   /* the effects hook is drawing this node: draw it plainly */
#define BR_WALK_FROM_LAYER   2u   /* drawn by its render layer */
#define BR_WALK_AS_MASK      4u   /* drawn as a stencil mask: ignore visible / is-a-mask */

static void br_render_walk(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags);
static void br_render_walk_node(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags);

/* ---- context state */

void br_render_ctx_changed(br_render_ctx *rc)
{
    if (rc->open_batch >= 0) {
        br_profiler_count(rc->e, BR_PROFILER_BREAK_STATE, 1);
    }
    br_render_state *r = rc->e->render;
    r->serial_counter += 1;
    rc->serial = r->serial_counter;
    rc->open_batch = -1;
}

static void br_render_update_transform(br_render_ctx *rc)
{
    br_rect a = rc->area;
    br_mat4 base = rc->has_projection ? rc->projection : br_mat4_ortho(a.x, a.x + a.width, a.y + a.height, a.y, -1.0f, 1.0f);
    rc->transform = br_mat4_mul(base, br_mat4_from_mat3(rc->space));
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
    if (rc->stencil == BR_STENCIL_INCR || rc->stencil == BR_STENCIL_DECR) {
        return 0xFFFFFFFFu; /* stencil mask geometry: a mask with alpha 0 still clips */
    }
    return rec->world_color; /* packed by the transform system whenever world tint/alpha change */
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
    rc->mask_draws += 1; /* a node in the mask masked by it would draw the mask again, forever */
    br_render_walk(rc, mask_index, BR_BLEND_NORMAL, BR_WALK_AS_MASK | BR_WALK_SKIP_EFFECTS);
    rc->mask_draws -= 1;
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

br_rect br_render_rect_to_pixels(br_render_ctx *rc, br_rect r)
{
    float x0 = 1e30f;
    float y0 = 1e30f;
    float x1 = -1e30f;
    float y1 = -1e30f;
    if (rc->has_projection) {
        /* the view's projection maps root space to clip space of the whole target */
        for (int i = 0; i < 4; i++) {
            float wx = (i & 1) ? r.x + r.width : r.x;
            float wy = (i & 2) ? r.y + r.height : r.y;
            br_vec3 clip = br_mat4_transform_point(rc->projection, br_vec3_make(wx, wy, 0.0f));
            float px = (clip.x + 1.0f) * 0.5f * rc->target_width;
            float py = (1.0f - clip.y) * 0.5f * rc->target_height;
            x0 = SDL_min(x0, px);
            y0 = SDL_min(y0, py);
            x1 = SDL_max(x1, px);
            y1 = SDL_max(y1, py);
        }
    } else {
        float sx = rc->pixel_width / rc->area.width;
        float sy = rc->pixel_height / rc->area.height;
        x0 = (r.x - rc->area.x) * sx;
        y0 = (r.y - rc->area.y) * sy;
        x1 = (r.x + r.width - rc->area.x) * sx;
        y1 = (r.y + r.height - rc->area.y) * sy;
    }
    /* a little slack: edges already on the pixel grid stay there after the float maths */
    const float slack = 1.0f / 256.0f;
    x0 = SDL_floorf(x0 + slack);
    y0 = SDL_floorf(y0 + slack);
    x1 = SDL_ceilf(x1 - slack);
    y1 = SDL_ceilf(y1 - slack);
    return br_rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* Stencil levels: content inside d nested masks is drawn where stencil == d.
 * Normal:  INCR the mask where == d; content == d+1; DECR the mask where == d+1.
 * Inverse: INCR the whole target, DECR the mask (so outside = d+1); undo in reverse order. */
static bool br_render_push_mask(br_render_ctx *rc, uint32_t index, br_mask_saved *saved)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_node_record *rec = br_scene_record_at(s, index);
    saved->kind = BR_MASK_NONE;
    if ((rec->mask_kind != BR_MASK_SCISSOR && rec->mask_kind != BR_MASK_STENCIL) || br_scene_record(e, rec->mask) == NULL) {
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
        if (rc->target_width <= 0.0f) {
            saved->kind = BR_MASK_NONE; /* captured for 3D (canvas3d): no pixels to clip */
            return true;
        }
        /* target pixels, clipped to the whole target; a view's own scissor (rc->scissor below)
         * clips to its viewport */
        br_rect px = br_render_rect_to_pixels(rc, bounds);
        px = br_rect_intersection(px, br_rect_make(0.0f, 0.0f, rc->target_width, rc->target_height));
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

static bool br_render_record_needs_effects(br_render_ctx *rc, const br_node_record *rec)
{
    /* masks met while a mask is drawn are ignored (br_render_ctx.mask_draws) */
    bool effect_mask = rc->mask_draws == 0 && (rec->mask_kind == BR_MASK_ALPHA || rec->mask_kind == BR_MASK_COLOR) &&
                       br_scene_record(rc->e, rec->mask) != NULL;
    return rec->filters != NULL || rec->cache_as_texture || effect_mask || rec->blend_mode >= BR_BLEND_ADVANCED_FIRST;
}

/* The walk recurses per level: past BR_RENDER_MAX_DEPTH it logs once and stops (deep chains,
 * or a render-layer loop built by rearranging the tree after attaching). */
static void br_render_walk(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags)
{
    if (rc->depth >= BR_RENDER_MAX_DEPTH) {
        if (!rc->e->render->warned_depth) {
            br_log_warn("render: hierarchy deeper than %d levels (or a render-layer loop): deeper nodes are not drawn",
                        BR_RENDER_MAX_DEPTH);
            rc->e->render->warned_depth = true;
        }
        return;
    }
    rc->depth += 1;
    br_render_walk_node(rc, index, inherited, flags);
    rc->depth -= 1;
}

static void br_render_walk_node(br_render_ctx *rc, uint32_t index, br_blend_mode inherited, uint32_t flags)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_render_state *r = e->render;
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->destroyed || (s->cull_frame != 0 && rec->culled_frame == s->cull_frame)) {
        br_profiler_count(e, BR_PROFILER_NODES_CULLED, rec->destroyed ? 0 : 1);
        return; /* destroyed, or culled (br_scene_is_culled) */
    }
    bool as_mask = (flags & BR_WALK_AS_MASK) != 0;
    /* a mask node is not drawn as content, but an explicit offscreen render of it (the top node
     * of br_render_subtree_to_texture) is allowed: alpha/colour masks render their mask that way */
    bool hidden_mask = rec->mask_users > 0 && (flags & BR_WALK_SKIP_EFFECTS) == 0;
    if (!as_mask && (!rec->visible || !rec->renderable || hidden_mask)) {
        return;
    }
    if ((flags & BR_WALK_FROM_LAYER) == 0 && rec->layer.generation != 0 && br_scene_record(e, rec->layer) != NULL) {
        return; /* its render layer draws it */
    }
    br_node n = br_scene_handle(s, index);
    br_blend_mode blend = inherited;
    if (rec->blend_mode != BR_BLEND_INHERIT && rec->blend_mode < BR_BLEND_BUILTIN_COUNT) {
        blend = (br_blend_mode)rec->blend_mode;
    }
    uint32_t saved_bits = rc->layer_bits;
    if (rec->layer_bits != 0) {
        rc->layer_bits = rec->layer_bits;
    }
    /* a node on a layer this camera does not draw: skip its own drawing, keep walking children */
    bool drawn = as_mask || (rc->layer_bits & rc->culling_mask) != 0;
    if (drawn && (flags & BR_WALK_SKIP_EFFECTS) == 0 && r->effects_fn != NULL && br_render_record_needs_effects(rc, rec)) {
        rc->blend = blend;
        if (r->effects_fn(e, n, rc, r->effects_user)) {
            rc->layer_bits = saved_bits;
            return;
        }
        rec = br_scene_record_at(s, index);
    }

    br_mask_saved mask;
    mask.kind = BR_MASK_NONE;
    if (!as_mask && rc->mask_draws == 0 && rec->mask.generation != 0 && !br_render_push_mask(rc, index, &mask)) {
        rc->layer_bits = saved_bits;
        return;
    }
    rec = br_scene_record_at(s, index);
    br_node_type_entry *type = br_scene_type_entry(s, rec->type_id);
    if (drawn && type->desc.render != NULL) {
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
        uint32_t child = ((const uint32_t *)rec->children.data)[i];
        br_render_walk(rc, child, blend, 0);
        rec = br_scene_record_at(s, index); /* a render callback may have created nodes */
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
    rc->layer_bits = saved_bits;
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
    rc->has_projection = false;
    rc->target_width = pixel_width;
    rc->target_height = pixel_height;
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
    br_render_subtree_to_texture_ex(rc, n, target, area, clear, clear_color, true);
}

void br_render_subtree_to_texture_ex(br_render_ctx *rc, br_node n, br_texture target, br_rect area, bool clear,
                                     br_color clear_color, bool own_color)
{
    br_engine *e = rc->e;
    br_scene_state *s = e->scene;
    br_render_target *rt = br_texture_render_target(e, target);
    if (br_scene_record(e, n) == NULL || rt == NULL || area.width <= 0.0f || area.height <= 0.0f) {
        return;
    }
    if (!own_color) {
        br_scene_refresh_subtree_uncolored(s, n.index);
    } else if (!br_render_is_on_stage(s, n.index)) {
        br_scene_refresh_subtree(s, n.index); /* the transform system only walks the stage */
    }
    br_render_ctx saved = *rc;
    br_batch_push_pass(rc, target, clear, clear_color);
    br_render_ctx_set_target(rc, target, area, (float)rt->width, (float)rt->height, rt->depth_stencil != NULL);
    br_render_set_space(rc, br_scene_group_space(s, n.index));
    if (br_scene_record_at(s, n.index)->mask_users > 0) {
        rc->mask_draws += 1; /* an alpha/colour mask: masks inside it are ignored (restored below) */
    }
    br_render_walk(rc, n.index, BR_BLEND_NORMAL, BR_WALK_SKIP_EFFECTS);
    if (!own_color) {
        br_scene_refresh_subtree(s, n.index); /* vertices are recorded: put the real colours back */
    }

    *rc = saved;
    br_batch_push_pass(rc, saved.target, false, clear_color);
    br_render_ctx_changed(rc);
}

bool br_render_capture_subtree(br_engine *e, br_node n, br_mat4 projection, br_rect area, float pixel_ratio,
                               br_array *out)
{
    br_render_state *r = e->render;
    br_scene_state *s = e->scene;
    if (r == NULL || br_scene_record(e, n) == NULL || br_engine_gpu(e) == NULL || area.width <= 0.0f ||
        area.height <= 0.0f) {
        return false;
    }
    if (!br_render_is_on_stage(s, n.index)) {
        br_scene_refresh_subtree(s, n.index);
    }
    br_render_ctx rc;
    SDL_zero(rc);
    rc.e = e;
    rc.open_batch = -1;
    rc.blend = BR_BLEND_NORMAL;
    rc.culling_mask = 0xFFFFFFFFu;
    rc.layer_bits = 1u;
    br_texture none = { 0, 0 };
    float ratio = pixel_ratio > 0.0f ? pixel_ratio : 1.0f;
    br_render_ctx_set_target(&rc, none, area, area.width * ratio, area.height * ratio, false);
    rc.has_projection = true;
    rc.projection = projection;
    rc.target_width = 0.0f; /* no pixels: scissor masks are skipped */
    rc.target_height = 0.0f;
    br_render_set_space(&rc, br_scene_group_space(s, n.index));
    uint32_t first = r->cmds.count;
    br_render_walk(&rc, n.index, BR_BLEND_NORMAL, BR_WALK_SKIP_EFFECTS);
    for (uint32_t i = first; i < r->cmds.count; i++) {
        br_array_push(out, br_array_at(&r->cmds, i));
    }
    r->cmds.count = first; /* the stage replay never sees them */
    return true;
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

br_render_effects_fn br_render_effects_hook(br_engine *e, void **user)
{
    if (user != NULL) {
        *user = e->render->effects_user;
    }
    return e->render->effects_fn;
}

void br_render_add_overlay(br_engine *e, br_render_overlay_fn fn, void *user)
{
    br_overlay_entry entry = { fn, user };
    br_array_push(&e->render->overlays, &entry);
}

void br_render_set_views(br_engine *e, const br_render_view *views, uint32_t count)
{
    br_render_state *r = e->render;
    br_array_clear(&r->views);
    if (views != NULL && count > 0) {
        br_array_push_n(&r->views, views, count);
    }
}

void br_render_get_stats(br_engine *e, br_render_stats *out)
{
    *out = e->render->stats;
}

/* ---- systems */

/* Layer bits a node inherits from its ancestors (1 = the stage's). */
static uint32_t br_render_inherited_bits(br_scene_state *s, uint32_t index)
{
    uint32_t parent = br_scene_record_at(s, index)->parent;
    while (parent != BR_NO_NODE) {
        const br_node_record *rec = br_scene_record_at(s, parent);
        if (rec->layer_bits != 0) {
            return rec->layer_bits;
        }
        parent = rec->parent;
    }
    return 1u;
}

/* Draws every view of br_render_set_views (cameras), in order. */
static void br_render_draw_views(br_render_ctx *rc, float window_w, float window_h, float pixel_w, float pixel_h)
{
    br_engine *e = rc->e;
    br_render_state *r = e->render;
    br_scene_state *s = e->scene;
    bool pass_open = false;
    br_texture current = { 0, 0 };
    for (uint32_t i = 0; i < r->views.count; i++) {
        const br_render_view *view = br_array_at(&r->views, i);
        /* target size in units and pixels */
        float units_w = window_w;
        float units_h = window_h;
        float target_w = pixel_w;
        float target_h = pixel_h;
        bool has_stencil = true;
        if (view->target.generation != 0) {
            br_render_target *rt = br_texture_render_target(e, view->target);
            br_texture_info info;
            if (rt == NULL || !br_texture_get_info(e, view->target, &info) || info.width <= 0.0f || info.height <= 0.0f) {
                continue;
            }
            units_w = info.width;
            units_h = info.height;
            target_w = (float)rt->width;
            target_h = (float)rt->height;
            has_stencil = rt->depth_stencil != NULL;
        }
        br_node root = br_node_valid(e, view->root) ? view->root : s->root;
        if (br_scene_record(e, root) == NULL) {
            continue;
        }
        if (!pass_open || current.index != view->target.index || current.generation != view->target.generation) {
            br_batch_push_pass(rc, view->target, false, br_color_make(0, 0, 0, 0));
            pass_open = true;
            current = view->target;
        }
        float sx = target_w / units_w;
        float sy = target_h / units_h;
        br_rect px = br_rect_make(SDL_roundf(view->viewport.x * sx), SDL_roundf(view->viewport.y * sy),
                                  SDL_roundf(view->viewport.width * sx), SDL_roundf(view->viewport.height * sy));
        px = br_rect_intersection(px, br_rect_make(0.0f, 0.0f, target_w, target_h));
        if (br_rect_is_empty(px)) {
            continue;
        }
        br_render_ctx_set_target(rc, view->target, view->area, px.width, px.height, has_stencil);
        rc->target_width = target_w;
        rc->target_height = target_h;
        rc->scissor_on = true;
        rc->scissor.x = (int)px.x;
        rc->scissor.y = (int)px.y;
        rc->scissor.w = (int)px.width;
        rc->scissor.h = (int)px.height;
        rc->has_projection = true;
        if (view->clear) {
            /* a quad over the viewport, written without blending (the pass keeps the rest) */
            rc->projection = br_mat4_ortho(0.0f, units_w, units_h, 0.0f, -1.0f, 1.0f);
            br_render_update_transform(rc);
            br_rect v = view->viewport;
            uint32_t c = br_render_pack_color(view->clear_color, 1.0f);
            br_vertex2d q[4] = { { v.x, v.y, 0.0f, 0.0f, c },
                                 { v.x + v.width, v.y, 1.0f, 0.0f, c },
                                 { v.x + v.width, v.y + v.height, 1.0f, 1.0f, c },
                                 { v.x, v.y + v.height, 0.0f, 1.0f, c } };
            rc->blend = BR_BLEND_NONE;
            br_render_push_quad(rc, br_texture_white(e), q);
        }
        rc->projection = view->projection;
        rc->culling_mask = view->culling_mask;
        rc->blend = BR_BLEND_NORMAL;
        if (root.index != s->root.index && !br_render_is_on_stage(s, root.index)) {
            br_scene_refresh_subtree(s, root.index);
        }
        br_render_set_space(rc, br_scene_group_space(s, root.index)); /* also applies the projection */
        rc->layer_bits = br_render_inherited_bits(s, root.index);
        br_render_walk(rc, root.index, BR_BLEND_NORMAL, 0);
        rc->culling_mask = 0xFFFFFFFFu;
    }
}

static void br_render_collect_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_render_state *r = e->render;
    br_batch_reset(e);
    int w, h, pw, ph;
    br_engine_window_size(e, &w, &h);
    br_engine_pixel_size(e, &pw, &ph);
    if (br_engine_gpu(e) == NULL) {
        /* headless: record the draw list for br_render_debug_set_screen's screen (tests) */
        w = pw = r->headless_width;
        h = ph = r->headless_height;
    }
    if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) {
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
    rc.culling_mask = 0xFFFFFFFFu;
    rc.layer_bits = 1u;
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

    if (r->views.count == 0) {
        br_batch_push_pass(&rc, frame, false, br_color_make(0, 0, 0, 0));
        br_render_walk(&rc, e->scene->root.index, BR_BLEND_NORMAL, 0);
    } else {
        br_render_draw_views(&rc, (float)w, (float)h, (float)pw, (float)ph);
        br_render_ctx_set_target(&rc, frame, window_area, (float)pw, (float)ph, true);
        br_batch_push_pass(&rc, frame, false, br_color_make(0, 0, 0, 0)); /* overlays go to the window */
    }
    br_profile_begin(e, "render.overlays");
    for (uint32_t i = 0; i < r->overlays.count; i++) {
        br_overlay_entry *o = br_array_at(&r->overlays, i);
        br_render_set_space(&rc, br_mat3_identity());
        rc.blend = BR_BLEND_NORMAL;
        o->fn(e, &rc, o->user);
    }
    br_profile_end(e);
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
    br_array_free(&r->indices);
    br_array_free(&r->uniforms);
    br_array_free(&r->headless_vertices);
    br_array_free(&r->headless_indices);
    br_array_free(&r->rtt_jobs);
    br_array_free(&r->overlays);
    br_array_free(&r->views);
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
    br_array_init(&r->indices, sizeof(uint16_t));
    br_array_init(&r->uniforms, 1);
    br_array_init(&r->headless_vertices, 1);
    br_array_init(&r->headless_indices, 1);
    br_array_init(&r->rtt_jobs, sizeof(br_rtt_job));
    br_array_init(&r->overlays, sizeof(br_overlay_entry));
    br_array_init(&r->views, sizeof(br_render_view));
    br_engine_on_shutdown(e, br_render_shutdown, NULL);
    br_sprite_register(e);
    br_system_add(e, "render.collect", BR_PHASE_PRE_RENDER, 1000, br_render_collect_system, NULL);
    br_system_add(e, "render.submit", BR_PHASE_RENDER, 0, br_render_submit_system, NULL);
}
