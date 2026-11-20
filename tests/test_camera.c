/* test_camera - Unity-style camera maths (extents at 16:9, 21:9, 9:16 for every fit policy,
 * screen <-> world round trips for 2D and 3D, ortho and perspective), follow / shake / anchor
 * maths, and cameras in a headless engine (pointer mapping, culling mask, render views). */
#include "braty/braty.h"
#include "br_test.h"

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-3f * SDL_max(1.0f, SDL_fabsf(b));
}

static bool vec3_eq(br_vec3 a, br_vec3 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y) && near_eq(a.z, b.z);
}

static const br_vec2 SCREENS[3] = { { 1280.0f, 720.0f }, { 2520.0f, 1080.0f }, { 720.0f, 1280.0f } }; /* 16:9 21:9 9:16 */

static void test_extents_2d(void)
{
    br_camera c = br_camera_default_2d();
    c.orthographic_size = 5.0f;
    c.pixels_per_unit = 100.0f;
    for (int i = 0; i < 3; i++) {
        br_vec2 s = SCREENS[i];
        float aspect = s.x / s.y;
        br_camera_view v = br_camera_compute_view(&c, br_mat4_identity(), false, s, 1.0f);
        /* vertical extent constant: 2 * size * ppu stage units; horizontal follows the aspect */
        BR_CHECK(near_eq(v.half_height, 500.0f));
        BR_CHECK(near_eq(v.half_width, 500.0f * aspect));
        BR_CHECK(near_eq(v.visible.height, 1000.0f));
        BR_CHECK(near_eq(v.visible.width, 1000.0f * aspect));
        /* top edge of the screen is world y = -500 (y down), centre is the camera */
        br_vec3 top = br_camera_view_screen_to_world(&v, br_vec3_make(s.x * 0.5f, 0.0f, 0.0f));
        BR_CHECK(near_eq(top.y, -500.0f));
        BR_CHECK(near_eq(top.x, 0.0f));
    }
}

static void test_fit_policies(void)
{
    float ref = 16.0f / 9.0f;
    for (int i = 0; i < 3; i++) {
        float a = SCREENS[i].x / SCREENS[i].y;
        br_vec2 h = br_camera_fit_extents(BR_CAMERA_FIT_HEIGHT, 1.0f, a, ref);
        BR_CHECK(near_eq(h.y, 1.0f) && near_eq(h.x, a));
        br_vec2 w = br_camera_fit_extents(BR_CAMERA_FIT_WIDTH, 1.0f, a, ref);
        BR_CHECK(near_eq(w.x, ref) && near_eq(w.y, ref / a));
        br_vec2 x = br_camera_fit_extents(BR_CAMERA_FIT_EXPAND, 1.0f, a, ref);
        BR_CHECK(x.x >= ref - 1e-4f && x.y >= 1.0f - 1e-4f); /* the design rect always fits */
        BR_CHECK(near_eq(x.x / x.y, a));
    }
    /* letterbox: the viewport shrinks to the design aspect, centred */
    br_camera c = br_camera_default_2d();
    c.fit = BR_CAMERA_FIT_LETTERBOX;
    c.orthographic_size = 360.0f;
    br_camera_view wide = br_camera_compute_view(&c, br_mat4_identity(), false, SCREENS[1], 1.0f);
    BR_CHECK(near_eq(wide.viewport.height, 1080.0f));
    BR_CHECK(near_eq(wide.viewport.width, 1920.0f));
    BR_CHECK(near_eq(wide.viewport.x, 300.0f));
    BR_CHECK(near_eq(wide.half_width, 640.0f) && near_eq(wide.half_height, 360.0f));
    br_camera_view tall = br_camera_compute_view(&c, br_mat4_identity(), false, SCREENS[2], 1.0f);
    BR_CHECK(near_eq(tall.viewport.width, 720.0f));
    BR_CHECK(near_eq(tall.viewport.height, 405.0f));
    BR_CHECK(near_eq(tall.viewport.y, (1280.0f - 405.0f) * 0.5f));
}

