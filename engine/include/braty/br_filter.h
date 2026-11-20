/* br_filter.h - filters (PixiJS Filter), cache_as_texture and advanced blend modes.
 *
 * A node's filter list is plain data: an array of br_filter values copied into the node
 * (br_filters_set) and editable in place (br_filters_get). Every frame a filtered node is drawn
 * into a pooled render texture covering its bounds (+ padding), each filter runs as one or more
 * full-texture passes (ping-pong between two pooled textures), and the result is drawn back into
 * the parent target with the node's blend mode. Filtered children inside filtered parents work.
 *
 * Built-in filters: alpha, blur (separable Gaussian), colour matrix (all Pixi presets),
 * displacement, noise. Custom filters: your fragment shader blob + a uniform block + up to
 * BR_FILTER_MAX_TEXTURES extra textures.
 *
 * Fixed inputs of every filter pass (custom shaders rely on these; see docs/agents/filters.md):
 *   vertex shader  : engine-owned (filter.vert.hlsl, a full-target triangle). Its output, which
 *                    your fragment input struct must equal exactly:
 *                      struct br_filter_v2f { float4 position : SV_Position;
 *                                             float2 uv : TEXCOORD0;      // input texture uv
 *                                             float2 local : TEXCOORD1; }; // 0..1 over the filter area
 *   t0/s0 (space2) : the input (premultiplied RGBA, linear clamp; content sits in uv 0..content.zw)
 *   t1..tN/s1..sN  : the filter's extra textures (their own sampling style)
 *   b0 (space3)    : one cbuffer: first the 64 bytes of br_filter_globals, then your uniforms
 * Output: premultiplied colour for that pixel (blending is off; the pass overwrites).
 *
 * Advanced blend modes (pixi.js/advanced-blend-modes): br_node_set_blend_mode(e, n, BR_BLEND_OVERLAY).
 * The node is drawn offscreen, the target region under it is copied to a pooled texture, and a
 * blend shader composites the two.
 *
 * cache_as_texture: br_node_set_cache_as_texture(e, n, true) renders the subtree once into a
 * texture (without the node's and its ancestors' alpha/tint); later frames draw that texture
 * with the node's current transform, alpha and tint. Call br_cache_update_texture after
 * changing the subtree's content.
 */
#ifndef BR_FILTER_H
#define BR_FILTER_H

#include "braty/br_render.h"

/* Registers the module (effects hook, pool system). Called once by br_builtin.c. */
void br_filters_register(br_engine *e);

#define BR_FILTER_MAX_PER_NODE 8         /* filters in one node's list */
#define BR_FILTER_MAX_UNIFORM_BYTES 256  /* custom uniform block size */
#define BR_FILTER_MAX_TEXTURES 4         /* extra textures of a custom filter (t1..t4) */
#define BR_FILTER_MAX_SIZE 4096          /* largest filter texture edge in pixels */

typedef enum br_filter_type {
    BR_FILTER_ALPHA,
    BR_FILTER_BLUR,
    BR_FILTER_COLOR_MATRIX,
    BR_FILTER_DISPLACEMENT,
    BR_FILTER_NOISE,
    BR_FILTER_CUSTOM
} br_filter_type;

/* One filter. Make it with a br_filter_* constructor, then change fields freely. Each type
 * reads only its own fields. */
