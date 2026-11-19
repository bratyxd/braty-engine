/* test_node - hierarchy operations, node types, deferred destroy, stale handles (headless). */
#include "braty/braty.h"
#include "br_test.h"

typedef struct counter_data {
    int value;
} counter_data;

static int g_created;
static int g_destroyed;
static int g_listener_calls;

static void counter_create(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    counter_data *c = data;
    c->value = 42;
    g_created++;
}

static void counter_destroy(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    (void)data;
    g_destroyed++;
}

static void on_node_destroyed(br_engine *e, br_node n, void *user)
{
    (void)user;
    /* the handle is already invalid while the listener runs */
    BR_CHECK(!br_node_valid(e, n));
    g_listener_calls++;
}

static void test_hierarchy(br_engine *e)
{
    br_node root = br_scene_root(e);
    BR_CHECK(br_node_valid(e, root));
    br_node a = br_node_create_container(e);
    br_node b = br_node_create_container(e);
    br_node c = br_node_create_container(e);
    br_node_set_label(e, a, "a");
    br_node_set_label(e, b, "b");
    br_node_set_label(e, c, "c");

    BR_CHECK(br_node_add_child(e, root, a));
    BR_CHECK(br_node_add_child(e, a, b));
    BR_CHECK(br_node_add_child(e, a, c));
    BR_CHECK(br_node_child_count(e, a) == 2);
    BR_CHECK(br_node_child_index(e, a, b) == 0);
    BR_CHECK(br_node_child_index(e, a, c) == 1);
    BR_CHECK(br_node_equal(br_node_parent(e, b), a));
    BR_CHECK(br_node_is_descendant_of(e, c, root));

    /* cycles are rejected */
    BR_CHECK(!br_node_add_child(e, b, a));
    BR_CHECK(!br_node_add_child(e, a, a));
    BR_CHECK(!br_node_add_child(e, a, root));

    BR_CHECK(br_node_swap_children(e, a, b, c));
    BR_CHECK(br_node_child_index(e, a, c) == 0);
    BR_CHECK(br_node_set_child_index(e, a, c, 5)); /* clamped to the end */
    BR_CHECK(br_node_child_index(e, a, c) == 1);
    BR_CHECK(br_node_equal(br_node_child_at(e, a, 0), b));

    /* add_child_at and re-adding moves the node */
    BR_CHECK(br_node_add_child_at(e, root, c, 0));
    BR_CHECK(br_node_child_count(e, a) == 1);
    BR_CHECK(br_node_child_index(e, root, c) == 0);

    BR_CHECK(br_node_equal(br_node_find_by_label(e, root, "b", true), b));
    BR_CHECK(br_node_find_by_label(e, root, "b", false).generation == 0);
    BR_CHECK(SDL_strcmp(br_node_label(e, b), "b") == 0);

    BR_CHECK(br_node_remove_child(e, root, c));
    BR_CHECK(!br_node_remove_child(e, root, c));
    BR_CHECK(br_node_parent(e, c).generation == 0);
    br_node_add_child(e, a, c);
    br_node_remove_children(e, a);
    BR_CHECK(br_node_child_count(e, a) == 0);
    br_node_destroy(e, a);
    br_node_destroy(e, b);
    br_node_destroy(e, c);
}

