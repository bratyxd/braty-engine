/* events_demo - pointer events on scene nodes.
 *
 *   events_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Top row: four buttons (hover = lighter, pressed = darker and smaller, each click adds a dot).
 * Middle left: a panel that lights up on ENTER/LEAVE; its two cards are buttons too, so moving
 *   between them does not make the panel leave.
 * Middle right: three overlapping cards in a sortable parent; clicking one brings it to the front.
 * Bottom left: a scissor-masked list scrolled with the wheel; rows outside the mask are not hit.
 * Bottom right: a box dragged with DOWN + GLOBAL_MOVE + UP.
 *
 * With --frames the demo injects a fixed input script (br_input_inject_*), so the screenshot is
 * reproducible: button 0 clicked twice, button 1 once, button 2 held down by a finger, the list
 * scrolled, the drag box moved, the back card raised, and the mouse resting on a panel card.
 */
#include "braty/braty_main.h"

static bool g_scripted = false;

enum { BUTTON_COUNT = 4, DOTS = 8, CARD_COUNT = 3, ROW_COUNT = 8 };

typedef struct demo_button {
    br_color base;
    bool     hovered;
    bool     pressed;
    int      clicks;
} demo_button;

typedef struct demo_state {
    br_texture white;
    uint32_t   button_component;
    br_node    buttons[BUTTON_COUNT];
    br_node    dots[BUTTON_COUNT][DOTS];
    br_node    panel;
    br_node    panel_bg;
    br_node    card_parent;
    int        top_z;
    br_node    list;
    float      list_scroll;
    br_node    drag_box;
    bool       dragging;
    br_vec2    drag_offset;
} demo_state;

/* ---- helpers */

static br_node make_rect(demo_state *s, br_engine *e, br_node parent, float x, float y, float w, float h, uint32_t rgb)
{
    br_node n = br_sprite_create(e, s->white);
    br_sprite_set_size(e, n, w, h);
    br_node_set_position(e, n, x, y);
    br_node_set_tint(e, n, br_color_from_hex(rgb, 1.0f));
    br_node_add_child(e, parent, n);
    return n;
}

/* ---- buttons: a component + listeners write state, a system turns it into looks */

static void button_event(br_engine *e, br_event *ev, void *user)
{
    demo_state *s = user;
    demo_button *b = br_component_get(e, ev->current_target, s->button_component);
    if (b == NULL) {
        return;
    }
    switch (ev->type) {
    case BR_EVENT_POINTER_OVER: b->hovered = true; break;
    case BR_EVENT_POINTER_OUT: b->hovered = false; break;
    case BR_EVENT_POINTER_DOWN: b->pressed = true; break;
    case BR_EVENT_POINTER_UP:
    case BR_EVENT_POINTER_UP_OUTSIDE: b->pressed = false; break;
    case BR_EVENT_CLICK:
    case BR_EVENT_TAP: b->clicks++; break;
    default: break;
    }
    br_event_stop_propagation(ev);   /* a card's click must not also raise its parent */
}

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

static void button_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    uint32_t count = br_component_count(e, s->button_component);
    for (uint32_t i = 0; i < count; i++) {
        demo_button *b = br_component_at(e, s->button_component, i);
        br_node n = br_component_owner_at(e, s->button_component, i);
        br_color c = b->base;
        if (b->pressed) {
            c = br_color_lerp(b->base, br_color_make(0, 0, 0, 1), 0.45f);
        } else if (b->hovered) {
            c = br_color_lerp(b->base, br_color_make(1, 1, 1, 1), 0.45f);
        }
        br_node_set_tint(e, n, c);
        float scale = b->pressed ? 0.9f : 1.0f;
        br_node_set_scale(e, n, scale, scale);
    }
    for (int i = 0; i < BUTTON_COUNT; i++) {
        demo_button *b = br_component_get(e, s->buttons[i], s->button_component);
        for (int d = 0; d < DOTS; d++) {
            br_node_set_visible(e, s->dots[i][d], b != NULL && d < b->clicks);
        }
    }
}

/* ---- panel, cards, list, drag box */

