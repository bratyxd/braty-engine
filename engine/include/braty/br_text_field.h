/* br_text_field.h - an editable text box (2D node; works inside a canvas3d too).
 *
 *   br_text_style st = br_text_style_default();
 *   st.font_size = 24;
 *   br_node name = br_text_field_create(e, br_vec2_make(320, 44), &st);
 *   br_text_field_set_placeholder(e, name, "Your name");
 *   br_text_field_set_listener(e, name, on_name, game);   CHANGED / SUBMITTED / FOCUSED / BLURRED
 *
 * Editing: caret, selection (mouse drag, shift + arrows / Home / End, double-click selects a word,
 * Ctrl+A), Ctrl + arrows and Ctrl + Backspace / Delete by word, clipboard (Ctrl+X / C / V through
 * SDL; copy and cut do nothing in password mode), undo of the last edit (Ctrl+Z; again = redo),
 * max length in code points, single line (scrolls sideways to keep the caret visible; Enter
 * submits) or multi-line (wraps, scrolls down; Enter = new line, Ctrl+Enter submits), password
 * mode (bullets), placeholder text, IME composition shown underlined at the caret and the IME
 * candidate window placed next to it (SDL_SetTextInputArea).
 *
 * Text is always valid UTF-8: invalid bytes (typed, injected, pasted, set) become U+FFFD, control
 * characters are dropped ('\n' kept in multi-line fields, turned into a space in single-line
 * ones), and input past max_length is cut on a code point boundary. Offsets in this API are byte
 * offsets into br_text_field_text that always sit on a code point boundary.
 * ponytail: editing steps over code points, not grapheme clusters: a combining accent or an
 * emoji sequence takes several Left presses; upgrade with a grapheme break table.
 *
 * Focus: one field per engine has the keyboard. A click / tap on a field focuses it (and places
 * the caret), a press anywhere else blurs it, Escape blurs it, br_text_field_focus does it from
 * code - which is how gamepad / keyboard menus focus a field (your focus system calls it when its
 * "confirm" lands on the field's row; SDL shows the on-screen keyboard where the platform has one).
 * While a field has focus the keyboard is captured (br_input_set_keyboard_captured): game actions
 * do not see the keys that are typed, and a key held when focus ends (the Enter that submitted)
 * stays up for actions until released. Gamepad and mouse bindings stay live.
 *
 * Look: style (font, size, fill) from a br_text_style (wrap, align, white space and baseline are
 * the field's own); colours from br_text_field_colors. The caret blinks on the unscaled ticker
 * clock (on for 0.5 s after every edit or move, then 0.5 s off), so --frames runs are
 * deterministic. Scene files: { "type": "text_field", ... } (docs/DATA_FORMATS.md).
 */
#ifndef BR_TEXT_FIELD_H
#define BR_TEXT_FIELD_H

#include "braty/br_text.h"

/* Upper bound for br_text_field_set_max_length (code points). */
#define BR_TEXT_FIELD_MAX_LENGTH 65536

typedef enum br_text_field_event {
    BR_TEXT_FIELD_CHANGED,    /* the user changed the text (typing, paste, cut, delete, undo, IME commit) */
    BR_TEXT_FIELD_SUBMITTED,  /* Enter (single line) or Ctrl+Enter (multi-line) */
    BR_TEXT_FIELD_FOCUSED,
    BR_TEXT_FIELD_BLURRED
} br_text_field_event;

/* Called after the change. May destroy the field, change its text or move focus. */
typedef void (*br_text_field_fn)(br_engine *e, br_node field, br_text_field_event event, void *user);

typedef struct br_text_field_colors {
    br_color background;      /* alpha 0 = none */
    br_color border;          /* alpha 0 = none; 1 px, brighter while focused (focus_border) */
    br_color focus_border;
    br_color selection;
    br_color caret;
    br_color placeholder;
} br_text_field_colors;

/* Registers the module (node type, event handler, systems). Called once by br_builtin.c. */
void br_text_field_register(br_engine *e);
/* Type id of text field nodes ("text_field"). */
uint32_t br_text_field_type(br_engine *e);
/* Creates a detached field of `size` (local units). style NULL = br_text_style_default(). */
br_node br_text_field_create(br_engine *e, br_vec2 size, const br_text_style *style);

/* ---- text */

/* Replaces the text (sanitised, cut to max_length), caret to the end, clears undo. No CHANGED event. */
void br_text_field_set_text(br_engine *e, br_node n, const char *utf8);
/* The text ("" for other nodes). Valid until the next edit. */
const char *br_text_field_text(br_engine *e, br_node n);
/* Replaces the selection with text as if typed or pasted (sanitised, cut to fit max_length,
 * undoable, CHANGED if anything changed). False if nothing could be inserted. */
