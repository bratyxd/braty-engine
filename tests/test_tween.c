/* test_tween - tweens, headless: to / from / from_to, easing, delay, repeat and yoyo counts,
 * on_complete once, sequence and parallel ordering, kill by handle / node / tag, automatic kill
 * when the node is destroyed, 3D nodes and component fields, and the "tween" / "animator"
 * scene-file components (with a real *.anim.json and *.animgraph.json through the asset loaders:
 * asset references released, events valid when a callback destroys its node, glTF node extras
 * on 3D nodes). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_filesystem.h>

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

static br_node make_node(br_engine *e, const char *label)
{
    br_node n = br_node_create_container(e);
    br_node_set_label(e, n, label);
    br_node_add_child(e, br_stage_root(e), n);
    return n;
}

static int g_done = 0;
static float g_done_at[8];
static int g_order[8];
static int g_order_count = 0;
static float g_clock = 0.0f;

static void on_done(br_engine *e, br_tween t, void *user)
{
    (void)e;
    (void)t;
    int id = (int)(intptr_t)user;
    g_done++;
    if (g_order_count < 8) {
        g_order[g_order_count] = id;
        g_done_at[g_order_count] = g_clock;
        g_order_count++;
    }
}

static void step(br_engine *e, float dt)
{
    g_clock += dt;
    br_engine_step(e, dt);
}

static void test_basics(br_engine *e)
{
    br_node n = make_node(e, "basic");
    br_anim_target t = br_anim_target_2d(n);
    float to[2] = { 100.0f, 50.0f };
    br_tween tw = br_tween_to(e, t, "position", to, 1.0f, BR_EASE_LINEAR);
    BR_CHECK(br_tween_alive(e, tw) && br_tween_count(e) == 1);
    step(e, 0.25f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 25.0f) && near_eq(br_node_position(e, n).y, 12.5f));
    step(e, 1.0f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 100.0f) && !br_tween_alive(e, tw) && br_tween_count(e) == 0);

    /* from: to the value the property has when the tween starts */
    br_node_set_alpha(e, n, 0.8f);
    float from[1] = { 0.0f };
    br_tween_from(e, t, "alpha", from, 1.0f, BR_EASE_LINEAR);
    step(e, 0.5f);
    BR_CHECK(near_eq(br_node_alpha(e, n), 0.4f));
    step(e, 0.6f);
    BR_CHECK(near_eq(br_node_alpha(e, n), 0.8f));

    /* from_to with an easing; to_f */
    float a[1] = { 0.0f };
    float b[1] = { 1.0f };
    br_tween_from_to(e, t, "rotation", a, b, 1.0f, BR_EASE_IN_QUAD);
    step(e, 0.5f);
    BR_CHECK(near_eq(br_node_rotation(e, n), 0.25f));
    step(e, 1.0f);
    br_tween tf = br_tween_to_f(e, t, "x", 0.0f, 0.5f, BR_EASE_OUT_CUBIC);
    BR_CHECK(br_tween_set_ease_name(e, tf, "linear") && !br_tween_set_ease_name(e, tf, "wobbly"));
    br_tween_set_ease(e, tf, BR_EASE_LINEAR);
    step(e, 0.25f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 50.0f));
    step(e, 0.5f);

    /* delay: nothing moves until it has passed, and the start value is read then */
    br_node_set_position(e, n, 0.0f, 0.0f);
    br_tween d = br_tween_to_f(e, t, "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_set_delay(e, d, 0.5f);
    step(e, 0.25f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 0.0f));
    br_node_set_position(e, n, 2.0f, 0.0f);
    step(e, 0.75f); /* 0.5 s into the tween, from 2 */
    BR_CHECK(near_eq(br_node_position(e, n).x, 6.0f));
    step(e, 1.0f);

    /* bad paths fail with a zero handle */
    BR_CHECK(br_tween_to_f(e, t, "nope", 1.0f, 1.0f, BR_EASE_LINEAR).generation == 0);
    BR_CHECK(br_tween_to_f(e, t, "ghost:x", 1.0f, 1.0f, BR_EASE_LINEAR).generation == 0);
    br_node_destroy(e, n);
    step(e, 0.0f);
}