static void test_extents_3d(void)
{
    br_camera c = br_camera_default_3d();
    c.field_of_view = 60.0f * BR_DEG_TO_RAD;
    br_mat4 world = br_mat4_translation(br_vec3_make(0.0f, 0.0f, 10.0f)); /* looks down -z at the origin */
    for (int i = 0; i < 3; i++) {
        br_vec2 s = SCREENS[i];
        float aspect = s.x / s.y;
        br_camera_view v = br_camera_compute_view(&c, world, true, s, 1.0f);
        float top = 10.0f * SDL_tanf(30.0f * BR_DEG_TO_RAD);
        br_vec3 p = br_camera_view_world_to_screen(&v, br_vec3_make(0.0f, top, 0.0f));
        BR_CHECK(near_eq(p.y, 0.0f));            /* vertical fov fixed at every aspect */
        BR_CHECK(near_eq(p.z, 10.0f));
        br_vec3 r = br_camera_view_world_to_screen(&v, br_vec3_make(top * aspect, 0.0f, 0.0f));
        BR_CHECK(near_eq(r.x, s.x));             /* horizontal follows */
    }
    c.projection = BR_CAMERA_ORTHOGRAPHIC;
    c.orthographic_size = 4.0f;
    for (int i = 0; i < 3; i++) {
        br_camera_view v = br_camera_compute_view(&c, world, true, SCREENS[i], 1.0f);
        BR_CHECK(near_eq(br_camera_view_world_to_screen(&v, br_vec3_make(0.0f, 4.0f, -3.0f)).y, 0.0f));
        BR_CHECK(near_eq(v.half_width, 4.0f * SCREENS[i].x / SCREENS[i].y));
    }
}

static void test_round_trips(void)
{
    /* 2D: rotated, offset camera in a half-screen viewport */
    br_camera c = br_camera_default_2d();
    c.viewport_rect = br_rect_make(0.5f, 0.0f, 0.5f, 1.0f);
    c.orthographic_size = 200.0f;
    br_mat4 world = br_mat4_from_mat3(br_mat3_mul(br_mat3_translation(300.0f, -40.0f), br_mat3_rotation(0.3f)));
    br_camera_view v = br_camera_compute_view(&c, world, false, br_vec2_make(1280.0f, 720.0f), 1.0f);
    br_vec3 centre = br_camera_view_world_to_screen(&v, br_vec3_make(300.0f, -40.0f, 0.0f));
    BR_CHECK(near_eq(centre.x, 960.0f) && near_eq(centre.y, 360.0f)); /* the camera sits in its viewport's middle */
    br_vec3 w = br_vec3_make(123.0f, 45.0f, 0.0f);
    BR_CHECK(vec3_eq(br_camera_view_screen_to_world(&v, br_camera_view_world_to_screen(&v, w)), w));
    br_vec3 sp = br_vec3_make(1000.0f, 100.0f, 0.0f);
    BR_CHECK(vec3_eq(br_camera_view_world_to_screen(&v, br_camera_view_screen_to_world(&v, sp)), sp));
    br_vec3 corner = br_camera_view_viewport_to_world(&v, br_vec3_make(0.5f, 0.5f, 0.0f));
    BR_CHECK(near_eq(corner.x, 300.0f) && near_eq(corner.y, -40.0f));

    /* 3D perspective and orthographic: world -> screen -> world at the same distance */
    br_camera c3 = br_camera_default_3d();
    c3.viewport_rect = br_rect_make(0.0f, 0.5f, 1.0f, 0.5f);
    br_mat4 eye = br_mat4_invert(br_mat4_look_at(br_vec3_make(3.0f, 4.0f, 8.0f), br_vec3_make(0.0f, 0.0f, 0.0f),
                                                 br_vec3_make(0.0f, 1.0f, 0.0f)), NULL);
    for (int k = 0; k < 2; k++) {
        c3.projection = k == 0 ? BR_CAMERA_PERSPECTIVE : BR_CAMERA_ORTHOGRAPHIC;
        br_camera_view v3 = br_camera_compute_view(&c3, eye, true, br_vec2_make(1280.0f, 720.0f), 1.0f);
        br_vec3 p = br_vec3_make(0.5f, -0.25f, 1.0f);
        br_vec3 s = br_camera_view_world_to_screen(&v3, p);
        BR_CHECK(s.y >= 360.0f && s.y <= 720.0f); /* inside the bottom-half viewport */
        BR_CHECK(vec3_eq(br_camera_view_screen_to_world(&v3, s), p));
        br_ray3d ray = br_camera_view_screen_to_ray(&v3, br_vec2_make(s.x, s.y));
        br_vec3 to_p = br_vec3_normalize(br_vec3_sub(p, ray.origin));
        BR_CHECK(br_vec3_dot(to_p, br_vec3_normalize(ray.direction)) > 0.9999f); /* the ray goes through p */
    }
}

