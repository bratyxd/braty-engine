# app - engine context, systems, phases, ticker, entry point

Headers: `engine/include/braty/br_app.h`, `engine/include/braty/braty_main.h`.
Source: `engine/src/app/`. Example: `examples/hello_quad.c`. Tests: `tests/test_core.c`,
`tests/test_app.c` (the app loop headless, boot options, groups, `--deterministic`),
`tests/test_pacing.c` (FPS cap accuracy, fixed timestep, vsync settings).

## The model

A game is data plus systems. A system is a plain function `void fn(br_engine *e, float dt, void *user)`
registered with a phase and a priority. Every frame the engine runs:

| Phase | Typical work | Runs when minimised / headless |
|---|---|---|
| `BR_PHASE_INPUT` | read input state | yes |
| `BR_PHASE_FIXED_UPDATE` | physics / simulation at a fixed rate: 0..8 runs per frame with dt = 1/hz (`br_app_set_fixed_timestep`); once per frame with the frame's dt when no rate is set | yes |
| `BR_PHASE_UPDATE` | game logic | yes |
| `BR_PHASE_LATE_UPDATE` | cameras, follow logic | yes |
| `BR_PHASE_PRE_RENDER` | transforms, culling, build draw lists, write transient geometry | yes |
| `BR_PHASE_RENDER` | upload + render passes into the frame target | no |
| `BR_PHASE_POST_RENDER` | overlays (debug UI) | no |

Within a phase: lower priority first, then adding order. Systems added while systems run start
next frame.

## Functions you actually need

| Function | What for |
|---|---|
| `br_main(argc, argv)` | The game's only required function; returns a `br_app_desc`. |
| `br_game_config_main("game.json", argc, argv, init)` | The whole `br_main` for a data-driven game (game.json applied before init, start scene after). |
| `br_engine_app_user(e)`, `br_engine_args(e, &argc)` | `br_app_desc.user` and the command line, inside init and systems. |
| `br_system_set_group(e, name, "gameplay")`, `br_system_group_set_enabled(e, "gameplay", false)` | Pause a group of systems (UI keeps running). |
| `br_app_set_vsync(e, BR_VSYNC_OFF/ON/ADAPTIVE/MAILBOX)` | Present mode; unsupported modes fall back with a log line (SDL GPU has no adaptive: it becomes on). |
| `br_app_set_max_fps(e, fps)` | FPS cap (0 = off), works with vsync off: deadline + `SDL_DelayPrecise` (1 ms sleeps, spin for the rest). |
| `br_app_set_fixed_timestep(e, hz)`, `br_app_fixed_alpha(e)` | Fixed-rate `FIXED_UPDATE`; alpha interpolates rendering between steps. |
| `br_app_deterministic(e)`, `br_engine_random(e)` | Reproducible runs; the seeded engine RNG. |
| `br_engine_screenshot(e, path)` | Last rendered frame as PNG. |
| `br_system_add(e, name, phase, priority, fn, user)` | Register behaviour; false (logged) for a duplicate name, NULL fn or bad phase. |
| `br_system_set_enabled(e, name, on)` | Pause a system. |
| `br_system_remove(e, name)` | Remove a system (safe inside a frame: it stops at once, leaves the list after the frame). |
| `br_engine_on_shutdown(e, fn, user)` | Free your state (runs after every node was destroyed, before the GPU is destroyed). |
| `br_engine_add_event_handler(e, fn, user)` | Raw SDL events (input modules). |
| `br_ticker_delta/elapsed/fps`, `br_ticker_set_speed/max_fps/min_fps` | Time. |
| `br_ticker_unscaled_delta(e)` | The frame's dt without the ticker speed: pause menus and UI animate with it while `br_ticker_set_speed(e, 0)` freezes the game. |
| `br_app_on_lifecycle(e, fn, user)`, `br_app_is_active(e)`, `br_app_is_paused(e)` | Focus / minimise / background: show a pause menu on `DEACTIVATED`, save on `TERMINATING`, drop caches on `LOW_MEMORY`. |
| `br_app_set_pause_when_inactive(e, on)` | Default on: while inactive, dt is 0 and audio output is paused (game.json `window.pause_when_inactive`, `br_app_desc.run_when_inactive`). |
| `br_engine_window_size`, `br_engine_pixel_size`, `br_engine_pixel_density` | Layout vs pixels. |
| `br_engine_create(title, asset_root)` + `br_engine_step(e, dt)` | Headless engine for tests. |

## Snippet (from examples/hello_quad.c)