static void test_repeat_yoyo(br_engine *e)
{
    br_node n = make_node(e, "repeat");
    br_anim_target t = br_anim_target_2d(n);
    g_done = 0;
    g_order_count = 0;
    br_tween r = br_tween_to_f(e, t, "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_set_repeat(e, r, 2, true); /* 3 passes: forwards, backwards, forwards */
    br_tween_set_on_complete(e, r, on_done, (void *)(intptr_t)1);
    step(e, 0.5f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 5.0f));
    step(e, 1.0f); /* pass 2 (backwards), 0.5 in */
    BR_CHECK(near_eq(br_node_position(e, n).x, 5.0f));
    step(e, 0.75f); /* pass 3 (forwards), 0.25 in */
    BR_CHECK(near_eq(br_node_position(e, n).x, 2.5f) && g_done == 0);
    step(e, 1.0f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 10.0f) && g_done == 1 && !br_tween_alive(e, r));
    step(e, 1.0f);
    BR_CHECK(g_done == 1); /* once */

    /* an even number of yoyo passes ends where it started; a big step finishes it at once */
    br_node_set_position(e, n, 0.0f, 0.0f);
    br_tween y = br_tween_to_f(e, t, "x", 10.0f, 0.5f, BR_EASE_LINEAR);
    br_tween_set_repeat(e, y, 3, true);
    br_tween_set_on_complete(e, y, on_done, (void *)(intptr_t)2);
    step(e, 10.0f);
    BR_CHECK(near_eq(br_node_position(e, n).x, 0.0f) && g_done == 2);

    /* forever: still alive after many passes, stopped by kill (no on_complete) */
    br_tween f = br_tween_to_f(e, t, "x", 10.0f, 0.1f, BR_EASE_LINEAR);
    br_tween_set_repeat(e, f, -1, false);
    br_tween_set_on_complete(e, f, on_done, (void *)(intptr_t)3);
    for (int i = 0; i < 30; i++) {
        step(e, 0.07f);
    }
    BR_CHECK(br_tween_alive(e, f));
    br_tween_kill(e, f);
    BR_CHECK(!br_tween_alive(e, f) && g_done == 2);
    br_node_destroy(e, n);
    step(e, 0.0f);
}

