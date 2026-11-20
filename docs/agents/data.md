# data - scene files, prefabs, game.json, hot reload

Headers: `engine/include/braty/br_scene_file.h`, `br_prefab.h`, `br_game_config.h`.
Source: `engine/src/data/` (one `br_node_json_<type>.c` per built-in node type).
Test: `tests/test_scene_file.c`. Example: `examples/data_demo.c` + `examples/data/`.
Every format with full examples: `docs/DATA_FORMATS.md`.

## The model

- A **scene file** is one node object (`type`, `label`, container fields, type fields,
  `components`, `children`). The loader creates each node (a factory for types whose creation
  needs data, e.g. rope meshes), applies the container fields, calls the type's `from_json`
  (installed with `br_node_type_set_from_json`), fills components through their
  `br_field_desc` list, then builds the children.
- Label references (`BR_FIELD_NODE` fields, `mask`, `layer`) resolve after the whole file is
  built: forward references work.
- Errors never abort: `file: json.path: reason` is logged (and kept in
  `br_scene_file_last_error`), the bad value or node is skipped, the rest loads.
- Each node remembers the JSON it came from; `br_scene_file_save` writes live transforms,
  components and text plus that stored JSON, so load -> save -> load is stable.
- **Prefabs** are scene files instantiated with overrides (objects merge, `children` append).
- **game.json** fills `br_app_desc` in `br_main` (no engine yet); with `br_game_config_main`
  the engine applies the rest before init and loads `start_scene` after init.
- **Asset references**: the outermost load (`br_scene_file_load*`, `br_prefab_instantiate`,
  hot reload) captures every reference it takes. A `br_scene` (`docs/agents/scenes.md`) gets
  them in its group; otherwise the subtree's root owns them (owner `file:<path>`) and they are
  released one frame after the root dies (after the node flush; at once outside a frame means at
  the next step). Meshes and materials made from view3d JSON are freed with their 3D node.
- **Hot reload** (debug builds): files loaded with `br_scene_file_load` are polled every 30 frames
  (`SDL_GetPathInfo`) and rebuilt into the same root node.

## Functions you actually need

| Function | What for |
|---|---|
| `br_scene_file_load(e, path, parent)` | Build a scene file under parent; returns its root. |
| `br_scene_file_load_json(e, text, size, name, parent)` | Same from memory (tests, tools). |
| `br_scene_file_save(e, root, path)`, `br_scene_file_to_json` | Write a subtree back out. |
| `br_prefab_instantiate(e, path, parent, overrides)`, `_at(e, path, parent, pos)` | One prefab instance (at a position). |
| `br_scene_file_add_handler(e, "name", fn, user)` | Lets files attach `"on": { "click": "name" }`. |
| `br_game_config_main(path, argc, argv, init)` | game.json for the whole app in one line (the engine owns the config). |
| `br_game_config_load` / `_args` / `_app_desc` / `_apply` | The same in steps, when a game needs more control. |
| `br_data_get_float/vec2/color/texture/enum/...` | Reading keys in your own type's `from_json` (errors carry the path). |
| `br_data_get_count(e, obj, key, min, max, &n)`, `br_data_get_int_range` | A count, size or index from data, range-checked before any cast: use it for every number that sizes a loop or an allocation. |
| `br_scene_file_parse_node`, `_apply_component`, `_read_field`, `_write_field`, `_collect_assets` | Building blocks for scenes/tools. |
| `br_scene_file_set_hot_reload`, `br_scene_file_poll` | Hot-reload control. |

## Snippet (from examples/data_demo.c)

```c
static bool demo_init(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
        { "spin", BR_FIELD_FLOAT, offsetof(mover, spin) },
    };
    br_component_desc desc = { "mover", sizeof(mover), fields, BR_ARRAY_COUNT(fields) };
    br_component_register(e, &desc);
    br_system_add(e, "demo.mover", BR_PHASE_UPDATE, 0, mover_system, NULL);
    return true; /* game.json was applied before init; its start scene loads after it */
}

br_app_desc br_main(int argc, char **argv)
{
    /* BRATY_EXAMPLES_DIR (set by CMake) is the source tree, so any build directory works */
    br_app_desc desc = br_game_config_main(BRATY_EXAMPLES_DIR "/data/game.json", argc, argv, demo_init);
    desc.asset_root = BRATY_EXAMPLES_DIR;
    return desc;
}
```

## Scene JSON (from examples/data/demo.scene.json)

