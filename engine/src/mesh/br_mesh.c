/* br_mesh: the mesh node type (Mesh, MeshSimple, MeshPlane, MeshRope, PerspectiveMesh). */
#include "app/br_engine_internal.h"
#include "braty/br_mesh.h"
#include "scene/br_scene_internal.h"

#include "textured.frag.h"
#include "textured.vert.h"

#define BR_MESH_MAX_UNIFORMS 256

typedef enum br_mesh_kind {
    BR_MESH_KIND_PLAIN,
    BR_MESH_KIND_ROPE,
    BR_MESH_KIND_PERSPECTIVE
} br_mesh_kind;

/* Per-node data. The arrays are owned (freed in on_destroy). */
typedef struct br_mesh_data {
    br_mesh_kind kind;
    br_texture   texture;
    br_array     positions;     /* br_vec2 */
    br_array     uvs;           /* br_vec2 */
    br_array     colors;        /* uint32_t, empty = white */
    br_array     indices;       /* uint32_t */
    /* rope */
    br_array     points;        /* br_vec2 */
    float        rope_width;
    float        texture_scale;
    /* custom shader */
    const br_shader_blob *vertex_shader;
    const br_shader_blob *fragment_shader;
    uint8_t      vertex_uniforms[BR_MESH_MAX_UNIFORMS];
    uint32_t     vertex_uniform_size;
    uint8_t      fragment_uniforms[BR_MESH_MAX_UNIFORMS];
    uint32_t     fragment_uniform_size;
} br_mesh_data;

typedef struct br_mesh_state {
    uint32_t type;
    br_array scratch_vertices;  /* br_vertex2d */
    br_array scratch_indices;   /* uint16_t */
} br_mesh_state;

/* ---- builders */

void br_mesh_build_plane(float width, float height, uint32_t vx, uint32_t vy, br_vec2 *positions, br_vec2 *uvs,
                         uint32_t *indices)
{
    for (uint32_t y = 0; y < vy; y++) {
        for (uint32_t x = 0; x < vx; x++) {
            float u = (float)x / (float)(vx - 1);
            float v = (float)y / (float)(vy - 1);
            positions[y * vx + x] = br_vec2_make(u * width, v * height);
            uvs[y * vx + x] = br_vec2_make(u, v);
        }
    }
    uint32_t k = 0;
    for (uint32_t y = 0; y + 1 < vy; y++) {
        for (uint32_t x = 0; x + 1 < vx; x++) {
            uint32_t tl = y * vx + x;
            uint32_t bl = tl + vx;
            indices[k++] = tl;
            indices[k++] = tl + 1;
            indices[k++] = bl + 1;
            indices[k++] = tl;
            indices[k++] = bl + 1;
            indices[k++] = bl;
        }
    }
}

void br_mesh_build_rope(const br_vec2 *points, uint32_t count, float width, float texture_scale,
                        float texture_width, br_vec2 *positions, br_vec2 *uvs, uint32_t *indices)
{
    float half = width * 0.5f;
    float length = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        /* direction through the point: neighbour to neighbour (one-sided at the ends) */
        br_vec2 prev = points[i > 0 ? i - 1 : i];
        br_vec2 next = points[i + 1 < count ? i + 1 : i];
        br_vec2 normal = br_vec2_normalize(br_vec2_perp(br_vec2_sub(next, prev)));
        if (i > 0) {
            length += br_vec2_distance(points[i - 1], points[i]);
        }
        float u;
        if (texture_scale > 0.0f && texture_width > 0.0f) {
            u = length / (texture_width * texture_scale);
        } else {
            u = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
        }
        positions[i * 2] = br_vec2_add(points[i], br_vec2_scale(normal, -half));
        positions[i * 2 + 1] = br_vec2_add(points[i], br_vec2_scale(normal, half));
        uvs[i * 2] = br_vec2_make(u, 0.0f);
        uvs[i * 2 + 1] = br_vec2_make(u, 1.0f);
    }
    for (uint32_t i = 0; i + 1 < count; i++) {
        uint32_t a = i * 2;
        indices[i * 6 + 0] = a;
        indices[i * 6 + 1] = a + 2;
        indices[i * 6 + 2] = a + 3;
        indices[i * 6 + 3] = a;
        indices[i * 6 + 4] = a + 3;
        indices[i * 6 + 5] = a + 1;
    }
}

