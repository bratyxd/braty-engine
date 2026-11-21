/* br_occlusion3d.c - software occlusion culling for a View3D.
 *
 * Collect sorts the opaque items front to back. br_render3d_occlusion walks them in that order:
 * an item whose world box is hidden behind what was rasterised so far is dropped from the camera
 * passes; a kept item that makes a good occluder (opaque, one instance, not deformed) has its
 * triangles rasterised into a small CPU depth buffer. Transparent items are tested at the end.
 *
 * Only drawing is skipped: nodes, components, animation, physics and shadow casting of a hidden
 * item go on as before. Everything happens on the CPU during collect, with this frame's camera,
 * so there is no extra draw, no GPU readback and no frame of delay (nothing pops in late), and
 * it works headless.
 *
 * Conservative by construction (an item is only dropped when it cannot be seen):
 *   - occluder depth per pixel is pushed back by the triangle's depth slope over that pixel;
 *   - an occludee is tested with its nearest corner depth over its screen rectangle grown by
 *     one pixel, which covers the half pixel that pixel-centre sampling can add to an occluder;
 *   - a box that touches the near plane is never dropped.
 * ponytail: plain scalar rasteriser at 192 x 108 with a triangle budget per view; SIMD or a
 * hierarchical buffer if a profile shows `render3d.occlusion` matters. */
#include "three_d/br_three_d_internal.h"
#include "braty/br_profiler.h"

#define BR_OCC_W 192
#define BR_OCC_H 108
#define BR_OCC_NEAR_W 1e-4f /* clip-space w below this is behind (or on) the camera */

/* A vertex after projection: pixel x, y, depth 0..1, and clip w (<= BR_OCC_NEAR_W: not projected). */
typedef struct br_occ_vertex {
    float x, y, z, w;
} br_occ_vertex;

static br_occ_vertex br_occ_project(br_vec4 clip)
{
    br_occ_vertex v = { 0.0f, 0.0f, 0.0f, clip.w };
    if (clip.w > BR_OCC_NEAR_W) {
        float inv = 1.0f / clip.w;
        v.x = (clip.x * inv * 0.5f + 0.5f) * (float)BR_OCC_W;
        v.y = (0.5f - clip.y * inv * 0.5f) * (float)BR_OCC_H;
        v.z = clip.z * inv;
    }
    return v;
}

/* One projected triangle. Front faces are counter-clockwise with y up, so clockwise here. */
static void br_occ_triangle(float *depth, br_occ_vertex a, br_occ_vertex b, br_occ_vertex c, bool cull_back)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f || (cull_back && area > 0.0f)) {
        return;
    }
    float min_x = SDL_min(a.x, SDL_min(b.x, c.x));
    float max_x = SDL_max(a.x, SDL_max(b.x, c.x));
    float min_y = SDL_min(a.y, SDL_min(b.y, c.y));
    float max_y = SDL_max(a.y, SDL_max(b.y, c.y));
    /* pixels whose centre can be inside; written as !(>=) so NaN and huge values fall out */
    if (!(max_x >= 0.5f) || !(min_x <= (float)BR_OCC_W) || !(max_y >= 0.5f) || !(min_y <= (float)BR_OCC_H)) {
        return;
    }
    int x0 = (int)SDL_ceilf(SDL_max(min_x, 0.0f) - 0.5f);
    int x1 = (int)SDL_floorf(SDL_min(max_x, (float)BR_OCC_W) - 0.5f);
    int y0 = (int)SDL_ceilf(SDL_max(min_y, 0.0f) - 0.5f);
    int y1 = (int)SDL_floorf(SDL_min(max_y, (float)BR_OCC_H) - 0.5f);
    x0 = SDL_max(x0, 0);
    y0 = SDL_max(y0, 0);
    x1 = SDL_min(x1, BR_OCC_W - 1);
    y1 = SDL_min(y1, BR_OCC_H - 1);
    if (x0 > x1 || y0 > y1) {
        return; /* no pixel centre inside: smaller than a pixel */
    }
    float inv_area = 1.0f / area;
    /* depth is affine in screen space: z = a.z + dzdx * (x - a.x) + dzdy * (y - a.y) */
    float dzdx = ((b.z - a.z) * (c.y - a.y) - (c.z - a.z) * (b.y - a.y)) * inv_area;
    float dzdy = ((c.z - a.z) * (b.x - a.x) - (b.z - a.z) * (c.x - a.x)) * inv_area;
    /* the farthest depth inside the pixel, so the buffer never claims more than the surface hides */
    float bias = 0.5f * (SDL_fabsf(dzdx) + SDL_fabsf(dzdy)) + 1e-6f;
    float s = area > 0.0f ? 1.0f : -1.0f;
    /* edge functions, >= 0 inside; e(x + 1) = e(x) + ex, e(y + 1) = e(y) + ey */
    float e0x = -s * (b.y - a.y), e0y = s * (b.x - a.x);
    float e1x = -s * (c.y - b.y), e1y = s * (c.x - b.x);
    float e2x = -s * (a.y - c.y), e2y = s * (a.x - c.x);
    float px = (float)x0 + 0.5f;
    float py = (float)y0 + 0.5f;
    float r0 = e0x * (px - a.x) + e0y * (py - a.y);
    float r1 = e1x * (px - b.x) + e1y * (py - b.y);
    float r2 = e2x * (px - c.x) + e2y * (py - c.y);
    float rz = a.z + dzdx * (px - a.x) + dzdy * (py - a.y) + bias;
    for (int y = y0; y <= y1; y++) {
        float w0 = r0;
        float w1 = r1;
        float w2 = r2;
        float z = rz;
        float *row = depth + (size_t)y * BR_OCC_W;
        for (int x = x0; x <= x1; x++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f && z < row[x]) {
                row[x] = z;
            }
            w0 += e0x;
            w1 += e1x;
            w2 += e2x;
            z += dzdx;
        }
        r0 += e0y;
        r1 += e1y;
        r2 += e2y;
        rz += dzdy;
    }
}

