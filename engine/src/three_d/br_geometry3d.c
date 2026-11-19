/* br_geometry3d: geometry resources (vertex/index/morph buffers + CPU copy), normals, tangents
 * and the built-in shapes. */
#include "three_d/br_three_d_internal.h"

br_vertex3d br_vertex3d_make(br_vec3 position, br_vec3 normal, br_vec2 uv)
{
    br_vertex3d v;
    SDL_zero(v);
    v.position = position;
    v.normal = normal;
    v.tangent = br_vec4_make(1.0f, 0.0f, 0.0f, 1.0f);
    v.uv0 = uv;
    v.uv1 = uv;
    v.color = br_vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    return v;
}

/* ---- resource */

br_geometry3d_data *br_geometry3d_data_get(br_engine *e, br_geometry3d g)
{
    if (e->three_d == NULL) {
        return NULL;
    }
    return br_pool_get(&e->three_d->geometries, g.index, g.generation);
}

static void br_geometry3d_release(br_engine *e, br_geometry3d_data *d)
{
    br_gpu *gpu = br_engine_gpu(e);
    if (gpu != NULL) {
        br_gpu_buffer_destroy(gpu, d->vertex_buffer);
        br_gpu_buffer_destroy(gpu, d->index_buffer);
        br_gpu_buffer_destroy(gpu, d->morph_buffer);
    }
    br_free(d->vertices);
    br_free(d->indices);
    SDL_zero(*d);
}

static bool br_geometry3d_upload(br_engine *e, br_geometry3d_data *d, const br_geometry3d_desc *desc)
{
    br_gpu *gpu = br_engine_gpu(e);
    if (gpu == NULL) {
        return true; /* headless: CPU copy only */
    }
    d->vertex_buffer = br_gpu_buffer_create(gpu, SDL_GPU_BUFFERUSAGE_VERTEX,
                                            d->vertex_count * (uint32_t)sizeof(br_vertex3d), d->vertices);
    d->index_buffer = br_gpu_buffer_create(gpu, SDL_GPU_BUFFERUSAGE_INDEX,
                                           d->index_count * (uint32_t)sizeof(uint32_t), d->indices);
    if (d->vertex_buffer == NULL || d->index_buffer == NULL) {
        return false;
    }
    if (d->morph_target_count == 0) {
        return true;
    }
    /* two float4 per target per vertex: position delta, normal delta */
    uint32_t count = d->morph_target_count * d->vertex_count;
    br_vec4 *packed = br_alloc(sizeof(br_vec4) * 2 * count);
    if (packed == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        br_vec3 p = desc->morph_positions[i];
        br_vec3 n = desc->morph_normals != NULL ? desc->morph_normals[i] : br_vec3_make(0.0f, 0.0f, 0.0f);
        packed[i * 2] = br_vec4_make(p.x, p.y, p.z, 0.0f);
        packed[i * 2 + 1] = br_vec4_make(n.x, n.y, n.z, 0.0f);
    }
    d->morph_buffer = br_gpu_buffer_create(gpu, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                           (uint32_t)(sizeof(br_vec4) * 2 * count), packed);
    br_free(packed);
    return d->morph_buffer != NULL;
}

br_geometry3d br_geometry3d_create(br_engine *e, const br_geometry3d_desc *desc)
{
    br_geometry3d none = { 0, 0 };
    br_three_d_state *s = e->three_d;
    if (s == NULL || desc == NULL || desc->vertices == NULL || desc->vertex_count == 0) {
        br_log_error("br_geometry3d_create: no vertices");
        return none;
    }
    if (desc->morph_target_count > 0 && desc->morph_positions == NULL) {
        br_log_error("br_geometry3d_create: morph targets without position deltas");
        return none;
    }
    uint32_t index_count = desc->indices != NULL ? desc->index_count : desc->vertex_count;
    for (uint32_t i = 0; desc->indices != NULL && i < index_count; i++) {
        if (desc->indices[i] >= desc->vertex_count) {
            br_log_error("br_geometry3d_create: index %u out of range (%u vertices)", desc->indices[i],
                         desc->vertex_count);
            return none;
        }
    }
    br_geometry3d g;
    br_geometry3d_data *d = br_pool_alloc(&s->geometries, &g.index, &g.generation);
    if (d == NULL) {
        return none;
    }
    SDL_zero(*d);
    d->vertex_count = desc->vertex_count;
    d->index_count = index_count;
    d->morph_target_count = desc->morph_target_count;
    d->vertices = br_alloc(sizeof(br_vertex3d) * desc->vertex_count);
    d->indices = br_alloc(sizeof(uint32_t) * (index_count > 0 ? index_count : 1));
    if (d->vertices == NULL || d->indices == NULL) {
        br_geometry3d_release(e, d);
        br_pool_release(&s->geometries, g.index, g.generation);
        return none;
    }
    SDL_memcpy(d->vertices, desc->vertices, sizeof(br_vertex3d) * desc->vertex_count);
    for (uint32_t i = 0; i < index_count; i++) {
        d->indices[i] = desc->indices != NULL ? desc->indices[i] : i;
    }
    d->bounds = br_aabb_empty();
    for (uint32_t i = 0; i < d->vertex_count; i++) {
        d->bounds = br_aabb_extend(d->bounds, d->vertices[i].position);
    }
    if (!br_geometry3d_upload(e, d, desc)) {
        br_log_error("br_geometry3d_create: GPU upload failed");
        br_geometry3d_release(e, d);
        br_pool_release(&s->geometries, g.index, g.generation);
        return none;
    }
    return g;
}

