# gltf - glTF models, skinning, morph targets, 3D animation

Headers: `engine/include/braty/br_gltf.h` (models, instances), `br_anim3d.h` (clips, sampling,
animator). Source: `engine/src/three_d/br_gltf*.c`, `br_anim3d.c`; scene JSON for views:
`engine/src/data/br_node_json_view3d.c`. Test: `tests/test_gltf.c` (+ `tests/data/tiny.gltf`,
regenerate with `python tests/data/make_tiny_gltf.py`). Example: `examples/gltf_demo.c`
(`--foxes N`, `--novsync`, `--focus 0..6`), models and their licences in `examples/models/`.

## The model

- **Parse once, instantiate many.** A `.gltf`/`.glb` loads (asset loader `"model"`) into a
  `br_model`: plain arrays of nodes (name, `extras` JSON text, parent, rest TRS, mesh / skin /
  camera / light index, rest morph weights), meshes -> primitives (a `br_geometry3d` + material
  index), materials, textures (one per image), skins (joint node indices + inverse binds),
  cameras, lights and animation clips. The model owns its GPU resources.
- **Instances** (`br_model_instantiate`) are a `"model3d"` 3D node holding one group node per
  glTF node (labelled with its name) and `mesh3d` / `camera3d` / `light3d` children for its
  content. Each instance keeps the model alive (`model->refs`), so `br_assets_unload` while
  instances live only drops the cache entry.
- **Clips** are flat data (`br_anim3d_clip`): channels with `times[]`, `values[]`, one
  interpolation (linear / step / cubic spline) and a target (model node index + path:
  translation, rotation, scale, weights). Phase 2's animation system reuses them as they are.
- **Animators** live in one dense pool, one per instance. The system `gltf.animate`
  (LATE_UPDATE) advances clocks, samples the clip (and the clip fading out), blends, writes node
  TRS and the mesh3d morph slot, then recomputes joint matrices into per-instance buffers that
  the mesh3d skin slots point at. No per-frame allocation.
- **Loading split:** `decode` (worker thread with `br_assets_load_async`) parses with cgltf,
  reads buffers (GLB chunk, base64 data URIs, external `.bin`) and images (bufferView, data URI,
  external file) and decodes them with stb_image, builds vertices/indices/morph deltas and the
  clips; `finish` (main thread) only creates textures (mip chain generated), materials and
  geometries.

## Functions you actually need

| Function | What for |
|---|---|
| `br_model_load(e, path)` / `br_assets_load_async(e, path, NULL, fn, user)` | Load (cached, ref counted). Async: `fn` gets the `br_model *`. |
| `br_model_instantiate(e, model)` + `br_node3d_add_child(e, root, n)` | Put a copy in a view. |
| `br_view3d_from_model(e, model, w, h)` | PixiJS `View3D.from`: view + instance + sun + framing camera. |
| `br_animator3d_play(e, n, "Run")`, `_play_index`, `_cross_fade(e, n, "Walk", 0.5f)`, `_stop` | Playback. |
| `br_animator3d_get(e, n)` | Edit `speed`, `loop`, `paused`, `time`, `on_event` (LOOP / END callback). |
| `br_model3d_node(e, n, i)`, `br_node3d_find_by_label(e, n, "Head")` | Reach a glTF node of an instance (attach props, IK, ...). |
| `br_model_find_clip`, `br_model_find_node`, `model->nodes[i].extras` | Inspect the parsed data (names, extras for gameplay). |
| `br_anim3d_sample`, `br_anim3d_joint_matrices`, `br_anim3d_decompose` | Pure maths (tests, tools, your own animation code). |
| `br_model_parse(e, path, bytes, size)` / `br_model_release` | Parse from memory without the cache. |

## Snippet (from examples/gltf_demo.c, `on_fox_loaded`)