```json
{ "type": "sprite", "label": "hero", "texture": "data/atlas.json#hero.png", "position": [80, 60], "scale": 2,
  "components": { "mover": { "velocity": [0, 0], "spin": 1.5 } } },
{ "prefab": "data/coin.prefab.json", "position": [400, 0],
  "components": { "mover": { "velocity": [-90, 0] } }, "tint": "#ffb0b0" }
```

## 3D views in scene files

A `view3d` node (a 2D node that renders a 3D scene) is read by
`engine/src/data/br_node_json_view3d.c`: view settings, a camera, and a `scene` array of 3D nodes
(`group`, `mesh`, `light`, `gltf`). Full key list: `docs/DATA_FORMATS.md`, section "3D views".

```json
{ "type": "view3d", "size": [420, 250], "position": [845, 455], "msaa": 4, "background": "#20242c",
  "camera": { "position": [0, 2, 6], "target": [0, 1, 0], "fov": 50 },
  "scene": [
    { "type": "light", "kind": "directional", "intensity": 2, "shadows": true, "position": [-4, 8, 4] },
    { "type": "mesh", "shape": "cube", "size": [1, 1, 1], "material": { "model": "pbr", "color": "#c04040" } },
    { "type": "gltf", "source": "models/Fox/Fox.glb", "animation": "Run", "scale": 0.02 } ] }
```

`br_scene_file_save` writes a view3d as it was loaded (live changes to its 3D nodes are not written), and 3D keys are not checked for typos.

## Your own node type in scene files

Register the type, then `br_node_type_set_from_json(e, type_id, my_from_json)`. In
`my_from_json(e, n, data, json)` read keys with `br_data_get_*`: absent keys return false
silently, wrong types log `file: children[3].my_key: expected a number`.

## Gotchas

- Register components (and handlers) **before** loading files that use them.
- Unknown keys of built-in node types, text styles and game.json are logged as warnings
  (`level.scene.json: children[0].posiiton: unknown key`); your own types' keys are not checked.
  Game settings in game.json go under `"game"`.
- Texture references: `"path.png"`, `"sheet.json#frame"`, `"alias#frame"` (a manifest alias of
  the sheet, shorter), or a bare frame name of a sheet that is already loaded (list sheets in the
  root's `"assets"` so they load first). Each takes a reference on the texture or sheet, owned by
  the load. A bare name is one map lookup; with several loaded sheets having it, the first loaded
  wins (a warning names the others).
- Prefab cycles (`a` includes `a`, or `a -> b -> a`) are reported with the chain and the closing
  node is skipped; prefabs nest at most 16 deep, nodes 256 deep. Asset collection opens each
  prefab file once per load.
- Numbers from data are checked before they become counts or ints (`... is out of range`):
  polygon sides 3..1024, star points 2..1024, mesh grids 2..1024 per side, blur quality 1..16,
  colour numbers 0..0xFFFFFF, ints within 32 bits, floats within float range.
- game.json: a value of the wrong type, out of range or too long for its field is logged and the
  default is kept (`br_game_config_parse` still returns true; false only for a syntax error).
- Root `"assets"` entries: a path, a manifest bundle name, or `{ "path" | "bundle": ..., "load":
  "preload" | "lazy" }`; `lazy` ones are listed in a br_scene's group but not loaded by the file.
- Component pointers (`br_component_get/at`) die when any component of that kind is added (a
  prefab instantiated, a scene loaded): re-fetch after anything that may create nodes.
- `BR_FIELD_STRING`/`BR_FIELD_ASSET` hold 63 bytes (longer is an error naming `BR_FIELD_TEXT`, a
  heap string of any length); asset fields are not loaded for you (arrays of assets are listed
  by `br_scene_file_collect_assets` like single ones).
- One reader for every field type (`br_data_read_field`, `br_scene_file.c`) serves scene files,
  prefabs, glTF extras / view3d JSON (only bools differ: extras also take 1 / 0) and play-test
  `set`; node labels are resolved by a callback per caller.
- Saving: graphics commands, filters, masks, hit areas and handlers are written as loaded (the
  engine cannot read them back from a node); edit them in code and they are not saved
  (`ponytail:` in br_node_json_container.c).
- Components named `module.name` are engine-internal and never saved.
- Hot reload recreates the children of the watched root: handles to old children turn stale.
  Edited prefab files are re-read only when a scene using them changes.
- `view3d` contents are saved as loaded, not from the live 3D nodes.