static void panel_event(br_engine *e, br_event *ev, void *user)
{
    demo_state *s = user;
    bool inside = ev->type == BR_EVENT_POINTER_ENTER;
    br_node_set_tint(e, s->panel_bg, br_color_from_hex(inside ? 0xf0c040 : 0x404858, 1.0f));
}

static void card_event(br_engine *e, br_event *ev, void *user)
{
    demo_state *s = user;
    s->top_z++;
    br_node_set_z_index(e, ev->current_target, s->top_z);
}

static void list_wheel(br_engine *e, br_event *ev, void *user)
{
    demo_state *s = user;
    s->list_scroll = br_clampf(s->list_scroll + ev->wheel.y * 40.0f, -(ROW_COUNT * 50.0f - 200.0f), 0.0f);
    br_node_set_position(e, s->list, 0.0f, s->list_scroll);
}

static void drag_event(br_engine *e, br_event *ev, void *user)
{
    demo_state *s = user;
    br_vec2 pos = br_node_position(e, s->drag_box);
    if (ev->type == BR_EVENT_POINTER_DOWN) {
        s->dragging = true;
        s->drag_offset = br_vec2_sub(ev->global, pos);
    } else if (ev->type == BR_EVENT_GLOBAL_MOVE && s->dragging) {
        br_vec2 p = br_vec2_sub(ev->global, s->drag_offset);
        br_node_set_position(e, s->drag_box, p.x, p.y);
    } else if (ev->type == BR_EVENT_POINTER_UP || ev->type == BR_EVENT_POINTER_UP_OUTSIDE) {
        s->dragging = false;
    }
}

/* ---- the scripted input (deterministic screenshots) */

static void script_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    uint64_t f = br_ticker_frame(e);
    if (f == 2) {   /* scroll the list by two rows */
        br_input_inject_pointer_move(e, 150, 560);
        br_input_inject_wheel(e, 0, -1);
        br_input_inject_wheel(e, 0, -1);
    } else if (f == 4 || f == 6) {   /* two clicks on button 0 */
        br_input_inject_pointer_move(e, 120, 90);
        br_input_inject_pointer_down(e, 120, 90, SDL_BUTTON_LEFT);
        br_input_inject_pointer_up(e, 120, 90, SDL_BUTTON_LEFT);
    } else if (f == 8) {   /* one click on button 1 */
        br_input_inject_pointer_move(e, 310, 90);
        br_input_inject_pointer_down(e, 310, 90, SDL_BUTTON_LEFT);
        br_input_inject_pointer_up(e, 310, 90, SDL_BUTTON_LEFT);
    } else if (f == 10) {   /* drag the box 200 px left and 60 px up */
        br_input_inject_pointer_move(e, 1100, 560);
        br_input_inject_pointer_down(e, 1100, 560, SDL_BUTTON_LEFT);
    } else if (f == 12) {
        br_input_inject_pointer_move(e, 1000, 530);
    } else if (f == 14) {
        br_input_inject_pointer_move(e, 900, 500);
        br_input_inject_pointer_up(e, 900, 500, SDL_BUTTON_LEFT);
    } else if (f == 16) {   /* raise the back card (only its left strip is visible) */
        br_input_inject_pointer_down(e, 770, 300, SDL_BUTTON_LEFT);
        br_input_inject_pointer_up(e, 770, 300, SDL_BUTTON_LEFT);
    } else if (f == 18) {   /* a finger holds button 2 down */
        br_input_inject_touch(e, SDL_EVENT_FINGER_DOWN, 1, 500, 90);
    } else if (f == 20) {   /* the mouse rests on the panel's second card */
        br_input_inject_pointer_move(e, 330, 330);
    }
}