void br_geometry3d_destroy(br_engine *e, br_geometry3d g)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, g);
    if (d != NULL) {
        br_geometry3d_release(e, d);
        br_pool_release(&e->three_d->geometries, g.index, g.generation);
    }
}

void br_geometry3d_free_all(br_engine *e)
{
    br_pool *p = &e->three_d->geometries;
    for (uint32_t i = 0; i < br_pool_capacity(p); i++) {
        if (br_pool_slot_alive(p, i)) {
            br_geometry3d_release(e, br_pool_slot_data(p, i));
        }
    }
    br_pool_free(p);
}

bool br_geometry3d_valid(br_engine *e, br_geometry3d g)
{
    return br_geometry3d_data_get(e, g) != NULL;
}

br_aabb br_geometry3d_bounds(br_engine *e, br_geometry3d g)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, g);
    return d != NULL ? d->bounds : br_aabb_empty();
}

const br_vertex3d *br_geometry3d_vertices(br_engine *e, br_geometry3d g, uint32_t *count)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, g);
    if (count != NULL) {
        *count = d != NULL ? d->vertex_count : 0;
    }
    return d != NULL ? d->vertices : NULL;
}

const uint32_t *br_geometry3d_indices(br_engine *e, br_geometry3d g, uint32_t *count)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, g);
    if (count != NULL) {
        *count = d != NULL ? d->index_count : 0;
    }
    return d != NULL ? d->indices : NULL;
}

uint32_t br_geometry3d_morph_target_count(br_engine *e, br_geometry3d g)
{
    br_geometry3d_data *d = br_geometry3d_data_get(e, g);
    return d != NULL ? d->morph_target_count : 0;
}

/* ---- normals and tangents */

void br_geometry3d_compute_normals(br_vertex3d *vertices, uint32_t vertex_count, const uint32_t *indices,
                                   uint32_t index_count)
{
    for (uint32_t i = 0; i < vertex_count; i++) {
        vertices[i].normal = br_vec3_make(0.0f, 0.0f, 0.0f);
    }
    uint32_t count = indices != NULL ? index_count : vertex_count;
    for (uint32_t i = 0; i + 2 < count; i += 3) {
        uint32_t a = indices != NULL ? indices[i] : i;
        uint32_t b = indices != NULL ? indices[i + 1] : i + 1;
        uint32_t c = indices != NULL ? indices[i + 2] : i + 2;
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) {
            continue;
        }
        br_vec3 n = br_vec3_cross(br_vec3_sub(vertices[b].position, vertices[a].position),
                                  br_vec3_sub(vertices[c].position, vertices[a].position)); /* area weighted */
        vertices[a].normal = br_vec3_add(vertices[a].normal, n);
        vertices[b].normal = br_vec3_add(vertices[b].normal, n);
        vertices[c].normal = br_vec3_add(vertices[c].normal, n);
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        vertices[i].normal = br_vec3_normalize(vertices[i].normal);
    }
}

