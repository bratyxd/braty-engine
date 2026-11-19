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
    game.action("start")                # the "start" action from game.json (Return)
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
| `--record session.json` | Plays normally and writes the real keyboard / mouse / text / touch input as a script at exit. |

`--playtest` and `--playtest-stdin` imply `--deterministic` (every frame is exactly 1/60 s, the
engine RNG is seeded, asset loads complete in order) and `--vsync off` unless `--vsync` is given,
so the same commands give the same frames, pixel for pixel. Add `--headless` to run without a
window or GPU (no screenshots); `ctest` runs Star Courier's round that way. Paths in commands
(screenshots, dumps, scripts) are relative to the working directory.

Replies always start with `"ok"` and `"frame"`; a failed command adds `"error"` and the game
keeps running. Engine logs go to stderr, replies to stdout.

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
| `mouse` | `{"cmd": "mouse", "action": "tap", "x": 640, "y": 360}` | `move`, `down`, `up`, `tap`, `wheel` (`"dx"`, `"dy"`); `"button"`: 1 left, 2 middle, 3 right. Window coordinates. |
| `click` | `{"cmd": "click", "label": "play_button"}` | Moves the mouse to the centre of the node's bounds and taps the left button. |
| `text` | `{"cmd": "text", "text": "PLAYER 1"}` | One text-input event, as an IME would send. |
| `touch` | `{"cmd": "touch", "action": "down", "finger": 0, "x": 100, "y": 200}` | `down`, `move`, `up`; window coordinates. |
| `gamepad` | | Not available yet: `br_input` has no gamepad injection (Task 14c adds it). |

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

### Changing and checking state

| Command | Example | Notes |
|---|---|---|
| `set` | `{"cmd": "set", "path": "game:lives", "value": 1}` | Forces a state that is hard to reach by playing. Node properties go through the scene-file reader, so every key a scene file accepts works. |
| `expect` | `{"cmd": "expect", "path": "game:score", "op": ">=", "value": 100}` | Fails the script (exit code 1) with `expect failed: game:score is 0, expected >= 100`. `"op"`: `==` (default), `!=`, `<`, `<=`, `>`, `>=`, `contains` (strings). Numbers compare with a 1e-4 relative tolerance; arrays element by element. |
| `expect` (log) | `{"cmd": "expect", "log": "round 1 over"}` | Some kept log line contains the text. Also for `wait_until`. |
| `log` | `{"cmd": "log", "message": "boss fight starts"}` | Writes `playtest: boss fight starts` to the log. |
| `quit` | `{"cmd": "quit"}` | Exits after this frame (a script also exits at its end). |

### Paths

| Path | Means |
|---|---|
| `ship:position` | Node labelled `ship` (searched in the whole stage), built-in property. Built-ins: `position`, `x`, `y`, `scale`, `rotation` (radians), `alpha`, `visible`, `tint`, `z_index`, `label`, `type`, `children` (count), `world_position`, `bounds` ([x, y, w, h], window coordinates), `exists`. |
| `final_score:text` | Any other key of the node's scene-file form (`text`, `texture`, `style`, ...). |
| `ship:pilot.carrying` | Component `pilot` of node `ship`, field `carrying` (fields listed in the game's `br_field_desc`). |
| `ship:position.x` | One element: `.x .y .z .w` (or `.0 .1 ...`). Works for `get` and `set`. |
| `game:score` | A struct the game exposed with `br_playtest_expose(e, "game", fields, count, &state)`. |

`type`, `children`, `world_position`, `bounds` and `exists` are read-only. 3D nodes inside a
`view3d` are not addressable yet.

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
- Recording does not capture gamepads (they cannot be injected yet), and records every mouse
  motion event (large files for long mouse-heavy sessions).
- The current scene name is not reported: scenes become objects in Task 16.
