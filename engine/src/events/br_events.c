/* br_events: hit testing, federated pointer events, per-node listener table. */
#include "braty/br_app.h"
#include "braty/br_camera.h"
#include "braty/br_events.h"
#include "events/br_input_internal.h"
#include "scene/br_scene_internal.h"

#define BR_EVENTS_MAX_DEPTH    64   /* propagation path length */
#define BR_EVENTS_MAX_POINTERS 16
#define BR_EVENTS_MAX_BUTTONS  8

typedef struct br_listener {
    br_event_listener_fn fn;    /* NULL = removed; dropped by the next compaction */
    void   *user;
    uint8_t type;
    bool    capture;
    bool    once;
} br_listener;

/* Listener rows of one node, at table index = node index. */
typedef struct br_listener_list {
    uint32_t generation;        /* generation of the owning node, 0 = unused */
    bool     dirty;             /* has removed rows; its index is in br_events_state.dirty */
    br_array rows;              /* br_listener */
} br_listener_list;

/* A propagation path: nodes[0] = target, nodes[count - 1] = outermost ancestor. */
typedef struct br_event_path {
    br_node  nodes[BR_EVENTS_MAX_DEPTH];
    uint32_t count;
} br_event_path;

typedef struct br_pointer {
    bool            used;
    br_pointer_type type;
    uint64_t        id;
    br_vec2         position;
    br_event_path   over;                           /* hover path (empty = over nothing) */
    br_node         press[BR_EVENTS_MAX_BUTTONS];   /* node each button went down on */
} br_pointer;

typedef struct br_events_state {
    br_array lists;             /* br_listener_list, indexed by node index */
    uint32_t dispatch_depth;    /* > 0 while listeners run */
    br_array dirty;             /* uint32_t: indices of lists with removed rows, compacted lazily */
    br_array scratch;           /* uint32_t: sorted child copies during hit tests */
    br_array global_nodes;      /* br_node: GLOBAL_MOVE receivers of one dispatch */
    br_pointer pointers[BR_EVENTS_MAX_POINTERS];
    bool     dynamic_seen;      /* a DYNAMIC node was visited by the last mouse hit test */
    bool     move_pending;      /* mouse motion not dispatched yet (coalesced to one per frame) */
    br_vec2  move_position;     /* stage position of the latest pending motion */
    uint64_t move_timestamp;
    bool     warned_depth;      /* the hit-test depth limit was logged */
    int      cursor;            /* SDL_SystemCursor shown, -1 = default */
    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT];
} br_events_state;

/* ---- listener table */

static br_listener_list *br_events_list(br_engine *e, br_node n)
{
    br_events_state *s = e->events;
    br_listener_list *list = br_array_at(&s->lists, n.index);
    if (list == NULL || list->generation != n.generation || n.generation == 0) {
        return NULL;
    }
    return list;
}

/* Drops the removed rows of the lists marked dirty: only those, so unloading a scene of N
 * listened nodes is O(N), not O(N^2) (it used to walk every list on every removal). */
static void br_events_compact(br_events_state *s)
{
    for (uint32_t d = 0; d < s->dirty.count; d++) {
        br_listener_list *list = br_array_at(&s->lists, *(uint32_t *)br_array_at(&s->dirty, d));
        if (list == NULL) {
            continue;
        }
        uint32_t kept = 0;
        for (uint32_t j = 0; j < list->rows.count; j++) {
            br_listener *row = br_array_at(&list->rows, j);
            if (row->fn != NULL) {
                *(br_listener *)br_array_at(&list->rows, kept) = *row;
                kept++;
            }
        }
        list->rows.count = kept;
        list->dirty = false;
        if (kept == 0) {
            br_array_free(&list->rows);
        }
    }
    br_array_clear(&s->dirty);
}

/* Marks a list as having removed rows. */
static void br_events_mark_dirty(br_events_state *s, uint32_t index)
{
    br_listener_list *list = br_array_at(&s->lists, index);
    if (list != NULL && !list->dirty) {
        list->dirty = true;
        br_array_push(&s->dirty, &index);
    }
}

/* Marks rows of list `index` removed; compacts now unless listeners are running. */
static void br_events_rows_removed(br_events_state *s, uint32_t index)
{
    br_events_mark_dirty(s, index);
    if (s->dispatch_depth == 0) {
        br_events_compact(s);
    }
}

