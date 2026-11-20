/* br_loc.h - localisation: string tables, the current locale, placeholders, plural forms and
 * text nodes that follow the locale.
 *
 * A table is a JSON asset named strings.<locale>.json ("strings/strings.de.json"): a flat object
 * of "key": "text", where a text may hold {name} placeholders, or a plural entry
 * { "one": "{count} crate", "other": "{count} crates", "zero": "no crates" }.
 *
 *   br_loc_setup(e, "strings", "en", NULL, 0);           (game.json "locale" does this)
 *   br_text_set_text(e, title, br_loc_get(e, "menu.title"));
 *   br_loc_arg args[] = { { "count", "3" }, { "name", "Ann" } };
 *   br_text_set_text(e, hud, br_loc_format(e, "hud.crates", args, 2));
 *   br_loc_bind_text(e, label, "menu.play");            (re-set on every br_loc_set_locale)
 *
 * Lookups try the current locale ("de-AT"), its language ("de"), the fallbacks in order, then
 * the default. A missing key shows the key itself and is logged once per locale.
 * Limits: plural forms are zero / one / other only (no ICU rules: "few" / "many" languages use
 * other), no bidirectional text, no shaping (br_text lays out code points left to right).
 */
#ifndef BR_LOC_H
#define BR_LOC_H

#include "braty/br_core.h"
#include "braty/br_node.h"

#define BR_LOC_NAME_SIZE 32   /* a locale name: letters, digits, '-' and '_' ("pt-BR") */
#define BR_LOC_MAX_FALLBACKS 8

/* One {name} placeholder value. Plural entries pick their form from the argument "count". */
typedef struct br_loc_arg { const char *name; const char *value; } br_loc_arg;

/* Registers the module (called by the engine) and the "strings" asset loader. */
void br_loc_register(br_engine *e);

/* Sets the table directory ("" = the asset root), the default locale and the fallback order,
 * then picks the current locale: the first of the system's preferred locales
 * (SDL_GetPreferredLocales) that has a table, else the default. Deterministic, headless and
 * --frames runs always start on the default so tests do not depend on the machine. False
 * (logged) if the default has no table or an argument is invalid. */
bool br_loc_setup(br_engine *e, const char *dir, const char *default_locale, const char *const *fallbacks,
                  uint32_t fallback_count);
/* Switches the locale (loads its tables, releases the old ones) and re-sets every bound text
 * node. False (logged, locale unchanged) if neither the locale nor its language has a table. */
bool br_loc_set_locale(br_engine *e, const char *locale);
/* The current locale ("" before one is set). */
const char *br_loc_locale(br_engine *e);
/* The text of key in the current locale chain (a plural entry: its "other" form), or the key
 * itself when missing (logged once). Valid until the locale changes. */
const char *br_loc_get(br_engine *e, const char *key);
/* True if some table of the current chain has key. */
bool br_loc_has(br_engine *e, const char *key);
/* br_loc_get with {name} placeholders replaced from args (unknown ones are kept as written) and,
 * for a plural entry, the form for the "count" argument (0: zero, else 1: one, else other).
 * Any length: the result lives in a buffer of the module, valid until the next br_loc_format. */
const char *br_loc_format(br_engine *e, const char *key, const br_loc_arg *args, uint32_t arg_count);
/* Sets a text / bitmap text / rich text node's text to key's text now and after every locale
 * change (key copied; NULL or "" unbinds). Scene files do this for "text_key". */
void br_loc_bind_text(br_engine *e, br_node n, const char *key);
/* The key bound to a text node, "" if none. */
const char *br_loc_text_key(br_engine *e, br_node n);

#endif
