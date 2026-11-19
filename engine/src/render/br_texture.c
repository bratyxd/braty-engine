/* br_texture: texture table (sources, frames, render textures), fallbacks, image loader. */
#include "render/br_render_internal.h"
#include "braty/br_assets.h"

#include "stb/stb_image.h"

static br_texture br_texture_zero(void)
{
    br_texture t = { 0, 0 };
    return t;
}

br_texture_record *br_texture_record_get(br_engine *e, br_texture t)
{
    if (e->render == NULL) {
        return NULL;
    }
    return br_pool_get(&e->render->textures, t.index, t.generation);
}

/* Allocates a record. The pointer dies at the next allocation. */
static br_texture_record *br_texture_alloc(br_engine *e, br_texture *out)
{
    br_texture_record *rec = br_pool_alloc(&e->render->textures, &out->index, &out->generation);
    if (rec != NULL) {
        rec->resolution = 1.0f;
    }
    return rec;
}

static void br_texture_set_whole_frame(br_texture_record *rec, uint32_t width, uint32_t height)
{
    rec->source_width = width;
    rec->source_height = height;
    SDL_zero(rec->frame);
    rec->frame.frame = br_rect_make(0.0f, 0.0f, (float)width, (float)height);
    rec->frame.orig = rec->frame.frame;
}

br_texture br_texture_create_rgba8(br_engine *e, uint32_t width, uint32_t height, const void *pixels,
                                   const br_texture_style *style)
{
    if (width == 0 || height == 0) {
        br_log_error("br_texture_create_rgba8: zero size");
        return br_texture_zero();
    }
    br_gpu *g = br_engine_gpu(e);
    br_gpu_texture *gpu = NULL;
    bool mipmaps = style != NULL && style->mipmaps;
    if (g != NULL) {
        uint8_t *premultiplied = NULL;
        if (pixels != NULL) {
            premultiplied = br_alloc((size_t)width * height * 4);
            if (premultiplied == NULL) {
                return br_texture_zero();
            }
            const uint8_t *src = pixels;
            for (size_t i = 0; i < (size_t)width * height; i++) {
                uint32_t a = src[i * 4 + 3];
                premultiplied[i * 4 + 0] = (uint8_t)((src[i * 4 + 0] * a + 127) / 255);
                premultiplied[i * 4 + 1] = (uint8_t)((src[i * 4 + 1] * a + 127) / 255);
                premultiplied[i * 4 + 2] = (uint8_t)((src[i * 4 + 2] * a + 127) / 255);
                premultiplied[i * 4 + 3] = (uint8_t)a;
            }
        }
        gpu = br_gpu_texture_create_rgba8(g, width, height, premultiplied, mipmaps);
        br_free(premultiplied);
        if (gpu == NULL) {
            return br_texture_zero();
        }
    }
    br_texture t;
    br_texture_record *rec = br_texture_alloc(e, &t);
    if (rec == NULL) {
        br_gpu_texture_destroy(g, gpu);
        return br_texture_zero();
    }
    rec->source = t;
    rec->gpu = gpu;
    rec->owns_gpu = true;
    br_texture_set_whole_frame(rec, width, height);
    if (style != NULL) {
        rec->style = *style;
    }
    return t;
}

br_texture br_texture_create_frame(br_engine *e, br_texture source, const br_texture_frame *frame)
{
    br_texture_record *src = br_texture_record_get(e, source);
    if (src == NULL || frame == NULL) {
        br_log_error("br_texture_create_frame: invalid source texture");
        return br_texture_zero();
    }
    br_texture src_handle = src->source;
    uint32_t sw = src->source_width;
    uint32_t sh = src->source_height;
    float resolution = src->resolution;
    br_texture_style style = src->style;
    br_texture t;
    br_texture_record *rec = br_texture_alloc(e, &t);
    if (rec == NULL) {
        return br_texture_zero();
    }
    rec->source = src_handle;
    rec->source_width = sw;
    rec->source_height = sh;
    rec->resolution = resolution;
    rec->style = style;
    rec->frame = *frame;
    if (rec->frame.orig.width <= 0.0f || rec->frame.orig.height <= 0.0f) {
        rec->frame.orig = br_rect_make(0.0f, 0.0f, frame->frame.width, frame->frame.height);
    }
    return t;
}

br_texture br_texture_wrap_gpu(br_engine *e, br_gpu_texture *gpu, bool take_ownership)
{
    if (gpu == NULL) {
        return br_texture_zero();
    }
    br_texture t;
    br_texture_record *rec = br_texture_alloc(e, &t);
    if (rec == NULL) {
        return br_texture_zero();
    }
    rec->source = t;
    rec->gpu = gpu;
    rec->owns_gpu = take_ownership;
    br_texture_set_whole_frame(rec, gpu->width, gpu->height);
    return t;
}