typedef struct br_filter {
    br_filter_type type;
    bool           enabled;
    float          padding;        /* world units added around the area (blur sets this) */
    float          alpha;          /* ALPHA: opacity; COLOR_MATRIX: mix of original (0) .. result (1) */
    /* BLUR */
    float          blur_x;         /* strength in world units, per axis */
    float          blur_y;
    int            quality;        /* passes per axis (more = smoother) */
    int            kernel_size;    /* taps per pass: 5, 7, 9, 11, 13 or 15 */
    /* COLOR_MATRIX: 4x5 row-major like Pixi (r' = m0 r + m1 g + m2 b + m3 a + m4; offsets in 0..1) */
    float          matrix[20];
    /* DISPLACEMENT: the map's red/green move pixels by (value - 0.5) * scale world units */
    br_texture     map;            /* tiled over the area at its own size (use a REPEAT style) */
    br_vec2        scale;
    br_vec2        map_offset;     /* world units; animate to scroll the map */
    /* NOISE */
    float          noise;          /* 0..1 */
    float          seed;
    /* CUSTOM */
    const br_shader_blob *shader;  /* fragment shader, see the fixed inputs above */
    uint8_t        uniforms[BR_FILTER_MAX_UNIFORM_BYTES];
    uint32_t       uniform_size;
    br_texture     textures[BR_FILTER_MAX_TEXTURES];
    uint32_t       texture_count;
} br_filter;

/* What the engine puts at the start of every filter pass's b0 cbuffer (and vertex b0). */
typedef struct br_filter_globals {
    float input_size[4];   /* input texture width, height, 1/width, 1/height (pixels) */
    float content[4];      /* uv rect of the filtered content in the input: x0, y0, x1, y1 */
    float area[4];         /* world rect of the filter area: x, y, width, height */
    float params[4];       /* elapsed seconds, pixels per world unit, 0, 0 */
} br_filter_globals;

/* ---- constructors (no engine needed) */

/* Multiplies the whole filtered subtree's opacity (Pixi AlphaFilter). */
br_filter br_filter_alpha(float alpha);
/* Gaussian blur; strength in world units (Pixi BlurFilter: strength 8, quality 4, kernel 5). */
br_filter br_filter_blur(float strength, int quality, int kernel_size);
/* Colour matrix starting as identity; change it with the br_color_matrix_* presets. */
br_filter br_filter_color_matrix(void);
/* Displacement by a map texture (Pixi DisplacementFilter). */
br_filter br_filter_displacement(br_texture map, float scale_x, float scale_y);
/* Grain noise (Pixi NoiseFilter: noise 0.5). */
br_filter br_filter_noise(float noise, float seed);
/* A custom filter: fragment shader + uniform block (copied, may be NULL) + extra textures later. */
br_filter br_filter_custom(const br_shader_blob *fragment_shader, const void *uniforms, uint32_t uniform_size);

/* ---- per-node filter lists */

/* Copies `count` filters into n's list (count 0 removes filters). False on bad arguments. */
bool br_filters_set(br_engine *e, br_node n, const br_filter *filters, uint32_t count);
/* Number of filters on n. */
uint32_t br_filters_count(br_engine *e, br_node n);
/* The i-th filter of n, editable in place; NULL if out of range. */
br_filter *br_filters_get(br_engine *e, br_node n, uint32_t i);
/* Filter texture pixels per target pixel (default 1; 0.5 = half resolution, cheaper blur). */
void br_filters_set_resolution(br_engine *e, br_node n, float resolution);
/* Filters only this rect in n's local space instead of the measured bounds (NULL = bounds). */
void br_filters_set_area(br_engine *e, br_node n, const br_rect *local_area);

/* ---- colour matrix presets (Pixi ColorMatrixFilter). Each one replaces m, or with
 * multiply = true combines with the current m (like Pixi: m = m x preset, so the preset acts
 * on the colour first, then the old m). */

