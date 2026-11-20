# mesh - meshes (Mesh, MeshSimple, MeshPlane, MeshRope, PerspectiveMesh)

Header: `engine/include/braty/br_mesh.h`. Source: `engine/src/mesh/br_mesh.c`.
Example: `examples/mesh_demo.c`. Test: `tests/test_sprites.c` (rope, plane, perspective).

## What it does

One `"mesh"` node type: indexed triangles (positions, uvs 0..1 over the texture frame, optional
premultiplied RGBA8 vertex colours), a texture, optionally your own shaders + uniforms. Plane,
rope and perspective meshes are the same type with generated geometry. Up to 65535 vertices
go through the batcher (batched with sprites); bigger meshes and custom shaders are one
`br_render_push_custom` draw each (32-bit indices). Ropes are rebuilt from their points every
frame by the `mesh.update` system (PRE_RENDER 500), which iterates the dense mesh pool.

## Functions you actually need

| Function | What for |
|---|---|
| `br_mesh_create(e, tex, pos, uvs, colors, n, indices, ni)` | Any mesh (colors/indices may be NULL). |
| `br_mesh_simple_create(e, tex, xy_floats, uv_floats, n, indices, ni)` | PixiJS MeshSimple. |
| `br_mesh_positions` / `br_mesh_uvs` | Edit vertices in place; drawn as they are next frame. |
| `br_mesh_plane_create(e, tex, vx, vy)` | Grid over the texture size. |
| `br_perspective_mesh_create` + `br_perspective_mesh_set_corners` | 4-corner projective card. |
| `br_mesh_rope_create(e, tex, points, n, texture_scale)` + `br_mesh_rope_points` | Rope; move points in place. |
| `br_mesh_set_shader(e, n, vs, fs, vu, vu_size, fu, fu_size)` | Custom shaders (NULL = default). |
| `br_mesh_build_plane/rope`, `br_mesh_perspective_point` | Pure geometry builders. |

## Snippet (from examples/mesh_demo.c)

```c
br_node plane = br_mesh_plane_create(e, s->checker, WAVE, WAVE);
uint32_t count = 0;
br_vec2 *p = br_mesh_positions(e, plane, &count); /* edit in place, every frame */
for (uint32_t i = 0; i < count; i++) {
    s->plane_rest[i] = p[i];
}
br_node_set_scale(e, plane, br_vec2_make(1.3f, 1.3f));
add(e, plane, 450, 30);
```

## Custom shaders

Vertex inputs `TEXCOORD0` float2 position (already in the current space), `TEXCOORD1` float2
uv, `TEXCOORD2` colour (UBYTE4_NORM); vertex b0 = engine transform, vertex b1 = your vertex
uniforms, fragment b0 = your fragment uniforms, t0 = the mesh texture. Start from
`engine/shaders/textured.vert.hlsl` / `textured.frag.hlsl` (passing NULL picks them).
mesh_demo draws a plane with `tiling.frag.hlsl` and a uniform block as an example.

## Gotchas

- UVs are mapped into the texture's frame (rotated atlas frames too). A rope with
  `texture_scale > 0` needs a whole texture with `BR_ADDRESS_REPEAT`.
- Rope width defaults to the texture height (`br_mesh_rope_set_width`).
- Position/uv pointers die when geometry is replaced or meshes are created/freed.
- Perspective meshes are a dense grid with affine uvs per triangle (as in PixiJS); use 10+
  vertices per side for a convincing card.
- Uniform blocks are copied, max 256 bytes each.
