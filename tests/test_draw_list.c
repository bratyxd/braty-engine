/* test_draw_list: the recorded draw lists, headless (br_render_debug_internal.h). 2D batching
 * (texture-slot limit, blend breaks, masks, render layers, culling, draw order) and the 3D
 * collect (frustum culling, sort order, light selection, shadow tiles, instancing). */
#include "braty/braty.h"
#include "render/br_render_debug_internal.h"
#include "br_test.h"

#define SCREEN_W 800
#define SCREEN_H 600

static void step(br_engine *e)
{
    br_engine_step(e, 1.0f / 60.0f);
}

/* Removes every child of the stage (each test starts from an empty stage). */
static void clear_stage(br_engine *e)
{
    br_node root = br_stage_root(e);
    while (br_node_child_count(e, root) > 0) {
        br_node_destroy(e, br_node_child_at(e, root, 0));
    }
    step(e); /* destroyed nodes are released at the frame's flush */
}

static br_node sprite_at(br_engine *e, br_node parent, br_texture t, float x, float y)
{
    br_node s = br_sprite_create(e, t);
    br_node_set_position(e, s, br_vec2_make(x, y));
    br_node_add_child(e, parent, s);
    return s;
}

/* The batch commands of this frame, in order; the unused slots point at an empty command so a
 * wrong count fails checks instead of crashing. */
static uint32_t batches(br_engine *e, const br_draw_cmd **out, uint32_t max)
{
    static const br_draw_cmd empty;
    uint32_t count = 0;
    const br_draw_cmd *cmds = br_render_debug_cmds(e, &count);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cmds[i].kind == BR_DRAW_BATCH && n < max) {
            out[n++] = &cmds[i];
        }
    }
    for (uint32_t i = n; i < max; i++) {
        out[i] = &empty;
    }
    return n;
}

/* x of the top-left vertex of every quad drawn this frame, in draw order. */
static uint32_t quad_xs(br_engine *e, float *xs, uint32_t max)
{
    const br_draw_cmd *b[64];
    uint32_t nb = batches(e, b, 64);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nb; i++) {
        const br_batch_vertex *v = br_render_debug_batch_vertices(e, b[i]);
        const uint16_t *idx = br_render_debug_batch_indices(e, b[i]);
        for (uint32_t k = 0; k + 5 < b[i]->index_count && n < max; k += 6) {
            xs[n++] = v[idx[k]].x; /* quads: indices base, base+1, base+2, base, base+2, base+3 */
        }
    }
    return n;
}

static void test_texture_limit(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_texture tex[BR_RENDER_MAX_TEXTURES + 1];
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(tex); i++) {
        tex[i] = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
        sprite_at(e, root, tex[i], 10.0f * (float)i, 0.0f);
    }
    sprite_at(e, root, tex[0], 200.0f, 0.0f); /* a texture already in the open batch: no break */
    step(e);
    const br_draw_cmd *b[8];
    BR_CHECK(batches(e, b, 8) == 2);
    BR_CHECK(b[0]->texture_count == BR_RENDER_MAX_TEXTURES);
    BR_CHECK(b[0]->index_count == BR_RENDER_MAX_TEXTURES * 6);
    BR_CHECK(b[1]->texture_count == 2); /* the 9th texture, then tex[0] again in the new batch */
    BR_CHECK(b[1]->index_count == 12);
    const br_batch_vertex *v = br_render_debug_batch_vertices(e, b[1]);
    BR_CHECK(v != NULL && v[0].texture == 0 && v[4].texture == 1); /* slot per vertex */
    BR_CHECK(b[1]->textures[0].index == tex[BR_RENDER_MAX_TEXTURES].index);
    BR_CHECK(b[1]->textures[1].index == tex[0].index);
    clear_stage(e);
}

