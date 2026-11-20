/* br_texture.h - textures (PixiJS Texture + TextureSource) and render textures.
 *
 * A br_texture is a handle. A "source" texture owns GPU pixels; a "frame" texture is a
 * rectangle of a source (atlas frame) with optional trim, 90-degree rotation and a default
 * anchor. Pixels are premultiplied on upload; every draw assumes premultiplied colour.
 *
 * Sizes: frame/orig/trim are in source pixels; br_texture_info.width/height are logical units
 * (orig / resolution). A stale handle (destroyed texture or destroyed source) draws as the
 * magenta "missing" texture.
 *
 * Headless engines (no GPU) still create texture records with sizes, without GPU pixels.
 */
#ifndef BR_TEXTURE_H
#define BR_TEXTURE_H

#include "braty/br_core.h"
#include "braty/br_gpu.h"
#include "braty/br_math.h"

typedef struct br_texture { uint32_t index; uint32_t generation; } br_texture;

/* Where a frame sits in its source (all in source pixels). Zero fields get defaults. */
typedef struct br_texture_frame {
    br_rect frame;           /* region of the source; for rotated frames w/h are the unrotated size */
    br_rect orig;            /* untrimmed size (x, y unused); zero = frame size */
    br_rect trim;            /* where the frame sits inside orig; zero size = not trimmed */
    bool    rotated;         /* stored rotated 90 degrees clockwise in the atlas (TexturePacker) */
    br_vec2 default_anchor;  /* anchor a new sprite takes */
} br_texture_frame;

/* Everything about a texture. */
typedef struct br_texture_info {
    float            width, height;          /* logical size (orig / resolution) */
    float            resolution;             /* source pixels per logical unit */
    uint32_t         source_width, source_height; /* pixels of the whole source */
    br_texture_frame frame;
    br_vec2          uvs[4];                 /* top-left, top-right, bottom-right, bottom-left */
    br_texture       source;                 /* the source texture (itself for sources) */
    br_texture_style style;
    bool             is_render_texture;
} br_texture_info;

/* Creates a source from straight-alpha RGBA8 pixels (premultiplied here). style NULL = default. */
br_texture br_texture_create_rgba8(br_engine *e, uint32_t width, uint32_t height, const void *pixels,
                                   const br_texture_style *style);
/* Loads (or returns the cached) texture asset at path. On failure logs and returns the
 * magenta missing texture, so the result is always drawable. */
br_texture br_texture_load(br_engine *e, const char *path);
/* Creates a frame texture over a source (atlas frames, spritesheets). */
br_texture br_texture_create_frame(br_engine *e, br_texture source, const br_texture_frame *frame);
/* Wraps an existing GPU texture (e.g. a 3D view's colour target). take_ownership: destroy it with the texture. */
br_texture br_texture_wrap_gpu(br_engine *e, br_gpu_texture *gpu, bool take_ownership);
/* Destroys a texture (frames of a destroyed source turn into the missing texture). */
void br_texture_destroy(br_engine *e, br_texture t);
/* True if the handle is live (and, for frames, the source too). */
bool br_texture_valid(br_engine *e, br_texture t);
/* Fills *out. False (and missing-texture info) for stale handles. */
bool br_texture_get_info(br_engine *e, br_texture t, br_texture_info *out);
/* Changes sampling (filter, address mode, mipmaps) of a source and all its frames. */
void br_texture_set_style(br_engine *e, br_texture t, const br_texture_style *style);
/* Sets source pixels per logical unit (default 1; 2 for @2x art). */
void br_texture_set_resolution(br_engine *e, br_texture t, float resolution);
/* The GPU texture of t's source (NULL headless or stale). Render textures: the current colour target. */
br_gpu_texture *br_texture_gpu(br_engine *e, br_texture t);
/* A 1x1 white texture (untextured geometry: draw with it and a vertex colour). */
br_texture br_texture_white(br_engine *e);
/* The 2x2 magenta fallback for missing/broken textures. */
br_texture br_texture_missing(br_engine *e);
/* Number of live textures (stats). */
uint32_t br_texture_count(br_engine *e);

/* ---- render textures (PixiJS RenderTexture): draw into them with br_render_to_texture */

#define BR_RENDER_TEXTURE_MAX_SIZE 16384 /* largest render texture edge in pixels */
/* Creates a render texture of width x height logical units at `resolution` pixels per unit
 * (<= 0 means 1). Has depth+stencil, so masks work inside it. Zero handle (logged) unless each
 * edge is 1..BR_RENDER_TEXTURE_MAX_SIZE pixels (NaN and negative sizes fail). */
br_texture br_render_texture_create(br_engine *e, float width, float height, float resolution);
/* Resizes a render texture (contents are lost). False for sizes create would refuse. */
bool br_render_texture_resize(br_engine *e, br_texture t, float width, float height);
/* The render target behind a render texture, NULL for other textures or headless. */
br_render_target *br_texture_render_target(br_engine *e, br_texture t);

/* Registers the png/jpg/jpeg/bmp/tga/gif/psd/hdr loader with the assets core (br_builtin does this). */
void br_texture_register_loader(br_engine *e);

#endif
