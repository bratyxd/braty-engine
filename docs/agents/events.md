# events - pointer events on nodes (PixiJS EventSystem) and polled input

Headers: `engine/include/braty/br_events.h`, `br_input.h`. Source: `engine/src/events/`.
Test: `tests/test_events.c`. Example: `examples/events_demo.c`.

## The model

- **Polled input** (`br_input_*`): plain state systems read every frame. `down` is live;
  `pressed`/`released` are edges latched by `input.begin_frame` (INPUT, priority -2000000), so a
  tap between two frames still reads as pressed once. Keyboard (scancodes), mouse, touch, pen,
  gamepads (hot-plug, axes, rumble; needs a window), text input.
- **Actions**: a name -> keys / mouse buttons / gamepad buttons / gamepad axis directions,
  usually loaded from JSON with `br_input_actions_from_json`. Gamepad bindings read every pad.
- **Pointer events** (`br_node_on`): mouse, touch and pen are hit-tested against the stage and
  dispatched to nodes with capture -> target -> bubble propagation. Listeners are
  `(type, fn, user)` rows per node.
- **Injection** (`br_input_inject_*`): builds an `SDL_Event` and calls `br_app_event`, the exact
  path real events take. Tests and scripted demos use it.

## Functions you actually need

| Function | What for |
|---|---|
| `br_node_set_event_mode(e, n, BR_EVENT_MODE_STATIC)` | Make a node a possible target. |
| `br_node_on / br_node_once / br_node_on_capture / br_node_off` | Listeners. |
| `br_event_stop_propagation`, `br_event_local(e, ev)` | Inside a listener. |
| `br_node_set_hit_area`, `br_node_set_cursor`, `br_node_set_interactive_children` | Hit shape, cursor, pruning. |
| `br_events_hit_test(e, point)`, `br_events_hovered(e)` | Queries. |
| `br_input_key_down/pressed/released`, `br_input_mouse_*`, `br_input_gamepad_*` | Polled state. |
| `br_input_actions_from_json`, `br_input_action_down/pressed/released/value` | Actions. |
| `br_input_inject_pointer_move/down/up`, `_wheel`, `_key`, `_touch` | Scripted input. |

## Snippet (from examples/events_demo.c)

```c
/* A button is a container (scaled and tinted by the system) holding a white rectangle. */
static br_node make_button(demo_state *s, br_engine *e, br_node parent, float x, float y, float w, float h, uint32_t rgb)
{
    br_node n = br_node_create_container(e);
    br_node_set_position(e, n, x + w * 0.5f, y + h * 0.5f);   /* centred: pressed scales around the middle */
    br_node_add_child(e, parent, n);
    make_rect(s, e, n, -w * 0.5f, -h * 0.5f, w, h, 0xffffff);
    br_node_set_event_mode(e, n, BR_EVENT_MODE_STATIC);
    br_node_set_cursor(e, n, SDL_SYSTEM_CURSOR_POINTER);
    demo_button *b = br_component_add(e, n, s->button_component);
    b->base = br_color_from_hex(rgb, 1.0f);
    br_event_type types[] = { BR_EVENT_POINTER_OVER, BR_EVENT_POINTER_OUT, BR_EVENT_POINTER_DOWN, BR_EVENT_POINTER_UP,
                              BR_EVENT_POINTER_UP_OUTSIDE, BR_EVENT_CLICK, BR_EVENT_TAP };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(types); i++) {
        br_node_on(e, n, types[i], button_event, s);
    }
    return n;
}
```

The listener only writes component data; a system turns it into looks (data-driven).

Actions from JSON (from tests/test_events.c):

```json
{ "jump": { "keys": ["Space", "W"], "buttons": ["a"] },
  "left": { "keys": ["A"], "axes": ["-leftx"] },
  "fire": { "mouse": [1] } }
```

Key names are SDL scancode names, buttons SDL gamepad names (`"a"` = south face button,
`"start"`, `"dpup"`), axes SDL axis names with a `+`/`-` direction. Axes count as down past 0.5.

## Hit testing rules

- Reverse draw order: render-layer members (drawn after children) first, then children last to
  first, z-index applied even if the sort system has not run yet, then the node itself.
- Skipped: invisible / non-renderable nodes, nodes used as masks, `NONE`, `PASSIVE` with
  `interactive_children = false`, points outside a node's `hit_area` (prunes its subtree too),
  points hidden by the node's mask (scissor = mask bounds, stencil/alpha/colour = mask geometry,
  inverse honoured).
- A node's own shape: `hit_area` if set, else its type's `hit_test` (sprites: their rectangle).
  Plain containers have no shape: they are hit only through children.
- Target = top-most `STATIC`/`DYNAMIC` node. A hit on a `PASSIVE`/`AUTO` child of an interactive
  node targets that ancestor (Pixi semantics).

## Event order

- Hover change: `OUT` (bubbles) on the old target, `LEAVE` on each node left (inner to outer),
  `OVER` (bubbles) on the new target, `ENTER` on each node entered (outer to inner), then `MOVE`.
- `UP_OUTSIDE`: on the pressed node and each ancestor up to the common ancestor with the release
  target; `CLICK`/`RIGHT_CLICK`/`TAP` then go to that common ancestor (or the pressed node).
- Mouse motion is coalesced: the latest position is hit-tested and dispatched once per frame by
  `events.ticker` (INPUT phase), or right before a mouse button, wheel or window-leave event (a
  press sees the hover of where the mouse went). Tests that inject a move and check hover at once
  call `br_events_flush(e)`. Touch and pen moves dispatch immediately.
- `GLOBAL_MOVE`: every dispatched move, to every node with such a listener and mode != NONE.
- Touch fingers and pens are separate pointers (`ev->pointer_id`); a touch gets OVER/ENTER on
  down and OUT/LEAVE on up. SDL's synthetic mouse events from touch/pen are ignored here.
- `DYNAMIC` nodes: while one is under a still mouse, `events.ticker` replays a move each frame.

## Gotchas

- Listeners may destroy nodes, add or remove listeners: removed rows are skipped, added rows run
  from the next dispatch, destroyed nodes are skipped for the rest of the path. SDL events arrive
  outside `br_engine_step`, so a destroy inside a listener is usually immediate - keep handles,
  not pointers.
- Listener tables are cleaned through `br_scene_on_node_destroy`; a new node reusing a slot starts
  with no listeners. Removed rows are compacted per list (only lists marked dirty), so unloading a
  scene of N listened nodes is linear.
- Coordinates are window coordinates = stage units. Touch coordinates are scaled by the window
  size (headless engines pass them through unscaled).
- `br_sprite_set_size` sets the node scale: put children and per-state scaling on a parent
  container (as the snippet does).
- Paths are capped at 64 ancestors (`// ponytail:` in br_events.c).
