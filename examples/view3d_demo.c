/* view3d_demo - the 3D module: a View3D over/under 2D content.
 *
 *   view3d_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *               [--camera 0|1]   camera angle (default 0); left-drag orbits, wheel zooms
 *               [--count N]      instanced cubes in the field (default 3000)
 *               [--msaa N]       MSAA samples of the HDR pass (default 4)
 *               [--novsync]      measure frame time without vsync
 *               [--resize-at N]  resize the window to 1000x640 at frame N (targets are recreated)
 *               [--filter]       a 2D colour-matrix filter (sepia) on the whole view node
 *
 * Scene (y up, the camera looks at the middle row):
 *   front row   cube, sphere, cylinder, cone, torus, capsule (PBR), a glass box (transparent)
 *   middle row  skinned two-bone cylinder | PBR, Phong, Flat, Toon spheres | morphing sphere
 *   back rows   textured PBR spheres: metallic 0 -> 1 (bottom), roughness 0 -> 1 (top),
 *               a panel textured with a live 2D render texture, an emissive ring (bloom)
 *   lights      sun with 3 shadow cascades, a warm point light, a cyan spot light with shadows
 *   field       N instanced cubes (one batched_mesh3d) around the scene
 *   2D          a gradient sky + text UNDER the view (its background is transparent), a title,
 *               a label following the torus via br_view3d_world_to_screen and a badge OVER it.
 *   extra       a custom 3D node type drawing axis lines (custom draw item), a vignette post
 *               pass, click on the front-row shapes (3D pointer events) to recolour them.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

#include "lines3d.frag.h"
#include "lines3d.vert.h"
#include "post3d_vignette.frag.h"

static int g_camera = 0;
static int g_count = 3000;
static int g_msaa = 4;
static bool g_no_vsync = false;
static int g_resize_at = -1;
static bool g_filter = false;

enum { TEX = 256, SHAPES = 6 };

typedef struct demo_axes {
    uint32_t vertex_offset; /* transient vertices of this frame */
} demo_axes;

typedef struct demo_state {
    br_node         view;
    br_node3d       root;
    br_node3d       camera;
    br_camera_orbit orbit;
    br_node3d       shapes[SHAPES];
    br_material3d   shape_materials[SHAPES];
    br_node3d       torus;
    br_node3d       skinned;
    br_mat4         joints[2];
    br_node3d       morphing;
    br_node3d       ring;
    br_node         panel_2d;      /* off-stage 2D content drawn into panel_texture each frame */
    br_node         panel_star;
    br_texture      panel_texture;
    br_node         label;
    br_node         stats_text;
    uint32_t        axes_type;
    demo_axes       axes;          /* heap-stable user data of the custom draw */
    uint64_t        last_ns;
    uint64_t        frame_ns_sum;
    uint64_t        frame_ns_count;
    uint32_t        rng;
} demo_state;

/* The View3D's own camera on an orbit: left-drag turns, the wheel zooms. (A br_camera gets the
 * same from its "camera.orbit" component; this example drives a View3D camera node itself.) */
static void orbit_view_camera(br_engine *e, br_camera_orbit *orbit, br_node3d camera, bool use_input)
{
    if (use_input && br_input_mouse_down(e, SDL_BUTTON_LEFT)) {
        br_vec2 d = br_input_mouse_delta(e);
        orbit->yaw -= d.x * orbit->rotate_speed;
        orbit->pitch = br_clampf(orbit->pitch + d.y * orbit->rotate_speed, -1.5f, 1.5f);
    }
    br_vec2 wheel = use_input ? br_input_mouse_wheel(e) : br_vec2_make(0.0f, 0.0f);
    if (wheel.y != 0.0f) {
        orbit->distance *= SDL_powf(1.0f - orbit->zoom_speed, wheel.y);
        orbit->distance = br_clampf(orbit->distance, orbit->min_distance, orbit->max_distance);
    }
    float cp = SDL_cosf(orbit->pitch);
    br_vec3 offset = br_vec3_make(SDL_sinf(orbit->yaw) * cp, SDL_sinf(orbit->pitch), SDL_cosf(orbit->yaw) * cp);
    br_node3d_set_position(e, camera, br_vec3_add(orbit->target, br_vec3_scale(offset, orbit->distance)));
    br_node3d_look_at(e, camera, orbit->target, br_vec3_make(0.0f, 1.0f, 0.0f));
}

