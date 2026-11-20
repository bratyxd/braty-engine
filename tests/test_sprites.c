/* test_sprites: nine-slice geometry, animated-sprite timing, rope/plane/perspective geometry.
 * Headless: texture records exist without a GPU, systems run through br_engine_step. */
#include "braty/braty.h"
#include "br_test.h"

static bool approx(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

/* Checks quad q of `v` covers x0..x1, y0..y1 (TL, TR, BR, BL order). */
static bool quad_is(const br_array *a, uint32_t q, float x0, float y0, float x1, float y1)
{
    const br_vertex2d *v = br_array_at(a, q * 4);
    return v != NULL && approx(v[0].x, x0) && approx(v[0].y, y0) && approx(v[2].x, x1) && approx(v[2].y, y1) &&
           approx(v[1].x, x1) && approx(v[1].y, y0) && approx(v[3].x, x0) && approx(v[3].y, y1);
}

static const br_vertex2d *quad_at(const br_array *a, uint32_t q)
{
    return br_array_at(a, q * 4);
}

static void test_nine_slice(br_engine *e)
{
    br_texture tex = br_texture_create_rgba8(e, 30, 30, NULL, NULL);
    br_node n = br_nine_slice_create(e, tex, 10, 10, 10, 10);
    br_nine_slice *ns = br_nine_slice_get(e, n);
    BR_CHECK(ns != NULL && approx(ns->width, 30) && approx(ns->height, 30));
    br_nine_slice_set_size(e, n, 100, 60);
    ns = br_nine_slice_get(e, n);
    br_texture_info info;
    br_texture_get_info(e, tex, &info);
    br_array out;
    br_array_init(&out, sizeof(br_vertex2d));

    /* stretch: 9 quads, row-major; corners keep 10x10, the centre stretches */
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 9);
    BR_CHECK(quad_is(&out, 0, 0, 0, 10, 10));
    BR_CHECK(quad_is(&out, 1, 10, 0, 90, 10));
    BR_CHECK(quad_is(&out, 2, 90, 0, 100, 10));
    BR_CHECK(quad_is(&out, 4, 10, 10, 90, 50));
    BR_CHECK(quad_is(&out, 8, 90, 50, 100, 60));
    const br_vertex2d *c = quad_at(&out, 4);
    BR_CHECK(approx(c[0].u, 1.0f / 3.0f) && approx(c[0].v, 1.0f / 3.0f) && approx(c[2].u, 2.0f / 3.0f));
    BR_CHECK(c[0].color == 0xFFFFFFFFu);

    /* hidden centre */
    br_array_clear(&out);
    ns->hide_center = true;
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 8);
    BR_CHECK(quad_is(&out, 4, 90, 10, 100, 50)); /* right edge follows the left edge directly */
    ns->hide_center = false;

    /* independent borders */
    br_array_clear(&out);
    br_nine_slice_set_borders(e, n, 4, 6, 8, 2);
    ns = br_nine_slice_get(e, n);
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 9);
    BR_CHECK(quad_is(&out, 0, 0, 0, 4, 6));
    BR_CHECK(quad_is(&out, 4, 4, 6, 92, 58));
    BR_CHECK(quad_is(&out, 8, 92, 58, 100, 60));
    br_nine_slice_set_borders(e, n, 10, 10, 10, 10);

    /* smaller than the borders: width 10 < 10 + 10, both borders shrink to 5; the centre column vanishes */
    br_array_clear(&out);
    ns->width = 10;
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 6);
    BR_CHECK(quad_is(&out, 0, 0, 0, 5, 10));
    BR_CHECK(quad_is(&out, 1, 5, 0, 10, 10));
    BR_CHECK(approx(quad_at(&out, 0)[1].u, 1.0f / 3.0f)); /* still the whole border of the source */
    /* uneven borders shrink proportionally */
    br_array_clear(&out);
    ns->left = 15;
    ns->right = 5;
    ns->width = 10;
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 6);
    BR_CHECK(quad_is(&out, 0, 0, 0, 7.5f, 10)); /* 15 + 5 into 10 -> 7.5 + 2.5 */
    BR_CHECK(quad_is(&out, 1, 7.5f, 0, 10, 10));
    ns->left = 10;
    ns->right = 10;

    /* tiled edges: top edge of 25 units, source tile 10 -> 10 + 10 + 5 */
    br_array_clear(&out);
    ns->width = 45;
    ns->height = 30;
    ns->edge_fill = BR_NINE_SLICE_TILE;
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 13);
    BR_CHECK(quad_is(&out, 1, 10, 0, 20, 10));
    BR_CHECK(quad_is(&out, 3, 30, 0, 35, 10));
    BR_CHECK(approx(quad_at(&out, 3)[0].u, 1.0f / 3.0f) && approx(quad_at(&out, 3)[1].u, 0.5f));
    /* tiled centre too: 25 x 10 -> 3 tiles */
    br_array_clear(&out);
    ns->center_fill = BR_NINE_SLICE_TILE;
    BR_CHECK(br_nine_slice_geometry(ns, &info, &out) == 15);
    ns->edge_fill = BR_NINE_SLICE_STRETCH;
    ns->center_fill = BR_NINE_SLICE_STRETCH;

    /* anchor */
    br_array_clear(&out);
    ns->width = 100;
    ns->height = 60;
    ns->anchor = br_vec2_make(0.5f, 0.5f);
    br_nine_slice_geometry(ns, &info, &out);
    BR_CHECK(quad_is(&out, 0, -50, -30, -40, -20));
    BR_CHECK(br_rect_contains(br_node_get_local_bounds(e, n), -49, -29));
    ns->anchor = br_vec2_make(0, 0);

    /* trimmed atlas frame: 20x20 stored, 30x30 original, trimmed 5 px on the top-left */
    br_texture atlas = br_texture_create_rgba8(e, 40, 40, NULL, NULL);
    br_texture_frame frame;
    SDL_zero(frame);
    frame.frame = br_rect_make(0, 0, 20, 20);
    frame.orig = br_rect_make(0, 0, 30, 30);
    frame.trim = br_rect_make(5, 5, 20, 20);
    br_texture trimmed = br_texture_create_frame(e, atlas, &frame);
    br_texture_get_info(e, trimmed, &info);
    br_nine_slice tns = *ns;
    tns.texture = trimmed;
    tns.width = 30;
    tns.height = 30;
    br_array_clear(&out);
    BR_CHECK(br_nine_slice_geometry(&tns, &info, &out) == 9);
    BR_CHECK(quad_is(&out, 0, 5, 5, 10, 10));               /* transparent padding has no geometry */
    BR_CHECK(approx(quad_at(&out, 0)[0].u, 0.0f) && approx(quad_at(&out, 0)[1].u, 0.125f));
    BR_CHECK(quad_is(&out, 8, 20, 20, 25, 25));

    /* rotated atlas frame: logical 20x10, stored 10 wide x 20 tall */
    SDL_zero(frame);
    frame.frame = br_rect_make(0, 0, 20, 10);
    frame.rotated = true;
    br_texture rotated = br_texture_create_frame(e, atlas, &frame);
    br_texture_get_info(e, rotated, &info);
    tns.texture = rotated;
    tns.left = tns.top = tns.right = tns.bottom = 0;
    tns.width = 20;
    tns.height = 10;
    br_array_clear(&out);
    BR_CHECK(br_nine_slice_geometry(&tns, &info, &out) == 1);
    const br_vertex2d *r = quad_at(&out, 0);
    BR_CHECK(approx(r[0].u, 0.25f) && approx(r[0].v, 0.0f));   /* logical top-left = stored top-right */
    BR_CHECK(approx(r[1].u, 0.25f) && approx(r[1].v, 0.5f));
    BR_CHECK(approx(r[3].u, 0.0f) && approx(r[3].v, 0.0f));

    br_array_free(&out);
}

