/* sprites - bunnymark plus a tour of the 2D render core.
 *
 *   sprites [--count N] [--novsync] [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Bottom layer: N bouncing sprites (default 5000) with several textures, tint, alpha, rotation,
 * driven by a component + system. Top row, left to right:
 *   1. blend modes (normal, add, multiply, screen, erase, none) over a gradient, and a z-index row
 *   2. a scissor-masked group
 *   3. a stencil-masked group with a nested inverse stencil mask
 *   4. a render texture drawn back as sprites, a PNG written+loaded at startup, the missing texture
 * Middle strip: the extension points other modules use (docs/agents/render.md): render group,
 *   custom-shader node type, in-pass and own-passes callbacks, effects hook (cache_as_texture),
 *   render layer (big bunny above everything), culling, and an overlay (frame-rate bar).
 * At shutdown it logs the average frame time and the last frame's batch count.
 */
#include "braty/braty_main.h"

#include "stb/stb_image_write.h"
#include "textured.frag.h"
#include "textured.vert.h"

#include <SDL3/SDL_timer.h>

/* Command-line options: br_main runs before the engine exists, so they go through a file static. */
static int g_bunny_count = 5000;
static bool g_no_vsync = false;

enum { BUNNY_TEXTURES = 4, PANEL_W = 290, PANEL_H = 210, BUNNY_TOP = 360 };

typedef struct bunny {
    br_vec2 velocity;
    float   spin;
} bunny;

typedef struct demo_state {
    uint32_t   bunny_component;
    br_texture bunny_textures[BUNNY_TEXTURES];
    br_texture white;
    br_texture checker;
    br_texture circle;
    br_texture gradient;
    br_texture render_texture;
    br_node    rt_scene;          /* not on the stage: drawn into render_texture every frame */
    br_node    rt_spinner;
    br_node    scissor_content;
    br_node    stencil_content;
    br_node    stencil_mask;
    /* extension-point strip */
    uint32_t   wave_type;
    uint32_t   callback_type;
    br_texture callback_target;   /* cleared by an own-passes callback every frame */
    br_texture effect_target;     /* filled by the effects hook */
    br_node    cached;            /* the node demo_effects draws */
    br_render_effects_fn next_effects;  /* the hook installed before ours (filters) */
    void      *next_effects_user;
    br_node    group;             /* render group moved every frame */
    br_node    culled;            /* cullable sprite parked off screen */
    uint32_t   triangle_offset;   /* this frame's triangle vertices (in-pass callback) */
    uint32_t   rng;
    uint64_t   frame_ns_sum;
    uint64_t   frame_ns_count;
    uint64_t   last_ns;
} demo_state;

/* ---- tiny helpers */

static float demo_random(demo_state *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

static void put_pixel(uint8_t *p, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *px = &p[(y * w + x) * 4];
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = a;
}

/* A 26x37 "bunny": round body, two ears, an eye. */
static br_texture make_bunny(br_engine *e, uint8_t r, uint8_t g, uint8_t b)
{
    enum { W = 26, H = 37 };
    uint8_t pixels[W * H * 4];
    SDL_memset(pixels, 0, sizeof(pixels));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float bx = ((float)x - 12.5f) / 12.0f;
            float by = ((float)y - 25.0f) / 11.5f;
            float ear1x = ((float)x - 7.5f) / 3.5f;
            float ear2x = ((float)x - 17.5f) / 3.5f;
            float eary = ((float)y - 8.0f) / 8.0f;
            bool body = bx * bx + by * by <= 1.0f;
            bool ears = ear1x * ear1x + eary * eary <= 1.0f || ear2x * ear2x + eary * eary <= 1.0f;
            bool eye = (x - 16) * (x - 16) + (y - 22) * (y - 22) <= 3;
            if (eye) {
                put_pixel(pixels, W, x, y, 20, 20, 30, 255);
            } else if (body || ears) {
                uint8_t shade = (uint8_t)(y < 17 ? 210 : 255);
                put_pixel(pixels, W, x, y, (uint8_t)(r * shade / 255), (uint8_t)(g * shade / 255),
                          (uint8_t)(b * shade / 255), 255);
            }
        }
    }
    return br_texture_create_rgba8(e, W, H, pixels, NULL);
}

