/* br_view3d: the "view3d" 2D node type. Owns a root 3D node, a camera, GPU targets and post
 * settings; its render callback collects the 3D scene, pushes the own-passes 3D callback and
 * then draws the tone-mapped texture as its quad (so 2D filters/masks/alpha/blend apply). */
#include "three_d/br_three_d_internal.h"

#define BR_HDR_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
#define BR_SHADOW_FORMAT SDL_GPU_TEXTUREFORMAT_R32_FLOAT

br_view3d_state *br_view3d_state_get(br_engine *e, br_node view)
{
    if (e->three_d == NULL) {
        return NULL;
    }
    br_view3d_node *vn = br_node_data(e, view, e->three_d->view_type);
    return vn != NULL ? vn->state : NULL;
}

br_post3d_settings br_post3d_default(void)
{
    br_post3d_settings p;
    SDL_zero(p);
    p.tonemap = BR_TONEMAP_ACES;
    p.exposure = 1.0f;
    p.bloom = true;
    p.bloom_threshold = 1.0f;
    p.bloom_intensity = 0.6f;
    p.bloom_levels = 5;
    return p;
}

/* ---- GPU targets */

void br_render3d_release_targets(br_engine *e, br_view3d_state *v)
{
    br_gpu *g = br_engine_gpu(e);
    if (g == NULL) {
        return;
    }
    br_gpu_render_target_destroy(g, v->hdr);
    v->hdr = NULL;
    for (int i = 0; i < 2; i++) {
        br_gpu_render_target_destroy(g, v->post_temp[i]);
        v->post_temp[i] = NULL;
    }
    for (uint32_t i = 0; i < BR_BLOOM3D_MAX_LEVELS; i++) {
        br_gpu_render_target_destroy(g, v->bloom[i]);
        v->bloom[i] = NULL;
    }
    v->bloom_count = 0;
    v->pixel_width = 0;
    v->pixel_height = 0;
}

static void br_view3d_free_all_targets(br_engine *e, br_view3d_state *v)
{
    br_gpu *g = br_engine_gpu(e);
    br_render3d_release_targets(e, v);
    if (g != NULL) {
        br_gpu_render_target_destroy(g, v->shadow_atlas);
        br_gpu_buffer_destroy(g, v->joint_buffer);
        if (v->joint_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(br_gpu_device(g), v->joint_transfer);
        }
    }
    v->shadow_atlas = NULL;
    v->joint_buffer = NULL;
    v->joint_transfer = NULL;
    v->joint_capacity = 0;
    if (v->output.generation != 0) {
        br_texture_destroy(e, v->output);
        v->output.generation = 0;
    }
}

static SDL_GPUSampleCount br_sample_count(br_gpu *g, uint32_t samples)
{
    SDL_GPUSampleCount wanted = SDL_GPU_SAMPLECOUNT_1;
    if (samples >= 8) {
        wanted = SDL_GPU_SAMPLECOUNT_8;
    } else if (samples >= 4) {
        wanted = SDL_GPU_SAMPLECOUNT_4;
    } else if (samples >= 2) {
        wanted = SDL_GPU_SAMPLECOUNT_2;
    }
    while (wanted != SDL_GPU_SAMPLECOUNT_1 &&
           !SDL_GPUTextureSupportsSampleCount(br_gpu_device(g), BR_HDR_FORMAT, wanted)) {
        wanted = (SDL_GPUSampleCount)(wanted - 1);
    }
    return wanted;
}