static void test_pixel_perfect(void)
{
    br_camera c = br_camera_default_2d();
    c.orthographic_size = 100.0f; /* 200 stage units on 720 pixels: zoom 3.6 -> 3 */
    c.pixel_perfect = true;
    br_mat4 world = br_mat4_translation(br_vec3_make(10.1f, 0.0f, 0.0f));
    br_camera_view v = br_camera_compute_view(&c, world, false, br_vec2_make(1280.0f, 720.0f), 1.0f);
    BR_CHECK(near_eq(v.half_height, 120.0f)); /* 720 / (2 * 3) */
    BR_CHECK(near_eq(v.position.x, 10.0f + 1.0f / 3.0f) || near_eq(v.position.x, 10.0f)); /* snapped to 1/3 */
    float snapped = v.position.x * 3.0f;
    BR_CHECK(near_eq(snapped, SDL_roundf(snapped)));
}

static void test_helpers(void)
{
    /* follow: inside the dead zone nothing moves; outside, it closes the gap to the zone edge */
    br_vec2 cam = br_vec2_make(0.0f, 0.0f);
    br_vec2 same = br_camera_follow_step(cam, br_vec2_make(5.0f, -5.0f), br_vec2_make(10.0f, 10.0f), 0.0f, 0.016f);
    BR_CHECK(near_eq(same.x, 0.0f) && near_eq(same.y, 0.0f));
    br_vec2 snap = br_camera_follow_step(cam, br_vec2_make(30.0f, 0.0f), br_vec2_make(10.0f, 10.0f), 0.0f, 0.016f);
    BR_CHECK(near_eq(snap.x, 20.0f));
    br_vec2 damped = br_camera_follow_step(cam, br_vec2_make(100.0f, 0.0f), br_vec2_make(0.0f, 0.0f), 0.5f, 0.5f);
    BR_CHECK(near_eq(damped.x, 100.0f * (1.0f - SDL_expf(-1.0f)))); /* one time constant: 63% */
    /* bounds: the view stays inside; a too-small bound centres */
    br_vec2 b = br_camera_clamp_to_bounds(br_vec2_make(-50.0f, 900.0f), br_vec2_make(100.0f, 50.0f),
                                          br_rect_make(0.0f, 0.0f, 1000.0f, 500.0f));
    BR_CHECK(near_eq(b.x, 100.0f) && near_eq(b.y, 450.0f));
    br_vec2 c = br_camera_clamp_to_bounds(br_vec2_make(7.0f, 7.0f), br_vec2_make(100.0f, 50.0f),
                                          br_rect_make(0.0f, 0.0f, 150.0f, 60.0f));
    BR_CHECK(near_eq(c.x, 75.0f) && near_eq(c.y, 30.0f));

    /* shake: zero without trauma, bounded by trauma^2 * max, deterministic for a seed */
    br_camera_shake sh;
    SDL_zero(sh);
    sh.max_offset = 10.0f;
    sh.max_angle = 0.1f;
    sh.frequency = 15.0f;
    sh.seed = 7;
    sh.time = 0.37f;
    br_vec3 zero = br_camera_shake_sample(&sh);
    BR_CHECK(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f);
    sh.trauma = 0.5f;
    br_vec3 a = br_camera_shake_sample(&sh);
    br_vec3 a2 = br_camera_shake_sample(&sh);
    BR_CHECK(a.x == a2.x && a.y == a2.y && a.z == a2.z);
    BR_CHECK(SDL_fabsf(a.x) <= 2.5f + 1e-4f && SDL_fabsf(a.z) <= 0.025f + 1e-5f);
    BR_CHECK(a.x != 0.0f || a.y != 0.0f);

    /* anchors: corners of the viewport, or of its intersection with the safe area */
    br_rect area = br_rect_make(0.0f, 0.0f, 1280.0f, 720.0f);
    br_rect safe = br_rect_make(40.0f, 0.0f, 1200.0f, 690.0f); /* a notch left and right, home bar below */
    br_vec2 tl = br_camera_anchor_point(area, safe, br_vec2_make(0.0f, 0.0f), true);
    br_vec2 br = br_camera_anchor_point(area, safe, br_vec2_make(1.0f, 1.0f), true);
    BR_CHECK(near_eq(tl.x, 40.0f) && near_eq(tl.y, 0.0f));
    BR_CHECK(near_eq(br.x, 1240.0f) && near_eq(br.y, 690.0f));
    br_vec2 raw = br_camera_anchor_point(area, safe, br_vec2_make(1.0f, 1.0f), false);
    BR_CHECK(near_eq(raw.x, 1280.0f) && near_eq(raw.y, 720.0f));
}