static br_texture make_checker(br_engine *e)
{
    enum { S = 64 };
    uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            bool light = ((x / 8) + (y / 8)) % 2 == 0;
            put_pixel(pixels, S, x, y, (uint8_t)(light ? 240 : 60 + x * 2), (uint8_t)(light ? 240 : 60 + y * 2),
                      (uint8_t)(light ? 240 : 170), 255);
        }
    }
    br_texture_style style = { BR_FILTER_NEAREST, BR_ADDRESS_CLAMP, false, 1.0f };
    return br_texture_create_rgba8(e, S, S, pixels, &style);
}

/* A soft-edged white disc (alpha falls off at the rim). */
static br_texture make_circle(br_engine *e)
{
    enum { S = 64 };
    uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = ((float)x + 0.5f - 32.0f) / 32.0f;
            float dy = ((float)y + 0.5f - 32.0f) / 32.0f;
            float d = SDL_sqrtf(dx * dx + dy * dy);
            float a = br_clampf((1.0f - d) * 8.0f, 0.0f, 1.0f);
            put_pixel(pixels, S, x, y, 255, 255, 255, (uint8_t)(a * 255.0f));
        }
    }
    return br_texture_create_rgba8(e, S, S, pixels, NULL);
}

/* Horizontal rainbow-ish gradient, 256x1. */
static br_texture make_gradient(br_engine *e)
{
    uint8_t pixels[256 * 4];
    for (int x = 0; x < 256; x++) {
        br_color c = br_color_from_hsl((float)x * 300.0f / 255.0f, 0.8f, 0.5f, 1.0f);
        put_pixel(pixels, 256, x, 0, (uint8_t)(c.r * 255), (uint8_t)(c.g * 255), (uint8_t)(c.b * 255), 255);
    }
    return br_texture_create_rgba8(e, 256, 1, pixels, NULL);
}

static void png_to_array(void *context, void *data, int size)
{
    br_array_push_n(context, data, (uint32_t)size);
}

/* Writes a small PNG next to the executable and loads it through the asset loader. */
static br_texture write_and_load_png(br_engine *e)
{
    enum { S = 32 };
    uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            bool ring = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 15 * 15;
            put_pixel(pixels, S, x, y, (uint8_t)(x * 8), 200, (uint8_t)(y * 8), (uint8_t)(ring ? 255 : 0));
        }
    }
    br_array png;
    br_array_init(&png, 1);
    stbi_write_png_to_func(png_to_array, &png, S, S, 4, pixels, S * 4);
    br_file_write_all(e, "sprites_generated.png", png.data, png.count);
    br_array_free(&png);
    return br_texture_load(e, "sprites_generated.png");
}

static br_node add_sprite(br_engine *e, br_node parent, br_texture t, float x, float y)
{
    br_node s = br_sprite_create(e, t);
    br_node_set_position(e, s, x, y);
    br_node_add_child(e, parent, s);
    return s;
}

/* A plain coloured rectangle: the white texture scaled and tinted. */
static br_node add_rect(br_engine *e, demo_state *s, br_node parent, float x, float y, float w, float h, uint32_t rgb)
{
    br_node r = add_sprite(e, parent, s->white, x, y);
    br_node_set_scale(e, r, w, h);
    br_node_set_tint(e, r, br_color_from_hex(rgb, 1.0f));
    return r;
}

static br_node add_panel(br_engine *e, demo_state *s, int column)
{
    br_node panel = br_node_create_container(e);
    br_node_set_position(e, panel, 20.0f + (float)column * (PANEL_W + 25), 20.0f);
    br_node_add_child(e, br_scene_root(e), panel);
    add_rect(e, s, panel, 0.0f, 0.0f, PANEL_W, PANEL_H, 0x262a40);
    return panel;
}

/* ---- scene setup */

