# app - engine context, systems, phases, ticker, entry point

Headers: `engine/include/braty/br_app.h`, `engine/include/braty/braty_main.h`.
Source: `engine/src/app/`. Example: `examples/hello_quad.c`. Test: `tests/test_core.c`.

## The model

A game is data plus systems. A system is a plain function `void fn(br_engine *e, float dt, void *user)`
registered with a phase and a priority. Every frame the engine runs:

| Phase | Typical work | Runs when minimised / headless |
|---|---|---|
| `BR_PHASE_INPUT` | read input state | yes |
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
| `br_system_add(e, name, phase, priority, fn, user)` | Register behaviour. |
| `br_system_set_enabled(e, name, on)` | Pause a system. |
| `br_engine_on_shutdown(e, fn, user)` | Free your state (runs before the GPU is destroyed). |
| `br_engine_add_event_handler(e, fn, user)` | Raw SDL events (input modules). |
| `br_ticker_delta/elapsed/fps`, `br_ticker_set_speed/max_fps/min_fps` | Time. |
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

`--frames N` (fixed 1/60 s steps, then exit 0), `--screenshot out.png` (last frame as PNG),
`--gpu-driver vulkan|direct3d12|metal`.

## Gotchas

- Include `braty_main.h` in exactly one `.c` file; it defines the SDL main callbacks.
- No globals: hang state off the `user` pointer, free it in a `br_engine_on_shutdown` callback.
- Module state lives in `struct br_engine` (`engine/src/app/br_engine_internal.h`): one pointer
  per module (`e->scene`, `e->render`, ...). A module defines its own `struct br_<module>_state`
  in its `_internal.h`, allocates it in `br_<module>_register(e)` (called from `br_builtin.c`)
  and frees it in an on-shutdown callback.
- `min_fps` (default 10) clamps `dt` so a stall does not produce one giant step; `speed` scales it.
- `max_fps` uses `SDL_HINT_MAIN_CALLBACK_RATE`; vsync is on by default (`br_gpu_set_vsync`).