static void test_blend_breaks(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_texture t = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    sprite_at(e, root, t, 0.0f, 0.0f);
    sprite_at(e, root, t, 10.0f, 0.0f);
    br_node add = sprite_at(e, root, t, 20.0f, 0.0f);
    br_node_set_blend_mode(e, add, BR_BLEND_ADD);
    sprite_at(e, root, t, 30.0f, 0.0f);
    step(e);
    const br_draw_cmd *b[8];
    BR_CHECK(batches(e, b, 8) == 3);
    BR_CHECK(b[0]->state.blend == BR_BLEND_NORMAL && b[0]->index_count == 12);
    BR_CHECK(b[1]->state.blend == BR_BLEND_ADD && b[1]->index_count == 6);
    BR_CHECK(b[2]->state.blend == BR_BLEND_NORMAL && b[2]->index_count == 6);
    br_render_stats stats;
    br_render_get_stats(e, &stats);
    BR_CHECK(stats.batches == 3);
    clear_stage(e);
}

static void test_masks(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_texture t = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    /* scissor: the mask's world box (target pixels) clips the masked subtree's batch */
    br_node clip = sprite_at(e, root, t, 100.0f, 50.0f);
    br_node_set_scale(e, clip, br_vec2_make(10.0f, 5.0f)); /* 40 x 20 */
    br_node masked = sprite_at(e, root, t, 0.0f, 0.0f);
    br_node_set_mask(e, masked, clip, BR_MASK_SCISSOR, false);
    br_node after = sprite_at(e, root, t, 300.0f, 0.0f);
    (void)after;
    step(e);
    const br_draw_cmd *b[8];
    BR_CHECK(batches(e, b, 8) == 2); /* the mask node itself is not drawn as content */
    BR_CHECK(b[0]->state.scissor_on);
    BR_CHECK(b[0]->state.scissor.x == 100 && b[0]->state.scissor.y == 50);
    BR_CHECK(b[0]->state.scissor.w == 40 && b[0]->state.scissor.h == 20);
    BR_CHECK(!b[1]->state.scissor_on);
    clear_stage(e);

    /* stencil: write the mask (+1), draw tested, erase the mask (-1) */
    br_node shape = sprite_at(e, root, t, 0.0f, 0.0f);
    masked = sprite_at(e, root, t, 0.0f, 0.0f);
    br_node_set_mask(e, masked, shape, BR_MASK_STENCIL, false);
    step(e);
    uint32_t n = batches(e, b, 8);
    BR_CHECK(n == 3);
    if (n == 3) {
        BR_CHECK(b[0]->state.stencil == BR_STENCIL_INCR && b[0]->state.stencil_ref == 0);
        BR_CHECK(b[1]->state.stencil == BR_STENCIL_TEST && b[1]->state.stencil_ref == 1);
        BR_CHECK(b[2]->state.stencil == BR_STENCIL_DECR && b[2]->state.stencil_ref == 1);
    }
    clear_stage(e);
}

static void test_layers_and_order(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_texture t = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    /* hierarchy order: a(0), b(10), c(20); z_index on a sortable parent puts c first */
    br_node group = br_node_create_container(e);
    br_node_add_child(e, root, group);
    br_node a = sprite_at(e, group, t, 0.0f, 0.0f);
    sprite_at(e, group, t, 10.0f, 0.0f);
    br_node c = sprite_at(e, group, t, 20.0f, 0.0f);
    step(e);
    float xs[16];
    BR_CHECK(quad_xs(e, xs, 16) == 3 && xs[0] == 0.0f && xs[1] == 10.0f && xs[2] == 20.0f);
    br_node_set_sortable_children(e, group, true);
    br_node_set_z_index(e, c, -1);
    step(e);
    BR_CHECK(quad_xs(e, xs, 16) == 3 && xs[0] == 20.0f && xs[1] == 0.0f && xs[2] == 10.0f);

    /* a render layer after the group draws its attached node last, wherever the node sits
     * (the sort reordered the children to c, a, b; equal z keeps that order) */
    br_node layer = br_node_create_container(e);
    br_node_add_child(e, root, layer);
    br_node_set_z_index(e, c, 0);
    BR_CHECK(br_node_layer_attach(e, layer, a));
    step(e);
    BR_CHECK(quad_xs(e, xs, 16) == 3 && xs[0] == 20.0f && xs[1] == 10.0f && xs[2] == 0.0f);
    clear_stage(e);
}

