# gltf - glTF models, skinning, morph targets, 3D animation, glTF as scene content

glTF is the engine's main 3D format: a `.glb` exported from Blender is a scene, a prefab, a node
in a scene file, or a kit of parts, with gameplay data in node extras - no editor, no hand-written
scene JSON.

Headers: `engine/include/braty/br_gltf.h` (models, instances, options, 3D components),
`br_anim3d.h` (clips, sampling, animator). Source: `engine/src/three_d/br_gltf*.c` (loader,
instances, batching, 3D components), `br_anim3d.c`; data side: `engine/src/data/br_gltf_extras.c`
(extras -> components, conventions), `br_gltf_scene.c` (a model path as a scene / prefab),
`br_node_json_view3d.c` (view3d JSON). Tests: `tests/test_gltf.c` (+ `tests/data/tiny.gltf`,
`python tests/data/make_tiny_gltf.py`), `tests/test_gltf_scene.c` (+ `tests/data/level.gltf`,
`python tests/data/make_level_gltf.py`, which also writes the demo's `examples/gltf_scene/*.glb`).
Examples: `examples/gltf_demo.c` (`--foxes N`, `--novsync`, `--focus 0..6`; models and licences in
`examples/models/`), `examples/gltf_scene_demo.c` (`--no-batch`, `--top`; golden
`tests/golden/gltf_scene_demo.png`).

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
- **Untrusted files:** right after parsing, one pass (`br_gltf_check_uris`,
  `br_gltf_check_data` in `br_gltf.c`) rejects the load, naming the file and field, when a
  buffer or image URI leaves the model's folder (absolute, drive, scheme or a `..` segment), a
  data URI holds fewer bytes than `byteLength`, a bufferView lies outside its buffer, an
  accessor (or its sparse part) reads past its bufferView or has no data, an accessor count is
  over 2^24 (2^20 without a bufferView), a scene lists a node twice, inverse binds are not float
  MAT4, or a node has over 2^20 GPU instances. cgltf then rejects cycles, multi-parent nodes and
  out-of-range indices; `tests/test_hostile_gltf.c` pins all of it.

## glTF as scene content

| Use | How |
|---|---|
| A level | `br_scene_load(e, "levels/island.glb", BR_SCENE_SINGLE)`: a full-window View3D with the file's nodes, cameras, lights; the first camera (or `br_gltf_set_load_options` `.camera`) is the view's camera; a sun is added if the file has no lights. The model is in the scene's asset group and released on unload. |
| A 2D prefab | `br_prefab_instantiate(e, "props/tree.glb", parent, overrides)`: a 640 x 360 `view3d` holding the model, framed, with a sun if the file has none (overrides apply to the view: `"size"`, `"position"`, ...). |
| A 3D child | `br_gltf_instantiate(e, "models/kit.glb#Tree", parent3d, &options)`: the instance owns its asset reference (inside a scene load, the scene does). |
| A scene-file node | `{ "type": "gltf", "source": "models/kit.glb#Tree", "camera": "TopCam", "default_light": true, "batch": false, "autoplay": "Spin", "components": { "spin": { "speed": 2 } } }` inside a `view3d` `scene` array. |
| Any asset call | `"kit.glb#Tree"` resolves to `kit.glb` (`br_assets_load`, groups, ref counts): the kit is parsed once and shared. |

- **Sub-assets** (`#Name`): one glTF node and its subtree; its translation is reset to 0 so the
  part lands where you put the instance (rotation and scale are kept). The scene extras'
  autoplay does not run on a part. A missing name logs an error and returns a zero handle.
- **Batching:** in each instance, static nodes (no animated ancestor, no skin, morph targets,
  extras or convention suffix) that share a mesh are drawn as one `batched_mesh3d` per primitive
  (labelled `batch:<mesh name>`, child of the instance root). Their group nodes stay (labels,
  positions) but moving them moves nothing: give such a node `"batch": false` (or any extras).
  Opt out per file (scene extras `"batch": false`), per load (`br_gltf_options.no_batching`,
  `br_gltf_set_load_options`) or per JSON node (`"batch": false`).
- **EXT_mesh_gpu_instancing** nodes become one `batched_mesh3d` per primitive under the node.
- **Scene extras** (Blender: Scene Properties > Custom Properties): `autoplay` (clip name, or
  true = the first clip), `batch`, and for `br_scene_load`: `background`, `msaa`, `sky`,
  `ground`, `ambient` (the view's environment).

## Extras -> components

Register the component (a struct + `br_field_desc` list, `br_component_register`) before loading.
A glTF node's extras then fill it on that node's group node, in either form:

```json
"extras": { "components": { "health": { "max": 5 }, "spin": { "speed": 3 } } }
"extras": { "health.max": 5, "spin.speed": 1.5, "spawn_point": true }
```

Flat keys are what Blender custom properties export: `"comp.field": value` (the last dot splits,
so dotted component names work) or `"comp": true` / `{ fields }`. Flat keys naming no registered
component are someone else's data and are skipped silently; an unknown component in a
`"components"` object is a warning. Bad values are errors with file, node and field, and are
skipped: `level.glb: node 'Coin_02'.extras.spin.speed: expected a number`. Values: bool (also 1
/ 0), int, float, `[x, y]`, `[x, y, z]`, colour (`"#rrggbb"`, `[r, g, b(, a)]`), string, asset path,
node (a glTF node name in the same instance -> a `br_node3d`).

**3D components** (`br_component3d_add/get/remove/count/at/owner_at`) use the same registry ids
and descriptors as 2D components, with their own dense rows per kind keyed by 3D node, so a
system walks only the nodes that have the component. Scene-JSON 3D nodes take `"components"` too
(node fields name labels in the view, forward references work).

## Naming conventions (Blender object names and custom properties)

| Need | Object name suffix | Or extras | Result |
|---|---|---|---|
| Collider | `-col` (drawn), `-colonly` (not drawn) | `"collider": true` | `gltf.collider` component: box from the mesh bounds (`center`, `size`), unit cube for empties |
| Trigger volume | `-trigger` | `"trigger": true` | `gltf.collider` with `trigger = true`, mesh not drawn |
| Helper object (not drawn) | `-norender` | `"no_render": true` | meshes hidden and not pickable; node and components stay |
| Spawn point, pickup, ... | - | `"spawn_point": true`, `"pickup.value": 10` | your registered components |
| Hidden at start | - | `"visible": false` | group node hidden |
| Camera / render layer | - | `"layer": 2` | `br_node3d_set_layer` on the node and its content |
| No shadow | - | `"cast_shadows": false` | meshes / lights (lights: `true` turns shadows on) |
| Movable static mesh | - | `"batch": false` | kept out of automatic batching |

`br_gltf_collider_component(e)` is the id; iterate with `br_component3d_count/at/owner_at`.

**Blender how-to:** select the object > Object Properties > Custom Properties > New; set the name
(`spin.speed`) and type/value (Float 2.0; Boolean for flags). Name helper objects with the
suffixes above. For level-wide settings use Scene Properties > Custom Properties. Export with
File > Export > glTF 2.0: Format *glTF Binary (.glb)*; Include: *Custom Properties*, *Cameras*,
*Punctual Lights*; Transform: *+Y Up*; Data > Mesh: *Apply Modifiers*; Animation on (each action
becomes a clip). Draco compression must stay off (it is rejected). *Use GPU Instances* (Scene
Graph) writes EXT_mesh_gpu_instancing for linked duplicates.

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
| `br_scene_load(e, "level.glb", mode)`, `br_gltf_set_load_options` | A glTF file as a scene (camera, autoplay, batching). |
| `br_gltf_instantiate(e, "kit.glb#Tree", parent, &opts)`, `br_model_instantiate_ex` | Place a model or one kit part (options: node, autoplay, no_batching). |
| `br_model3d_camera(e, inst, "TopCam")` + `br_view3d_set_camera` | Use an imported camera. |
| `br_component3d_count/at/owner_at`, `_get`, `_add` | Components on 3D nodes (from extras or code). |

## Snippet (from examples/gltf_scene_demo.c)

```c
br_gltf_options load;
SDL_zero(load);
load.no_batching = g_no_batch;
load.camera = g_top ? "TopCam" : NULL;
br_gltf_set_load_options(e, &load);
d->level = br_scene_load(e, "gltf_scene/level.glb", BR_SCENE_SINGLE);
```

A system over a component that came from glTF extras (`spin_system`):

```c
uint32_t count = br_component3d_count(e, d->spin_id);
for (uint32_t i = 0; i < count; i++) {
    const spin *s = br_component3d_at(e, d->spin_id, i);
    br_node3d n = br_component3d_owner_at(e, d->spin_id, i);
    if (!br_node3d_valid(e, n)) {
        continue;
    }
    br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), s->speed * dt);
    br_node3d_set_rotation(e, n, br_quat_mul(turn, br_node3d_rotation(e, n)));
}
```

Kit parts from `gltf_scene/props.json` (`place_props`):
`br_node3d n = br_gltf_instantiate(e, source->valuestring, root, NULL); /* kit parsed once, shared */`

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
animator. Task 18 keys: `"source": "kit.glb#Node"`, `"camera"` (true / node name), `"default_light"`,
`"batch"`, `"autoplay"`, and `"components"` on every 3D node.

## Gotchas

- Conventions match the 3D core and glTF: right-handed, y up, CCW front faces, uv (0,0) = top
  left of the image. Verified with OrientationTest, TextureCoordinateTest (text reads
  correctly), NormalTangentMirrorTest and TextureTransformTest on D3D12 and Vulkan.
- Compressed glTF (`KHR_draco_mesh_compression`, `EXT_meshopt_compression`,
  `KHR_texture_basisu`) fails with an error naming the extension (no C decoders; deferred by
  design). Unknown optional extensions (e.g. `KHR_materials_clearcoat`, `_ior`, `_volume`) load
  and are logged once per engine by name; unknown required ones fail.
- Material extensions: emissive strength, unlit (FLAT model), texture transform, transmission
  (alpha blending with alpha = 1 - 0.7 * factor; no refraction).
- Cameras are imported as `camera3d` nodes (the View3D's camera), not as `br_camera` nodes: a
  `br_camera` 3D camera owns its own off-stage View3D, which would render the level a second time.
  Put follow / shake on a `br_camera_create_3d` over `br_scene_root_3d` if you need them, and
  hide the scene's own view (`br_node_set_visible`) or the level is drawn twice; copy the view's
  environment by hand (Crate Rush `src/flow.c`, `cr_level_setup`; API_REVIEW items 23-24).
- Batching merges repeated meshes inside one instance; kit parts placed as separate instances
  are separate draws (`ponytail:` limit; a cross-instance merge pass would remove it).
- A 3D component's `BR_FIELD_NODE` field holds a `br_node3d`; extras are parsed per instance.
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
- Scene-JSON meshes create their own geometry/material; model references belong to the scene /
  scene-file root / instance that loaded them and are released with it.

## Numbers (RTX 3060 Ti, Release, 1280x720, MSAA 4, vsync off)

`gltf_scene_demo` (level.glb: 8 repeated two-primitive trees, 6 GPU-instanced rocks, plus 9 kit
parts and the Fox; 3 shadow cascades): batching on 32 opaque items / 129 draw calls (main +
shadow passes), `--no-batch` 46 items / 185 draw calls, identical on D3D12 and Vulkan.

`gltf_demo --foxes 200 --novsync` (201 skinned Fox instances with 24 joints each, desynchronised
clips, plus the demo scene and inset view; 209 animators): 1.28 ms/frame on D3D12, 1.44 ms on
Vulkan; the `gltf.animate` system takes 0.46-0.49 ms of that. Without the extra foxes: 0.43 /
0.50 ms per frame.