static float frand(demo_state *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

static br_color lin(uint32_t rgb)
{
    return br_color_to_linear(br_color_from_hex(rgb, 1.0f));
}

/* ---- generated textures */

static br_texture make_checker(br_engine *e)
{
    static uint8_t px[TEX * TEX * 4];
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            bool odd = ((x / 32) + (y / 32)) % 2 == 1;
            uint8_t *p = &px[(y * TEX + x) * 4];
            p[0] = odd ? 230 : 200;
            p[1] = odd ? 190 : 80;
            p[2] = odd ? 120 : 60;
            p[3] = 255;
        }
    }
    br_texture_style style = { BR_FILTER_LINEAR, BR_ADDRESS_REPEAT, true, 8.0f };
    return br_texture_create_rgba8(e, TEX, TEX, px, &style);
}

/* Tangent-space normal map of round bumps (one per checker cell). */
static br_texture make_bumps(br_engine *e)
{
    static uint8_t px[TEX * TEX * 4];
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            float fx = (float)(x % 32) / 16.0f - 1.0f;
            float fy = (float)(y % 32) / 16.0f - 1.0f;
            float r2 = fx * fx + fy * fy;
            br_vec3 n = br_vec3_make(0.0f, 0.0f, 1.0f);
            if (r2 < 0.8f) {
                /* +Y of a normal map points to the top of the image (-v) */
                n = br_vec3_normalize(br_vec3_make(fx * 0.8f, -fy * 0.8f, 1.0f));
            }
            uint8_t *p = &px[(y * TEX + x) * 4];
            p[0] = (uint8_t)((n.x * 0.5f + 0.5f) * 255.0f);
            p[1] = (uint8_t)((n.y * 0.5f + 0.5f) * 255.0f);
            p[2] = (uint8_t)((n.z * 0.5f + 0.5f) * 255.0f);
            p[3] = 255;
        }
    }
    br_texture_style style = { BR_FILTER_LINEAR, BR_ADDRESS_REPEAT, true, 8.0f };
    return br_texture_create_rgba8(e, TEX, TEX, px, &style);
}

static br_texture make_sky(br_engine *e)
{
    static uint8_t px[256 * 4];
    for (int y = 0; y < 256; y++) {
        br_color c = br_color_lerp(br_color_from_hex(0x2B4C8C, 1.0f), br_color_from_hex(0xF2C38B, 1.0f), (float)y / 255.0f);
        uint32_t rgba = br_color_to_rgba8(c);
        SDL_memcpy(&px[y * 4], &rgba, 4);
    }
    return br_texture_create_rgba8(e, 1, 256, px, NULL);
}

/* ---- scene helpers */

static br_material3d make_material(br_engine *e, br_material3d_model model, uint32_t srgb, float metallic, float roughness)
{
    br_material3d_desc d = br_material3d_default(model);
    d.base_color = lin(srgb);
    d.metallic = metallic;
    d.roughness = roughness;
    d.specular = br_color_make(0.8f, 0.8f, 0.8f, 1.0f);
    return br_material3d_create(e, &d);
}

static br_node3d add_mesh(br_engine *e, demo_state *s, br_geometry3d g, br_material3d m, float x, float y, float z)
{
    br_node3d n = br_mesh3d_create(e, g, m);
    br_node3d_set_position(e, n, br_vec3_make(x, y, z));
    br_node3d_add_child(e, s->root, n);
    return n;
}

/* Pointer events on 3D nodes: clicking a front-row shape gives it a random colour. */
static void on_shape_click(br_engine *e, br_event3d *ev, void *user)
{
    demo_state *s = user;
    br_mesh3d *m = br_mesh3d_get(e, ev->target);
    if (m != NULL) {
        br_material3d_get(e, m->material)->base_color = br_color_make(frand(s), frand(s), frand(s), 1.0f);
        br_log_info("view3d_demo: clicked %s at (%.2f, %.2f, %.2f)", br_node3d_label(e, ev->target), ev->hit.point.x,
                    ev->hit.point.y, ev->hit.point.z);
    }
}