static void br_events_add(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user,
                          bool capture, bool once)
{
    br_events_state *s = e->events;
    if (!br_node_valid(e, n) || fn == NULL || type < 0 || type >= BR_EVENT_TYPE_COUNT) {
        br_log_error("events: listener ignored (invalid node, type or function)");
        return;
    }
    if (n.index >= s->lists.count && !br_array_resize(&s->lists, n.index + 1)) {
        return;
    }
    br_listener_list *list = br_array_at(&s->lists, n.index);
    if (list->generation != n.generation) {
        /* rows of a previous node at this index: drop them (the destroy hook already did) */
        for (uint32_t j = 0; j < list->rows.count; j++) {
            ((br_listener *)br_array_at(&list->rows, j))->fn = NULL;
        }
        if (list->rows.elem_size == 0) {
            br_array_init(&list->rows, sizeof(br_listener));
        }
        list->generation = n.generation;
        if (list->rows.count > 0) {
            br_events_mark_dirty(s, n.index);
        }
    }
    br_listener row;
    SDL_zero(row);
    row.fn = fn;
    row.user = user;
    row.type = (uint8_t)type;
    row.capture = capture;
    row.once = once;
    br_array_push(&list->rows, &row);
}

void br_node_on(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user)
{
    br_events_add(e, n, type, fn, user, false, false);
}

void br_node_once(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user)
{
    br_events_add(e, n, type, fn, user, false, true);
}

void br_node_on_capture(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user)
{
    br_events_add(e, n, type, fn, user, true, false);
}

void br_node_off(br_engine *e, br_node n, br_event_type type, br_event_listener_fn fn, void *user)
{
    br_listener_list *list = br_events_list(e, n);
    if (list == NULL) {
        return;
    }
    for (uint32_t j = 0; j < list->rows.count; j++) {
        br_listener *row = br_array_at(&list->rows, j);
        if (row->fn == fn && row->user == user && row->type == (uint8_t)type) {
            row->fn = NULL;
        }
    }
    br_events_rows_removed(e->events, n.index);
}

void br_node_off_all(br_engine *e, br_node n)
{
    br_listener_list *list = br_events_list(e, n);
    if (list == NULL) {
        return;
    }
    for (uint32_t j = 0; j < list->rows.count; j++) {
        ((br_listener *)br_array_at(&list->rows, j))->fn = NULL;
    }
    br_events_rows_removed(e->events, n.index);
}

uint32_t br_node_listener_count(br_engine *e, br_node n)
{
    br_listener_list *list = br_events_list(e, n);
    uint32_t count = 0;
    for (uint32_t j = 0; list != NULL && j < list->rows.count; j++) {
        count += ((br_listener *)br_array_at(&list->rows, j))->fn != NULL ? 1u : 0u;
    }
    return count;
}

static void br_events_node_destroyed(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_events_state *s = e->events;
    br_listener_list *list = br_array_at(&s->lists, n.index);
    if (list == NULL || list->generation != n.generation) {
        return;
    }
    for (uint32_t j = 0; j < list->rows.count; j++) {
        ((br_listener *)br_array_at(&list->rows, j))->fn = NULL;
    }
    list->generation = 0;
    br_events_rows_removed(s, n.index);
}

/* ---- dispatch */

void br_event_stop_propagation(br_event *ev)
{
    ev->propagation_stopped = true;
}

void br_event_stop_immediate_propagation(br_event *ev)
{
    ev->propagation_stopped = true;
    ev->immediate_stopped = true;
}

br_vec2 br_event_local(br_engine *e, const br_event *ev)
{
    return br_node_to_local(e, ev->current_target, ev->global);
}

/* Runs n's listeners of ev->type for one phase. Rows added meanwhile wait for the next dispatch. */
static void br_events_notify(br_engine *e, br_node n, br_event *ev, br_event_phase phase, bool capture)
{
    br_listener_list *list = br_events_list(e, n);
    if (list == NULL || !br_node_valid(e, n)) {
        return;
    }
    uint32_t count = list->rows.count;
    ev->current_target = n;
    ev->phase = phase;
    for (uint32_t j = 0; j < count; j++) {
        list = br_events_list(e, n);   /* the table may grow or the node die inside a listener */
        if (list == NULL || ev->immediate_stopped || !br_node_valid(e, n)) {
            return;
        }
        br_listener *row = br_array_at(&list->rows, j);
        if (row->fn == NULL || row->type != (uint8_t)ev->type || row->capture != capture) {
            continue;
        }
        br_event_listener_fn fn = row->fn;
        void *user = row->user;
        if (row->once) {
            row->fn = NULL;
            br_events_mark_dirty(e->events, n.index);
        }
        ev->current_target = n;
        ev->phase = phase;
        fn(e, ev, user);
    }
}

