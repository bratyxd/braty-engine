/* test_components - register/add/get/remove/iterate, swap-remove keeps owners right (headless). */
#include "braty/braty.h"
#include "br_test.h"

typedef struct velocity {
    br_vec2 value;
    float   damping;
    char    name[BR_FIELD_STRING_SIZE];
} velocity;

static const br_field_desc velocity_fields[] = {
    { "value", BR_FIELD_VEC2, offsetof(velocity, value) },
    { "damping", BR_FIELD_FLOAT, offsetof(velocity, damping) },
    { "name", BR_FIELD_STRING, offsetof(velocity, name) },
};

/* Every instance's owner must point back at the same data. */
static bool owners_consistent(br_engine *e, uint32_t id)
{
    for (uint32_t i = 0; i < br_component_count(e, id); i++) {
        br_node owner = br_component_owner_at(e, id, i);
        if (br_component_get(e, owner, id) != br_component_at(e, id, i)) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    br_engine *e = br_engine_create("test_components", NULL);
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    br_component_desc desc = { "velocity", sizeof(velocity), velocity_fields, BR_ARRAY_COUNT(velocity_fields) };
    uint32_t id = br_component_register(e, &desc);
    BR_CHECK(id != 0);
    BR_CHECK(br_component_register(e, &desc) == 0); /* duplicate */
    BR_CHECK(br_component_find(e, "velocity") == id);
    BR_CHECK(br_component_find(e, "nope") == 0);
    const br_component_desc *copy = br_component_get_desc(e, id);
    BR_CHECK(copy != NULL && copy->field_count == 3 && copy->fields != velocity_fields);
    BR_CHECK(copy != NULL && SDL_strcmp(copy->fields[1].name, "damping") == 0);

    /* a field that does not fit the struct (wrong offsetof) would let data write past it */
    br_field_desc past_end[] = { { "far", BR_FIELD_VEC2, sizeof(velocity) - 4 } };
    br_component_desc bad = { "bad_offset", sizeof(velocity), past_end, 1 };
    BR_CHECK(br_component_register(e, &bad) == 0);
    br_field_desc unknown_type[] = { { "odd", (br_field_type)99, 0 } };
    br_component_desc bad_type = { "bad_type", sizeof(velocity), unknown_type, 1 };
    BR_CHECK(br_component_register(e, &bad_type) == 0);
    br_field_desc no_name[] = { { NULL, BR_FIELD_INT, 0 } };
    br_component_desc bad_name = { "bad_name", sizeof(velocity), no_name, 1 };
    BR_CHECK(br_component_register(e, &bad_name) == 0);
    BR_CHECK(br_component_find(e, "bad_offset") == 0);

    br_node nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i] = br_node_create_container(e);
        velocity *v = br_component_add(e, nodes[i], id);
        BR_CHECK(v != NULL && v->damping == 0.0f); /* zeroed */
        v->damping = (float)i;
    }
    BR_CHECK(br_component_add(e, nodes[2], id) == br_component_get(e, nodes[2], id)); /* no duplicate */
    BR_CHECK(br_component_count(e, id) == 5);

    /* iterate like a system does */
    float sum = 0.0f;
    for (uint32_t i = 0; i < br_component_count(e, id); i++) {
        velocity *v = br_component_at(e, id, i);
        sum += v->damping;
        BR_CHECK(br_node_valid(e, br_component_owner_at(e, id, i)));
    }
    BR_CHECK(sum == 10.0f);

    /* remove from the middle: the last instance (node 4) moves into slot 1 */
    br_component_remove(e, nodes[1], id);
    BR_CHECK(br_component_count(e, id) == 4);
    BR_CHECK(br_component_get(e, nodes[1], id) == NULL);
    BR_CHECK(br_node_equal(br_component_owner_at(e, id, 1), nodes[4]));
    BR_CHECK(((velocity *)br_component_get(e, nodes[4], id))->damping == 4.0f);
    BR_CHECK(owners_consistent(e, id));

    /* remove the last one and the first one */
    br_component_remove(e, nodes[2], id);
    br_component_remove(e, nodes[0], id);
    BR_CHECK(br_component_count(e, id) == 2);
    BR_CHECK(owners_consistent(e, id));
    BR_CHECK(((velocity *)br_component_get(e, nodes[3], id))->damping == 3.0f);
    br_component_remove(e, nodes[0], id); /* not present: no-op */
    BR_CHECK(br_component_count(e, id) == 2);

    /* destroying a node removes its components (immediately outside a frame) */
    br_node_destroy(e, nodes[4]);
    BR_CHECK(br_component_count(e, id) == 1);
    BR_CHECK(owners_consistent(e, id));
    BR_CHECK(br_component_get(e, nodes[4], id) == NULL); /* stale handle */

    /* a new node reusing the slot does not inherit the component */
    br_node reused = br_node_create_container(e);
    BR_CHECK(reused.index == nodes[4].index);
    BR_CHECK(br_component_get(e, reused, id) == NULL);

    /* two component types on one node are independent */
    br_component_desc tag_desc = { "tag", sizeof(int), NULL, 0 };
    uint32_t tag = br_component_register(e, &tag_desc);
    *(int *)br_component_add(e, nodes[3], tag) = 5;
    BR_CHECK(br_component_get(e, nodes[3], id) != NULL);
    br_component_remove(e, nodes[3], id);
    BR_CHECK(*(int *)br_component_get(e, nodes[3], tag) == 5);
    BR_CHECK(br_component_count(e, id) == 0);

    br_engine_destroy(e);
    return br_test_finish();
}
