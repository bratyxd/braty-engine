# Play-testing a Braty game

Every Braty executable can be driven by a script or by another program: press keys and input
actions, click nodes by label, step frames, take screenshots, read and write node properties,
component fields and game state, and check them. It is built for AI agents (look at a
screenshot, decide, send the next input) and for regression tests (a recorded session plus a few
`expect` lines is a test). Module: `br_playtest` (`engine/include/braty/br_playtest.h`, guide
`docs/agents/playtest.md`). Nothing runs unless one of the flags below is given.

## Recipe (copy, paste, run from the repository root)

```sh
cmake --preset windows-gcc
cmake --build --preset windows-gcc
python tools/playtest.py build/windows-gcc/sample_projects/star_courier/star_courier.exe "step 60" "action start" "step 30" "screenshot build/playtest_shot.png" "get ship:position" quit
```

Output (each line is the game's reply to one command; `frame` is the frame count after it):

```
{"ok": true, "frame": 60}
{"ok": true, "frame": 60}
{"ok": true, "frame": 90}
{"ok": true, "frame": 90, "file": "build/playtest_shot.png"}
{"ok": true, "frame": 90, "value": [400, 400]}
{"ok": true, "frame": 90}
```

Open `build/playtest_shot.png` to see the game 30 frames into the first round. The same from
Python (save as a file, run from the repository root; it prints `[474.196045, 400] play 3`):

```python
import sys
sys.path.insert(0, "tools")
from playtest import Playtest

with Playtest("build/windows-gcc/sample_projects/star_courier/star_courier.exe") as game:
    game.step(60)                       # the title screen
    game.action("start")                # the "start" input action (game.json binds it to Return)
    game.step(30)
    game.key("Right", frames=20)        # hold Right for 20 frames...
    game.step(20)                       # ...while 20 frames pass
    game.screenshot("build/playtest_shot.png")
    print(game.get("ship:position"), game.get("game:state"), game.get("game:lives"))
```

Other games: replace the executable. Labels (`ship`), components and exposed state (`game:`)
are the game's own; `dump` shows all of them.

## Three ways to run

| Flag | What happens |
|---|---|
| `--playtest script.json` | Runs a script (a JSON array of commands), then exits: code 0 if every step passed, 1 if one failed (the reason and step number are logged). |
| `--playtest-stdin` | Interactive: one JSON command per line on stdin, one JSON reply per line on stdout. The game advances only on `step` / `wait`. `tools/playtest.py` speaks this. |
| `--record session.json` | Plays normally and writes the real keyboard / mouse / text / touch / gamepad input as a script at exit. |

`--playtest` and `--playtest-stdin` imply `--deterministic` (every frame is exactly 1/60 s, the
engine RNG is seeded, asset loads complete in order) and `--vsync off` unless `--vsync` is given,
so the same commands give the same frames, pixel for pixel. Add `--headless` to run without a
window or GPU (no screenshots); `ctest` runs Star Courier's round that way. Paths in commands
(screenshots, dumps, scripts) are relative to the working directory.

Replies always start with `"ok"` and `"frame"`; a failed command adds `"error"` and the game
keeps running. Every line gets exactly one reply: a line that is not JSON, not an object, an
unknown command, a missing argument, an argument of the wrong type (`{"cmd": "step",
"frames": "ten"}` -> `step: "frames" must be a number`) or an argument the command does not
take (a typo: `{"cmd": "key", "key": "Right", "frame": 30}` -> `key: unknown argument 'frame'
(takes: ...)`) each answer `{"ok": false, ..., "error": "..."}`; a script with such a step does
not start. Closing stdin ends the game (exit code 0, or 1 if any command failed).
`{"cmd": "help"}` lists every command with its arguments (`"key:string action?:string
frames?:number"`; `?` = optional) and a one-line description. Engine logs go to stderr, replies
to stdout.

`tools/playtest.py` raises `PlaytestError` (message = the engine's `"error"`, `.reply` = the
whole reply) for a failed command, a game that exited, or a reply that is not JSON. The game
process is ended when the `with` block exits (killed at once if it exits with an exception, or
after 30 s if `quit` hangs) and, failing that, when Python exits.

## Commands

One JSON object each, the same in scripts and on stdin.

### Time

| Command | Example | Notes |
|---|---|---|
| `step` / `wait` | `{"cmd": "step", "frames": 60}` | `"seconds": 1.5` also works (60 frames per second). Default 1 frame. |
| `wait_until` | `{"cmd": "wait_until", "path": "game:state", "value": "gameover", "timeout_frames": 1200}` | Steps until the condition holds; fails after the timeout (default 600 frames, or `"timeout"` in seconds). A path that does not resolve yet counts as "not yet". |

### Input (goes through `br_input_inject_*`, the same path as real devices)

| Command | Example | Notes |
|---|---|---|
| `key` | `{"cmd": "key", "key": "Space"}` | `"action"`: `tap` (default: down now, up after `"frames"`, default 1), `down`, `up`. SDL key names: `Space`, `Return`, `Escape`, `Left`, `A`, `F1`... |
| `action` | `{"cmd": "action", "name": "start"}` | Drives the input action itself (`br_input_inject_action`), whatever it is bound to (game.json or code). `"action"`: `tap` (default, `"frames"` long), `down` / `press` (held until `up`), `up` / `release`. `"value"` 0..1 (default 1) for analogue actions: `{"cmd": "action", "name": "throttle", "action": "down", "value": 0.4}`. Games that read keys directly (`br_input_key_down`) do not see it: use `key`. |
| `mouse` | `{"cmd": "mouse", "action": "tap", "x": 640, "y": 360}` | `move`, `down`, `up`, `tap`, `wheel` (`"dx"`, `"dy"`); `"button"`: 1 left, 2 middle, 3 right. Window coordinates. `move` takes an optional `"dx"`, `"dy"` (the relative movement; default: the difference to the last position), so relative-mode mouse-look replays. |
| `click` | `{"cmd": "click", "label": "play_button"}` | Moves the mouse to the centre of the node's bounds now, presses the left button (`"button"`) in the next frame and releases it in the frame after: step 3 frames to see the result. |
| `text` | `{"cmd": "text", "text": "PLAYER 1"}` | One text-input event, as an IME would send. |
| `touch` | `{"cmd": "touch", "action": "down", "finger": 0, "x": 100, "y": 200}` | `down`, `move`, `up`; window coordinates. |
| `gamepad` | see below | Virtual pads, buttons, sticks and triggers. |
| `window` | `{"cmd": "window", "width": 2520, "height": 1080}` | Resizes the window (16..16384 each; the next frame renders at the new size). Headless, it sets the screen size every camera sees (`br_camera_set_headless_screen`), so 21:9 and 9:16 layouts, letterboxing and anchors can be checked in one run: `window`, `step`, then `get` / `expect` / `screenshot`. Short form: `window 2520 1080`. |

`gamepad` commands (`"slot"` is the player slot 0..7, default 0; one example each):

| Action | Example | Notes |
|---|---|---|
| `connect` | `{"cmd": "gamepad", "action": "connect", "type": "ps5"}` | Attaches an SDL virtual pad of that family (also headless); the reply has its `"slot"`. Types: `xbox`, `playstation`, `switch`, `generic`, or exactly `xbox360`, `xboxone`, `ps3`, `ps4`, `ps5`, `switchpro`, `joyconleft`, `joyconright`, `joyconpair`, `gamecube`, `steam`. `"slot"` moves it to that slot. Glyphs and prompts follow the family. `"injected": true` makes an injected pad of that family in `"slot"` instead (no SDL device; fails if the slot is taken). |
| `disconnect` | `{"cmd": "gamepad", "action": "disconnect", "slot": 0}` | Removes the pad (virtual or injected). |
| `tap` | `{"cmd": "gamepad", "action": "tap", "button": "a", "frames": 2}` | Down now, up after `"frames"` (default 1). SDL button names: `a` (south face button: Cross, Switch B), `b`, `x`, `y`, `back`, `guide`, `start`, `leftstick`, `rightstick`, `leftshoulder`, `rightshoulder`, `dpup`, `dpdown`, `dpleft`, `dpright`, `misc1`, `touchpad`... |
| `down` | `{"cmd": "gamepad", "action": "down", "button": "rightshoulder"}` | Held until `up`. |
| `up` | `{"cmd": "gamepad", "action": "up", "button": "rightshoulder"}` | |
| `axis` | `{"cmd": "gamepad", "action": "axis", "axis": "leftx", "value": 1}` | Holds the axis at `"value"` until the next `axis`: `leftx`, `lefty` (y > 0 = down), `rightx`, `righty` -1..1, `lefttrigger`, `righttrigger` 0..1. Dead zones apply as for a real pad. |

A button press or a non-zero axis on an empty slot creates an "injected" pad there (no SDL
device, generic family), so `connect` is only needed for a specific family. A release or a zero
axis on an empty slot is dropped: the second half of a `tap` sent after `disconnect` does not
bring the pad back. A pad of a recorded session comes back as
`connect` with its type and slot, so a real controller session replays on a virtual pad.

### Looking at the game

| Command | Example | Reply |
|---|---|---|
| `screenshot` | `{"cmd": "screenshot", "file": "build/shot.png"}` | Writes the last rendered frame. `"downsample": 4` averages 4 x 4 blocks (golden images). |
| `get` | `{"cmd": "get", "path": "ship:position"}` | `"value": [640, 400]` |
| `find` | `{"cmd": "find", "label": "ship"}` | `"found"`, `"type"`, `"position"`, `"world_position"`, `"bounds"`, `"visible"` |
| `list` | `{"cmd": "list", "component": "crate"}` | `"nodes": [{"label", "type", "position"}...]`; or `"type": "sprite"`; neither = the whole tree |
| `dump` | `{"cmd": "dump", "file": "build/state.json"}` | Frame, time, node count, exposed state, and the scene tree with every component (scene-file format). Without `"file"` the dump is in the reply. |
| `stats` | `{"cmd": "stats"}` | `"stats"`: frame, time, fps, nodes, draw_calls, textures, skipped_frames, heap_bytes, vsync, max_fps, deterministic |
| `logs` | `{"cmd": "logs", "count": 20}` | `"lines"`: the last log lines (128 are kept) |
| `profile` | `{"cmd": "profile", "frames": 300}` | Runs N frames (default 300), then `"profile"`: `br_profiler_summary_json` of them (`frames` = frame intervals measured, at most 300; `fps`, `frame_ms_avg`, `frame_ms_p95`, `frame_ms_p99`, `frame_ms_max`, `cpu_ms_avg`, `cpu_ms_p95`, `gpu_ms_avg` (`null` when unknown), `update_ms_avg`, `render_ms_avg`, `verdict`, `top_scopes`). Short form: `profile [N]`. Under `--deterministic` frames still take real time, so the numbers are real; for frame-rate checks run without `--headless`. |

### Changing and checking state

| Command | Example | Notes |
|---|---|---|
| `set` | `{"cmd": "set", "path": "game:lives", "value": 1}` | Forces a state that is hard to reach by playing. Node properties go through the scene-file reader, so every key a scene file accepts works. |
| `expect` | `{"cmd": "expect", "path": "game:score", "op": ">=", "value": 100}` | Fails the script (exit code 1) with `expect failed: game:score is 0, expected >= 100`. `"op"`: `==` (default), `!=`, `<`, `<=`, `>`, `>=`, `contains` (strings). Numbers compare with a 1e-4 relative tolerance; arrays element by element. |
| `expect` (log) | `{"cmd": "expect", "log": "round 1 over"}` | Some kept log line contains the text. Also for `wait_until`. |
| `log` | `{"cmd": "log", "message": "boss fight starts"}` | Writes `playtest: boss fight starts` to the log. |
| `quit` | `{"cmd": "quit"}` | Exits after this frame (a script also exits at its end). |
| `help` | `{"cmd": "help"}` | `"commands"`: `[{"cmd", "args", "doc"}...]`, the protocol for an agent. |

### Paths

| Path | Means |
|---|---|
| `ship:position` | Node labelled `ship` (searched in the whole stage), built-in property. Built-ins: `position`, `x`, `y`, `scale`, `rotation` (radians), `alpha`, `visible`, `tint`, `z_index`, `label`, `type`, `children` (count), `world_position`, `bounds` ([x, y, w, h], stage coordinates: window coordinates unless a 2D camera moves the stage), `exists`. |
| `final_score:text` | Any other key of the node's scene-file form (`text`, `texture`, `style`, ...). |
| `ship:pilot.carrying` | Component `pilot` of node `ship`, field `carrying` (fields listed in the game's `br_field_desc`). Enums read and write by name, text fields take strings of any length, arrays are lists (`ship:pilot.cargo.2` is one element). |
| `level_camera:camera.follow.offset3d.x` | A component whose name has a dot: after the colon the longest run of parts that names a registered component is the component (`camera.follow`), the next part its field, then an optional element. Names without a dot read as before. |
| `ship:position.x` | One element: `.x .y .z .w` (or `.0 .1 ...`). Works for `get` and `set`. |
| `game:score` | A struct the game exposed with `br_playtest_expose(e, "game", fields, count, &state)`. |
| `profile:frame_ms_p95` | A key of the last `profile` command's summary (any key listed above; `profile:*` = all). Use it in `expect`: `{"cmd": "expect", "path": "profile:frame_ms_p95", "op": "<", "value": 16.7}`, `{"cmd": "expect", "path": "profile:verdict", "op": "contains", "value": "vsync"}`. Before any `profile` it covers every frame kept. |
| `engine:scene` | Name of the active scene (`br_scene_active_name`, `""` when none): `{"cmd": "wait_until", "path": "engine:scene", "value": "play"}`. |
| `json_view/json_fox:position` | 3D node `json_fox` inside the `view3d` labelled `json_view` (`*/json_fox` = any view; an unlabelled view is `/json_fox`). Properties: `position`, `scale`, `rotation` (quaternion [x, y, z, w]), `euler` (radians, X then Y then Z), `visible`, `world_position`, `screen_position` (stage coordinates, `null` behind the camera), `label`, `type`, `children`, `exists`; `.x` elements as above. |

`type`, `children`, `world_position`, `bounds`, `screen_position` and `exists` are read-only.
3D nodes work with `get`, `set`, `expect`, `wait_until`, `find` and `click` (`{"cmd": "click",
"label": "/cube"}` clicks where the node's origin is on screen, so the view's 3D pointer events
fire; a node whose origin is not on its surface, such as a torus, is missed). `click` goes through
2D cameras: it aims at the window point where a camera shows the node (`br_camera_stage_to_screen`).
`dump` lists the
3D subtree of every view under `"views3d"`.

## A game's side: exposing state

Labels and components are visible without any code. Game state that lives outside components
(score, lives, the state machine) needs one call in `init`, with the same field lists as
components (Star Courier, `src/main.c`):

```c
static const br_field_desc fields[] = {
    { "state", BR_FIELD_STRING, offsetof(sc_game, state_name) },
    { "score", BR_FIELD_INT, offsetof(sc_game, score) },
    { "lives", BR_FIELD_INT, offsetof(sc_game, lives) },
};
br_playtest_expose(e, "game", fields, BR_ARRAY_COUNT(fields), g);
```

Games that read save files should skip them when `br_app_deterministic(e)` is true, so a test
run does not depend on what the last run saved (Star Courier does this for its best score).

## Turning a session into a regression test

1. Play it: `star_courier --record build/session.json` (play, then close the window). Or let a
   bot or another script provide the input: anything that arrives as input events is recorded.
2. Check the replay: `star_courier --playtest build/session.json` plays exactly the same game
   (input lands on the same frames; the run is deterministic).
3. Add checks: insert `expect` lines between the recorded commands (the file has one command
   per line), e.g. after the `wait` that reaches the game-over screen:
   `{"cmd": "expect", "path": "game:state", "value": "gameover"}`. A `screenshot` line before
   the final `quit` gives you an image to look at.
4. Commit it next to the game (`sample_projects/<game>/playtest/`) and register it with CTest
   (Star Courier's `CMakeLists.txt`: `add_test(... --headless --playtest .../round.json)`,
   label `playtest`). For pixels, add the game to `tools/golden.py` (see `docs/TESTING.md`).

Star Courier's `playtest/round.json` was made this way: its old autoplay bot's input, recorded,
plus checks on state, score, best score, lives, pause and the final score text.
`playtest/soak.json` is the 60-second soak (three rounds; compare the `round start` stats lines).

Crate Rush's `playtest/round.json` is hand-written instead, and walks to waypoints rather than
counting frames: hold a key (or a virtual pad's stick), `wait_until` the player's position passes
a coordinate (`*/player:position.x`), release, `expect` the crate count. Such a script survives
tuning changes. Its `playtest/soak.json` (written by `make_soak.py`) plays all three levels three
times and `expect`s node, 3D node, asset, animator, tween and sound counts back at their menu
baseline, and `profile:frame_ms_p95` under a budget.

A recorded session replays exactly only against the same game code and data. When gameplay
changes on purpose, record it again.

## C API (tests written in C, headless)

```c
br_engine *e = br_engine_create("my_test", NULL);
/* ... register the game's components and systems ... */
bool passed = br_playtest_run_script(e, "round.json");   /* steps until the script ends */
char *reply = br_playtest_command(e, "{\"cmd\":\"get\",\"path\":\"ship:x\"}");
br_free(reply);
br_playtest_step(e, 10);
cJSON *x = br_playtest_get(e, "ship:x");                 /* cJSON_Delete when done */
char *dump = br_playtest_dump_json(e);                   /* br_free */
```

Also `br_playtest_set`, `br_playtest_screenshot`, `br_playtest_record_start/stop`,
`br_playtest_last_error`. `tests/test_playtest.c` uses all of them.

## Limits

- **stdin is the one exception to "SDL only".** SDL cannot read the process's standard input, so
  `engine/src/playtest/br_playtest_stdin.c` uses C `<stdio.h>`; it is compiled only with the
  CMake option `BRATY_PLAYTEST_STDIN` (on for desktop, off for Android / iOS / Emscripten, where
  `--playtest-stdin` reports that it is unavailable). Script mode and the C API work everywhere.
- While `--playtest-stdin` waits for the next command the read blocks (the game is paused on
  purpose), so the window does not process events and Windows may label it "not responding";
  frames and screenshots are unaffected.
- Recording captures gamepad connect / disconnect, buttons and axes, not their touchpads,
  gyros or accelerometers. It records every mouse motion (with its relative delta) and stick
  event (large files for long sessions). SDL's mouse copies of touch and pen input are not
  recorded (the touch is), so a replay does not click twice.
- 2D labels are searched stage-wide (no `parent/child` for 2D): `docs/API_REVIEW.md` item 37.
  Labels in `view/node` paths are at most 63 characters each (longer ones fail "label too long").
- Log lines from any thread (asset workers) are captured; the 128-line ring is locked.
