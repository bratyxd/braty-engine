/* br_mesh.h - meshes (PixiJS Mesh, MeshSimple, MeshRope, MeshPlane, PerspectiveMesh).
 *
 * One node type, "mesh": indexed triangles with positions, uvs, optional per-vertex colours,
 * a texture and optionally your own shaders. Plane, rope and perspective meshes are the same
 * type with geometry generated for you:
 *   - plane: a verts_x x verts_y grid over the texture size (move its vertices for effects);
 *   - rope: a strip along a list of points, rebuilt every frame from br_mesh_rope_points;
 *   - perspective: a plane whose 4 corners can be placed anywhere (projective mapping).
 *
 * UVs are 0..1 over the texture frame (atlas frames, rotated ones included, are mapped for
 * you). Colours are premultiplied RGBA8 like br_vertex2d.color (br_render_pack_color) and are
 * multiplied by the node's tint * alpha. Up to 65535 vertices go through the batcher; bigger
 * meshes and custom shaders are one br_render_push_custom draw each.
 *
 * Custom shaders get the batcher's contract: vertex inputs TEXCOORD0 float2 position (already in
 * the current space), TEXCOORD1 float2 uv, TEXCOORD2 colour (UBYTE4_NORM); vertex b0 = engine
 * transform, vertex b1 = your vertex uniforms, fragment b0 = your fragment uniforms, t0 = texture
 * (see engine/shaders/textured.vert.hlsl).
 */
#ifndef BR_MESH_H
#define BR_MESH_H

#include "braty/br_render.h"

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_mesh_register(br_engine *e);

/* Type id of meshes. */
uint32_t br_mesh_type(br_engine *e);

/* ---- plain meshes */

/* Creates a mesh (data copied). colors may be NULL (white); indices may be NULL (0,1,2,3,...). */
br_node br_mesh_create(br_engine *e, br_texture texture, const br_vec2 *positions, const br_vec2 *uvs,
                       const uint32_t *colors, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count);
/* PixiJS MeshSimple: flat float arrays (x,y pairs and u,v pairs). */
br_node br_mesh_simple_create(br_engine *e, br_texture texture, const float *vertices, const float *uvs,
                              uint32_t vertex_count, const uint32_t *indices, uint32_t index_count);
/* Replaces the geometry (copied; same rules as br_mesh_create). */
bool br_mesh_set_geometry(br_engine *e, br_node n, const br_vec2 *positions, const br_vec2 *uvs,
                          const uint32_t *colors, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count);
/* Positions to edit in place (read every frame; NULL if not a mesh). */
br_vec2 *br_mesh_positions(br_engine *e, br_node n, uint32_t *count);
/* UVs to edit in place. */
br_vec2 *br_mesh_uvs(br_engine *e, br_node n, uint32_t *count);
/* Changes the texture. */
void br_mesh_set_texture(br_engine *e, br_node n, br_texture texture);
/* The texture (zero handle if not a mesh). */
br_texture br_mesh_texture(br_engine *e, br_node n);
/* Draws with your shaders and uniforms (copied, at most 256 bytes each). NULL shaders = default. */
void br_mesh_set_shader(br_engine *e, br_node n, const br_shader_blob *vertex_shader,
                        const br_shader_blob *fragment_shader, const void *vertex_uniforms,
                        uint32_t vertex_uniform_size, const void *fragment_uniforms, uint32_t fragment_uniform_size);

/* ---- plane (PixiJS MeshPlane) and perspective mesh (PixiJS PerspectiveMesh) */

/* A verts_x x verts_y grid (>= 2 each) the size of the texture. */
br_node br_mesh_plane_create(br_engine *e, br_texture texture, uint32_t verts_x, uint32_t verts_y);
/* A plane whose corners start at the texture rect; move them with br_perspective_mesh_set_corners. */
br_node br_perspective_mesh_create(br_engine *e, br_texture texture, uint32_t verts_x, uint32_t verts_y);
/* Places the four corners (top-left, top-right, bottom-right, bottom-left) in local space. */
void br_perspective_mesh_set_corners(br_engine *e, br_node n, br_vec2 tl, br_vec2 tr, br_vec2 br, br_vec2 bl);

/* ---- rope (PixiJS MeshRope) */

/* A textured strip along `points` (copied, >= 2). Width = texture height. texture_scale 0
 * stretches the texture over the whole rope; > 0 repeats it every texture width * scale
 * (needs a whole texture with BR_ADDRESS_REPEAT). */
br_node br_mesh_rope_create(br_engine *e, br_texture texture, const br_vec2 *points, uint32_t count,
                            float texture_scale);
/* The rope points to move in place; the strip follows next frame. NULL if not a rope. */
br_vec2 *br_mesh_rope_points(br_engine *e, br_node n, uint32_t *count);
/* Strip width in local units (default: texture height). */
void br_mesh_rope_set_width(br_engine *e, br_node n, float width);

/* ---- geometry builders (pure; used by the node types and the tests) */

/* Grid of vx x vy vertices over width x height: positions/uvs need vx*vy entries, indices
 * (vx-1)*(vy-1)*6. Row-major from the top-left. */
void br_mesh_build_plane(float width, float height, uint32_t vx, uint32_t vy, br_vec2 *positions, br_vec2 *uvs,
                         uint32_t *indices);
/* Rope strip: positions/uvs need count*2 entries (top then bottom vertex per point), indices
 * (count-1)*6. u runs 0..1 (texture_scale 0) or by length / (texture_width * texture_scale). */
void br_mesh_build_rope(const br_vec2 *points, uint32_t count, float width, float texture_scale,
                        float texture_width, br_vec2 *positions, br_vec2 *uvs, uint32_t *indices);
/* Maps (u,v) in the unit square onto the quad tl,tr,br,bl with a projective (homography) map. */
br_vec2 br_mesh_perspective_point(const br_vec2 corners[4], float u, float v);

#endif
