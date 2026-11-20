# three_d - 3D nodes, meshes, materials, lights, shadows, View3D, post, picking

Headers (read in this order): `br_view3d.h` (3D node table, 3D node types, draw submission,
View3D, picking, 3D events), `br_mesh3d.h`, `br_material3d.h`, `br_light3d.h`, `br_camera3d.h`,
`br_post3d.h`. Source: `engine/src/three_d/` (glTF/animation files there belong to Task 11).
Shaders: `engine/shaders/mesh3d*.hlsl(i)`, `shadow3d.frag.hlsl`, `post3d*.hlsl(i)`, `lines3d.*`.
Test: `tests/test_three_d.c`. Example: `examples/view3d_demo.c` (`--camera 0|1`, `--count N`,
`--msaa N`, `--novsync`, `--resize-at N`, `--filter`).

## The model

- **3D nodes** (`br_node3d { index, generation }`) live in ONE struct-of-arrays table per
  engine: parent / first child / siblings, label, position, rotation (quaternion), scale, cached
  world `br_mat4`, visible, pickable, layer bits. Stale handles are ignored. Destroy is deferred
  inside a frame (freed by `three_d.flush_destroyed`, INPUT -999000), immediate outside. An
  `on_destroy` or destroy listener may destroy other nodes: they join the running flush.
- **3D node types** (`br_node3d_type_desc`) mirror the 2D registry: name, `data_size` (dense
  pool), `on_create/on_destroy`, `local_bounds`, `collect` (submit draw items), `pick` (ray in
  local space), `from_json`. `mesh3d`, `batched_mesh3d`, `light3d`, `camera3d` (and the plain
  `group3d`, id 1) are registered through it like any other module would.
- **View3D** is a 2D node type (`"view3d"`). It owns a root 3D node and a camera; everything
  under the root is drawn. Its 2D render callback collects the scene, pushes ONE own-passes
  render callback (shadow pass, HDR main pass, bloom, custom post, tone map) and draws the 8-bit
  result as a quad, so 2D filters, masks, alpha and blend modes apply to the whole view
  (`--filter` in the demo puts a sepia filter on it). 2D nodes before it draw under it (its
  background is transparent by default), nodes after it draw over it.
- **Resources** are handles in generational pools: `br_geometry3d` (GPU buffers + CPU copy for
  bounds/picking), `br_material3d` (a plain `br_material3d_desc` you edit in place).

## Conventions

- Right-handed, y up; cameras and lights look down their local **-Z** (`br_node3d_look_at`).
- **Depth: standard Z.** Near plane = 0, far = 1, `LESS_OR_EQUAL`, cleared to 1 (what
  `br_gpu_render_target_begin` does). Verified on D3D12 and Vulkan: identical screenshots; the
  render-to-texture result is upright on both (the "2D in 3D" panel text and the scene/sky
  orientation in `view3d_demo`).
- **Cameras are Unity-style:** `fov_y` (vertical) or `ortho_size` (half the visible height) is
  fixed; the horizontal extent follows the view's aspect (`camera.aspect > 0` overrides).
- **Colours of materials and lights are linear.** Use `br_color_to_linear` for picker
  colours. Base-colour and emissive textures are sRGB: glTF images feeding them are
  `R8G8B8A8_UNORM_SRGB` textures (decoded by the sampler, filtered in linear), textures you set
  yourself are decoded in the shader (`docs/agents/gltf.md`, "Texture colour spaces"); the tone
  map writes sRGB.
- Double-sided materials light back faces with the whole tangent frame flipped, so a normal map
  reads the same from both sides (`examples/backface_normal_demo.c`, golden
  `backface_normal_demo`). HDR target: RGBA16F with depth (optional MSAA, resolved before post).
- Screen points for `br_view3d_pick` / `screen_to_ray` / `world_to_screen` are stage (global 2D)
  coordinates; the view node's 2D transform is applied.
- Camera hooks: a 3D camera (`br_camera_create_3d`) owns a View3D that is not on the stage and
  points it at your scene with `br_view3d_set_root(e, view, root)` / `br_view3d_set_camera`;
  `br_view3d_set_culling_mask` + `br_node3d_set_layer_bits` choose what it draws. Such views get no
  pointer events (`docs/agents/camera.md`).

## Functions you actually need