static void test_types_and_stale(br_engine *e)
{
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "counter";
    desc.data_size = sizeof(counter_data);
    desc.on_create = counter_create;
    desc.on_destroy = counter_destroy;
    uint32_t type = br_node_type_register(e, &desc);
    BR_CHECK(type != 0);
    BR_CHECK(br_node_type_register(e, &desc) == 0); /* duplicate name */
    BR_CHECK(br_node_type_find(e, "counter") == type);
    BR_CHECK(br_node_type_find(e, "sprite") == br_sprite_type(e));

    br_node n1 = br_node_create(e, type);
    br_node n2 = br_node_create(e, type);
    br_node n3 = br_node_create(e, type);
    BR_CHECK(g_created == 3);
    counter_data *d = br_node_data(e, n2, type);
    BR_CHECK(d != NULL && d->value == 42);
    d->value = 2;
    ((counter_data *)br_node_data(e, n3, type))->value = 3;
    BR_CHECK(br_node_data(e, n2, BR_NODE_TYPE_CONTAINER) == NULL); /* wrong type */

    /* outside a frame, destroy is immediate; the dense pool keeps the others right */
    br_node_destroy(e, n1);
    BR_CHECK(g_destroyed == 1);
    BR_CHECK(!br_node_valid(e, n1));
    BR_CHECK(((counter_data *)br_node_data(e, n2, type))->value == 2);
    BR_CHECK(((counter_data *)br_node_data(e, n3, type))->value == 3);

    /* stale handles are rejected everywhere, even after the slot is reused */
    br_node reused = br_node_create_container(e);
    BR_CHECK(reused.index == n1.index && reused.generation != n1.generation);
    BR_CHECK(br_node_data(e, n1, type) == NULL);
    BR_CHECK(!br_node_add_child(e, br_scene_root(e), n1));
    br_node_set_position(e, n1, 5.0f, 5.0f); /* ignored */
    BR_CHECK(br_node_position(e, reused).x == 0.0f);
    BR_CHECK(br_node_child_count(e, n1) == 0);
    br_node_destroy(e, n1); /* no-op */
    BR_CHECK(br_node_valid(e, reused));
    br_node zero = { 0, 0 };
    BR_CHECK(!br_node_valid(e, zero));

    br_node_destroy(e, reused);
    br_node_destroy(e, n2);
    br_node_destroy(e, n3);
}

/* ---- deferred destroy while a system runs */

typedef struct destroy_test {
    br_node  parent;
    br_node  victim;
    br_node  grandchild;
    uint32_t component;
    int      frame;
    bool     checked;
} destroy_test;

static void destroy_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    destroy_test *t = user;
    t->frame++;
    if (t->frame != 1) {
        return;
    }
    /* iterate the parent's children and destroy one of them mid-loop */
    uint32_t count = br_node_child_count(e, t->parent);
    uint32_t visited = 0;
    for (uint32_t i = 0; i < br_node_child_count(e, t->parent); i++) {
        br_node child = br_node_child_at(e, t->parent, i);
        if (br_node_equal(child, t->victim)) {
            br_node_destroy(e, child);
        }
        visited++;
    }
    BR_CHECK(visited == count); /* the list did not shift under the loop */
    BR_CHECK(!br_node_valid(e, t->victim));
    BR_CHECK(!br_node_valid(e, t->grandchild)); /* subtree goes with it */
    BR_CHECK(br_node_child_count(e, t->parent) == count); /* memory and links kept until the flush */
    /* components of the dying node stay in the dense array during this frame */
    BR_CHECK(br_component_count(e, t->component) == 2);
    BR_CHECK(br_component_get(e, t->victim, t->component) == NULL);
    bool found_invalid_owner = false;
    for (uint32_t i = 0; i < br_component_count(e, t->component); i++) {
        if (!br_node_valid(e, br_component_owner_at(e, t->component, i))) {
            found_invalid_owner = true;
        }
    }
    BR_CHECK(found_invalid_owner);
    BR_CHECK(g_listener_calls == 0);
    t->checked = true;
}

static void test_deferred_destroy(br_engine *e)
{
    br_component_desc cdesc = { "tag", sizeof(int), NULL, 0 };
    destroy_test t;
    SDL_zero(t);
    t.component = br_component_register(e, &cdesc);
    t.parent = br_node_create_container(e);
    br_node_add_child(e, br_scene_root(e), t.parent);
    br_node first = br_node_create_container(e);
    t.victim = br_node_create_container(e);
    br_node last = br_node_create_container(e);
    t.grandchild = br_node_create_container(e);
    br_node_add_child(e, t.parent, first);
    br_node_add_child(e, t.parent, t.victim);
    br_node_add_child(e, t.parent, last);
    br_node_add_child(e, t.victim, t.grandchild);
    *(int *)br_component_add(e, t.victim, t.component) = 7;
    *(int *)br_component_add(e, last, t.component) = 9;
    uint32_t nodes_before = br_scene_node_count(e);

    br_scene_on_node_destroy(e, on_node_destroyed, NULL);
    br_system_add(e, "test.destroy", BR_PHASE_UPDATE, 0, destroy_system, &t);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(t.checked);
    BR_CHECK(br_scene_node_count(e) == nodes_before);
    br_engine_step(e, 1.0f / 60.0f); /* the flush runs at the start of this frame */
    BR_CHECK(g_listener_calls == 2);
    BR_CHECK(br_scene_node_count(e) == nodes_before - 2);
    BR_CHECK(br_node_child_count(e, t.parent) == 2);
    BR_CHECK(br_node_child_index(e, t.parent, last) == 1);
    BR_CHECK(br_component_count(e, t.component) == 1);
    BR_CHECK(*(int *)br_component_get(e, last, t.component) == 9);
    br_system_set_enabled(e, "test.destroy", false);
    br_node_destroy(e, t.parent);
}

