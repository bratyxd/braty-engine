/* test_events - hit testing, propagation, hover sequences, listener safety, input actions (headless). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_timer.h>
#include <string.h>

/* ---- a test node type: a w x h box at the local origin (bounds + hit test) */

typedef struct box_data {
    float w;
    float h;
} box_data;

static uint32_t g_box_type;

static bool box_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const box_data *b = data;
    *out = br_rect_make(0.0f, 0.0f, b->w, b->h);
    return true;
}

static bool box_hit(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    (void)e;
    (void)n;
    const box_data *b = data;
    return p.x >= 0.0f && p.y >= 0.0f && p.x < b->w && p.y < b->h;
}

static br_node make_box(br_engine *e, br_node parent, const char *label, float x, float y, float w, float h,
                        br_event_mode mode)
{
    br_node n = br_node_create(e, g_box_type);
    box_data *b = br_node_data(e, n, g_box_type);
    b->w = w;
    b->h = h;
    br_node_set_label(e, n, label);
    br_node_set_position(e, n, x, y);
    br_node_set_event_mode(e, n, mode);
    br_node_add_child(e, parent, n);
    return n;
}

/* ---- an event log: "type:current_target" entries separated by spaces */

static char g_log[2048];

static const char *type_name(br_event_type t)
{
    static const char *names[] = { "down", "up", "upoutside", "move", "over", "out", "enter", "leave",
                                   "tap", "click", "rightclick", "wheel", "globalmove" };
    return names[t];
}

static void log_event(br_engine *e, br_event *ev, void *user)
{
    const char *tag = user;
    char entry[64];
    SDL_snprintf(entry, sizeof(entry), "%s%s:%s ", tag != NULL ? tag : "", type_name(ev->type),
                 br_node_label(e, ev->current_target));
    SDL_strlcat(g_log, entry, sizeof(g_log));
}

static void clear_log(void)
{
    g_log[0] = '\0';
}

static bool log_is(const char *expected)
{
    if (SDL_strcmp(g_log, expected) != 0) {
        printf("  log:      '%s'\n  expected: '%s'\n", g_log, expected);
        return false;
    }
    return true;
}

static br_engine *make_engine(void)
{
    br_engine *e = br_engine_create("test_events", NULL);
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "test_box";
    desc.data_size = sizeof(box_data);
    desc.local_bounds = box_bounds;
    desc.hit_test = box_hit;
    g_box_type = br_node_type_register(e, &desc);
    return e;
}

static const char *hit_label(br_engine *e, float x, float y)
{
    br_node n = br_events_hit_test(e, br_vec2_make(x, y));
    return br_node_valid(e, n) ? br_node_label(e, n) : "-";
}

/* ---- hit testing */

static void test_hit_order(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node a = make_box(e, root, "a", 0, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_node b = make_box(e, root, "b", 50, 50, 100, 100, BR_EVENT_MODE_STATIC);
    (void)b;
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "b") == 0);   /* drawn last = on top */
    BR_CHECK(SDL_strcmp(hit_label(e, 25, 25), "a") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 500, 500), "-") == 0);

    /* z-index: before the sort system ran, and after */
    br_node_set_sortable_children(e, root, true);
    br_node_set_z_index(e, a, 10);
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "a") == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "a") == 0);
    br_node_set_z_index(e, a, -1);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "b") == 0);

    /* render layer: c is a child of a (below b) but drawn by a layer added last */
    br_node c = make_box(e, a, "c", 60, 60, 20, 20, BR_EVENT_MODE_STATIC);
    BR_CHECK(SDL_strcmp(hit_label(e, 70, 70), "b") == 0);
    br_node layer = br_node_create_container(e);
    br_node_set_z_index(e, layer, 100);
    br_node_add_child(e, root, layer);
    br_node_layer_attach(e, layer, c);
    BR_CHECK(SDL_strcmp(hit_label(e, 70, 70), "c") == 0);
    br_node_layer_detach(e, c);
    BR_CHECK(SDL_strcmp(hit_label(e, 70, 70), "b") == 0);

    /* invisible and event-mode none nodes are skipped */
    br_node_set_visible(e, b, false);
    BR_CHECK(SDL_strcmp(hit_label(e, 120, 120), "-") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "c") == 0);
    br_node_set_event_mode(e, a, BR_EVENT_MODE_NONE);
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "-") == 0);
    br_engine_destroy(e);
}