bool br_text_field_insert(br_engine *e, br_node n, const char *utf8);
/* The text shown while empty (copied). */
void br_text_field_set_placeholder(br_engine *e, br_node n, const char *utf8);
/* The placeholder ("" if none). */
const char *br_text_field_placeholder(br_engine *e, br_node n);
/* Max length in code points, 1..BR_TEXT_FIELD_MAX_LENGTH (default 256); longer text is cut. */
void br_text_field_set_max_length(br_engine *e, br_node n, uint32_t max_length);
/* The max length in code points. */
uint32_t br_text_field_max_length(br_engine *e, br_node n);

/* ---- caret and selection (byte offsets, snapped back to a code point start) */

/* Selects anchor..caret (equal = just a caret). */
void br_text_field_set_selection(br_engine *e, br_node n, uint32_t anchor, uint32_t caret);
/* Copies the selection's ends into *anchor and *caret (either may be NULL). False for other nodes. */
bool br_text_field_get_selection(br_engine *e, br_node n, uint32_t *anchor, uint32_t *caret);
/* Selects everything. */
void br_text_field_select_all(br_engine *e, br_node n);
/* The caret box in the field's local space (x, top, 1 px wide, line height). False for other nodes. */
bool br_text_field_get_caret_rect(br_engine *e, br_node n, br_rect *out);
/* The byte offset nearest a local point (where a click there puts the caret). */
uint32_t br_text_field_offset_at(br_engine *e, br_node n, br_vec2 local_point);
/* IME pre-edit text being composed in the field ("" if none). */
const char *br_text_field_composition(br_engine *e, br_node n);

/* ---- clipboard and undo */

/* Copies the selection to the clipboard (not in password mode). */
bool br_text_field_copy(br_engine *e, br_node n);
/* Copies, then deletes the selection (not in password mode). */
bool br_text_field_cut(br_engine *e, br_node n);
/* Inserts the clipboard text (br_text_field_insert rules). */
bool br_text_field_paste(br_engine *e, br_node n);
/* Undoes the last edit (a run of typing or of deleting counts as one); a second undo redoes it. */
bool br_text_field_undo(br_engine *e, br_node n);

/* ---- options */

/* Multi-line: wraps at the width, Enter inserts '\n' (default false). Switching off turns '\n' into spaces. */
void br_text_field_set_multiline(br_engine *e, br_node n, bool multiline);
/* True for a multi-line field. */
bool br_text_field_multiline(br_engine *e, br_node n);
/* Password mode: shows one bullet per code point, no copy / cut, no IME (default false). */
void br_text_field_set_password(br_engine *e, br_node n, bool password);
/* True in password mode. */
bool br_text_field_password(br_engine *e, br_node n);
/* Box size in local units (1..16384 each way). */
void br_text_field_set_size(br_engine *e, br_node n, br_vec2 size);
/* The box size. */
br_vec2 br_text_field_size(br_engine *e, br_node n);
/* Space between the box edge and the text (0..1000, default 8). */
void br_text_field_set_padding(br_engine *e, br_node n, float padding);
/* The padding. */
float br_text_field_padding(br_engine *e, br_node n);
/* Text style (copied). The field overrides word wrap, align, white space and baseline. */
void br_text_field_set_style(br_engine *e, br_node n, const br_text_style *style);
/* Copies the style into *out. False for other nodes. */
bool br_text_field_get_style(br_engine *e, br_node n, br_text_style *out);
/* Colours (copied). */
void br_text_field_set_colors(br_engine *e, br_node n, const br_text_field_colors *colors);
/* Copies the colours into *out. False for other nodes. */
bool br_text_field_get_colors(br_engine *e, br_node n, br_text_field_colors *out);
/* The default colours (white box, grey border, blue selection, dark caret, grey placeholder). */
br_text_field_colors br_text_field_colors_default(void);
/* One listener per field (NULL removes it). */
void br_text_field_set_listener(br_engine *e, br_node n, br_text_field_fn fn, void *user);

/* ---- focus */

/* Gives n the keyboard (starts SDL text input, captures the keyboard). A zero handle (or any
 * node that is not a field) blurs the focused field. A composition in progress is dropped. */
void br_text_field_focus(br_engine *e, br_node n);
/* The focused field (zero handle if none). */
br_node br_text_field_focused(br_engine *e);

#endif