static void test_culling(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_texture t = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    br_node on = sprite_at(e, root, t, 10.0f, 10.0f);
    br_node off = sprite_at(e, root, t, (float)SCREEN_W + 50.0f, 10.0f);
    br_node off_kept = sprite_at(e, root, t, -100.0f, 10.0f); /* not cullable: still drawn */
    br_node_set_cullable(e, on, true);
    br_node_set_cullable(e, off, true);
    (void)off_kept;
    step(e);
    float xs[16];
    uint32_t n = quad_xs(e, xs, 16);
    BR_CHECK(n == 2 && xs[0] == 10.0f && xs[1] == -100.0f);
    BR_CHECK(br_node_is_culled(e, off) && !br_node_is_culled(e, on));
    clear_stage(e);
}

/* ---- 3D */

static br_material3d material(br_engine *e, bool transparent)
{
    br_material3d_desc d = br_material3d_default(BR_MATERIAL3D_PBR);
    if (transparent) {
        d.alpha_mode = BR_ALPHA_BLEND;
    }
    return br_material3d_create(e, &d);
}

static br_node3d mesh_at(br_engine *e, br_node view, br_geometry3d g, br_material3d m, br_vec3 p)
{
    br_node3d n = br_mesh3d_create(e, g, m);
    br_node3d_set_position(e, n, p);
    br_node3d_add_child(e, br_view3d_root(e, view), n);
    return n;
}

static br_node view_on_stage(br_engine *e)
{
    br_node view = br_view3d_create(e, 200.0f, 100.0f);
    br_node_add_child(e, br_stage_root(e), view);
    return view; /* default camera: at (0, 2, 8) looking at the origin */
}

static float item_z(br_engine *e, const br_item3d *item)
{
    const br_instance3d *inst = br_render_debug_instances3d(e, item);
    return inst != NULL ? inst[0].model.m[14] : 0.0f;
}

static void test_frustum_and_sort(br_engine *e)
{
    br_node view = view_on_stage(e);
    br_geometry3d cube = br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f));
    br_material3d opaque = material(e, false);
    br_material3d glass = material(e, true);
    /* collect order deliberately mixed: far, near, middle */
    mesh_at(e, view, cube, opaque, br_vec3_make(0.0f, 0.0f, -30.0f));
    mesh_at(e, view, cube, opaque, br_vec3_make(0.0f, 0.0f, -5.0f));
    mesh_at(e, view, cube, opaque, br_vec3_make(0.0f, 0.0f, -15.0f));
    mesh_at(e, view, cube, glass, br_vec3_make(0.0f, 0.0f, -6.0f));
    mesh_at(e, view, cube, glass, br_vec3_make(0.0f, 0.0f, -20.0f));
    br_node3d behind = mesh_at(e, view, cube, opaque, br_vec3_make(0.0f, 0.0f, 10.0f));
    br_node3d beside = mesh_at(e, view, cube, opaque, br_vec3_make(500.0f, 0.0f, -5.0f));
    br_mesh3d_get(e, beside)->cast_shadows = false;
    step(e);
    const br_view3d_state *v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL);
    if (v == NULL) {
        return;
    }
    const br_item3d *items = v->items.data;
    /* culled meshes are not in the camera passes; `behind` still casts shadows, `beside` is gone */
    BR_CHECK(v->items.count == 6);
    BR_CHECK(v->opaque.count == 3 && v->transparent.count == 2);
    BR_CHECK(v->shadow.count == 6);
    bool behind_shadow_only = false;
    for (uint32_t i = 0; i < v->items.count; i++) {
        const br_instance3d *inst = br_render_debug_instances3d(e, &items[i]);
        if (inst != NULL && inst[0].model.m[14] == 10.0f) {
            behind_shadow_only = items[i].passes == BR_PASS3D_SHADOW;
        }
    }
    BR_CHECK(behind_shadow_only);
    (void)behind;
    /* opaque near -> far, transparent far -> near */
    const uint32_t *o = v->opaque.data;
    const uint32_t *t = v->transparent.data;
    BR_CHECK(items[o[0]].depth < items[o[1]].depth && items[o[1]].depth < items[o[2]].depth);
    /* identify the items by their z (camera space depth is not plain -z: the default camera is
     * placed above and behind the origin) */
    BR_CHECK(item_z(e, &items[o[0]]) == -5.0f && item_z(e, &items[o[1]]) == -15.0f && item_z(e, &items[o[2]]) == -30.0f);
    BR_CHECK(item_z(e, &items[t[0]]) == -20.0f && item_z(e, &items[t[1]]) == -6.0f);
    BR_CHECK(items[t[0]].depth > items[t[1]].depth);
    BR_CHECK(v->stats.items_opaque == 3 && v->stats.items_transparent == 2);
    br_node_destroy(e, view);
    step(e);
}

