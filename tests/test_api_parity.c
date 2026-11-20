/* test_api_parity - every mirrored 2D / 3D call pair behaves the same way: create, destroy,
 * parent (add_child, add_child_at, remove_from_parent, reparent), child iteration (child_count +
 * child_at), find_by_label (root excluded, deep or not), visibility, layer bits, position /
 * rotation / scale getters and setters, to_global / to_local, destroy listeners, counts and
 * components. The 3D child cursor is checked against inserts and destroys. Headless. */
#include "braty/braty.h"
#include "br_test.h"

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-3f;
}

static bool vec2_eq(br_vec2 a, br_vec2 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y);
}

static bool vec3_eq(br_vec3 a, br_vec3 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y) && near_eq(a.z, b.z);
}

static uint32_t g_destroyed_2d;
static uint32_t g_destroyed_3d;

static void on_destroy_2d(br_engine *e, br_node n, void *user)
{
    (void)e;
    (void)n;
    (void)user;
    g_destroyed_2d++;
}

static void on_destroy_3d(br_engine *e, br_node3d n, void *user)
{
    (void)e;
    (void)n;
    (void)user;
    g_destroyed_3d++;
}

/* Labels of n's children in order, joined ("a,b,c"), through child_count + child_at. */
static void labels_2d(br_engine *e, br_node n, char *out, size_t size)
{
    out[0] = '\0';
    for (uint32_t i = 0; i < br_node_child_count(e, n); i++) {
        SDL_strlcat(out, i > 0 ? "," : "", size);
        SDL_strlcat(out, br_node_label(e, br_node_child_at(e, n, i)), size);
    }
}

static void labels_3d(br_engine *e, br_node3d n, char *out, size_t size)
{
    out[0] = '\0';
    for (uint32_t i = 0; i < br_node3d_child_count(e, n); i++) {
        SDL_strlcat(out, i > 0 ? "," : "", size);
        SDL_strlcat(out, br_node3d_label(e, br_node3d_child_at(e, n, i)), size);
    }
}

static br_node child_2d(br_engine *e, br_node parent, const char *label)
{
    br_node n = br_node_create_container(e);
    br_node_set_label(e, n, label);
    br_node_add_child(e, parent, n);
    return n;
}

static br_node3d child_3d(br_engine *e, br_node3d parent, const char *label)
{
    br_node3d n = br_node3d_create_container(e);
    br_node3d_set_label(e, n, label);
    br_node3d_add_child(e, parent, n);
    return n;
}

/* create / destroy / counts / destroy listeners */
static void test_lifetime(br_engine *e)
{
    uint32_t nodes = br_node_count(e);
    uint32_t nodes3d = br_node3d_count(e);
    br_node a = br_node_create_container(e);
    br_node3d a3 = br_node3d_create_container(e);
    BR_CHECK(br_node_valid(e, a) && br_node_type_of(e, a) == BR_NODE_TYPE_CONTAINER);
    BR_CHECK(br_node3d_valid(e, a3) && br_node3d_type_of(e, a3) == BR_NODE3D_TYPE_CONTAINER);
    BR_CHECK(br_node_count(e) == nodes + 1 && br_node3d_count(e) == nodes3d + 1);
    br_node_add_child(e, a, br_node_create_container(e));
    br_node3d_add_child(e, a3, br_node3d_create_container(e));
    g_destroyed_2d = 0;
    g_destroyed_3d = 0;
    br_node_destroy(e, a);     /* outside a frame: freed now, listeners run once per node */
    br_node3d_destroy(e, a3);
    BR_CHECK(!br_node_valid(e, a) && !br_node3d_valid(e, a3));
    BR_CHECK(g_destroyed_2d == 2 && g_destroyed_3d == 2);
    BR_CHECK(br_node_count(e) == nodes && br_node3d_count(e) == nodes3d);
}