typedef struct anim_counts {
    int changes;
    int loops;
    int completes;
} anim_counts;

static void on_change(br_engine *e, br_node n, void *user)
{
    (void)e;
    (void)n;
    ((anim_counts *)user)->changes += 1;
}

static void on_loop(br_engine *e, br_node n, void *user)
{
    (void)e;
    (void)n;
    ((anim_counts *)user)->loops += 1;
}

static void on_complete(br_engine *e, br_node n, void *user)
{
    (void)e;
    (void)n;
    ((anim_counts *)user)->completes += 1;
}

static void test_animation(br_engine *e)
{
    br_texture t[3];
    for (int i = 0; i < 3; i++) {
        t[i] = br_texture_create_rgba8(e, 8, 8, NULL, NULL);
    }
    br_anim_frame frames[3] = { { t[0], 0.1f }, { t[1], 0.2f }, { t[2], 0.1f } };
    br_node s = br_animated_sprite_create(e, frames, 3);
    br_node_add_child(e, br_stage_root(e), s);
    anim_counts counts = { 0, 0, 0 };
    br_animated_sprite_set_callbacks(e, s, on_complete, on_change, on_loop, &counts);
    BR_CHECK(br_animated_sprite_is(e, s) && br_animated_sprite_total_frames(e, s) == 3);
    BR_CHECK(br_node_type_of(e, s) == br_sprite_type(e)); /* an ordinary sprite */

    /* stopped by default */
    br_engine_step(e, 0.05f);
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 0 && !br_animated_sprite_playing(e, s));

    /* per-frame durations: 0.1, 0.2, 0.1 */
    br_animated_sprite_play(e, s);
    br_engine_step(e, 0.05f); /* 0.05 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 0);
    br_engine_step(e, 0.06f); /* 0.11 -> frame 1 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 1);
    BR_CHECK(br_sprite_texture(e, s).index == t[1].index);
    br_engine_step(e, 0.09f); /* 0.10 into frame 1 (lasts 0.2) */
    br_engine_step(e, 0.09f); /* 0.19 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 1);
    br_engine_step(e, 0.02f); /* 0.21 -> frame 2 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 2);
    br_engine_step(e, 0.1f);  /* past the end: loop to frame 0 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 0);
    BR_CHECK(counts.changes == 3 && counts.loops == 1 && counts.completes == 0);

    /* one big step crosses several frames, fires one change */
    br_animated_sprite_goto_and_play(e, s, 0);
    counts.changes = 0;
    br_engine_step(e, 0.095f);
    br_engine_step(e, 0.095f); /* 0.19 -> frame 1 */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 1 && counts.changes == 1);

    /* speed */
    br_animated_sprite_goto_and_play(e, s, 0);
    br_animated_sprite_set_speed(e, s, 2.0f);
    br_engine_step(e, 0.06f); /* 0.12 of animation time */
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 1);

    /* backwards with loop: from frame 0 back to the last */
    br_animated_sprite_goto_and_play(e, s, 0);
    br_animated_sprite_set_speed(e, s, -1.0f);
    counts.loops = 0;
    br_engine_step(e, 0.01f);
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 2 && counts.loops == 1);

    /* no loop: stops on the last frame and completes once */
    br_animated_sprite_set_speed(e, s, 1.0f);
    br_animated_sprite_set_loop(e, s, false);
    br_animated_sprite_goto_and_play(e, s, 2);
    br_engine_step(e, 0.05f);
    br_engine_step(e, 0.06f);
    BR_CHECK(br_animated_sprite_current_frame(e, s) == 2 && !br_animated_sprite_playing(e, s));
    BR_CHECK(counts.completes == 1);
    br_engine_step(e, 0.1f);
    BR_CHECK(counts.completes == 1);

    /* goto_and_stop shows the frame */
    br_animated_sprite_goto_and_stop(e, s, 1);
    BR_CHECK(br_sprite_texture(e, s).index == t[1].index && !br_animated_sprite_playing(e, s));

    /* uniform fps + update anchor */
    br_texture_frame f;
    SDL_zero(f);
    f.frame = br_rect_make(0, 0, 4, 4);
    f.default_anchor = br_vec2_make(0.5f, 1.0f);
    br_texture anchored = br_texture_create_frame(e, t[0], &f);
    br_texture two[2] = { t[0], anchored };
    br_node u = br_animated_sprite_create_uniform(e, two, 2, 10.0f);
    br_node_add_child(e, br_stage_root(e), u);
    br_animated_sprite_set_update_anchor(e, u, true);
    br_animated_sprite_play(e, u);
    br_engine_step(e, 0.099f);
    BR_CHECK(br_animated_sprite_current_frame(e, u) == 0);
    br_engine_step(e, 0.002f);
    BR_CHECK(br_animated_sprite_current_frame(e, u) == 1);
    BR_CHECK(approx(br_sprite_anchor(e, u).y, 1.0f));

    /* destroying animated sprites frees their frames (no crash, no stale state) */
    br_node_destroy(e, s);
    br_node_destroy(e, u);
    br_engine_step(e, 0.1f);
    BR_CHECK(!br_animated_sprite_is(e, s));
}

