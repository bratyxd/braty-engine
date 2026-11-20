/* test_transform - world matrices vs hand-computed values, pivot/skew/origin, to_global/to_local,
 * render groups, reparent, bounds (headless). */
#include "braty/braty.h"
#include "br_test.h"

static bool near_vec(br_vec2 a, float x, float y)
{
    return br_nearly_equal(a.x, x, 1e-3f) && br_nearly_equal(a.y, y, 1e-3f);
}

int main(void)
{
    br_engine *e = br_engine_create("test_transform", NULL);
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    br_node root = br_stage_root(e);
    br_node parent = br_node_create_container(e);
    br_node child = br_node_create_container(e);
    br_node_add_child(e, root, parent);
    br_node_add_child(e, parent, child);

    /* parent: translate (100, 50), scale 2; child: translate (10, 0), rotate 90 degrees clockwise */
    br_node_set_position(e, parent, br_vec2_make(100.0f, 50.0f));
    br_node_set_scale(e, parent, br_vec2_make(2.0f, 2.0f));
    br_node_set_position(e, child, br_vec2_make(10.0f, 0.0f));
    br_node_set_rotation(e, child, BR_PI * 0.5f);
    /* child local (1, 0) -> rotated (0, 1) -> +(10,0) = (10, 1) -> *2 = (20, 2) -> +(100,50) */
    BR_CHECK(near_vec(br_node_to_global(e, child, br_vec2_make(1.0f, 0.0f)), 120.0f, 52.0f));
    BR_CHECK(near_vec(br_node_to_local(e, child, br_vec2_make(120.0f, 52.0f)), 1.0f, 0.0f));

    /* the transform system's cached world matrix agrees after a frame */
    br_engine_step(e, 1.0f / 60.0f);
    br_mat3 world = br_node_world_transform(e, child);
    br_mat3 expected = br_mat3_mul(br_mat3_mul(br_mat3_translation(100.0f, 50.0f), br_mat3_scaling(2.0f, 2.0f)),
                                   br_mat3_mul(br_mat3_translation(10.0f, 0.0f), br_mat3_rotation(BR_PI * 0.5f)));
    BR_CHECK(br_mat3_nearly_equal(world, expected, 1e-4f));

    /* pivot: the pivot point lands on the position */
    br_node p = br_node_create_container(e);
    br_node_set_position(e, p, br_vec2_make(30.0f, 40.0f));
    br_node_set_pivot(e, p, br_vec2_make(5.0f, 5.0f));
    br_node_set_rotation(e, p, 1.0f);
    BR_CHECK(near_vec(br_node_to_global(e, p, br_vec2_make(5.0f, 5.0f)), 30.0f, 40.0f));

    /* origin: rotation/scale centre that does not move the node: origin maps to position + origin */
    br_node o = br_node_create_container(e);
    br_node_set_position(e, o, br_vec2_make(30.0f, 40.0f));
    br_node_set_origin(e, o, br_vec2_make(5.0f, 5.0f));
    br_node_set_scale(e, o, br_vec2_make(3.0f, 3.0f));
    br_node_set_rotation(e, o, 0.7f);
    BR_CHECK(near_vec(br_node_to_global(e, o, br_vec2_make(5.0f, 5.0f)), 35.0f, 45.0f));
    /* (6,5) is 1 unit right of the origin: scaled 3 and rotated 0.7 around it */
    BR_CHECK(near_vec(br_node_to_global(e, o, br_vec2_make(6.0f, 5.0f)), 35.0f + 3.0f * SDL_cosf(0.7f),
                      45.0f + 3.0f * SDL_sinf(0.7f)));

    /* skew x: a = cos(skew_y) sx, c = -sin(-skew_x) sy -> (0,1) maps to (sin(skx), cos(skx)) */
    br_node k = br_node_create_container(e);
    br_node_set_skew(e, k, br_vec2_make(0.5f, 0.0f));
    BR_CHECK(near_vec(br_node_to_global(e, k, br_vec2_make(0.0f, 1.0f)), SDL_sinf(0.5f), SDL_cosf(0.5f)));
    BR_CHECK(near_vec(br_node_to_global(e, k, br_vec2_make(1.0f, 0.0f)), 1.0f, 0.0f));

    /* reparent keeps the world position */
    br_vec2 before = br_node_to_global(e, child, br_vec2_make(3.0f, 4.0f));
    BR_CHECK(br_node_reparent(e, child, root));
    BR_CHECK(br_node_equal(br_node_parent(e, child), root));
    br_vec2 after = br_node_to_global(e, child, br_vec2_make(3.0f, 4.0f));
    BR_CHECK(near_vec(after, before.x, before.y));
    br_node_reparent(e, child, parent);

    /* render group: cached world of children is relative to the group, global queries are not */
    br_node group = br_node_create_container(e);
    br_node member = br_node_create_container(e);
    br_node_add_child(e, root, group);
    br_node_add_child(e, group, member);
    br_node_set_render_group(e, group, true);
    br_node_set_position(e, group, br_vec2_make(1000.0f, 0.0f));
    br_node_set_position(e, member, br_vec2_make(7.0f, 8.0f));
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(near_vec(br_node_to_global(e, member, br_vec2_make(0.0f, 0.0f)), 1007.0f, 8.0f));

    /* alpha and tint multiply down the hierarchy */
    br_node_set_alpha(e, parent, 0.5f);
    br_node_set_alpha(e, child, 0.5f);
    br_node_set_tint(e, parent, br_color_make(1.0f, 0.5f, 1.0f, 1.0f));
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_nearly_equal(br_node_world_alpha(e, child), 0.25f, 1e-5f));
    BR_CHECK(br_nearly_equal(br_node_world_tint(e, child).g, 0.5f, 1e-5f));

    /* bounds: a 10x20 sprite anchored at its centre, scaled 2, under a moved parent */
    uint8_t pixels[10 * 20 * 4];
    SDL_memset(pixels, 255, sizeof(pixels));
    br_texture tex = br_texture_create_rgba8(e, 10, 20, pixels, NULL);
    BR_CHECK(br_texture_valid(e, tex));
    br_node s = br_sprite_create(e, tex);
    br_sprite_set_anchor(e, s, 0.5f, 0.5f);
    br_node holder = br_node_create_container(e);
    br_node_set_position(e, holder, br_vec2_make(100.0f, 100.0f));
    br_node_add_child(e, holder, s);
    br_node_set_scale(e, s, br_vec2_make(2.0f, 2.0f));
    br_rect b = br_node_get_bounds(e, holder);
    BR_CHECK(br_nearly_equal(b.x, 90.0f, 1e-4f) && br_nearly_equal(b.y, 80.0f, 1e-4f));
    BR_CHECK(br_nearly_equal(b.width, 20.0f, 1e-4f) && br_nearly_equal(b.height, 40.0f, 1e-4f));
    br_rect lb = br_node_get_local_bounds(e, holder);
    BR_CHECK(br_nearly_equal(lb.x, -10.0f, 1e-4f) && br_nearly_equal(lb.width, 20.0f, 1e-4f));
    br_sprite_set_size(e, s, 50.0f, 10.0f);
    BR_CHECK(near_vec(br_sprite_size(e, s), 50.0f, 10.0f));
    BR_CHECK(br_node_type_hit_test(e, s, br_vec2_make(0.0f, 0.0f)));
    BR_CHECK(!br_node_type_hit_test(e, s, br_vec2_make(6.0f, 0.0f)));

    /* missing texture: logs, returns the drawable fallback */
    br_texture missing = br_texture_load(e, "does/not/exist.png");
    BR_CHECK(missing.index == br_texture_missing(e).index && missing.generation == br_texture_missing(e).generation);
    br_texture_info info;
    BR_CHECK(br_texture_get_info(e, missing, &info) && info.width == 2.0f);

    br_engine_destroy(e);
    return br_test_finish();
}
