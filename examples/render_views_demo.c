/* render_views_demo - 2D drawing cases that only a GPU frame can check (golden image).
 *
 * The window is split into two views side by side: the left one shows world 0..400, the right
 * one world 400..800, both with the whole-window projection (like split screen). Expected:
 *   right view: a red square clipped to 450..550 x 50..150 by a scissor mask (it vanished when
 *     scissor rects were not offset by the viewport), and a white BR_BLEND_DIFFERENCE box on
 *     blue that reads yellow (magenta = the backdrop was read from the left view);
 *   left view: two white boxes faded to 25% over green, the left one cache_as_texture (the fade
 *     applies without a re-render) and the right one plain, both light green; a red box shown
 *     through a stencil mask whose node has alpha 0 (the mask still clips); a box whose mask
 *     contains a node masked by the same mask (drawn once, no endless recursion).
 *
 * Run: render_views_demo --frames 10 --screenshot out.png [--gpu-driver vulkan|direct3d12]
 */
#include "braty/braty_main.h"

typedef struct demo_state {
    br_node cached;
    int     frame;
} demo_state;

static br_node box(br_engine *e, br_node parent, float x, float y, float w, float h, br_color c)
{
    br_node s = br_sprite_create(e, br_texture_white(e));
    br_node_set_position(e, s, br_vec2_make(x, y));
    br_node_set_scale(e, s, br_vec2_make(w, h));
    br_node_set_tint(e, s, c);
    br_node_add_child(e, parent, s);
    return s;
}

static void fade_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *d = user;
    d->frame++;
    if (d->frame == 5) {
        br_node_set_alpha(e, d->cached, 0.25f); /* after the cache was drawn at full alpha */
    }
}

static void views_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_render_view v[2];
    SDL_zeroa(v);
    for (int i = 0; i < 2; i++) {
        v[i].area = br_rect_make(400.0f * (float)i, 0.0f, 400.0f, 400.0f);
        v[i].viewport = v[i].area;
        v[i].projection = br_mat4_ortho(0.0f, 800.0f, 400.0f, 0.0f, -1.0f, 1.0f);
        v[i].culling_mask = 0xFFFFFFFFu;
    }
    br_render_set_views(e, v, 2);
}

static void free_state(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool init(br_engine *e)
{
    demo_state *d = br_alloc(sizeof(demo_state));
    if (d == NULL) {
        return false;
    }
    SDL_zerop(d);
    br_engine_on_shutdown(e, free_state, d);
    br_node root = br_stage_root(e);
    box(e, root, 0, 0, 400, 400, br_color_make(0, 1, 0, 1));
    box(e, root, 400, 0, 400, 400, br_color_make(0, 0, 1, 1));

    /* right view: scissor mask and advanced blend */
    br_node clip = box(e, root, 450, 50, 100, 100, br_color_make(1, 1, 1, 1));
    br_node clipped = box(e, root, 420, 20, 200, 200, br_color_make(1, 0, 0, 1));
    br_node_set_mask(e, clipped, clip, BR_MASK_SCISSOR, false);
    br_node diff = box(e, root, 650, 50, 100, 100, br_color_make(1, 1, 1, 1));
    br_node_set_blend_mode(e, diff, BR_BLEND_DIFFERENCE);

    /* left view: cached fade next to a plain fade */
    d->cached = br_node_create_container(e);
    br_node_add_child(e, root, d->cached);
    box(e, d->cached, 20, 20, 100, 100, br_color_make(1, 1, 1, 1));
    br_node_set_cache_as_texture(e, d->cached, true);
    br_node plain = br_node_create_container(e);
    br_node_add_child(e, root, plain);
    box(e, plain, 140, 20, 100, 100, br_color_make(1, 1, 1, 1));
    br_node_set_alpha(e, plain, 0.25f);

    /* left view: stencil mask with alpha 0 */
    br_node hidden_mask = box(e, root, 40, 180, 60, 60, br_color_make(1, 1, 1, 1));
    br_node_set_alpha(e, hidden_mask, 0.0f);
    br_node shown = box(e, root, 20, 160, 100, 100, br_color_make(1, 0, 0, 1));
    br_node_set_mask(e, shown, hidden_mask, BR_MASK_STENCIL, false);

    /* left view: a node inside its own mask (built after set_mask, which rejects it directly) */
    br_node mask = br_node_create_container(e);
    br_node_add_child(e, root, mask);
    box(e, mask, 180, 180, 60, 60, br_color_make(1, 1, 1, 1));
    br_node inner = box(e, root, 200, 200, 20, 20, br_color_make(1, 1, 1, 1));
    br_node content = box(e, root, 160, 160, 100, 100, br_color_make(1, 1, 0, 1));
    br_node_set_mask(e, content, mask, BR_MASK_STENCIL, false);
    br_node_set_mask(e, inner, mask, BR_MASK_STENCIL, false);
    br_node_add_child(e, mask, inner); /* now inside the mask it uses */

    br_system_add(e, "demo.fade", BR_PHASE_UPDATE, 0, fade_system, d);
    br_system_add(e, "demo.views", BR_PHASE_PRE_RENDER, -500, views_system, NULL);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "render views demo";
    desc.width = 800;
    desc.height = 400;
    desc.init = init;
    return desc;
}