static void br_events_path_of(br_engine *e, br_node target, br_event_path *path)
{
    path->count = 0;
    br_node n = target;
    // ponytail: paths stop at 64 ancestors; make br_event_path a br_array if scenes nest deeper.
    while (br_node_valid(e, n) && path->count < BR_EVENTS_MAX_DEPTH) {
        path->nodes[path->count] = n;
        path->count++;
        n = br_node_parent(e, n);
    }
}

static bool br_events_path_has(const br_event_path *path, br_node n)
{
    for (uint32_t i = 0; i < path->count; i++) {
        if (br_node_equal(path->nodes[i], n)) {
            return true;
        }
    }
    return false;
}

static void br_events_begin(br_events_state *s)
{
    s->dispatch_depth++;
}

static void br_events_end(br_events_state *s)
{
    s->dispatch_depth--;
    if (s->dispatch_depth == 0 && s->dirty.count > 0) {
        br_events_compact(s);
    }
}

/* Capture root -> target, target, bubble target -> root. */
static void br_events_propagate(br_engine *e, br_event *ev, const br_event_path *path)
{
    if (path->count == 0) {
        return;
    }
    br_events_state *s = e->events;
    br_events_begin(s);
    ev->target = path->nodes[0];
    ev->propagation_stopped = false;
    ev->immediate_stopped = false;
    for (uint32_t i = path->count - 1; i > 0 && !ev->propagation_stopped; i--) {
        br_events_notify(e, path->nodes[i], ev, BR_EVENT_PHASE_CAPTURE, true);
    }
    if (!ev->propagation_stopped) {
        br_events_notify(e, path->nodes[0], ev, BR_EVENT_PHASE_AT_TARGET, true);
        br_events_notify(e, path->nodes[0], ev, BR_EVENT_PHASE_AT_TARGET, false);
    }
    for (uint32_t i = 1; i < path->count && !ev->propagation_stopped; i++) {
        br_events_notify(e, path->nodes[i], ev, BR_EVENT_PHASE_BUBBLE, false);
    }
    br_events_end(s);
}

/* One node only, no propagation (enter, leave, upoutside, global move). */
static void br_events_notify_single(br_engine *e, br_node n, br_event *ev)
{
    br_events_begin(e->events);
    ev->propagation_stopped = false;
    ev->immediate_stopped = false;
    br_events_notify(e, n, ev, BR_EVENT_PHASE_AT_TARGET, true);
    br_events_notify(e, n, ev, BR_EVENT_PHASE_AT_TARGET, false);
    br_events_end(e->events);
}

void br_events_dispatch(br_engine *e, br_event *ev)
{
    br_event_path path;
    br_events_path_of(e, ev->target, &path);
    br_events_propagate(e, ev, &path);
}

/* ---- hit testing (reverse draw order, see br_render_walk) */

enum { BR_HIT_NONE, BR_HIT_ONLY, BR_HIT_TARGET };

static bool br_events_self_contains(br_engine *e, uint32_t index, br_vec2 world_point)
{
    br_scene_state *s = e->scene;
    br_node_record *rec = br_scene_record_at(s, index);
    br_node n = br_scene_handle(s, index);
    // ponytail: walks the ancestor chain per tested node; pass matrices down if hit tests show in profiles.
    br_vec2 local = br_node_to_local(e, n, world_point);
    if (rec->hit_area.type != BR_HIT_AREA_NONE) {
        return br_hit_area_contains(&rec->hit_area, local);
    }
    return br_node_type_hit_test(e, n, local);
}

/* Point inside a node's drawn geometry, children included (stencil masks). */
/* Hit tests recurse per level, so they stop where the render walk stops drawing
 * (BR_RENDER_MAX_DEPTH): deeper nodes are neither drawn nor hit. */
#define BR_EVENTS_MAX_HIT_DEPTH 2048

static bool br_events_too_deep(br_engine *e, uint32_t depth)
{
    if (depth < BR_EVENTS_MAX_HIT_DEPTH) {
        return false;
    }
    if (!e->events->warned_depth) {
        br_log_warn("events: hierarchy deeper than %d levels (or a render-layer loop): deeper nodes are not hit",
                    BR_EVENTS_MAX_HIT_DEPTH);
        e->events->warned_depth = true;
    }
    return true;
}

