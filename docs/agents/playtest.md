# playtest - scripted and interactive play-testing, state queries, record and replay

Header: `engine/include/braty/br_playtest.h`. Source: `engine/src/playtest/`. Test:
`tests/test_playtest.c`. Driver: `tools/playtest.py`. User guide with the full command
reference: `docs/PLAYTESTING.md`. Used by: `tools/golden.py`, Star Courier's `playtest/*.json`.

Every `br_app` executable has it; it does nothing unless `--playtest FILE`, `--playtest-stdin`
or `--record FILE` is given (or the C API is called).

## How it works

- One system, `playtest.frame` (BR_PHASE_INPUT, priority -3000000, before `input.begin_frame`
  latches input): releases taps that are due, then runs script steps until one has to wait.
  Input is injected with `br_input_inject_*`, so it is indistinguishable from a device.
- Commands are JSON objects described by one table (`br_pt_commands`: name, typed arguments
  `"key:string action?:string"`, one-line doc). `br_pt_validate` checks a command against it
  before anything runs (scripts: at load, with the step number; stdin: one `ok:false` reply),
  and `{"cmd":"help"}` returns it. A new command = a table row + a branch in `br_pt_exec`.
- Commands are JSON objects; `br_pt_exec` runs one. `wait` / `wait_until` set a wait that the
  script resumes from in a later frame; `br_playtest_command` (interactive, C API) instead steps
  the engine in place until the wait is over, then replies.
- Paths (`br_playtest_query.c`): `label:property`, `label:component.field`, `name:field` for
  `br_playtest_expose`d structs, optional `.x` element. Reads use the data module's writers
  (`br_scene_file_to_json`, `br_scene_file_write_field`); node writes go through
  `br_data_apply_node`, so every scene-file key can be set.
- Recording is an event handler: every real input event becomes a command stamped with the
  frame that consumes it (`e->stepping ? frame : frame + 1`), written as waits + commands.
  Gamepads: br_input handles an event before the recorder (registered earlier), so the
  recorder keeps its own slot map (`pad_ids`) to name the slot of a removed pad and to skip
  SDL's late duplicate ADDED of a virtual pad. Pads replay as `connect` (virtual pad of the
  recorded type, moved to the recorded slot) + injected buttons/axes; axis values round-trip
  exactly because injection rounds (`SDL_lroundf`).
- `--playtest-stdin` is `br_playtest_stdin.c`: the only engine file with C stdio, compiled only
  with `BRATY_PLAYTEST_STDIN`. `br_app_iterate` calls it instead of stepping: read a line
  (blocking), run it, print the reply.

## Functions you actually need

| Function | What for |
|---|---|
| `br_playtest_expose(e, "game", fields, count, &state)` | Make game state readable/writable as `game:<field>`. |
| `br_playtest_run_script(e, path)` | Headless test: run a script to the end, true = passed. |
| `br_playtest_command(e, json)` | One interactive command, returns the reply line (`br_free`). |
| `br_playtest_get(e, path)` / `_set(e, path, value)` | Read / write by path (cJSON values). |
| `br_playtest_step(e, frames)` | Advance deterministic 1/60 s frames. |
| `br_playtest_dump_json(e)` | Scene tree + components + exposed state as text. |
| `br_playtest_record_start(e, path)` / `_stop(e)` | Record real input as a script. |
| `br_playtest_last_error(e)` | Why the last command or script failed. |

## Snippet (the pattern of tests/test_playtest.c)

```c
br_engine *e = make_game();   /* br_engine_create + a "walker" component, its system, a "hero" node */
char *reply = br_playtest_command(e, "{\"cmd\":\"key\",\"key\":\"Right\",\"action\":\"down\"}");
br_free(reply);
reply = br_playtest_command(e, "{\"cmd\":\"step\",\"frames\":60}");  /* {"ok":true,"frame":60} */
br_free(reply);
cJSON *x = br_playtest_get(e, "hero:x");     /* 160: from 100 at 60 units per second */
cJSON_Delete(x);
br_engine_destroy(e);
```

## Gotchas

- `click` moves the pointer now, presses in the next frame and releases in the one after (a
  press in the move frame looks like a drag to camera controls): step 3 frames before checking.

- Scripts start running in frame 1; a screenshot taken in frame N shows frame N-1 (the last
  rendered one). `{"cmd":"wait","frames":N}` then `screenshot` shows frame N.
- `--playtest*` imply `--deterministic` and default to `--vsync off`. A game that reads save data
  should skip it when `br_app_deterministic(e)`.
- The log capture replaces SDL's log output function (process-wide) while play-testing; two
  engines in one process must shut down in reverse order (see the `ponytail:`).
- `action` injects the action (`br_input_inject_action`), not a key: systems polling keys
  directly do not see it.
- 3D nodes are addressed as `view/node:property` through the three_d public API
  (`br_node3d_find_by_label` under `br_view3d_root`, `br_view3d_world_to_screen` for `click` and
  `screen_position`); labels must not contain `/` or `:`.
