/* br_render.h - the 2D renderer: draw list, batcher, masks, render-to-texture, extension points.
 *
 * Two phases per frame:
 *   PRE_RENDER "render.collect" (priority 1000): walks the stage (and queued render-to-texture
 *       jobs), calls each node type's render callback with a br_render_ctx; callbacks push
 *       geometry. Result: a CPU draw list + vertices written to the GPU transient buffers.
 *   RENDER "render.submit" (priority 0): uploads, then replays the draw list through SDL_GPU.
 *
 * Batching: quads/triangles that share blend mode, transform space, scissor, stencil state
 * and fit in 8 texture slots and 65535 vertices go out in one draw call.
 *
 * Vertex positions you push are in the current "space" (see br_render_node_transform): use
 * br_render_node_transform(rc, n) to map a node's local coordinates. Colours are packed
 * premultiplied RGBA8 (br_render_node_color gives the node's tint * world alpha).
 *
 * Extension points (details and examples in docs/agents/render.md):
 *   - a node type's `render` callback: br_render_push_quad / br_render_push_triangles /
 *     br_render_push_custom (own shaders) / br_render_push_callback (own SDL_GPU passes);
 *   - br_render_subtree_to_texture: draw any subtree into a render texture mid-traversal;
 *   - br_render_set_effects_hook: one per engine; gets every node with filters, cache_as_texture,
 *     an alpha/colour mask or an advanced blend mode (Task 8);
 *   - br_render_add_overlay: draw after the stage in window space (debug UI).
 */
#ifndef BR_RENDER_H
#define BR_RENDER_H

#include "braty/br_core.h"
#include "braty/br_gpu.h"
#include "braty/br_math.h"
#include "braty/br_node.h"
#include "braty/br_texture.h"

/* Max textures one batch (and one custom draw) can bind. */
#define BR_RENDER_MAX_TEXTURES 8
/* Max vertices one br_render_push_triangles call / one batch can hold (16-bit indices). */
#define BR_RENDER_MAX_BATCH_VERTICES 65535

/* One batched vertex. color = premultiplied RGBA8 in memory order (br_render_pack_color). */
typedef struct br_vertex2d {
    float    x, y;
    float    u, v;
    uint32_t color;
} br_vertex2d;

/* ---- inside a node type's render callback (or an effects hook / overlay) */

/* Matrix mapping n's local coordinates into the space pushed vertices are in. */
br_mat3 br_render_node_transform(br_render_ctx *rc, br_node n);
/* n's world tint * world alpha, premultiplied and packed for br_vertex2d.color. */
uint32_t br_render_node_color(br_render_ctx *rc, br_node n);
/* Packs a straight-alpha colour times an extra alpha into premultiplied RGBA8. */
uint32_t br_render_pack_color(br_color color, float alpha);
/* Blend mode used by pushes (the node's resolved blend mode during its callback). */
br_blend_mode br_render_blend(br_render_ctx *rc);
/* Overrides the blend mode for following pushes; returns the previous one. */
br_blend_mode br_render_set_blend(br_render_ctx *rc, br_blend_mode blend);
/* Pixels of the current target per world unit (for pixel snapping and resolution). */
float br_render_pixel_ratio(br_render_ctx *rc);
/* The world rect the current target shows. */
br_rect br_render_area(br_render_ctx *rc);
/* The current space as a world matrix (identity outside render groups). */
br_mat3 br_render_space(br_render_ctx *rc);
/* Sets the space following pushes are in (identity = world coordinates); returns the old one. */
br_mat3 br_render_set_space(br_render_ctx *rc, br_mat3 space);

/* br_texture_get_info, cached for the rest of this collect pass (cheap to call per node). */
const br_texture_info *br_render_texture_info(br_render_ctx *rc, br_texture t);

/* Pushes one textured quad: v = top-left, top-right, bottom-right, bottom-left. */
void br_render_push_quad(br_render_ctx *rc, br_texture texture, const br_vertex2d v[4]);
/* Pushes indexed triangles (indices relative to `vertices`, vertex_count <= 65535). */
void br_render_push_triangles(br_render_ctx *rc, br_texture texture, const br_vertex2d *vertices,
                              uint32_t vertex_count, const uint16_t *indices, uint32_t index_count);

/* A draw with your own shaders. Data is copied at push time.
 * Uniform slots: vertex b0 = br_mat4 transform (ortho * space, pushed by the engine),
 * vertex b1 = vertex_uniforms, fragment b0 = fragment_uniforms. Texture i binds to t[i]/s[i]. */
typedef struct br_custom_draw {
    const br_shader_blob *vertex_shader;
    const br_shader_blob *fragment_shader;
    br_vertex_layout      layout;           /* buffer 0 = vertices, buffer 1 = instances (optional) */
    SDL_GPUPrimitiveType  primitive;        /* zero = triangle list */
    const void           *vertices;
    uint32_t              vertex_bytes;
    const void           *instances;        /* NULL = not instanced */
    uint32_t              instance_bytes;
    uint32_t              instance_count;   /* 0 = 1 */
    const uint32_t       *indices;          /* NULL = draw vertex_bytes / stride vertices */
    uint32_t              index_count;
    br_texture            textures[BR_RENDER_MAX_TEXTURES];
    uint32_t              texture_count;
    const void           *vertex_uniforms;
    uint32_t              vertex_uniform_size;
    const void           *fragment_uniforms;
    uint32_t              fragment_uniform_size;
} br_custom_draw;
/* Pushes a custom-shader draw (blend = br_render_blend, masks/scissor apply). */
void br_render_push_custom(br_render_ctx *rc, const br_custom_draw *draw);

