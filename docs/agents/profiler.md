# profiler - CPU scopes, GPU estimate, counters, bottleneck verdict, capture

Header: `engine/include/braty/br_profiler.h`. Source: `engine/src/profiler/` (UI in
`engine/src/debug/br_debug_profiler.c`). Example: `examples/profiler_demo.c`. Test:
`tests/test_profiler.c`. What every number means and the verdict rule: `docs/PROFILING.md`.

## What it does

Every frame is recorded into a ring of the last 300 frames (no per-frame allocation): one scope
per engine system plus the frame steps (`update`, `render.prepare`, `gpu.acquire`, `render`,
`gpu.present`, `pace`), the scopes your code adds, 30 counters (draw calls, batch breaks by
reason, uploads, nodes, heap...), and a GPU frame time estimated from fences. From those it
says each frame what limits the frame rate: `CPU bound (update)`, `CPU bound (render submit)`,
`GPU bound`, `vsync limited` or `FPS cap limited`, with the three most expensive scopes.

F2 opens the panel (frame graph, stacked bar, scope tree, counters with sparklines, memory;
pause, step back, click a spike). `--profiler-hud` shows a corner readout instead.

## Functions you actually need

| Function | What for |
|---|---|
| `br_profile_begin(e, "name")` / `br_profile_end(e)` | Time your own code (nested; names are literals or `br_profiler_intern`ed). |
| `br_profiler_set_budget(e, "name", ms)` | Warn once when a scope takes longer than ms in a frame. |
| `br_profiler_get_summary(e, &s)` | FPS, CPU / GPU ms, verdict, top three scopes (last 30 frames). |
| `br_profiler_save(e, "out.json")` | Chrome trace (Perfetto) + `out.json.txt` summary. `--profile out.json` does it at exit. |
| `br_profiler_summary(e, frames, buf, size)` / `_summary_json(e, frames, obj)` | avg / p95 / p99 frame ms, verdict, top scopes as text or JSON. |
| `br_profiler_set_visible(e, true)`, `br_profiler_set_hud(e, true)` | Panel / corner readout from code. |
| `br_profiler_scope_time(e, age, "name", &total, &self, &calls)` | One scope's numbers in a recorded frame (tests). |
| `br_profiler_set_clock(e, fn, user)` | Inject timestamps (tests). |

## Snippet (from examples/profiler_demo.c)

```c
    br_profile_begin(e, "demo.move");
    for (uint32_t i = 0; i < BALLS; i++) {
        ...
    }
    br_profile_end(e);

    br_profiler_set_budget(e, "demo.cpu_load", 20.0f); /* logs once when a frame's busy loop exceeds 20 ms */
```

Command line (any app): `--profiler` (panel open), `--profiler-hud`, `--profile out.json`.

## Gotchas

- Scopes are main-thread only (others are ignored); a missing `br_profile_end` is closed at
  frame end with one warning, an extra one is ignored with one warning.
- Names are cached by pointer: pass literals, or `br_profiler_intern(e, buf)` for built names.
- The GPU number is an estimate (fence polling, ~0.5 ms resolution on Windows). With vsync on it
  also contains display waits and is shown as `GPU <= x ms`; measure GPU cost with `--vsync off`.
  The fence thread runs only while the panel / HUD / `--profile` / a play-test `profile` uses it
  (`br_profiler_gpu_estimating`); shipped games pay nothing.
- F2 is the debug module's key: `br_debug_set_profiler_key`, or `br_debug_set_keys_enabled(e,
  false)` / game.json `"debug": { "keys": false }` to switch F1 and F2 off for shipping.
- `--deterministic` (play-tests, goldens): the panel and HUD show fixed placeholder times.
- `-DBRATY_PROFILER_OFF` (CMake `BRATY_PROFILER=OFF`) makes scopes and counters empty inline
  functions; frame times, the GPU estimate and the HUD still work, the update/render split not.
- Counters cover the 2D batcher plus animators, tweens, particles drawn, 3D draw items, 3D items
  culled and 3D shadow casters; 3D / particle pipeline binds and uploads outside `br_gpu` are not
  counted (docs/PROFILING.md, "Not measured yet").
