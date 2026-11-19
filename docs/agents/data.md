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
- **game.json** fills `br_app_desc` in `br_main` (no engine yet) and applies the rest in init.
- **Hot reload** (debug builds): files loaded with `br_scene_file_load` are polled every 30 frames
  (`SDL_GetPathInfo`) and rebuilt into the same root node.

## Functions you actually need

| Function | What for |
|---|---|
| `br_scene_file_load(e, path, parent)` | Build a scene file under parent; returns its root. |
| `br_scene_file_load_json(e, text, size, name, parent)` | Same from memory (tests, tools). |
| `br_scene_file_save(e, root, path)`, `br_scene_file_to_json` | Write a subtree back out. |
| `br_prefab_instantiate(e, path, parent, overrides)` | One prefab instance. |
| `br_scene_file_add_handler(e, "name", fn, user)` | Lets files attach `"on": { "click": "name" }`. |
| `br_game_config_load` / `_args` / `_app_desc` / `_apply` | game.json from `br_main` to init. |
| `br_data_get_float/vec2/color/texture/enum/...` | Reading keys in your own type's `from_json` (errors carry the path). |
| `br_scene_file_parse_node`, `_apply_component`, `_read_field`, `_write_field`, `_collect_assets` | Building blocks for scenes/tools. |
| `br_scene_file_set_hot_reload`, `br_scene_file_poll` | Hot-reload control. |

## Snippet (from examples/data_demo.c)

```c
static br_game_config config; /* br_main's desc points into it */

static bool demo_init(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
        { "spin", BR_FIELD_FLOAT, offsetof(mover, spin) },
    };
    br_component_desc desc = { "mover", sizeof(mover), fields, BR_ARRAY_COUNT(fields) };
    br_component_register(e, &desc);
    br_system_add(e, "demo.mover", BR_PHASE_UPDATE, 0, mover_system, NULL);
    return br_node_valid(e, br_game_config_apply(e, &config)); /* loads the start scene */
}

br_app_desc br_main(int argc, char **argv)
{
    br_game_config_load(NULL, "../../examples/data/game.json", &config); /* relative to the exe */
    br_game_config_args(&config, argc, argv);                             /* --debug, --scene */
    br_app_desc desc = br_game_config_app_desc(&config);
    desc.init = demo_init;
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

## Your own node type in scene files

Register the type, then `br_node_type_set_from_json(e, type_id, my_from_json)`. In
`my_from_json(e, n, data, json)` read keys with `br_data_get_*`: absent keys return false
silently, wrong types log `file: children[3].my_key: expected a number`.

## Gotchas

- Register components (and handlers) **before** loading files that use them.
- Texture references: `"path.png"`, `"sheet.json#frame"`, or a bare frame name of a sheet that is
  already loaded (list sheets in the root's `"assets"` so they load first).
- `BR_FIELD_STRING`/`BR_FIELD_ASSET` hold 63 bytes; asset fields are not loaded for you.
- Saving: graphics commands, filters, masks, hit areas and handlers are written as loaded (the
  engine cannot read them back from a node); edit them in code and they are not saved
  (`ponytail:` in br_node_json_container.c).
- Components named `module.name` are engine-internal and never saved.
- Scene loads take asset references that are not released when the scene is destroyed (Phase 2
  Task 16 adds asset groups).
- Hot reload recreates the children of the watched root: handles to old children turn stale.
  Edited prefab files are re-read only when a scene using them changes.
- `view3d` (Task 9) has no JSON yet.
