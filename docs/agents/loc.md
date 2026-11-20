# loc - string tables, locale, placeholders, plural forms, bound text nodes

Public header: `engine/include/braty/br_loc.h`. Source: `engine/src/loc/br_loc.c`. Test:
`tests/test_loc.c`. File format: `docs/DATA_FORMATS.md` section 9.

## Model

- A table is an asset: `strings.<locale>.json` loaded by the `"strings"` loader (claims `.json`
  files named `strings.<something>.json`; `strings.json` stays a plain json asset). Asset =
  the parsed document plus a key map. Loads are forced to the strings loader and checked, so a
  broken table never comes back as another loader's asset.
- The current locale has a chain of up to 11 tables: the locale (`de-AT`), its language (`de`),
  the fallbacks, the default. Each exists-checked first (`br_assets_resolve` + `br_file_exists`),
  so a missing region file is not an error. `br_loc_set_locale` loads the new chain before
  releasing the old one (shared tables keep their reference) and fails, locale unchanged, when
  neither the locale nor its language loads.
- `br_loc_setup(e, dir, default, fallbacks, n)` (game.json `"locale"` calls it from
  `br_game_config_apply_settings`, before the start scene): the first `SDL_GetPreferredLocales`
  entry with a table (region form, then language), else the default. Deterministic, headless
  and `--frames` runs skip the system query: tests and golden images never depend on the
  machine's language.
- `br_loc_get` returns a pointer into the table (valid until the locale changes) or the key
  itself; a missing key logs once per locale (a map cleared on every locale change).
- `br_loc_format` writes into one module buffer (a `br_array` of char, any length): valid until
  the next call. `{name}` from args; plural form from the `count` argument.
- Bound text nodes: `br_loc_bind_text(e, n, key)` keeps a copied key per node index (with the
  generation); `br_node_on_destroy` frees it; every locale change re-sets each live bound node
  with `br_text_set_text` (works for text, bitmap text and rich text). Scene files call it for
  `"text_key"` (`br_node_json_text.c`), after `"text"`, so the key wins; the stored JSON keeps
  `text_key`, so saved files stay bound.
- Play-test: `get loc:<key>` (the whole rest of the path is the key), `get` / `set
  engine:locale` (`br_playtest_query.c`, after any exposed struct named `loc` / `engine`).

## Limits

- Plural rules are zero / one / other (no ICU: Russian or Arabic "few" / "many" use `other`).
- No bidirectional reordering and no shaping: Arabic, Hebrew and Indic scripts lay out as
  isolated code points left to right. Glyphs missing from the font are a text-module matter
  (font fallback, Phase 3 Task B2).
- One format buffer: `br_loc_format(a) ... br_loc_format(b)` in one expression overwrites `a`
  (copy the first result if you need both).
- Tables are not hot-reloaded: an edited file is read again once no locale holds it (`ponytail:`
  in `br_loc.c`).