static void test_hit_modes(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    /* AUTO / PASSIVE children of an interactive parent: the parent is the target */
    br_node p = make_box(e, root, "p", 0, 0, 10, 10, BR_EVENT_MODE_STATIC);
    make_box(e, p, "auto", 100, 0, 50, 50, BR_EVENT_MODE_AUTO);
    make_box(e, p, "passive", 200, 0, 50, 50, BR_EVENT_MODE_PASSIVE);
    BR_CHECK(SDL_strcmp(hit_label(e, 120, 20), "p") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 220, 20), "p") == 0);
    /* AUTO under a passive parent is not a hit */
    br_node q = make_box(e, root, "q", 0, 300, 10, 10, BR_EVENT_MODE_PASSIVE);
    make_box(e, q, "auto2", 100, 0, 50, 50, BR_EVENT_MODE_AUTO);
    BR_CHECK(SDL_strcmp(hit_label(e, 120, 320), "-") == 0);
    /* passive parent with interactive_children = false hides a static child */
    br_node r = br_node_create_container(e);
    br_node_add_child(e, root, r);
    make_box(e, r, "child", 400, 0, 50, 50, BR_EVENT_MODE_STATIC);
    BR_CHECK(SDL_strcmp(hit_label(e, 420, 20), "child") == 0);
    br_node_set_interactive_children(e, r, false);
    BR_CHECK(SDL_strcmp(hit_label(e, 420, 20), "-") == 0);
    /* a parent's hit area prunes its children outside it */
    br_node s = br_node_create_container(e);
    br_node_add_child(e, root, s);
    make_box(e, s, "inside", 600, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_hit_area area;
    SDL_zero(area);
    area.type = BR_HIT_AREA_RECT;
    area.rect = br_rect_make(600, 0, 50, 50);
    br_node_set_hit_area(e, s, &area);
    BR_CHECK(SDL_strcmp(hit_label(e, 620, 20), "inside") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 680, 80), "-") == 0);
    br_engine_destroy(e);
}

static void test_hit_masks(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    /* scissor: content 0..100, mask box 0..50 */
    br_node group = br_node_create_container(e);
    br_node_add_child(e, root, group);
    make_box(e, group, "sc", 0, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_node smask = make_box(e, root, "smask", 0, 0, 50, 50, BR_EVENT_MODE_STATIC);
    br_node_set_mask(e, group, smask, BR_MASK_SCISSOR, false);
    BR_CHECK(SDL_strcmp(hit_label(e, 25, 25), "sc") == 0);   /* the mask itself is not hittable */
    BR_CHECK(SDL_strcmp(hit_label(e, 75, 75), "-") == 0);

    /* stencil: a rotated mask box; inverse flips it */
    br_node group2 = br_node_create_container(e);
    br_node_add_child(e, root, group2);
    make_box(e, group2, "st", 200, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_node mask = br_node_create_container(e);
    br_node_add_child(e, root, mask);
    br_node mbox = make_box(e, mask, "mbox", 250, 0, 50, 50, BR_EVENT_MODE_PASSIVE);
    (void)mbox;
    br_node_set_mask(e, group2, mask, BR_MASK_STENCIL, false);
    BR_CHECK(SDL_strcmp(hit_label(e, 275, 25), "st") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 225, 25), "-") == 0);
    br_node_set_mask(e, group2, mask, BR_MASK_STENCIL, true);
    BR_CHECK(SDL_strcmp(hit_label(e, 275, 25), "-") == 0);
    BR_CHECK(SDL_strcmp(hit_label(e, 225, 25), "st") == 0);
    br_engine_destroy(e);
}

/* A chain of `depth` boxes, each 10 x 10 and one unit right of its parent; node 5 is "n5". */
static br_node make_chain(br_engine *e, br_node parent, uint32_t depth, float x, br_event_mode mode)
{
    br_node top = make_box(e, parent, "top", x, 0, 10, 10, mode);
    br_node p = top;
    for (uint32_t i = 1; i < depth; i++) {
        p = make_box(e, p, i == 5 ? "n5" : "", 1, 0, 10, 10, mode);
    }
    return top;
}

/* 50,000 levels: the hit test and the mask geometry test must not overflow the stack. */
static void test_hit_deep_chain(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    make_chain(e, root, 50000, 0, BR_EVENT_MODE_STATIC);
    BR_CHECK(SDL_strcmp(hit_label(e, 5, 5), "n5") == 0); /* the deepest box under the point */

    br_node group = br_node_create_container(e);
    br_node_add_child(e, root, group);
    make_box(e, group, "masked", 0, 100, 50, 50, BR_EVENT_MODE_STATIC);
    br_node mask = make_chain(e, root, 50000, 200, BR_EVENT_MODE_PASSIVE); /* far from the point */
    br_node_set_mask(e, group, mask, BR_MASK_STENCIL, false);
    BR_CHECK(SDL_strcmp(hit_label(e, 25, 125), "-") == 0);
    br_engine_destroy(e);
}

