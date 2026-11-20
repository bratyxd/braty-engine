/* br_post3d.h - HDR post-processing of a View3D: bloom, custom passes, tone mapping.
 *
 * Chain per frame: HDR scene (RGBA16F, optional MSAA resolved) -> bloom (threshold +
 * downsample chain + additive upsample, half resolution and below) -> custom passes in order
 * (each reads the current HDR image, writes the next) -> tone map + exposure + sRGB encode into
 * the view's 8-bit texture, which the 2D renderer then draws as the view's quad.
 *
 * Custom pass shader: a fragment shader using engine/shaders/post3d_common.hlsli (input struct
 * and the input HDR texture at t0/s0), uniforms at register(b0, space3), returning linear HDR.
 */
#ifndef BR_POST3D_H
#define BR_POST3D_H

#include "braty/br_view3d.h"

/* Custom post passes per view. */
#define BR_POST3D_MAX_PASSES 4
/* Bytes of uniform data per custom pass. */
#define BR_POST3D_UNIFORM_SIZE 256

typedef enum br_tonemap {
    BR_TONEMAP_ACES,
    BR_TONEMAP_REINHARD,
    BR_TONEMAP_LINEAR      /* clamp only */
} br_tonemap;

typedef struct br_post3d_settings {
    br_tonemap tonemap;
    float      exposure;           /* multiplier before tone mapping (default 1) */
    bool       bloom;
    float      bloom_threshold;    /* HDR luminance where bloom starts (default 1) */
    float      bloom_intensity;    /* added amount (default 0.6) */
    uint32_t   bloom_levels;       /* downsample steps, 1..6 (default 5) */
} br_post3d_settings;

/* One custom pass. */
typedef struct br_post3d_pass {
    const br_shader_blob *fragment_shader;
    uint8_t               uniforms[BR_POST3D_UNIFORM_SIZE];
    uint32_t              uniform_size;
    bool                  enabled;
} br_post3d_pass;

/* ACES, exposure 1, bloom on (threshold 1, intensity 0.6, 5 levels). */
br_post3d_settings br_post3d_default(void);
/* Replaces the view's post settings. */
void br_view3d_set_post(br_engine *e, br_node view, const br_post3d_settings *settings);
/* The view's post settings, editable in place (NULL if view is not a view3d). */
br_post3d_settings *br_view3d_post(br_engine *e, br_node view);
/* Appends a custom pass (uniforms copied). Returns its index, or -1 when full. */
int br_view3d_add_post_pass(br_engine *e, br_node view, const br_shader_blob *fragment_shader,
                            const void *uniforms, uint32_t uniform_size);
/* A custom pass, editable in place (uniforms, enabled; uniform_size is clamped to
 * BR_POST3D_UNIFORM_SIZE when drawn). NULL if out of range. */
br_post3d_pass *br_view3d_post_pass(br_engine *e, br_node view, int index);

#endif
