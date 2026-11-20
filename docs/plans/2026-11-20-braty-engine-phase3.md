# Braty Engine Phase 3: the larger changes from the full review

**Goal:** apply the eight "recommended larger changes" of `docs/REVIEW_2026-11-20.md` plus the
open items it lists, so the API has one way to do each thing and a game can ship: translated,
packed, with text entry.

**Spec:** `docs/REVIEW_2026-11-20.md` ("Recommended larger changes", "Found but not fixed"),
`docs/API_REVIEW.md` items 23, 24, 40, 42-52. Earlier plans' Global Constraints still apply.

## Global Constraints

- Everything in `AGENTS.md` "Code rules": C11, no clever macros, only SDL3 + C standard library,
  no platform headers or `#ifdef`s, `br_file_*` for files, errors return + log.
- Every behaviour change ships with a test that fails without it. Visible changes: golden image,
  looked at on `direct3d12` and `vulkan`. Golden tolerance stays strict.
- Values from data files are range-checked; bad data logs and fails, never crashes or hangs.
- API removals are real removals (no deprecated aliases): engine, examples, tests, both sample
  games, `tools/check_getting_started.py` and every doc move to the surviving API in the same
  commit series.
- New third-party code: none. (No ICU, no HarfBuzz, no zlib: see Task B2/B3 limits.)
- Docs: each task updates its module guide(s), header comments and `docs/DATA_FORMATS.md` rows;
  `tests/test_data_formats.c` must load every new documented example.

## Review Focus

1. A game written against a removed API: the compiler error must point at an obvious
   replacement (documented migration table in `docs/API_REVIEW.md`).
2. A translated string longer than any buffer, or with glyphs in no installed font: shows text
   (fallback font or the missing-glyph box), never truncates silently, never crashes.
3. A pack file that is truncated, has a wrong magic, or an entry pointing past the end: load
   fails with the pack name and entry; loose files still work.
4. Text field: IME composition, paste of 1 MB, invalid UTF-8 from the clipboard, caret movement
   over multi-byte and combining characters, focus lost mid-composition.
5. Old save data and old scene files keep loading after the component field changes.

## Order

`A -> {B1, B2, B3, B4, B5, B6 in parallel worktrees} -> C -> D`

## Task A: one API (serial, touches the whole repo)

API_REVIEW 42, 44, 45, 47, 49, 50 and 48.

- Remove duplicates: the legacy glTF animator (keep `br_animator`), the legacy orbit camera in
  favour of the `br_camera` orbit controller, the ticker's max-FPS setter in favour of
  `br_app_set_max_fps`, the second colour helper, the engine RNG wrappers in favour of
  `br_random` (the engine keeps one seeded `br_random` reachable through one accessor). Old
  names: the migration table in `docs/API_REVIEW.md`.
- Align 2D and 3D nodes: same verbs and argument order for create / destroy / parent / child
  iteration / find by label / visibility / layers / position-rotation-scale getters and setters /
  destroy listeners / counts. `br_node3d_*` mirrors `br_node_*`; where behaviour must differ the
  header says so in one line. Move `br_component3d_*` out of `br_gltf.h` next to the 2D
  component API.
- "scene" means one thing: the loadable unit (`br_scene.h`). The node-tree accessors are named
  for the stage (`br_stage_root` and friends).
- One getter naming rule (`br_<module>_<noun>` for getters, `_set_<noun>` for setters), one
  mutation style per module, `br_node_set_blend_mode` takes the enum.
- A migration table (old name -> new name) in `docs/API_REVIEW.md`; items marked done.

**Test:** the whole existing suite and both samples pass after the rename; a new
`tests/test_api_parity.c` exercises every mirrored 2D/3D call pair.

## Task B1: component field types, then localisation

- Field types (API_REVIEW 43): long text (heap string, any length), enum by name (table of
  names in the field description), vec4, quaternion, colour, a node reference by label (2D and
  3D), fixed-capacity arrays of any scalar type. All readable from scene files, prefabs, glTF
  extras, play-test `get` / `set`, the tween / animation property paths where numeric, and
  hot reload. Existing 63-byte string fields keep working.
- Localisation: `br_loc.h`. String tables as JSON assets (`strings.<locale>.json`, flat
  `"key": "text"` with `{name}` placeholders and plural forms `one` / `other`), loaded through
  the asset system, current locale from `SDL_GetPreferredLocales` with a game.json default and
  fallback order, `br_loc_get(e, key)`, `br_loc_format`, `br_loc_set_locale` (re-resolves every
  bound text node), text nodes and rich text take `"text_key"` in data, missing keys show the key
  and log once. Play-test: `get loc:<key>`, `set engine:locale`.