/* Occlusion culling: items hidden behind opaque meshes leave the camera passes, nothing else. */
static void test_occlusion(br_engine *e)
{
    br_node view = view_on_stage(e); /* camera at (0, 2, 8) looking at the origin */
    br_geometry3d cube = br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f));
    br_geometry3d wall = br_geometry3d_cube(e, br_vec3_make(40.0f, 40.0f, 1.0f));
    br_material3d opaque = material(e, false);
    br_material3d glass = material(e, true);
    BR_CHECK(br_view3d_occlusion_culling(e, view) == BR_VIEW3D_OCCLUSION_TRIANGLES); /* on by default */
    mesh_at(e, view, wall, opaque, br_vec3_make(0.0f, 0.0f, -10.0f));
    for (int i = 0; i < 8; i++) {
        mesh_at(e, view, cube, opaque, br_vec3_make(-3.5f + (float)i, 0.0f, -30.0f)); /* behind the wall */
    }
    mesh_at(e, view, cube, opaque, br_vec3_make(0.0f, 0.0f, -4.0f));  /* in front of it */
    mesh_at(e, view, cube, glass, br_vec3_make(0.0f, 0.0f, -31.0f));  /* transparent, behind */
    mesh_at(e, view, cube, glass, br_vec3_make(0.0f, 0.0f, -3.0f));   /* transparent, in front */
    step(e);
    const br_view3d_state *v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL);
    if (v == NULL) {
        return;
    }
    BR_CHECK(v->items.count == 12);
    BR_CHECK(v->opaque.count == 2 && v->transparent.count == 1); /* wall + front cube, front glass */
    BR_CHECK(v->stats.items_occluded == 9 && v->stats.items_opaque == 2 && v->stats.items_transparent == 1);
    BR_CHECK(v->shadow.count == 12); /* hidden from the camera, still casting */
    const br_item3d *items = v->items.data;
    const uint32_t *o = v->opaque.data;
    BR_CHECK(item_z(e, &items[o[0]]) == -4.0f && item_z(e, &items[o[1]]) == -10.0f);

    /* off: everything in the frustum is drawn again */
    br_view3d_set_occlusion_culling(e, view, 0);
    step(e);
    v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL && v->opaque.count == 10 && v->transparent.count == 2 && v->stats.items_occluded == 0);

    /* a budget too small for the wall's 12 triangles: no occluder, nothing hidden */
    br_view3d_set_occlusion_culling(e, view, 11);
    step(e);
    v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL && v->opaque.count == 10 && v->stats.items_occluded == 0);
    br_node_destroy(e, view);
    step(e);

    /* a floor seen from above hides what is under it; its two triangles reach behind the camera,
     * so this also checks the cut at the camera plane and the front-face winding */
    view = view_on_stage(e);
    br_geometry3d floor = br_geometry3d_plane(e, 400.0f, 400.0f, 1, 1);
    mesh_at(e, view, floor, opaque, br_vec3_make(0.0f, -1.0f, 0.0f));
    for (int i = 0; i < 8; i++) {
        mesh_at(e, view, cube, opaque, br_vec3_make(-3.5f + (float)i, -10.0f, -10.0f));
    }
    step(e);
    v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL && v->opaque.count == 1 && v->stats.items_occluded == 8);
    br_node_destroy(e, view);
    step(e);

    /* the same plane seen from below is back-face culled when drawn, so it hides nothing */
    view = view_on_stage(e);
    mesh_at(e, view, floor, opaque, br_vec3_make(0.0f, 3.5f, 0.0f));
    for (int i = 0; i < 8; i++) {
        mesh_at(e, view, cube, opaque, br_vec3_make(-3.5f + (float)i, 5.0f, -20.0f));
    }
    step(e);
    v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL && v->opaque.count == 9 && v->stats.items_occluded == 0);
    br_node_destroy(e, view);
    step(e);
}

