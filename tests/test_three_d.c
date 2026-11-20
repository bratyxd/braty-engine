/* test_three_d - 3D maths (projection incl. aspect behaviour, ray-triangle, ray-box, frustum-box,
 * cascade splits), the 3D node table (hierarchy world matrices, look_at, deferred destroy,
 * stale handles), geometry builders, picking and 3D pointer events. Headless. */
#include "braty/braty.h"
#include "br_test.h"

#include <math.h>

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-3f;
}

static bool vec3_eq(br_vec3 a, br_vec3 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y) && near_eq(a.z, b.z);
}

static br_vec3 project(br_mat4 m, br_vec3 p)
{
    br_vec4 c = br_mat4_transform_vec4(m, br_vec4_make(p.x, p.y, p.z, 1.0f));
    return br_vec3_make(c.x / c.w, c.y / c.w, c.z / c.w);
}

static void test_projection(void)
{
    /* perspective: vertical fov fixed, horizontal follows the aspect (Unity) */
    br_camera3d cam = br_camera3d_perspective(60.0f * BR_DEG_TO_RAD, 0.5f, 100.0f);
    float d = 10.0f;
    float top = d * SDL_tanf(30.0f * BR_DEG_TO_RAD);
    float aspects[3] = { 16.0f / 9.0f, 21.0f / 9.0f, 9.0f / 16.0f };
    for (int i = 0; i < 3; i++) {
        br_mat4 p = br_camera3d_projection(&cam, aspects[i]);
        BR_CHECK(near_eq(project(p, br_vec3_make(0.0f, top, -d)).y, 1.0f));
        BR_CHECK(near_eq(project(p, br_vec3_make(top * aspects[i], 0.0f, -d)).x, 1.0f));
    }
    br_mat4 p = br_camera3d_projection(&cam, 2.0f);
    BR_CHECK(near_eq(project(p, br_vec3_make(0.0f, 0.0f, -0.5f)).z, 0.0f));   /* near -> depth 0 */
    BR_CHECK(near_eq(project(p, br_vec3_make(0.0f, 0.0f, -100.0f)).z, 1.0f)); /* far -> depth 1 */
    /* aspect override wins over the view's aspect */
    cam.aspect = 1.0f;
    BR_CHECK(br_mat4_nearly_equal(br_camera3d_projection(&cam, 3.0f), br_camera3d_projection(&cam, 0.5f), 1e-6f));

    /* orthographic: size = half the visible height, width = size * aspect */
    br_camera3d ortho = br_camera3d_orthographic(5.0f, 0.1f, 50.0f);
    br_mat4 o = br_camera3d_projection(&ortho, 2.0f);
    BR_CHECK(near_eq(project(o, br_vec3_make(0.0f, 5.0f, -10.0f)).y, 1.0f));
    BR_CHECK(near_eq(project(o, br_vec3_make(10.0f, 0.0f, -10.0f)).x, 1.0f));
    BR_CHECK(near_eq(project(o, br_vec3_make(0.0f, 0.0f, -0.1f)).z, 0.0f));
    br_mat4 portrait = br_camera3d_projection(&ortho, 0.5f);
    BR_CHECK(near_eq(project(portrait, br_vec3_make(0.0f, 5.0f, -10.0f)).y, 1.0f)); /* still 5 tall */
    BR_CHECK(near_eq(project(portrait, br_vec3_make(2.5f, 0.0f, -10.0f)).x, 1.0f));

    /* ray through the centre of a camera at the origin goes down -Z */
    br_mat4 inv = br_mat4_invert(p, NULL);
    br_ray3d r = br_ray3d_from_ndc(inv, 0.0f, 0.0f);
    BR_CHECK(vec3_eq(r.direction, br_vec3_make(0.0f, 0.0f, -1.0f)));
    BR_CHECK(near_eq(r.origin.z, -0.5f));
}