static bool br_events_geometry_contains(br_engine *e, uint32_t index, br_vec2 p, uint32_t depth)
{
    if (br_events_too_deep(e, depth)) {
        return false;
    }
    br_node_record *rec = br_scene_record_at(e->scene, index);
    if (rec->destroyed) {
        return false;
    }
    if (br_events_self_contains(e, index, p)) {
        return true;
    }
    for (uint32_t i = 0; i < rec->children.count; i++) {
        uint32_t child = *(uint32_t *)br_array_at(&rec->children, i);
        if (br_events_geometry_contains(e, child, p, depth + 1)) {
            return true;
        }
        rec = br_scene_record_at(e->scene, index);
    }
    return false;
}

/* False if the node's scissor/stencil mask hides p. */
static bool br_events_mask_allows(br_engine *e, const br_node_record *rec, br_vec2 p)
{
    br_scene_state *s = e->scene;
    if (rec->mask_kind == BR_MASK_NONE || br_scene_record(e, rec->mask) == NULL) {
        return true;
    }
    uint32_t mask = rec->mask.index;
    if (rec->mask_kind == BR_MASK_SCISSOR) {
        br_rect bounds = br_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
        bool has = false;
        br_scene_bounds(s, e, mask, br_scene_chain_transform(s, mask, false), &bounds, &has);
        return has && br_rect_contains(bounds, p.x, p.y);
    }
    /* stencil, alpha and colour masks: the mask geometry decides */
    bool inside = br_events_geometry_contains(e, mask, p, 0);
    return rec->mask_inverse ? !inside : inside;
}

typedef struct br_hit_ctx {
    br_engine *e;
    br_vec2    point;
    br_node    target;
    uint32_t   depth;
} br_hit_ctx;

static int br_events_hit_node(br_hit_ctx *h, uint32_t index, bool inherited, bool from_layer);

static int br_events_hit(br_hit_ctx *h, uint32_t index, bool inherited, bool from_layer)
{
    if (br_events_too_deep(h->e, h->depth)) {
        return BR_HIT_NONE;
    }
    h->depth += 1;
    int r = br_events_hit_node(h, index, inherited, from_layer);
    h->depth -= 1;
    return r;
}

static int br_events_hit_node(br_hit_ctx *h, uint32_t index, bool inherited, bool from_layer)
{
    br_engine *e = h->e;
    br_scene_state *s = e->scene;
    br_events_state *es = e->events;
    br_node_record *rec = br_scene_record_at(s, index);
    if (rec->destroyed || !rec->visible || !rec->renderable || rec->mask_users > 0) {
        return BR_HIT_NONE;
    }
    if (!from_layer && br_scene_record(e, rec->layer) != NULL) {
        return BR_HIT_NONE; /* its render layer tests it */
    }
    if (rec->event_mode == BR_EVENT_MODE_NONE ||
        (rec->event_mode == BR_EVENT_MODE_PASSIVE && !rec->interactive_children)) {
        return BR_HIT_NONE;
    }
    if (rec->hit_area.type != BR_HIT_AREA_NONE && !br_events_self_contains(e, index, h->point)) {
        return BR_HIT_NONE;
    }
    if (!br_events_mask_allows(e, rec, h->point)) {
        return BR_HIT_NONE;
    }
    bool interactive = rec->event_mode == BR_EVENT_MODE_STATIC || rec->event_mode == BR_EVENT_MODE_DYNAMIC;
    if (rec->event_mode == BR_EVENT_MODE_DYNAMIC) {
        es->dynamic_seen = true;
    }
    br_node self = br_scene_handle(s, index);

    if (rec->interactive_children) {
        /* layer members are drawn after the children, so they are tested first */
        for (uint32_t i = rec->layer_members.count; i > 0; i--) {
            br_node member = *(br_node *)br_array_at(&rec->layer_members, i - 1);
            if (br_scene_record(e, member) == NULL) {
                continue;
            }
            int r = br_events_hit(h, member.index, inherited || interactive, true);
            rec = br_scene_record_at(s, index);
            if (r == BR_HIT_TARGET) {
                return r;
            }
            if (r == BR_HIT_ONLY) {
                h->target = interactive ? self : h->target;
                return interactive ? BR_HIT_TARGET : BR_HIT_ONLY;
            }
        }
        /* children in draw order; sort a copy if a z-index changed since the last sort */
        uint32_t base = es->scratch.count;
        uint32_t count = rec->children.count;
        if (br_array_push_n(&es->scratch, rec->children.data, count) == NULL && count > 0) {
            return BR_HIT_NONE;
        }
        if (rec->sortable_children && rec->sort_queued) {
            br_scene_sort_by_z(s, (uint32_t *)es->scratch.data + base, count);
        }
        int result = BR_HIT_NONE;
        for (uint32_t i = count; i > 0 && result == BR_HIT_NONE; i--) {
            uint32_t child = ((uint32_t *)es->scratch.data)[base + i - 1];
            result = br_events_hit(h, child, inherited || interactive, false);
        }
        es->scratch.count = base;
        if (result == BR_HIT_TARGET) {
            return result;
        }
        if (result == BR_HIT_ONLY) {
            h->target = interactive ? self : h->target;
            return interactive ? BR_HIT_TARGET : BR_HIT_ONLY;
        }
    }
    if ((inherited || interactive) && br_events_self_contains(e, index, h->point)) {
        if (interactive) {
            h->target = self;
            return BR_HIT_TARGET;
        }
        return BR_HIT_ONLY;
    }
    return BR_HIT_NONE;
}

