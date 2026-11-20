/* br_render_internal.h - texture table, draw list and render context. Engine-private. */
#ifndef BR_RENDER_INTERNAL_H
#define BR_RENDER_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_render.h"

typedef struct br_texture_record {
    br_texture        source;        /* itself for sources, the owning source for frames */
    br_gpu_texture   *gpu;           /* sources: the pixels (NULL headless or for render textures) */
    br_render_target *target;        /* render textures only */
    bool              owns_gpu;
    uint32_t          source_width;  /* pixels */
    uint32_t          source_height;
    br_texture_frame  frame;         /* frame/orig/trim filled in (no zero defaults) */
    float             resolution;
    br_texture_style  style;
} br_texture_record;

/* What the stencil buffer does for a draw. */
typedef enum br_stencil_mode {
    BR_STENCIL_OFF,
    BR_STENCIL_TEST,   /* draw where stencil == ref */
    BR_STENCIL_INCR,   /* no colour; stencil += 1 where stencil == ref */
    BR_STENCIL_DECR    /* no colour; stencil -= 1 where stencil == ref */
} br_stencil_mode;

/* Pipeline-relevant state of a draw, copied into each command. */
typedef struct br_draw_state {
    br_blend_mode   blend;
    br_stencil_mode stencil;
    uint8_t         stencil_ref;
    bool            scissor_on;
    SDL_Rect        scissor;       /* target pixels */
    br_mat4         transform;     /* ortho(area) * space */
} br_draw_state;

typedef enum br_draw_kind {
    BR_DRAW_PASS,
    BR_DRAW_BATCH,
    BR_DRAW_CUSTOM,
    BR_DRAW_CALLBACK
} br_draw_kind;

typedef struct br_draw_cmd {
    br_draw_kind  kind;
    br_draw_state state;
    uint32_t      serial;          /* render-ctx state serial when recorded (batch merging) */
    /* pass */
    br_texture    target;          /* zero = the frame target */
    bool          clear;
    br_color      clear_color;
    /* batch: vertices live in the GPU transient vertex stream, indices in br_render_state.indices */
    uint32_t      first_index;
    uint32_t      index_count;
    uint32_t      vertex_offset;   /* bytes into the transient vertex buffer */
    uint32_t      vertex_count;    /* indices are relative to vertex_offset */
    uint32_t      texture_count;
    br_texture    textures[BR_RENDER_MAX_TEXTURES];  /* source textures */
    /* custom */
    uint32_t      custom;          /* index into br_render_state.customs */
    /* callback */
    br_render_callback_fn fn;
    void         *user;
    bool          own_passes;
} br_draw_cmd;

typedef struct br_custom_cmd {
    const br_shader_blob *vertex_shader;
    const br_shader_blob *fragment_shader;
    br_vertex_layout      layout;
    SDL_GPUPrimitiveType  primitive;
    uint32_t              vertex_offset;    /* bytes in the transient vertex buffer */
    uint32_t              vertex_count;
    bool                  instanced;
    uint32_t              instance_offset;
    uint32_t              instance_count;
    bool                  indexed;
    uint32_t              index_offset;     /* bytes in the transient index buffer (32-bit indices) */
    uint32_t              index_count;
    br_texture            textures[BR_RENDER_MAX_TEXTURES];
    uint32_t              texture_count;
    uint32_t              vertex_uniform_offset;  /* bytes into br_render_state.uniforms */
    uint32_t              vertex_uniform_size;
    uint32_t              fragment_uniform_offset;
    uint32_t              fragment_uniform_size;
} br_custom_cmd;

/* The vertex the batch shader reads (br_vertex2d + texture slot). */
typedef struct br_batch_vertex {
    float    x, y;
    float    u, v;
    uint32_t color;
    uint32_t texture;
} br_batch_vertex;

#define BR_RENDER_INFO_CACHE 16  /* texture infos cached per collect, direct-mapped by index */
#define BR_RENDER_MAX_DEPTH 2048 /* render walk levels (it recurses); deeper nodes are not drawn */

struct br_render_ctx {
    br_engine *e;
    br_texture target;            /* zero = frame target */
    br_rect    area;              /* world rect shown by the target */
    float      pixel_width;       /* target size in pixels */
    float      pixel_height;
    bool       has_stencil;       /* target has a stencil buffer */
    br_mat3    space;
    br_blend_mode blend;
    br_stencil_mode stencil;
    uint8_t    stencil_ref;
    uint8_t    stencil_depth;     /* nested stencil masks active */
    bool       scissor_on;
    SDL_Rect   scissor;
    br_mat4    transform;         /* projection * space */
    bool       has_projection;    /* a view (camera): `projection` instead of ortho(area) */
    br_mat4    projection;        /* world -> clip of the whole target */
    float      target_width;      /* target size in pixels (scissor maths of views) */
    float      target_height;
    uint32_t   culling_mask;      /* layer bits a node needs to be drawn */
    uint32_t   layer_bits;        /* effective layer bits of the node being walked (inherited) */
    uint32_t   depth;             /* levels of br_render_walk active (limit BR_RENDER_MAX_DEPTH) */
    uint32_t   mask_draws;        /* > 0 while a mask's geometry is drawn: masks inside are ignored */
    uint32_t   serial;            /* changes whenever state that splits batches changes */
    int        open_batch;        /* index of the batch accepting quads, -1 if none */
    /* per-pass caches (textures do not change while the stage is walked) */
    br_texture      info_keys[BR_RENDER_INFO_CACHE];
    br_texture_info info_cache[BR_RENDER_INFO_CACHE];
    br_texture      last_texture;     /* last pushed texture and its source */
    br_texture      last_source;
};

