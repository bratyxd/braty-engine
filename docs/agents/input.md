# input - devices, gamepads, actions, contexts, rebinding, glyphs

Header: `engine/include/braty/br_input.h`. Source: `engine/src/events/br_input*.c`
(`br_input.c` devices + latch + injection, `br_input_gamepad.c`, `br_input_action.c`,
`br_input_glyph.c`, private `br_input_internal.h`). Tests: `tests/test_input.c` (this guide),
`tests/test_events.c` (pointer events + basic actions). Example: `examples/input_demo.c`.
Pointer events on nodes are in `events.md`.

## The model

- Polled state, latched once per frame by `input.begin_frame` (INPUT phase, priority
  `BR_INPUT_PRIORITY_LATCH`). `down` is live; `pressed`/`released` and action values are what the
  latch saw. A system that injects input for the current frame runs at
  `BR_INPUT_PRIORITY_LATCH - 1000`.
- SDL already maps Xbox, PlayStation, Switch and generic pads to one positional layout
  (`SDL_GAMEPAD_BUTTON_SOUTH` = bottom face button). br_input adds: slots 0..7 = players (a pad
  that reconnects gets its old slot; `br_input_gamepad_assign` moves one), a radial stick dead
  zone (default inner 0.15, outer 0.95, `br_input_gamepad_set_deadzone`) and a separate linear
  trigger dead zone (default 0.05 / 0.95, `br_input_gamepad_set_trigger_deadzone`;
  `br_input_gamepad_axis_raw` skips both), the family (`br_gamepad_type` / `br_gamepad_family`,
  from SDL's type of the connected pad), info (battery, wired/wireless, capabilities), rumble +
  trigger rumble, light bar, raw effect packets, gyro/accelerometer, touchpads.
- Hot-plug: `br_input_gamepad_joined(e, slot)` / `br_input_gamepad_left(e, slot)` are true for one
  frame (poll them for "player 2 joined" / "controller disconnected, pause"). A pad unplugged
  while buttons are held reports them released in that frame (`br_input_gamepad_released`,
  `br_input_action_released`), its axes read 0, and `br_input_last_gamepad` /
  `br_input_action_last_gamepad` go back to -1. Prompts keep the family of the pad used last, so
  a PlayStation player still sees "Cross" after the pad drops.
- Actions: name -> bindings (`br_input_binding`: key, mouse button, pad button, pad axis
  direction). Value = the largest binding; down at `>= threshold` (default 0.5, per action).
  Any pad drives an action; `br_input_action_value_for(e, action, player)` reads one player
  (-1 = keyboard/mouse, 0..7 = pad slot). Composites: `br_input_action_axis`,
  `br_input_action_vec2` (WASD -> unit-clamped vec2, y down).
- Contexts: a stack of up to 8 names. An action with a context is live only while that context
  is on top; actions without one are always live. An action that becomes live while its input is
  held stays idle until released (closing a menu with Space does not also jump).
- Rebinding: `br_input_capture_start(e, device)`, then each frame `br_input_capture_poll` until it
  returns the pressed binding; check `br_input_binding_conflict`, then
  `br_input_action_binding_set`. Actions are idle while capturing, and the captured press does
  not fire its old action. `br_input_bindings_save/load` store all bindings as JSON in save data;
  loading gives each saved action exactly the file's bindings, except that an action none of
  whose saved entries still parse (a renamed key) keeps its current bindings (logged).
- Glyphs: `br_input_glyph(e, "jump", &g)` gives `g.label` ("A", "Cross", "B", "Space", "LMB")
  and `g.frame` (sprite frame name) for the device used last; `br_input_format_prompt` expands
  `"{jump}"` in a string. Frame naming scheme (ship an atlas with these names):
  `key_<scancode name, lower case, spaces as _>` (`key_space`, `key_left_shift`),
  `mouse_left|middle|right|x1|x2`, and `<set>_<part>` with set `xbox | ps | switch` (generic and
  Steam pads use `xbox`, GameCube uses `switch`); part = printed face label (`xbox_a`,
  `ps_cross`, `switch_b`), else SDL's button name (`ps_start`, `xbox_leftshoulder`,
  `switch_dpup`), axes `leftstick`, `rightstick`, `lefttrigger`, `righttrigger`.
- Injection (tests, play-testing): keys, mouse (`br_input_inject_pointer_motion` with an
  explicit delta for relative mode), wheel, touch, text, pad buttons and axes (a press or a
  non-zero axis on an empty slot creates an "injected" pad with no SDL device; a release or 0
  on an empty slot is dropped, so a late release never brings a disconnected pad back),
  `br_input_inject_gamepad_connect(e, slot, BR_GAMEPAD_TYPE_PS5)` for an injected pad of a
  family, whole actions (`br_input_inject_action(e, "start", 1, 1)` = a one-frame tap whatever
  it is bound to), and SDL virtual pads with a chosen family (`br_input_gamepad_attach_virtual`).
  Injected pad ids come from a counter, never from the slot, so `br_input_gamepad_assign` cannot
  make two pads share one.

## Functions you actually need