br_node br_events_hit_test(br_engine *e, br_vec2 point)
{
    br_hit_ctx h;
    h.e = e;
    h.point = point;
    h.target = (br_node){ 0, 0 };
    h.depth = 0;
    br_node root = br_stage_root(e);
    if (br_node_valid(e, root)) {
        br_events_hit(&h, root.index, false, false);
    }
    return h.target;
}

/* ---- pointers */

static br_pointer *br_events_pointer(br_events_state *s, br_pointer_type type, uint64_t id)
{
    br_pointer *free_slot = NULL;
    for (int i = 0; i < BR_EVENTS_MAX_POINTERS; i++) {
        br_pointer *p = &s->pointers[i];
        if (p->used && p->type == type && p->id == id) {
            return p;
        }
        if (!p->used && free_slot == NULL) {
            free_slot = p;
        }
    }
    if (free_slot != NULL) {
        SDL_zerop(free_slot);
        free_slot->used = true;
        free_slot->type = type;
        free_slot->id = id;
    }
    return free_slot;
}

static void br_events_init_event(br_event *ev, br_event_type type, const br_pointer *p, uint64_t timestamp)
{
    SDL_zerop(ev);
    ev->type = type;
    ev->global = p->position;
    ev->pointer_type = p->type;
    ev->pointer_id = p->id;
    ev->pressure = p->type == BR_POINTER_MOUSE ? 0.5f : 1.0f;
    ev->timestamp_ns = timestamp;
}

static void br_events_update_cursor(br_engine *e, const br_event_path *path)
{
    br_events_state *s = e->events;
    if (br_engine_window(e) == NULL) {
        return;
    }
    int cursor = -1;
    for (uint32_t i = 0; i < path->count && cursor < 0; i++) {
        cursor = br_node_cursor(e, path->nodes[i]);
    }
    if (cursor == s->cursor || cursor >= SDL_SYSTEM_CURSOR_COUNT) {
        return;
    }
    int shown = cursor < 0 ? SDL_SYSTEM_CURSOR_DEFAULT : cursor;
    if (s->cursors[shown] == NULL) {
        s->cursors[shown] = SDL_CreateSystemCursor((SDL_SystemCursor)shown);
    }
    if (cursor < 0 && br_input_custom_cursor(e) != NULL) {
        SDL_SetCursor(br_input_custom_cursor(e));   /* the game's cursor image is the default */
    } else if (s->cursors[shown] != NULL) {
        SDL_SetCursor(s->cursors[shown]);
    }
    s->cursor = cursor;
}

/* OUT, LEAVE..., OVER, ENTER... when the hovered node changes. */
static void br_events_hover(br_engine *e, br_pointer *p, br_node target, uint64_t timestamp)
{
    br_event_path old_path = p->over;
    br_event_path new_path;
    br_events_path_of(e, target, &new_path);
    br_node old_target = old_path.count > 0 ? old_path.nodes[0] : (br_node){ 0, 0 };
    if (br_node_equal(old_target, target) && new_path.count == old_path.count) {
        return;
    }
    p->over = new_path;
    if (p->type == BR_POINTER_MOUSE) {
        br_events_update_cursor(e, &new_path);
    }
    br_event ev;
    if (br_node_valid(e, old_target) && !br_node_equal(old_target, target)) {
        br_events_init_event(&ev, BR_EVENT_POINTER_OUT, p, timestamp);
        br_event_path out_path;
        br_events_path_of(e, old_target, &out_path);
        br_events_propagate(e, &ev, &out_path);
    }
    for (uint32_t i = 0; i < old_path.count; i++) {
        if (!br_events_path_has(&new_path, old_path.nodes[i])) {
            br_events_init_event(&ev, BR_EVENT_POINTER_LEAVE, p, timestamp);
            ev.target = old_path.nodes[i];
            br_events_notify_single(e, old_path.nodes[i], &ev);
        }
    }
    if (br_node_valid(e, target) && !br_node_equal(old_target, target)) {
        br_events_init_event(&ev, BR_EVENT_POINTER_OVER, p, timestamp);
        br_events_propagate(e, &ev, &new_path);
    }
    for (uint32_t i = new_path.count; i > 0; i--) {
        br_node n = new_path.nodes[i - 1];
        if (!br_events_path_has(&old_path, n)) {
            br_events_init_event(&ev, BR_EVENT_POINTER_ENTER, p, timestamp);
            ev.target = n;
            br_events_notify_single(e, n, &ev);
        }
    }
}