static br_node setup_bunnies(br_engine *e, demo_state *s)
{
    br_component_desc desc = { "bunny", sizeof(bunny), NULL, 0 };
    s->bunny_component = br_component_register(e, &desc);
    br_node layer = br_node_create_container(e);
    br_node_add_child(e, br_scene_root(e), layer);
    for (int i = 0; i < g_bunny_count; i++) {
        br_node b = br_sprite_create(e, s->bunny_textures[i % BUNNY_TEXTURES]);
        br_sprite_set_anchor(e, b, 0.5f, 0.5f);
        br_node_set_position(e, b, demo_random(s) * 1280.0f, BUNNY_TOP + demo_random(s) * (720.0f - BUNNY_TOP));
        br_node_set_rotation(e, b, demo_random(s) * BR_TAU);
        br_node_set_alpha(e, b, 0.6f + 0.4f * demo_random(s));
        br_node_set_tint(e, b, br_color_from_hsl(demo_random(s) * 360.0f, 0.4f, 0.85f, 1.0f));
        br_node_add_child(e, layer, b);
        bunny *data = br_component_add(e, b, s->bunny_component);
        data->velocity = br_vec2_make((demo_random(s) - 0.5f) * 400.0f, (demo_random(s) - 0.5f) * 400.0f);
        data->spin = (demo_random(s) - 0.5f) * 4.0f;
    }
    return layer;
}

static void setup_blend_panel(br_engine *e, demo_state *s)
{
    br_node panel = add_panel(e, s, 0);
    br_node strip = add_sprite(e, panel, s->gradient, 10.0f, 10.0f);
    br_node_set_scale(e, strip, (PANEL_W - 20) / 256.0f, 80.0f);
    int modes[6] = { BR_BLEND_NORMAL, BR_BLEND_ADD, BR_BLEND_MULTIPLY, BR_BLEND_SCREEN, BR_BLEND_ERASE, BR_BLEND_NONE };
    for (int i = 0; i < 6; i++) {
        br_node c = add_sprite(e, panel, s->circle, 15.0f + (float)i * 45.0f, 30.0f);
        br_node_set_scale(e, c, 0.62f, 0.62f);
        br_node_set_tint(e, c, br_color_from_hex(0x808080, 1.0f));
        br_node_set_blend_mode(e, c, modes[i]);
    }
    /* z-index: added left to right, but sorted so the leftmost is on top */
    br_node row = br_node_create_container(e);
    br_node_set_position(e, row, 20.0f, 110.0f);
    br_node_add_child(e, panel, row);
    br_node_set_sortable_children(e, row, true);
    uint32_t colors[5] = { 0xe94f37, 0xf6c445, 0x6cc551, 0x3f88c5, 0xa45ee5 };
    for (int i = 0; i < 5; i++) {
        br_node r = add_rect(e, s, row, (float)i * 45.0f, (float)(i % 2) * 20.0f, 70.0f, 70.0f, colors[i]);
        br_node_set_z_index(e, r, 10 - i);
    }
}

static void setup_scissor_panel(br_engine *e, demo_state *s)
{
    br_node panel = add_panel(e, s, 1);
    br_node group = br_node_create_container(e);
    br_node_add_child(e, panel, group);
    s->scissor_content = add_sprite(e, group, s->checker, PANEL_W * 0.5f, PANEL_H * 0.5f);
    br_sprite_set_anchor(e, s->scissor_content, 0.5f, 0.5f);
    br_node_set_scale(e, s->scissor_content, 5.0f, 5.0f);
    for (int i = 0; i < 6; i++) {
        br_node b = add_sprite(e, group, s->bunny_textures[i % BUNNY_TEXTURES], -20.0f + (float)i * 60.0f, 90.0f);
        br_node_set_scale(e, b, 2.0f, 2.0f);
    }
    /* the mask: an (invisible) rectangle inside the panel; its world bounds clip the group */
    br_node mask = br_node_create_container(e);
    br_node_add_child(e, panel, mask);
    br_node frame = add_rect(e, s, mask, 30.0f, 25.0f, PANEL_W - 60.0f, PANEL_H - 50.0f, 0xffffff);
    (void)frame;
    br_node_set_mask(e, group, mask, BR_MASK_SCISSOR, false);
}

