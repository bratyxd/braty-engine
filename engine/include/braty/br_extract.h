/* br_extract.h - read pixels back and render nodes into new textures (Pixi ExtractSystem,
 * GenerateTextureSystem).
 *
 * br_generate_texture queues the render: the texture is filled during this frame's render
 * (after the stage), so it shows its content from the next frame on. br_extract_* read a
 * texture back from the GPU (blocking); call them after the frame that filled it, e.g. from an
 * UPDATE system one frame later. Headless engines have no pixels: extract returns false.
 */
#ifndef BR_EXTRACT_H
#define BR_EXTRACT_H

#include "braty/br_render.h"

typedef struct br_generate_texture_options {
    br_rect  region;       /* world rect to capture; zero size = the node's bounds */
    float    resolution;   /* texture pixels per world unit; <= 0 = window pixel density */
    br_color clear_color;  /* background (default transparent) */
} br_generate_texture_options;

/* Creates a render texture holding n's subtree (options may be NULL). Destroy it with
 * br_texture_destroy. Zero handle if the region is empty. */
br_texture br_generate_texture(br_engine *e, br_node n, const br_generate_texture_options *options);
/* Reads texture t (its frame, for atlas frames) as straight-alpha RGBA8, top row first.
 * Free *rgba with br_free. False headless or on GPU errors. */
bool br_extract_pixels(br_engine *e, br_texture t, uint8_t **rgba, uint32_t *width, uint32_t *height);
/* Writes texture t as a PNG file (path as for br_file_write_all). */
bool br_extract_image_png(br_engine *e, br_texture t, const char *path);

#endif