/* add_child / add_child_at / remove_from_parent / parent / child_count + child_at */
static void test_hierarchy(br_engine *e)
{
    char got[64];
    br_node root = br_node_create_container(e);
    br_node3d root3 = br_node3d_create_container(e);
    br_node a = child_2d(e, root, "a");
    br_node3d a3 = child_3d(e, root3, "a");
    child_2d(e, root, "c");
    child_3d(e, root3, "c");
    br_node b = br_node_create_container(e);
    br_node3d b3 = br_node3d_create_container(e);
    br_node_set_label(e, b, "b");
    br_node3d_set_label(e, b3, "b");
    BR_CHECK(br_node_add_child_at(e, root, b, 1));
    BR_CHECK(br_node3d_add_child_at(e, root3, b3, 1));
    labels_2d(e, root, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,b,c") == 0);
    labels_3d(e, root3, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,b,c") == 0);
    /* index past the end appends, index 0 prepends */
    br_node_add_child_at(e, root, br_node_create_container(e), 99);
    br_node3d_add_child_at(e, root3, br_node3d_create_container(e), 99);
    br_node_set_label(e, br_node_child_at(e, root, 3), "d");
    br_node3d_set_label(e, br_node3d_child_at(e, root3, 3), "d");
    br_node_add_child_at(e, root, a, 0);
    br_node3d_add_child_at(e, root3, a3, 0);
    labels_2d(e, root, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,b,c,d") == 0);
    labels_3d(e, root3, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,b,c,d") == 0);
    BR_CHECK(br_node_equal(br_node_parent(e, b), root));
    BR_CHECK(br_node3d_parent(e, b3).index == root3.index);
    BR_CHECK(br_node_child_at(e, root, 4).index == 0 && br_node3d_child_at(e, root3, 4).index == 0);
    /* cycles are refused */
    BR_CHECK(!br_node_add_child(e, a, root) && !br_node3d_add_child(e, a3, root3));

    /* the 3D cursor follows changes: walk, remove one, walk again, read backwards */
    br_node_remove_from_parent(e, b);
    br_node3d_remove_from_parent(e, b3);
    BR_CHECK(br_node_parent(e, b).index == 0 && br_node3d_parent(e, b3).index == 0);
    labels_2d(e, root, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,c,d") == 0);
    labels_3d(e, root3, got, sizeof(got));
    BR_CHECK(SDL_strcmp(got, "a,c,d") == 0);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, br_node3d_child_at(e, root3, 2)), "d") == 0);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, br_node3d_child_at(e, root3, 1)), "c") == 0);
    BR_CHECK(br_node3d_child_count(e, root3) == 3);
    br_node3d_destroy(e, br_node3d_child_at(e, root3, 1));
    BR_CHECK(br_node3d_child_count(e, root3) == 2);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, br_node3d_child_at(e, root3, 1)), "d") == 0);
    /* a destroyed childless root's slot is reused: no stale count or cursor */
    br_node3d lone = br_node3d_create_container(e);
    child_3d(e, lone, "x");
    BR_CHECK(br_node3d_child_count(e, lone) == 1 && br_node3d_child_at(e, lone, 0).index != 0);
    br_node3d_destroy(e, br_node3d_child_at(e, lone, 0));
    br_node3d_destroy(e, lone);
    br_node3d fresh = br_node3d_create_container(e);
    BR_CHECK(br_node3d_child_count(e, fresh) == 0 && br_node3d_child_at(e, fresh, 0).index == 0);
    br_node3d_destroy(e, fresh);

    br_node_destroy(e, b);
    br_node3d_destroy(e, b3);
    br_node_destroy(e, root);
    br_node3d_destroy(e, root3);
}

/* find_by_label: root is not matched; deep finds grandchildren, shallow does not */
static void test_find(br_engine *e)
{
    br_node root = br_node_create_container(e);
    br_node3d root3 = br_node3d_create_container(e);
    br_node_set_label(e, root, "root");
    br_node3d_set_label(e, root3, "root");
    br_node mid = child_2d(e, root, "mid");
    br_node3d mid3 = child_3d(e, root3, "mid");
    br_node leaf = child_2d(e, mid, "leaf");
    br_node3d leaf3 = child_3d(e, mid3, "leaf");
    child_2d(e, leaf, "mid");   /* a deeper node with the same label as a child: the child wins */
    child_3d(e, leaf3, "mid");
    BR_CHECK(br_node_find_by_label(e, root, "root", true).index == 0);
    BR_CHECK(br_node3d_find_by_label(e, root3, "root", true).index == 0);
    BR_CHECK(br_node_find_by_label(e, root, "leaf", false).index == 0);
    BR_CHECK(br_node3d_find_by_label(e, root3, "leaf", false).index == 0);
    BR_CHECK(br_node_equal(br_node_find_by_label(e, root, "leaf", true), leaf));
    BR_CHECK(br_node3d_find_by_label(e, root3, "leaf", true).index == leaf3.index);
    BR_CHECK(br_node_equal(br_node_find_by_label(e, root, "mid", true), mid));
    BR_CHECK(br_node3d_find_by_label(e, root3, "mid", true).index == mid3.index);
    BR_CHECK(br_node_find_by_label(e, root, "nope", true).index == 0);
    BR_CHECK(br_node3d_find_by_label(e, root3, "nope", true).index == 0);
    br_node_destroy(e, root);
    br_node3d_destroy(e, root3);
}