br_texture br_render_texture_create(br_engine *e, float width, float height, float resolution)
{
    if (resolution <= 0.0f) {
        resolution = 1.0f;
    }
    uint32_t pw = (uint32_t)SDL_ceilf(width * resolution);
    uint32_t ph = (uint32_t)SDL_ceilf(height * resolution);
    if (pw == 0 || ph == 0) {
        br_log_error("br_render_texture_create: zero size");
        return br_texture_zero();
    }
    br_render_target *rt = NULL;
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        rt = br_gpu_render_target_create(g, pw, ph, br_gpu_swapchain_format(g), SDL_GPU_SAMPLECOUNT_1, true);
        if (rt == NULL) {
            return br_texture_zero();
        }
    }
    br_texture t;
    br_texture_record *rec = br_texture_alloc(e, &t);
    if (rec == NULL) {
        br_gpu_render_target_destroy(g, rt);
        return br_texture_zero();
    }
    rec->source = t;
    rec->target = rt;
    rec->resolution = resolution;
    br_texture_set_whole_frame(rec, pw, ph);
    return t;
}

bool br_render_texture_resize(br_engine *e, br_texture t, float width, float height)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    if (rec == NULL || rec->source.index != t.index) {
        return false;
    }
    uint32_t pw = (uint32_t)SDL_ceilf(width * rec->resolution);
    uint32_t ph = (uint32_t)SDL_ceilf(height * rec->resolution);
    if (pw == 0 || ph == 0) {
        return false;
    }
    if (rec->target != NULL && !br_gpu_render_target_resize(br_engine_gpu(e), rec->target, pw, ph)) {
        return false;
    }
    br_texture_set_whole_frame(rec, pw, ph);
    return true;
}

br_render_target *br_texture_render_target(br_engine *e, br_texture t)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    return rec != NULL ? rec->target : NULL;
}

void br_texture_destroy(br_engine *e, br_texture t)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    if (rec == NULL) {
        return;
    }
    br_gpu *g = br_engine_gpu(e);
    if (rec->owns_gpu) {
        br_gpu_texture_destroy(g, rec->gpu);
    }
    br_gpu_render_target_destroy(g, rec->target);
    br_pool_release(&e->render->textures, t.index, t.generation);
}

bool br_texture_valid(br_engine *e, br_texture t)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    return rec != NULL && br_texture_record_get(e, rec->source) != NULL;
}

br_texture br_texture_source_of(br_engine *e, br_texture t)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    if (rec == NULL || br_texture_record_get(e, rec->source) == NULL) {
        return br_texture_missing(e);
    }
    return rec->source;
}

bool br_texture_get_info(br_engine *e, br_texture t, br_texture_info *out)
{
    SDL_zerop(out);
    bool ok = br_texture_valid(e, t);
    br_texture_record *rec = br_texture_record_get(e, ok ? t : br_texture_missing(e));
    if (rec == NULL) {
        return false;
    }
    br_texture_record *src = br_texture_record_get(e, rec->source);
    out->resolution = rec->resolution;
    out->width = rec->frame.orig.width / rec->resolution;
    out->height = rec->frame.orig.height / rec->resolution;
    out->source_width = rec->source_width;
    out->source_height = rec->source_height;
    out->frame = rec->frame;
    out->source = rec->source;
    out->style = src != NULL ? src->style : rec->style;
    out->is_render_texture = src != NULL && src->target != NULL;

    float sw = (float)(rec->source_width > 0 ? rec->source_width : 1);
    float sh = (float)(rec->source_height > 0 ? rec->source_height : 1);
    br_rect f = rec->frame.frame;
    if (rec->frame.rotated) {
        /* stored 90 degrees clockwise: the region in the atlas is height x width */
        out->uvs[0] = br_vec2_make((f.x + f.height) / sw, f.y / sh);
        out->uvs[1] = br_vec2_make((f.x + f.height) / sw, (f.y + f.width) / sh);
        out->uvs[2] = br_vec2_make(f.x / sw, (f.y + f.width) / sh);
        out->uvs[3] = br_vec2_make(f.x / sw, f.y / sh);
    } else {
        out->uvs[0] = br_vec2_make(f.x / sw, f.y / sh);
        out->uvs[1] = br_vec2_make((f.x + f.width) / sw, f.y / sh);
        out->uvs[2] = br_vec2_make((f.x + f.width) / sw, (f.y + f.height) / sh);
        out->uvs[3] = br_vec2_make(f.x / sw, (f.y + f.height) / sh);
    }
    return ok;
}

void br_texture_set_style(br_engine *e, br_texture t, const br_texture_style *style)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    br_texture_record *src = rec != NULL ? br_texture_record_get(e, rec->source) : NULL;
    if (src != NULL && style != NULL) {
        src->style = *style;
    }
}