/* Skinned cylinder: two joints, the upper half bends around the middle. */
static br_node3d make_skinned_cylinder(br_engine *e, demo_state *s, br_material3d m)
{
    br_geometry3d tmp = br_geometry3d_cylinder(e, 0.35f, 0.35f, 3.0f, 16, 16);
    uint32_t count = 0;
    const br_vertex3d *src = br_geometry3d_vertices(e, tmp, &count);
    uint32_t index_count = 0;
    const uint32_t *indices = br_geometry3d_indices(e, tmp, &index_count);
    br_vertex3d *v = br_alloc(sizeof(br_vertex3d) * count);
    uint32_t *idx = br_alloc(sizeof(uint32_t) * index_count);
    SDL_memcpy(v, src, sizeof(br_vertex3d) * count);
    SDL_memcpy(idx, indices, sizeof(uint32_t) * index_count);
    for (uint32_t i = 0; i < count; i++) {
        float y = v[i].position.y; /* -1.5 .. 1.5 */
        float w1 = br_clampf((y + 0.4f) / 0.8f, 0.0f, 1.0f);
        v[i].joints[0] = 0;
        v[i].joints[1] = 1;
        v[i].weights = br_vec4_make(1.0f - w1, w1, 0.0f, 0.0f);
    }
    br_geometry3d_desc desc;
    SDL_zero(desc);
    desc.vertices = v;
    desc.vertex_count = count;
    desc.indices = idx;
    desc.index_count = index_count;
    br_geometry3d g = br_geometry3d_create(e, &desc);
    br_geometry3d_destroy(e, tmp);
    br_free(v);
    br_free(idx);
    br_node3d n = add_mesh(e, s, g, m, -8.5f, 1.5f, -3.0f);
    s->joints[0] = br_mat4_identity();
    s->joints[1] = br_mat4_identity();
    br_mesh3d *mesh = br_mesh3d_get(e, n);
    mesh->joint_matrices = s->joints; /* the skin slot: we write s->joints every frame */
    mesh->joint_count = 2;
    return n;
}

/* Morphing sphere: one target pushes the surface out into bumps. */
static br_node3d make_morph_sphere(br_engine *e, demo_state *s, br_material3d m)
{
    br_geometry3d tmp = br_geometry3d_sphere(e, 1.0f, 48, 24);
    uint32_t count = 0;
    const br_vertex3d *src = br_geometry3d_vertices(e, tmp, &count);
    uint32_t index_count = 0;
    const uint32_t *indices = br_geometry3d_indices(e, tmp, &index_count);
    br_vertex3d *target = br_alloc(sizeof(br_vertex3d) * count);
    br_vec3 *dpos = br_alloc(sizeof(br_vec3) * count);
    br_vec3 *dnrm = br_alloc(sizeof(br_vec3) * count);
    SDL_memcpy(target, src, sizeof(br_vertex3d) * count);
    for (uint32_t i = 0; i < count; i++) {
        float u = src[i].uv0.x * BR_TAU;
        float v = src[i].uv0.y * BR_PI;
        float bump = 0.35f * SDL_fabsf(SDL_sinf(u * 3.0f) * SDL_sinf(v * 3.0f));
        dpos[i] = br_vec3_scale(src[i].normal, bump);
        target[i].position = br_vec3_add(src[i].position, dpos[i]);
    }
    br_geometry3d_compute_normals(target, count, indices, index_count);
    for (uint32_t i = 0; i < count; i++) {
        dnrm[i] = br_vec3_sub(target[i].normal, src[i].normal);
    }
    br_geometry3d_desc desc;
    SDL_zero(desc);
    desc.vertices = src;
    desc.vertex_count = count;
    desc.indices = indices;
    desc.index_count = index_count;
    desc.morph_positions = dpos;
    desc.morph_normals = dnrm;
    desc.morph_target_count = 1;
    br_geometry3d g = br_geometry3d_create(e, &desc);
    br_geometry3d_destroy(e, tmp);
    br_free(target);
    br_free(dpos);
    br_free(dnrm);
    br_node3d n = add_mesh(e, s, g, m, 8.5f, 1.3f, -3.0f);
    br_mesh3d_get(e, n)->morph_weight_count = 1;
    return n;
}

/* ---- a custom 3D node type: axis lines submitted as a custom draw item */

typedef struct axes_vertex {
    float x, y, z;
    float r, g, b, a;
} axes_vertex;

static void axes_draw(br_engine *e, const br_draw3d_custom_info *info, void *user)
{
    demo_axes *axes = user;
    br_gpu *g = br_engine_gpu(e);
    br_pipeline_desc desc;
    SDL_zero(desc);
    desc.vertex_shader = &br_shader_lines3d_vert;
    desc.fragment_shader = &br_shader_lines3d_frag;
    desc.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
    desc.layout.buffer_count = 1;
    desc.layout.strides[0] = sizeof(axes_vertex);
    desc.layout.attribute_count = 2;
    desc.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
    desc.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12 };
    br_render3d_pipeline_setup(info, &desc);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    if (pipeline == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(info->pass, pipeline);
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), axes->vertex_offset };
    SDL_BindGPUVertexBuffers(info->pass, 0, &vb, 1);
    SDL_PushGPUVertexUniformData(info->cmd, 0, &info->view_projection, sizeof(br_mat4));
    SDL_DrawGPUPrimitives(info->pass, 6, 1, 0, 0);
}