/* ---- propagation */

static void stop_listener(br_engine *e, br_event *ev, void *user)
{
    log_event(e, ev, NULL);
    if (user != NULL) {
        br_event_stop_immediate_propagation(ev);
    } else {
        br_event_stop_propagation(ev);
    }
}

static void test_propagation(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node_set_label(e, root, "root");
    br_node p = make_box(e, root, "p", 0, 0, 200, 200, BR_EVENT_MODE_STATIC);
    br_node c = make_box(e, p, "c", 0, 0, 50, 50, BR_EVENT_MODE_STATIC);
    br_node nodes[3] = { root, p, c };
    for (int i = 0; i < 3; i++) {
        br_node_on_capture(e, nodes[i], BR_EVENT_POINTER_DOWN, log_event, "cap-");
        br_node_on(e, nodes[i], BR_EVENT_POINTER_DOWN, log_event, NULL);
    }
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("cap-down:root cap-down:p cap-down:c down:c down:p down:root "));
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);

    /* stop_propagation finishes c's listeners, then stops */
    br_node_off(e, c, BR_EVENT_POINTER_DOWN, log_event, NULL);
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, stop_listener, NULL);
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, log_event, "2nd-");
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("cap-down:root cap-down:p cap-down:c down:c 2nd-down:c "));
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);

    /* immediate: the second listener of c is skipped too */
    br_node_off(e, c, BR_EVENT_POINTER_DOWN, stop_listener, NULL);
    br_node_off(e, c, BR_EVENT_POINTER_DOWN, log_event, "2nd-");
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, stop_listener, "immediate");
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, log_event, "2nd-");
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("cap-down:root cap-down:p cap-down:c down:c "));
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);

    /* stop during capture: nothing below runs */
    br_node_off_all(e, c);
    br_node_on_capture(e, p, BR_EVENT_POINTER_DOWN, stop_listener, NULL);
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("cap-down:root cap-down:p down:p "));
    br_engine_destroy(e);
}

/* ---- hover sequences, clicks */

static void listen_all(br_engine *e, br_node n)
{
    br_event_type types[] = { BR_EVENT_POINTER_OVER, BR_EVENT_POINTER_OUT, BR_EVENT_POINTER_ENTER,
                              BR_EVENT_POINTER_LEAVE, BR_EVENT_POINTER_UP_OUTSIDE, BR_EVENT_CLICK,
                              BR_EVENT_RIGHT_CLICK, BR_EVENT_TAP };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(types); i++) {
        br_node_on(e, n, types[i], log_event, NULL);
    }
}

