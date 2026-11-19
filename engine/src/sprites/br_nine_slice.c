/* br_nine_slice: the nine-slice sprite node type (PixiJS NineSliceSprite, plus tiling fills,
 * hidden centre and trimmed/rotated atlas frames). */
#include "sprites/br_sprites_internal.h"

/* ponytail: a tiled cell emits at most this many tiles per axis, beyond that the rest
 * stretches; raise it (or tile in a shader like tiling sprites) if huge tiled panels show up. */
#define BR_NINE_SLICE_MAX_TILES 256

/* Everything one piece of geometry needs: frame placement and the output array. */
typedef struct br_nine_slice_emit {
    const br_texture_info *info;
    br_rect  trim;      /* where texture pixels exist, in untrimmed texture units */
    float    origin_x;  /* -anchor * size */
    float    origin_y;
    br_array *out;
    uint32_t quads;
} br_nine_slice_emit;

/* uv of a point given as a fraction of the stored frame (handles rotated frames). */
static br_vec2 br_nine_slice_uv(const br_texture_info *info, float fx, float fy)
{
    br_vec2 o = info->uvs[0];
    br_vec2 ux = br_vec2_sub(info->uvs[1], info->uvs[0]);
    br_vec2 uy = br_vec2_sub(info->uvs[3], info->uvs[0]);
    return br_vec2_add(o, br_vec2_add(br_vec2_scale(ux, fx), br_vec2_scale(uy, fy)));
}

/* Emits source rect [sx0,sx1]x[sy0,sy1] (texture units) onto [dx0,dx1]x[dy0,dy1] (local
 * units), cut to the trimmed area so transparent padding produces no geometry. */
static void br_nine_slice_piece(br_nine_slice_emit *em, float sx0, float sy0, float sx1, float sy1, float dx0,
                                float dy0, float dx1, float dy1)
{
    if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) {
        return;
    }
    float kx = (dx1 - dx0) / (sx1 - sx0);
    float ky = (dy1 - dy0) / (sy1 - sy0);
    float cx0 = SDL_max(sx0, em->trim.x);
    float cy0 = SDL_max(sy0, em->trim.y);
    float cx1 = SDL_min(sx1, em->trim.x + em->trim.width);
    float cy1 = SDL_min(sy1, em->trim.y + em->trim.height);
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }
    float x0 = em->origin_x + dx0 + (cx0 - sx0) * kx;
    float y0 = em->origin_y + dy0 + (cy0 - sy0) * ky;
    float x1 = em->origin_x + dx0 + (cx1 - sx0) * kx;
    float y1 = em->origin_y + dy0 + (cy1 - sy0) * ky;
    float fx0 = (cx0 - em->trim.x) / em->trim.width;
    float fy0 = (cy0 - em->trim.y) / em->trim.height;
    float fx1 = (cx1 - em->trim.x) / em->trim.width;
    float fy1 = (cy1 - em->trim.y) / em->trim.height;
    br_vertex2d *v = br_array_push_n(em->out, NULL, 4);
    if (v == NULL) {
        return;
    }
    float xs[4] = { x0, x1, x1, x0 };
    float ys[4] = { y0, y0, y1, y1 };
    float fxs[4] = { fx0, fx1, fx1, fx0 };
    float fys[4] = { fy0, fy0, fy1, fy1 };
    for (int i = 0; i < 4; i++) {
        br_vec2 uv = br_nine_slice_uv(em->info, fxs[i], fys[i]);
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].u = uv.x;
        v[i].v = uv.y;
        v[i].color = 0xFFFFFFFFu;
    }
    em->quads += 1;
}

/* Fills one destination cell from one source cell, stretched or tiled per axis. A tile is
 * the source cell at scale (kx, ky); the last tile on each axis is cut short. */