static br_vec4 br_occ_clip(const br_mat4 *mvp, br_vec3 p)
{
    return br_mat4_transform_vec4(*mvp, br_vec4_make(p.x, p.y, p.z, 1.0f));
}

/* A triangle that crosses the camera plane: cut it at w = near (in clip space, where the cut is
 * a straight line) and rasterise what is in front. A big floor or wall under the camera is the
 * best occluder there is, so these are worth the extra work. */
static void br_occ_triangle_clipped(float *depth, const br_mat4 *mvp, br_vec3 pa, br_vec3 pb, br_vec3 pc, bool cull_back)
{
    br_vec4 in[3] = { br_occ_clip(mvp, pa), br_occ_clip(mvp, pb), br_occ_clip(mvp, pc) };
    br_vec4 out[4];
    int n = 0;
    const float near_w = BR_OCC_NEAR_W * 2.0f;
    for (int i = 0; i < 3; i++) {
        br_vec4 p = in[i];
        br_vec4 q = in[(i + 1) % 3];
        bool p_in = p.w >= near_w;
        bool q_in = q.w >= near_w;
        if (p_in) {
            out[n++] = p;
        }
        if (p_in != q_in) {
            float t = (near_w - p.w) / (q.w - p.w);
            out[n++] = br_vec4_make(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, near_w);
        }
    }
    for (int i = 1; i + 1 < n; i++) {
        br_occ_triangle(depth, br_occ_project(out[0]), br_occ_project(out[i]), br_occ_project(out[i + 1]), cull_back);
    }
}

/* Rasterises a mesh. `scratch` holds vertex_count projected vertices. */
static void br_occ_mesh(float *depth, br_occ_vertex *scratch, const br_mat4 *mvp, const br_geometry3d_data *geo, bool cull_back)
{
    for (uint32_t i = 0; i < geo->vertex_count; i++) {
        scratch[i] = br_occ_project(br_occ_clip(mvp, geo->vertices[i].position));
    }
    for (uint32_t i = 0; i + 2 < geo->index_count; i += 3) {
        uint32_t ia = geo->indices[i];
        uint32_t ib = geo->indices[i + 1];
        uint32_t ic = geo->indices[i + 2];
        if (ia >= geo->vertex_count || ib >= geo->vertex_count || ic >= geo->vertex_count) {
            continue;
        }
        br_occ_vertex a = scratch[ia];
        br_occ_vertex b = scratch[ib];
        br_occ_vertex c = scratch[ic];
        int in_front = (a.w > BR_OCC_NEAR_W) + (b.w > BR_OCC_NEAR_W) + (c.w > BR_OCC_NEAR_W);
        if (in_front == 3) {
            br_occ_triangle(depth, a, b, c, cull_back);
        } else if (in_front > 0) {
            br_occ_triangle_clipped(depth, mvp, geo->vertices[ia].position, geo->vertices[ib].position,
                                    geo->vertices[ic].position, cull_back);
        }
    }
}