void br_geometry3d_compute_tangents(br_vertex3d *vertices, uint32_t vertex_count, const uint32_t *indices,
                                    uint32_t index_count)
{
    br_vec3 *tan = br_alloc(sizeof(br_vec3) * vertex_count * 2);
    if (tan == NULL) {
        return;
    }
    br_vec3 *bit = tan + vertex_count;
    uint32_t count = indices != NULL ? index_count : vertex_count;
    for (uint32_t i = 0; i + 2 < count; i += 3) {
        uint32_t ids[3];
        for (int k = 0; k < 3; k++) {
            ids[k] = indices != NULL ? indices[i + (uint32_t)k] : i + (uint32_t)k;
        }
        if (ids[0] >= vertex_count || ids[1] >= vertex_count || ids[2] >= vertex_count) {
            continue;
        }
        const br_vertex3d *v0 = &vertices[ids[0]];
        br_vec3 e1 = br_vec3_sub(vertices[ids[1]].position, v0->position);
        br_vec3 e2 = br_vec3_sub(vertices[ids[2]].position, v0->position);
        br_vec2 d1 = br_vec2_sub(vertices[ids[1]].uv0, v0->uv0);
        br_vec2 d2 = br_vec2_sub(vertices[ids[2]].uv0, v0->uv0);
        float det = d1.x * d2.y - d2.x * d1.y;
        if (SDL_fabsf(det) < 1e-12f) {
            continue;
        }
        float r = 1.0f / det;
        br_vec3 t = br_vec3_scale(br_vec3_sub(br_vec3_scale(e1, d2.y), br_vec3_scale(e2, d1.y)), r);
        br_vec3 b = br_vec3_scale(br_vec3_sub(br_vec3_scale(e2, d1.x), br_vec3_scale(e1, d2.x)), r);
        for (int k = 0; k < 3; k++) {
            tan[ids[k]] = br_vec3_add(tan[ids[k]], t);
            bit[ids[k]] = br_vec3_add(bit[ids[k]], b);
        }
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        br_vec3 n = vertices[i].normal;
        br_vec3 t = br_vec3_sub(tan[i], br_vec3_scale(n, br_vec3_dot(n, tan[i])));
        if (br_vec3_length(t) < 1e-8f) {
            /* no uv gradient: any vector perpendicular to n */
            t = br_vec3_cross(SDL_fabsf(n.y) < 0.99f ? br_vec3_make(0.0f, 1.0f, 0.0f) : br_vec3_make(1.0f, 0.0f, 0.0f), n);
        }
        t = br_vec3_normalize(t);
        /* glTF: bitangent = cross(n, t) * w points to the TOP of the image, i.e. towards -v */
        float w = br_vec3_dot(br_vec3_cross(n, t), bit[i]) > 0.0f ? -1.0f : 1.0f;
        vertices[i].tangent = br_vec4_make(t.x, t.y, t.z, w);
    }
    br_free(tan);
}

/* ---- shape building */

typedef struct br_shape_builder {
    br_array vertices; /* br_vertex3d */
    br_array indices;  /* uint32_t */
} br_shape_builder;

static void br_shape_begin(br_shape_builder *b)
{
    br_array_init(&b->vertices, sizeof(br_vertex3d));
    br_array_init(&b->indices, sizeof(uint32_t));
}

static void br_shape_vertex(br_shape_builder *b, br_vec3 p, br_vec3 n, float u, float v)
{
    br_vertex3d vertex = br_vertex3d_make(p, n, br_vec2_make(u, v));
    br_array_push(&b->vertices, &vertex);
}

static void br_shape_triangle(br_shape_builder *b, uint32_t a, uint32_t c, uint32_t d)
{
    br_array_push(&b->indices, &a);
    br_array_push(&b->indices, &c);
    br_array_push(&b->indices, &d);
}

/* Two triangles for a grid of rows x (columns + 1) vertices starting at `first`, where moving
 * to the next row and to the next column keeps counter-clockwise winding from outside. */
static void br_shape_grid(br_shape_builder *b, uint32_t first, uint32_t rows, uint32_t columns)
{
    for (uint32_t r = 0; r + 1 < rows; r++) {
        for (uint32_t c = 0; c < columns; c++) {
            uint32_t a = first + r * (columns + 1) + c;
            uint32_t below = a + columns + 1;
            br_shape_triangle(b, a, below, a + 1);
            br_shape_triangle(b, below, below + 1, a + 1);
        }
    }
}

static br_geometry3d br_shape_finish(br_engine *e, br_shape_builder *b)
{
    br_geometry3d_compute_tangents(b->vertices.data, b->vertices.count, b->indices.data, b->indices.count);
    br_geometry3d_desc desc;
    SDL_zero(desc);
    desc.vertices = b->vertices.data;
    desc.vertex_count = b->vertices.count;
    desc.indices = b->indices.data;
    desc.index_count = b->indices.count;
    br_geometry3d g = br_geometry3d_create(e, &desc);
    br_array_free(&b->vertices);
    br_array_free(&b->indices);
    return g;
}