br_vec2 br_mesh_perspective_point(const br_vec2 corners[4], float u, float v)
{
    /* Heckbert's square-to-quad homography */
    br_vec2 p0 = corners[0];
    br_vec2 p1 = corners[1];
    br_vec2 p2 = corners[2];
    br_vec2 p3 = corners[3];
    float dx1 = p1.x - p2.x;
    float dy1 = p1.y - p2.y;
    float dx2 = p3.x - p2.x;
    float dy2 = p3.y - p2.y;
    float dx3 = p0.x - p1.x + p2.x - p3.x;
    float dy3 = p0.y - p1.y + p2.y - p3.y;
    float det = dx1 * dy2 - dx2 * dy1;
    float g = 0.0f;
    float h = 0.0f;
    if (det != 0.0f) {
        g = (dx3 * dy2 - dx2 * dy3) / det;
        h = (dx1 * dy3 - dx3 * dy1) / det;
    }
    float a = p1.x - p0.x + g * p1.x;
    float b = p3.x - p0.x + h * p3.x;
    float d = p1.y - p0.y + g * p1.y;
    float f = p3.y - p0.y + h * p3.y;
    float w = g * u + h * v + 1.0f;
    if (w == 0.0f) {
        w = 1e-6f;
    }
    return br_vec2_make((a * u + b * v + p0.x) / w, (d * u + f * v + p0.y) / w);
}

/* ---- node type */

static br_mesh_data *br_mesh_get(br_engine *e, br_node n)
{
    return br_node_data(e, n, e->mesh->type);
}

static void br_mesh_on_create(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_mesh_data *m = data;
    br_array_init(&m->positions, sizeof(br_vec2));
    br_array_init(&m->uvs, sizeof(br_vec2));
    br_array_init(&m->colors, sizeof(uint32_t));
    br_array_init(&m->indices, sizeof(uint32_t));
    br_array_init(&m->points, sizeof(br_vec2));
}

static void br_mesh_on_destroy(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_mesh_data *m = data;
    br_array_free(&m->positions);
    br_array_free(&m->uvs);
    br_array_free(&m->colors);
    br_array_free(&m->indices);
    br_array_free(&m->points);
}

/* Multiplies two premultiplied RGBA8 colours channel by channel. */
static uint32_t br_mesh_mul_color(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        uint32_t ca = (a >> shift) & 0xFFu;
        uint32_t cb = (b >> shift) & 0xFFu;
        out |= ((ca * cb + 127u) / 255u) << shift;
    }
    return out;
}

static void br_mesh_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_mesh_data *m = data;
    uint32_t color = br_render_node_color(rc, n);
    uint32_t count = m->positions.count;
    if (color == 0 || count == 0 || m->indices.count < 3) {
        return;
    }
    br_mesh_state *s = e->mesh;
    if (!br_array_resize(&s->scratch_vertices, count)) {
        return;
    }
    const br_texture_info *info = br_render_texture_info(rc, m->texture);
    br_vec2 uv0 = info->uvs[0];
    br_vec2 ux = br_vec2_sub(info->uvs[1], info->uvs[0]);
    br_vec2 uy = br_vec2_sub(info->uvs[3], info->uvs[0]);
    br_mat3 xf = br_render_node_transform(rc, n);
    const br_vec2 *pos = m->positions.data;
    const br_vec2 *uvs = m->uvs.data;
    const uint32_t *colors = m->colors.count == count ? m->colors.data : NULL;
    br_vertex2d *v = s->scratch_vertices.data;
    for (uint32_t i = 0; i < count; i++) {
        br_vec2 p = br_mat3_apply(xf, pos[i]);
        /* 0..1 over the frame -> source uvs (also right for rotated atlas frames) */
        br_vec2 uv = br_vec2_add(uv0, br_vec2_add(br_vec2_scale(ux, uvs[i].x), br_vec2_scale(uy, uvs[i].y)));
        v[i].x = p.x;
        v[i].y = p.y;
        v[i].u = uv.x;
        v[i].v = uv.y;
        v[i].color = colors != NULL ? br_mesh_mul_color(color, colors[i]) : color;
    }

    bool custom = m->vertex_shader != NULL || m->fragment_shader != NULL;
    if (!custom && count <= BR_RENDER_MAX_BATCH_VERTICES) {
        if (!br_array_resize(&s->scratch_indices, m->indices.count)) {
            return;
        }
        uint16_t *idx = s->scratch_indices.data;
        const uint32_t *src = m->indices.data;
        for (uint32_t i = 0; i < m->indices.count; i++) {
            idx[i] = (uint16_t)src[i];
        }
        br_render_push_triangles(rc, m->texture, v, count, idx, m->indices.count);
        return;
    }
    br_custom_draw draw;
    SDL_zero(draw);
    draw.vertex_shader = m->vertex_shader != NULL ? m->vertex_shader : &br_shader_textured_vert;
    draw.fragment_shader = m->fragment_shader != NULL ? m->fragment_shader : &br_shader_textured_frag;
    draw.layout.buffer_count = 1;
    draw.layout.strides[0] = sizeof(br_vertex2d);
    draw.layout.attribute_count = 3;
    draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
    draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
    draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
    draw.vertices = v;
    draw.vertex_bytes = count * (uint32_t)sizeof(br_vertex2d);
    draw.indices = m->indices.data;
    draw.index_count = m->indices.count;
    draw.textures[0] = m->texture;
    draw.texture_count = 1;
    draw.vertex_uniforms = m->vertex_uniform_size > 0 ? m->vertex_uniforms : NULL;
    draw.vertex_uniform_size = m->vertex_uniform_size;
    draw.fragment_uniforms = m->fragment_uniform_size > 0 ? m->fragment_uniforms : NULL;
    draw.fragment_uniform_size = m->fragment_uniform_size;
    br_render_push_custom(rc, &draw);
}