static void br_events_global_move(br_engine *e, const br_event *move)
{
    br_events_state *s = e->events;
    br_array_clear(&s->global_nodes);
    for (uint32_t i = 0; i < s->lists.count; i++) {
        br_listener_list *list = br_array_at(&s->lists, i);
        for (uint32_t j = 0; j < list->rows.count; j++) {
            br_listener *row = br_array_at(&list->rows, j);
            br_node n = { i, list->generation };
            if (row->fn != NULL && row->type == BR_EVENT_GLOBAL_MOVE && br_node_event_mode(e, n) != BR_EVENT_MODE_NONE) {
                br_array_push(&s->global_nodes, &n);
                break;
            }
        }
    }
    /* copied out first: listeners may add listeners (growing the table) */
    for (uint32_t i = 0; i < s->global_nodes.count; i++) {
        br_event ev = *move;
        ev.type = BR_EVENT_GLOBAL_MOVE;
        br_node n = *(br_node *)br_array_at(&s->global_nodes, i);
        br_events_notify_single(e, n, &ev);
    }
}

static void br_events_pointer_move(br_engine *e, br_pointer *p, br_vec2 pos, uint64_t timestamp)
{
    br_vec2 movement = br_vec2_sub(pos, p->position);
    p->position = pos;
    if (p->type == BR_POINTER_MOUSE) {
        e->events->dynamic_seen = false;
    }
    br_node target = br_events_hit_test(e, pos);
    br_events_hover(e, p, target, timestamp);
    br_event ev;
    br_events_init_event(&ev, BR_EVENT_POINTER_MOVE, p, timestamp);
    ev.movement = movement;
    if (br_node_valid(e, target)) {
        br_event_path path;
        br_events_path_of(e, target, &path);
        br_events_propagate(e, &ev, &path);
    }
    ev.target = target;
    br_events_global_move(e, &ev);
}

static void br_events_pointer_down(br_engine *e, br_pointer *p, br_vec2 pos, int button, uint64_t timestamp)
{
    p->position = pos;
    br_node target = br_events_hit_test(e, pos);
    br_events_hover(e, p, target, timestamp);
    if (button >= 0 && button < BR_EVENTS_MAX_BUTTONS) {
        p->press[button] = target;
    }
    if (!br_node_valid(e, target)) {
        return;
    }
    br_event ev;
    br_events_init_event(&ev, BR_EVENT_POINTER_DOWN, p, timestamp);
    ev.button = button;
    br_event_path path;
    br_events_path_of(e, target, &path);
    br_events_propagate(e, &ev, &path);
}

static void br_events_pointer_up(br_engine *e, br_pointer *p, br_vec2 pos, int button, uint64_t timestamp)
{
    p->position = pos;
    br_node target = br_events_hit_test(e, pos);
    br_event ev;
    br_event_path up_path;
    br_events_path_of(e, target, &up_path);
    if (up_path.count > 0) {
        br_events_init_event(&ev, BR_EVENT_POINTER_UP, p, timestamp);
        ev.button = button;
        br_events_propagate(e, &ev, &up_path);
        br_events_path_of(e, target, &up_path);   /* listeners may have changed the tree */
    }
    if (button < 0 || button >= BR_EVENTS_MAX_BUTTONS) {
        return;
    }
    br_node press = p->press[button];
    p->press[button] = (br_node){ 0, 0 };
    if (!br_node_valid(e, press)) {
        return;
    }
    br_node click_target = press;
    if (!br_events_path_has(&up_path, press)) {
        /* UP_OUTSIDE on the pressed node and its ancestors up to the common ancestor */
        br_node n = press;
        while (br_node_valid(e, n) && !br_events_path_has(&up_path, n)) {
            br_events_init_event(&ev, BR_EVENT_POINTER_UP_OUTSIDE, p, timestamp);
            ev.button = button;
            ev.target = press;
            br_events_notify_single(e, n, &ev);
            n = br_node_parent(e, n);
        }
        click_target = n;
    }
    br_event_type click = BR_EVENT_TYPE_COUNT;
    if (p->type == BR_POINTER_TOUCH) {
        click = BR_EVENT_TAP;
    } else if (button == SDL_BUTTON_LEFT) {
        click = BR_EVENT_CLICK;
    } else if (button == SDL_BUTTON_RIGHT) {
        click = BR_EVENT_RIGHT_CLICK;
    }
    if (click != BR_EVENT_TYPE_COUNT && br_node_valid(e, click_target)) {
        br_events_init_event(&ev, click, p, timestamp);
        ev.button = button;
        br_event_path path;
        br_events_path_of(e, click_target, &path);
        br_events_propagate(e, &ev, &path);
    }
}

