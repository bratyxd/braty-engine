# text_field - editable text boxes

Header: `engine/include/braty/br_text_field.h`. Source: `engine/src/text_field/br_text_field.c`.
Test: `tests/test_text_field.c` (play-test command in `tests/test_playtest.c`). Example:
`examples/text_field_demo.c` (golden `text_field_demo`).

## What it does

- A 2D node type `"text_field"`: caret, selection (mouse drag, shift + arrows / Home / End,
  double-click = word, triple-click / Ctrl+A = all), Ctrl + arrows / Backspace / Delete by word,
  clipboard (Ctrl+X / C / V through SDL; nothing is copied in password mode), undo of the last
  edit (Ctrl+Z or Ctrl+Y; a run of typing or of deleting is one step; undo again = redo), max
  length in code points, single line (scrolls sideways; Enter submits) or multi-line (wraps,
  scrolls down; Enter = new line, Ctrl+Enter submits), password mode (bullets), placeholder.
- IME: `SDL_EVENT_TEXT_EDITING` shows the pre-edit text underlined at the caret (it is not part
  of the text until `SDL_EVENT_TEXT_INPUT` commits it; an empty update cancels it). Focus starts
  SDL text input (`SDL_StartTextInputWithProperties`: password and multi-line hints) and every
  frame tells the IME where the caret is (`br_input_text_set_area`, window coordinates through
  the 2D cameras). Losing focus drops a composition in progress (not committed).
- Built on the public text API only: a child text node shows the text, its char boxes and lines
  give the caret stops; a child white sprite scissor-masks the box. The field's own render
  callback draws background, border, selection, IME underline and caret.
- Works inside a canvas3d: clicks arrive through the 3D pick as normal 2D events.

## Functions you actually need

| Function | What for |
|---|---|
| `br_text_field_create(e, size, &style)` | A field (JSON `"type": "text_field"`). |
| `br_text_field_text`, `_set_text`, `_insert` | Read / replace (no event) / type-or-paste (undoable, CHANGED). |
| `br_text_field_set_listener(e, n, fn, user)` | CHANGED, SUBMITTED, FOCUSED, BLURRED. |
| `br_text_field_focus(e, n)`, `br_text_field_focused(e)` | Keyboard focus (zero handle blurs). |
| `_set_placeholder`, `_set_max_length`, `_set_multiline`, `_set_password`, `_set_colors`, `_set_style`, `_set_size`, `_set_padding` | Options. |
| `_get_selection`, `_set_selection`, `_select_all`, `_get_caret_rect`, `_offset_at` | Caret and selection (byte offsets on code point starts). |
| `br_text_field_copy / cut / paste / undo` | What the shortcuts do. |

## Focus and game input (the rule)

One field per engine has the keyboard. A press on a field focuses it, a press anywhere else (or
Escape) blurs it, `br_text_field_focus` does it from code. Gamepad / keyboard menus have no
engine focus ring: your menu's focus logic calls `br_text_field_focus` when its "confirm" lands on
the field's row (SDL shows the on-screen keyboard where the platform has one) and
`br_text_field_focus(e, zero)` to leave.

While a field has focus the keyboard is captured (`br_input_set_keyboard_captured`): actions
read their key bindings as up, so typing "w" does not walk and Space does not jump; mouse and
gamepad bindings stay live. When focus ends, an action whose key is still held (the Enter that
submitted the field) stays up until that key is released. Raw key queries
(`br_input_key_down/pressed`) are not filtered: a system reading keys directly checks
`br_node_valid(e, br_text_field_focused(e))` first.

## Play-testing

`{"cmd": "type", "label": "player_name", "text": "Ada"}` (short form `type player_name "Ada"`)
focuses the field and sends one text-input event; `get player_name:text` reads it. Keys go
through `key` (`key Left`, `key Backspace`). IME and invalid bytes are tested with
`br_input_inject_text_editing` / `br_input_inject_text` in C.

## Limits

- Editing steps over code points, not grapheme clusters: a combining accent or an emoji sequence
  takes several Left presses (`ponytail:` in the header). No bidi, no shaping (text module).
- Text is always valid UTF-8: invalid bytes become U+FFFD, C0 / C1 control characters are
  dropped, `\n` / `\t` are kept in multi-line fields and become spaces in single-line ones.
- Max length 1..65536 code points (default 256); a paste larger than the room left reads only
  what fits (a 1 MB paste costs one linear pass). The caret stops are rebuilt each frame
  (O(text length) per field, `ponytail:` in the source).
- Selection, caret, align: the field forces left alignment, `pre` white space and top baseline.
- The IME candidate window is placed through the 2D cameras; for a field on a canvas3d it uses
  canvas coordinates (wrong place on screen). Drag-selecting on a canvas3d works only while the
  pointer stays over the field (3D picks send no global moves).
- Double-click timing uses event timestamps (500 ms, 6 px).