static br_node3d light_at(br_engine *e, br_node view, br_light3d l, br_vec3 p, br_vec3 target)
{
    br_node3d n = br_light3d_create(e, &l);
    br_node3d_add_child(e, br_view3d_root(e, view), n);
    br_node3d_set_position(e, n, p);
    br_node3d_look_at(e, n, target, br_vec3_make(0.0f, 1.0f, 0.0f));
    return n;
}

static void test_light_selection(br_engine *e)
{
    br_node view = view_on_stage(e);
    br_vec3 origin = br_vec3_make(0.0f, 0.0f, 0.0f);
    /* 24 point lights, farthest first: the old "first N" rule would keep the far ones */
    for (int i = 24; i >= 1; i--) {
        br_light3d p = br_light3d_default(BR_LIGHT3D_POINT);
        p.intensity = 1.0f;
        light_at(e, view, p, br_vec3_make((float)i * 10.0f, 0.0f, 0.0f), origin);
    }
    /* far but very bright beats a dim near one */
    br_light3d bright = br_light3d_default(BR_LIGHT3D_POINT);
    bright.intensity = 1.0e6f;
    light_at(e, view, bright, br_vec3_make(0.0f, 0.0f, -900.0f), origin);
    /* the directional light comes last and is still kept */
    light_at(e, view, br_light3d_default(BR_LIGHT3D_DIRECTIONAL), br_vec3_make(0.0f, 10.0f, 0.0f), origin);
    step(e);
    const br_view3d_state *v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL);
    if (v == NULL) {
        return;
    }
    BR_CHECK(v->lights.count == BR_LIGHT3D_MAX);
    const br_light3d_entry *l = v->lights.data;
    uint32_t directional = 0;
    uint32_t bright_kept = 0;
    float farthest_point = 0.0f;
    for (uint32_t i = 0; i < v->lights.count; i++) {
        if (l[i].light.kind == BR_LIGHT3D_DIRECTIONAL) {
            directional++;
        } else if (l[i].light.intensity > 1.0f) {
            bright_kept++;
        } else {
            farthest_point = SDL_max(farthest_point, l[i].world.m[12]);
        }
    }
    BR_CHECK(directional == 1 && bright_kept == 1);
    /* the 14 nearest unit lights: x = 10 .. 140 */
    BR_CHECK(farthest_point == (float)(BR_LIGHT3D_MAX - 2) * 10.0f);
    BR_CHECK(v->stats.lights == BR_LIGHT3D_MAX);
    br_node_destroy(e, view);
    step(e);
}