static void test_rays(void)
{
    br_ray3d r = { { 0.25f, 0.25f, 5.0f }, { 0.0f, 0.0f, -1.0f } };
    br_vec3 a = br_vec3_make(0.0f, 0.0f, 0.0f);
    br_vec3 b = br_vec3_make(1.0f, 0.0f, 0.0f);
    br_vec3 c = br_vec3_make(0.0f, 1.0f, 0.0f);
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    BR_CHECK(br_ray_triangle(r.origin, r.direction, a, b, c, &t, &u, &v));
    BR_CHECK(near_eq(t, 5.0f) && near_eq(u, 0.25f) && near_eq(v, 0.25f));
    BR_CHECK(br_ray_triangle(r.origin, r.direction, a, c, b, &t, NULL, NULL)); /* back face hits too */
    br_ray3d miss = { { 0.8f, 0.8f, 5.0f }, { 0.0f, 0.0f, -1.0f } };
    BR_CHECK(!br_ray_triangle(miss.origin, miss.direction, a, b, c, &t, NULL, NULL));
    br_ray3d away = { { 0.25f, 0.25f, 5.0f }, { 0.0f, 0.0f, 1.0f } };
    BR_CHECK(!br_ray_triangle(away.origin, away.direction, a, b, c, &t, NULL, NULL)); /* behind the origin */
    br_ray3d parallel = { { 0.25f, 0.25f, 5.0f }, { 1.0f, 0.0f, 0.0f } };
    BR_CHECK(!br_ray_triangle(parallel.origin, parallel.direction, a, b, c, &t, NULL, NULL));

    br_aabb box = { { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f } };
    br_ray3d into = { { 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, -1.0f } };
    BR_CHECK(br_ray3d_box(into, box, &t) && near_eq(t, 9.0f));
    br_ray3d inside = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    BR_CHECK(br_ray3d_box(inside, box, &t) && near_eq(t, 0.0f));
    br_ray3d past = { { 0.0f, 3.0f, 10.0f }, { 0.0f, 0.0f, -1.0f } };
    BR_CHECK(!br_ray3d_box(past, box, &t));
}

static br_aabb box_at(float x, float y, float z, float h)
{
    br_aabb b = { { x - h, y - h, z - h }, { x + h, y + h, z + h } };
    return b;
}

static void test_frustum(void)
{
    br_mat4 proj = br_mat4_perspective(90.0f * BR_DEG_TO_RAD, 1.0f, 1.0f, 100.0f);
    br_mat4 view = br_mat4_look_at(br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 0.0f, -1.0f),
                                   br_vec3_make(0.0f, 1.0f, 0.0f));
    br_plane f[6];
    br_frustum_planes(br_mat4_mul(proj, view), f);
    BR_CHECK(br_frustum_planes_aabb(f, box_at(0.0f, 0.0f, -10.0f, 1.0f)));    /* in front */
    BR_CHECK(!br_frustum_planes_aabb(f, box_at(0.0f, 0.0f, 10.0f, 1.0f)));    /* behind */
    BR_CHECK(!br_frustum_planes_aabb(f, box_at(-30.0f, 0.0f, -10.0f, 1.0f))); /* left of the 90 degree cone */
    BR_CHECK(!br_frustum_planes_aabb(f, box_at(0.0f, 0.0f, -200.0f, 1.0f)));  /* beyond far */
    BR_CHECK(br_frustum_planes_aabb(f, box_at(-10.5f, 0.0f, -10.0f, 1.0f)));  /* straddles the left plane */
    BR_CHECK(br_frustum_planes_aabb(f, box_at(0.0f, 0.0f, -0.5f, 1.0f)));    /* straddles near */

    br_aabb e = br_aabb_empty();
    BR_CHECK(!br_aabb_is_valid(e));
    e = br_aabb_extend(e, br_vec3_make(1.0f, 2.0f, 3.0f));
    BR_CHECK(br_aabb_is_valid(e));
    br_aabb moved = br_aabb_transform(box_at(0.0f, 0.0f, 0.0f, 1.0f), br_mat4_translation(br_vec3_make(5.0f, 0.0f, 0.0f)));
    BR_CHECK(near_eq(moved.min.x, 4.0f) && near_eq(moved.max.x, 6.0f));
}