br_geometry3d br_geometry3d_cube(br_engine *e, br_vec3 size)
{
    /* per face: outward normal, right and up as seen from outside (right x up = normal) */
    static const float faces[6][9] = {
        { 0, 0, 1, 1, 0, 0, 0, 1, 0 },   { 0, 0, -1, -1, 0, 0, 0, 1, 0 }, { 1, 0, 0, 0, 0, -1, 0, 1, 0 },
        { -1, 0, 0, 0, 0, 1, 0, 1, 0 }, { 0, 1, 0, 1, 0, 0, 0, 0, -1 },  { 0, -1, 0, 1, 0, 0, 0, 0, 1 },
    };
    static const float corners[4][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } }; /* TL TR BR BL */
    br_vec3 h = br_vec3_scale(size, 0.5f);
    br_shape_builder b;
    br_shape_begin(&b);
    for (uint32_t f = 0; f < 6; f++) {
        br_vec3 n = br_vec3_make(faces[f][0], faces[f][1], faces[f][2]);
        br_vec3 r = br_vec3_make(faces[f][3], faces[f][4], faces[f][5]);
        br_vec3 u = br_vec3_make(faces[f][6], faces[f][7], faces[f][8]);
        uint32_t first = b.vertices.count;
        for (int c = 0; c < 4; c++) {
            br_vec3 p = br_vec3_add(n, br_vec3_add(br_vec3_scale(r, corners[c][0]), br_vec3_scale(u, corners[c][1])));
            br_shape_vertex(&b, br_vec3_mul(p, h), n, corners[c][0] * 0.5f + 0.5f, 0.5f - corners[c][1] * 0.5f);
        }
        br_shape_triangle(&b, first, first + 3, first + 2);
        br_shape_triangle(&b, first, first + 2, first + 1);
    }
    return br_shape_finish(e, &b);
}

br_geometry3d br_geometry3d_sphere(br_engine *e, float radius, uint32_t segments, uint32_t rings)
{
    segments = segments < 3 ? 3 : segments;
    rings = rings < 2 ? 2 : rings;
    br_shape_builder b;
    br_shape_begin(&b);
    for (uint32_t r = 0; r <= rings; r++) {
        float v = (float)r / (float)rings;
        float phi = v * BR_PI;
        for (uint32_t s = 0; s <= segments; s++) {
            float u = (float)s / (float)segments;
            float theta = u * BR_TAU;
            br_vec3 n = br_vec3_make(SDL_sinf(phi) * SDL_sinf(theta), SDL_cosf(phi), SDL_sinf(phi) * SDL_cosf(theta));
            br_shape_vertex(&b, br_vec3_scale(n, radius), n, u, v);
        }
    }
    br_shape_grid(&b, 0, rings + 1, segments);
    return br_shape_finish(e, &b);
}

br_geometry3d br_geometry3d_plane(br_engine *e, float width, float depth, uint32_t sx, uint32_t sz)
{
    sx = sx < 1 ? 1 : sx;
    sz = sz < 1 ? 1 : sz;
    br_shape_builder b;
    br_shape_begin(&b);
    for (uint32_t j = 0; j <= sz; j++) {
        float v = (float)j / (float)sz;
        for (uint32_t i = 0; i <= sx; i++) {
            float u = (float)i / (float)sx;
            br_shape_vertex(&b, br_vec3_make((u - 0.5f) * width, 0.0f, (v - 0.5f) * depth), br_vec3_make(0.0f, 1.0f, 0.0f),
                            u, v);
        }
    }
    br_shape_grid(&b, 0, sz + 1, sx);
    return br_shape_finish(e, &b);
}

/* Disc cap at height y facing up (+Y) or down. */
static void br_shape_cap(br_shape_builder *b, float radius, float y, uint32_t segments, bool up)
{
    br_vec3 n = br_vec3_make(0.0f, up ? 1.0f : -1.0f, 0.0f);
    uint32_t center = b->vertices.count;
    br_shape_vertex(b, br_vec3_make(0.0f, y, 0.0f), n, 0.5f, 0.5f);
    for (uint32_t s = 0; s <= segments; s++) {
        float theta = (float)s / (float)segments * BR_TAU;
        float x = SDL_sinf(theta);
        float z = SDL_cosf(theta);
        br_shape_vertex(b, br_vec3_make(x * radius, y, z * radius), n, 0.5f + x * 0.5f, 0.5f + (up ? z : -z) * 0.5f);
    }
    for (uint32_t s = 0; s < segments; s++) {
        if (up) {
            br_shape_triangle(b, center, center + 1 + s, center + 2 + s);
        } else {
            br_shape_triangle(b, center, center + 2 + s, center + 1 + s);
        }
    }
}