```c
#include "braty/braty_main.h"

static void update_system(br_engine *e, float dt, void *user)
{
    (void)e;
    hello_state *s = user;
    s->angle += dt * 0.5f;
}

static bool hello_init(br_engine *e)
{
    hello_state *s = br_alloc(sizeof(hello_state));
    br_engine_on_shutdown(e, hello_shutdown, s);
    br_system_add(e, "hello.update", BR_PHASE_UPDATE, 0, update_system, s);
    br_system_add(e, "hello.render", BR_PHASE_RENDER, 0, render_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - hello quad";
    desc.init = hello_init;
    return desc;
}
```

## Command line (every app)

`--frames N` (fixed 1/60 s steps, then exit 0; logs the measured frame interval), `--screenshot
out.png` (last frame as PNG), `--gpu-driver vulkan|direct3d12|metal`, `--headless` (no window or
GPU), `--assets DIR` / `BRATY_ASSETS` (asset root, working-directory relative), `--vsync
off|on|adaptive|mailbox`, `--max-fps N`, `--deterministic [--seed N]`, `--playtest FILE`,
`--playtest-stdin`, `--record FILE` (docs/PLAYTESTING.md). The command line overrides game.json.

Measured FPS cap accuracy (RTX 3060 Ti, D3D12, `hello_quad --vsync off --max-fps N --frames
250`, first 10 frames excluded): 30 -> 33.333 ms average (max 33.97), 60 -> 16.667 ms (max
17.17), 144 -> 6.944 ms (max 7.30); headless `test_pacing` over 120 frames: 0.000% / 0.001%
average error at 60 / 144.

## Gotchas

- Include `braty_main.h` in exactly one `.c` file; it defines the SDL main callbacks.
- No globals: hang state off the `user` pointer, free it in a `br_engine_on_shutdown` callback
  (or `desc.shutdown`). Exit order: every 3D and 2D node is destroyed first (their destroy
  listeners still see your state), then `desc.shutdown`, then shutdown callbacks in reverse order
  of adding, then the GPU and window.
- Module state lives in `struct br_engine` (`engine/src/app/br_engine_internal.h`): one pointer
  per module (`e->scene`, `e->render`, ...). A module defines its own `struct br_<module>_state`
  in its `_internal.h`, allocates it in `br_<module>_register(e)` (called from `br_builtin.c`)
  and frees it in an on-shutdown callback.
- `min_fps` (default 10) clamps `dt` so a stall does not produce one giant step; `speed` scales it
  (negative or NaN speed is clamped to 0 = paused).
- Pause menus: switch the gameplay group off (`br_system_group_set_enabled`) or set the ticker
  speed to 0, and animate the menu with `br_ticker_unscaled_delta(e)` instead of the system's
  `dt` (which is scaled). Tweens, animators and particles are engine systems that use the scaled
  dt, so speed 0 freezes them too.
- Lifecycle: the app is active while its window is focused, not minimised and not in the
  background (SDL focus / minimise / restore and will/did-enter-background / foreground events,
  handled in `br_app_event`). Going inactive calls the lifecycle callbacks with `DEACTIVATED`;
  with `pause_when_inactive` (default) every system keeps running with dt 0 (FIXED_UPDATE does
  not step, elapsed stops, the loop naps 16 ms per frame) and the audio device is paused; coming
  back resumes without one giant dt. Never in `--deterministic`, `--frames` or headless runs.
  `LOW_MEMORY` and `TERMINATING` are passed to the callbacks as they come.
- Vsync (default on) and the FPS cap are independent. With vsync on and a cap below the refresh
  rate the cap wins; above it the display does. The cap schedules deadlines, so a late frame is
  not followed by a rushed one; more than a whole frame late restarts the schedule.
- A fixed timestep drops the backlog beyond 8 steps in one frame (no spiral of death); render
  with `br_app_fixed_alpha` to interpolate between the last two simulation states.
- `--deterministic` steps 1/60 s per frame and loads assets synchronously; wall-clock values on
  screen (timings, FPS) must be hidden then, or golden images differ (see `gltf_demo`).
- `--headless` runs every system with no window and no GPU (RENDER / POST_RENDER skipped, no
  screenshots). Every example runs under it; the ones that talk to the GPU directly
  (`hello_quad`, parts of `sprites_demo` and `text_demo`) log that they skip drawing. Audio opens
  no device: under `--deterministic` sounds advance by game time (`docs/agents/audio.md`).
  Cameras need a screen size: `br_camera_set_headless_screen(e, w, h)`.