static void axes_collect(br_engine *e, br_node3d n, const void *data, br_render3d_ctx *ctx)
{
    demo_axes *axes = *(demo_axes *const *)data; /* heap-stable: type data may move */
    br_mat4 w = br_render3d_world(ctx, n);
    br_vec3 o = br_mat4_transform_point(w, br_vec3_make(0.0f, 0.0f, 0.0f));
    axes_vertex *v = br_gpu_transient_alloc_vertices(br_engine_gpu(e), 6 * sizeof(axes_vertex), &axes->vertex_offset);
    if (v == NULL) {
        return;
    }
    for (int a = 0; a < 3; a++) {
        br_vec3 tip = br_mat4_transform_point(w, br_vec3_make(a == 0 ? 1.5f : 0.0f, a == 1 ? 1.5f : 0.0f, a == 2 ? 1.5f : 0.0f));
        float c[3] = { a == 0 ? 4.0f : 0.1f, a == 1 ? 4.0f : 0.1f, a == 2 ? 4.0f : 0.1f }; /* HDR: they glow */
        v[a * 2] = (axes_vertex){ o.x, o.y, o.z, c[0], c[1], c[2], 1.0f };
        v[a * 2 + 1] = (axes_vertex){ tip.x, tip.y, tip.z, c[0], c[1], c[2], 1.0f };
    }
    br_render3d_submit_custom(ctx, BR_PASS3D_OPAQUE, o, axes_draw, axes);
}

/* ---- setup */

static void setup_2d_panel(br_engine *e, demo_state *s)
{
    /* off-stage 2D content rendered into a texture every frame, then used by a 3D material */
    s->panel_texture = br_render_texture_create(e, 256.0f, 160.0f, 1.0f);
    s->panel_2d = br_node_create_container(e);
    br_graphics_context *bg = br_graphics_context_create(e);
    br_graphics_rect(bg, 0.0f, 0.0f, 256.0f, 160.0f);
    br_graphics_fill_color(bg, br_color_from_hex(0x1D2B53, 1.0f));
    br_graphics_fillet_rect(bg, 10.0f, 10.0f, 236.0f, 140.0f, 16.0f);
    br_graphics_fill_color(bg, br_color_from_hex(0x7E2553, 1.0f));
    br_node_add_child(e, s->panel_2d, br_graphics_create(e, bg));
    br_graphics_context *star = br_graphics_context_create(e);
    br_graphics_star(star, 0.0f, 0.0f, 5, 36.0f, 16.0f, 0.0f);
    br_graphics_fill_color(star, br_color_from_hex(0xFFEC27, 1.0f));
    s->panel_star = br_graphics_create(e, star);
    br_node_set_position(e, s->panel_star, br_vec2_make(200.0f, 80.0f));
    br_node_add_child(e, s->panel_2d, s->panel_star);
    br_text_style ts = br_text_style_default();
    ts.font_size = 30.0f;
    ts.fill = br_color_from_hex(0xFFF1E8, 1.0f);
    br_node text = br_text_create(e, "2D in 3D", &ts);
    br_node_set_position(e, text, br_vec2_make(24.0f, 60.0f));
    br_node_add_child(e, s->panel_2d, text);
}