static bool br_mesh_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_mesh_data *m = data;
    if (m->positions.count == 0) {
        return false;
    }
    const br_vec2 *p = m->positions.data;
    float x0 = p[0].x;
    float y0 = p[0].y;
    float x1 = x0;
    float y1 = y0;
    for (uint32_t i = 1; i < m->positions.count; i++) {
        x0 = SDL_min(x0, p[i].x);
        y0 = SDL_min(y0, p[i].y);
        x1 = SDL_max(x1, p[i].x);
        y1 = SDL_max(y1, p[i].y);
    }
    *out = br_rect_make(x0, y0, x1 - x0, y1 - y0);
    return true;
}

static bool br_mesh_hit_test(br_engine *e, br_node n, const void *data, br_vec2 point)
{
    (void)e;
    (void)n;
    const br_mesh_data *m = data;
    const br_vec2 *p = m->positions.data;
    const uint32_t *idx = m->indices.data;
    for (uint32_t i = 0; i + 2 < m->indices.count; i += 3) {
        if (idx[i] >= m->positions.count || idx[i + 1] >= m->positions.count || idx[i + 2] >= m->positions.count) {
            continue;
        }
        br_vec2 a = p[idx[i]];
        br_vec2 b = p[idx[i + 1]];
        br_vec2 c = p[idx[i + 2]];
        br_triangle t = { a.x, a.y, b.x, b.y, c.x, c.y };
        if (br_triangle_contains(t, point.x, point.y)) {
            return true;
        }
    }
    return false;
}

/* Rebuilds a rope strip from its points (PixiJS autoUpdate): ropes are cheap, points move. */
static void br_rope_rebuild(br_engine *e, br_mesh_data *m)
{
    uint32_t count = m->points.count;
    if (count < 2 || !br_array_resize(&m->positions, count * 2) || !br_array_resize(&m->uvs, count * 2) ||
        !br_array_resize(&m->indices, (count - 1) * 6)) {
        return;
    }
    br_texture_info info;
    br_texture_get_info(e, m->texture, &info);
    br_mesh_build_rope(m->points.data, count, m->rope_width, m->texture_scale, info.width, m->positions.data,
                       m->uvs.data, m->indices.data);
}

/* PRE_RENDER, before collect: one pass over the dense mesh pool, rebuilding ropes. */
static void br_mesh_update_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_node_type_entry *type = br_scene_type_entry(e->scene, e->mesh->type);
    if (type == NULL) {
        return;
    }
    for (uint32_t i = 0; i < type->data.count; i++) {
        br_mesh_data *m = br_array_at(&type->data, i);
        if (m->kind == BR_MESH_KIND_ROPE) {
            br_rope_rebuild(e, m);
        }
    }
}

static void br_mesh_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_array_free(&e->mesh->scratch_vertices);
    br_array_free(&e->mesh->scratch_indices);
    br_free(e->mesh);
    e->mesh = NULL;
}

void br_mesh_register(br_engine *e)
{
    e->mesh = br_alloc(sizeof(br_mesh_state));
    if (e->mesh == NULL) {
        return;
    }
    br_array_init(&e->mesh->scratch_vertices, sizeof(br_vertex2d));
    br_array_init(&e->mesh->scratch_indices, sizeof(uint16_t));
    br_engine_on_shutdown(e, br_mesh_shutdown, NULL);

    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "mesh";
    desc.data_size = sizeof(br_mesh_data);
    desc.on_create = br_mesh_on_create;
    desc.on_destroy = br_mesh_on_destroy;
    desc.local_bounds = br_mesh_bounds;
    desc.render = br_mesh_render;
    desc.hit_test = br_mesh_hit_test;
    e->mesh->type = br_node_type_register(e, &desc);
    br_system_add(e, "mesh.update", BR_PHASE_PRE_RENDER, 500, br_mesh_update_system, NULL);
}