static void test_sequences(br_engine *e)
{
    br_node n = make_node(e, "seq");
    br_node m = make_node(e, "par");
    br_anim_target tn = br_anim_target_2d(n);
    br_anim_target tm = br_anim_target_2d(m);
    g_done = 0;
    g_order_count = 0;
    g_clock = 0.0f;
    br_tween_sequence seq = br_tween_sequence_begin(42);
    br_tween a = br_tween_to_f(e, tn, "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_set_on_complete(e, a, on_done, (void *)(intptr_t)1);
    br_tween_sequence_add(e, &seq, a);                                   /* 0 .. 1 */
    br_tween b = br_tween_to_f(e, tm, "x", 10.0f, 0.5f, BR_EASE_LINEAR);
    br_tween_set_on_complete(e, b, on_done, (void *)(intptr_t)2);
    br_tween_sequence_join(e, &seq, b);                                  /* 0 .. 0.5, with a */
    br_tween_sequence_wait(&seq, 0.5f);                                  /* 1 .. 1.5 */
    br_tween c = br_tween_to_f(e, tn, "y", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_set_delay(e, c, 0.25f);                                     /* its own delay counts */
    br_tween_set_on_complete(e, c, on_done, (void *)(intptr_t)3);
    br_tween_sequence_add(e, &seq, c);                                   /* 1.75 .. 2.75 */
    br_tween_sequence_call(e, &seq, on_done, (void *)(intptr_t)4);       /* at 2.75 */
    BR_CHECK(near_eq(seq.cursor, 2.75f));
    for (int i = 0; i < 70; i++) {
        step(e, 0.05f);
        if (i == 29) { /* t = 1.5: a and b done, c waiting */
            BR_CHECK(near_eq(br_node_position(e, n).x, 10.0f) && near_eq(br_node_position(e, n).y, 0.0f));
        }
    }
    BR_CHECK(g_order_count == 4 && g_order[0] == 2 && g_order[1] == 1 && g_order[2] == 3 && g_order[3] == 4);
    BR_CHECK(SDL_fabsf(g_done_at[0] - 0.5f) < 0.06f && SDL_fabsf(g_done_at[1] - 1.0f) < 0.06f);
    BR_CHECK(SDL_fabsf(g_done_at[2] - 2.75f) < 0.06f && SDL_fabsf(g_done_at[3] - 2.75f) < 0.06f);
    BR_CHECK(near_eq(br_node_position(e, n).y, 10.0f));

    /* kill a whole sequence by its tag */
    br_tween_sequence s2 = br_tween_sequence_begin(7);
    br_tween_sequence_add(e, &s2, br_tween_to_f(e, tn, "x", 0.0f, 1.0f, BR_EASE_LINEAR));
    br_tween_sequence_add(e, &s2, br_tween_to_f(e, tm, "x", 0.0f, 1.0f, BR_EASE_LINEAR));
    br_tween_sequence_call(e, &s2, on_done, (void *)(intptr_t)5);
    step(e, 0.5f);
    BR_CHECK(br_tween_kill_tag(e, 7) == 3 && br_tween_count(e) == 0);
    step(e, 3.0f);
    BR_CHECK(g_order_count == 4);
    br_node_destroy(e, n);
    br_node_destroy(e, m);
    step(e, 0.0f);
}

static br_tween g_victim;
static br_anim_target g_kill_target;

static void on_done_kill(br_engine *e, br_tween t, void *user)
{
    (void)t;
    (void)user;
    br_tween_kill(e, g_victim);
    br_tween_to_f(e, g_kill_target, "y", 5.0f, 0.1f, BR_EASE_LINEAR); /* starts next frame */
}

static void test_kill(br_engine *e)
{
    br_node n = make_node(e, "victim");
    br_node other = make_node(e, "other");
    br_anim_target t = br_anim_target_2d(n);
    br_tween_to_f(e, t, "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_to_f(e, t, "alpha", 0.0f, 1.0f, BR_EASE_LINEAR);
    br_tween keep = br_tween_to_f(e, br_anim_target_2d(other), "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    BR_CHECK(br_tween_kill_target(e, t) == 2 && br_tween_count(e) == 1 && br_tween_alive(e, keep));
    /* destroying the node kills its tweens */
    br_tween_to_f(e, t, "x", 10.0f, 1.0f, BR_EASE_LINEAR);
    br_tween_to_f(e, t, "y", 10.0f, 1.0f, BR_EASE_LINEAR);
    step(e, 0.1f);
    BR_CHECK(br_tween_count(e) == 3);
    br_node_destroy(e, n);
    step(e, 0.1f);
    BR_CHECK(br_tween_count(e) == 1 && br_tween_alive(e, keep));
    br_node_destroy(e, other);
    step(e, 0.1f);
    BR_CHECK(br_tween_count(e) == 0);

    /* a tween made through a child path is killed by the node the path was given from */
    br_node parent = make_node(e, "kparent");
    br_node child = br_node_create_container(e);
    br_node_set_label(e, child, "label");
    br_node_add_child(e, parent, child);
    br_tween_to_f(e, br_anim_target_2d(parent), "label:alpha", 0.0f, 1.0f, BR_EASE_LINEAR);
    BR_CHECK(br_tween_kill_target(e, br_anim_target_2d(parent)) == 1 && br_tween_count(e) == 0);
    br_tween_to_f(e, br_anim_target_2d(parent), "label:alpha", 0.0f, 1.0f, BR_EASE_LINEAR);
    BR_CHECK(br_tween_kill_target(e, br_anim_target_2d(child)) == 1); /* the resolved node still works */

    /* killing and creating tweens from inside an on_complete callback */
    g_kill_target = br_anim_target_2d(parent);
    g_victim = br_tween_to_f(e, g_kill_target, "alpha", 0.0f, 1.0f, BR_EASE_LINEAR);
    br_tween first = br_tween_to_f(e, g_kill_target, "x", 10.0f, 0.1f, BR_EASE_LINEAR);
    br_tween_set_on_complete(e, first, on_done_kill, NULL);
    step(e, 0.15f);
    BR_CHECK(!br_tween_alive(e, first) && !br_tween_alive(e, g_victim) && br_tween_count(e) == 1);
    step(e, 0.2f);
    BR_CHECK(br_tween_count(e) == 0 && near_eq(br_node_position(e, parent).y, 5.0f));
    br_node_destroy(e, parent);
    step(e, 0.0f);
}

static void test_3d_and_components(br_engine *e)
{
    br_node3d g = br_node3d_create_group(e);
    float p[3] = { 2.0f, 4.0f, 6.0f };
    br_tween_to(e, br_anim_target_3d(g), "position", p, 1.0f, BR_EASE_LINEAR);
    br_quat q1 = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), 1.0f);
    float q[4] = { q1.x, q1.y, q1.z, q1.w };
    br_tween_to(e, br_anim_target_3d(g), "rotation", q, 1.0f, BR_EASE_LINEAR);
    step(e, 0.5f);
    br_vec3 at = br_node3d_position(e, g);
    BR_CHECK(near_eq(at.x, 1.0f) && near_eq(at.y, 2.0f) && near_eq(at.z, 3.0f));
    br_quat half = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), 0.5f);
    BR_CHECK(near_eq(SDL_fabsf(br_quat_dot(br_node3d_rotation(e, g), half)), 1.0f));
    step(e, 1.0f);
    br_node3d_destroy(e, g);
    step(e, 0.0f);
    BR_CHECK(br_tween_count(e) == 0);

    typedef struct glow { float power; } glow;
    static const br_field_desc fields[] = { { "power", BR_FIELD_FLOAT, offsetof(glow, power) } };
    br_component_desc desc = { "glow", sizeof(glow), fields, 1 };
    uint32_t id = br_component_register(e, &desc);
    br_node n = make_node(e, "glowing");
    br_component_add(e, n, id);
    br_tween_to_f(e, br_anim_target_2d(n), "glow.power", 8.0f, 1.0f, BR_EASE_LINEAR);
    step(e, 0.25f);
    BR_CHECK(near_eq(((glow *)br_component_get(e, n, id))->power, 2.0f));
    br_node_destroy(e, n);
    step(e, 0.0f);
}

