/* Shader blob selection, and the shader / pipeline caches. */
#include "gpu/br_gpu_internal.h"

/* SDL flag for each br_shader_format slot. */
static const SDL_GPUShaderFormat br_shader_format_flags[BR_SHADER_FORMAT_COUNT] = {
    SDL_GPU_SHADERFORMAT_PRIVATE, SDL_GPU_SHADERFORMAT_SPIRV, SDL_GPU_SHADERFORMAT_DXBC,
    SDL_GPU_SHADERFORMAT_DXIL,    SDL_GPU_SHADERFORMAT_MSL,   SDL_GPU_SHADERFORMAT_METALLIB,
};

/* Native formats first (a console's PRIVATE format, then the desktop formats). */
static const br_shader_format br_shader_format_order[BR_SHADER_FORMAT_COUNT] = {
    BR_SHADER_FORMAT_PRIVATE, BR_SHADER_FORMAT_DXIL,     BR_SHADER_FORMAT_SPIRV,
    BR_SHADER_FORMAT_METALLIB, BR_SHADER_FORMAT_MSL,     BR_SHADER_FORMAT_DXBC,
};

SDL_GPUShader *br_gpu_shader_get(br_gpu *g, const br_shader_blob *blob)
{
    for (uint32_t i = 0; i < g->shaders.count; i++) {
        br_shader_entry *entry = br_array_at(&g->shaders, i);
        if (entry->blob == blob) {
            return entry->shader;
        }
    }
    SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(g->device);
    const br_shader_code *code = NULL;
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
    for (int i = 0; i < BR_SHADER_FORMAT_COUNT; i++) {
        br_shader_format slot = br_shader_format_order[i];
        if ((supported & br_shader_format_flags[slot]) != 0 && blob->formats[slot].size > 0) {
            code = &blob->formats[slot];
            format = br_shader_format_flags[slot];
            break;
        }
    }
    if (code == NULL) {
        br_log_error("shader '%s' has no code for driver %s", blob->name, SDL_GetGPUDeviceDriver(g->device));
        return NULL;
    }
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code_size = code->size;
    info.code = code->code;
    info.entrypoint = code->entrypoint;
    info.format = format;
    info.stage = blob->stage;
    info.num_samplers = blob->num_samplers;
    info.num_storage_textures = blob->num_storage_textures;
    info.num_storage_buffers = blob->num_storage_buffers;
    info.num_uniform_buffers = blob->num_uniform_buffers;
    SDL_GPUShader *shader = SDL_CreateGPUShader(g->device, &info);
    if (shader == NULL) {
        br_log_error("cannot create shader '%s': %s", blob->name, SDL_GetError());
        return NULL;
    }
    br_shader_entry entry = { blob, shader };
    br_array_push(&g->shaders, &entry);
    return shader;
}

/* ---- pipeline key comparison, field by field (struct padding makes memcmp unreliable) */

static bool br_attribute_equal(const SDL_GPUVertexAttribute *a, const SDL_GPUVertexAttribute *b)
{
    return a->location == b->location && a->buffer_slot == b->buffer_slot && a->format == b->format &&
           a->offset == b->offset;
}