/* Cameras inside a headless engine: views, culling mask in pointer mapping, follow/shake/anchor systems. */
static void test_engine(void)
{
    br_engine *e = br_engine_create("test_camera", NULL);
    br_camera_set_headless_screen(e, 1280.0f, 720.0f);
    br_node root = br_stage_root(e);

    /* no camera: the stage is the screen */
    BR_CHECK(near_eq(br_camera_screen_to_stage(e, br_vec2_make(10.0f, 20.0f)).x, 10.0f));

    /* world camera (layer 1) zoomed 2x on (1000, 1000); UI camera (layer 2) at the default stage */
    br_camera wc = br_camera_default_2d();
    wc.orthographic_size = 180.0f;
    wc.culling_mask = 1u;
    br_node world_cam = br_camera_create(e, &wc);
    br_node_set_position(e, world_cam, 1000.0f, 1000.0f);
    br_node_add_child(e, root, world_cam);
    br_camera uc = br_camera_default_2d();
    uc.culling_mask = 2u;
    uc.depth = 1.0f;
    uc.clear_flags = BR_CAMERA_CLEAR_NOTHING;
    br_node ui_cam = br_camera_create(e, &uc);
    br_node_set_position(e, ui_cam, 640.0f, 360.0f);
    br_node_add_child(e, root, ui_cam);

    br_node button = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, button, 100.0f, 50.0f);
    br_node_set_position(e, button, 20.0f, 20.0f);
    br_node_set_layer_bits(e, button, 2u);
    br_node_set_event_mode(e, button, BR_EVENT_MODE_STATIC);
    br_node_add_child(e, root, button);
    BR_CHECK(br_node_layer_bits(e, br_node_create_container(e)) == 0u); /* inherits */
    br_engine_step(e, 1.0f / 60.0f);

    /* a point on the UI button maps through the UI camera (identity) */
    br_vec2 on_button = br_camera_screen_to_stage(e, br_vec2_make(50.0f, 40.0f));
    BR_CHECK(near_eq(on_button.x, 50.0f) && near_eq(on_button.y, 40.0f));
    BR_CHECK(br_node_equal(br_events_hit_test(e, on_button), button));
    /* elsewhere nothing of layer 2 is hit: the top-most camera still maps it (UI), the world below
     * camera maps the centre to its position */
    br_vec2 centre_world = br_vec2_make(br_camera_screen_to_world(e, world_cam, br_vec3_make(640.0f, 360.0f, 0.0f)).x,
                                        br_camera_screen_to_world(e, world_cam, br_vec3_make(640.0f, 360.0f, 0.0f)).y);
    BR_CHECK(near_eq(centre_world.x, 1000.0f) && near_eq(centre_world.y, 1000.0f));
    /* a world sprite (layer 1) under the screen centre is found through the world camera */
    br_node crate = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, crate, 40.0f, 40.0f);
    br_node_set_position(e, crate, 980.0f, 980.0f);
    br_node_set_event_mode(e, crate, BR_EVENT_MODE_STATIC);
    br_node_add_child(e, root, crate);
    br_engine_step(e, 1.0f / 60.0f);
    br_vec2 p = br_camera_screen_to_stage(e, br_vec2_make(640.0f, 360.0f));
    BR_CHECK(near_eq(p.x, 1000.0f) && near_eq(p.y, 1000.0f));
    BR_CHECK(br_node_equal(br_events_hit_test(e, p), crate));
    /* stage -> screen respects the culling mask: the crate is drawn by the world camera only */
    br_vec2 s = br_camera_stage_to_screen(e, crate, br_vec2_make(1000.0f, 1000.0f));
    BR_CHECK(near_eq(s.x, 640.0f) && near_eq(s.y, 360.0f));
    br_vec2 s_ui = br_camera_stage_to_screen(e, button, br_vec2_make(70.0f, 45.0f));
    BR_CHECK(near_eq(s_ui.x, 70.0f) && near_eq(s_ui.y, 45.0f));

    /* follow system moves the world camera toward the crate (snap, no dead zone) */
    br_camera_follow *f = br_component_add(e, world_cam, br_camera_follow_component(e));
    f->target = crate;
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(near_eq(br_node_position(e, world_cam).x, 980.0f));

    /* shake: deterministic offset that decays to zero */
    br_camera_shake_add(e, world_cam, 1.0f);
    br_engine_step(e, 1.0f / 60.0f);
    br_camera_shake *sh = br_component_get(e, world_cam, br_camera_shake_component(e));
    BR_CHECK(sh != NULL && sh->seed != 0 && sh->trauma < 1.0f);
    for (int i = 0; i < 120; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    sh = br_component_get(e, world_cam, br_camera_shake_component(e));
    BR_CHECK(sh != NULL && sh->trauma == 0.0f);

    /* anchor: a HUD node pinned to the bottom-right of the UI camera's viewport */
    br_node hud = br_node_create_container(e);
    br_node_add_child(e, root, hud);
    br_camera_anchor *a = br_component_add(e, hud, br_camera_anchor_component(e));
    a->camera = ui_cam;
    a->anchor = br_vec2_make(1.0f, 1.0f);
    a->offset = br_vec2_make(-16.0f, -16.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(near_eq(br_node_position(e, hud).x, 1264.0f) && near_eq(br_node_position(e, hud).y, 704.0f));
    /* portrait: the HUD follows the new corner, the world camera keeps its vertical extent */
    br_camera_set_headless_screen(e, 720.0f, 1280.0f);
    br_engine_step(e, 1.0f / 60.0f);
    /* the UI camera keeps 720 stage units vertically: its bottom-right is (640 + 360 * 9/16, 720) */
    BR_CHECK(near_eq(br_node_position(e, hud).x, 640.0f + 202.5f - 16.0f) && near_eq(br_node_position(e, hud).y, 704.0f));
    BR_CHECK(near_eq(br_camera_get_view(e, world_cam).half_height, 180.0f));

    /* destroying cameras goes back to the plain stage */
    br_node_destroy(e, world_cam);
    br_node_destroy(e, ui_cam);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(near_eq(br_camera_screen_to_stage(e, br_vec2_make(10.0f, 20.0f)).y, 20.0f));
    br_engine_destroy(e);
}

/* 3D camera: far_z = 0 (SDL_zero) reaches the camera3d guarded like the view maths; shake
 * defaults are world units, not stage units. */
static void test_engine_3d(void)
{
    br_engine *e = br_engine_create("test_camera_3d", NULL);
    br_camera_set_headless_screen(e, 1280.0f, 720.0f);
    br_node3d scene = br_node3d_create_container(e);
    br_camera c;
    SDL_zero(c);
    c.projection = BR_CAMERA_PERSPECTIVE;
    c.field_of_view = 1.0f;
    c.enabled = true;
    c.viewport_rect = br_rect_make(0.0f, 0.0f, 1.0f, 1.0f);
    br_node cam = br_camera_create_3d(e, scene, &c);
    BR_CHECK(br_node_valid(e, cam));
    br_engine_step(e, 1.0f / 60.0f);
    br_node3d cam3 = br_view3d_camera(e, br_camera_view3d(e, cam));
    br_camera3d *c3 = br_camera3d_get(e, cam3);
    BR_CHECK(c3 != NULL && c3->far_z > c3->near_z && near_eq(c3->far_z, c3->near_z + 1000.0f));

    br_camera_shake_add(e, cam, 1.0f);
    br_camera_shake *sh = br_component_get(e, cam, br_camera_shake_component(e));
    BR_CHECK(sh != NULL && sh->max_offset < 1.0f); /* world units */
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_vec3_length(br_node3d_position(e, cam3)) <= sh->max_offset + 1e-4f);
    br_engine_destroy(e);
}

