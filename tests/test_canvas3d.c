/* test_canvas3d - 2D-in-3D maths (2D -> plane mapping with pivot and ppu, world ray -> 2D local
 * point, billboards) and, headless, picking a canvas through a View3D and a click reaching a
 * 2D button inside it. */
#include "braty/braty.h"
#include "br_test.h"

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-3f;
}

static bool vec3_eq(br_vec3 a, br_vec3 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y) && near_eq(a.z, b.z);
}

static void test_plane(void)
{
    br_canvas3d_desc d = br_canvas3d_default(200.0f, 100.0f);
    d.pixels_per_unit = 50.0f;
    br_mat4 m = br_canvas3d_plane_matrix(&d);
    /* the pivot (centre) sits on the node; 2D y down is 3D y up; 50 2D units = 1 world unit */
    BR_CHECK(vec3_eq(br_mat4_transform_point(m, br_vec3_make(100.0f, 50.0f, 0.0f)), br_vec3_make(0.0f, 0.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_mat4_transform_point(m, br_vec3_make(0.0f, 0.0f, 0.0f)), br_vec3_make(-2.0f, 1.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_mat4_transform_point(m, br_vec3_make(200.0f, 100.0f, 0.0f)), br_vec3_make(2.0f, -1.0f, 0.0f)));
    d.pivot = br_vec2_make(0.5f, 1.0f); /* bottom centre: a sprite standing on the ground */
    m = br_canvas3d_plane_matrix(&d);
    BR_CHECK(vec3_eq(br_mat4_transform_point(m, br_vec3_make(100.0f, 100.0f, 0.0f)), br_vec3_make(0.0f, 0.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_mat4_transform_point(m, br_vec3_make(100.0f, 0.0f, 0.0f)), br_vec3_make(0.0f, 2.0f, 0.0f)));
}

static void test_ray_to_local(void)
{
    br_canvas3d_desc d = br_canvas3d_default(200.0f, 100.0f);
    d.pixels_per_unit = 100.0f; /* 2 x 1 world units */
    /* canvas at (5, 1, -3) turned 90 degrees about Y: its +Z (front) points to +X */
    br_mat4 world = br_mat4_mul(br_mat4_translation(br_vec3_make(5.0f, 1.0f, -3.0f)), br_mat4_rotation_y(BR_PI * 0.5f));
    br_ray3d ray;
    ray.origin = br_vec3_make(10.0f, 1.25f, -3.5f);
    ray.direction = br_vec3_make(-1.0f, 0.0f, 0.0f);
    br_vec2 p;
    float t = 0.0f;
    BR_CHECK(br_canvas3d_ray_to_local(&d, world, ray, &p, &t));
    BR_CHECK(near_eq(t, 5.0f));
    /* world z -3.5 is local +x 0.5 (rotated): 2D x = 100 + 50; world y 1.25 is 2D y = 50 - 25 */
    BR_CHECK(near_eq(p.x, 150.0f) && near_eq(p.y, 25.0f));
    /* outside the content rect, parallel, and behind the ray: misses */
    ray.origin.y = 3.0f;
    BR_CHECK(!br_canvas3d_ray_to_local(&d, world, ray, &p, &t));
    ray.origin.y = 1.25f;
    ray.direction = br_vec3_make(0.0f, 0.0f, 1.0f);
    BR_CHECK(!br_canvas3d_ray_to_local(&d, world, ray, &p, &t));
    ray.direction = br_vec3_make(1.0f, 0.0f, 0.0f);
    BR_CHECK(!br_canvas3d_ray_to_local(&d, world, ray, &p, &t));
}

static void test_billboard(void)
{
    br_mat4 node = br_mat4_mul(br_mat4_translation(br_vec3_make(1.0f, 2.0f, 3.0f)), br_mat4_scaling(br_vec3_make(2.0f, 2.0f, 2.0f)));
    br_mat4 camera = br_mat4_invert(br_mat4_look_at(br_vec3_make(6.0f, 7.0f, 8.0f), br_vec3_make(1.0f, 2.0f, 3.0f),
                                                    br_vec3_make(0.0f, 1.0f, 0.0f)), NULL);
    BR_CHECK(br_mat4_nearly_equal(br_canvas3d_billboard_matrix(BR_CANVAS3D_BILLBOARD_NONE, node, camera), node, 1e-6f));
    br_mat4 full = br_canvas3d_billboard_matrix(BR_CANVAS3D_BILLBOARD_FULL, node, camera);
    br_vec3 front = br_vec3_normalize(br_mat4_transform_dir(full, br_vec3_make(0.0f, 0.0f, 1.0f)));
    br_vec3 to_camera = br_vec3_normalize(br_vec3_make(5.0f, 5.0f, 5.0f));
    BR_CHECK(vec3_eq(front, to_camera));                                     /* faces the camera */
    BR_CHECK(vec3_eq(br_mat4_transform_point(full, br_vec3_make(0.0f, 0.0f, 0.0f)), br_vec3_make(1.0f, 2.0f, 3.0f)));
    BR_CHECK(near_eq(br_vec3_length(br_mat4_transform_dir(full, br_vec3_make(1.0f, 0.0f, 0.0f))), 2.0f)); /* scale kept */
    br_mat4 yaxis = br_canvas3d_billboard_matrix(BR_CANVAS3D_BILLBOARD_Y_AXIS, node, camera);
    br_vec3 yfront = br_vec3_normalize(br_mat4_transform_dir(yaxis, br_vec3_make(0.0f, 0.0f, 1.0f)));
    BR_CHECK(vec3_eq(yfront, br_vec3_normalize(br_vec3_make(5.0f, 0.0f, 5.0f)))); /* turned about Y only */
    BR_CHECK(vec3_eq(br_vec3_normalize(br_mat4_transform_dir(yaxis, br_vec3_make(0.0f, 1.0f, 0.0f))),
                     br_vec3_make(0.0f, 1.0f, 0.0f)));
}

static int g_clicks = 0;
static int g_overs = 0;

static void on_click(br_engine *e, br_event *ev, void *user)
{
    (void)user;
    g_clicks++;
    br_vec2 local = br_event_local(e, ev);
    /* the button is 100 x 40 at (50, 30): the click hit its middle */
    BR_CHECK(near_eq(local.x, 50.0f) && near_eq(local.y, 20.0f));
}

static void on_over(br_engine *e, br_event *ev, void *user)
{
    (void)e;
    (void)ev;
    (void)user;
    g_overs++;
}

static void test_engine(void)
{
    br_engine *e = br_engine_create("test_canvas3d", NULL);
    br_node view = br_view3d_create(e, 200.0f, 200.0f);
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.0f, 10.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    /* a 200 x 100 canvas (2 x 1 world units) at the origin, a 100 x 40 button centred on it */
    br_canvas3d_desc d = br_canvas3d_default(200.0f, 100.0f);
    br_node3d canvas = br_canvas3d_create(e, &d);
    BR_CHECK(br_node3d_type_of(e, canvas) == br_canvas3d_type(e));
    br_node3d_add_child(e, br_view3d_root(e, view), canvas);
    br_node root = br_canvas3d_root(e, canvas);
    BR_CHECK(br_node_valid(e, root) && !br_node_valid(e, br_node_parent(e, root))); /* detached */
    br_node button = br_node_create_container(e);
    br_node_set_position(e, button, br_vec2_make(50.0f, 30.0f));
    br_hit_area area;
    SDL_zero(area);
    area.type = BR_HIT_AREA_RECT;
    area.rect = br_rect_make(0.0f, 0.0f, 100.0f, 40.0f);
    br_node_set_hit_area(e, button, &area);
    br_node_set_event_mode(e, button, BR_EVENT_MODE_STATIC);
    br_node_add_child(e, root, button);
    br_node_on(e, button, BR_EVENT_CLICK, on_click, NULL);
    br_node_on(e, button, BR_EVENT_POINTER_OVER, on_over, NULL);
    BR_CHECK(br_node_equal(br_canvas3d_content(e, canvas), button));

    /* picking: the canvas plane is at z = 0, 9.9 from the near plane */
    br_pick3d_hit hit;
    BR_CHECK(br_view3d_pick(e, view, br_vec2_make(100.0f, 100.0f), &hit));
    BR_CHECK(hit.node.index == canvas.index && near_eq(hit.distance, 9.9f));
    BR_CHECK(!br_view3d_pick(e, view, br_vec2_make(2.0f, 2.0f), &hit));

    /* a click at the view's centre = the canvas centre = 2D (100, 50): inside the button */
    br_input_inject_pointer_move(e, 100.0f, 100.0f);
    br_input_inject_pointer_down(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1);
    BR_CHECK(g_overs == 1);
    /* a click on the canvas but outside the button reaches no 2D node */
    br_vec2 corner;
    BR_CHECK(br_view3d_world_to_screen(e, view, br_vec3_make(-0.9f, 0.4f, 0.0f), &corner));
    br_input_inject_pointer_down(e, corner.x, corner.y, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, corner.x, corner.y, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1);
    /* a 50,000-level chain on the canvas: the canvas hit test must not overflow the stack */
    br_node chain = root;
    for (int i = 0; i < 50000; i++) {
        br_node next = br_node_create_container(e);
        br_node_add_child(e, chain, next);
        chain = next;
    }
    br_input_inject_pointer_down(e, corner.x, corner.y, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, corner.x, corner.y, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1);

    /* convenience creators size the canvas to their content */
    br_node3d sprite = br_sprite3d_create(e, br_texture_white(e), 100.0f);
    BR_CHECK(br_canvas3d_get(e, sprite) != NULL && br_canvas3d_get(e, sprite)->width > 0.0f);
    br_text_style st = br_text_style_default();
    br_node3d text = br_text3d_create(e, "Hello", &st, 100.0f);
    BR_CHECK(br_canvas3d_get(e, text)->width > 20.0f && br_canvas3d_get(e, text)->height > 10.0f);
    BR_CHECK(br_canvas3d_sdf_font(e) != NULL);
    br_node3d plate = br_nine_slice3d_create(e, br_texture_white(e), 1, 1, 1, 1, 240.0f, 60.0f, 100.0f);
    BR_CHECK(near_eq(br_canvas3d_get(e, plate)->width, 240.0f));

    /* destroying a canvas destroys its 2D root */
    br_node3d_destroy(e, canvas);
    BR_CHECK(!br_node_valid(e, root));
    br_engine_step(e, 1.0f / 60.0f);
    br_engine_destroy(e);
}

int main(void)
{
    test_plane();
    test_ray_to_local();
    test_billboard();
    test_engine();
    return br_test_finish();
}