static void setup_stencil_panel(br_engine *e, demo_state *s)
{
    br_node panel = add_panel(e, s, 2);
    br_node group = br_node_create_container(e);
    br_node_add_child(e, panel, group);
    br_node bg = add_sprite(e, group, s->checker, 0.0f, 0.0f);
    br_node_set_scale(e, bg, PANEL_W / 64.0f, PANEL_H / 64.0f);
    br_node inner = br_node_create_container(e);
    br_node_add_child(e, group, inner);
    s->stencil_content = inner;
    for (int i = 0; i < 12; i++) {
        float angle = (float)i * BR_TAU / 12.0f;
        br_node b = add_sprite(e, inner, s->bunny_textures[i % BUNNY_TEXTURES], SDL_cosf(angle) * 70.0f,
                               SDL_sinf(angle) * 70.0f);
        br_sprite_set_anchor(e, b, 0.5f, 0.5f);
        br_node_set_scale(e, b, 1.5f, 1.5f);
    }
    br_node centre = add_sprite(e, inner, s->bunny_textures[0], 0.0f, 0.0f);
    br_sprite_set_anchor(e, centre, 0.5f, 0.5f);
    br_node_set_scale(e, centre, 3.0f, 3.0f);
    br_node_set_position(e, inner, PANEL_W * 0.5f, PANEL_H * 0.5f);

    /* outer mask: a diamond (rotated square); any node can be a stencil mask */
    s->stencil_mask = add_sprite(e, panel, s->white, PANEL_W * 0.5f, PANEL_H * 0.5f);
    br_sprite_set_anchor(e, s->stencil_mask, 0.5f, 0.5f);
    br_node_set_scale(e, s->stencil_mask, 140.0f, 140.0f);
    br_node_set_rotation(e, s->stencil_mask, BR_PI * 0.25f);
    br_node_set_mask(e, group, s->stencil_mask, BR_MASK_STENCIL, false);

    /* nested inverse mask: cut a square hole into the inner group (through the big centre bunny) */
    br_node hole = add_sprite(e, inner, s->white, 0.0f, 0.0f);
    br_sprite_set_anchor(e, hole, 0.5f, 0.5f);
    br_node_set_scale(e, hole, 36.0f, 36.0f);
    br_node_set_mask(e, inner, hole, BR_MASK_STENCIL, true);
}

static void setup_render_texture_panel(br_engine *e, demo_state *s)
{
    br_node panel = add_panel(e, s, 3);
    s->render_texture = br_render_texture_create(e, 128.0f, 128.0f, 1.0f);

    /* a little scene that lives off-stage and is drawn into the render texture */
    s->rt_scene = br_node_create_container(e);
    br_node bg = add_sprite(e, s->rt_scene, s->checker, 0.0f, 0.0f);
    br_node_set_scale(e, bg, 2.0f, 2.0f);
    s->rt_spinner = br_node_create_container(e);
    br_node_set_position(e, s->rt_spinner, 64.0f, 64.0f);
    br_node_add_child(e, s->rt_scene, s->rt_spinner);
    for (int i = 0; i < 5; i++) {
        float angle = (float)i * BR_TAU / 5.0f;
        br_node b = add_sprite(e, s->rt_spinner, s->bunny_textures[i % BUNNY_TEXTURES], SDL_cosf(angle) * 40.0f,
                               SDL_sinf(angle) * 40.0f);
        br_sprite_set_anchor(e, b, 0.5f, 0.5f);
    }

    br_node big = add_sprite(e, panel, s->render_texture, 85.0f, 105.0f);
    br_sprite_set_anchor(e, big, 0.5f, 0.5f);
    br_node_set_rotation(e, big, 0.2f);
    br_node_set_scale(e, big, 1.2f, 1.2f);
    br_node small = add_sprite(e, panel, s->render_texture, 180.0f, 20.0f);
    br_node_set_scale(e, small, 0.6f, 0.6f);
    br_node_set_tint(e, small, br_color_from_hex(0xff9090, 1.0f));

    br_node png = add_sprite(e, panel, write_and_load_png(e), 190.0f, 120.0f);
    br_node_set_scale(e, png, 2.0f, 2.0f);
    br_node missing = add_sprite(e, panel, br_texture_load(e, "missing.png"), 250.0f, 120.0f);
    br_node_set_scale(e, missing, 15.0f, 15.0f);
}