/* Creates/recreates every target this frame needs. False if the view cannot render. */
static bool br_view3d_ensure_targets(br_engine *e, br_view3d_state *v)
{
    br_gpu *g = br_engine_gpu(e);
    br_three_d_state *s = e->three_d;
    if (g != NULL && s->dummy_storage == NULL) {
        float zero[16];
        SDL_zeroa(zero);
        s->dummy_storage = br_gpu_buffer_create(g, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, sizeof(zero), zero);
    }
    float resolution = v->resolution > 0.0f ? v->resolution : br_engine_pixel_density(e);
    if (v->width < 1.0f || v->height < 1.0f) {
        return false;
    }
    /* the 8-bit output: a render texture so the 2D side can draw it (and filters can use it) */
    br_texture_info info;
    if (v->output.generation != 0 && br_texture_get_info(e, v->output, &info) && info.resolution != resolution) {
        br_texture_destroy(e, v->output);
        v->output.generation = 0;
    }
    if (v->output.generation == 0) {
        v->output = br_render_texture_create(e, v->width, v->height, resolution);
    } else {
        br_render_texture_resize(e, v->output, v->width, v->height);
    }
    if (g == NULL) {
        return v->output.generation != 0; /* headless: collect only, the output has no pixels */
    }
    br_render_target *out = br_texture_render_target(e, v->output);
    if (out == NULL) {
        return false;
    }
    SDL_GPUSampleCount samples = br_sample_count(g, v->msaa);
    if (v->hdr != NULL && (out->width != v->pixel_width || out->height != v->pixel_height || samples != v->hdr->sample_count)) {
        br_render3d_release_targets(e, v); /* resize or MSAA change: recreate everything */
    }
    if (v->hdr == NULL) {
        v->pixel_width = out->width;
        v->pixel_height = out->height;
        v->hdr = br_gpu_render_target_create(g, out->width, out->height, BR_HDR_FORMAT, samples, true);
        if (v->hdr == NULL) {
            return false;
        }
    }
    v->samples = (uint32_t)v->hdr->sample_count;
    /* bloom chain: half resolution and below */
    uint32_t levels = v->post.bloom ? SDL_clamp(v->post.bloom_levels, 1u, (uint32_t)BR_BLOOM3D_MAX_LEVELS) : 0;
    for (uint32_t i = 0; i < BR_BLOOM3D_MAX_LEVELS; i++) {
        uint32_t w = v->pixel_width >> (i + 1);
        uint32_t h = v->pixel_height >> (i + 1);
        bool wanted = i < levels && w >= 2 && h >= 2;
        if (!wanted) {
            br_gpu_render_target_destroy(g, v->bloom[i]);
            v->bloom[i] = NULL;
            levels = SDL_min(levels, i);
            continue;
        }
        if (v->bloom[i] == NULL) {
            v->bloom[i] = br_gpu_render_target_create(g, w, h, BR_HDR_FORMAT, SDL_GPU_SAMPLECOUNT_1, false);
        }
        if (v->bloom[i] == NULL) {
            levels = i;
        }
    }
    v->bloom_count = levels;
    uint32_t enabled_passes = 0;
    for (uint32_t i = 0; i < v->pass_count; i++) {
        enabled_passes += v->passes[i].enabled ? 1u : 0u;
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (i < enabled_passes && v->post_temp[i] == NULL) {
            v->post_temp[i] = br_gpu_render_target_create(g, v->pixel_width, v->pixel_height, BR_HDR_FORMAT,
                                                          SDL_GPU_SAMPLECOUNT_1, false);
        }
    }
    return true;
}

static void br_view3d_ensure_shadow_atlas(br_engine *e, br_view3d_state *v)
{
    br_gpu *g = br_engine_gpu(e);
    if (v->shadow_atlas != NULL && v->shadow_atlas_tile != v->shadow_tile_size) {
        br_gpu_render_target_destroy(g, v->shadow_atlas);
        v->shadow_atlas = NULL;
    }
    if (v->shadow_atlas == NULL) {
        v->shadow_atlas = br_gpu_render_target_create(g, v->shadow_tile_size * 4, v->shadow_tile_size * 2,
                                                      BR_SHADOW_FORMAT, SDL_GPU_SAMPLECOUNT_1, true);
        v->shadow_atlas_tile = v->shadow_tile_size;
    }
}

/* ---- the 2D node type */