static void setup_scene(br_engine *e, demo_state *s)
{
    /* --- snippet: docs/agents/three_d.md --- */
    s->view = br_view3d_create(e, 1280.0f, 720.0f);
    br_view3d_set_auto_resize(e, s->view, true);           /* follows the window */
    br_view3d_set_msaa(e, s->view, g_msaa > 1 ? (uint32_t)g_msaa : 1u);
    br_view3d_set_environment(e, s->view, lin(0x9DB8E8), lin(0x5A4A3A), 0.35f);
    s->root = br_view3d_root(e, s->view);
    s->camera = br_view3d_camera(e, s->view);
    br_camera3d_get(e, s->camera)->fov_y = 50.0f * BR_DEG_TO_RAD; /* vertical; horizontal follows aspect */

    br_material3d ground = make_material(e, BR_MATERIAL3D_PBR, 0x6B7078, 0.0f, 0.8f);
    br_node3d ground_node = add_mesh(e, s, br_geometry3d_plane(e, 80.0f, 80.0f, 1, 1), ground, 0.0f, 0.0f, 0.0f);
    br_mesh3d_get(e, ground_node)->cast_shadows = false; /* receives only: keeps the shadow depth range tight */

    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 1.6f;
    sun.color = lin(0xFFF0DC);
    sun.cast_shadows = true;
    sun.cascade_count = 3;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_add_child(e, s->root, sun_node);
    br_node3d_set_position(e, sun_node, br_vec3_make(-6.0f, 10.0f, 5.0f));
    br_node3d_look_at(e, sun_node, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    /* --- end snippet --- */

    br_light3d point = br_light3d_default(BR_LIGHT3D_POINT);
    point.color = lin(0xFF9A3C);
    point.intensity = 14.0f;
    point.range = 9.0f;
    br_node3d point_node = br_light3d_create(e, &point);
    br_node3d_set_position(e, point_node, br_vec3_make(-3.0f, 2.2f, 3.2f));
    br_node3d_add_child(e, s->root, point_node);
    br_material3d bulb = make_material(e, BR_MATERIAL3D_FLAT, 0xFFC080, 0.0f, 1.0f);
    br_material3d_get(e, bulb)->emissive = br_color_make(3.0f, 1.6f, 0.6f, 1.0f);
    br_node3d bulb_node = add_mesh(e, s, br_geometry3d_sphere(e, 0.12f, 12, 6), bulb, -3.0f, 2.2f, 3.2f);
    br_mesh3d_get(e, bulb_node)->cast_shadows = false;

    br_light3d spot = br_light3d_default(BR_LIGHT3D_SPOT);
    spot.color = lin(0x56E0FF);
    spot.intensity = 60.0f;
    spot.range = 20.0f;
    spot.inner_angle = 14.0f * BR_DEG_TO_RAD;
    spot.outer_angle = 22.0f * BR_DEG_TO_RAD;
    spot.cast_shadows = true;
    br_node3d spot_node = br_light3d_create(e, &spot);
    br_node3d_add_child(e, s->root, spot_node);
    br_node3d_set_position(e, spot_node, br_vec3_make(9.0f, 7.0f, 4.0f));
    br_node3d_look_at(e, spot_node, br_vec3_make(6.0f, 0.0f, 0.5f), br_vec3_make(0.0f, 1.0f, 0.0f));

    /* front row: every primitive */
    br_geometry3d prims[SHAPES] = {
        br_geometry3d_cube(e, br_vec3_make(1.6f, 1.6f, 1.6f)),
        br_geometry3d_sphere(e, 0.9f, 32, 16),
        br_geometry3d_cylinder(e, 0.7f, 0.7f, 1.8f, 32, 1),
        br_geometry3d_cone(e, 0.9f, 1.8f, 32),
        br_geometry3d_torus(e, 0.65f, 0.28f, 32, 16),
        br_geometry3d_capsule(e, 0.55f, 0.8f, 24, 12),
    };
    static const char *names[SHAPES] = { "cube", "sphere", "cylinder", "cone", "torus", "capsule" };
    static const uint32_t colors[SHAPES] = { 0xE63946, 0xF4A261, 0x2A9D8F, 0x8AB17D, 0xE9C46A, 0x9B5DE5 };
    for (int i = 0; i < SHAPES; i++) {
        s->shape_materials[i] = make_material(e, BR_MATERIAL3D_PBR, colors[i], 0.0f, 0.4f);
        s->shapes[i] = add_mesh(e, s, prims[i], s->shape_materials[i], -7.5f + 3.0f * (float)i, 0.95f, 1.0f);
        br_node3d_set_label(e, s->shapes[i], names[i]);
        br_node3d_on(e, s->shapes[i], BR_EVENT_CLICK, on_shape_click, s);
    }
    s->torus = s->shapes[4];
    br_node3d_set_euler(e, s->torus, br_vec3_make(1.2f, 0.0f, 0.0f));

    /* transparent glass box with an opaque ball inside */
    br_material3d_desc glass = br_material3d_default(BR_MATERIAL3D_PBR);
    glass.base_color = br_color_make(0.35f, 0.6f, 1.0f, 0.35f);
    glass.roughness = 0.1f;
    glass.alpha_mode = BR_ALPHA_BLEND;
    glass.double_sided = true;
    br_node3d glass_node = add_mesh(e, s, br_geometry3d_cube(e, br_vec3_make(1.8f, 1.8f, 1.8f)),
                                    br_material3d_create(e, &glass), 0.0f, 0.9f, 4.5f);
    br_mesh3d_get(e, glass_node)->cast_shadows = false;
    add_mesh(e, s, br_geometry3d_sphere(e, 0.45f, 24, 12), make_material(e, BR_MATERIAL3D_PBR, 0xFF006E, 0.0f, 0.3f),
             0.0f, 0.9f, 4.5f);

    /* middle row: the four material models side by side */
    br_geometry3d ball = br_geometry3d_sphere(e, 1.0f, 40, 20);
    br_material3d_model models[4] = { BR_MATERIAL3D_PBR, BR_MATERIAL3D_PHONG, BR_MATERIAL3D_FLAT, BR_MATERIAL3D_TOON };
    for (int i = 0; i < 4; i++) {
        br_material3d m = make_material(e, models[i], 0x4D96FF, 0.0f, 0.35f);
        add_mesh(e, s, ball, m, -4.5f + 3.0f * (float)i, 1.0f, -3.0f);
    }
    s->skinned = make_skinned_cylinder(e, s, make_material(e, BR_MATERIAL3D_PBR, 0x06D6A0, 0.0f, 0.5f));
    s->morphing = make_morph_sphere(e, s, make_material(e, BR_MATERIAL3D_PBR, 0xEF476F, 0.2f, 0.35f));

    /* back rows: textured PBR, metallic varies (low row) and roughness varies (high row) */
    br_texture checker = make_checker(e);
    br_texture bumps = make_bumps(e);
    br_geometry3d small_ball = br_geometry3d_sphere(e, 0.75f, 40, 20);
    for (int row = 0; row < 2; row++) {
        for (int i = 0; i < 6; i++) {
            br_material3d_desc d = br_material3d_default(BR_MATERIAL3D_PBR);
            float k = (float)i / 5.0f;
            d.metallic = row == 0 ? k : 1.0f;
            d.roughness = row == 0 ? 0.3f : br_lerpf(0.05f, 1.0f, k);
            d.textures[BR_MATERIAL3D_BASE_COLOR].texture = checker;
            d.textures[BR_MATERIAL3D_BASE_COLOR].scale = br_vec2_make(2.0f, 1.0f); /* KHR_texture_transform */
            d.textures[BR_MATERIAL3D_NORMAL].texture = bumps;
            d.textures[BR_MATERIAL3D_NORMAL].scale = br_vec2_make(2.0f, 1.0f);
            add_mesh(e, s, small_ball, br_material3d_create(e, &d), -6.25f + 2.5f * (float)i, 0.8f + 1.8f * (float)row, -7.5f);
        }
    }

    /* a panel whose material texture is a live 2D render texture */
    setup_2d_panel(e, s);
    br_material3d_desc pd = br_material3d_default(BR_MATERIAL3D_PBR);
    pd.roughness = 0.6f;
    pd.textures[BR_MATERIAL3D_BASE_COLOR].texture = s->panel_texture;
    pd.textures[BR_MATERIAL3D_EMISSIVE].texture = s->panel_texture;
    pd.emissive = br_color_make(0.4f, 0.4f, 0.4f, 1.0f);
    br_node3d panel = add_mesh(e, s, br_geometry3d_plane(e, 3.2f, 2.0f, 1, 1), br_material3d_create(e, &pd), 10.5f, 1.6f, -7.0f);
    br_node3d_set_euler(e, panel, br_vec3_make(BR_PI * 0.5f, -0.5f, 0.0f)); /* stand it up, turn it towards the camera */

    /* emissive ring: bright enough to bloom */
    br_material3d_desc rd = br_material3d_default(BR_MATERIAL3D_PBR);
    rd.base_color = br_color_make(0.1f, 0.1f, 0.1f, 1.0f);
    rd.emissive = lin(0xFF7B00);
    rd.emissive_strength = 8.0f;
    s->ring = add_mesh(e, s, br_geometry3d_torus(e, 1.3f, 0.12f, 48, 12), br_material3d_create(e, &rd), 0.0f, 5.2f, -9.0f);
    br_node3d_set_euler(e, s->ring, br_vec3_make(BR_PI * 0.5f, 0.0f, 0.0f));

    /* instanced field: one batched mesh, N cubes in a ring around the scene */
    br_material3d field_material = make_material(e, BR_MATERIAL3D_PBR, 0xFFFFFF, 0.0f, 0.6f);
    br_node3d field = br_batched_mesh3d_create(e, br_geometry3d_cube(e, br_vec3_make(0.3f, 0.3f, 0.3f)), field_material);
    br_node3d_add_child(e, s->root, field);
    float outer = 16.0f + SDL_sqrtf((float)g_count) * 0.15f;
    for (int i = 0; i < g_count; i++) {
        float a = frand(s) * BR_TAU;
        float r = 14.0f + (outer - 14.0f) * SDL_sqrtf(frand(s));
        float h = 0.15f + frand(s) * 0.8f;
        br_quat q = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), frand(s) * BR_TAU);
        br_mat4 m = br_mat4_from_trs(br_vec3_make(SDL_cosf(a) * r, h * 0.5f, SDL_sinf(a) * r), q,
                                     br_vec3_make(1.0f, h / 0.3f, 1.0f));
        br_color c = br_color_from_hsl(frand(s) * 360.0f, 0.6f, 0.55f, 1.0f);
        br_batched_mesh3d_add(e, field, m, br_color_to_linear(c));
    }

    /* custom node type drawing axis lines (custom draw item) */
    br_node3d_type_desc axes_desc;
    SDL_zero(axes_desc);
    axes_desc.name = "demo_axes";
    axes_desc.data_size = sizeof(demo_axes *);
    axes_desc.collect = axes_collect;
    s->axes_type = br_node3d_type_register(e, &axes_desc);
    br_node3d axes = br_node3d_create(e, s->axes_type);
    *(demo_axes **)br_node3d_data(e, axes, s->axes_type) = &s->axes;
    br_node3d_set_position(e, axes, br_vec3_make(-11.0f, 0.02f, 3.0f));
    br_node3d_add_child(e, s->root, axes);

    /* post: bloom is on by default; add a custom vignette pass */
    float vignette[4] = { 0.55f, 0.45f, 0.0f, 0.0f };
    br_view3d_add_post_pass(e, s->view, &br_shader_post3d_vignette_frag, vignette, sizeof(vignette));

    if (g_camera == 1) {
        s->orbit = br_camera_orbit_make(br_vec3_make(1.0f, 1.0f, -3.0f), 19.0f, -1.05f, 0.32f);
    } else {
        s->orbit = br_camera_orbit_make(br_vec3_make(0.0f, 1.2f, -2.5f), 20.0f, 0.0f, 0.42f);
    }
    orbit_view_camera(e, &s->orbit, s->camera, false);
}