| Function | What for |
|---|---|
| `br_view3d_create(e, w, h)`, `br_view3d_root`, `br_view3d_camera`, `_set_auto_resize`, `_set_msaa` | A view in the 2D stage. |
| `br_view3d_set_environment`, `_set_background`, `br_view3d_post` / `_add_post_pass` | Ambient, background, bloom/tone map/custom passes. |
| `br_node3d_create(e, type)`, `_add_child`, `_set_position/rotation/euler/scale`, `_look_at`, `_destroy` | Scene graph. |
| `br_geometry3d_cube/sphere/plane/cylinder/cone/torus/capsule`, `br_geometry3d_create(desc)` | Geometry (full vertex layout, 32-bit indices, morph targets). |
| `br_material3d_default(model)` + `br_material3d_create`, `br_material3d_get` | PBR / Phong / Flat / Toon / custom materials, edit live. |
| `br_mesh3d_create(e, geo, mat)`, `br_mesh3d_get` (skin + morph slots) | A mesh node. |
| `br_batched_mesh3d_create`, `_add(e, n, matrix, color)` | Instancing: one draw for N copies. |
| `br_light3d_default(kind)` + `br_light3d_create` | Directional (cascaded shadows), point, spot (shadows). |
| `br_camera_create_3d` + the `camera.orbit` component (`br_camera_orbit_make`, `br_camera.h`) | Mouse orbit camera. |
| `br_view3d_pick`, `br_view3d_world_to_screen`, `br_node3d_on(e, n, BR_EVENT_CLICK, fn, user)` | Picking, 2D following 3D, 3D pointer events. |

## Snippet (from examples/view3d_demo.c, `setup_scene`)

```c
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
```

(`make_material` = `br_material3d_default` + base colour/metallic/roughness + `br_material3d_create`;
`add_mesh` = `br_mesh3d_create` + position + `br_node3d_add_child(root, ...)`.)

## How to add a 3D node type

Same shape as a 2D type. From your module's register function (or game init):

```c
br_node3d_type_desc axes_desc;
SDL_zero(axes_desc);
axes_desc.name = "demo_axes";
axes_desc.data_size = sizeof(demo_axes *);
axes_desc.collect = axes_collect;
s->axes_type = br_node3d_type_register(e, &axes_desc);
br_node3d axes = br_node3d_create(e, s->axes_type);
*(demo_axes **)br_node3d_data(e, axes, s->axes_type) = &s->axes;
```

- `collect(e, n, data, ctx)` runs once per view per frame for visible nodes whose `layer`
  matches the view's culling mask. Get the node's world matrix with `br_render3d_world(ctx, n)`.
- `local_bounds` feeds culling users; `pick(e, n, data, local_ray, hit)` gets the ray in the
  node's local space with an unnormalised direction (so `t` is the world `t`): only report a hit
  closer than `hit->distance`, fill point/normal in local space; the view converts to world.
- Type data lives in a dense pool and MOVES when nodes of the type die: draw callbacks must get
  a heap-stable `user` pointer (the demo stores a pointer in the node data).
- Phase 2's `canvas3d` is exactly this: a 3D type whose `collect` submits a custom draw that
  replays 2D batcher triangles with the canvas' world matrix.

## How to submit draw items (opaque / transparent / shadow)

Mesh draws: fill a `br_draw3d` (geometry, material, instances with world matrices + colour,
`world_bounds`, `cast_shadows`, optional joints / morph weights) and call
`br_render3d_submit(ctx, &draw)`. The material's `alpha_mode` picks OPAQUE (front to back, depth
write) or TRANSPARENT (`BR_ALPHA_BLEND`: back to front, premultiplied blend, no depth write);
`cast_shadows` adds the SHADOW pass. Items outside the camera frustum skip the main passes but
still cast shadows. Instances are copied into this frame's transient vertex buffer at submit.

Custom draws (own shaders, any geometry): allocate transient data during collect, then
`br_render3d_submit_custom(ctx, BR_PASS3D_OPAQUE | BR_PASS3D_SHADOW ..., sort_point, fn, user)`.
`fn` runs inside each requested pass with `br_draw3d_custom_info` (pass, view/projection of the
camera or of the shadow tile, target formats); build the pipeline with
`br_render3d_pipeline_setup(info, &desc)`. From the demo (axis lines, `lines3d.*.hlsl`):

```c
static void axes_draw(br_engine *e, const br_draw3d_custom_info *info, void *user)
{
    demo_axes *axes = user;
    ...
    desc.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
    br_render3d_pipeline_setup(info, &desc);
    SDL_GPUGraphicsPipeline *pipeline = br_gpu_pipeline_get(g, &desc);
    SDL_BindGPUGraphicsPipeline(info->pass, pipeline);
    SDL_GPUBufferBinding vb = { br_gpu_transient_vertex_buffer(g), axes->vertex_offset };
    SDL_BindGPUVertexBuffers(info->pass, 0, &vb, 1);
    SDL_PushGPUVertexUniformData(info->cmd, 0, &info->view_projection, sizeof(br_mat4));
    SDL_DrawGPUPrimitives(info->pass, 6, 1, 0, 0);
}
/* in collect: */
axes_vertex *v = br_gpu_transient_alloc_vertices(br_engine_gpu(e), 6 * sizeof(axes_vertex), &axes->vertex_offset);
...
br_render3d_submit_custom(ctx, BR_PASS3D_OPAQUE, o, axes_draw, axes);
```

