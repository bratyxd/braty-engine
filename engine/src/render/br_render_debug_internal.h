/* br_render_debug_internal.h - headless test API: read this frame's draw lists without a GPU.
 * Engine-internal (not in braty.h); tests include it from engine/src.
 *
 * Collect (PRE_RENDER) records everything on the CPU: 2D draw commands, batch vertices and
 * indices, 3D items with their instances and joints, sorted pass lists, lights and shadow tiles.
 * RENDER only uploads and replays. A headless engine collects too once it has a screen size:
 *
 *   br_render_debug_set_screen(e, 800, 600);
 *   br_engine_step(e, 1.0f / 60.0f);
 *   uint32_t count;
 *   const br_draw_cmd *cmds = br_render_debug_cmds(e, &count);
 *
 * Pointers stay valid until the next frame's collect.
 */
#ifndef BR_RENDER_DEBUG_INTERNAL_H
#define BR_RENDER_DEBUG_INTERNAL_H

#include "render/br_render_internal.h"
#include "three_d/br_three_d_internal.h"

/* Screen (window units = pixels) a headless engine renders, culls and runs cameras for; 0 x 0
 * (the default) skips collect. Ignored while a window exists. */
void br_render_debug_set_screen(br_engine *e, int width, int height);
/* This frame's 2D draw list in replay order (passes, batches, custom draws, callbacks). */
const br_draw_cmd *br_render_debug_cmds(br_engine *e, uint32_t *count);
/* The vertex_count vertices of a BR_DRAW_BATCH command. */
const br_batch_vertex *br_render_debug_batch_vertices(br_engine *e, const br_draw_cmd *batch);
/* The index_count indices of a BR_DRAW_BATCH command (relative to its first vertex). */
const uint16_t *br_render_debug_batch_indices(br_engine *e, const br_draw_cmd *batch);
/* The custom draw behind a BR_DRAW_CUSTOM command. */
const br_custom_cmd *br_render_debug_custom(br_engine *e, const br_draw_cmd *cmd);
/* What a view3d node collected this frame (items, opaque / transparent / shadow order, joints,
 * lights, frame uniforms, shadow_tiles); NULL if it was not drawn this frame. */
const br_view3d_state *br_render_debug_view3d(br_engine *e, br_node view);
/* The instance_count instances of a 3D mesh item. */
const br_instance3d *br_render_debug_instances3d(br_engine *e, const br_item3d *item);

#endif
