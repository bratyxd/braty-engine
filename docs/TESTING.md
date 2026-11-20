# Testing Braty

Three kinds of tests, one command:

```sh
cmake --build --preset windows-gcc
ctest --preset windows-gcc              # everything: unit tests, the play-test, golden images
ctest --preset windows-gcc -LE golden   # without the golden images (no windows open)
ctest --preset windows-gcc -L golden    # only the golden images
ctest --preset windows-gcc -L playtest  # only the play-test scripts
```

| Kind | Where | Needs |
|---|---|---|
| Unit tests | `tests/test_<module>.c`, one CTest each | nothing (headless engine) |
| Play-test scripts | `sample_projects/<game>/playtest/*.json`, label `playtest` | nothing (`--headless`) |
| Getting-started guide | `python tools/check_getting_started.py` (by hand, not in CTest: builds 13 programs) | a GPU for the screenshots |
| Golden images | `tools/golden.py` + `tests/golden/*.png`, test `golden`, label `golden` | a GPU; reported **skipped** without one |

**Rule (AGENTS.md):** every behaviour change ships with a test that fails without it. Logic gets
a unit test; anything visible gets a golden image.

## Unit tests

`tests/test_<module>.c` with a plain `main`, `BR_CHECK(cond)` from `tests/br_test.h`, and
`return br_test_finish();`. CMake picks every `tests/test_*.c` up; the working directory is
`tests/`. Use a headless engine (`br_engine_create("test", NULL)` + `br_engine_step`) or the
whole app loop headless (`br_app_init` with `--headless`, see `tests/test_app.c`). Write
temporary files next to the executable (`SDL_GetBasePath()`), not into the source tree.

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
quit. A pixel differs when any channel is off by more than 6; a case fails when more than 0.2%
of its pixels differ. On failure the new screenshot and a diff image (red = differing pixels over
a dimmed golden) are in `build/golden/` (`build/<preset>/golden/` when run from CTest). Exit code
77 (CTest: skipped) when no GPU device or window can be created.

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
Ti). Three D3D12 runs give byte-identical screenshots. Vulkan on the same machine matched 14 of
16 within the tolerance; `graphics_demo` (0.23% of pixels, antialiased edges) and `view3d_demo`
(0.84%, edge pixels and the skinned figure) differ by more, so Vulkan runs are informative, not
gating (measured with the first 16 cases; the two Crate Rush goldens match on Vulkan too: 0% and
0.026% of pixels). Other GPUs and drivers may need goldens of their own.

**Size.** 24 PNGs, RGB, 320 x 180, 776 KB in total. Keep new ones at this size.

## Determinism

`--deterministic` (implied by `--playtest` and `--playtest-stdin`) makes a run reproducible:
every frame steps exactly 1/60 s whatever the wall clock says, `br_engine_random` is seeded
(`--seed N`, default 1), asset loads skip the worker threads so callbacks fire in request order,
and play-test input lands on exact frames. Verified: two runs of Star Courier's `round.json`
give byte-identical screenshots and identical state dumps.
