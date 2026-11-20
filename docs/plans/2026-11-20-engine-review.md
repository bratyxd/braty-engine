# Braty Engine full code review plan (2026-11-20)

**Goal:** read every line of engine code with fresh eyes, find what is wrong, missing or better
done differently, then fix it with tests and bring the docs in line.

**Scope:** `engine/` (sources, public headers, HLSL), `tests/`, `tools/`, `CMakeLists.txt`,
`CMakePresets.json`, CI files, `docs/`, `AGENTS.md`, `README.md`. Third-party code is out of
scope except how we call it. Sample games only as API consumers.

## Stages

1. **Review (read-only, 8 Opus 5.5 reviewers in parallel, one slice each).** A reviewer reads its
   whole slice, not a sample of it. It may build and run anything and may write throwaway repro
   programs under `build/review/` (gitignored). It changes no tracked file and makes no commit.
2. **Triage (orchestrator).** Merge the eight reports, drop duplicates, check each high-severity
   claim against the code, decide fix / document / reject.
3. **Fix (Opus 5.5 implementers).** Every accepted bug is first reproduced by a test that fails,
   then fixed. A finding that cannot be made to fail is reported back as not reproduced, not
   "fixed". Docs and guides are updated in the same commit as the behaviour.
4. **Verify (orchestrator).** Clean build, zero warnings, all CTest tests, goldens, both sample
   play-tests, screenshots on both GPU drivers.

## Slices

| # | Slice | Directories |
|---|---|---|
| R1 | Foundation | `engine/src/core`, `app`, `gpu`, `math` + their headers |
| R2 | 2D scene and drawing | `scene`, `render`, `sprites`, `mesh`, `particles`, `filters` + HLSL they use |
| R3 | Assets and data | `assets`, `scenes`, `data` (scene files, prefabs, game config, glTF-as-scene) |
| R4 | Graphics and text | `graphics`, `text` (skip generated font tables) |
| R5 | Interaction and tooling | `events` (input, actions, gamepads), `audio`, `playtest`, `debug`, `profiler` |
| R6 | 3D | `three_d` (views, meshes, materials, lights, shadows, post, glTF import), `camera`, `canvas3d` + HLSL |
| R7 | Animation | `animation` (clips, graphs, tweens, skinning path, events, root motion) |
| R8 | Whole-engine view | public headers as one API, docs vs code, test gaps, build / CI / portability, missing features |

## Checklist every reviewer applies (R1-R7)

1. **Memory safety:** bounds on every index from data or user input; integer overflow in size
   maths (`count * size`); use after free / dangling pointers into arrays that can grow
   (a pointer kept across a call that may `realloc` the pool); double free; leaks on every error
   path; uninitialised reads.
2. **Handles and lifetime:** stale `br_node` / `br_node3d` / asset handles; destroy during
   iteration; callbacks that destroy their own node; shutdown order; reference counts balanced.
3. **Hostile or broken data:** truncated / malformed JSON, glTF, images, fonts, audio; missing
   fields; wrong types; huge counts; cycles (prefab includes itself, node parented to its child).
   Bad data must log and fail, never crash or hang.
4. **Logic:** off-by-one, wrong operator, wrong unit (degrees / radians, pixels / units, seconds /
   ms), float compare, NaN / division by zero (zero scale, zero size, dt = 0, zero-length vectors),
   precision over long run times (hours of accumulated `float` time).
5. **Determinism:** anything in a `--deterministic` run that depends on wall clock, pointer
   values, uninitialised memory, hash order or thread timing.
6. **Threads:** data touched from the audio callback or asset worker threads without a lock or
   atomic; SDL calls made off the main thread that must not be.
7. **GPU:** resources released while still used by a submitted command buffer; missing release;
   pass / copy rules of SDL_GPU (no upload inside a render pass, cycle flags); vertex layout vs
   shader input; fragment input struct equal to vertex output; sRGB / premultiplied alpha
   consistency; depth convention; behaviour when the swapchain is unavailable (minimised).
8. **Performance traps:** O(n^2) per frame, per-frame heap allocation in hot loops, tree walks
   where a dense pool exists, repeated string compares / lookups per element.
9. **Code rules (AGENTS.md):** no clever macros, no platform headers or `#ifdef`s, no `fopen`,
   errors return + log, public functions documented, naming. Flag code a reader must decode.
10. **API design:** inconsistent argument order or naming between sibling functions, missing
    getter for a setter, missing destroy for a create, values that cannot be set from data,
    surprising defaults, things a game needs but cannot do.
11. **Tests:** which behaviours in the slice have no test that would fail if they broke.
12. **Docs:** every statement in the slice's `docs/agents/<module>.md` and its header comments
    checked against the code; snippets must be real.

## R8 specifics

- Read all of `engine/include/braty/*.h` as one API: consistency across modules, leftovers,
  duplicates (two ways to do one thing), items in `docs/API_REVIEW.md` still open and which are
  worth fixing now.
- Feature maps in both plans vs what exists: what PixiJS v8 / `@pixi/3d` feature or Phase 2
  requirement is missing, half-done or only claimed. What a small shipped game would hit first
  (e.g. save data, localisation, window / display handling, pause on focus loss, text input / IME,
  touch, HiDPI, error reporting, packaging assets).
- `README.md`, `AGENTS.md`, `docs/*.md`: wrong, stale or contradictory statements; commands that
  do not run as written.
- `CMakeLists.txt`, presets, CI workflow, `tools/*.py`: portability (paths, case sensitivity,
  MSVC flags, Clang warnings), console-readiness (anything that assumes a filesystem layout,
  desktop windowing or a writable working directory).
- `tests/`: weak tests (assert nothing, pass when the feature is broken), missing areas, golden
  cases that could hide regressions (too tolerant, wrong frame).

## Finding format

Each finding, most severe first:

```
[S1|S2|S3|S4] <module> file:line - one-line title
What: what is wrong, concretely.
Trigger: the input / sequence that shows it (or "by reading" if not run).
Confirmed: yes (ran it: how) | no (reasoned from code)
Fix: the smallest change that fixes it. Test: what test would catch it.
```

- **S1** crash, memory corruption, data loss, hang, wrong output in normal use.
- **S2** wrong behaviour in a reachable edge case, leak, determinism break, thread race.
- **S3** API / design problem, performance trap, missing test for important behaviour, wrong doc.
- **S4** nit (style, naming, small cleanup).

Rules for reviewers: no finding without a file and line. Say "confirmed: no" honestly; do not
pad. S4 capped at ten per report. End with "Read in full:" (list of files) and "Not read:" (what
and why), then up to five "would be better to change" design suggestions with the reason.
Write the full report to `build/review/R<n>.md` and return a summary under 400 words.