static void test_rope(void)
{
    br_vec2 points[3] = { { 0, 0 }, { 10, 0 }, { 20, 0 } };
    br_vec2 pos[6];
    br_vec2 uvs[6];
    uint32_t idx[12];
    br_mesh_build_rope(points, 3, 4.0f, 0.0f, 5.0f, pos, uvs, idx);
    BR_CHECK(approx(pos[0].x, 0) && approx(pos[0].y, -2) && approx(pos[1].y, 2));
    BR_CHECK(approx(pos[4].x, 20) && approx(pos[4].y, -2) && approx(pos[5].y, 2));
    BR_CHECK(approx(uvs[0].x, 0) && approx(uvs[2].x, 0.5f) && approx(uvs[4].x, 1.0f));
    BR_CHECK(approx(uvs[0].y, 0) && approx(uvs[1].y, 1));
    BR_CHECK(idx[0] == 0 && idx[1] == 2 && idx[2] == 3 && idx[3] == 0 && idx[4] == 3 && idx[5] == 1);
    BR_CHECK(idx[6] == 2 && idx[11] == 3);
    /* repeating texture: u = length / (texture width * scale) */
    br_mesh_build_rope(points, 3, 4.0f, 2.0f, 5.0f, pos, uvs, idx);
    BR_CHECK(approx(uvs[2].x, 1.0f) && approx(uvs[4].x, 2.0f));
    /* a corner: the middle normal is perpendicular to neighbour-to-neighbour */
    br_vec2 bend[3] = { { 0, 0 }, { 10, 0 }, { 10, 10 } };
    br_mesh_build_rope(bend, 3, 2.0f, 0.0f, 1.0f, pos, uvs, idx);
    float s = SDL_sqrtf(0.5f);
    BR_CHECK(approx(pos[2].x, 10 + s) && approx(pos[2].y, -s));
    BR_CHECK(approx(pos[4].x, 11) && approx(pos[4].y, 10));
}

