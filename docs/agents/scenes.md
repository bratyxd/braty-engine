# scenes - first-class scenes, asset groups, transitions, persistent nodes

Headers: `engine/include/braty/br_scene.h` (scenes), `br_asset_group.h` (groups; implemented in
`engine/src/assets/br_asset_group.c`). Source: `engine/src/scenes/br_scenes.c`. Tests:
`tests/test_scene.c`, `tests/test_asset_group.c` (data in `tests/data/scenes/`). Example:
`examples/scenes_demo.c` + `examples/scenes/` (golden `tests/golden/scenes_demo.png`).
The scene *graph* (nodes, `br_stage_root`) is the other module, `docs/agents/scene.md`.

## The model

- **Three levels of asset loading, all reference counted.** Single: `br_assets_load` /
  `br_assets_unload`. Group: `br_asset_group` (a named set, one reference per asset, loaded sync
  or async with progress). Scene: every `br_scene` owns a group that holds everything its file
  references. An asset shared by two owners stays loaded until the last one lets go.
- **A scene** = a scene file built under a 2D root container on the stage + its group. The group
  gets the root's `"assets"` list (paths or manifest bundle names, each `preload` or `lazy`) and
  every path the nodes, prefabs and components name (textures, sheets, fonts, glTF `source`,
  `BR_FIELD_ASSET` fields such as sounds). Loading a scene loads them first, then builds nodes.
- **Single / additive.** `BR_SCENE_SINGLE` unloads every other scene once it is built and
  becomes active; `BR_SCENE_ADDITIVE` goes on top. Later scenes draw on top.
- **Ownership.** A scene owns its root's subtree. Nodes added to the stage while a scene is
  active move into its root at the start of the next frame. Nodes on the stage while no scene is
  active belong to nobody. `br_node_set_persistent` keeps a node across unloads (drawn above scenes).
- **Unload** destroys the nodes now and gives the group back after the node flush (dying text may
  still use its font). Unloading a scene that is still loading is safe: groups are polled, never
  called back, and the asset core cancels loads nobody wants any more.
- **Switch** (`br_scene_switch`): fade to black, unload everything, show the loading scene (if
  set), keep loading the target (started at the switch), fade, build target, fade in.
- **Deterministic mode** (`--deterministic`, play-tests, goldens): async groups request one asset
  per frame in file order, so progress and the frame a scene appears are reproducible.

## Which API to use when

| You want | Use |
|---|---|
| A screen, menu, level, HUD layer (load, unload, switch) | `br_scene_load` / `_async` / `_switch` |
| A piece inside a scene, from code (spawned enemy, debris) | `br_prefab_instantiate(_at)` |
| A file under a node you manage yourself (tools, old code) | `br_scene_file_load` (its root owns its asset references) |
| Assets with no scene (music for the whole game, a preload set) | `br_asset_group` or a manifest bundle |

## Functions you actually need

| Function | What for |
|---|---|
| `br_scene_load(e, "level1", BR_SCENE_SINGLE)` | Load now; a game.json `"scenes"` key or a path. |
| `br_scene_load_async(e, name, mode)` + `br_scene_progress` / `_is_loaded` | Background load. |
| `br_scene_unload(e, s)` | Nodes now, assets after the flush. |
| `br_scene_switch(e, name, BR_TRANSITION_FADE, 0.3f)`, `br_scene_set_loading_scene` | Transition with optional loading screen. |
| `br_scene_active`, `_set_active`, `_active_name`, `_find`, `_count`, `_at` | Which scenes exist. |
| `br_scene_root_2d(e, s)`, `br_scene_root_3d(e, s)` | Where its nodes are (3D: first view3d, created if none). |
| `br_scene_on_event(e, fn, user)` | LOADED / ACTIVATED / UNLOADING, synchronous. |
| `br_system_add_for_scene(e, s, name, phase, prio, fn, user)` | A system that stops with the scene. |
| `br_node_set_persistent(e, n, true)` | DontDestroyOnLoad. |
| `br_asset_group_create/add/add_lazy/add_manifest_bundle/load/load_async/unload/destroy` | Groups. |
| `br_assets_report(e)`, `br_assets_owners(e, path, out, max)`, `br_assets_list` | Who holds what (type, refs, bytes, owners). |

## Snippet (examples/scenes_demo.c)

```c
static void flow_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo *d = user;
    if (!br_input_action_pressed(e, "next") || br_scene_switching(e)) {
        return;
    }
    d->idle = 0;
    d->step = (d->step + 1) % 4;
    if (d->step == 1) {
        br_scene_switch(e, "level_a", BR_TRANSITION_FADE, 0.2f);
    } else if (d->step == 2) {
        br_scene_load(e, "overlay", BR_SCENE_ADDITIVE); /* on top of level A, level A stays active */
    } else if (d->step == 3) {
        br_scene_switch(e, "level_b", BR_TRANSITION_FADE, 0.2f); /* unloads level A and the overlay */
    } else {
        br_scene_switch(e, "menu", BR_TRANSITION_FADE, 0.2f);
    }
}
```

The loading scene's progress bar is a scene system added when it loads:

```c
    if (SDL_strcmp(br_scene_name(e, scene), "loading") == 0) {
        /* runs only while the loading scene exists; the name is reused by the next one */
        br_system_add_for_scene(e, scene, "demo.progress", BR_PHASE_UPDATE, 0, progress_system, NULL);
    }
```

game.json names the scenes (`examples/scenes/game.json`):

```json
"start_scene": "menu",
"scenes": { "menu": "scenes/menu.scene.json", "loading": "scenes/loading.scene.json",
            "level_a": "scenes/level_a.scene.json" }
```

## Gotchas

- A scene's name is its game.json `"scenes"` key, else the file name without extensions.
- The file's own root is the child of `br_scene_root_2d`; find nodes with
  `br_node_find_by_label(e, br_scene_root_2d(e, s), "hero", true)`.
- Automatic collection only adds paths that exist as files or are already cached (a bare frame
  name like `"hero.png"` is resolved by the node, not loaded as a file).
- `br_system_add_for_scene`: the system is removed (`br_system_remove`) when its scene unloads,
  so a later scene can add the same name with its own phase and priority. A name that an engine
  or game system already has is refused (logged): the unload only removes what the scene added.
- A switch whose target is unloaded during the fade-out keeps the scenes that are there and fades
  back in (nothing is unloaded).
- Assets of an unloaded scene are freed at the next frame's flush (or at once outside a frame):
  a report printed in the same frame still lists them.
- Nodes are built in one frame once the assets are in (not time-sliced).
- Group callbacks run from `assets.update`; destroying a group inside its own callback is fine.
- The start scene of game.json is a `br_scene` (SINGLE); `br_game_config_apply` returns the file's root.