void br_texture_set_resolution(br_engine *e, br_texture t, float resolution)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    if (rec != NULL && resolution > 0.0f) {
        rec->resolution = resolution;
    }
}

br_gpu_texture *br_texture_gpu(br_engine *e, br_texture t)
{
    br_texture_record *rec = br_texture_record_get(e, t);
    br_texture_record *src = rec != NULL ? br_texture_record_get(e, rec->source) : NULL;
    if (src == NULL) {
        return NULL;
    }
    return src->target != NULL ? src->target->color : src->gpu;
}

bool br_texture_binding(br_engine *e, br_texture source, SDL_GPUTextureSamplerBinding *out)
{
    br_gpu *g = br_engine_gpu(e);
    br_texture_record *src = br_texture_record_get(e, source);
    br_gpu_texture *gpu = br_texture_gpu(e, source);
    if (gpu == NULL) {
        src = br_texture_record_get(e, br_texture_white(e));
        gpu = src != NULL ? src->gpu : NULL;
    }
    if (gpu == NULL || src == NULL) {
        return false;
    }
    out->texture = gpu->handle;
    br_texture_style style = src->style;
    if (style.mipmaps && gpu->mip_levels <= 1) {
        style.mipmaps = false;
    }
    out->sampler = br_gpu_sampler_get(g, &style);
    return out->sampler != NULL;
}

/* The built-in textures are made on first use: the GPU does not exist yet while modules register. */
static br_texture br_texture_builtin(br_engine *e, br_texture *slot, uint32_t size, uint32_t rgba)
{
    br_texture_record *rec = br_texture_record_get(e, *slot);
    bool needs_gpu = br_engine_gpu(e) != NULL && rec != NULL && rec->gpu == NULL;
    if (rec != NULL && !needs_gpu) {
        return *slot;
    }
    if (rec != NULL) {
        br_texture_destroy(e, *slot);
    }
    uint32_t pixels[4];
    for (uint32_t i = 0; i < 4; i++) {
        pixels[i] = rgba;
    }
    *slot = br_texture_create_rgba8(e, size, size, pixels, NULL);
    return *slot;
}

br_texture br_texture_white(br_engine *e)
{
    return br_texture_builtin(e, &e->render->white, 1, br_color_to_rgba8(br_color_make(1.0f, 1.0f, 1.0f, 1.0f)));
}

br_texture br_texture_missing(br_engine *e)
{
    return br_texture_builtin(e, &e->render->missing, 2, br_color_to_rgba8(br_color_make(1.0f, 0.0f, 1.0f, 1.0f)));
}

uint32_t br_texture_count(br_engine *e)
{
    return e->render->textures.alive_count;
}

void br_texture_init(br_engine *e)
{
    br_pool_init(&e->render->textures, sizeof(br_texture_record));
}

void br_texture_shutdown(br_engine *e)
{
    br_pool *p = &e->render->textures;
    for (uint32_t i = 0; i < br_pool_capacity(p); i++) {
        if (br_pool_slot_alive(p, i)) {
            br_texture t = { i, br_pool_slot_generation(p, i) };
            br_texture_destroy(e, t);
        }
    }
    br_pool_free(p);
}

/* ---- loader */

static void *br_texture_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    if (size > 0x7fffffff) {
        br_log_error("texture '%s': file too large", path);
        return NULL;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (pixels == NULL) {
        br_log_error("texture '%s': cannot decode: %s", path, stbi_failure_reason());
        return NULL;
    }
    br_texture t = br_texture_create_rgba8(e, (uint32_t)w, (uint32_t)h, pixels, NULL);
    stbi_image_free(pixels);
    br_texture *asset = br_alloc(sizeof(br_texture));
    if (t.generation == 0 || asset == NULL) {
        br_texture_destroy(e, t);
        br_free(asset);
        return NULL;
    }
    *asset = t;
    return asset;
}

static void br_texture_asset_unload(br_engine *e, void *asset)
{
    br_texture *t = asset;
    br_texture_destroy(e, *t);
    br_free(t);
}

void br_texture_register_loader(br_engine *e)
{
    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "texture";
    desc.extensions = "png,jpg,jpeg,bmp,tga,gif,psd,hdr";
    desc.load = br_texture_asset_load;
    desc.unload = br_texture_asset_unload;
    br_assets_register_loader(e, &desc);
}

br_texture br_texture_load(br_engine *e, const char *path)
{
    br_texture *t = br_assets_load(e, path);
    if (t != NULL && !br_str_equal(br_assets_type(e, path), "texture")) {
        br_log_error("br_texture_load: '%s' is not a texture", path);
        t = NULL;
    }
    if (t == NULL) {
        br_log_error("br_texture_load: using the missing texture for '%s'", path);
        return br_texture_missing(e);
    }
    return *t;
}