static void setup_2d(br_engine *e, demo_state *s)
{
    br_node stage = br_stage_root(e);
    /* under the view: a sky gradient and big text, visible where the 3D background is empty */
    br_node sky = br_sprite_create(e, make_sky(e));
    br_sprite_set_size(e, sky, 1280.0f, 720.0f);
    br_node_add_child(e, stage, sky);
    br_text_style under = br_text_style_default();
    under.font_size = 120.0f;
    under.bold = true;
    under.fill = br_color_from_hex(0xFFFFFF, 0.35f);
    br_node under_text = br_text_create(e, "UNDER THE VIEW", &under);
    br_node_set_position(e, under_text, br_vec2_make(180.0f, 70.0f));
    br_node_add_child(e, stage, under_text);

    br_node_add_child(e, stage, s->view);
    if (g_filter) {
        /* the view is a 2D node: 2D filters apply to the whole 3D image */
        br_filter sepia = br_filter_color_matrix();
        br_color_matrix_sepia(sepia.matrix, false);
        br_filters_set(e, s->view, &sepia, 1);
    }

    /* over the view */
    br_text_style title = br_text_style_default();
    title.font_size = 28.0f;
    title.fill = br_color_from_hex(0xFFFFFF, 1.0f);
    title.drop_shadow = true;
    title.shadow_color = br_color_from_hex(0x000000, 0.8f);
    title.shadow_distance = 2.0f;
    br_node t = br_text_create(e, "Braty View3D: PBR, shadows, bloom, skinning, morphs, instancing", &title);
    br_node_set_position(e, t, br_vec2_make(16.0f, 12.0f));
    br_node_add_child(e, stage, t);
    title.font_size = 18.0f;
    s->stats_text = br_text_create(e, "", &title);
    br_node_set_position(e, s->stats_text, br_vec2_make(16.0f, 48.0f));
    br_node_add_child(e, stage, s->stats_text);
    title.font_size = 20.0f;
    title.fill = br_color_from_hex(0xFFE066, 1.0f);
    s->label = br_text_create(e, "torus (2D label)", &title);
    br_text_set_anchor(e, s->label, 0.5f, 1.0f);
    br_node_add_child(e, stage, s->label);
    br_graphics_context *badge = br_graphics_context_create(e);
    br_graphics_fillet_rect(badge, 0.0f, 0.0f, 120.0f, 36.0f, 10.0f);
    br_graphics_fill_color(badge, br_color_from_hex(0x111111, 0.75f));
    br_graphics_star(badge, 20.0f, 18.0f, 5, 12.0f, 5.0f, 0.0f);
    br_graphics_fill_color(badge, br_color_from_hex(0xFFD60A, 1.0f));
    br_node b = br_graphics_create(e, badge);
    br_node_set_position(e, b, br_vec2_make(16.0f, 660.0f));
    br_node_add_child(e, stage, b);
    br_text_style bs = br_text_style_default();
    bs.font_size = 16.0f;
    bs.fill = br_color_from_hex(0xFFFFFF, 1.0f);
    br_node bt = br_text_create(e, "2D over 3D", &bs);
    br_node_set_position(e, bt, br_vec2_make(38.0f, 8.0f));
    br_node_add_child(e, b, bt);
}

