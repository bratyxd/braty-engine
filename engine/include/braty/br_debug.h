/* br_debug.h - the in-game debug overlay (microui, drawn through the batcher as an overlay).
 *
 * F1 (configurable) toggles it. Panels:
 *   Stats     FPS, frame-time graph (red = over the frame budget + 10%), draw calls / batches /
 *             vertices / passes, node, texture, asset and render-target-pool counts, the asset
 *             cache list
 *   Scene     the node tree under the stage; a row's name button selects the node, "+" / "-"
 *             open and close its children
 *   Inspector live position / scale / rotation / alpha / visible of the selected node (editable)
 *   Systems   every system with its last run time; checkboxes enable/disable them
 *   Log       the last lines written through br_log_* (and SDL_Log)
 * F2 toggles the profiler panel (br_profiler.h): frame graph, scope tree, counters, memory.
 * Clicks, wheel and typing that land on an overlay window are kept from the game; key releases
 * always reach it. A shipped game turns the keys off: br_debug_set_keys_enabled(e, false) or
 * game.json "debug": { "keys": false } (code can still open the overlay).
 *
 * Input comes from the input module (mouse, wheel, keys, text); text uses a bitmap font made
 * from the text module's default font. Headless engines keep the state and log capture but
 * never draw.
 */
#ifndef BR_DEBUG_H
#define BR_DEBUG_H

#include "braty/br_render.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>

/* Lines kept by the log console. */
#define BR_DEBUG_LOG_LINES 256

/* Registers the module (overlay state, log capture, systems, the render overlay). Called once by br_builtin.c. */
void br_debug_register(br_engine *e);

/* Shows or hides the overlay (F1). */
void br_debug_set_visible(br_engine *e, bool visible);
/* True while the overlay is shown. */
bool br_debug_visible(br_engine *e);
/* The key that toggles the overlay (default SDL_SCANCODE_F1; SDL_SCANCODE_UNKNOWN = none). */
void br_debug_set_toggle_key(br_engine *e, SDL_Scancode key);
/* The key that toggles the profiler panel (default SDL_SCANCODE_F2; SDL_SCANCODE_UNKNOWN = none). */
void br_debug_set_profiler_key(br_engine *e, SDL_Scancode key);
/* Turns both keys on or off (default on). Off for shipped games: players cannot open the
 * inspector or the profiler; br_debug_set_visible / br_profiler_set_visible still work from code. */
void br_debug_set_keys_enabled(br_engine *e, bool enabled);
/* Selects the node shown in the inspector (zero handle = none). */
void br_debug_select(br_engine *e, br_node n);
/* The selected node (zero handle if none or destroyed). */
br_node br_debug_selected(br_engine *e);
/* True if the overlay takes this event (a click or wheel on one of its windows, the release of
 * such a click, a key press or text while one of its fields is edited; never a key release):
 * the overlay's SDL event filter then keeps it from the game. */
bool br_debug_wants_event(br_engine *e, const SDL_Event *ev);
/* Number of captured log lines (at most BR_DEBUG_LOG_LINES). */
uint32_t br_debug_log_count(br_engine *e);
/* Copies captured log line i (0 = oldest kept) into out (always terminated). False (out = "")
 * if out of range. Safe while other threads log. */
bool br_debug_log_line(br_engine *e, uint32_t i, char *out, size_t out_size);

#endif