/* ---- pointer events through a zoomed, offset 2D camera reach the node drawn under the pointer */

static int s_target_clicks;
static int s_decoy_clicks;

static void on_target_click(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    s_target_clicks++;
}

static void on_decoy_click(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    s_decoy_clicks++;
}

static br_node make_clickable(br_engine *e, float x, float y, br_event_listener_fn fn)
{
    br_node n = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, n, 40.0f, 40.0f);
    br_node_set_position(e, n, x, y);
    br_node_set_event_mode(e, n, BR_EVENT_MODE_STATIC);
    br_node_on(e, n, BR_EVENT_CLICK, fn, NULL);
    br_node_add_child(e, br_stage_root(e), n);
    return n;
}

static void test_pointer_events(void)
{
    br_engine *e = br_engine_create("test_camera_pointer", NULL);
    br_camera_set_headless_screen(e, 1280.0f, 720.0f);
    br_camera c = br_camera_default_2d();
    c.orthographic_size = 180.0f; /* 2x zoom, centred on (1000, 1000) */
    br_node cam = br_camera_create(e, &c);
    br_node_set_position(e, cam, 1000.0f, 1000.0f);
    br_node_add_child(e, br_stage_root(e), cam);
    make_clickable(e, 1000.0f, 1000.0f, on_target_click); /* drawn at window (640..720, 360..440) */
    make_clickable(e, 640.0f, 360.0f, on_decoy_click);   /* where the raw window point would land */
    br_engine_step(e, 1.0f / 60.0f);
    br_input_inject_pointer_move(e, 660.0f, 380.0f);
    br_input_inject_pointer_down(e, 660.0f, 380.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 660.0f, 380.0f, SDL_BUTTON_LEFT);
    BR_CHECK(s_target_clicks == 1);
    BR_CHECK(s_decoy_clicks == 0);
    br_engine_destroy(e);
}

int main(void)
{
    test_extents_2d();
    test_fit_policies();
    test_extents_3d();
    test_round_trips();
    test_pixel_perfect();
    test_helpers();
    test_engine();
    test_engine_3d();
    test_pointer_events();
    return br_test_finish();
}