br_geometry3d br_geometry3d_cylinder(br_engine *e, float radius_top, float radius_bottom, float height,
                                     uint32_t segments, uint32_t height_segments)
{
    segments = segments < 3 ? 3 : segments;
    height_segments = height_segments < 1 ? 1 : height_segments;
    br_shape_builder b;
    br_shape_begin(&b);
    float slope = (radius_bottom - radius_top) / height;
    for (uint32_t r = 0; r <= height_segments; r++) {
        float v = (float)r / (float)height_segments;
        float y = height * 0.5f - v * height;
        float radius = br_lerpf(radius_top, radius_bottom, v);
        for (uint32_t s = 0; s <= segments; s++) {
            float u = (float)s / (float)segments;
            float theta = u * BR_TAU;
            br_vec3 n = br_vec3_normalize(br_vec3_make(SDL_sinf(theta), slope, SDL_cosf(theta)));
            br_shape_vertex(&b, br_vec3_make(SDL_sinf(theta) * radius, y, SDL_cosf(theta) * radius), n, u, v);
        }
    }
    br_shape_grid(&b, 0, height_segments + 1, segments);
    if (radius_top > 0.0f) {
        br_shape_cap(&b, radius_top, height * 0.5f, segments, true);
    }
    if (radius_bottom > 0.0f) {
        br_shape_cap(&b, radius_bottom, -height * 0.5f, segments, false);
    }
    return br_shape_finish(e, &b);
}

br_geometry3d br_geometry3d_cone(br_engine *e, float radius, float height, uint32_t segments)
{
    return br_geometry3d_cylinder(e, 0.0f, radius, height, segments, 1);
}

br_geometry3d br_geometry3d_torus(br_engine *e, float radius, float tube, uint32_t segments, uint32_t tube_segments)
{
    segments = segments < 3 ? 3 : segments;
    tube_segments = tube_segments < 3 ? 3 : tube_segments;
    br_shape_builder b;
    br_shape_begin(&b);
    /* rows walk the tube angle, columns the ring angle: winding stays counter-clockwise outside */
    for (uint32_t j = 0; j <= tube_segments; j++) {
        float v = (float)j / (float)tube_segments;
        float phi = v * BR_TAU;
        for (uint32_t i = 0; i <= segments; i++) {
            float u = (float)i / (float)segments;
            float theta = u * BR_TAU;
            br_vec3 n = br_vec3_make(SDL_cosf(phi) * SDL_sinf(theta), SDL_sinf(phi), SDL_cosf(phi) * SDL_cosf(theta));
            float ring = radius + tube * SDL_cosf(phi);
            br_vec3 p = br_vec3_make(ring * SDL_sinf(theta), tube * SDL_sinf(phi), ring * SDL_cosf(theta));
            br_shape_vertex(&b, p, n, u, v);
        }
    }
    for (uint32_t j = 0; j < tube_segments; j++) {
        for (uint32_t i = 0; i < segments; i++) {
            uint32_t a = j * (segments + 1) + i;
            uint32_t up = a + segments + 1;
            br_shape_triangle(&b, a, a + 1, up);
            br_shape_triangle(&b, a + 1, up + 1, up);
        }
    }
    return br_shape_finish(e, &b);
}

br_geometry3d br_geometry3d_capsule(br_engine *e, float radius, float length, uint32_t segments, uint32_t rings)
{
    segments = segments < 3 ? 3 : segments;
    uint32_t half = rings < 4 ? 2 : rings / 2;
    float total = length + 2.0f * radius;
    br_shape_builder b;
    br_shape_begin(&b);
    /* top hemisphere rows (pole to equator), then bottom (equator to pole): the two equator
     * rows enclose the straight part */
    for (uint32_t r = 0; r <= 2 * half + 1; r++) {
        bool top = r <= half;
        float phi = top ? (float)r / (float)half * BR_PI * 0.5f : BR_PI * 0.5f + (float)(r - half - 1) / (float)half * BR_PI * 0.5f;
        float offset = top ? length * 0.5f : -length * 0.5f;
        for (uint32_t s = 0; s <= segments; s++) {
            float u = (float)s / (float)segments;
            float theta = u * BR_TAU;
            br_vec3 n = br_vec3_make(SDL_sinf(phi) * SDL_sinf(theta), SDL_cosf(phi), SDL_sinf(phi) * SDL_cosf(theta));
            br_vec3 p = br_vec3_add(br_vec3_scale(n, radius), br_vec3_make(0.0f, offset, 0.0f));
            br_shape_vertex(&b, p, n, u, (total * 0.5f - p.y) / total);
        }
    }
    br_shape_grid(&b, 0, 2 * half + 2, segments);
    return br_shape_finish(e, &b);
}
