/* br_prefab.h - prefabs: a scene file instantiated any number of times, with overrides.
 *
 * A prefab file is an ordinary scene file (one node object). Each instance is built from a
 * copy of it with `overrides` merged on top:
 *   - objects merge key by key, recursively ("components": { "velocity": { "x": 5 } } changes
 *     only velocity.x),
 *   - "children" arrays are appended to the prefab's children,
 *   - everything else replaces the prefab's value.
 *
 * In a scene file the same thing is a node object with a "prefab" key; the other keys are the
 * overrides:
 *   { "prefab": "prefabs/coin.prefab.json", "position": [120, 40], "tint": "gold" }
 *
 * Parsed prefab files are cached by path (br_prefab_clear_cache forgets them; hot reload does).
 */
#ifndef BR_PREFAB_H
#define BR_PREFAB_H

#include "braty/br_render.h"

/* Builds one instance of the prefab at `path` under parent (zero handle = detached), with
 * optional overrides (NULL = none). Returns the instance root, zero handle on failure (logged). */
br_node br_prefab_instantiate(br_engine *e, const char *path, br_node parent, const cJSON *overrides);
/* Forgets every cached prefab file (the next instantiate reads the file again). */
void br_prefab_clear_cache(br_engine *e);

#endif