/* ---- extension points (snippets in docs/agents/render.md come from here) */

typedef struct wave_data {
    br_texture texture;
    float      width;
    float      height;
} wave_data;

/* A node type drawing a waving textured strip with its own shaders (br_render_push_custom). */
static void wave_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const wave_data *w = data;
    enum { SEGMENTS = 16 };
    br_vertex2d vertices[(SEGMENTS + 1) * 2];
    uint32_t indices[SEGMENTS * 6];
    br_mat3 m = br_render_node_transform(rc, n);
    float t = (float)br_ticker_elapsed(e);
    for (int i = 0; i <= SEGMENTS; i++) {
        float u = (float)i / SEGMENTS;
        float y = SDL_sinf(u * BR_TAU + t * 3.0f) * 10.0f;
        br_vec2 top = br_mat3_apply(m, br_vec2_make(u * w->width, y));
        br_vec2 bottom = br_mat3_apply(m, br_vec2_make(u * w->width, y + w->height));
        uint32_t color = br_render_pack_color(br_color_from_hsl(u * 360.0f, 0.7f, 0.7f, 1.0f), 1.0f);
        vertices[i * 2] = (br_vertex2d){ top.x, top.y, u, 0.0f, color };
        vertices[i * 2 + 1] = (br_vertex2d){ bottom.x, bottom.y, u, 1.0f, color };
    }
    for (uint32_t i = 0; i < SEGMENTS; i++) {
        uint32_t v = i * 2;
        uint32_t *q = &indices[i * 6];
        q[0] = v;
        q[1] = v + 1;
        q[2] = v + 2;
        q[3] = v + 1;
        q[4] = v + 3;
        q[5] = v + 2;
    }
    br_custom_draw draw;
    SDL_zero(draw);
    draw.vertex_shader = &br_shader_textured_vert; /* its b0 is the engine-pushed transform */
    draw.fragment_shader = &br_shader_textured_frag;
    draw.layout.buffer_count = 1;
    draw.layout.strides[0] = sizeof(br_vertex2d);
    draw.layout.attribute_count = 3;
    draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    draw.vertices = vertices;
    draw.vertex_bytes = sizeof(vertices);
    draw.indices = indices;
    draw.index_count = BR_ARRAY_COUNT(indices);
    draw.textures[0] = w->texture;
    draw.texture_count = 1;
    br_render_push_custom(rc, &draw);
}

/* In-pass callback: draws a triangle whose vertices were written during collect. */
static void triangle_draw(br_engine *e, const br_render_callback_info *info, void *user)
{
    demo_state *s = user;
    br_gpu *g = br_engine_gpu(e);
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_textured_vert;
    desc.fragment_shader = &br_shader_textured_frag;
    desc.layout.buffer_count = 1;
    desc.layout.strides[0] = sizeof(br_vertex2d);
    desc.layout.attribute_count = 3;
    desc.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    desc.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    desc.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    br_render_pipeline_setup(info, &desc); /* blend, target formats, stencil test */
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    br_texture_style style;
    SDL_zero(style);
    br_gpu_texture *white = br_texture_gpu(e, s->white);
    if (pipeline == NULL || white == NULL) {
        return;
    }
    SDL_GPUTextureSamplerBinding tex = { white->handle, br_gpu_sampler_get(g, &style) };
    SDL_BindGPUGraphicsPipeline(info->pass, pipeline);
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), s->triangle_offset };
    SDL_BindGPUVertexBuffers(info->pass, 0, &vb, 1);
    SDL_BindGPUFragmentSamplers(info->pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(info->cmd, 0, &info->transform, sizeof(br_mat4));
    SDL_DrawGPUPrimitives(info->pass, 3, 1, 0, 0);
}