uint32_t br_mesh_type(br_engine *e)
{
    return e->mesh->type;
}

/* ---- plain meshes */

bool br_mesh_set_geometry(br_engine *e, br_node n, const br_vec2 *positions, const br_vec2 *uvs,
                          const uint32_t *colors, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL || positions == NULL || uvs == NULL) {
        br_log_error("br_mesh_set_geometry: not a mesh, or positions/uvs missing");
        return false;
    }
    if (indices == NULL) {
        index_count = vertex_count;
    }
    for (uint32_t i = 0; indices != NULL && i < index_count; i++) {
        if (indices[i] >= vertex_count) {
            br_log_error("br_mesh_set_geometry: index %u out of range (%u vertices)", indices[i], vertex_count);
            return false;
        }
    }
    if (!br_array_resize(&m->positions, vertex_count) || !br_array_resize(&m->uvs, vertex_count) ||
        !br_array_resize(&m->colors, colors != NULL ? vertex_count : 0) || !br_array_resize(&m->indices, index_count)) {
        return false;
    }
    SDL_memcpy(m->positions.data, positions, sizeof(br_vec2) * vertex_count);
    SDL_memcpy(m->uvs.data, uvs, sizeof(br_vec2) * vertex_count);
    if (colors != NULL) {
        SDL_memcpy(m->colors.data, colors, sizeof(uint32_t) * vertex_count);
    }
    uint32_t *dst = m->indices.data;
    for (uint32_t i = 0; i < index_count; i++) {
        dst[i] = indices != NULL ? indices[i] : i;
    }
    return true;
}

br_node br_mesh_create(br_engine *e, br_texture texture, const br_vec2 *positions, const br_vec2 *uvs,
                       const uint32_t *colors, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count)
{
    br_node n = br_node_create(e, e->mesh->type);
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL) {
        return n;
    }
    m->texture = texture;
    if (!br_mesh_set_geometry(e, n, positions, uvs, colors, vertex_count, indices, index_count)) {
        br_node_destroy(e, n);
        br_node none = { 0, 0 };
        return none;
    }
    return n;
}

br_node br_mesh_simple_create(br_engine *e, br_texture texture, const float *vertices, const float *uvs,
                              uint32_t vertex_count, const uint32_t *indices, uint32_t index_count)
{
    br_vec2 *pos = br_alloc(sizeof(br_vec2) * vertex_count * 2);
    br_node none = { 0, 0 };
    if (pos == NULL || vertices == NULL || uvs == NULL) {
        br_free(pos);
        return none;
    }
    br_vec2 *uv = pos + vertex_count;
    for (uint32_t i = 0; i < vertex_count; i++) {
        pos[i] = br_vec2_make(vertices[i * 2], vertices[i * 2 + 1]);
        uv[i] = br_vec2_make(uvs[i * 2], uvs[i * 2 + 1]);
    }
    br_node n = br_mesh_create(e, texture, pos, uv, NULL, vertex_count, indices, index_count);
    br_free(pos);
    return n;
}

br_vec2 *br_mesh_positions(br_engine *e, br_node n, uint32_t *count)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (count != NULL) {
        *count = m != NULL ? m->positions.count : 0;
    }
    return m != NULL ? m->positions.data : NULL;
}

br_vec2 *br_mesh_uvs(br_engine *e, br_node n, uint32_t *count)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (count != NULL) {
        *count = m != NULL ? m->uvs.count : 0;
    }
    return m != NULL ? m->uvs.data : NULL;
}

void br_mesh_set_texture(br_engine *e, br_node n, br_texture texture)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (m != NULL) {
        m->texture = texture;
    }
}

br_texture br_mesh_texture(br_engine *e, br_node n)
{
    br_mesh_data *m = br_mesh_get(e, n);
    br_texture none = { 0, 0 };
    return m != NULL ? m->texture : none;
}