/* True if nothing of the world box can be seen past what the buffer holds. */
static bool br_occ_box_hidden(const float *depth, const br_mat4 *view_projection, br_aabb box)
{
    float min_x = 1e30f;
    float min_y = 1e30f;
    float max_x = -1e30f;
    float max_y = -1e30f;
    float min_z = 1e30f;
    for (int i = 0; i < 8; i++) {
        br_vec3 p = br_vec3_make((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
        br_occ_vertex v = br_occ_project(br_occ_clip(view_projection, p));
        if (!(v.w > BR_OCC_NEAR_W)) {
            return false; /* reaches the camera plane: it may fill the screen */
        }
        min_x = SDL_min(min_x, v.x);
        max_x = SDL_max(max_x, v.x);
        min_y = SDL_min(min_y, v.y);
        max_y = SDL_max(max_y, v.y);
        min_z = SDL_min(min_z, v.z);
    }
    if (!(max_x >= 0.0f) || !(min_x <= (float)BR_OCC_W) || !(max_y >= 0.0f) || !(min_y <= (float)BR_OCC_H) || !(min_z >= 0.0f)) {
        return false; /* off screen or NaN: the frustum test decides, not this one */
    }
    int x0 = SDL_max((int)SDL_floorf(SDL_max(min_x, 0.0f)) - 1, 0);
    int y0 = SDL_max((int)SDL_floorf(SDL_max(min_y, 0.0f)) - 1, 0);
    int x1 = SDL_min((int)SDL_floorf(SDL_min(max_x, (float)BR_OCC_W)) + 1, BR_OCC_W - 1);
    int y1 = SDL_min((int)SDL_floorf(SDL_min(max_y, (float)BR_OCC_H)) + 1, BR_OCC_H - 1);
    for (int y = y0; y <= y1; y++) {
        const float *row = depth + (size_t)y * BR_OCC_W;
        for (int x = x0; x <= x1; x++) {
            if (!(row[x] < min_z)) {
                return false; /* this pixel shows something at or behind the box's nearest point */
            }
        }
    }
    return true;
}

void br_occlusion3d_free(br_engine *e)
{
    br_three_d_state *s = e->three_d;
    br_free(s->occlusion_depth);
    s->occlusion_depth = NULL;
    br_array_free(&s->occlusion_scratch);
}

void br_render3d_occlusion(br_engine *e, br_view3d_state *v)
{
    br_three_d_state *s = e->three_d;
    v->stats.items_occluded = 0;
    if (v->occlusion_triangles == 0 || v->opaque.count + v->transparent.count < BR_OCCLUSION3D_MIN_ITEMS) {
        return; /* off, or too few items for hiding one to pay for the buffer */
    }
    if (s->occlusion_depth == NULL) {
        s->occlusion_depth = br_alloc(sizeof(float) * BR_OCC_W * BR_OCC_H);
        br_array_init(&s->occlusion_scratch, sizeof(br_occ_vertex));
        if (s->occlusion_depth == NULL) {
            return;
        }
    }
    br_profile_begin(e, "render3d.occlusion");
    float *depth = s->occlusion_depth;
    for (uint32_t i = 0; i < BR_OCC_W * BR_OCC_H; i++) {
        depth[i] = 2.0f; /* nothing drawn: farther than any depth */
    }
    const br_item3d *items = v->items.data;
    uint32_t budget = v->occlusion_triangles;
    uint32_t occluded = 0;

    uint32_t *list = v->opaque.data;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < v->opaque.count; i++) {
        const br_item3d *item = &items[list[i]];
        if (item->custom_fn == NULL && br_occ_box_hidden(depth, &v->view_projection, item->bounds)) {
            occluded++;
            continue;
        }
        list[kept++] = list[i];
        if (item->custom_fn != NULL || item->instance_count != 1 || item->joint_count > 0 || item->morph_count > 0) {
            continue; /* no CPU copy of what will be drawn: not an occluder */
        }
        const br_geometry3d_data *geo = br_geometry3d_data_get(e, item->geometry);
        const br_material3d_desc *mat = br_material3d_get(e, item->material);
        if (geo == NULL || mat == NULL || geo->vertices == NULL || geo->indices == NULL ||
            mat->alpha_mode != BR_ALPHA_OPAQUE || mat->model == BR_MATERIAL3D_CUSTOM) {
            continue; /* cut-out or custom shading may let pixels through */
        }
        uint32_t triangles = geo->index_count / 3;
        if (triangles == 0 || triangles > budget || !br_array_resize(&s->occlusion_scratch, geo->vertex_count)) {
            continue;
        }
        budget -= triangles;
        br_mat4 mvp = br_mat4_mul(v->view_projection, item->model);
        br_occ_mesh(depth, s->occlusion_scratch.data, &mvp, geo, !mat->double_sided);
    }
    v->opaque.count = kept;

    list = v->transparent.data;
    kept = 0;
    for (uint32_t i = 0; i < v->transparent.count; i++) {
        const br_item3d *item = &items[list[i]];
        if (item->custom_fn == NULL && br_occ_box_hidden(depth, &v->view_projection, item->bounds)) {
            occluded++;
            continue;
        }
        list[kept++] = list[i];
    }
    v->transparent.count = kept;

    v->stats.items_occluded = occluded;
    br_profiler_count(e, BR_PROFILER_ITEMS_3D_OCCLUDED, occluded);
    br_profile_end(e);
}