static void test_shadow_tiles(br_engine *e)
{
    br_node view = view_on_stage(e);
    br_geometry3d cube = br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f));
    br_node3d caster = mesh_at(e, view, cube, material(e, false), br_vec3_make(0.0f, 0.0f, -5.0f));
    br_vec3 target = br_vec3_make(0.0f, 0.0f, -5.0f);
    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.cast_shadows = true;
    sun.cascade_count = 3;
    light_at(e, view, sun, br_vec3_make(0.0f, 10.0f, 0.0f), target);
    br_light3d sun2 = sun; /* only the first shadowed directional light gets cascades */
    light_at(e, view, sun2, br_vec3_make(5.0f, 10.0f, 0.0f), target);
    for (int i = 0; i < BR_SHADOW3D_MAX_SPOT + 1; i++) {
        br_light3d spot = br_light3d_default(BR_LIGHT3D_SPOT);
        spot.cast_shadows = true;
        light_at(e, view, spot, br_vec3_make((float)i, 5.0f, 0.0f), target);
    }
    step(e);
    const br_view3d_state *v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL);
    if (v == NULL) {
        return;
    }
    /* tiles 0-2: the cascades; 4-7: the first four shadowed spots; tile 3 unused */
    BR_CHECK(v->shadow_tiles == 0xF7u);
    BR_CHECK(v->stats.shadow_tiles == 7);
    const br_frame3d_uniforms *f = &v->frame;
    BR_CHECK(f->counts[1] == 3.0f);
    BR_CHECK(v->lights.count == 2 + BR_SHADOW3D_MAX_SPOT + 1);
    BR_CHECK(f->light_color[0][3] == 0.0f);   /* sun: cascades start at tile 0 */
    BR_CHECK(f->light_color[1][3] == -1.0f);  /* second sun: no shadow */
    for (int i = 0; i < BR_SHADOW3D_MAX_SPOT; i++) {
        BR_CHECK(f->light_color[2 + i][3] == (float)(BR_SHADOW3D_MAX_CASCADES + i));
    }
    BR_CHECK(f->light_color[2 + BR_SHADOW3D_MAX_SPOT][3] == -1.0f); /* the fifth spot: none left */
    /* cascade splits grow and tile rects sit in the 4 x 2 atlas */
    BR_CHECK(f->cascade_far[0] > 0.0f && f->cascade_far[0] < f->cascade_far[1] && f->cascade_far[1] < f->cascade_far[2]);
    BR_CHECK(f->shadow_rect[5][0] == 0.25f && f->shadow_rect[5][1] == 0.5f && f->shadow_rect[5][2] == 0.25f);

    /* nothing casts: no tiles, lights read "fully lit" */
    br_mesh3d_get(e, caster)->cast_shadows = false;
    step(e);
    v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL && v->shadow_tiles == 0 && v->frame.light_color[0][3] == -1.0f);
    br_node_destroy(e, view);
    step(e);
}

static void test_instancing(br_engine *e)
{
    br_node view = view_on_stage(e);
    br_geometry3d cube = br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f));
    br_material3d m = material(e, false);
    br_node3d batch = br_batched_mesh3d_create(e, cube, m);
    br_node3d_add_child(e, br_view3d_root(e, view), batch);
    for (int i = 0; i < 50; i++) {
        br_batched_mesh3d_add(e, batch, br_mat4_translation(br_vec3_make((float)(i % 10), (float)(i / 10), -20.0f)),
                              br_color_make(1.0f, 1.0f, 1.0f, 1.0f));
    }
    br_batched_mesh3d_update(e, batch);
    mesh_at(e, view, cube, m, br_vec3_make(0.0f, 0.0f, -8.0f)); /* a plain mesh: its own item */
    step(e);
    const br_view3d_state *v = br_render_debug_view3d(e, view);
    BR_CHECK(v != NULL);
    if (v == NULL) {
        return;
    }
    BR_CHECK(v->opaque.count == 2);
    const br_item3d *items = v->items.data;
    uint32_t total = 0;
    bool found = false;
    for (uint32_t i = 0; i < v->items.count; i++) {
        total += items[i].instance_count;
        if (items[i].instance_count == 50) {
            const br_instance3d *inst = br_render_debug_instances3d(e, &items[i]);
            found = inst != NULL && inst[49].model.m[12] == 9.0f && inst[49].model.m[13] == 4.0f;
        }
    }
    BR_CHECK(found && total == 51);
    BR_CHECK(v->stats.instances == 51);
    br_node_destroy(e, view);
    step(e);
}

int main(void)
{
    br_engine *e = br_engine_create("test_draw_list", NULL);
    BR_CHECK(e != NULL && br_engine_gpu(e) == NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    /* without a screen size headless collect is skipped, as before */
    br_node_add_child(e, br_stage_root(e), br_sprite_create(e, br_texture_white(e)));
    step(e);
    uint32_t count = 1;
    br_render_debug_cmds(e, &count);
    BR_CHECK(count == 0);
    clear_stage(e);

    br_render_debug_set_screen(e, SCREEN_W, SCREEN_H);
    test_texture_limit(e);
    test_blend_breaks(e);
    test_masks(e);
    test_layers_and_order(e);
    test_culling(e);
    test_frustum_and_sort(e);
    test_occlusion(e);
    test_light_selection(e);
    test_shadow_tiles(e);
    test_instancing(e);
    br_engine_destroy(e);
    return br_test_finish();
}
