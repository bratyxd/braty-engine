/* hello_quad - one textured, rotating quad drawn through the raw br_gpu layer.
 *
 * Shows the frame flow every renderer follows:
 *   UPDATE      : change data (the angle)
 *   PRE_RENDER  : write this frame's vertices into transient GPU memory
 *   RENDER      : upload transient data, begin a pass on the frame target, bind, draw
 *
 * Run: hello_quad --frames 3 --screenshot out.png [--gpu-driver vulkan|direct3d12]
 */
#include "braty/braty_main.h"

#include "textured.frag.h"
#include "textured.vert.h"

typedef struct quad_vertex {
    float    x, y;
    float    u, v;
    uint32_t color; /* RGBA8, from br_color_to_rgba8 */
} quad_vertex;

typedef struct hello_state {
    br_gpu_texture *texture;
    SDL_GPUBuffer  *index_buffer;   /* static: uploaded once */
    uint32_t        vertex_offset;  /* this frame's vertices in the transient buffer */
    float           angle;
} hello_state;

/* A 64x64 checkerboard with a colour gradient, so orientation and filtering are visible. */
static br_gpu_texture *make_checker_texture(br_gpu *g)
{
    enum { SIZE = 64, CELL = 8 };
    uint8_t pixels[SIZE * SIZE * 4]; /* 16 KiB on the stack */
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            bool light = ((x / CELL) + (y / CELL)) % 2 == 0;
            uint8_t *p = &pixels[(y * SIZE + x) * 4];
            p[0] = (uint8_t)(light ? 255 : 40 + x * 3);
            p[1] = (uint8_t)(light ? 255 : 40 + y * 3);
            p[2] = (uint8_t)(light ? 255 : 160);
            p[3] = 255;
        }
    }
    return br_gpu_texture_create_rgba8(g, SIZE, SIZE, pixels, true);
}

static void update_system(br_engine *e, float dt, void *user)
{
    (void)e;
    hello_state *s = user;
    s->angle += dt * 0.5f;
}

static void prepare_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    hello_state *s = user;
    int w, h;
    br_engine_window_size(e, &w, &h);
    float half = (float)(w < h ? w : h) * 0.3f;
    br_mat3 m = br_mat3_from_transform((float)w * 0.5f, (float)h * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, s->angle, 0.0f, 0.0f);
    br_vec2 corners[4] = { { -half, -half }, { half, -half }, { half, half }, { -half, half } };
    float uvs[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    uint32_t white = br_color_to_rgba8(br_color_make(1, 1, 1, 1));
    quad_vertex *v = br_gpu_transient_alloc_vertices(br_engine_gpu(e), 4 * sizeof(quad_vertex), &s->vertex_offset);
    if (v == NULL) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        br_vec2 p = br_mat3_apply(m, corners[i]);
        v[i].x = p.x;
        v[i].y = p.y;
        v[i].u = uvs[i][0];
        v[i].v = uvs[i][1];
        v[i].color = white;
    }
}

static void render_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    hello_state *s = user;
    br_gpu *g = br_engine_gpu(e);
    SDL_GPUCommandBuffer *cmd = br_gpu_frame_cmd(g);
    br_render_target *target = br_gpu_frame_target(g);

    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_textured_vert;
    desc.fragment_shader = &br_shader_textured_frag;
    desc.layout.buffer_count = 1;
    desc.layout.strides[0] = sizeof(quad_vertex);
    desc.layout.attribute_count = 3;
    desc.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    desc.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    desc.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    desc.blend = br_gpu_blend_premultiplied();
    desc.color_format = target->color_format;
    desc.depth_stencil_format = br_gpu_render_target_depth_format(target);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);

    br_texture_style style = { BR_FILTER_LINEAR, BR_ADDRESS_CLAMP, true, 4.0f };
    SDL_GPUSampler *sampler = br_gpu_sampler_get(g, &style);
    if (pipeline == NULL || sampler == NULL || !br_gpu_transient_upload(g, cmd)) {
        return;
    }

    int w, h;
    br_engine_window_size(e, &w, &h);
    br_mat4 projection = br_mat4_ortho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f);

    SDL_GPURenderPass *pass = br_gpu_frame_begin_pass(g);
    if (pass == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), s->vertex_offset };
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { s->index_buffer, 0 };
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_GPUTextureSamplerBinding tex = { s->texture->handle, sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &projection, sizeof(projection));
    SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void hello_shutdown(br_engine *e, void *user)
{
    hello_state *s = user;
    br_gpu_texture_destroy(br_engine_gpu(e), s->texture);
    br_gpu_buffer_destroy(br_engine_gpu(e), s->index_buffer);
    br_free(s);
}

static bool hello_init(br_engine *e)
{
    br_gpu *g = br_engine_gpu(e);
    hello_state *s = br_alloc(sizeof(hello_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, hello_shutdown, s);
    s->texture = make_checker_texture(g);
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    s->index_buffer = br_gpu_buffer_create(g, SDL_GPU_BUFFERUSAGE_INDEX, sizeof(indices), indices);
    if (s->texture == NULL || s->index_buffer == NULL) {
        return false;
    }
    br_gpu_set_clear_color(g, br_color_from_hex(0x1d2b3a, 1.0f));
    br_system_add(e, "hello.update", BR_PHASE_UPDATE, 0, update_system, s);
    br_system_add(e, "hello.prepare", BR_PHASE_PRE_RENDER, 0, prepare_system, s);
    br_system_add(e, "hello.render", BR_PHASE_RENDER, 0, render_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - hello quad";
    desc.width = 960;
    desc.height = 540;
    desc.init = hello_init;
    return desc;
}