/* Own-passes callback: runs a pass of its own (clears a render texture to a pulsing colour). */
static void clear_target_draw(br_engine *e, const br_render_callback_info *info, void *user)
{
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);
    br_color c = br_color_from_hsl(SDL_fmodf(t * 90.0f, 360.0f), 0.8f, 0.5f, 1.0f);
    br_render_target *rt = br_texture_render_target(e, s->callback_target);
    SDL_GPURenderPass *pass = br_gpu_render_target_begin(br_engine_gpu(e), info->cmd, rt, true, c);
    if (pass != NULL) {
        SDL_EndGPURenderPass(pass);
    }
}

/* A node type whose render callback queues both kinds of callback command. */
static void callback_node_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    demo_state *s = *(demo_state *const *)data;
    br_mat3 m = br_render_node_transform(rc, n);
    br_vertex2d *v = br_gpu_transient_alloc_vertices(br_engine_gpu(e), 3 * sizeof(br_vertex2d), &s->triangle_offset);
    if (v != NULL) {
        br_vec2 p0 = br_mat3_apply(m, br_vec2_make(30.0f, 0.0f));
        br_vec2 p1 = br_mat3_apply(m, br_vec2_make(60.0f, 60.0f));
        br_vec2 p2 = br_mat3_apply(m, br_vec2_make(0.0f, 60.0f));
        v[0] = (br_vertex2d){ p0.x, p0.y, 0.0f, 0.0f, br_render_pack_color(br_color_from_hex(0xff4040, 1.0f), 1.0f) };
        v[1] = (br_vertex2d){ p1.x, p1.y, 0.0f, 0.0f, br_render_pack_color(br_color_from_hex(0x40ff40, 1.0f), 1.0f) };
        v[2] = (br_vertex2d){ p2.x, p2.y, 0.0f, 0.0f, br_render_pack_color(br_color_from_hex(0x4040ff, 1.0f), 1.0f) };
        br_render_push_callback(rc, triangle_draw, s, false);
    }
    br_render_push_callback(rc, clear_target_draw, s, true);
}

/* Effects hook: draws one cache_as_texture node through a render texture, tinted to show it.
 * Every other node goes to the hook installed before ours (the filters module's). */
static bool demo_effects(br_engine *e, br_node n, br_render_ctx *rc, void *user)
{
    demo_state *s = user;
    if (!br_node_equal(n, s->cached)) {
        return s->next_effects != NULL && s->next_effects(e, n, rc, s->next_effects_user);
    }
    br_rect b = br_node_get_bounds(e, n);
    if (br_rect_is_empty(b) || !br_render_texture_resize(e, s->effect_target, b.width, b.height)) {
        return true;
    }
    br_render_subtree_to_texture(rc, n, s->effect_target, b, true, br_color_make(0, 0, 0, 0));
    br_mat3 old = br_render_set_space(rc, br_mat3_identity()); /* b is in world coordinates */
    uint32_t tint = br_render_pack_color(br_color_from_hex(0xffa0a0, 1.0f), 1.0f);
    br_vertex2d v[4] = {
        { b.x, b.y, 0.0f, 0.0f, tint },
        { b.x + b.width, b.y, 1.0f, 0.0f, tint },
        { b.x + b.width, b.y + b.height, 1.0f, 1.0f, tint },
        { b.x, b.y + b.height, 0.0f, 1.0f, tint },
    };
    br_render_push_quad(rc, s->effect_target, v);
    br_render_set_space(rc, old);
    return true;
}

/* Overlay: a frame-rate bar in the bottom-left corner, drawn after the stage. */
static void demo_overlay(br_engine *e, br_render_ctx *rc, void *user)
{
    demo_state *s = user;
    int w, h;
    br_engine_window_size(e, &w, &h);
    float fps = br_ticker_fps(e);
    float bar = br_clampf(fps, 0.0f, 120.0f) * 2.0f;
    uint32_t color = br_render_pack_color(br_color_from_hex(fps >= 55.0f ? 0x40e040 : 0xe04040, 1.0f), 0.9f);
    br_vertex2d v[4] = {
        { 10.0f, (float)h - 18.0f, 0.0f, 0.0f, color },
        { 10.0f + bar, (float)h - 18.0f, 1.0f, 0.0f, color },
        { 10.0f + bar, (float)h - 10.0f, 1.0f, 1.0f, color },
        { 10.0f, (float)h - 10.0f, 0.0f, 1.0f, color },
    };
    br_render_push_quad(rc, s->white, v);
}