static void br_events_wheel(br_engine *e, br_pointer *p, br_vec2 pos, br_vec2 delta, uint64_t timestamp)
{
    p->position = pos;
    br_node target = br_events_hit_test(e, pos);
    if (!br_node_valid(e, target)) {
        return;
    }
    br_event ev;
    br_events_init_event(&ev, BR_EVENT_WHEEL, p, timestamp);
    ev.wheel = delta;
    br_event_path path;
    br_events_path_of(e, target, &path);
    br_events_propagate(e, &ev, &path);
}

static void br_events_pointer_gone(br_engine *e, br_pointer *p, uint64_t timestamp)
{
    br_events_hover(e, p, (br_node){ 0, 0 }, timestamp);
    if (p->type != BR_POINTER_MOUSE) {
        p->used = false;
    }
}

static br_vec2 br_events_touch_position(br_engine *e, float nx, float ny)
{
    int w = 0;
    int h = 0;
    br_engine_window_size(e, &w, &h);
    if (w <= 0 || h <= 0) {
        return br_vec2_make(nx, ny); /* headless: see br_input_inject_touch */
    }
    return br_vec2_make(nx * (float)w, ny * (float)h);
}

/* Dispatches the pending (coalesced) mouse motion: one hit test however many motion events
 * arrived (a 1000 Hz mouse sends ~16 per 60 Hz frame). */
static void br_events_flush_move(br_engine *e)
{
    br_events_state *s = e->events;
    if (!s->move_pending) {
        return;
    }
    s->move_pending = false;
    br_pointer *p = br_events_pointer(s, BR_POINTER_MOUSE, 0);
    if (p != NULL) {
        br_events_pointer_move(e, p, s->move_position, s->move_timestamp);
    }
}

void br_events_flush(br_engine *e)
{
    if (e->events != NULL) {
        br_events_flush_move(e);
    }
}

static void br_events_sdl_event(br_engine *e, const SDL_Event *ev, void *user)
{
    (void)user;
    br_events_state *s = e->events;
    uint64_t t = ev->common.timestamp;
    br_pointer *p = NULL;
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (ev->motion.which != SDL_TOUCH_MOUSEID && ev->motion.which != SDL_PEN_MOUSEID) {
            /* kept until the frame's events.ticker (or the next button / wheel / leave) */
            s->move_pending = true;
            s->move_position = br_camera_screen_to_stage(e, br_vec2_make(ev->motion.x, ev->motion.y));
            s->move_timestamp = t;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.which != SDL_TOUCH_MOUSEID && ev->button.which != SDL_PEN_MOUSEID) {
            br_events_flush_move(e); /* the press sees the hover of where the mouse went first */
            p = br_events_pointer(s, BR_POINTER_MOUSE, 0);
            br_vec2 pos = br_camera_screen_to_stage(e, br_vec2_make(ev->button.x, ev->button.y));
            if (ev->button.down) {
                br_events_pointer_down(e, p, pos, ev->button.button, t);
            } else {
                br_events_pointer_up(e, p, pos, ev->button.button, t);
            }
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ev->wheel.which != SDL_TOUCH_MOUSEID && ev->wheel.which != SDL_PEN_MOUSEID) {
            br_events_flush_move(e);
            float dir = ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            p = br_events_pointer(s, BR_POINTER_MOUSE, 0);
            br_events_wheel(e, p, br_camera_screen_to_stage(e, br_vec2_make(ev->wheel.mouse_x, ev->wheel.mouse_y)),
                            br_vec2_make(ev->wheel.x * dir, ev->wheel.y * dir), t);
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        br_events_flush_move(e);
        p = br_events_pointer(s, BR_POINTER_MOUSE, 0);
        if (p != NULL) {
            br_events_pointer_gone(e, p, t);
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        p = br_events_pointer(s, BR_POINTER_TOUCH, (uint64_t)ev->tfinger.fingerID);
        if (p == NULL) {
            break;
        }
        br_vec2 pos = br_camera_screen_to_stage(e, br_events_touch_position(e, ev->tfinger.x, ev->tfinger.y));
        if (ev->type == SDL_EVENT_FINGER_DOWN) {
            br_events_pointer_down(e, p, pos, SDL_BUTTON_LEFT, t);
        } else if (ev->type == SDL_EVENT_FINGER_MOTION) {
            br_events_pointer_move(e, p, pos, t);
        } else {
            if (ev->type == SDL_EVENT_FINGER_UP) {
                br_events_pointer_up(e, p, pos, SDL_BUTTON_LEFT, t);
            }
            br_events_pointer_gone(e, p, t);
        }
        break;
    }
    case SDL_EVENT_PEN_MOTION:
        p = br_events_pointer(s, BR_POINTER_PEN, ev->pmotion.which);
        if (p != NULL) {
            br_events_pointer_move(e, p, br_camera_screen_to_stage(e, br_vec2_make(ev->pmotion.x, ev->pmotion.y)), t);
        }
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP: {
        p = br_events_pointer(s, BR_POINTER_PEN, ev->ptouch.which);
        br_vec2 pos = br_camera_screen_to_stage(e, br_vec2_make(ev->ptouch.x, ev->ptouch.y));
        if (p != NULL && ev->ptouch.down) {
            br_events_pointer_down(e, p, pos, SDL_BUTTON_LEFT, t);
        } else if (p != NULL) {
            br_events_pointer_up(e, p, pos, SDL_BUTTON_LEFT, t);
        }
        break;
    }
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        p = br_events_pointer(s, BR_POINTER_PEN, ev->pproximity.which);
        if (p != NULL) {
            br_events_pointer_gone(e, p, t);
            p->used = false;
        }
        break;
    default:
        break;
    }
}