static void br_view3d_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    br_view3d_state *v = ((const br_view3d_node *)data)->state;
    if (v == NULL) {
        return;
    }
    uint64_t frame = br_ticker_frame(e);
    if (!v->collected || v->collected_frame != frame) {
        /* first draw this frame: collect and queue the 3D passes. A second draw in the same
         * frame (e.g. inside a filter) reuses the image: the callback runs before both quads. */
        if (v->auto_resize) {
            int w = 0;
            int h = 0;
            br_engine_window_size(e, &w, &h);
            if (w > 0 && h > 0) {
                v->width = (float)w;
                v->height = (float)h;
            }
        }
        if (!br_view3d_ensure_targets(e, v)) {
            return;
        }
        if (!br_node3d_valid(e, v->root)) {
            v->root = v->own_root; /* a shared root was destroyed */
        }
        br_render3d_collect(e, v);
        if (v->shadow_tiles != 0 && br_engine_gpu(e) != NULL) {
            br_view3d_ensure_shadow_atlas(e, v);
        }
        v->collected = true;
        v->collected_frame = frame;
        br_render_push_callback(rc, br_render3d_draw, v, true);
    }
    const br_texture_info *info = br_render_texture_info(rc, v->output);
    br_mat3 m = br_render_node_transform(rc, n);
    uint32_t color = br_render_node_color(rc, n);
    br_vec2 corners[4] = { { 0.0f, 0.0f }, { v->width, 0.0f }, { v->width, v->height }, { 0.0f, v->height } };
    br_vertex2d quad[4];
    for (int i = 0; i < 4; i++) {
        br_vec2 p = br_mat3_apply(m, corners[i]);
        quad[i] = (br_vertex2d){ p.x, p.y, info->uvs[i].x, info->uvs[i].y, color };
    }
    br_render_push_quad(rc, v->output, quad);
}

static bool br_view3d_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_view3d_state *v = ((const br_view3d_node *)data)->state;
    if (v == NULL) {
        return false;
    }
    *out = br_rect_make(0.0f, 0.0f, v->width, v->height);
    return true;
}

static bool br_view3d_hit_test(br_engine *e, br_node n, const void *data, br_vec2 local_point)
{
    (void)n;
    br_view3d_state *v = ((const br_view3d_node *)data)->state;
    return v != NULL && br_pick3d_view_hit_test(e, v, local_point);
}

