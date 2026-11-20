/* br_events.h - federated pointer events on scene nodes (PixiJS EventSystem / EventBoundary).
 *
 *   br_node_set_event_mode(e, button, BR_EVENT_MODE_STATIC);
 *   br_node_on(e, button, BR_EVENT_CLICK, on_click, state);
 *
 * Mouse, touch and pen SDL events are hit-tested against the stage (br_scene_root) in reverse
 * draw order: render layers, z-index sorting, masks (scissor, stencil, inverse), hit areas
 * and the node type's hit_test are respected. The target is the top-most node whose event mode
 * is STATIC or DYNAMIC (a hit on a PASSIVE/AUTO child goes to its nearest interactive ancestor).
 *
 * Propagation (bubbling types): capture listeners root -> target, then target, then normal
 * listeners target -> root. br_event_stop_propagation finishes the current node and stops;
 * br_event_stop_immediate_propagation stops at once. ENTER/LEAVE do not bubble: each node
 * entered/left gets its own event (leave: inner to outer; enter: outer to inner). Order on a
 * hover change: OUT, LEAVE..., OVER, ENTER..., then MOVE.
 *
 * Listeners are rows (type, fn, user) per node. Adding/removing listeners or destroying nodes
 * inside a listener is safe: removed rows are skipped, rows added during a dispatch run from
 * the next dispatch, destroyed nodes are skipped for the rest of the path.
 *
 * Mouse motion is coalesced: the latest position is hit-tested once per frame, by the
 * "events.ticker" system (INPUT phase), or earlier when a mouse button, wheel or leave event
 * arrives (so a press sees the hover of where the mouse went first). Code that injects moves
 * and checks hover without stepping calls br_events_flush. Touch and pen moves dispatch at once.
 *
 * Synthetic mouse events SDL makes from touch/pen are ignored here (touch and pen produce
 * their own pointer events with pointer_type TOUCH/PEN). Coordinates are window coordinates,
 * which is the stage space.
 */
#ifndef BR_EVENTS_H
#define BR_EVENTS_H

#include "braty/br_render.h"

typedef enum br_event_type {
    BR_EVENT_POINTER_DOWN,
    BR_EVENT_POINTER_UP,
    BR_EVENT_POINTER_UP_OUTSIDE,  /* released outside the node it went down on */
    BR_EVENT_POINTER_MOVE,
    BR_EVENT_POINTER_OVER,
    BR_EVENT_POINTER_OUT,
    BR_EVENT_POINTER_ENTER,       /* does not bubble */
    BR_EVENT_POINTER_LEAVE,       /* does not bubble */
    BR_EVENT_TAP,                 /* touch down+up on the same node */
    BR_EVENT_CLICK,               /* mouse/pen primary button down+up (released elsewhere: the common ancestor) */
    BR_EVENT_RIGHT_CLICK,         /* mouse right button down+up (released elsewhere: the common ancestor) */
    BR_EVENT_WHEEL,
    BR_EVENT_GLOBAL_MOVE,         /* every move, to every node listening (hit or not; mode != NONE) */
    BR_EVENT_TYPE_COUNT
} br_event_type;

typedef enum br_event_phase {
    BR_EVENT_PHASE_NONE,
    BR_EVENT_PHASE_CAPTURE,
    BR_EVENT_PHASE_AT_TARGET,
    BR_EVENT_PHASE_BUBBLE
} br_event_phase;

typedef enum br_pointer_type {
    BR_POINTER_MOUSE,
    BR_POINTER_TOUCH,
    BR_POINTER_PEN
} br_pointer_type;

/* One federated event. Listeners get a pointer valid only during the call. */
typedef struct br_event {
    br_event_type   type;
    br_event_phase  phase;
    br_node         target;          /* the hit node */
    br_node         current_target;  /* the node whose listener runs */
    br_vec2         global;          /* stage / window coordinates */
    br_vec2         movement;        /* moves: change since the previous position */
    br_vec2         wheel;           /* WHEEL: scroll amount (y > 0 = away from the user) */
    br_pointer_type pointer_type;
    uint64_t        pointer_id;      /* 0 = mouse, finger id, pen id */
    int             button;          /* SDL_BUTTON_* of the button that changed (down/up/click) */
    float           pressure;
    uint64_t        timestamp_ns;
    bool            propagation_stopped;
    bool            immediate_stopped;
} br_event;

/* A listener. e may destroy nodes, add or remove listeners. */
typedef void (*br_event_listener_fn)(br_engine *e, br_event *ev, void *user);

/* Registers the module (listener table, SDL event handler, ticker). Called once by br_builtin.c. */
void br_events_register(br_engine *e);

/* ---- listeners */

/* Adds a listener (bubble and target phase). */
void br_node_on(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user);
/* Adds a listener removed after its first call. */
void br_node_once(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user);
/* Adds a capture-phase listener (runs root -> target before bubbling listeners). */
void br_node_on_capture(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user);
/* Removes every listener of n matching (type, fn, user), capture or not. */
void br_node_off(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user);
/* Removes every listener of n. */
void br_node_off_all(br_engine *e, br_node n);
/* Number of listeners of n (all types). */
uint32_t br_node_listener_count(br_engine *e, br_node n);

/* ---- inside a listener */

/* Stops propagation after the current node's listeners. */
void br_event_stop_propagation(br_event *ev);
/* Stops propagation now (remaining listeners of the current node are skipped too). */
void br_event_stop_immediate_propagation(br_event *ev);
/* The event position in the current target's local space. */
br_vec2 br_event_local(br_engine *e, const br_event *ev);

/* ---- queries */

/* Top-most interactive node at a stage point (zero handle if none). */
br_node br_events_hit_test(br_engine *e, br_vec2 point);
/* The node the mouse is over (zero handle if none), as of the last dispatched move. */
br_node br_events_hovered(br_engine *e);
/* Dispatches the pending coalesced mouse motion now (normally done once per frame). */
void br_events_flush(br_engine *e);
/* Dispatches a custom event to ev->target with full propagation (Pixi dispatchEvent). */
void br_events_dispatch(br_engine *e, br_event *ev);

#endif
