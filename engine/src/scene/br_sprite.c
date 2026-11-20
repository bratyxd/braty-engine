/* br_sprite: the sprite node type (textured quad with anchor, trim and rotated frames). */
#include "braty/br_sprite.h"
#include "render/br_render_internal.h"
#include "scene/br_scene_internal.h"

/* Local corners of the drawn quad (top-left, top-right, bottom-right, bottom-left). */
static void br_sprite_corners(const br_sprite *sp, const br_texture_info *info, br_vec2 out[4])
{
    float res = info->resolution;
    float orig_w = info->frame.orig.width / res;
    float orig_h = info->frame.orig.height / res;
    float x0 = -sp->anchor.x * orig_w;
    float y0 = -sp->anchor.y * orig_h;
    float x1 = x0 + orig_w;
    float y1 = y0 + orig_h;
    if (info->frame.trim.width > 0.0f && info->frame.trim.height > 0.0f) {
        /* trimmed: only the opaque part is stored, placed inside the original rect */
        x0 += info->frame.trim.x / res;
        y0 += info->frame.trim.y / res;
        x1 = x0 + info->frame.trim.width / res;
        y1 = y0 + info->frame.trim.height / res;
    }
    out[0] = br_vec2_make(x0, y0);
    out[1] = br_vec2_make(x1, y0);
    out[2] = br_vec2_make(x1, y1);
    out[3] = br_vec2_make(x0, y1);
}

static void br_sprite_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_sprite *sp = data;
    /* the walk only calls render for live nodes: read the tables directly */
    uint32_t color = br_scene_record_at(e->scene, n.index)->world_color;
    if (rc->stencil == BR_STENCIL_INCR || rc->stencil == BR_STENCIL_DECR) {
        color = 0xFFFFFFFFu; /* stencil mask geometry (br_render_node_color): alpha 0 still clips */
    }
    if (color == 0) {
        return; /* premultiplied and fully transparent: all four bytes are zero */
    }
    const br_texture_info *info = br_render_texture_info(rc, sp->texture);
    br_vec2 corners[4];
    br_sprite_corners(sp, info, corners);
    br_mat3 m = br_scene_transform_at(e->scene, n.index)->world;
    uint32_t slot;
    br_batch_vertex *v = br_batch_quad(rc, sp->texture, &slot); /* written in place, no copies */
    if (v == NULL) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        br_vec2 c = corners[i];
        br_vec2 p = { m.a * c.x + m.c * c.y + m.tx, m.b * c.x + m.d * c.y + m.ty }; /* br_mat3_apply, inlined */
        if (sp->round_pixels) {
            float ratio = br_render_pixel_ratio(rc);
            p.x = SDL_roundf(p.x * ratio) / ratio;
            p.y = SDL_roundf(p.y * ratio) / ratio;
        }
        v[i].x = p.x;
        v[i].y = p.y;
        v[i].u = info->uvs[i].x;
        v[i].v = info->uvs[i].y;
        v[i].color = color;
        v[i].texture = slot;
    }
}

static bool br_sprite_local_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)n;
    const br_sprite *sp = data;
    br_texture_info info;
    br_texture_get_info(e, sp->texture, &info);
    br_vec2 c[4];
    br_sprite_corners(sp, &info, c);
    *out = br_rect_make(c[0].x, c[0].y, c[2].x - c[0].x, c[2].y - c[0].y);
    return true;
}

static bool br_sprite_hit_test(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    br_rect r;
    br_sprite_local_bounds(e, n, data, &r);
    return br_rect_contains(r, p.x, p.y);
}

void br_sprite_register(br_engine *e)
{
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "sprite";
    desc.data_size = sizeof(br_sprite);
    desc.local_bounds = br_sprite_local_bounds;
    desc.render = br_sprite_render;
    desc.hit_test = br_sprite_hit_test;
    e->scene->sprite_type = br_node_type_register(e, &desc);
}

uint32_t br_sprite_type(br_engine *e)
{
    return e->scene->sprite_type;
}

br_node br_sprite_create(br_engine *e, br_texture texture)
{
    br_node n = br_node_create(e, e->scene->sprite_type);
    br_sprite *sp = br_node_data(e, n, e->scene->sprite_type);
    if (sp != NULL) {
        br_texture_info info;
        br_texture_get_info(e, texture, &info);
        sp->texture = texture;
        sp->anchor = info.frame.default_anchor;
    }
    return n;
}

void br_sprite_set_texture(br_engine *e, br_node sprite, br_texture texture)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    if (sp != NULL) {
        sp->texture = texture;
    }
}

br_texture br_sprite_texture(br_engine *e, br_node sprite)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    br_texture none = { 0, 0 };
    return sp != NULL ? sp->texture : none;
}

void br_sprite_set_anchor(br_engine *e, br_node sprite, float x, float y)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    if (sp != NULL) {
        sp->anchor = br_vec2_make(x, y);
    }
}

br_vec2 br_sprite_anchor(br_engine *e, br_node sprite)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    return sp != NULL ? sp->anchor : br_vec2_make(0.0f, 0.0f);
}

void br_sprite_set_size(br_engine *e, br_node sprite, float width, float height)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    if (sp == NULL) {
        return;
    }
    br_texture_info info;
    br_texture_get_info(e, sp->texture, &info);
    br_vec2 scale = br_node_scale(e, sprite);
    /* keep the sign of the current scale so a flipped sprite stays flipped (Pixi) */
    float sx = info.width > 0.0f ? width / info.width : 1.0f;
    float sy = info.height > 0.0f ? height / info.height : 1.0f;
    br_node_set_scale(e, sprite, br_vec2_make(scale.x < 0.0f ? -sx : sx, scale.y < 0.0f ? -sy : sy));
}

br_vec2 br_sprite_size(br_engine *e, br_node sprite)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    if (sp == NULL) {
        return br_vec2_make(0.0f, 0.0f);
    }
    br_texture_info info;
    br_texture_get_info(e, sp->texture, &info);
    br_vec2 scale = br_node_scale(e, sprite);
    return br_vec2_make(info.width * SDL_fabsf(scale.x), info.height * SDL_fabsf(scale.y));
}

void br_sprite_set_round_pixels(br_engine *e, br_node sprite, bool round_pixels)
{
    br_sprite *sp = br_node_data(e, sprite, e->scene->sprite_type);
    if (sp != NULL) {
        sp->round_pixels = round_pixels;
    }
}
