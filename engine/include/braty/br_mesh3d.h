/* br_mesh3d.h - 3D geometry, mesh nodes, batched (instanced) meshes, skinning and morph targets.
 *
 * Geometry is a handle to vertex + 32-bit index data in GPU buffers, with a CPU copy kept for
 * bounds and picking. Every vertex has the full layout (position, normal, tangent, uv0, uv1,
 * colour, joints, weights), so any loader can fill any attribute. Morph targets (position and
 * normal deltas) live with the geometry; their weights and the skin's joint matrices live on
 * the mesh node, written by animation code every frame and uploaded by the renderer.
 *
 * Skinning: joint_matrices[i] maps a bind-pose vertex in the mesh's local space to its posed
 * position in the same space (glTF: inverse(mesh world) * joint world * inverse bind matrix).
 * Weights should sum to 1. Picking skinned/morphed meshes tests the bind pose.
 */
#ifndef BR_MESH3D_H
#define BR_MESH3D_H

#include "braty/br_view3d.h"

/* Morph weights a mesh can apply at once (targets beyond this are ignored). */
#define BR_MESH3D_MAX_MORPH_WEIGHTS 8

/* One vertex (96 bytes; matches the vertex shader input). */
typedef struct br_vertex3d {
    br_vec3  position;
    br_vec3  normal;
    br_vec4  tangent;     /* xyz = tangent, w = bitangent sign (+1 / -1) */
    br_vec2  uv0;
    br_vec2  uv1;
    br_vec4  color;       /* linear rgba, multiplies the base colour (1,1,1,1 = none) */
    uint16_t joints[4];
    br_vec4  weights;
} br_vertex3d;

/* Everything needed to create a geometry. Morph arrays are target-major (target t, vertex i
 * at [t * vertex_count + i]); morph_normals may be NULL. */
typedef struct br_geometry3d_desc {
    const br_vertex3d *vertices;
    uint32_t           vertex_count;
    const uint32_t    *indices;            /* NULL = 0, 1, 2, ... */
    uint32_t           index_count;
    const br_vec3     *morph_positions;
    const br_vec3     *morph_normals;
    uint32_t           morph_target_count;
} br_geometry3d_desc;

/* ---- geometry */

/* Creates a geometry (data copied; GPU buffers when a GPU exists). Zero handle on failure. */
br_geometry3d br_geometry3d_create(br_engine *e, const br_geometry3d_desc *desc);
/* Destroys a geometry (meshes still using it draw nothing). */
void br_geometry3d_destroy(br_engine *e, br_geometry3d g);
/* True if the handle is live. */
bool br_geometry3d_valid(br_engine *e, br_geometry3d g);
/* Local bounds of the vertices. */
br_aabb br_geometry3d_bounds(br_engine *e, br_geometry3d g);
/* The CPU copy of the vertices (count may be NULL). NULL if stale. */
const br_vertex3d *br_geometry3d_vertices(br_engine *e, br_geometry3d g, uint32_t *count);
/* The CPU copy of the indices (count may be NULL). NULL if stale. */
const uint32_t *br_geometry3d_indices(br_engine *e, br_geometry3d g, uint32_t *count);
/* Number of morph targets. */
uint32_t br_geometry3d_morph_target_count(br_engine *e, br_geometry3d g);
/* Fills vertex tangents from positions, normals and uv0 (call before create). */
void br_geometry3d_compute_tangents(br_vertex3d *vertices, uint32_t vertex_count, const uint32_t *indices,
                                    uint32_t index_count);
/* Fills smooth vertex normals from triangle faces (call before create). */
void br_geometry3d_compute_normals(br_vertex3d *vertices, uint32_t vertex_count, const uint32_t *indices,
                                   uint32_t index_count);
/* A vertex with position, normal, uv0, white colour, tangent +X and no skin. */
br_vertex3d br_vertex3d_make(br_vec3 position, br_vec3 normal, br_vec2 uv);

/* ---- built-in shapes (centred on the origin, y up, tangents filled) */

/* Box with size x size x size... per axis. */
br_geometry3d br_geometry3d_cube(br_engine *e, br_vec3 size);
/* UV sphere. */
br_geometry3d br_geometry3d_sphere(br_engine *e, float radius, uint32_t segments, uint32_t rings);
/* Flat plane in XZ facing +Y, divided into sx x sz cells. */
br_geometry3d br_geometry3d_plane(br_engine *e, float width, float depth, uint32_t sx, uint32_t sz);
/* Cylinder along Y with caps (radius_top 0 = cone). height_segments rings along the side. */
br_geometry3d br_geometry3d_cylinder(br_engine *e, float radius_top, float radius_bottom, float height,
                                     uint32_t segments, uint32_t height_segments);
/* Cone along Y, apex up. */
br_geometry3d br_geometry3d_cone(br_engine *e, float radius, float height, uint32_t segments);
/* Torus in XZ: ring radius and tube radius. */
br_geometry3d br_geometry3d_torus(br_engine *e, float radius, float tube, uint32_t segments, uint32_t tube_segments);
/* Capsule along Y: radius and the length of the straight middle part. */
br_geometry3d br_geometry3d_capsule(br_engine *e, float radius, float length, uint32_t segments, uint32_t rings);

/* ---- mesh3d node */

/* Data of a "mesh3d" node: edit fields directly through br_mesh3d_get. */
typedef struct br_mesh3d {
    br_geometry3d  geometry;
    br_material3d  material;
    br_color       color;                 /* linear multiplier of the base colour (default white) */
    bool           cast_shadows;          /* default true */
    /* skin slot: written by animation code, uploaded every frame */
    const br_mat4 *joint_matrices;        /* NULL = not skinned; must stay valid while set */
    uint32_t       joint_count;
    /* morph slot: weights of the geometry's targets */
    float          morph_weights[BR_MESH3D_MAX_MORPH_WEIGHTS];
    uint32_t       morph_weight_count;
} br_mesh3d;

/* Type id of "mesh3d". */
uint32_t br_mesh3d_type(br_engine *e);
/* Creates a detached mesh node. */
br_node3d br_mesh3d_create(br_engine *e, br_geometry3d geometry, br_material3d material);
/* The mesh data, NULL if n is not a live mesh3d. */
br_mesh3d *br_mesh3d_get(br_engine *e, br_node3d n);

/* ---- batched_mesh3d node: one geometry + material, N instances, one instanced draw */

/* Type id of "batched_mesh3d". */
uint32_t br_batched_mesh3d_type(br_engine *e);
/* Creates a detached batched mesh node. Instance matrices are relative to the node. */
br_node3d br_batched_mesh3d_create(br_engine *e, br_geometry3d geometry, br_material3d material);
/* Appends an instance; returns its index (UINT32_MAX on failure). */
uint32_t br_batched_mesh3d_add(br_engine *e, br_node3d n, br_mat4 transform, br_color color);
/* The instances (editable in place; call br_batched_mesh3d_update after moving them). */
br_instance3d *br_batched_mesh3d_instances(br_engine *e, br_node3d n, uint32_t *count);
/* Recomputes the local bounds after instances were edited. */
void br_batched_mesh3d_update(br_engine *e, br_node3d n);
/* Removes every instance. */
void br_batched_mesh3d_clear(br_engine *e, br_node3d n);
/* Enables/disables shadow casting (default true). */
void br_batched_mesh3d_set_cast_shadows(br_engine *e, br_node3d n, bool cast);

#endif