/* ---- setup */

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    s->white = br_texture_white(e);
    br_component_desc desc = { "demo.button", sizeof(demo_button), NULL, 0 };
    s->button_component = br_component_register(e, &desc);
    br_node root = br_stage_root(e);
    make_rect(s, e, root, 0, 0, 1280, 720, 0x1c2030);   /* background (passive: not a target) */

    /* buttons + click dots */
    uint32_t colors[BUTTON_COUNT] = { 0x3a7bd5, 0x2fa36b, 0xd5583a, 0x9b59b6 };
    for (int i = 0; i < BUTTON_COUNT; i++) {
        float x = 40.0f + (float)i * 190.0f;
        s->buttons[i] = make_button(s, e, root, x, 60, 160, 60, colors[i]);
        for (int d = 0; d < DOTS; d++) {
            s->dots[i][d] = make_rect(s, e, root, x + (float)d * 20.0f, 130, 14, 14, 0xffffff);
        }
    }

    /* panel with ENTER/LEAVE and two button cards inside it */
    s->panel = br_node_create_container(e);
    br_node_set_position(e, s->panel, 40, 200);
    br_node_add_child(e, root, s->panel);
    br_node_set_event_mode(e, s->panel, BR_EVENT_MODE_STATIC);
    br_node_on(e, s->panel, BR_EVENT_POINTER_ENTER, panel_event, s);
    br_node_on(e, s->panel, BR_EVENT_POINTER_LEAVE, panel_event, s);
    s->panel_bg = make_rect(s, e, s->panel, 0, 0, 420, 200, 0x404858);
    make_button(s, e, s->panel, 30, 40, 150, 120, 0x2c8fa0);
    make_button(s, e, s->panel, 220, 40, 150, 120, 0x2c8fa0);

    /* overlapping cards, click to raise */
    s->card_parent = br_node_create_container(e);
    br_node_set_sortable_children(e, s->card_parent, true);
    br_node_add_child(e, root, s->card_parent);
    uint32_t card_colors[CARD_COUNT] = { 0xe0a030, 0x30a0e0, 0xe03080 };
    for (int i = 0; i < CARD_COUNT; i++) {
        br_node c = make_rect(s, e, s->card_parent, 760.0f + (float)i * 60.0f, 220.0f + (float)i * 30.0f, 220, 150,
                              card_colors[i]);
        br_node_set_event_mode(e, c, BR_EVENT_MODE_STATIC);
        br_node_set_cursor(e, c, SDL_SYSTEM_CURSOR_POINTER);
        br_node_on(e, c, BR_EVENT_CLICK, card_event, s);
    }

    /* scissor-masked list scrolled by the wheel */
    br_node viewport = br_node_create_container(e);
    br_node_set_position(e, viewport, 40, 460);
    br_node_add_child(e, root, viewport);
    br_hit_area area;
    SDL_zero(area);
    area.type = BR_HIT_AREA_RECT;
    area.rect = br_rect_make(0, 0, 300, 200);
    br_node_set_hit_area(e, viewport, &area);
    br_node_set_event_mode(e, viewport, BR_EVENT_MODE_STATIC);
    br_node_on(e, viewport, BR_EVENT_WHEEL, list_wheel, s);
    br_node mask = make_rect(s, e, viewport, 0, 0, 300, 200, 0xffffff);
    s->list = br_node_create_container(e);
    br_node_add_child(e, viewport, s->list);
    br_node_set_mask(e, s->list, mask, BR_MASK_SCISSOR, false);
    for (int i = 0; i < ROW_COUNT; i++) {
        uint32_t rgb = (i % 2) == 0 ? 0x5a6378 : 0x4a5266;
        make_button(s, e, s->list, 0, (float)i * 50.0f, 300, 46, rgb);
    }

    /* drag box */
    s->drag_box = make_rect(s, e, root, 1060, 520, 100, 100, 0x50d080);
    br_node_set_event_mode(e, s->drag_box, BR_EVENT_MODE_STATIC);
    br_node_set_cursor(e, s->drag_box, SDL_SYSTEM_CURSOR_MOVE);
    br_node_on(e, s->drag_box, BR_EVENT_POINTER_DOWN, drag_event, s);
    br_node_on(e, s->drag_box, BR_EVENT_GLOBAL_MOVE, drag_event, s);
    br_node_on(e, s->drag_box, BR_EVENT_POINTER_UP, drag_event, s);
    br_node_on(e, s->drag_box, BR_EVENT_POINTER_UP_OUTSIDE, drag_event, s);

    br_system_add(e, "demo.buttons", BR_PHASE_UPDATE, 0, button_system, s);
    if (g_scripted) {
        br_system_add(e, "demo.script", BR_PHASE_INPUT, 0, script_system, s);
    }
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--frames") == 0) {
            g_scripted = true;
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - events";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