static void test_cascades(void)
{
    float s[4];
    br_shadow3d_cascade_splits(1.0f, 100.0f, 4, 0.0f, s); /* uniform */
    BR_CHECK(near_eq(s[0], 25.75f) && near_eq(s[1], 50.5f) && near_eq(s[2], 75.25f) && near_eq(s[3], 100.0f));
    br_shadow3d_cascade_splits(1.0f, 100.0f, 2, 1.0f, s); /* logarithmic */
    BR_CHECK(near_eq(s[0], 10.0f) && near_eq(s[1], 100.0f));
    br_shadow3d_cascade_splits(0.1f, 60.0f, 3, 0.75f, s);
    BR_CHECK(s[0] > 0.1f && s[0] < s[1] && s[1] < s[2] && near_eq(s[2], 60.0f));
    br_shadow3d_cascade_splits(0.1f, 60.0f, 1, 0.75f, s);
    BR_CHECK(near_eq(s[0], 60.0f));
}

static int g_destroyed;
static br_node3d g_victim;

static void counting_destroy(br_engine *e, br_node3d n, void *data)
{
    (void)e;
    (void)n;
    (void)data;
    g_destroyed++;
}

static void destroy_in_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    if (br_node3d_valid(e, g_victim)) {
        br_node3d_destroy(e, g_victim);
        BR_CHECK(!br_node3d_valid(e, g_victim)); /* invalid at once */
        BR_CHECK(g_destroyed == 0);              /* memory not freed yet */
    }
}

static void test_nodes(br_engine *e)
{
    br_node3d parent = br_node3d_create_container(e);
    br_node3d child = br_node3d_create_container(e);
    br_node3d_set_label(e, child, "child");
    BR_CHECK(br_node3d_add_child(e, parent, child));
    BR_CHECK(!br_node3d_add_child(e, child, parent)); /* cycle */
    BR_CHECK(br_node3d_child_count(e, parent) == 1);
    BR_CHECK(br_node3d_find_by_label(e, parent, "child", true).index == child.index);

    /* parent at (1,0,0) turned 90 degrees about +Y; child 1 unit down its -Z */
    br_node3d_set_position(e, parent, br_vec3_make(1.0f, 0.0f, 0.0f));
    br_node3d_set_rotation(e, parent, br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.5f));
    br_node3d_set_position(e, child, br_vec3_make(0.0f, 0.0f, -1.0f));
    BR_CHECK(vec3_eq(br_node3d_world_position(e, child), br_vec3_make(0.0f, 0.0f, 0.0f)));
    br_node3d_set_scale(e, parent, br_vec3_make(2.0f, 2.0f, 2.0f));
    BR_CHECK(vec3_eq(br_node3d_world_position(e, child), br_vec3_make(-1.0f, 0.0f, 0.0f)));

    /* look_at points -Z at the target, also under a rotated parent */
    br_node3d_look_at(e, child, br_vec3_make(-1.0f, 0.0f, -5.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_mat4 w = br_node3d_world_transform(e, child);
    BR_CHECK(vec3_eq(br_vec3_normalize(br_vec3_make(-w.m[8], -w.m[9], -w.m[10])), br_vec3_make(0.0f, 0.0f, -1.0f)));
    br_node3d_look_at(e, child, br_vec3_make(-1.0f, 10.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f)); /* straight up */
    w = br_node3d_world_transform(e, child);
    BR_CHECK(vec3_eq(br_vec3_normalize(br_vec3_make(-w.m[8], -w.m[9], -w.m[10])), br_vec3_make(0.0f, 1.0f, 0.0f)));

    /* destroy outside a frame: immediate; subtree goes too; stale handles ignored */
    uint32_t before = br_node3d_count(e);
    br_node3d_destroy(e, parent);
    BR_CHECK(br_node3d_count(e) == before - 2);
    BR_CHECK(!br_node3d_valid(e, parent) && !br_node3d_valid(e, child));
    br_node3d_set_position(e, child, br_vec3_make(9.0f, 9.0f, 9.0f)); /* ignored */
    BR_CHECK(vec3_eq(br_node3d_position(e, child), br_vec3_make(0.0f, 0.0f, 0.0f)));
    br_node3d reuse = br_node3d_create_container(e); /* may reuse the slot: the old handle stays stale */
    BR_CHECK(br_node3d_valid(e, reuse));
    BR_CHECK(!br_node3d_valid(e, parent) && !br_node3d_valid(e, child));
    br_node3d_destroy(e, reuse);

    /* destroy inside a frame: deferred to the next frame's flush */
    br_node3d_type_desc desc;
    SDL_zero(desc);
    desc.name = "test_counted";
    desc.data_size = sizeof(int);
    desc.on_destroy = counting_destroy;
    uint32_t type = br_node3d_type_register(e, &desc);
    BR_CHECK(type != 0);
    BR_CHECK(br_node3d_type_register(e, &desc) == 0); /* duplicate name */
    BR_CHECK(br_node3d_type_find(e, "test_counted") == type);
    g_victim = br_node3d_create(e, type);
    br_node3d keep = br_node3d_create(e, type); /* its data moves when the victim's is removed */
    *(int *)br_node3d_data(e, keep, type) = 77;
    BR_CHECK(br_node3d_data(e, keep, BR_NODE3D_TYPE_CONTAINER) == NULL); /* wrong type */
    br_system_add(e, "test.destroy3d", BR_PHASE_UPDATE, 0, destroy_in_system, NULL);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_destroyed == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_destroyed == 1);
    BR_CHECK(*(int *)br_node3d_data(e, keep, type) == 77);
    br_node3d_destroy(e, keep);
    BR_CHECK(g_destroyed == 2);
}