static void test_delay_call(br_engine *e)
{
    g_done = 0;
    br_tween c = br_tween_delay_call(e, 0.5f, on_done, NULL);
    step(e, 0.25f);
    BR_CHECK(g_done == 0 && br_tween_alive(e, c));
    step(e, 0.3f);
    BR_CHECK(g_done == 1 && !br_tween_alive(e, c));
}

static int g_destroyed = 0;

static void on_destroy_root(br_engine *e, const br_anim_event *ev, void *user)
{
    (void)user;
    if (br_node_valid(e, br_anim_target_node(ev->root))) {
        br_node_destroy(e, br_anim_target_node(ev->root));
        g_destroyed++;
    }
}

/* The scene-file components, with data files written next to the test executable. */
static void test_scene_components(void)
{
    const char *base = SDL_GetBasePath();
    br_engine *e = br_engine_create("test_tween_files", base);
    if (e == NULL) {
        BR_CHECK(e != NULL);
        return;
    }
    const char *anim = "{ \"clips\": [ { \"name\": \"slide\", \"tracks\": [ { \"target\": \"arm:x\", \"keys\": [[0, 0], [1, 100]] } ],"
                       "    \"events\": [ { \"time\": 0.25, \"name\": \"mid\" } ] },"
                       "  { \"name\": \"rest\", \"tracks\": [ { \"target\": \"arm:x\", \"keys\": [[0, -5]] } ] } ] }";
    const char *graph = "{ \"parameters\": { \"go\": false }, \"entry\": \"still\","
                        "  \"states\": [ { \"name\": \"still\", \"clip\": \"rest\" }, { \"name\": \"moving\", \"clip\": \"slide\" } ],"
                        "  \"transitions\": [ { \"from\": \"still\", \"to\": \"moving\", \"when\": [[\"go\", \"==\", true]], \"duration\": 0 } ] }";
    BR_CHECK(br_file_write_all(e, "tween_test.anim.json", anim, SDL_strlen(anim)));
    BR_CHECK(br_file_write_all(e, "tween_test.animgraph.json", graph, SDL_strlen(graph)));
    const br_anim_library *lib = br_anim_library_load(e, "tween_test.anim.json");
    BR_CHECK(lib != NULL && lib->count == 2 && SDL_strcmp(br_assets_type(e, "tween_test.anim.json"), "anim") == 0);
    BR_CHECK(br_anim_graph_load(e, "tween_test.animgraph.json") != NULL);
    BR_CHECK(SDL_strcmp(br_assets_type(e, "tween_test.animgraph.json"), "animgraph") == 0);

    const char *scene =
        "{ \"type\": \"container\", \"label\": \"root\", \"children\": ["
        "  { \"type\": \"container\", \"label\": \"panel\", \"alpha\": 0.5,"
        "    \"components\": { \"tween\": { \"property\": \"alpha\", \"to\": 1, \"duration\": 1, \"delay\": 0.5 } } },"
        "  { \"type\": \"container\", \"label\": \"tinted\","
        "    \"components\": { \"tween\": { \"property\": \"tint\", \"mode\": \"from_to\", \"from_color\": \"#000000\", \"to_color\": \"#ffffff\", \"duration\": 1, \"ease\": \"in_quad\" } } },"
        "  { \"type\": \"container\", \"label\": \"rig\", \"children\": [ { \"type\": \"container\", \"label\": \"arm\" } ],"
        "    \"components\": { \"animator\": { \"clips\": \"tween_test.anim.json\", \"play\": \"slide\", \"speed\": 2 } } },"
        "  { \"type\": \"container\", \"label\": \"brain\", \"children\": [ { \"type\": \"container\", \"label\": \"arm\" } ],"
        "    \"components\": { \"animator\": { \"clips\": \"tween_test.anim.json\", \"graph\": \"tween_test.animgraph.json\" } } } ] }";
    br_node root = br_scene_file_load_json(e, scene, SDL_strlen(scene), "components.scene.json", br_stage_root(e));
    BR_CHECK(br_node_valid(e, root));
    br_node panel = br_node_find_by_label(e, root, "panel", true);
    br_node tinted = br_node_find_by_label(e, root, "tinted", true);
    br_node rig = br_node_find_by_label(e, root, "rig", true);
    br_node brain = br_node_find_by_label(e, root, "brain", true);
    br_engine_step(e, 0.25f); /* the components start: tween delays begin, the animator plays from 0 */
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(br_node_alpha(e, panel), 0.625f));    /* 0.25 s past the delay: 0.5 -> 1 */
    BR_CHECK(near_eq(br_node_tint(e, tinted).r, 0.5625f));   /* in_quad(0.75) */
    BR_CHECK(near_eq(br_node_position(e, br_node_find_by_label(e, rig, "arm", false)).x, 50.0f)); /* 1.5 s at speed 2, looped */
    br_animator a = br_animator_get(e, br_anim_target_2d(brain));
    BR_CHECK(br_animator_valid(e, a) && SDL_strcmp(br_animator_state(e, a), "still") == 0);
    BR_CHECK(near_eq(br_node_position(e, br_node_find_by_label(e, brain, "arm", false)).x, -5.0f));
    br_animator_set_bool(e, a, "go", true);
    br_engine_step(e, 0.1f);
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "moving") == 0);

    /* each animator component holds one reference per asset (plus the test's own) */
    const char *lib_path = "tween_test.anim.json";
    const char *graph_path = "tween_test.animgraph.json";
    BR_CHECK(br_assets_ref_count(e, lib_path) == 3 && br_assets_ref_count(e, graph_path) == 2);
    /* a callback destroying its node: the update's events stay valid, the reference goes next update */
    br_animator_set_event_callback(e, br_animator_get(e, br_anim_target_2d(rig)), on_destroy_root, NULL);
    br_engine_step(e, 0.5f); /* clip time 0.7 -> 1.7: passes "mid" */
    uint32_t count = 0;
    const br_anim_event *ev = br_animation_events(e, &count);
    BR_CHECK(g_destroyed == 1 && !br_node_valid(e, rig) && count >= 1 && SDL_strcmp(ev[0].name, "mid") == 0);
    BR_CHECK(br_assets_ref_count(e, lib_path) == 3);
    br_engine_step(e, 0.0f);
    BR_CHECK(br_assets_ref_count(e, lib_path) == 2);
    br_node_destroy(e, root);
    br_engine_step(e, 0.0f);
    BR_CHECK(br_animator_count(e) == 0 && br_tween_count(e) == 0);
    BR_CHECK(br_assets_ref_count(e, lib_path) == 1 && br_assets_ref_count(e, graph_path) == 1);

    /* glTF node extras start an animator and a tween on 3D nodes */
    const char *rise = "{ \"clips\": [ { \"name\": \"rise\", \"tracks\": [ { \"target\": \":position\", \"keys\": [[0, [0, 0, 0]], [1, [0, 10, 0]]] } ] } ] }";
    const char *gltf =
        "{ \"asset\": { \"version\": \"2.0\" }, \"scene\": 0, \"scenes\": [ { \"nodes\": [0, 1] } ],"
        "  \"nodes\": [ { \"name\": \"lift\", \"extras\": { \"animator.clips\": \"tween_test3d.anim.json\", \"animator.play\": \"rise\" } },"
        "               { \"name\": \"slider\", \"extras\": { \"tween.property\": \"position\", \"tween.to3\": [2, 4, 6], \"tween.duration\": 1 } } ] }";
    BR_CHECK(br_file_write_all(e, "tween_test3d.anim.json", rise, SDL_strlen(rise)));
    BR_CHECK(br_file_write_all(e, "tween_test.gltf", gltf, SDL_strlen(gltf)));
    br_node3d none = { 0, 0 };
    br_node3d inst = br_gltf_instantiate(e, "tween_test.gltf", none, NULL);
    BR_CHECK(br_node3d_valid(e, inst));
    br_engine_step(e, 0.5f);
    br_node3d lift = br_model3d_node(e, inst, 0);
    br_node3d slider = br_model3d_node(e, inst, 1);
    BR_CHECK(near_eq(br_node3d_position(e, lift).y, 5.0f));
    br_vec3 sp = br_node3d_position(e, slider);
    BR_CHECK(near_eq(sp.x, 1.0f) && near_eq(sp.y, 2.0f) && near_eq(sp.z, 3.0f));
    BR_CHECK(br_assets_ref_count(e, "tween_test3d.anim.json") == 1);
    br_node3d_destroy(e, inst);
    br_engine_step(e, 0.0f);
    BR_CHECK(br_animator_count(e) == 0 && br_tween_count(e) == 0 && br_assets_ref_count(e, "tween_test3d.anim.json") == 0);
    br_engine_destroy(e);
}

int main(void)
{
    br_engine *e = br_engine_create("test_tween", NULL);
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_basics(e);
        test_repeat_yoyo(e);
        test_sequences(e);
        test_kill(e);
        test_3d_and_components(e);
        test_delay_call(e);
        BR_CHECK(br_tween_count(e) == 0);
        br_engine_destroy(e);
    }
    test_scene_components();
    return br_test_finish();
}