/* What a render callback sees when the draw list is replayed (RENDER phase). */
typedef struct br_render_callback_info {
    SDL_GPUCommandBuffer *cmd;
    SDL_GPURenderPass    *pass;          /* open pass on target; NULL for own_passes callbacks */
    br_render_target     *target;        /* the target being drawn (frame target or render texture) */
    br_mat4               transform;     /* ortho(area) * space at push time */
    br_blend_mode         blend;
    bool                  stencil_test;  /* inside a stencil mask: test EQUAL stencil_ref */
    uint8_t               stencil_ref;
    bool                  scissor_on;    /* inside a scissor mask: clip to `scissor` */
    SDL_Rect              scissor;       /* target pixels, clamped to the target (whole target if !scissor_on).
                                          * In-pass callbacks start with it set; reset it after
                                          * SDL_SetGPUScissor calls of your own. */
} br_render_callback_info;
typedef void (*br_render_callback_fn)(br_engine *e, const br_render_callback_info *info, void *user);
/* Pushes a callback run during replay. own_passes = false: draw into info->pass.
 * own_passes = true: the engine ends its pass first (pass = NULL), you may run any passes
 * (3D, filter ping-pong) on cmd, and the engine reopens the target afterwards (contents kept).
 * `user` must stay valid until the RENDER phase of this frame ends. */
void br_render_push_callback(br_render_ctx *rc, br_render_callback_fn fn, void *user, bool own_passes);
/* Fills blend, colour/depth formats and stencil test of a pipeline so it fits the callback's target. */
void br_render_pipeline_setup(const br_render_callback_info *info, br_pipeline_desc *desc);
/* SDL blend state for a built-in blend mode (premultiplied). */
SDL_GPUColorTargetBlendState br_render_blend_state(br_blend_mode blend);

/* Renders n's subtree (n included even if it is another node's mask; n's own effects ignored)
 * into a render texture, mapping the world rect `area` onto the whole texture; then continues
 * where it was. Use from a node render callback, an effects hook or an overlay.
 * clear: clear to clear_color first. */
void br_render_subtree_to_texture(br_render_ctx *rc, br_node n, br_texture target, br_rect area, bool clear,
                                  br_color clear_color);

/* ---- hooks */

/* Effects hook: called instead of the normal draw for nodes where br_node_needs_effects is
 * true. Return true if you drew the node (typically via br_render_subtree_to_texture + a
 * quad), false to let the engine draw it normally. One per engine (Task 8). */
typedef bool (*br_render_effects_fn)(br_engine *e, br_node n, br_render_ctx *rc, void *user);
/* Installs the effects hook (replaces any previous one, e.g. the filters module's). */
void br_render_set_effects_hook(br_engine *e, br_render_effects_fn fn, void *user);
/* The installed effects hook (NULL if none) and its user pointer. Read it before installing your
 * own and call it for the nodes you do not handle, so the filters module keeps working. */
br_render_effects_fn br_render_effects_hook(br_engine *e, void **user);

/* Overlay: called every frame after the stage, in window space (debug UI, HUD tools). */
typedef void (*br_render_overlay_fn)(br_engine *e, br_render_ctx *rc, void *user);
/* Adds an overlay (drawn in the order added). */
void br_render_add_overlay(br_engine *e, br_render_overlay_fn fn, void *user);

/* ---- outside the render phase */

/* Queues n's subtree to be drawn into a render texture this frame (before the stage), with
 * n's world transform; texture pixel (0,0) is world (0,0). Clears the texture first. */
void br_render_to_texture(br_engine *e, br_node n, br_texture target);

/* ---- views: cameras (br_camera.h) draw the stage through these */

/* One way to draw the stage (or another 2D subtree) this frame. */
typedef struct br_render_view {
    br_node    root;           /* zero handle = the stage (br_scene_root) */
    br_texture target;         /* zero handle = the window */
    br_mat4    projection;     /* root-space -> clip space of the WHOLE target */
    br_rect    area;           /* root-space rect the view shows (br_render_area, inverse masks) */
    br_rect    viewport;       /* part of the target drawn into, in target units (window units) */
    uint32_t   culling_mask;   /* nodes with (layer bits & mask) == 0 are not drawn; children still visited */
    bool       clear;          /* fill the viewport with clear_color first */
    br_color   clear_color;
} br_render_view;
/* Draws these views, in order, instead of the default pixel stage (copied; kept until the next
 * call). count = 0 goes back to the default: the stage in window coordinates. */
void br_render_set_views(br_engine *e, const br_render_view *views, uint32_t count);

/* Last frame's numbers. */
typedef struct br_render_stats {
    uint32_t draw_calls;    /* batches + custom draws */
    uint32_t batches;
    uint32_t vertices;
    uint32_t passes;
    uint32_t nodes_drawn;   /* render callbacks called */
} br_render_stats;
/* Copies the stats of the last collected frame. */
void br_render_get_stats(br_engine *e, br_render_stats *out);

#endif