typedef struct linked3d {
    br_node3d other;
    int       tag;
} linked3d;

static int g_linked_calls;

static void linked_destroy(br_engine *e, br_node3d n, void *data)
{
    (void)n;
    linked3d *l = data;
    g_linked_calls++;
    br_node3d_destroy(e, l->other); /* a companion node, destroyed from on_destroy */
}

/* R6 S1: an on_destroy that destroys another node outside a frame must not re-enter the flush */
static void test_nested_destroy(br_engine *e)
{
    br_node3d_type_desc d;
    SDL_zero(d);
    d.name = "test_linked";
    d.data_size = sizeof(linked3d);
    d.on_destroy = linked_destroy;
    uint32_t type = br_node3d_type_register(e, &d);
    br_node3d a = br_node3d_create(e, type);
    br_node3d keep1 = br_node3d_create(e, type);
    br_node3d keep2 = br_node3d_create(e, type);
    br_node3d b = br_node3d_create_container(e);
    ((linked3d *)br_node3d_data(e, a, type))->other = b;
    ((linked3d *)br_node3d_data(e, keep1, type))->tag = 2;
    ((linked3d *)br_node3d_data(e, keep2, type))->tag = 3;
    uint32_t before = br_node3d_count(e);
    br_node3d_destroy(e, a);
    BR_CHECK(g_linked_calls == 1);
    BR_CHECK(br_node3d_count(e) == before - 2);
    BR_CHECK(!br_node3d_valid(e, b));
    BR_CHECK(((linked3d *)br_node3d_data(e, keep1, type))->tag == 2);
    BR_CHECK(((linked3d *)br_node3d_data(e, keep2, type))->tag == 3);
    br_node3d n1 = br_node3d_create_container(e);
    br_node3d n2 = br_node3d_create_container(e);
    br_node3d n3 = br_node3d_create_container(e);
    BR_CHECK(n1.index != n2.index && n2.index != n3.index && n1.index != n3.index);
    BR_CHECK(br_node3d_valid(e, n1) && br_node3d_valid(e, n2) && br_node3d_valid(e, n3));
    br_node3d_destroy(e, n1);
    br_node3d_destroy(e, n2);
    br_node3d_destroy(e, n3);
    br_node3d_destroy(e, keep1);
    br_node3d_destroy(e, keep2);
}

