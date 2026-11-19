# debug - in-game debug overlay (microui)

Header: `engine/include/braty/br_debug.h`. Source: `engine/src/debug/`.
Example: `examples/data_demo.c --debug`. Test: `tests/test_scene_file.c` (log capture, toggle).

## What it does

F1 (configurable) toggles five microui windows drawn after the stage through
`br_render_add_overlay`, with glyphs from a bitmap font installed from the text module's default
font:

| Window | Shows |
|---|---|
| Stats | FPS, frame-time graph (red = over 17 ms), draw calls / batches / passes / vertices / nodes drawn (`br_render_get_stats`), node / texture / asset counts, render-target pool, asset cache list (`br_assets_list`) |
| Systems | every system (`br_system_get_info`): enable checkbox, phase, last run time |
| Scene | node tree under the stage; `+`/`-` opens a node, clicking a name selects it |
| Inspector | live position, scale, rotation, alpha (slider), visible of the selected node, editable |
| Log | the last 256 lines logged through `br_log_*` / `SDL_Log` |

Input comes from the input module (mouse, wheel, Shift/Ctrl/Alt/Backspace/Return, text).

## Functions you actually need

| Function | What for |
|---|---|
| `br_debug_set_open(e, true)`, `br_debug_is_open` | Open/close from code. |
| `br_debug_set_toggle_key(e, SDL_SCANCODE_F12)` | Another toggle key (game.json `debug.toggle_key`). |
| `br_debug_select(e, n)`, `br_debug_selected` | Point the inspector at a node. |
| `br_debug_log_count`, `br_debug_log_line` | Read the captured log. |

## Snippet

The overlay needs no code: it is registered for every engine. Open it at start from game.json
(`"debug": { "overlay": true }`) or with `--debug` when br_main calls `br_game_config_args`
(from examples/data_demo.c):

```c
br_game_config_args(&config, argc, argv);                             /* --debug, --scene */
```

## Gotchas

- Log capture hooks SDL's process-wide log output (chained to the previous one); with several
  engines in one process the last one created captures.
- Headless engines never draw (the overlay runs in render.collect), but the toggle key, log and
  selection work.
- Stats are those of the previous frame (the overlay is collected inside the current one).
- The overlay does not swallow input: a click on a window also reaches pointer events and
  game systems underneath.
- Glyphs are rasterised at 1 pixel per unit (soft on HiDPI; `ponytail:` in br_debug.c).