| Function | What for |
|---|---|
| `br_input_actions_from_json`, `br_input_action_pressed/down/value` | Bindings as data, polling. |
| `br_input_action_vec2(e, "left", "right", "up", "down")` | Movement from keys, d-pad and stick. |
| `br_input_context_push/pop`, `br_input_action_set_context` | Gameplay / menu / dialog input. |
| `br_input_glyph`, `br_input_format_prompt` | Button prompts that follow the device. |
| `br_input_capture_start/poll`, `br_input_binding_conflict`, `br_input_bindings_save/load` | Rebinding menu. |
| `br_input_gamepad_stick`, `_axis`, `_set_deadzone`, `_set_trigger_deadzone` | Analog input after the dead zones. |
| `br_input_gamepad_joined`, `_left`, `_count` | Hot-plug: players joining, pads dropping. |
| `br_input_action_last_gamepad`, `br_input_gamepad_rumble(_triggers)` | Rumble the pad that acted. |
| `br_input_gamepad_info`, `_type`, `_sensor`, `_touchpad`, `_set_led` | Pad details and extras. |
| `br_input_last_device`, `br_input_last_gamepad` | Which device the player is using. |
| `br_input_keycode_down`, `br_input_key_mods`, `br_input_key_name` | Layout keys, modifiers, names. |
| `br_input_mouse_set_relative/_confined`, `_show_cursor`, `_set_cursor_image`, `_wheel_ticks` | Mouse. |
| `br_input_text_start`, `br_input_text`, `br_input_text_composition`, `_set_area` | Text + IME. |
| `br_input_inject_*`, `br_input_inject_gamepad_connect`, `br_input_gamepad_attach_virtual` | Tests and scripted play. |

Action JSON (also the save format of `br_input_bindings_save`):

```json
{ "jump":  { "keys": ["Space"], "buttons": ["a"], "context": "gameplay" },
  "fire":  { "mouse": [1], "axes": ["+righttrigger"], "threshold": 0.3 },
  "left":  { "keys": ["A"], "axes": ["-leftx"], "buttons": ["dpleft"] } }
```

## Snippet (from examples/input_demo.c)

```c
br_glyph jump;
br_glyph fire;
br_input_glyph(e, "jump", &jump);
br_input_glyph(e, "fire", &fire);
```

```c
char prompt[160];
br_input_format_prompt(e, "Press {jump} to jump, hold {fire} to fire, {pause} to pause", prompt, sizeof(prompt));
```

## Gotchas

- Real pads need a window (the gamepad subsystem starts on the first frame with one); virtual
  pads start it on demand, also headless.
- SDL ignores pad state while the app is not focused unless
  `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is "1" (input_demo sets it for `--frames`).
- Injected pad buttons/axes bypass SDL (synthetic events), so they work for virtual pads too;
  touchpad and sensor data of a virtual pad go through SDL (`SDL_SetJoystickVirtualTouchpad`,
  `SDL_SendJoystickVirtualSensorData`).
- Faces are positional: on a Switch pad the south button's glyph is "B" (`switch_b`).
- Face labels come from SDL's pad *type* (`SDL_GetGamepadButtonLabelForType`): a third-party pad
  SDL reports as a standard pad gets the Xbox set even if its buttons print other letters.
- Rebinding capture of an axis is level-triggered: a stick already held past 0.5 when the
  capture starts is taken at once (wait for neutral before starting a capture).
- game.json input actions are checked for unknown keys by the data module; `threshold` and
  `context` are accepted by br_input but that check does not list them yet (warning only).
- All mice are merged into one; per-mouse state is not provided. Pen has no injection function.
- Context changes and injected actions take effect at the next latch.

## Verified with virtual devices only

No physical controller was available. `tests/test_input.c` and the demo verify with SDL
virtual joysticks and injected events: family detection for all 12 types from USB ids (SDL
reported every one exactly), glyph labels/frames for every family, gyro and touchpad readings
(PS5 virtual), slot reuse on reconnect, assignment (and injection after it), stick and trigger
dead zones, join / leave edges, release of held buttons on disconnect, prompt family after a
disconnect, injected pads of a family, actions, contexts, capture, save/load. Not verified at
all: rumble, trigger rumble, LED colour, player LEDs, battery and
wireless reporting, `br_input_gamepad_send_effect` (DualSense adaptive triggers: raw packet
pass-through, untested), real Joy-Con pairing, real IME composition, relative mouse mode,
cursor confine and custom cursor images on screen, pens.

## Manual checklist (a human with a real Xbox and PlayStation pad)

Run `build/windows-gcc/input_demo.exe` (no `--frames`):

1. Plug in the Xbox pad: slot 0 shows "Xbox One" (or 360), Xbox family, buttons labelled A/B/X/Y,
   LB/RB, View/Menu. Press A: the prompt says "Press A", frames `xbox_a`.
2. Plug in the PlayStation pad: slot 1, PS4/PS5, labels Cross/Circle/Square/Triangle, L1/R1,
   Share or Create/Options. Press Cross: the prompt switches to "Cross"; touch the touchpad:
   "touchpad finger" with coordinates; tilt the pad: gyro numbers move.
3. Press Space on the keyboard: the prompt switches back to "Space".
4. Move each stick slightly: the dot stays centred inside the dead zone, then moves smoothly to
   the edge; diagonals reach the corner circle, not the square.
5. Unplug the PlayStation pad and plug it back: it returns to slot 1.
6. Hold Cross (or push a stick) and unplug the pad: a game polling `br_input_gamepad_left(e, 1)`
   sees it for one frame, the held button reports released (`br_input_gamepad_released`), the
   stick reads 0, and the prompt still says "Cross" (not "A") until another device is used.
7. Squeeze each trigger slowly: `br_input_gamepad_axis` leaves 0 within the first ~5% of travel
   and reaches 1 just before the stop (trigger dead zone), independent of the stick dead zone.
8. Battery shows a percentage for wireless pads; "wireless" is shown when connected by Bluetooth.
9. Rumble and LED: not wired to a key in the demo; call `br_input_gamepad_rumble` /
   `_set_led` from a test game and feel/see it.
10. Keyboard: with F1 open, hold W, shift-click a number in the Inspector, release W, finish the
    edit: W reads up (key releases always pass the overlay).