/* visibility and layer bits (3D: default 1 and no inheritance, as the header says) */
static void test_flags(br_engine *e)
{
    br_node n = br_node_create_container(e);
    br_node3d n3 = br_node3d_create_container(e);
    BR_CHECK(br_node_visible(e, n) && br_node3d_visible(e, n3));
    br_node_set_visible(e, n, false);
    br_node3d_set_visible(e, n3, false);
    BR_CHECK(!br_node_visible(e, n) && !br_node3d_visible(e, n3));
    BR_CHECK(br_node_layer_bits(e, n) == 0 && br_node3d_layer_bits(e, n3) == 1);
    br_node_set_layer_bits(e, n, 6);
    br_node3d_set_layer_bits(e, n3, 6);
    BR_CHECK(br_node_layer_bits(e, n) == 6 && br_node3d_layer_bits(e, n3) == 6);
    br_node_destroy(e, n);
    br_node3d_destroy(e, n3);
}

/* transform setters take what the getters return; to_global / to_local; reparent keeps the world */
static void test_transform(br_engine *e)
{
    br_node parent = br_node_create_container(e);
    br_node3d parent3 = br_node3d_create_container(e);
    br_node n = br_node_create_container(e);
    br_node3d n3 = br_node3d_create_container(e);
    br_node_add_child(e, br_stage_root(e), parent);
    br_node_add_child(e, parent, n);
    br_node3d_add_child(e, parent3, n3);

    br_node_set_position(e, n, br_vec2_make(3.0f, 4.0f));
    br_node3d_set_position(e, n3, br_vec3_make(3.0f, 4.0f, 5.0f));
    BR_CHECK(vec2_eq(br_node_position(e, n), br_vec2_make(3.0f, 4.0f)));
    BR_CHECK(vec3_eq(br_node3d_position(e, n3), br_vec3_make(3.0f, 4.0f, 5.0f)));
    br_node_set_scale(e, n, br_vec2_make(2.0f, 2.0f));
    br_node3d_set_scale(e, n3, br_vec3_make(2.0f, 2.0f, 2.0f));
    BR_CHECK(vec2_eq(br_node_scale(e, n), br_vec2_make(2.0f, 2.0f)));
    BR_CHECK(vec3_eq(br_node3d_scale(e, n3), br_vec3_make(2.0f, 2.0f, 2.0f)));
    br_node_set_rotation(e, n, 0.5f);
    br_node3d_set_euler(e, n3, br_vec3_make(0.0f, 0.5f, 0.0f));
    BR_CHECK(near_eq(br_node_rotation(e, n), 0.5f));
    BR_CHECK(vec3_eq(br_node3d_euler(e, n3), br_vec3_make(0.0f, 0.5f, 0.0f)));
    br_quat q = br_quat_from_euler(0.2f, 0.3f, 0.1f);
    br_node3d_set_rotation(e, n3, q);
    BR_CHECK(SDL_fabsf(br_quat_dot(br_node3d_rotation(e, n3), q)) > 0.9999f);
    BR_CHECK(vec3_eq(br_node3d_euler(e, n3), br_vec3_make(0.2f, 0.3f, 0.1f)));
    br_node3d_set_euler(e, n3, br_vec3_make(0.0f, 0.0f, 0.0f));
    br_node_set_rotation(e, n, 0.0f);

    /* parents moved: local <-> global round trips, and match the world matrices */
    br_node_set_position(e, parent, br_vec2_make(10.0f, 20.0f));
    br_node3d_set_position(e, parent3, br_vec3_make(10.0f, 20.0f, 30.0f));
    br_vec2 g2 = br_node_to_global(e, n, br_vec2_make(1.0f, 1.0f));
    br_vec3 g3 = br_node3d_to_global(e, n3, br_vec3_make(1.0f, 1.0f, 1.0f));
    BR_CHECK(vec2_eq(g2, br_vec2_make(15.0f, 26.0f)));
    BR_CHECK(vec3_eq(g3, br_vec3_make(15.0f, 26.0f, 37.0f)));
    BR_CHECK(vec2_eq(br_node_to_local(e, n, g2), br_vec2_make(1.0f, 1.0f)));
    BR_CHECK(vec3_eq(br_node3d_to_local(e, n3, g3), br_vec3_make(1.0f, 1.0f, 1.0f)));
    BR_CHECK(vec2_eq(br_mat3_apply(br_node_world_transform(e, n), br_vec2_make(1.0f, 1.0f)), g2));
    BR_CHECK(vec3_eq(br_mat4_transform_point(br_node3d_world_transform(e, n3), br_vec3_make(1.0f, 1.0f, 1.0f)), g3));
    BR_CHECK(vec3_eq(br_mat4_transform_point(br_node3d_local_transform(e, n3), br_vec3_make(1.0f, 1.0f, 1.0f)),
                     br_vec3_make(5.0f, 6.0f, 7.0f)));

    /* reparent to the stage / a detached root: same world position, new local values */
    br_node3d other3 = br_node3d_create_container(e);
    br_node3d_set_position(e, other3, br_vec3_make(-1.0f, 0.0f, 0.0f));
    br_node3d_set_scale(e, other3, br_vec3_make(0.5f, 0.5f, 0.5f));
    br_node other = child_2d(e, br_stage_root(e), "other");
    br_node_set_position(e, other, br_vec2_make(-1.0f, 0.0f));
    br_node_set_scale(e, other, br_vec2_make(0.5f, 0.5f));
    br_vec2 before2 = br_node_to_global(e, n, br_vec2_make(0.0f, 0.0f));
    br_vec3 before3 = br_node3d_world_position(e, n3);
    BR_CHECK(br_node_reparent(e, n, other));
    BR_CHECK(br_node3d_reparent(e, n3, other3));
    BR_CHECK(br_node_equal(br_node_parent(e, n), other) && br_node3d_parent(e, n3).index == other3.index);
    BR_CHECK(vec2_eq(br_node_to_global(e, n, br_vec2_make(0.0f, 0.0f)), before2));
    BR_CHECK(vec3_eq(br_node3d_world_position(e, n3), before3));
    BR_CHECK(vec2_eq(br_node_scale(e, n), br_vec2_make(4.0f, 4.0f)));
    BR_CHECK(vec3_eq(br_node3d_scale(e, n3), br_vec3_make(4.0f, 4.0f, 4.0f)));
    BR_CHECK(!br_node3d_reparent(e, other3, n3)); /* a cycle */

    br_node_destroy(e, parent);
    br_node_destroy(e, other);
    br_node3d_destroy(e, parent3);
    br_node3d_destroy(e, other3);
}

