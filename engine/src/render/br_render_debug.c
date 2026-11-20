/* br_render_debug: the headless test API (br_render_debug_internal.h). */
#include "render/br_render_debug_internal.h"
#include "braty/br_camera.h"
#include "scene/br_scene_internal.h"

void br_render_debug_set_screen(br_engine *e, int width, int height)
{
    br_render_state *r = e->render;
    r->headless_width = width > 0 ? width : 0;
    r->headless_height = height > 0 ? height : 0;
    /* the culler and the cameras read the window size, which is 0 x 0 without a window */
    br_rect screen = br_rect_make(0.0f, 0.0f, (float)r->headless_width, (float)r->headless_height);
    br_scene_set_cull_view(e->scene, r->headless_width > 0 ? &screen : NULL);
    br_camera_set_headless_screen(e, screen.width, screen.height);
}

const br_draw_cmd *br_render_debug_cmds(br_engine *e, uint32_t *count)
{
    *count = e->render->cmds.count;
    return e->render->cmds.data;
}

const br_batch_vertex *br_render_debug_batch_vertices(br_engine *e, const br_draw_cmd *batch)
{
    const uint8_t *base = br_render_transient_data(e, false);
    if (batch == NULL || batch->kind != BR_DRAW_BATCH || base == NULL) {
        return NULL;
    }
    return (const br_batch_vertex *)(base + batch->vertex_offset);
}

const uint16_t *br_render_debug_batch_indices(br_engine *e, const br_draw_cmd *batch)
{
    if (batch == NULL || batch->kind != BR_DRAW_BATCH) {
        return NULL;
    }
    return (const uint16_t *)e->render->indices.data + batch->first_index;
}

const br_custom_cmd *br_render_debug_custom(br_engine *e, const br_draw_cmd *cmd)
{
    if (cmd == NULL || cmd->kind != BR_DRAW_CUSTOM) {
        return NULL;
    }
    return br_array_at(&e->render->customs, cmd->custom);
}

const br_view3d_state *br_render_debug_view3d(br_engine *e, br_node view)
{
    const br_view3d_state *v = br_view3d_state_get(e, view);
    if (v == NULL || !v->collected || v->collected_frame != br_ticker_frame(e)) {
        return NULL;
    }
    return v;
}

const br_instance3d *br_render_debug_instances3d(br_engine *e, const br_item3d *item)
{
    const uint8_t *base = br_render_transient_data(e, false);
    if (item == NULL || item->custom_fn != NULL || base == NULL) {
        return NULL;
    }
    return (const br_instance3d *)(base + item->instance_offset);
}