/* ---- per frame */

static void demo_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);

    orbit_view_camera(e, &s->orbit, s->camera, true);

    /* skin: joint 1 bends the upper half around the middle (mesh space, bind pose at origin) */
    float bend = SDL_sinf(t * 1.6f) * 0.9f + 0.4f;
    s->joints[1] = br_mat4_rotation(br_vec3_make(0.0f, 0.0f, 1.0f), bend);
    s->joints[0] = br_mat4_rotation(br_vec3_make(0.0f, 0.0f, 1.0f), bend * 0.15f);
    /* morph weight */
    br_mesh3d_get(e, s->morphing)->morph_weights[0] = 0.5f + 0.5f * SDL_sinf(t * 2.0f);
    br_node3d_set_euler(e, s->ring, br_vec3_make(BR_PI * 0.5f, 0.0f, t * 0.5f));

    /* 2D content of the render texture used by the panel material */
    br_node_set_rotation(e, s->panel_star, t * 1.5f);
    br_render_to_texture(e, s->panel_2d, s->panel_texture);

    if ((int)br_ticker_frame(e) == g_resize_at) {
        SDL_SetWindowSize(br_engine_window(e), 1000, 640);
    }

    /* 2D label following a 3D node */
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    br_vec2 p;
    br_vec3 top = br_vec3_add(br_node3d_world_position(e, s->torus), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_node_set_visible(e, s->label, br_view3d_world_to_screen(e, s->view, top, &p));
    br_node_set_position(e, s->label, p);

    br_view3d_stats st;
    br_view3d_get_stats(e, s->view, &st);
    char line[160];
    SDL_snprintf(line, sizeof(line), "%u opaque, %u transparent, %u shadow items, %u instances, %u draws, %u lights, %u shadow tiles",
                 st.items_opaque, st.items_transparent, st.items_shadow, st.instances, st.draw_calls, st.lights,
                 st.shadow_tiles);
    br_text_set_text(e, s->stats_text, line);

    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0 && br_ticker_frame(e) > 10) {
        s->frame_ns_sum += now - s->last_ns;
        s->frame_ns_count += 1;
    }
    s->last_ns = now;
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    double avg = s->frame_ns_count > 0 ? (double)s->frame_ns_sum / (double)s->frame_ns_count / 1e6 : 0.0;
    br_view3d_stats st;
    br_view3d_get_stats(e, s->view, &st);
    br_log_info("view3d_demo: %d instanced cubes, avg frame %.2f ms over %llu frames (%s), %u 3D draws",
                g_count, avg, (unsigned long long)s->frame_ns_count, g_no_vsync ? "vsync off" : "vsync on",
                st.draw_calls);
    br_node_destroy(e, s->panel_2d);
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    s->rng = 12345u;
    br_engine_on_shutdown(e, demo_shutdown, s);
    if (g_no_vsync) {
        br_app_set_vsync(e, BR_VSYNC_OFF);
    }
    setup_scene(e, s);
    setup_2d(e, s);
    br_system_add(e, "demo.update", BR_PHASE_UPDATE, 0, demo_update, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            g_camera = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            g_count = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            g_msaa = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--novsync") == 0) {
            g_no_vsync = true;
        } else if (SDL_strcmp(argv[i], "--resize-at") == 0 && i + 1 < argc) {
            g_resize_at = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--filter") == 0) {
            g_filter = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - view3d demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