static void br_view3d_on_create(br_engine *e, br_node n, void *data)
{
    br_view3d_node *vn = data;
    br_view3d_state *v = br_alloc(sizeof(br_view3d_state));
    vn->state = v;
    if (v == NULL) {
        return;
    }
    v->node = n;
    v->width = 1.0f;
    v->height = 1.0f;
    v->msaa = 1;
    v->occlusion_triangles = BR_VIEW3D_OCCLUSION_TRIANGLES;
    v->background = br_color_make(0.0f, 0.0f, 0.0f, 0.0f);
    v->sky = br_color_make(0.6f, 0.7f, 0.9f, 1.0f);
    v->ground = br_color_make(0.25f, 0.2f, 0.15f, 1.0f);
    v->ambient_intensity = 0.4f;
    v->culling_mask = 0xFFFFFFFFu;
    v->shadow_tile_size = 1024;
    v->post = br_post3d_default();
    br_array_init(&v->items, sizeof(br_item3d));
    br_array_init(&v->opaque, sizeof(uint32_t));
    br_array_init(&v->transparent, sizeof(uint32_t));
    br_array_init(&v->shadow, sizeof(uint32_t));
    br_array_init(&v->joints, sizeof(br_mat4));
    br_array_init(&v->lights, sizeof(br_light3d_entry));
    v->root = br_node3d_create_container(e);
    v->own_root = v->root;
    br_node3d_set_label(e, v->root, "root");
    v->camera = br_camera3d_create(e, NULL);
    br_node3d_set_label(e, v->camera, "camera");
    br_node3d_add_child(e, v->root, v->camera);
    br_node3d_set_position(e, v->camera, br_vec3_make(0.0f, 2.0f, 8.0f));
    br_node3d_look_at(e, v->camera, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_array_push(&e->three_d->views, &v);
}

static void br_view3d_on_destroy(br_engine *e, br_node n, void *data)
{
    (void)n;
    br_view3d_node *vn = data;
    br_view3d_state *v = vn->state;
    if (v == NULL) {
        return;
    }
    br_three_d_state *s = e->three_d;
    for (uint32_t i = 0; i < s->views.count; i++) {
        if (*(br_view3d_state **)br_array_at(&s->views, i) == v) {
            br_array_remove_swap(&s->views, i);
            break;
        }
    }
    br_node3d_destroy(e, v->own_root);
    br_view3d_free_all_targets(e, v);
    br_array_free(&v->items);
    br_array_free(&v->opaque);
    br_array_free(&v->transparent);
    br_array_free(&v->shadow);
    br_array_free(&v->joints);
    br_array_free(&v->lights);
    br_free(v);
    vn->state = NULL;
}

void br_view3d_register_type(br_engine *e)
{
    br_node_type_desc d;
    SDL_zero(d);
    d.name = "view3d";
    d.data_size = sizeof(br_view3d_node);
    d.on_create = br_view3d_on_create;
    d.on_destroy = br_view3d_on_destroy;
    d.local_bounds = br_view3d_bounds;
    d.render = br_view3d_render;
    d.hit_test = br_view3d_hit_test;
    e->three_d->view_type = br_node_type_register(e, &d);
}

/* ---- API */

uint32_t br_view3d_type(br_engine *e)
{
    return e->three_d != NULL ? e->three_d->view_type : 0;
}

br_node br_view3d_create(br_engine *e, float width, float height)
{
    br_node n = br_node_create(e, br_view3d_type(e));
    br_view3d_state *v = br_view3d_state_get(e, n);
    if (v == NULL) {
        br_node_destroy(e, n);
        br_node none = { 0, 0 };
        return none;
    }
    v->width = width;
    v->height = height;
    br_node_set_event_mode(e, n, BR_EVENT_MODE_STATIC);
    br_pick3d_attach_view(e, v);
    return n;
}

br_node3d br_view3d_root(br_engine *e, br_node view)
{
    br_node3d none = { 0, 0 };
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->root : none;
}

void br_view3d_set_root(br_engine *e, br_node view, br_node3d root)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL && br_node3d_valid(e, root)) {
        v->root = root;
    }
}

void br_view3d_set_camera(br_engine *e, br_node view, br_node3d camera)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL && br_camera3d_get(e, camera) != NULL) {
        v->camera = camera;
    }
}

br_node3d br_view3d_camera(br_engine *e, br_node view)
{
    br_node3d none = { 0, 0 };
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->camera : none;
}

void br_view3d_set_size(br_engine *e, br_node view, float width, float height)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->width = SDL_max(width, 1.0f);
        v->height = SDL_max(height, 1.0f);
    }
}

br_vec2 br_view3d_size(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? br_vec2_make(v->width, v->height) : br_vec2_make(0.0f, 0.0f);
}

void br_view3d_set_auto_resize(br_engine *e, br_node view, bool auto_resize)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->auto_resize = auto_resize;
    }
}

void br_view3d_set_resolution(br_engine *e, br_node view, float resolution)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->resolution = SDL_max(resolution, 0.0f);
    }
}

void br_view3d_set_msaa(br_engine *e, br_node view, uint32_t samples)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->msaa = samples;
    }
}

void br_view3d_set_occlusion_culling(br_engine *e, br_node view, uint32_t triangles)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->occlusion_triangles = triangles;
    }
}

uint32_t br_view3d_occlusion_culling(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->occlusion_triangles : 0;
}

void br_view3d_set_background(br_engine *e, br_node view, br_color color)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->background = br_color_premultiply(color);
    }
}

void br_view3d_set_environment(br_engine *e, br_node view, br_color sky, br_color ground, float intensity)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->sky = sky;
        v->ground = ground;
        v->ambient_intensity = intensity;
    }
}

void br_view3d_set_culling_mask(br_engine *e, br_node view, uint32_t mask)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->culling_mask = mask;
    }
}