typedef struct br_rtt_job {
    br_node    node;
    br_texture target;
} br_rtt_job;

typedef struct br_overlay_entry {
    br_render_overlay_fn fn;
    void                *user;
} br_overlay_entry;

typedef struct br_render_state {
    br_pool    textures;          /* br_texture_record */
    br_texture white;
    br_texture missing;

    br_array   cmds;              /* br_draw_cmd */
    br_array   customs;           /* br_custom_cmd */
    br_array   indices;           /* uint16_t, copied to the transient index buffer at the end of collect */
    br_array   uniforms;          /* uint8_t */
    br_array   rtt_jobs;          /* br_rtt_job */
    br_array   overlays;          /* br_overlay_entry */
    br_array   views;             /* br_render_view (br_render_set_views); empty = default stage */
    uint32_t   index_base;        /* byte offset of `indices` in the transient index buffer */
    uint32_t   serial_counter;
    bool       collected;         /* the draw list belongs to this frame */
    bool       warned_no_stencil;
    bool       warned_depth;

    br_render_effects_fn effects_fn;
    void                *effects_user;

    br_render_stats stats;
    br_render_stats building;
} br_render_state;

/* ---- br_texture.c */

/* Creates the texture table. */
void br_texture_init(br_engine *e);
/* Destroys every texture and the table. */
void br_texture_shutdown(br_engine *e);
/* Record of a live texture, NULL if stale. */
br_texture_record *br_texture_record_get(br_engine *e, br_texture t);
/* The source handle behind t (missing texture if t or its source is stale). */
br_texture br_texture_source_of(br_engine *e, br_texture t);
/* GPU texture + sampler of a source for binding; falls back to the white texture. */
bool br_texture_binding(br_engine *e, br_texture source, SDL_GPUTextureSamplerBinding *out);

/* ---- br_render.c */

/* Registers the render module (textures, sprite type, collect/submit systems). */
void br_render_register(br_engine *e);
/* Starts a new state serial: following pushes do not join the open batch. */
void br_render_ctx_changed(br_render_ctx *rc);
/* Target-pixel rect covering a rect of root space (the space of the context's area / view
 * projection), floored/ceiled outward, not clipped. Includes the viewport offset of views and
 * their zoom; a rotated view gives the bounding box. Used by scissor masks and the filters
 * module (backdrop copies). Meaningless while capturing for 3D (target_width == 0). */
br_rect br_render_rect_to_pixels(br_render_ctx *rc, br_rect r);

/* br_render_subtree_to_texture; own_color false draws the subtree as if n and its ancestors had
 * alpha 1 and a white tint (cache_as_texture: the cached quad applies n's colour when drawn). */
void br_render_subtree_to_texture_ex(br_render_ctx *rc, br_node n, br_texture target, br_rect area, bool clear,
                                     br_color clear_color, bool own_color);

/* Records n's subtree (n's own effects skipped, like br_render_subtree_to_texture) with
 * `projection` mapping n's root space to "clip" space, but instead of drawing it, moves the
 * commands into `out` (br_draw_cmd). Batches keep pointing at this frame's transient vertices
 * and at br_render_state.indices / customs / uniforms, which stay valid until RENDER ends.
 * pixel_ratio = pixels per root unit the content is rasterised for (text resolution).
 * Call only during PRE_RENDER collect (e.g. from a 3D node's collect). False if nothing to do. */
bool br_render_capture_subtree(br_engine *e, br_node n, br_mat4 projection, br_rect area, float pixel_ratio,
                               br_array *out);

/* ---- br_batch.c */

/* Reserves a quad (4 vertices + its 6 indices) of `texture` in the current batch: fill the
 * returned vertices in place (they are already in the GPU transient buffer), with .texture =
 * *slot. NULL on failure. */
br_batch_vertex *br_batch_quad(br_render_ctx *rc, br_texture texture, uint32_t *slot);
/* Appends a pass command and opens no batch. */
void br_batch_push_pass(br_render_ctx *rc, br_texture target, bool clear, br_color clear_color);
/* Writes this frame's batched vertices/indices into the GPU transient buffers. */
void br_batch_finish(br_engine *e);
/* Replays the draw list (RENDER phase). */
void br_batch_replay(br_engine *e);
/* Clears the draw list for a new frame. */
void br_batch_reset(br_engine *e);

#endif