static void test_geometry(br_engine *e)
{
    br_geometry3d cube = br_geometry3d_cube(e, br_vec3_make(2.0f, 4.0f, 6.0f));
    uint32_t vc = 0;
    uint32_t ic = 0;
    const br_vertex3d *v = br_geometry3d_vertices(e, cube, &vc);
    br_geometry3d_indices(e, cube, &ic);
    BR_CHECK(v != NULL && vc == 24 && ic == 36);
    br_aabb b = br_geometry3d_bounds(e, cube);
    BR_CHECK(vec3_eq(b.min, br_vec3_make(-1.0f, -2.0f, -3.0f)) && vec3_eq(b.max, br_vec3_make(1.0f, 2.0f, 3.0f)));
    /* every cube triangle winds counter-clockwise seen from outside */
    const uint32_t *idx = br_geometry3d_indices(e, cube, NULL);
    bool outward = true;
    for (uint32_t i = 0; i < ic; i += 3) {
        br_vec3 p0 = v[idx[i]].position;
        br_vec3 n = br_vec3_cross(br_vec3_sub(v[idx[i + 1]].position, p0), br_vec3_sub(v[idx[i + 2]].position, p0));
        outward = outward && br_vec3_dot(n, v[idx[i]].normal) > 0.0f;
    }
    BR_CHECK(outward);

    br_geometry3d sphere = br_geometry3d_sphere(e, 1.0f, 16, 8);
    v = br_geometry3d_vertices(e, sphere, &vc);
    idx = br_geometry3d_indices(e, sphere, &ic);
    BR_CHECK(vc == 17 * 9);
    outward = true;
    for (uint32_t i = 0; i < ic; i += 3) {
        br_vec3 p0 = v[idx[i]].position;
        br_vec3 n = br_vec3_cross(br_vec3_sub(v[idx[i + 1]].position, p0), br_vec3_sub(v[idx[i + 2]].position, p0));
        if (br_vec3_length(n) > 1e-6f) {
            outward = outward && br_vec3_dot(n, p0) > 0.0f;
        }
    }
    BR_CHECK(outward);

    br_geometry3d plane = br_geometry3d_plane(e, 2.0f, 2.0f, 1, 1);
    v = br_geometry3d_vertices(e, plane, &vc);
    BR_CHECK(vc == 4 && vec3_eq(v[0].normal, br_vec3_make(0.0f, 1.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_vec3_make(v[0].tangent.x, v[0].tangent.y, v[0].tangent.z), br_vec3_make(1.0f, 0.0f, 0.0f)));
    BR_CHECK(v[0].tangent.w == 1.0f); /* glTF: cross(n, t) * w = -Z = towards the top of the image */

    BR_CHECK(br_geometry3d_valid(e, br_geometry3d_torus(e, 1.0f, 0.3f, 12, 8)));
    BR_CHECK(br_geometry3d_valid(e, br_geometry3d_capsule(e, 0.5f, 1.0f, 12, 8)));
    br_geometry3d cone = br_geometry3d_cone(e, 1.0f, 2.0f, 12);
    BR_CHECK(near_eq(br_geometry3d_bounds(e, cone).max.y, 1.0f));

    uint32_t bad_indices[3] = { 0, 1, 7 };
    br_geometry3d_desc desc;
    SDL_zero(desc);
    desc.vertices = v;
    desc.vertex_count = 4;
    desc.indices = bad_indices;
    desc.index_count = 3;
    BR_CHECK(br_geometry3d_create(e, &desc).generation == 0); /* out-of-range index rejected */
    /* R6 S2: byte sizes that wrap a uint32 GPU buffer size are rejected before any allocation
     * or copy (desc.vertices points at 4 vertices only) */
    desc.indices = NULL;
    desc.vertex_count = 0x02AAAAABu; /* * 96 bytes > 4 GiB */
    BR_CHECK(br_geometry3d_create(e, &desc).generation == 0);
    br_vec3 deltas[4];
    SDL_zero(deltas);
    desc.vertex_count = 4;
    desc.morph_positions = deltas;
    desc.morph_target_count = 0x08000000u; /* 4 vertices * 32 bytes each: 2^34 */
    BR_CHECK(br_geometry3d_create(e, &desc).generation == 0);
    desc.morph_positions = NULL;
    desc.morph_target_count = 0;
    br_geometry3d_destroy(e, plane);
    BR_CHECK(!br_geometry3d_valid(e, plane));
}

static int g_clicks;
static int g_bubbled;
static br_node3d g_clicked;

static void on_click3d(br_engine *e, br_event3d *ev, void *user)
{
    (void)e;
    (void)user;
    g_clicks++;
    g_clicked = ev->target;
}

static void on_bubble3d(br_engine *e, br_event3d *ev, void *user)
{
    (void)e;
    (void)user;
    if (ev->current_target.index != ev->target.index) {
        g_bubbled++;
    }
}

static void test_picking(br_engine *e)
{
    br_node view = br_view3d_create(e, 200.0f, 200.0f);
    BR_CHECK(br_node_valid(e, view));
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d root = br_view3d_root(e, view);
    br_node3d cam = br_view3d_camera(e, view);
    BR_CHECK(br_camera3d_get(e, cam) != NULL);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.0f, 10.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    br_material3d mat = br_material3d_create(e, NULL);
    BR_CHECK(mat.generation == 0); /* NULL desc */
    br_material3d_desc md = br_material3d_default(BR_MATERIAL3D_PBR);
    mat = br_material3d_create(e, &md);
    BR_CHECK(br_material3d_valid(e, mat));
    br_node3d group = br_node3d_create_container(e);
    br_node3d_add_child(e, root, group);
    br_node3d box = br_mesh3d_create(e, br_geometry3d_cube(e, br_vec3_make(2.0f, 2.0f, 2.0f)), mat);
    br_node3d_add_child(e, group, box);
    br_node3d_set_position(e, box, br_vec3_make(0.0f, 0.0f, -1.0f));

    br_pick3d_hit hit;
    BR_CHECK(br_view3d_pick(e, view, br_vec2_make(100.0f, 100.0f), &hit));
    BR_CHECK(hit.node.index == box.index && near_eq(hit.distance, 9.9f)); /* from the near plane (0.1) to z = 0 */
    BR_CHECK(vec3_eq(hit.normal, br_vec3_make(0.0f, 0.0f, 1.0f)));
    BR_CHECK(!br_view3d_pick(e, view, br_vec2_make(5.0f, 5.0f), &hit)); /* corner: empty */

    br_vec2 screen;
    BR_CHECK(br_view3d_world_to_screen(e, view, br_vec3_make(0.0f, 0.0f, 0.0f), &screen));
    BR_CHECK(near_eq(screen.x, 100.0f) && near_eq(screen.y, 100.0f));
    BR_CHECK(br_view3d_world_to_screen(e, view, br_vec3_make(0.0f, 1.0f, 0.0f), &screen) && screen.y < 100.0f);
    br_ray3d ray = br_view3d_screen_to_ray(e, view, br_vec2_make(100.0f, 100.0f));
    BR_CHECK(vec3_eq(ray.direction, br_vec3_make(0.0f, 0.0f, -1.0f)));

    /* batched: the instance hit is reported */
    br_node3d field = br_batched_mesh3d_create(e, br_geometry3d_sphere(e, 0.5f, 12, 6), mat);
    br_node3d_add_child(e, root, field);
    br_batched_mesh3d_add(e, field, br_mat4_translation(br_vec3_make(-3.0f, 0.0f, 2.0f)), br_color_make(1, 1, 1, 1));
    br_batched_mesh3d_add(e, field, br_mat4_translation(br_vec3_make(3.0f, 0.0f, 2.0f)), br_color_make(1, 1, 1, 1));
    br_vec2 right;
    BR_CHECK(br_view3d_world_to_screen(e, view, br_vec3_make(3.0f, 0.0f, 2.0f), &right));
    BR_CHECK(br_view3d_pick(e, view, right, &hit) && hit.node.index == field.index && hit.instance == 1);
    br_node3d_set_pickable(e, field, false);
    BR_CHECK(!br_view3d_pick(e, view, right, &hit));

    /* 3D pointer events through the view: click on the box, bubbling to its group */
    br_node3d_on(e, box, BR_EVENT_CLICK, on_click3d, NULL);
    br_node3d_on(e, group, BR_EVENT_CLICK, on_bubble3d, NULL);
    br_input_inject_pointer_down(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1 && g_clicked.index == box.index);
    BR_CHECK(g_bubbled == 1);
    /* a click on empty space is not taken by the view */
    br_input_inject_pointer_down(e, 5.0f, 5.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 5.0f, 5.0f, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1);
    BR_CHECK(br_events_hit_test(e, br_vec2_make(5.0f, 5.0f)).generation == 0);
    /* destroying the node drops its listeners */
    br_node3d_destroy(e, box);
    br_input_inject_pointer_down(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    BR_CHECK(g_clicks == 1);

    br_node_destroy(e, view);
    BR_CHECK(!br_node3d_valid(e, root) && !br_node3d_valid(e, group));
}

/* CPU copy of the spot cone in mesh3d_common.hlsli: smoothstep(cos_outer, cos_inner, cd), with
 * the division HLSL's smoothstep does before its saturate. */
static float spot_t(float cos_outer, float cos_inner, float cd)
{
    return (cd - cos_outer) / (cos_inner - cos_outer);
}

static bool finite_unit(float t)
{
    return !SDL_isnan(t) && !SDL_isinf(t);
}

/* inner == outer: the shader's smoothstep divides by zero (0/0 = NaN on the cone edge, +-INF
 * elsewhere; what saturate makes of those is driver-dependent). br_light3d_spot_cone, which the
 * renderer packs, keeps the inner cosine above the outer one: finite everywhere, a hard edge. */
static void test_spot_cone(void)
{
    br_light3d l = br_light3d_default(BR_LIGHT3D_SPOT);
    l.inner_angle = 0.4f;
    l.outer_angle = 0.4f;
    float co = SDL_cosf(l.outer_angle);
    BR_CHECK(SDL_isnan(spot_t(co, SDL_cosf(l.inner_angle), co))); /* unguarded: the bug */
    BR_CHECK(SDL_isinf(spot_t(co, SDL_cosf(l.inner_angle), 1.0f)));
    br_vec2 cone = br_light3d_spot_cone(&l);
    BR_CHECK(cone.y > cone.x && near_eq(cone.x, co));
    float samples[5] = { -1.0f, co - 0.01f, co, co + 0.01f, 1.0f };
    for (int i = 0; i < 5; i++) {
        BR_CHECK(finite_unit(spot_t(cone.x, cone.y, samples[i])));
    }
    BR_CHECK(spot_t(cone.x, cone.y, co + 0.01f) >= 1.0f && spot_t(cone.x, cone.y, co - 0.01f) <= 0.0f);
    /* inner > outer (an inverted cone) and NaN angles are guarded the same way */
    l.inner_angle = 0.6f;
    cone = br_light3d_spot_cone(&l);
    BR_CHECK(cone.y > cone.x);
    l.inner_angle = NAN;
    l.outer_angle = INFINITY;
    cone = br_light3d_spot_cone(&l);
    BR_CHECK(finite_unit(cone.x) && finite_unit(cone.y) && cone.y > cone.x);
    /* a normal cone is passed through */
    l = br_light3d_default(BR_LIGHT3D_SPOT);
    cone = br_light3d_spot_cone(&l);
    BR_CHECK(near_eq(cone.x, SDL_cosf(l.outer_angle)) && near_eq(cone.y, SDL_cosf(l.inner_angle)));
}

/* every View3D setter has a getter, and br_view3d_copy_settings carries them to another view */
static void test_view_settings(br_engine *e)
{
    br_node a = br_view3d_create(e, 100.0f, 50.0f);
    br_node b = br_view3d_create(e, 10.0f, 10.0f);
    BR_CHECK(br_view3d_msaa(e, a) == 1 && !br_view3d_auto_resize(e, a) && br_view3d_culling_mask(e, a) == 0xFFFFFFFFu);
    br_view3d_set_msaa(e, a, 4);
    br_view3d_set_auto_resize(e, a, true);
    br_view3d_set_resolution(e, a, 2.0f);
    br_view3d_set_background(e, a, br_color_make(0.2f, 0.4f, 0.6f, 0.5f));
    br_view3d_set_environment(e, a, br_color_make(1.0f, 0.0f, 0.0f, 1.0f), br_color_make(0.0f, 1.0f, 0.0f, 1.0f), 0.7f);
    br_view3d_set_culling_mask(e, a, 0x5u);
    br_view3d_set_shadow_resolution(e, a, 512);
    br_view3d_post(e, a)->exposure = 2.5f;
    BR_CHECK(br_view3d_msaa(e, a) == 4 && br_view3d_auto_resize(e, a) && near_eq(br_view3d_resolution(e, a), 2.0f));
    br_color bg = br_view3d_background(e, a);
    BR_CHECK(near_eq(bg.r, 0.2f) && near_eq(bg.g, 0.4f) && near_eq(bg.b, 0.6f) && near_eq(bg.a, 0.5f));
    br_color sky;
    br_color ground;
    float intensity = 0.0f;
    BR_CHECK(br_view3d_get_environment(e, a, &sky, &ground, &intensity));
    BR_CHECK(near_eq(sky.r, 1.0f) && near_eq(ground.g, 1.0f) && near_eq(intensity, 0.7f));
    BR_CHECK(br_view3d_culling_mask(e, a) == 0x5u && br_view3d_shadow_resolution(e, a) == 512);
    br_node not_view = br_node_create_container(e);
    BR_CHECK(!br_view3d_get_environment(e, not_view, &sky, &ground, &intensity) && br_view3d_msaa(e, not_view) == 0);

    BR_CHECK(br_view3d_copy_settings(e, a, b));
    BR_CHECK(br_view3d_msaa(e, b) == 4 && near_eq(br_view3d_resolution(e, b), 2.0f) && br_view3d_culling_mask(e, b) == 0x5u);
    BR_CHECK(br_view3d_shadow_resolution(e, b) == 512 && near_eq(br_view3d_post(e, b)->exposure, 2.5f));
    bg = br_view3d_background(e, b);
    BR_CHECK(near_eq(bg.b, 0.6f) && near_eq(bg.a, 0.5f));
    BR_CHECK(br_view3d_get_environment(e, b, &sky, &ground, &intensity) && near_eq(intensity, 0.7f));
    /* size, root, camera and auto-resize belong to the view, not its look */
    BR_CHECK(near_eq(br_view3d_size(e, b).x, 10.0f) && !br_view3d_auto_resize(e, b));
    BR_CHECK(!br_view3d_copy_settings(e, a, not_view));
    br_node_destroy(e, not_view);
    br_node_destroy(e, a);
    br_node_destroy(e, b);
}

int main(void)
{
    test_projection();
    test_rays();
    test_frustum();
    test_cascades();
    test_spot_cone();
    br_engine *e = br_engine_create("test_three_d", NULL);
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_nodes(e);
        test_nested_destroy(e);
        test_geometry(e);
        test_picking(e);
        test_view_settings(e);
        br_engine_destroy(e);
    }
    return br_test_finish();
}