static void setup_extension_strip(br_engine *e, demo_state *s, br_node top_layer, br_node bunny_parent)
{
    br_node strip = br_node_create_container(e);
    br_node_set_position(e, strip, 20.0f, 250.0f);
    br_node_add_child(e, br_scene_root(e), strip);
    add_rect(e, s, strip, 0.0f, 0.0f, 1240.0f, 95.0f, 0x1c2033);

    /* render group: five sprites moved together by one matrix */
    s->group = br_node_create_container(e);
    br_node_set_render_group(e, s->group, true);
    br_node_set_position(e, s->group, 10.0f, 15.0f);
    br_node_add_child(e, strip, s->group);
    for (int i = 0; i < 5; i++) {
        add_sprite(e, s->group, s->bunny_textures[i % BUNNY_TEXTURES], (float)i * 30.0f, 10.0f);
    }

    /* custom-shader node type */
    br_node_type_desc wave;
    SDL_zero(wave);
    wave.name = "demo_wave";
    wave.data_size = sizeof(wave_data);
    wave.render = wave_render;
    s->wave_type = br_node_type_register(e, &wave);
    br_node wn = br_node_create(e, s->wave_type);
    wave_data *wd = br_node_data(e, wn, s->wave_type);
    wd->texture = s->checker;
    wd->width = 220.0f;
    wd->height = 50.0f;
    br_node_set_position(e, wn, 220.0f, 25.0f);
    br_node_add_child(e, strip, wn);

    /* callbacks: in-pass triangle + own-pass clear of a render texture shown next to it */
    br_node_type_desc cb;
    SDL_zero(cb);
    cb.name = "demo_callbacks";
    cb.data_size = sizeof(demo_state *);
    cb.render = callback_node_render;
    s->callback_type = br_node_type_register(e, &cb);
    br_node cn = br_node_create(e, s->callback_type);
    *(demo_state **)br_node_data(e, cn, s->callback_type) = s;
    br_node_set_position(e, cn, 480.0f, 17.0f);
    br_node_add_child(e, strip, cn);
    s->callback_target = br_render_texture_create(e, 60.0f, 60.0f, 1.0f);
    add_sprite(e, strip, s->callback_target, 560.0f, 17.0f);

    /* effects hook: a cache_as_texture group is drawn by demo_effects */
    s->effect_target = br_render_texture_create(e, 64.0f, 64.0f, 1.0f);
    s->next_effects = br_render_effects_hook(e, &s->next_effects_user); /* chain, do not replace */
    br_render_set_effects_hook(e, demo_effects, s);
    s->cached = br_node_create_container(e);
    br_node_set_position(e, s->cached, 660.0f, 15.0f);
    br_node_add_child(e, strip, s->cached);
    add_sprite(e, s->cached, s->checker, 0.0f, 0.0f);
    add_sprite(e, s->cached, s->bunny_textures[2], 20.0f, 14.0f);
    br_node_set_cache_as_texture(e, s->cached, true);

    /* render layer: a big bunny that lives among the bunnies but draws above everything */
    br_node big = add_sprite(e, bunny_parent, s->bunny_textures[1], 800.0f, 245.0f);
    br_node_set_scale(e, big, 3.0f, 3.0f);
    br_node_layer_attach(e, top_layer, big);

    /* culling: a cullable sprite parked off screen is skipped */
    s->culled = add_sprite(e, strip, s->checker, -500.0f, 0.0f);
    br_node_set_cullable(e, s->culled, true);

    br_render_add_overlay(e, demo_overlay, s);
}

/* ---- systems */