**Limits (documented):** no ICU plural rules beyond one / other / zero, no bidi, no shaping.

## Task B2: text: font fallback and the quad cache

- Fallback chain: a font (and each installed family) has an ordered list of fallback fonts;
  layout picks the first font that has the glyph, per glyph, for coverage, SDF and bitmap fonts;
  a missing-glyph box when none has it; settable in code and from game.json (`"fonts"`).
- Per-node quad cache (review item 8): glyph lookup and quad generation happen when text, style,
  font or atlas generation changes, not every frame; the frame cost of an unchanged text node is
  a memcpy / batch push. Measure before and after with 2,000 text nodes and record the numbers in
  `docs/agents/text.md`.

## Task B3: storage layer and asset packs

- One storage layer behind `br_file_*` (API_REVIEW 51): title storage (read-only game data),
  user storage (saves), and loose files for development; every path base is defined once and
  documented. No code outside it opens files.
- Pack format `.brpack`: header, table of contents (path, offset, size, hash), raw entries (no
  compression in this version). `tools/pack_assets.py` builds one from a data folder;
  the asset system mounts packs listed in game.json (`"packs"`) or passed with `--pack`, loose
  files override packed ones in development builds, async loads read from packs on the worker.
  Install rules in the sample CMake files to produce a shippable folder (exe + pack).
- Start-up errors visible to a player: a message box through SDL when the game cannot start, and
  a log file in user storage (release builds).

## Task B4: headless draw-list test harness

Review item 5. Draw items, instances, joints, light lists and shadow tiles are recorded into CPU
arrays at collect / submit and GPU data is allocated at render time, in both the 2D batcher and
the 3D renderer. A test API (`br_render_debug_*`, engine-internal header) reads them without a
GPU. New unit tests: 2D batching (texture limit, blend breaks, masks, layers, culling), 3D
frustum culling, opaque / transparent sort order, light selection, shadow cascade tiles,
instancing. Golden images must not change.

## Task B5: glTF cameras, View3D getters and the open small items

- API_REVIEW 23 + 24: a camera in a `.glb` scene imports as a `br_camera` that can follow,
  shake and be driven like any other, and the scene renders once; View3D settings are readable.
  Crate Rush drops its hidden-view workaround.
- API_REVIEW 46 and 52: pause is complete (tweens, animators, particles and audio follow system
  group pause or have an explicit time-scale / pause; documented which clock each uses), audio
  volume getters and a global pause.
- Review "found but not fixed": `br_asset_group_load` during an async load cancels `on_done`;
  the audio lock is held while Vorbis decodes; spot light inner == outer (prove with a test on
  the CPU side of the shader maths or guard it); `tools/playtest.py` rejects trailing garbage
  (API_REVIEW 40); effects inside a canvas3d captured subtree.

## Task B6: text-field node

`br_text_field.h`: a 2D node type (usable in canvas3d) with caret, selection (mouse drag,
shift + arrows, double-click word, select all), clipboard, undo of the last edit, max length,
single line and multi-line, password mode, placeholder, IME composition text and candidate
rectangle (`SDL_StartTextInput` / `SDL_SetTextInputArea`), gamepad / keyboard focus through the
existing focus rules, change / submit events, creatable from scene files, driven by play-test
(`type <label> "text"`). All editing is on UTF-8 code points; grapheme clusters are a documented
limit.

## Task C: samples and docs use it

- Crate Rush: strings from tables with a second language and a language row in settings; ships
  as exe + pack through the install rule; a name-entry text field on the results screen stored in
  save data; components use the new field types where they replace hand parsing.
- Star Courier: moved to the surviving APIs (done in Task A), strings from a table.
- `docs/GETTING_STARTED.md` gains steps for localisation, packing and a text field;
  `tools/check_getting_started.py` builds them. README status tables, AGENTS.md, DATA_FORMATS,
  API_REVIEW and the review summary updated; "Not implemented yet" shrinks to what is true.

## Task D: integration review (orchestrator)

Clean build, zero warnings, debug and release ctest, goldens, both samples played through the
play-test tool with screenshots on both drivers, banned-construct search, README limits checked
against reality.