static void test_hover_and_click(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node_set_label(e, root, "root");
    br_node a = make_box(e, root, "A", 0, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_node a1 = make_box(e, a, "A1", 0, 0, 50, 50, BR_EVENT_MODE_STATIC);
    br_node b = make_box(e, root, "B", 200, 200, 100, 100, BR_EVENT_MODE_STATIC);
    listen_all(e, root);
    listen_all(e, a);
    listen_all(e, a1);
    listen_all(e, b);

    clear_log();
    br_input_inject_pointer_move(e, 75, 75);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(log_is("over:A over:root enter:root enter:A "));
    BR_CHECK(br_node_equal(br_events_hovered(e), a));
    clear_log();
    br_input_inject_pointer_move(e, 25, 25);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(log_is("out:A out:root over:A1 over:A over:root enter:A1 "));
    clear_log();
    br_input_inject_pointer_move(e, 250, 250);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(log_is("out:A1 out:A out:root leave:A1 leave:A over:B over:root enter:B "));
    clear_log();
    br_input_inject_pointer_move(e, 250, 260);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(log_is(""));
    clear_log();
    br_input_inject_pointer_move(e, 900, 900);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(log_is("out:B out:root leave:B leave:root "));

    /* click on the same node; up outside goes to the pressed node's ancestors, click to the common one */
    br_input_inject_pointer_move(e, 25, 25);
    br_events_flush(e); /* motion is coalesced per frame */
    clear_log();
    br_input_inject_pointer_down(e, 25, 25, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 25, 25, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("click:A1 click:A click:root "));
    clear_log();
    br_input_inject_pointer_down(e, 25, 25, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 75, 75, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("upoutside:A1 click:A click:root "));
    clear_log();
    br_input_inject_pointer_down(e, 25, 25, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 250, 250, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("upoutside:A1 upoutside:A click:root "));
    clear_log();
    br_input_inject_pointer_down(e, 25, 25, SDL_BUTTON_RIGHT);
    br_input_inject_pointer_up(e, 25, 25, SDL_BUTTON_RIGHT);
    BR_CHECK(log_is("rightclick:A1 rightclick:A rightclick:root "));

    /* touch: over/enter on down, tap, out/leave on up */
    br_input_inject_pointer_move(e, 900, 900);
    br_events_flush(e); /* motion is coalesced per frame */
    clear_log();
    br_input_inject_touch(e, SDL_EVENT_FINGER_DOWN, 7, 250, 250);
    br_input_inject_touch(e, SDL_EVENT_FINGER_UP, 7, 250, 250);
    BR_CHECK(log_is("over:B over:root enter:root enter:B tap:B tap:root out:B out:root leave:B leave:root "));
    BR_CHECK(br_input_touch_count(e) == 0);
    br_engine_destroy(e);
}

static br_vec2 g_wheel;
static br_vec2 g_local;
static int g_global_moves;

static void wheel_listener(br_engine *e, br_event *ev, void *user)
{
    (void)user;
    g_wheel = ev->wheel;
    g_local = br_event_local(e, ev);
}

static void global_listener(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    g_global_moves++;
}

static void test_wheel_and_global_move(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node a = make_box(e, root, "A", 100, 100, 100, 100, BR_EVENT_MODE_STATIC);
    br_node far = make_box(e, root, "far", 1000, 1000, 10, 10, BR_EVENT_MODE_STATIC);
    br_node_on(e, a, BR_EVENT_WHEEL, wheel_listener, NULL);
    br_node_on(e, far, BR_EVENT_GLOBAL_MOVE, global_listener, NULL);
    br_input_inject_pointer_move(e, 150, 120);
    br_events_flush(e); /* motion is coalesced per frame */
    br_input_inject_wheel(e, 0.0f, 2.0f);
    BR_CHECK(g_wheel.y == 2.0f);
    BR_CHECK(g_local.x == 50.0f && g_local.y == 20.0f);
    BR_CHECK(g_global_moves == 1);   /* "far" was not hit but still hears every move */
    br_input_inject_pointer_move(e, 5, 5);
    br_events_flush(e); /* motion is coalesced per frame */
    BR_CHECK(g_global_moves == 2);
    br_engine_destroy(e);
}

/* 16 motion events in one frame (a 1000 Hz mouse) are one hit test and one dispatch; a press
 * flushes the pending move first, so it sees where the mouse went. */
static void test_move_coalescing(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node a = make_box(e, root, "A", 100, 100, 100, 100, BR_EVENT_MODE_STATIC);
    br_node far = make_box(e, root, "far", 1000, 1000, 10, 10, BR_EVENT_MODE_STATIC);
    br_node_on(e, far, BR_EVENT_GLOBAL_MOVE, global_listener, NULL);
    g_global_moves = 0;
    for (int i = 0; i < 16; i++) {
        br_input_inject_pointer_move(e, 10.0f + (float)i, 10.0f);
    }
    BR_CHECK(g_global_moves == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_global_moves == 1);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_global_moves == 1); /* still mouse, no DYNAMIC node: nothing replayed */
    listen_all(e, a);
    clear_log();
    br_input_inject_pointer_move(e, 150, 150);
    br_input_inject_pointer_down(e, 150, 150, SDL_BUTTON_LEFT);
    BR_CHECK(log_is("over:A enter:A ")); /* hovered before the press is handled */
    br_engine_destroy(e);
}

/* Removing the listeners of many nodes (a UI scene unloading) is linear: it used to compact the
 * whole table on every removal (16000 nodes: 1.3 s). */