void br_mesh_set_shader(br_engine *e, br_node n, const br_shader_blob *vertex_shader,
                        const br_shader_blob *fragment_shader, const void *vertex_uniforms,
                        uint32_t vertex_uniform_size, const void *fragment_uniforms, uint32_t fragment_uniform_size)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL) {
        return;
    }
    if (vertex_uniform_size > BR_MESH_MAX_UNIFORMS || fragment_uniform_size > BR_MESH_MAX_UNIFORMS) {
        br_log_error("br_mesh_set_shader: uniforms larger than %d bytes", BR_MESH_MAX_UNIFORMS);
        return;
    }
    m->vertex_shader = vertex_shader;
    m->fragment_shader = fragment_shader;
    m->vertex_uniform_size = vertex_uniforms != NULL ? vertex_uniform_size : 0;
    m->fragment_uniform_size = fragment_uniforms != NULL ? fragment_uniform_size : 0;
    if (m->vertex_uniform_size > 0) {
        SDL_memcpy(m->vertex_uniforms, vertex_uniforms, m->vertex_uniform_size);
    }
    if (m->fragment_uniform_size > 0) {
        SDL_memcpy(m->fragment_uniforms, fragment_uniforms, m->fragment_uniform_size);
    }
}

/* ---- plane and perspective */

static br_node br_mesh_grid_create(br_engine *e, br_texture texture, uint32_t vx, uint32_t vy, br_mesh_kind kind)
{
    br_node none = { 0, 0 };
    if (vx < 2 || vy < 2 || vx > 4096 || vy > 4096) {
        br_log_error("br_mesh_plane_create: need 2..4096 vertices per side");
        return none;
    }
    br_node n = br_node_create(e, e->mesh->type);
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL) {
        return n;
    }
    br_texture_info info;
    br_texture_get_info(e, texture, &info);
    m->kind = kind;
    m->texture = texture;
    if (!br_array_resize(&m->positions, vx * vy) || !br_array_resize(&m->uvs, vx * vy) ||
        !br_array_resize(&m->indices, (vx - 1) * (vy - 1) * 6)) {
        br_node_destroy(e, n);
        return none;
    }
    br_mesh_build_plane(info.width, info.height, vx, vy, m->positions.data, m->uvs.data, m->indices.data);
    return n;
}

br_node br_mesh_plane_create(br_engine *e, br_texture texture, uint32_t verts_x, uint32_t verts_y)
{
    return br_mesh_grid_create(e, texture, verts_x, verts_y, BR_MESH_KIND_PLAIN);
}

br_node br_perspective_mesh_create(br_engine *e, br_texture texture, uint32_t verts_x, uint32_t verts_y)
{
    return br_mesh_grid_create(e, texture, verts_x, verts_y, BR_MESH_KIND_PERSPECTIVE);
}

void br_perspective_mesh_set_corners(br_engine *e, br_node n, br_vec2 tl, br_vec2 tr, br_vec2 br, br_vec2 bl)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL || m->kind != BR_MESH_KIND_PERSPECTIVE) {
        return;
    }
    br_vec2 corners[4] = { tl, tr, br, bl };
    br_vec2 *pos = m->positions.data;
    const br_vec2 *uvs = m->uvs.data;
    for (uint32_t i = 0; i < m->positions.count; i++) {
        pos[i] = br_mesh_perspective_point(corners, uvs[i].x, uvs[i].y);
    }
}

/* ---- rope */

br_node br_mesh_rope_create(br_engine *e, br_texture texture, const br_vec2 *points, uint32_t count,
                            float texture_scale)
{
    br_node none = { 0, 0 };
    if (points == NULL || count < 2) {
        br_log_error("br_mesh_rope_create: a rope needs at least 2 points");
        return none;
    }
    br_node n = br_node_create(e, e->mesh->type);
    br_mesh_data *m = br_mesh_get(e, n);
    if (m == NULL) {
        return n;
    }
    br_texture_info info;
    br_texture_get_info(e, texture, &info);
    m->kind = BR_MESH_KIND_ROPE;
    m->texture = texture;
    m->rope_width = info.height;
    m->texture_scale = texture_scale;
    if (br_array_push_n(&m->points, points, count) == NULL) {
        br_node_destroy(e, n);
        return none;
    }
    br_rope_rebuild(e, m); /* geometry right away: bounds and hit tests work before the first frame */
    return n;
}

br_vec2 *br_mesh_rope_points(br_engine *e, br_node n, uint32_t *count)
{
    br_mesh_data *m = br_mesh_get(e, n);
    bool rope = m != NULL && m->kind == BR_MESH_KIND_ROPE;
    if (count != NULL) {
        *count = rope ? m->points.count : 0;
    }
    return rope ? m->points.data : NULL;
}

void br_mesh_rope_set_width(br_engine *e, br_node n, float width)
{
    br_mesh_data *m = br_mesh_get(e, n);
    if (m != NULL && m->kind == BR_MESH_KIND_ROPE) {
        m->rope_width = width;
        br_rope_rebuild(e, m);
    }
}
