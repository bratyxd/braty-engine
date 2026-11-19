/* br_tiling_sprite: the tiling sprite node type (PixiJS TilingSprite). The CPU writes the
 * unwrapped tile coordinate per corner; tiling.frag wraps it inside the texture frame, so
 * atlas frames tile without needing a REPEAT sampler. */
#include "sprites/br_sprites_internal.h"

#include "textured.vert.h"
#include "tiling.frag.h"

/* Fragment uniforms of tiling.frag.hlsl (three float4 registers). */
typedef struct br_tiling_uniforms {
    float origin_x[4];   /* uv of the frame's top-left, uv step along the frame's x */
    float y[4];          /* uv step along the frame's y, unused */
    float clamp[4];      /* min x, min y, max x, max y of the wrapped coordinate */
} br_tiling_uniforms;

static void br_tiling_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    (void)e;
    const br_tiling_sprite *ts = data;
    uint32_t color = br_render_node_color(rc, n);
    if (color == 0 || ts->width <= 0.0f || ts->height <= 0.0f) {
        return;
    }
    const br_texture_info *info = br_render_texture_info(rc, ts->texture);
    /* one tile = the stored frame (trim padding is not repeated) */
    float frame_w = info->frame.frame.width / info->resolution;
    float frame_h = info->frame.frame.height / info->resolution;
    float scale_x = ts->tile_scale.x != 0.0f ? ts->tile_scale.x : 1.0f;
    float scale_y = ts->tile_scale.y != 0.0f ? ts->tile_scale.y : 1.0f;
    if (frame_w <= 0.0f || frame_h <= 0.0f) {
        return;
    }
    float x0 = -ts->anchor.x * ts->width;
    float y0 = -ts->anchor.y * ts->height;
    br_vec2 corners[4] = {
        { x0, y0 }, { x0 + ts->width, y0 }, { x0 + ts->width, y0 + ts->height }, { x0, y0 + ts->height }
    };
    br_mat3 m = br_render_node_transform(rc, n);
    br_vertex2d v[4];
    for (int i = 0; i < 4; i++) {
        /* local point -> pattern space: undo tile position, rotation, scale, then tile size */
        br_vec2 q = br_vec2_rotate(br_vec2_sub(corners[i], ts->tile_position), -ts->tile_rotation);
        br_vec2 p = br_mat3_apply(m, corners[i]);
        v[i].x = p.x;
        v[i].y = p.y;
        v[i].u = q.x / (scale_x * frame_w);
        v[i].v = q.y / (scale_y * frame_h);
        v[i].color = color;
    }
    uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    br_tiling_uniforms u;
    SDL_zero(u);
    u.origin_x[0] = info->uvs[0].x;
    u.origin_x[1] = info->uvs[0].y;
    u.origin_x[2] = info->uvs[1].x - info->uvs[0].x;
    u.origin_x[3] = info->uvs[1].y - info->uvs[0].y;
    u.y[0] = info->uvs[3].x - info->uvs[0].x;
    u.y[1] = info->uvs[3].y - info->uvs[0].y;
    float margin_x = br_clampf(ts->clamp_margin / info->frame.frame.width, 0.0f, 0.5f);
    float margin_y = br_clampf(ts->clamp_margin / info->frame.frame.height, 0.0f, 0.5f);
    u.clamp[0] = margin_x;
    u.clamp[1] = margin_y;
    u.clamp[2] = 1.0f - margin_x;
    u.clamp[3] = 1.0f - margin_y;

    br_custom_draw draw;
    SDL_zero(draw);
    draw.vertex_shader = &br_shader_textured_vert;
    draw.fragment_shader = &br_shader_tiling_frag;
    draw.layout.buffer_count = 1;
    draw.layout.strides[0] = sizeof(br_vertex2d);
    draw.layout.attribute_count = 3;
    draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    draw.vertices = v;
    draw.vertex_bytes = sizeof(v);
    draw.indices = indices;
    draw.index_count = 6;
    draw.textures[0] = ts->texture;
    draw.texture_count = 1;
    draw.fragment_uniforms = &u;
    draw.fragment_uniform_size = sizeof(u);
    br_render_push_custom(rc, &draw);
}

static bool br_tiling_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_tiling_sprite *ts = data;
    *out = br_rect_make(-ts->anchor.x * ts->width, -ts->anchor.y * ts->height, ts->width, ts->height);
    return true;
}

static bool br_tiling_hit_test(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    br_rect r;
    br_tiling_bounds(e, n, data, &r);
    return br_rect_contains(r, p.x, p.y);
}

void br_tiling_sprite_register(br_engine *e)
{
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "tiling_sprite";
    desc.data_size = sizeof(br_tiling_sprite);
    desc.local_bounds = br_tiling_bounds;
    desc.render = br_tiling_render;
    desc.hit_test = br_tiling_hit_test;
    e->sprites->tiling_type = br_node_type_register(e, &desc);
}

uint32_t br_tiling_sprite_type(br_engine *e)
{
    return e->sprites->tiling_type;
}

br_node br_tiling_sprite_create(br_engine *e, br_texture texture, float width, float height)
{
    br_node n = br_node_create(e, e->sprites->tiling_type);
    br_tiling_sprite *ts = br_node_data(e, n, e->sprites->tiling_type);
    if (ts != NULL) {
        ts->texture = texture;
        ts->width = width;
        ts->height = height;
        ts->tile_scale = br_vec2_make(1.0f, 1.0f);
        ts->clamp_margin = 0.5f;
    }
    return n;
}

br_tiling_sprite *br_tiling_sprite_get(br_engine *e, br_node n)
{
    return br_node_data(e, n, e->sprites->tiling_type);
}
