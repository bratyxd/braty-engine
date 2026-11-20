/* br_extract: br_generate_texture (rendered by an overlay during collect) and GPU readback. */
#include "filters/br_filters_internal.h"

#include "stb/stb_image_write.h"

br_texture br_generate_texture(br_engine *e, br_node n, const br_generate_texture_options *options)
{
    br_texture none = { 0, 0 };
    br_filters_state *fs = e->filters;
    if (fs == NULL || !br_node_valid(e, n)) {
        return none;
    }
    br_generate_texture_options o;
    SDL_zero(o);
    if (options != NULL) {
        o = *options;
    }
    br_rect region = o.region;
    if (br_rect_is_empty(region)) {
        region = br_node_bounds(e, n);
    }
    if (br_rect_is_empty(region)) {
        br_log_error("br_generate_texture: empty region");
        return none;
    }
    float resolution = o.resolution > 0.0f ? o.resolution : br_engine_pixel_density(e);
    br_texture t = br_render_texture_create(e, region.width, region.height, resolution);
    if (t.generation == 0) {
        return none;
    }
    if (br_engine_gpu(e) != NULL) {
        br_generate_job job = { n, t, region, o.clear_color };
        br_array_push(&fs->generate_jobs, &job);
    }
    return t;
}

/* Overlay: renders the queued br_generate_texture jobs (each into its own texture). */
static void br_extract_overlay(br_engine *e, br_render_ctx *rc, void *user)
{
    br_filters_state *fs = user;
    for (uint32_t i = 0; i < fs->generate_jobs.count; i++) {
        br_generate_job *job = br_array_at(&fs->generate_jobs, i);
        if (br_node_valid(e, job->node) && br_texture_valid(e, job->target)) {
            br_render_subtree_to_texture(rc, job->node, job->target, job->region, true, job->clear);
        }
    }
    br_array_clear(&fs->generate_jobs);
}

void br_extract_init(br_engine *e)
{
    br_render_add_overlay(e, br_extract_overlay, e->filters);
}

bool br_extract_pixels(br_engine *e, br_texture t, uint8_t **rgba, uint32_t *width, uint32_t *height)
{
    *rgba = NULL;
    *width = 0;
    *height = 0;
    br_gpu *g = br_engine_gpu(e);
    br_gpu_texture *gpu = br_texture_gpu(e, t);
    br_texture_info info;
    if (g == NULL || gpu == NULL || !br_texture_get_info(e, t, &info)) {
        br_log_error("br_extract_pixels: no GPU pixels for this texture");
        return false;
    }
    uint8_t *all = NULL;
    if (!br_gpu_texture_read(g, gpu, &all)) {
        return false;
    }
    // ponytail: rotated atlas frames are returned as stored (not rotated back); add that when a game needs it.
    br_rect f = info.frame.frame;
    uint32_t x0 = (uint32_t)SDL_max(f.x, 0.0f);
    uint32_t y0 = (uint32_t)SDL_max(f.y, 0.0f);
    uint32_t w = (uint32_t)f.width;
    uint32_t h = (uint32_t)f.height;
    if (x0 + w > gpu->width || y0 + h > gpu->height || w == 0 || h == 0) {
        x0 = 0;
        y0 = 0;
        w = gpu->width;
        h = gpu->height;
    }
    uint8_t *out = br_alloc((size_t)w * h * 4);
    if (out == NULL) {
        br_free(all);
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = all + ((size_t)(y0 + y) * gpu->width + x0) * 4;
        uint8_t *dst = out + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            /* textures are premultiplied; callers get straight alpha (like Pixi's extract) */
            uint8_t a = src[x * 4 + 3];
            for (int c = 0; c < 3; c++) {
                uint32_t v = src[x * 4 + c];
                dst[x * 4 + c] = (uint8_t)(a > 0 ? SDL_min(255u, (v * 255u + a / 2u) / a) : 0u);
            }
            dst[x * 4 + 3] = a;
        }
    }
    br_free(all);
    *rgba = out;
    *width = w;
    *height = h;
    return true;
}

/* stb_image_write callback: appends to a byte array. */
static void br_extract_png_write(void *context, void *data, int size)
{
    br_array_push_n(context, data, (uint32_t)size);
}

bool br_extract_image_png(br_engine *e, br_texture t, const char *path)
{
    uint8_t *rgba;
    uint32_t w, h;
    if (!br_extract_pixels(e, t, &rgba, &w, &h)) {
        return false;
    }
    br_array png;
    br_array_init(&png, 1);
    bool ok = stbi_write_png_to_func(br_extract_png_write, &png, (int)w, (int)h, 4, rgba, (int)w * 4) != 0;
    ok = ok && br_file_write_all(e, path, png.data, png.count);
    if (!ok) {
        br_log_error("br_extract_image_png: cannot write '%s'", path);
    }
    br_array_free(&png);
    br_free(rgba);
    return ok;
}