void br_color_matrix_identity(float m[20]);
/* Combines a and b: the result applies b first, then a. out may alias a or b. */
void br_color_matrix_multiply(float out[20], const float a[20], const float b[20]);
/* Applies m to a straight-alpha colour (what the shader does per pixel), clamped to 0..1. */
br_color br_color_matrix_apply(const float m[20], br_color c);
void br_color_matrix_brightness(float m[20], float b, bool multiply);
void br_color_matrix_tint(float m[20], br_color color, bool multiply);
void br_color_matrix_greyscale(float m[20], float scale, bool multiply);
void br_color_matrix_black_and_white(float m[20], bool multiply);
void br_color_matrix_hue(float m[20], float degrees, bool multiply);
void br_color_matrix_contrast(float m[20], float amount, bool multiply);
void br_color_matrix_saturate(float m[20], float amount, bool multiply);
void br_color_matrix_desaturate(float m[20]);
void br_color_matrix_negative(float m[20], bool multiply);
void br_color_matrix_sepia(float m[20], bool multiply);
void br_color_matrix_technicolor(float m[20], bool multiply);
void br_color_matrix_polaroid(float m[20], bool multiply);
void br_color_matrix_to_bgr(float m[20], bool multiply);
void br_color_matrix_kodachrome(float m[20], bool multiply);
void br_color_matrix_browni(float m[20], bool multiply);
void br_color_matrix_vintage(float m[20], bool multiply);
void br_color_matrix_color_tone(float m[20], float desaturation, float toned, br_color light, br_color dark,
                                bool multiply);
void br_color_matrix_night(float m[20], float intensity, bool multiply);
void br_color_matrix_predator(float m[20], float amount, bool multiply);
void br_color_matrix_lsd(float m[20], bool multiply);

/* ---- blur helper */

/* Gaussian weights for a kernel (5..15 taps, odd; other sizes are clamped/rounded up).
 * weights[0] = centre, weights[i] = taps at +-i; centre + 2 * sides sums to 1.
 * Returns the number of weights written (kernel_size / 2 + 1, at most 8). */
uint32_t br_filter_blur_kernel(int kernel_size, float weights[8]);

/* ---- render-target pool (filters, masks, blend backdrops; usable by your own effects) */

/* Pool bucket for a pixel size: next power of two, at least 16, at most BR_FILTER_MAX_SIZE. */
uint32_t br_filters_pool_bucket(uint32_t pixels);
/* A free pooled render texture of bucket(width) x bucket(height) pixels (resolution 1). It is
 * yours until the pool is released at the start of the next frame. Zero handle on failure. */
br_texture br_filters_pool_acquire(br_engine *e, uint32_t width, uint32_t height);
/* Marks every pooled texture free (the module does this at the start of each frame). */
void br_filters_pool_release_all(br_engine *e);
/* Destroys the free pooled textures (done on window resize and for textures idle 120 frames). */
void br_filters_pool_trim(br_engine *e);
/* Pooled textures alive (in use or free). */
uint32_t br_filters_pool_count(br_engine *e);

/* ---- cache_as_texture (Pixi cacheAsTexture / updateCacheTexture) */

/* Re-renders n's cached texture next frame (after its content changed). */
void br_cache_update_texture(br_engine *e, br_node n);

/* ---- advanced blend modes: values for br_node_set_blend_mode */

typedef enum br_blend_advanced {
    BR_BLEND_COLOR = BR_BLEND_ADVANCED_FIRST,
    BR_BLEND_COLOR_BURN,
    BR_BLEND_COLOR_DODGE,
    BR_BLEND_DARKEN,
    BR_BLEND_DIFFERENCE,
    BR_BLEND_DIVIDE,
    BR_BLEND_EXCLUSION,
    BR_BLEND_HARD_LIGHT,
    BR_BLEND_HARD_MIX,
    BR_BLEND_LIGHTEN,
    BR_BLEND_LINEAR_BURN,
    BR_BLEND_LINEAR_DODGE,
    BR_BLEND_LINEAR_LIGHT,
    BR_BLEND_LUMINOSITY,
    BR_BLEND_NEGATION,
    BR_BLEND_OVERLAY,
    BR_BLEND_PIN_LIGHT,
    BR_BLEND_SATURATION,
    BR_BLEND_SOFT_LIGHT,
    BR_BLEND_SUBTRACT,
    BR_BLEND_VIVID_LIGHT,
    BR_BLEND_ADVANCED_END
} br_blend_advanced;

#endif