In the SHADOW pass the target is the R32F shadow atlas: write `SV_Position.z` to colour 0 (see
`shadow3d.frag.hlsl`). The engine re-pushes its own uniforms after a custom draw.

## Data for loaders and animation (Task 11 / Phase 2)

- Geometry: `br_geometry3d_desc` takes `br_vertex3d` (position, normal, tangent with sign,
  uv0, uv1, colour, 4 x uint16 joints, weights), 32-bit indices, and morph targets as
  position/normal delta arrays (target-major). `br_geometry3d_compute_normals/tangents` fill
  missing attributes (glTF tangent convention: `cross(n, t) * w` points to the image top).
- Materials: every glTF metallic-roughness field, alpha modes + cutoff, double sided,
  `emissive_strength`, and per texture slot: texture, sampler (`has_sampler` + style), uv set,
  KHR_texture_transform offset/rotation/scale. `BR_MATERIAL3D_FLAT` = KHR_materials_unlit.
- Skin slot (`br_mesh3d.joint_matrices/joint_count`): a pointer to matrices the animation
  writes every frame (glTF joint matrix = inverse(mesh world) * joint world * inverse bind);
  the renderer copies them per frame into a storage buffer (one copy pass per view).
- Morph slot (`br_mesh3d.morph_weights[8]/morph_weight_count`); deltas live on the geometry.
  Skinned/morphed meshes use `mesh3d_deform.vert.hlsl` automatically.
- Animation targets are plain data: node TRS (`br_node3d_set_*`), material params
  (`br_material3d_get`), morph weights, light/camera data (`br_light3d_get`, `br_camera3d_get`).

## Limits (documented, `// ponytail:` where it matters)

- `BR_LIGHT3D_MAX` 16 lights per view (extra ignored); one directional light gets cascades
  (`BR_SHADOW3D_MAX_CASCADES` 4), `BR_SHADOW3D_MAX_SPOT` 4 spot shadows; no point shadows.
  Shadow atlas: 4 x 2 tiles of `br_view3d_set_shadow_resolution` (default 1024).
- 8 morph weights per mesh, `BR_POST3D_MAX_PASSES` 4, custom material uniforms 256 bytes.
- No environment maps / IBL: ambient is the sky/ground hemisphere (metals look dark-ish).
- `batched_mesh3d` is culled as one box (no per-instance culling); shadow casters are not culled
  against the light frustum.
- World matrices of a view's subtree are recomputed every frame (no dirty flags).

## Pointer events on 3D nodes

The view node has event mode STATIC and a `hit_test` that is true only where a pickable 3D node
with 3D listeners (on itself or an ancestor) is under the pointer, so empty areas pass events
to 2D nodes below. The view's own 2D listeners re-pick and call `br_node3d_on` listeners on the
hit node, bubbling up its 3D parents (`ev->propagation_stopped` stops). OVER/OUT are derived
from moves; CLICK/TAP need down and up on the same 3D node. No events-module file was changed.

## Numbers (RTX 3060 Ti, Release, 1280x720, MSAA 4, vsync off)

`view3d_demo` (3000 instanced cubes, 31 opaque + 1 transparent items, 3 cascades + 1 spot
shadow, bloom, vignette): about 0.7-0.8 ms/frame on both D3D12 and Vulkan. `--count 50000`
(50,000 instanced cubes with shadows): about 3.5-3.8 ms/frame. D3D12 runs occasionally showed
outliers (9-17 ms) from presentation, not from the renderer.

## Gotchas

- Instances of a batched mesh are relative to its node; call `br_batched_mesh3d_update` after
  editing them in place (bounds).
- The default camera sits under the view's root at (0, 2, 8) looking at the origin.
- Light `shadow_bias` / `shadow_normal_bias` are world units; the shader adds slope scaling.
  Large non-casting receivers (ground planes) should set `cast_shadows = false`: casters extend
  the cascades' depth range.
- `br_view3d_screen_to_ray` starts on the near plane, so pick distances are from there.
- A view drawn twice in a frame (e.g. inside a filter) renders the 3D scene once.
- Custom post shaders read and write linear HDR (`post3d_common.hlsli`); the tone map runs last.