static void test_sort_and_layers(br_engine *e)
{
    br_node root = br_scene_root(e);
    br_node parent = br_node_create_container(e);
    br_node_add_child(e, root, parent);
    br_node kids[4];
    int z[4] = { 3, 1, 3, 0 };
    for (int i = 0; i < 4; i++) {
        kids[i] = br_node_create_container(e);
        br_node_add_child(e, parent, kids[i]);
        br_node_set_z_index(e, kids[i], z[i]);
    }
    br_node_set_sortable_children(e, parent, true);
    br_engine_step(e, 1.0f / 60.0f);
    /* stable ascending: z 0, 1, 3 (kids[0] before kids[2]) */
    BR_CHECK(br_node_equal(br_node_child_at(e, parent, 0), kids[3]));
    BR_CHECK(br_node_equal(br_node_child_at(e, parent, 1), kids[1]));
    BR_CHECK(br_node_equal(br_node_child_at(e, parent, 2), kids[0]));
    BR_CHECK(br_node_equal(br_node_child_at(e, parent, 3), kids[2]));

    br_node layer = br_node_create_container(e);
    br_node_add_child(e, root, layer);
    BR_CHECK(br_node_layer_attach(e, layer, kids[1]));
    BR_CHECK(br_node_equal(br_node_layer(e, kids[1]), layer));
    br_node_destroy(e, layer);
    BR_CHECK(br_node_layer(e, kids[1]).generation == 0);
    br_node_destroy(e, parent);
}

static void test_fields(br_engine *e)
{
    br_node n = br_node_create_container(e);
    BR_CHECK(br_node_event_mode(e, n) == BR_EVENT_MODE_PASSIVE);
    BR_CHECK(br_node_interactive_children(e, n));
    BR_CHECK(br_node_cursor(e, n) == -1);
    br_vec2 tri[3] = { { 0, 0 }, { 10, 0 }, { 0, 10 } };
    br_hit_area area;
    SDL_zero(area);
    area.type = BR_HIT_AREA_POLYGON;
    area.points = tri;
    area.point_count = 3;
    br_node_set_hit_area(e, n, &area);
    br_hit_area got = br_node_hit_area(e, n);
    BR_CHECK(got.points != tri && got.point_count == 3); /* copied */
    BR_CHECK(br_hit_area_contains(&got, br_vec2_make(2, 2)));
    BR_CHECK(!br_hit_area_contains(&got, br_vec2_make(8, 8)));

    br_node m = br_node_create_container(e);
    br_node_set_mask(e, n, m, BR_MASK_STENCIL, true);
    br_mask_kind kind;
    bool inverse;
    BR_CHECK(br_node_equal(br_node_mask(e, n, &kind, &inverse), m));
    BR_CHECK(kind == BR_MASK_STENCIL && inverse);
    BR_CHECK(!br_node_needs_effects(e, n));
    br_node_set_cache_as_texture(e, n, true);
    BR_CHECK(br_node_needs_effects(e, n));
    br_node_destroy(e, m);
    BR_CHECK(br_node_mask(e, n, NULL, NULL).generation == 0);
    br_node_destroy(e, n);
}

int main(void)
{
    br_engine *e = br_engine_create("test_node", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    uint32_t base = br_scene_node_count(e);
    BR_CHECK(base == 1); /* the stage */
    test_hierarchy(e);
    test_types_and_stale(e);
    test_deferred_destroy(e);
    test_sort_and_layers(e);
    test_fields(e);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_scene_node_count(e) == base);
    br_engine_destroy(e);
    BR_CHECK(g_destroyed == g_created);
    return br_test_finish();
}
