# debug - in-game debug overlay (microui)

Header: `engine/include/braty/br_debug.h`. Source: `engine/src/debug/`.
Example: `examples/data_demo.c --debug`, `examples/profiler_demo.c` (F2 panel). Tests:
`tests/test_scene_file.c` (log capture, toggle), `tests/test_profiler.c` (input taken by the
overlay), `tests/test_debug.c` (the font replaced while the overlay is open; needs a window).

## What it does

F1 (configurable) toggles five microui windows drawn after the stage through
`br_render_add_overlay`, with glyphs from a bitmap font installed from the text module's default
font:

| Window | Shows |
|---|---|
| Stats | FPS, frame-time graph (red = over the pacing frame budget + 10%), draw calls / batches / passes / vertices / nodes drawn (`br_render_get_stats`), node / texture / asset counts, render-target pool, heap (`br_memory_get_stats`: live KB, blocks, peak), Arenas header (every tracked arena: used / capacity / peak, see `docs/agents/arena.md`), asset cache list (`br_assets_list`) |
| Systems | every system (`br_system_get_info`): enable checkbox, phase, last run time |
| Scene | node tree under the stage; `+`/`-` opens a node, clicking a name selects it |
| Inspector | live position, scale, rotation, alpha (slider), visible of the selected node, editable |
| Log | the last 256 lines logged through `br_log_*` / `SDL_Log` |

F2 toggles the **profiler panel** and `--profiler-hud` shows its corner readout
(`br_debug_profiler.c`, numbers from `docs/agents/profiler.md`).

Input comes from the input module (mouse, wheel, Shift/Ctrl/Alt/Backspace/Return, text). The
overlay installs an SDL event filter: a click or wheel on one of its windows (and the release of
such a click), and key presses / text while a field is edited, are fed to microui and **never
reach the game** (input module, pointer events, systems). Key releases always reach the game, so
a key held when an edit starts (W to walk) does not stay down. The profiler corner readout lets
clicks through.

## Functions you actually need

| Function | What for |
|---|---|
| `br_debug_set_open(e, true)`, `br_debug_is_open` | Open/close from code. |
| `br_debug_set_toggle_key(e, SDL_SCANCODE_F12)` | Another toggle key (game.json `debug.toggle_key`; `SDL_SCANCODE_UNKNOWN` / `"none"` = no key). |
| `br_debug_set_profiler_key(e, key)` | Another profiler key (default F2; `SDL_SCANCODE_UNKNOWN` = none). |
| `br_debug_set_keys_enabled(e, false)` | Shipping: F1 / F2 do nothing (game.json `"debug": { "keys": false }`); code can still open both. |
| `br_debug_wants_event(e, &ev)` | Would the overlay take this event (it then never reaches the game)? |
| `br_debug_select(e, n)`, `br_debug_selected` | Point the inspector at a node. |
| `br_debug_log_count`, `br_debug_log_line(e, i, buf, size)` | Read the captured log (copied under the lock; any thread may log). |

## Snippet

The overlay needs no code: it is registered for every engine. Open it at start from game.json
(`"debug": { "overlay": true }`) or with `--debug` (read by `br_game_config_main`, which calls
`br_game_config_args`; examples/data_demo.c):

```c
br_app_desc desc = br_game_config_main(BRATY_EXAMPLES_DIR "/data/game.json", argc, argv, demo_init);
```

A shipped game turns the keys off in game.json:

```json
"debug": { "keys": false, "hot_reload": false }
```

## Gotchas

- Log capture hooks SDL's process-wide log output (chained to the previous one); with several
  engines in one process the last one created captures.
- The UI is built in PRE_RENDER (`debug.ui`, also headless, so hit tests work in tests) and
  drawn in render.collect; headless engines never draw it.
- Stats are those of the previous frame (the overlay is collected inside the current one).
- The event filter is process-wide (chained to any previous filter), like the log hook. Input
  injected with `br_input_inject_*` (play-tests) skips SDL's queue and therefore the filter: an
  injected click on a window reaches microui and the game.
- Glyphs are rasterised at 1 pixel per unit (soft on HiDPI; `ponytail:` in br_debug.c).
- The glyphs are the bitmap font installed as `"braty.debug"`, looked up by name on every draw: a
  game may install its own font under that name (the overlay uses it) or uninstall it (the
  overlay installs its own again on the next frame).