/* Dispatches the frame's coalesced mouse motion. Pixi's EventsTicker: when DYNAMIC nodes are
 * under a still mouse, replay a move each frame so nodes moving under the cursor get over/out. */
static void br_events_ticker(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_events_state *s = e->events;
    if (s->move_pending) {
        br_events_flush_move(e);
        return;
    }
    br_pointer *mouse = NULL;
    for (int i = 0; i < BR_EVENTS_MAX_POINTERS; i++) {
        if (s->pointers[i].used && s->pointers[i].type == BR_POINTER_MOUSE) {
            mouse = &s->pointers[i];
        }
    }
    /* ponytail: replays the stored stage position, so a camera moving under a still mouse does
     * not re-map it; keep the window position per pointer and map here if that matters */
    if (mouse != NULL && s->dynamic_seen) {
        br_events_pointer_move(e, mouse, mouse->position, SDL_GetTicksNS());
    }
}

br_node br_events_hovered(br_engine *e)
{
    br_events_state *s = e->events;
    for (int i = 0; i < BR_EVENTS_MAX_POINTERS; i++) {
        br_pointer *p = &s->pointers[i];
        if (p->used && p->type == BR_POINTER_MOUSE && p->over.count > 0 && br_node_valid(e, p->over.nodes[0])) {
            return p->over.nodes[0];
        }
    }
    return (br_node){ 0, 0 };
}

/* ---- module */

static void br_events_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_events_state *s = e->events;
    for (uint32_t i = 0; i < s->lists.count; i++) {
        br_array_free(&((br_listener_list *)br_array_at(&s->lists, i))->rows);
    }
    br_array_free(&s->lists);
    br_array_free(&s->dirty);
    br_array_free(&s->scratch);
    br_array_free(&s->global_nodes);
    for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++) {
        if (s->cursors[i] != NULL) {
            SDL_DestroyCursor(s->cursors[i]);
        }
    }
    br_free(s);
    e->events = NULL;
}

void br_events_register(br_engine *e)
{
    br_events_state *s = br_alloc(sizeof(br_events_state));
    if (s == NULL) {
        return;
    }
    e->events = s;
    br_array_init(&s->lists, sizeof(br_listener_list));
    br_array_init(&s->dirty, sizeof(uint32_t));
    br_array_init(&s->scratch, sizeof(uint32_t));
    br_array_init(&s->global_nodes, sizeof(br_node));
    s->cursor = -1;
    br_engine_add_event_handler(e, br_events_sdl_event, NULL);
    br_node_on_destroy(e, br_events_node_destroyed, NULL);
    br_system_add(e, "events.ticker", BR_PHASE_INPUT, -1000, br_events_ticker, NULL);
    br_engine_on_shutdown(e, br_events_shutdown, NULL);
}