static void test_meshes(br_engine *e)
{
    br_vec2 pos[6];
    br_vec2 uvs[6];
    uint32_t idx[12];
    br_mesh_build_plane(20, 10, 3, 2, pos, uvs, idx);
    BR_CHECK(approx(pos[2].x, 20) && approx(pos[5].x, 20) && approx(pos[5].y, 10) && approx(uvs[4].x, 0.5f));
    BR_CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 4 && idx[6] == 1);

    br_vec2 square[4] = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
    br_vec2 p = br_mesh_perspective_point(square, 0.5f, 0.25f);
    BR_CHECK(approx(p.x, 5) && approx(p.y, 2.5f));
    br_vec2 trap[4] = { { 2, 0 }, { 8, 0 }, { 10, 10 }, { 0, 10 } };
    BR_CHECK(approx(br_mesh_perspective_point(trap, 1, 1).x, 10) && approx(br_mesh_perspective_point(trap, 1, 0).x, 8));
    /* projective, not bilinear: the narrow (far) half is compressed, the middle moves up */
    BR_CHECK(approx(br_mesh_perspective_point(trap, 0.5f, 0.5f).y, 3.75f));

    br_texture tex = br_texture_create_rgba8(e, 16, 4, NULL, NULL);
    br_vec2 rp[3] = { { 0, 0 }, { 10, 0 }, { 20, 0 } };
    br_node rope = br_mesh_rope_create(e, tex, rp, 3, 0.0f);
    br_node_add_child(e, br_stage_root(e), rope);
    BR_CHECK(br_node_type_hit_test(e, rope, br_vec2_make(15, 1)));
    BR_CHECK(!br_node_type_hit_test(e, rope, br_vec2_make(15, 3)));
    uint32_t count = 0;
    br_vec2 *points = br_mesh_rope_points(e, rope, &count);
    BR_CHECK(points != NULL && count == 3);
    points[2].y = 50; /* the strip follows next frame */
    br_engine_step(e, 0.016f);
    BR_CHECK(br_node_type_hit_test(e, rope, br_vec2_make(19.5f, 48)));

    uint32_t tri[3] = { 0, 1, 2 };
    br_vec2 tp[3] = { { 0, 0 }, { 10, 0 }, { 0, 10 } };
    br_node m = br_mesh_create(e, tex, tp, tp, NULL, 3, tri, 3);
    BR_CHECK(br_node_type_hit_test(e, m, br_vec2_make(2, 2)) && !br_node_type_hit_test(e, m, br_vec2_make(8, 8)));
    uint32_t bad[3] = { 0, 1, 7 };
    BR_CHECK(!br_mesh_set_geometry(e, m, tp, tp, NULL, 3, bad, 3));
    br_node_destroy(e, m);
}

static void test_particles(br_engine *e)
{
    br_texture tex = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    br_node pc = br_particle_container_create(e, tex, BR_PARTICLE_POSITION);
    br_particle *p = br_particle_container_add(e, pc, 10);
    BR_CHECK(p != NULL && approx(p[3].scale_x, 1) && approx(p[3].alpha, 1) && p[3].tint == 0xFFFFFFu);
    p[5].x = 5;
    br_particle_container_remove(e, pc, 0, 4);
    uint32_t count = 0;
    p = br_particle_container_particles(e, pc, &count);
    BR_CHECK(count == 6 && approx(p[1].x, 5));
    br_particle_container_clear(e, pc);
    br_particle_container_particles(e, pc, &count);
    BR_CHECK(count == 0);
    br_node_destroy(e, pc);
}

int main(void)
{
    br_engine *e = br_engine_create("test_sprites", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    test_nine_slice(e);
    test_animation(e);
    test_rope();
    test_meshes(e);
    test_particles(e);
    br_engine_destroy(e);
    return br_test_finish();
}