static bool br_layout_equal(const br_vertex_layout *a, const br_vertex_layout *b)
{
    if (a->buffer_count != b->buffer_count || a->attribute_count != b->attribute_count) {
        return false;
    }
    for (uint32_t i = 0; i < a->buffer_count; i++) {
        if (a->strides[i] != b->strides[i] || a->per_instance[i] != b->per_instance[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a->attribute_count; i++) {
        if (!br_attribute_equal(&a->attributes[i], &b->attributes[i])) {
            return false;
        }
    }
    return true;
}

static bool br_blend_equal(const SDL_GPUColorTargetBlendState *a, const SDL_GPUColorTargetBlendState *b)
{
    return a->src_color_blendfactor == b->src_color_blendfactor &&
           a->dst_color_blendfactor == b->dst_color_blendfactor && a->color_blend_op == b->color_blend_op &&
           a->src_alpha_blendfactor == b->src_alpha_blendfactor &&
           a->dst_alpha_blendfactor == b->dst_alpha_blendfactor && a->alpha_blend_op == b->alpha_blend_op &&
           a->color_write_mask == b->color_write_mask && a->enable_blend == b->enable_blend &&
           a->enable_color_write_mask == b->enable_color_write_mask;
}

static bool br_stencil_equal(const SDL_GPUStencilOpState *a, const SDL_GPUStencilOpState *b)
{
    return a->fail_op == b->fail_op && a->pass_op == b->pass_op && a->depth_fail_op == b->depth_fail_op &&
           a->compare_op == b->compare_op;
}

static bool br_pipeline_desc_equal(const br_pipeline_desc *a, const br_pipeline_desc *b)
{
    return a->vertex_shader == b->vertex_shader && a->fragment_shader == b->fragment_shader &&
           br_layout_equal(&a->layout, &b->layout) && a->primitive == b->primitive &&
           a->cull_mode == b->cull_mode && a->front_face == b->front_face && br_blend_equal(&a->blend, &b->blend) &&
           a->color_format == b->color_format && a->depth_stencil_format == b->depth_stencil_format &&
           a->sample_count == b->sample_count && a->depth_test == b->depth_test &&
           a->depth_write == b->depth_write && a->depth_compare == b->depth_compare &&
           a->stencil_test == b->stencil_test && br_stencil_equal(&a->stencil_front, &b->stencil_front) &&
           br_stencil_equal(&a->stencil_back, &b->stencil_back) &&
           a->stencil_read_mask == b->stencil_read_mask && a->stencil_write_mask == b->stencil_write_mask;
}

/* Unset (zero) compare/stencil ops are replaced by valid ones so every backend accepts them. */
static SDL_GPUCompareOp br_valid_compare(SDL_GPUCompareOp op)
{
    return op == SDL_GPU_COMPAREOP_INVALID ? SDL_GPU_COMPAREOP_ALWAYS : op;
}

static SDL_GPUStencilOpState br_valid_stencil(SDL_GPUStencilOpState s)
{
    if (s.fail_op == SDL_GPU_STENCILOP_INVALID) {
        s.fail_op = SDL_GPU_STENCILOP_KEEP;
    }
    if (s.pass_op == SDL_GPU_STENCILOP_INVALID) {
        s.pass_op = SDL_GPU_STENCILOP_KEEP;
    }
    if (s.depth_fail_op == SDL_GPU_STENCILOP_INVALID) {
        s.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    }
    s.compare_op = br_valid_compare(s.compare_op);
    return s;
}

static SDL_GPUGraphicsPipeline *br_gpu_pipeline_create(br_gpu *g, const br_pipeline_desc *desc)
{
    if (desc->vertex_shader == NULL || desc->fragment_shader == NULL ||
        desc->layout.buffer_count > BR_MAX_VERTEX_BUFFERS ||
        desc->layout.attribute_count > BR_MAX_VERTEX_ATTRIBUTES) {
        br_log_error("invalid pipeline desc (missing shader or too many vertex buffers/attributes)");
        return NULL;
    }
    SDL_GPUShader *vs = br_gpu_shader_get(g, desc->vertex_shader);
    SDL_GPUShader *fs = br_gpu_shader_get(g, desc->fragment_shader);
    if (vs == NULL || fs == NULL) {
        return NULL;
    }
    SDL_GPUVertexBufferDescription buffers[BR_MAX_VERTEX_BUFFERS];
    SDL_zeroa(buffers);
    for (uint32_t i = 0; i < desc->layout.buffer_count && i < BR_MAX_VERTEX_BUFFERS; i++) {
        buffers[i].slot = i;
        buffers[i].pitch = desc->layout.strides[i];
        buffers[i].input_rate = desc->layout.per_instance[i] ? SDL_GPU_VERTEXINPUTRATE_INSTANCE
                                                             : SDL_GPU_VERTEXINPUTRATE_VERTEX;
    }
    SDL_GPUColorTargetDescription color_target;
    SDL_zero(color_target);
    color_target.format = desc->color_format;
    color_target.blend_state = desc->blend;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = desc->layout.buffer_count;
    info.vertex_input_state.vertex_attributes = desc->layout.attributes;
    info.vertex_input_state.num_vertex_attributes = desc->layout.attribute_count;
    info.primitive_type = desc->primitive;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = desc->cull_mode;
    info.rasterizer_state.front_face = desc->front_face;
    info.multisample_state.sample_count = desc->sample_count;
    info.depth_stencil_state.compare_op = br_valid_compare(desc->depth_compare);
    info.depth_stencil_state.front_stencil_state = br_valid_stencil(desc->stencil_front);
    info.depth_stencil_state.back_stencil_state = br_valid_stencil(desc->stencil_back);
    info.depth_stencil_state.compare_mask = desc->stencil_read_mask;
    info.depth_stencil_state.write_mask = desc->stencil_write_mask;
    info.depth_stencil_state.enable_depth_test = desc->depth_test;
    info.depth_stencil_state.enable_depth_write = desc->depth_write;
    info.depth_stencil_state.enable_stencil_test = desc->stencil_test;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = desc->depth_stencil_format;
    info.target_info.has_depth_stencil_target = desc->depth_stencil_format != SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    if (pipeline == NULL) {
        br_log_error("cannot create pipeline (%s + %s): %s", desc->vertex_shader->name,
                     desc->fragment_shader->name, SDL_GetError());
    }
    return pipeline;
}

SDL_GPUGraphicsPipeline *br_gpu_pipeline_get(br_gpu *g, const br_pipeline_desc *desc)
{
    for (uint32_t i = 0; i < g->pipelines.count; i++) {
        br_pipeline_entry *entry = br_array_at(&g->pipelines, i);
        if (br_pipeline_desc_equal(&entry->desc, desc)) {
            return entry->pipeline;
        }
    }
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_create(g, desc);
    if (pipeline == NULL) {
        return NULL;
    }
    br_pipeline_entry entry;
    entry.desc = *desc;
    entry.pipeline = pipeline;
    br_array_push(&g->pipelines, &entry);
    return pipeline;
}

uint32_t br_gpu_pipeline_count(br_gpu *g)
{
    if (g == NULL) {
        return 0; /* headless */
    }
    return g->pipelines.count;
}

SDL_GPUColorTargetBlendState br_gpu_blend_premultiplied(void)
{
    SDL_GPUColorTargetBlendState b;
    SDL_zero(b);
    b.enable_blend = true;
    b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    b.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    return b;
}

void br_gpu_release_caches(br_gpu *g)
{
    for (uint32_t i = 0; i < g->pipelines.count; i++) {
        br_pipeline_entry *entry = br_array_at(&g->pipelines, i);
        SDL_ReleaseGPUGraphicsPipeline(g->device, entry->pipeline);
    }
    for (uint32_t i = 0; i < g->shaders.count; i++) {
        br_shader_entry *entry = br_array_at(&g->shaders, i);
        SDL_ReleaseGPUShader(g->device, entry->shader);
    }
    for (uint32_t i = 0; i < g->samplers.count; i++) {
        br_sampler_entry *entry = br_array_at(&g->samplers, i);
        SDL_ReleaseGPUSampler(g->device, entry->sampler);
    }
    br_array_free(&g->pipelines);
    br_array_free(&g->shaders);
    br_array_free(&g->samplers);
}