typedef struct parity_tag {
    int32_t value;
} parity_tag;

/* one component registry, rows for 2D and 3D nodes */
static void test_components(br_engine *e)
{
    static const br_field_desc fields[] = { { "value", BR_FIELD_INT, offsetof(parity_tag, value) } };
    br_component_desc desc = { "parity.tag", sizeof(parity_tag), fields, BR_ARRAY_COUNT(fields) };
    uint32_t id = br_component_register(e, &desc);
    BR_CHECK(id != 0 && br_component_desc_get(e, id) != NULL);
    br_node n = br_node_create_container(e);
    br_node3d n3 = br_node3d_create_container(e);
    parity_tag *t = br_component_add(e, n, id);
    parity_tag *t3 = br_component3d_add(e, n3, id);
    BR_CHECK(t != NULL && t3 != NULL);
    t->value = 2;
    t3->value = 3;
    BR_CHECK(br_component_count(e, id) == 1 && br_component3d_count(e, id) == 1);
    BR_CHECK(((parity_tag *)br_component_get(e, n, id))->value == 2);
    BR_CHECK(((parity_tag *)br_component3d_get(e, n3, id))->value == 3);
    BR_CHECK(br_node_equal(br_component_owner_at(e, id, 0), n));
    BR_CHECK(br_component3d_owner_at(e, id, 0).index == n3.index);
    BR_CHECK(((parity_tag *)br_component_at(e, id, 0))->value == 2);
    BR_CHECK(((parity_tag *)br_component3d_at(e, id, 0))->value == 3);
    br_component_remove(e, n, id);
    br_component3d_remove(e, n3, id);
    BR_CHECK(br_component_count(e, id) == 0 && br_component3d_count(e, id) == 0);
    br_node_destroy(e, n);
    br_node3d_destroy(e, n3);
}

int main(void)
{
    br_engine *e = br_engine_create("test_api_parity", NULL);
    BR_CHECK(e != NULL);
    if (e != NULL) {
        br_node_on_destroy(e, on_destroy_2d, NULL);
        br_node3d_on_destroy(e, on_destroy_3d, NULL);
        test_lifetime(e);
        test_hierarchy(e);
        test_find(e);
        test_flags(e);
        test_transform(e);
        test_components(e);
        br_engine_destroy(e);
    }
    return br_test_finish();
}
