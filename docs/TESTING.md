# Testing Braty

Three kinds of tests, one command:

```sh
cmake --build --preset windows-gcc
ctest --preset windows-gcc              # everything: unit tests, the play-test, golden images
ctest --preset windows-gcc -LE golden   # without the golden images (no windows open)
ctest --preset windows-gcc -L golden    # only the golden images
ctest --preset windows-gcc -L playtest  # only the play-test scripts
ctest --preset windows-gcc -L headless  # only the examples run with --headless --frames 5
```

| Kind | Where | Needs |
|---|---|---|
| Unit tests | `tests/test_<module>.c`, one CTest each | nothing (headless engine); `test_gpu`, `test_debug` and one `test_profiler` case open a window and skip without a GPU |
| Headless examples | every `examples/*.c` as `headless_<name>` (`--headless --frames 5`), label `headless` | nothing: catches code that assumes a GPU (`br_engine_gpu` is NULL headless) |
| Play-test scripts | `sample_projects/<game>/playtest/*.json`, label `playtest` | nothing (`--headless`) |
| Getting-started guide | `python tools/check_getting_started.py` (by hand, not in CTest: builds 13 programs) | a GPU for the screenshots |
| Golden images | `tools/golden.py` + `tests/golden/*.png`, test `golden`, label `golden` | a GPU; reported **skipped** without one |

**Rule (AGENTS.md):** every behaviour change ships with a test that fails without it. Logic gets
a unit test; anything visible gets a golden image.

## Unit tests

`tests/test_<module>.c` with a plain `main`, `BR_CHECK(cond)` from `tests/br_test.h`, and
`return br_test_finish();`. CMake picks every `tests/test_*.c` up; the working directory is
`tests/`. Test data is read from the source tree through `BRATY_SOURCE_DIR` (set by CMake:
`BRATY_SOURCE_DIR "/tests/data"`), never relative to the executable, so any build directory
works. Use a headless engine (`br_engine_create("test", NULL)` + `br_engine_step`) or the
whole app loop headless (`br_app_init` with `--headless`, see `tests/test_app.c`). Write
temporary files next to the executable (`SDL_GetBasePath()`), not into the source tree.

Wall-clock checks (`test_pacing`'s FPS cap, `test_profiler`'s scope overhead) print the measured
number and assert only a gross bound (cap: not faster than the target, at most 25 % slower;
200 scopes: under 1 ms), so a loaded CI runner does not fail them. Read the printed numbers when
you change pacing or the profiler.

**Release build.** `ctest --preset windows-gcc-release` runs the same tests optimised with
`NDEBUG`, `BRATY_PROFILER=OFF` and `BRATY_PLAYTEST_STDIN=OFF`; the profiler-only tests and the
`--playtest-stdin` session are compiled out or skipped there, and so is the `profiler_demo`
golden. CI runs it in a build directory of a different depth.

## Play-test scripts

A script is a recorded or hand-written list of inputs plus `expect` checks; it is the way to
test game flow (title -> play -> game over), not just functions. How to write and record one:
`docs/PLAYTESTING.md`, "Turning a session into a regression test". Register it in the game's
`CMakeLists.txt`:

```cmake
add_test(NAME my_game_round COMMAND my_game --headless --playtest ${CMAKE_CURRENT_SOURCE_DIR}/playtest/round.json)
set_tests_properties(my_game_round PROPERTIES LABELS playtest)
```

## Golden images

```sh
python tools/golden.py run                         # compare everything with tests/golden/
python tools/golden.py run --only text_demo        # one case
python tools/golden.py run --gpu-driver vulkan     # the other backend (see below)
python tools/golden.py update --only text_demo     # re-create one golden
```

Each case (the `CASES` table in `tools/golden.py`) runs an example or the sample game through a
generated play-test script under `--deterministic --vsync off`: wait N frames (Star Courier:
replay `round.json`'s input up to frame N), take a screenshot averaged down 4 x 4 (320 x 180),
quit. A pixel differs when any channel is off by more than 2; a case fails when **any** pixel
differs (why so tight: "Driver" below). On failure the new screenshot and a diff image (red = differing pixels over
a dimmed golden) are in `build/golden/` (`build/<preset>/golden/` when run from CTest). Exit code
77 (CTest: skipped) when no GPU device or window can be created or the window cannot be claimed
for the GPU (what `SDL_VIDEODRIVER=dummy` gives); exit code 2 when an `--only` name is not a case
(a typo used to run nothing and pass).

**Add a golden** for a new example: add a line to `CASES` (name, executable, frame, extra
arguments, optional base script), `python tools/golden.py update --only <name>`, open the PNG in
`tests/golden/`, commit it. Keep scenes deterministic: no wall-clock time on screen (hide it when
`br_app_deterministic(e)` is true, as `gltf_demo` does), randomness from a seeded generator
(`br_engine_random` or the game's own).

**When an update is legitimate:** the change is *meant* to change the picture (a new feature, a
fixed rendering bug, a changed example) and you have looked at the new PNG and the diff image and
they show exactly that change. Never update to make a red test green without looking; an
unexplained difference is a regression until proven otherwise. Commit updated goldens in the same
commit as the change that caused them and say why in the message.

**Driver.** The goldens are made with **Direct3D 12** on the dev machine (Windows 11, RTX 3060
Ti), and CTest's `golden` test passes `--gpu-driver direct3d12` on Windows instead of taking
whatever SDL picks. Vulkan screenshots are *not* identical; Vulkan runs are informative, not
gating, and other GPUs and drivers may need goldens of their own.

**Tolerance, measured 2026-11-20** (downsampled pixels, worst channel difference; the 24 cases of
the first measurement, re-run with all 27):

| Run against the goldens | Worst difference | Cases with a pixel off by more than 2 |
|---|---|---|
| Direct3D 12, three runs | 0 (one run: 1 in `canvas3d_demo`) | none (after updating `profiler_demo`, below) |
| Direct3D 12, one run of all 27 cases | 0 | none |
| Vulkan, one run (27 cases) | up to 29 | 10: `view3d_demo` 993 px, `filters_demo` 303, `canvas3d_demo` 141, `graphics_demo` 128, `crate_rush_play` 52, `gltf_demo` 50, `animation_demo` 6, `gltf_scene_demo` 5, `text_demo` 2, `crate_rush_menu` 2 |

Direct3D 12 is deterministic, so the check is the tightest value that is stable there: channel
tolerance 2, zero pixels over it. The previous rule (6 per channel, 0.2 % = 115 pixels per case)
let a changed glyph or HUD digit (tens of downsampled pixels) pass; it also hid a real 8-pixel
change in `profiler_demo` (the panel's scrollbar thumb got shorter as systems were added), found
while measuring this. Vulkan's differences are rasterisation (1-px line lists, antialiased edges,
bloom around thin lines), not missing content. If a driver update makes D3D12 drift by a pixel or
two, look at the diff, then raise `MAX_BAD_PIXELS` in `tools/golden.py` by that much, not the
channel tolerance.

**Size.** 27 PNGs, RGB, 320 x 180, about 790 KB in total. Keep new ones at this size.

## Determinism

`--deterministic` (implied by `--playtest` and `--playtest-stdin`) makes a run reproducible:
every frame steps exactly 1/60 s whatever the wall clock says, `br_engine_random` is seeded
(`--seed N`, default 1), asset loads skip the worker threads so callbacks fire in request order,
and play-test input lands on exact frames. Verified: two runs of Star Courier's `round.json`
give byte-identical screenshots and identical state dumps.