void br_view3d_set_shadow_resolution(br_engine *e, br_node view, uint32_t tile_size)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL) {
        v->shadow_tile_size = SDL_clamp(tile_size, 64u, 4096u);
    }
}

br_texture br_view3d_texture(br_engine *e, br_node view)
{
    br_texture none = { 0, 0 };
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->output : none;
}

bool br_view3d_auto_resize(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL && v->auto_resize;
}

float br_view3d_resolution(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->resolution : 0.0f;
}

uint32_t br_view3d_msaa(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->msaa : 0;
}

br_color br_view3d_background(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    br_color c = br_color_make(0.0f, 0.0f, 0.0f, 0.0f);
    if (v != NULL && v->background.a > 0.0f) {
        c = br_color_make(v->background.r / v->background.a, v->background.g / v->background.a,
                          v->background.b / v->background.a, v->background.a);
    }
    return c;
}

bool br_view3d_get_environment(br_engine *e, br_node view, br_color *sky, br_color *ground, float *intensity)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL) {
        return false;
    }
    if (sky != NULL) {
        *sky = v->sky;
    }
    if (ground != NULL) {
        *ground = v->ground;
    }
    if (intensity != NULL) {
        *intensity = v->ambient_intensity;
    }
    return true;
}

uint32_t br_view3d_culling_mask(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->culling_mask : 0;
}

uint32_t br_view3d_shadow_resolution(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? v->shadow_tile_size : 0;
}

bool br_view3d_copy_settings(br_engine *e, br_node from, br_node to)
{
    br_view3d_state *a = br_view3d_state_get(e, from);
    br_view3d_state *b = br_view3d_state_get(e, to);
    if (a == NULL || b == NULL) {
        br_log_error("br_view3d_copy_settings: both nodes must be view3d nodes");
        return false;
    }
    b->msaa = a->msaa;
    b->occlusion_triangles = a->occlusion_triangles;
    b->resolution = a->resolution;
    b->background = a->background; /* premultiplied both sides: no round trip */
    b->sky = a->sky;
    b->ground = a->ground;
    b->ambient_intensity = a->ambient_intensity;
    b->culling_mask = a->culling_mask;
    b->shadow_tile_size = a->shadow_tile_size;
    b->post = a->post;
    SDL_memcpy(b->passes, a->passes, sizeof(b->passes));
    b->pass_count = a->pass_count;
    return true;
}

void br_view3d_get_stats(br_engine *e, br_node view, br_view3d_stats *out)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (out == NULL) {
        return;
    }
    SDL_zero(*out);
    if (v != NULL) {
        *out = v->stats;
    }
}

/* ---- post settings */

void br_view3d_set_post(br_engine *e, br_node view, const br_post3d_settings *settings)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v != NULL && settings != NULL) {
        v->post = *settings;
    }
}

br_post3d_settings *br_view3d_post(br_engine *e, br_node view)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    return v != NULL ? &v->post : NULL;
}

int br_view3d_add_post_pass(br_engine *e, br_node view, const br_shader_blob *fragment_shader, const void *uniforms,
                            uint32_t uniform_size)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL || fragment_shader == NULL || v->pass_count >= BR_POST3D_MAX_PASSES ||
        uniform_size > BR_POST3D_UNIFORM_SIZE) {
        br_log_error("br_view3d_add_post_pass: invalid view, shader or uniforms, or too many passes");
        return -1;
    }
    br_post3d_pass *p = &v->passes[v->pass_count];
    SDL_zero(*p);
    p->fragment_shader = fragment_shader;
    if (uniforms != NULL && uniform_size > 0) {
        SDL_memcpy(p->uniforms, uniforms, uniform_size);
    }
    p->uniform_size = uniform_size;
    p->enabled = true;
    v->pass_count++;
    return (int)v->pass_count - 1;
}

br_post3d_pass *br_view3d_post_pass(br_engine *e, br_node view, int index)
{
    br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL || index < 0 || (uint32_t)index >= v->pass_count) {
        return NULL;
    }
    return &v->passes[index];
}