```c
s->fox = br_model_instantiate(e, fox);                    /* a "model3d" node */
br_node3d_add_child(e, s->root, s->fox);
br_node3d_set_scale(e, s->fox, br_vec3_make(0.025f, 0.025f, 0.025f));
br_node3d_set_position(e, s->fox, br_vec3_make(g_row_x[6], 0.0f, 2.6f));
br_node3d_set_euler(e, s->fox, 0.0f, 0.9f, 0.0f);
br_animator3d_play(e, s->fox, "Survey");
/* later, every 3 s: br_animator3d_cross_fade(e, s->fox, "Walk", 2.0f); */
```

(`fox` is the `br_model *` handed to the `br_assets_load_async` completion callback.)

## Scene JSON (from examples/models/gltf_demo.scene.json)

A `view3d` node takes `size`, `auto_resize`, `msaa`, `background`, `environment`, `camera` and a
`scene` array of 3D nodes (`group`, `mesh` with a primitive `shape` + `material`, `light`,
`gltf`), each with `label`, `position`, `rotation` (Euler degrees), `scale`, `target`
(look-at), `visible`, `children`. Colours are sRGB as a colour picker shows them.

```json
{ "type": "view3d", "size": [420, 250], "position": [845, 455], "msaa": 4,
  "camera": { "position": [0, 2.2, 6.5], "target": [0, 0.8, 0], "fov": 40 },
  "scene": [
    { "type": "light", "kind": "directional", "intensity": 2.2, "shadows": true, "position": [-3, 6, 4], "target": [0, 0, 0] },
    { "type": "mesh", "shape": "plane", "size": [20, 20], "cast_shadows": false, "material": { "color": "#5c6470" } },
    { "type": "gltf", "source": "Fox/Fox.glb", "animation": "Run", "rotation": [0, 60, 0], "scale": 0.02 },
    { "type": "gltf", "source": "BoxAnimated/BoxAnimated.glb", "animation": 0 } ] }
```

`"animation"` is a clip name or an index (unnamed clips); `"speed"`, `"loop"`, `"time"` set the
animator.

## Gotchas

- Conventions match the 3D core and glTF: right-handed, y up, CCW front faces, uv (0,0) = top
  left of the image. Verified with OrientationTest, TextureCoordinateTest (text reads
  correctly), NormalTangentMirrorTest and TextureTransformTest on D3D12 and Vulkan.
- Compressed glTF (`KHR_draco_mesh_compression`, `EXT_meshopt_compression`,
  `KHR_texture_basisu`) fails with an error naming the extension. Unknown optional extensions
  load and are logged once per engine; unknown required ones fail.
- Base colour / emissive textures are decoded from sRGB in the shader; textures are
  premultiplied on upload (the 3D core undoes it for colour slots), so non-colour maps with
  alpha < 1 would lose RGB precision.
- Missing normals are generated smooth (glTF says flat); missing tangents use per-triangle
  accumulation, not MikkTSpace. Only triangle primitives (lists, strips, fans) are drawn.
- 8 morph targets per mesh (extra ones are dropped with a warning). One filter and one wrap mode
  per sampler (`br_texture_style`): glTF min filter and `wrapT` follow mag / `wrapS`.
- Light intensities from KHR_lights_punctual pass through unscaled (no lux/candela conversion);
  imported lights do not cast shadows until you set `cast_shadows`.
- A cross-fade blends nodes the new clip does not animate towards their rest pose.
- Skinned meshes are culled with the core's bind-pose box grown 2x.
- `br_animator3d_get` returns a pointer into the dense pool: do not keep it across frames.
- Scene-JSON meshes create their own geometry/material and scene loads keep their model asset
  references (freed at shutdown; Phase 2 asset groups).

## Numbers (RTX 3060 Ti, Release, 1280x720, MSAA 4, vsync off)

`gltf_demo --foxes 200 --novsync` (201 skinned Fox instances with 24 joints each, desynchronised
clips, plus the demo scene and inset view; 209 animators): 1.28 ms/frame on D3D12, 1.44 ms on
Vulkan; the `gltf.animate` system takes 0.46-0.49 ms of that. Without the extra foxes: 0.43 /
0.50 ms per frame.
