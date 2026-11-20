# Profiling a Braty game

Is the game slow because of the CPU or the GPU, and where? Press **F2** in any Braty app (the
profiler panel), or run with `--profiler-hud` for a small corner readout:

```
FPS 27.6   CPU 2.80 ms   GPU 36.25 ms
GPU bound   render.collect
```

```sh
build/windows-gcc/profiler_demo.exe --vsync off --gpu-load 3000          # see GPU bound
build/windows-gcc/my_game.exe --vsync off --profiler-hud                 # corner readout only
build/windows-gcc/my_game.exe --vsync off --frames 600 --profile build/trace.json
```

`--profile FILE` writes, at exit, `FILE` (Chrome trace-event JSON: open it in
<https://ui.perfetto.dev> or `chrome://tracing`) and `FILE.txt`, a summary an agent can read:

```
braty profile: 299 frames, driver direct3d12, vsync off, cap 0
frame ms: avg 0.373  p95 0.517  p99 0.682  max 0.706  (2678.2 FPS)
cpu ms:   avg 0.200  p95 0.257  (update 0.008, render 0.191)
waits ms: acquire 0.018  present 0.143  fps cap 0.000
gpu ms:   avg 0.316 (estimated from fences over 298 frames)
verdict:  CPU bound (render submit)
top scopes (self ms per frame, total ms per frame, calls per frame):
  render.collect                      0.093    0.101    1.0
  render.submit                       0.079    0.079    1.0
  ...
```

(Star Courier's title screen, D3D12, RTX 3060 Ti, Debug build.) **Measure with `--vsync off`**:
with vsync on every frame waits for the display, so every game looks "vsync limited" and the
GPU number includes that wait.

API: `engine/include/braty/br_profiler.h`; agent guide: `docs/agents/profiler.md`.

## What is recorded

Every frame goes into a ring of the last 300 frames (5 s at 60 FPS, ~2.5 MB allocated once).
Each frame holds its scopes (start/end in ns, nesting), 30 counters and the timings below.

The engine wraps every system in a scope named after the system, and the steps of
`br_engine_step` in these **top-level scopes**:

| Scope | What runs inside |
|---|---|
| `pace` | the FPS-cap sleep (`br_app_iterate`, before the step) |
| `update` | INPUT, FIXED_UPDATE (0..8 times), UPDATE, LATE_UPDATE systems |
| `render.prepare` | PRE_RENDER systems: transforms, culling, `render.collect` (stage walk, batching, overlays incl. this UI as `render.overlays`) |
| `gpu.acquire` | acquiring the command buffer and the swapchain texture (blocks while too many frames are in flight, and for vsync) |
| `render` | RENDER and POST_RENDER systems: `render.submit` replays the draw list into SDL_GPU calls, 3D views draw |
| `gpu.present` | blit to the swapchain and `SDL_SubmitGPUCommandBuffer` (driver submit + present; blocks when the present queue is full) |

Your own scopes: `br_profile_begin(e, "name")` ... `br_profile_end(e)`, nested freely. Names
must be string literals (or `br_profiler_intern(e, buf)` for names built at runtime): they are
looked up by pointer. Up to 512 scopes per frame, 32 deep, 256 distinct names; more are counted
as "dropped" and shown in the panel. Scopes are main-thread only.

## What the numbers mean

| Number | Definition |
|---|---|
| frame ms | start of a frame to the start of the next (`1000 / FPS`); includes every wait |
| CPU ms | frame wall time minus the three waits (`gpu.acquire`, `gpu.present`, `pace`): the time the main thread worked |
| update / render | total of the `update` scope / of `render.prepare` + `render` |
| acquire / present / cap | the wait scopes above |
| GPU ms (est.) | GPU time of a frame estimated from fences, below |

The header and the corner readout average the last 30 frames. The scope tree shows, per call
path, self and total ms and calls of the selected frame, and min / avg / max of the total over the
30 frames ending there.

### The GPU estimate

SDL_GPU has no timer queries, so GPU time is measured from what is portable: every frame is
submitted with `SDL_SubmitGPUCommandBufferAndAcquireFence`, and a small thread polls the pending
fences with `SDL_QueryGPUFence` (non-blocking, no SDL lock on D3D12 or Vulkan; the game never
waits for it). The first moment a fence is seen signalled is that frame's completion, `done(i)`.
The GPU works through frames in submission order, so a frame can start when it was submitted
**and** the previous frame was finished:

```
gpu(i) = done(i) - max(submit(i), done(i - 1))
```

What this number is, case by case:

- **GPU bound, vsync off:** frames queue up, `start = done(i - 1)`, and `gpu(i)` is the time the
  GPU spent on frame i. It matches the frame interval (`--gpu-load 3000`: 36.25 ms GPU, 36.21 ms
  frame interval on D3D12; 31.05 / 31.05 ms on Vulkan).
- **CPU bound, vsync off:** the GPU idles between frames, `start = submit(i)`, and `gpu(i)` is
  submit-to-done, the GPU work plus the queue latency. The poll thread keeps watching while the
  main thread is busy, so a slow CPU frame does not inflate it (`--cpu-load 25`: 1.06 ms GPU in a
  26.4 ms frame).
- **Vsync on:** the command buffer can wait for a swapchain image before it runs (Vulkan does;
  D3D12 waits in acquire), so the estimate includes display waits. It is an upper bound and shown
  as `GPU <= x ms (est., incl. vsync)`; the verdict does not use it then.
- **Resolution:** the thread polls every 0.1 ms, which Windows rounds to ~0.5 ms
  (`test_profiler` prints the measured period: median 500 us). A late "done" also delays the
  next frame's start, so at thousands of FPS the estimate creeps up to the frame interval. The
  verdict ignores GPU estimates within 1 ms of the CPU time.
- Other command buffers (texture uploads submitted by asset loads) are not in the frame's fence
  and are not counted.

A frame's estimate arrives 1-3 frames after the frame (when its fence signals); the graph fills
the orange line in then. The fence per frame and the poll thread run only while something uses
the estimate: the panel or the HUD is shown, `--profile` was given, or a play-test `profile`
command is measuring (`br_profiler_gpu_estimating`). A game that never shows the profiler pays
nothing; closing the panel stops the thread (one wait for the GPU to go idle). The first frames
after opening the panel have no GPU number yet. A GPU-side breakdown (which pass costs what) needs timer queries, which
SDL 3.4 does not have.

## The verdict

Computed every frame from the averages of the last 30 frames
(`br_profiler_verdict_rule`, tested in `tests/test_profiler.c` for every outcome). With
`cpu = update + render`, `waits = acquire + present`, `near(a, b)` = within 10% of b:

1. **FPS cap limited** if a cap is set, `near(frame, cap period)` and the cap sleep is at least
   10% of the frame.
2. **vsync limited** if vsync waits for the display (on / adaptive), `near(frame, refresh
   period)` and `cpu < 0.9 x refresh period`: the game runs at the display rate with time to
   spare.
3. **GPU bound** if (vsync off and) `gpu > cpu + 1 ms`, or `waits > cpu` (the CPU spends more
   time waiting for the GPU than working; this also covers vsync on and missing fence data).
4. Otherwise **CPU bound (update)** if `update >= render`, else **CPU bound (render submit)**.

The shown verdict changes only after the new one held for 15 consecutive frames (no flicker);
during the first 10 frames it reads "measuring...". The **top three** are the scopes with the most
self time over those 30 frames, wait scopes excluded.

Measured (Debug build with the D3D12 debug layer / Vulkan validation as SDL enables them,
Windows 11, RTX 3060 Ti, 1280 x 720, `profiler_demo --vsync off --frames 300`, panel open):

| Run | Driver | frame ms | CPU ms (update / render) | waits ms | GPU ms (est.) | verdict |
|---|---|---|---|---|---|---|
| no load | direct3d12 | 0.77 | 0.60 (0.19 / 0.41) | 0.16 | 0.17 | CPU bound (render submit) |
| no load | vulkan | 0.73 | 0.61 (0.18 / 0.43) | 0.11 | 0.39 | CPU bound (render submit) |
| `--gpu-load 3000` | direct3d12 | 36.21 | 2.80 (0.50 / 2.29) | 33.36 | 36.25 | GPU bound |
| `--gpu-load 3000` | vulkan | 31.05 | 1.59 (0.35 / 1.23) | 29.44 | 31.05 | GPU bound |
| `--cpu-load 25` | direct3d12 | 26.42 | 25.95 (25.20 / 0.74) | 0.43 | 1.06 | CPU bound (update) |
| `--cpu-load 25` | vulkan | 26.25 | 25.93 (25.18 / 0.74) | 0.29 | 1.15 | CPU bound (update) |
| `--draw-load 2000` | direct3d12 | 24.05 | 23.41 (0.19 / 23.21) | 0.61 | 1.20 | CPU bound (render submit) |
| `--draw-load 2000` | vulkan | 63.63 | 62.57 (0.20 / 62.36) | 1.02 | 1.14 | CPU bound (render submit) |
| `--max-fps 30` | direct3d12 | 33.13 | 1.07 | 0.48 + cap 31.54 | 0.57 | FPS cap limited |
| `--max-fps 30` | vulkan | 33.13 | 1.44 | 0.44 + cap 31.20 | 0.82 | FPS cap limited |

`--draw-load` is ~12 us (D3D12) / ~31 us (Vulkan) per draw call in this Debug build: the
validation layers dominate; Release builds are far cheaper per draw. With vsync on, this machine's
session presented at ~4 FPS (the window was not on a visible desktop); the profiler then reads
"GPU bound" with `GPU <= 255 ms (incl. vsync)`, i.e. "the CPU waits on the GPU/display". Vsync
limited is covered by the unit test, not by this table.

## The panel (F2)

- Header: FPS, frame, CPU, GPU (est.), verdict and top three.
- Pause / `<` `>` (step through the captured frames) / Live; the inspected frame number.
- Frame graph, oldest left: grey bar = frame interval, green = CPU, orange = GPU (est.); lines
  at 16.6 and 33.3 ms, scale 0-40 ms. **Click a bar to pause on that frame** (it turns white).
- Stacked bar: the inspected frame split by top-level scope, with a legend in ms.
- Scopes tab: the scope tree (`+` / `-` opens a row; top level open by default).
- Counters tab: every counter of the inspected frame with a sparkline of the 120 frames before.
- Memory tab: heap (live, peak, allocations), every tracked arena (used / peak / capacity),
  cached assets grouped by loader type with their approximate bytes, loads in flight.

Clicks, wheel and typing that land on the panel (or any debug window) do not reach the game.
The corner readout is display-only: clicks go through it.

## Counters

Per frame; summed over the frame or sampled at its end.

| Counter | Source |
|---|---|
| draw calls, batches, vertices, passes, nodes drawn | `br_render_get_stats` (2D) |
| triangles | 2D batcher indices / 3 + custom draws |
| break: texture / blend / state / shader / other | why a 2D batch was closed: all texture slots used; blend mode changed; mask, scissor, stencil or transform changed; a custom-shader draw or render callback came in between; vertex limit or non-contiguous vertices |
| pipeline switches | pipeline binds in the 2D batcher's replay |
| target switches | render passes begun (every pass: 2D, filters, 3D, shadow) |
| uploads, upload bytes | `br_gpu` copy-pass uploads: textures, buffers, per-frame geometry |
| nodes, nodes culled | 2D nodes alive; subtrees skipped by culling in the stage walk |
| 3D nodes, 3D models | `br_node3d_count`; glTF model instances alive |
| audio voices | `br_audio_instance_count` |
| heap bytes, heap allocs | `br_memory_get_stats`: live bytes, `br_alloc` calls this frame (whole process) |
| assets, assets loading | `br_assets_count`, `br_assets_pending` |

Also counted (Phase 2 integration): `animators` and `tweens` alive (every animator and tween
alive is advanced each frame), `particles drawn` (particles of every particle_container drawn),
`3D items` (draw items in the main passes, every view), `3D items culled` (outside the camera
frustum) and `3D shadow casters`. Headless runs draw nothing, so the particle and 3D counters stay
0 there.

**Not measured yet:** pipeline binds outside the 2D batcher (3D, particles, filters), uploads
that bypass `br_gpu` (`br_render3d.c` instance data, compressed textures).

## Capture, summaries, budgets

- `br_profiler_save(e, path)` / `--profile path`: the ring as trace events. Thread 1 "CPU (main
  thread)": a `frame N` slice containing every scope as balanced `B`/`E` pairs; thread 2 "GPU
  (estimated from fences)": one slice per frame from its estimated start to done; counter tracks
  (`frame ms`, every counter). Relative paths are relative to the working directory.
- `br_profiler_summary(e, frames, buf, size)`: the text above for the last `frames` frames.
- `br_profiler_summary_json(e, frames, obj)`: the same as JSON members: `frames`, `fps`,
  `frame_ms_avg`, `frame_ms_p95`, `frame_ms_p99`, `frame_ms_max`, `cpu_ms_avg`, `cpu_ms_p95`,
  `update_ms_avg`, `render_ms_avg`, `gpu_ms_avg` (null if unknown), `verdict`, `top_scopes`
  (`name`, `self_ms`, `total_ms`, `calls` per frame). Percentiles are nearest-rank.
- `br_profiler_set_budget(e, "scope", ms)`: when the scope's total in a frame exceeds ms, one
  warning (`profiler: 'scope' took 6.000 ms, budget 5.000 ms (frame 0)`); it warns again only
  after 60 frames under budget. `ms <= 0` removes it.
- Play-tests: `{"cmd":"profile","frames":300}` waits 300 frames and replies with the JSON
  summary; `{"cmd":"expect","path":"profile:frame_ms_p95","op":"<","value":20}` checks it
  (wired in `br_playtest.c`). Play-tests run `--deterministic`, which fixes the simulation step,
  not the wall clock: the measured times are real.

## Overhead

`tests/test_profiler.c` runs 2000 headless frames with and without 200 scopes (100 pairs nested
two deep) and prints the difference: **~37 us per frame for 200 scopes, 0.22% of a 16.6 ms frame
(~185 ns per scope, including the frame-end bookkeeping: paths, counters, 30-frame window, top
three)**; it fails above 1%. The panel itself costs ~0.3 ms of CPU per frame while open
(`debug.ui` + `render.overlays`, visible in its own tree). Build with `BRATY_PROFILER=OFF`
(`-DBRATY_PROFILER_OFF`) and `br_profile_begin` / `br_profile_end` / `br_profiler_count` become
empty inline functions; frame times, the GPU estimate and the HUD keep working, the
update / render split does not (the verdict then says CPU bound (update) for any CPU-bound frame).

## Golden images and `--deterministic`

Under `br_app_deterministic(e)` every time in the panel and the corner readout is a fixed
placeholder (FPS 60.0, CPU 4.00 ms, GPU 2.00 ms, "vsync limited", 0.10 ms per scope, a fixed
graph), heap numbers too, so `tests/golden/profiler_demo.png` does not depend on the machine.
Counters other than the heap are deterministic and shown as measured.