static void br_nine_slice_cell(br_nine_slice_emit *em, const float s[4], const float d[4], bool tile_x, bool tile_y,
                               float kx, float ky)
{
    float sw = s[2] - s[0];
    float sh = s[3] - s[1];
    float dw = d[2] - d[0];
    float dh = d[3] - d[1];
    if (sw <= 0.0f || sh <= 0.0f || dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    float tw = tile_x ? sw * kx : dw;
    float th = tile_y ? sh * ky : dh;
    if (dw / tw > BR_NINE_SLICE_MAX_TILES) {
        tw = dw;
    }
    if (dh / th > BR_NINE_SLICE_MAX_TILES) {
        th = dh;
    }
    for (float y = d[1]; y < d[3] - 1e-4f; y += th) {
        float y1 = SDL_min(y + th, d[3]);
        float sy1 = s[1] + sh * (y1 - y) / th;
        for (float x = d[0]; x < d[2] - 1e-4f; x += tw) {
            float x1 = SDL_min(x + tw, d[2]);
            float sx1 = s[0] + sw * (x1 - x) / tw;
            br_nine_slice_piece(em, s[0], s[1], sx1, sy1, x, y, x1, y1);
        }
    }
}

uint32_t br_nine_slice_geometry(const br_nine_slice *ns, const br_texture_info *info, br_array *out)
{
    float res = info->resolution > 0.0f ? info->resolution : 1.0f;
    float tex_w = info->width;
    float tex_h = info->height;
    if (tex_w <= 0.0f || tex_h <= 0.0f) {
        return 0;
    }
    float w = ns->width;
    float h = ns->height;
    /* borders cannot be larger than the texture */
    float l = br_clampf(ns->left, 0.0f, tex_w);
    float r = br_clampf(ns->right, 0.0f, tex_w - l);
    float t = br_clampf(ns->top, 0.0f, tex_h);
    float b = br_clampf(ns->bottom, 0.0f, tex_h - t);
    /* smaller than the borders: shrink both borders of that axis proportionally */
    float kx = (l + r > w && l + r > 0.0f) ? w / (l + r) : 1.0f;
    float ky = (t + b > h && t + b > 0.0f) ? h / (t + b) : 1.0f;

    br_nine_slice_emit em;
    SDL_zero(em);
    em.info = info;
    em.out = out;
    em.origin_x = -ns->anchor.x * w;
    em.origin_y = -ns->anchor.y * h;
    em.trim = br_rect_make(0.0f, 0.0f, tex_w, tex_h);
    if (info->frame.trim.width > 0.0f && info->frame.trim.height > 0.0f) {
        em.trim = br_rect_make(info->frame.trim.x / res, info->frame.trim.y / res, info->frame.trim.width / res,
                               info->frame.trim.height / res);
    }

    float sx[4] = { 0.0f, l, tex_w - r, tex_w };
    float sy[4] = { 0.0f, t, tex_h - b, tex_h };
    float dx[4] = { 0.0f, l * kx, w - r * kx, w };
    float dy[4] = { 0.0f, t * ky, h - b * ky, h };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            bool center = row == 1 && col == 1;
            if (center && ns->hide_center) {
                continue;
            }
            br_nine_slice_fill fill = center ? ns->center_fill : ns->edge_fill;
            bool tile_x = fill == BR_NINE_SLICE_TILE && col == 1;
            bool tile_y = fill == BR_NINE_SLICE_TILE && row == 1;
            float s[4] = { sx[col], sy[row], sx[col + 1], sy[row + 1] };
            float d[4] = { dx[col], dy[row], dx[col + 1], dy[row + 1] };
            br_nine_slice_cell(&em, s, d, tile_x, tile_y, kx, ky);
        }
    }
    return em.quads;
}

static void br_nine_slice_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_nine_slice *ns = data;
    uint32_t color = br_render_node_color(rc, n);
    if (color == 0) {
        return;
    }
    br_array *scratch = &e->sprites->scratch;
    br_array_clear(scratch);
    uint32_t quads = br_nine_slice_geometry(ns, br_render_texture_info(rc, ns->texture), scratch);
    br_mat3 m = br_render_node_transform(rc, n);
    for (uint32_t q = 0; q < quads; q++) {
        br_vertex2d *v = br_array_at(scratch, q * 4);
        for (int i = 0; i < 4; i++) {
            br_vec2 p = br_mat3_apply(m, br_vec2_make(v[i].x, v[i].y));
            v[i].x = p.x;
            v[i].y = p.y;
            v[i].color = color;
        }
        br_render_push_quad(rc, ns->texture, v);
    }
}

static bool br_nine_slice_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_nine_slice *ns = data;
    *out = br_rect_make(-ns->anchor.x * ns->width, -ns->anchor.y * ns->height, ns->width, ns->height);
    return true;
}

static bool br_nine_slice_hit_test(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    br_rect r;
    br_nine_slice_bounds(e, n, data, &r);
    return br_rect_contains(r, p.x, p.y);
}

void br_nine_slice_register(br_engine *e)
{
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "nine_slice";
    desc.data_size = sizeof(br_nine_slice);
    desc.local_bounds = br_nine_slice_bounds;
    desc.render = br_nine_slice_render;
    desc.hit_test = br_nine_slice_hit_test;
    e->sprites->nine_slice_type = br_node_type_register(e, &desc);
}

uint32_t br_nine_slice_type(br_engine *e)
{
    return e->sprites->nine_slice_type;
}

br_node br_nine_slice_create(br_engine *e, br_texture texture, float left, float top, float right, float bottom)
{
    br_node n = br_node_create(e, e->sprites->nine_slice_type);
    br_nine_slice *ns = br_node_data(e, n, e->sprites->nine_slice_type);
    if (ns != NULL) {
        br_texture_info info;
        br_texture_get_info(e, texture, &info);
        ns->texture = texture;
        ns->left = left;
        ns->top = top;
        ns->right = right;
        ns->bottom = bottom;
        ns->width = info.width;
        ns->height = info.height;
    }
    return n;
}

br_nine_slice *br_nine_slice_get(br_engine *e, br_node n)
{
    return br_node_data(e, n, e->sprites->nine_slice_type);
}

void br_nine_slice_set_size(br_engine *e, br_node n, float width, float height)
{
    br_nine_slice *ns = br_nine_slice_get(e, n);
    if (ns != NULL) {
        ns->width = SDL_max(width, 0.0f);
        ns->height = SDL_max(height, 0.0f);
    }
}

void br_nine_slice_set_borders(br_engine *e, br_node n, float left, float top, float right, float bottom)
{
    br_nine_slice *ns = br_nine_slice_get(e, n);
    if (ns != NULL) {
        ns->left = left;
        ns->top = top;
        ns->right = right;
        ns->bottom = bottom;
    }
}