static void bunny_system(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    int w, h;
    br_engine_window_size(e, &w, &h);
    uint32_t count = br_component_count(e, s->bunny_component);
    for (uint32_t i = 0; i < count; i++) {
        bunny *b = br_component_at(e, s->bunny_component, i);
        br_node n = br_component_owner_at(e, s->bunny_component, i);
        br_vec2 p = br_node_position(e, n);
        b->velocity.y += 300.0f * dt;
        p = br_vec2_add(p, br_vec2_scale(b->velocity, dt));
        if (p.x < 0.0f || p.x > (float)w) {
            b->velocity.x = -b->velocity.x;
            p.x = br_clampf(p.x, 0.0f, (float)w);
        }
        if (p.y > (float)h) {
            b->velocity.y = -b->velocity.y * 0.85f;
            p.y = (float)h;
            if (b->velocity.y > -100.0f) {
                b->velocity.y = -300.0f - 200.0f * demo_random(s);
            }
        } else if (p.y < BUNNY_TOP) {
            b->velocity.y = SDL_fabsf(b->velocity.y);
            p.y = BUNNY_TOP;
        }
        br_node_set_position(e, n, p.x, p.y);
        br_node_set_rotation(e, n, br_node_rotation(e, n) + b->spin * dt);
    }
}

static void animate_system(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);
    br_node_set_rotation(e, s->scissor_content, t * 0.3f);
    br_node_set_rotation(e, s->stencil_content, -t * 0.5f);
    br_node_set_rotation(e, s->rt_spinner, t);
    br_node_set_rotation(e, s->stencil_mask, BR_PI * 0.25f + t * 0.2f);
    br_render_to_texture(e, s->rt_scene, s->render_texture);
    br_node_set_position(e, s->group, 10.0f + 30.0f * SDL_sinf(t * 2.0f), 15.0f);

    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0 && br_ticker_frame(e) > 10) {
        s->frame_ns_sum += now - s->last_ns;
        s->frame_ns_count += 1;
    }
    s->last_ns = now;
    (void)dt;
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    br_render_stats stats;
    br_render_get_stats(e, &stats);
    double avg_ms = s->frame_ns_count > 0 ? (double)s->frame_ns_sum / (double)s->frame_ns_count / 1e6 : 0.0;
    br_log_info("sprites: %d bunnies, avg frame %.2f ms over %llu frames (%s), last frame: %u draw calls, "
                "%u batches, %u vertices, %u passes, %u nodes drawn, %u pipelines",
                g_bunny_count, avg_ms, (unsigned long long)s->frame_ns_count, g_no_vsync ? "vsync off" : "vsync on",
                stats.draw_calls, stats.batches, stats.vertices, stats.passes, stats.nodes_drawn,
                br_gpu_pipeline_count(br_engine_gpu(e)));
    br_log_info("sprites: off-screen cullable sprite culled = %s", br_node_is_culled(e, s->culled) ? "yes" : "no");
    br_system_info info;
    for (uint32_t i = 0; br_system_get_info(e, i, &info); i++) {
        br_log_info("  system %-24s %.2f ms", info.name, (double)info.last_time_ns / 1e6);
    }
    br_node_destroy(e, s->rt_scene); /* off-stage nodes are ours to free (the engine would too) */
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    s->rng = 12345u;
    if (g_no_vsync) {
        br_gpu_set_vsync(br_engine_gpu(e), false);
    }
    br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x101422, 1.0f));
    s->bunny_textures[0] = make_bunny(e, 255, 255, 255);
    s->bunny_textures[1] = make_bunny(e, 255, 170, 170);
    s->bunny_textures[2] = make_bunny(e, 170, 255, 190);
    s->bunny_textures[3] = make_bunny(e, 170, 200, 255);
    s->white = br_texture_white(e);
    s->checker = make_checker(e);
    s->circle = make_circle(e);
    s->gradient = make_gradient(e);

    br_node bunny_parent = setup_bunnies(e, s);
    setup_blend_panel(e, s);
    setup_scissor_panel(e, s);
    setup_stencil_panel(e, s);
    setup_render_texture_panel(e, s);
    br_node top_layer = br_node_create_container(e);
    setup_extension_strip(e, s, top_layer, bunny_parent);
    br_node_add_child(e, br_scene_root(e), top_layer); /* last child of the stage: drawn last */
    br_system_add(e, "sprites.bunnies", BR_PHASE_UPDATE, 0, bunny_system, s);
    br_system_add(e, "sprites.animate", BR_PHASE_UPDATE, 1, animate_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            g_bunny_count = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--novsync") == 0) {
            g_no_vsync = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - sprites";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