static void test_listener_removal_cost(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    enum { COUNT = 16000 };
    static br_node nodes[COUNT];
    for (int i = 0; i < COUNT; i++) {
        nodes[i] = br_node_create_container(e);
        br_node_add_child(e, root, nodes[i]);
        br_node_on(e, nodes[i], BR_EVENT_CLICK, log_event, "x");
    }
    uint64_t start = SDL_GetTicksNS();
    for (int i = 0; i < COUNT; i++) {
        br_node_off_all(e, nodes[i]);
    }
    double ms = (double)(SDL_GetTicksNS() - start) / 1e6;
    BR_CHECK(ms < 100.0); /* linear is ~1 ms; the quadratic version took over a second */
    BR_CHECK(br_node_listener_count(e, nodes[0]) == 0 && br_node_listener_count(e, nodes[COUNT - 1]) == 0);
    if (ms >= 100.0) {
        printf("removing %d listeners took %.1f ms\n", COUNT, ms);
    }
    br_engine_destroy(e);
}

/* ---- listener and node lifetime during dispatch */

typedef struct lifetime_ctx {
    br_node victim;
    br_node other;
    int     calls;
    int     late_calls;
} lifetime_ctx;

static void late_listener(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    lifetime_ctx *ctx = user;
    ctx->late_calls++;
}

static void removing_listener(br_engine *e, br_event *ev, void *user)
{
    lifetime_ctx *ctx = user;
    ctx->calls++;
    /* removes the next listener and adds one: neither runs in this dispatch */
    br_node_off(e, ev->current_target, BR_EVENT_POINTER_DOWN, log_event, NULL);
    br_node_on(e, ev->current_target, BR_EVENT_POINTER_DOWN, late_listener, ctx);
}

static void destroying_listener(br_engine *e, br_event *ev, void *user)
{
    (void)ev;
    lifetime_ctx *ctx = user;
    ctx->calls++;
    br_node_destroy(e, ctx->victim);
    /* a node created now may reuse the freed slot: it must not inherit listeners */
    ctx->other = br_node_create_container(e);
}

static void once_listener(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    int *count = user;
    (*count)++;
}

static void destroy_in_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    lifetime_ctx *ctx = user;
    if (br_node_valid(e, ctx->victim)) {
        br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
        br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);
    }
}

static void test_lifetime(void)
{
    br_engine *e = make_engine();
    br_node root = br_scene_root(e);
    br_node p = make_box(e, root, "p", 0, 0, 100, 100, BR_EVENT_MODE_STATIC);
    br_node c = make_box(e, p, "c", 0, 0, 50, 50, BR_EVENT_MODE_STATIC);

    /* removal and addition during dispatch */
    lifetime_ctx ctx;
    SDL_zero(ctx);
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, removing_listener, &ctx);
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, log_event, NULL);
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(ctx.calls == 1 && ctx.late_calls == 0 && log_is(""));
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);
    br_node_off(e, c, BR_EVENT_POINTER_DOWN, removing_listener, &ctx);
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(ctx.late_calls == 1);
    BR_CHECK(br_node_listener_count(e, c) == 1);
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);

    /* once */
    int once_count = 0;
    br_node_once(e, c, BR_EVENT_CLICK, once_listener, &once_count);
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(once_count == 1);

    /* destroying the target (immediate: we are outside a step) and its parent mid-dispatch */
    br_node_off_all(e, c);
    SDL_zero(ctx);
    ctx.victim = p;
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, destroying_listener, &ctx);
    br_node_on(e, c, BR_EVENT_POINTER_DOWN, log_event, NULL);
    br_node_on(e, p, BR_EVENT_POINTER_DOWN, log_event, NULL);
    br_node_set_label(e, root, "root");
    br_node_on(e, root, BR_EVENT_POINTER_DOWN, log_event, NULL);
    clear_log();
    br_input_inject_pointer_down(e, 10, 10, SDL_BUTTON_LEFT);
    BR_CHECK(ctx.calls == 1);
    BR_CHECK(log_is("down:root "));   /* c's second listener and p were skipped */
    BR_CHECK(!br_node_valid(e, c) && !br_node_valid(e, p));
    BR_CHECK(br_node_listener_count(e, c) == 0);
    BR_CHECK(br_node_listener_count(e, ctx.other) == 0);
    br_input_inject_pointer_up(e, 10, 10, SDL_BUTTON_LEFT);
    br_node_off_all(e, root);

    /* destroying during a step (deferred) from a click listener, then flushing */
    br_node q = make_box(e, root, "q", 0, 0, 50, 50, BR_EVENT_MODE_STATIC);
    SDL_zero(ctx);
    ctx.victim = q;
    br_node_on(e, q, BR_EVENT_CLICK, destroying_listener, &ctx);
    br_node_on(e, q, BR_EVENT_CLICK, log_event, NULL);
    br_system_add(e, "test.destroy", BR_PHASE_UPDATE, 0, destroy_in_system, &ctx);
    clear_log();
    br_engine_step(e, 1.0f / 60.0f);   /* the click destroys q: deferred, q's second listener skipped */
    br_engine_step(e, 1.0f / 60.0f);   /* flush at the start of this step */
    BR_CHECK(ctx.calls == 1);
    BR_CHECK(log_is(""));
    BR_CHECK(!br_node_valid(e, q));
    BR_CHECK(SDL_strcmp(hit_label(e, 10, 10), "-") == 0);
    br_engine_destroy(e);
}

/* ---- input state and actions */

static void send_event(br_engine *e, SDL_Event *ev)
{
    br_app_event(e, ev);
}

static void test_input_actions(void)
{
    br_engine *e = make_engine();
    const char *json_text =
        "{ \"jump\": { \"keys\": [\"Space\", \"W\"], \"buttons\": [\"a\"] },"
        "  \"left\": { \"keys\": [\"A\"], \"axes\": [\"-leftx\"] },"
        "  \"fire\": { \"mouse\": [1] },"
        "  \"bad\":  { \"keys\": [\"NoSuchKey\"] } }";
    cJSON *json = cJSON_Parse(json_text);
    BR_CHECK(br_input_actions_from_json(e, json));
    cJSON_Delete(json);
    BR_CHECK(!br_input_actions_from_json(e, NULL));

    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_key_down(e, SDL_SCANCODE_SPACE) && br_input_key_pressed(e, SDL_SCANCODE_SPACE));
    BR_CHECK(br_input_action_down(e, "jump") && br_input_action_pressed(e, "jump"));
    BR_CHECK(br_input_action_value(e, "jump") == 1.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_down(e, "jump") && !br_input_action_pressed(e, "jump"));
    BR_CHECK(!br_input_key_pressed(e, SDL_SCANCODE_SPACE));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_input_action_down(e, "jump") && br_input_action_released(e, "jump"));
    BR_CHECK(br_input_key_released(e, SDL_SCANCODE_SPACE));

    /* a tap between two frames still counts once */
    br_input_inject_key(e, SDL_SCANCODE_W, true);
    br_input_inject_key(e, SDL_SCANCODE_W, false);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_pressed(e, "jump"));
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_released(e, "jump") && !br_input_action_down(e, "jump"));

    /* mouse binding, position, delta */
    br_input_inject_pointer_move(e, 10, 20);
    br_input_inject_pointer_down(e, 10, 20, SDL_BUTTON_LEFT);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_pressed(e, "fire") && br_input_mouse_pressed(e, SDL_BUTTON_LEFT));
    BR_CHECK(br_input_mouse_position(e).x == 10.0f && br_input_mouse_delta(e).y == 20.0f);
    br_input_inject_pointer_up(e, 10, 20, SDL_BUTTON_LEFT);

    /* gamepad: hot-plug, button and axis bindings */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_ADDED;
    ev.gdevice.which = 77;
    send_event(e, &ev);
    BR_CHECK(br_input_gamepad_count(e) == 1 && br_input_gamepad_connected(e, 0));
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    ev.gaxis.which = 77;
    ev.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    ev.gaxis.value = -32767;
    send_event(e, &ev);
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    ev.gbutton.which = 77;
    ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    ev.gbutton.down = true;
    send_event(e, &ev);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_action_value(e, "left") == 1.0f && br_input_action_pressed(e, "left"));
    BR_CHECK(br_input_gamepad_axis(e, 0, SDL_GAMEPAD_AXIS_LEFTX) == -1.0f);
    BR_CHECK(br_input_gamepad_pressed(e, 0, SDL_GAMEPAD_BUTTON_SOUTH) && br_input_action_down(e, "jump"));
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_REMOVED;
    ev.gdevice.which = 77;
    send_event(e, &ev);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_gamepad_count(e) == 0 && !br_input_action_down(e, "left"));

    /* text input */
    SDL_zero(ev);
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = "hi";
    send_event(e, &ev);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(SDL_strcmp(br_input_text(e), "hi") == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_input_text(e)[0] == '\0');
    br_engine_destroy(e);
}

int main(void)
{
    test_hit_order();
    test_hit_modes();
    test_hit_masks();
    test_hit_deep_chain();
    test_propagation();
    test_hover_and_click();
    test_wheel_and_global_move();
    test_move_coalescing();
    test_listener_removal_cost();
    test_lifetime();
    test_input_actions();
    return br_test_finish();
}
