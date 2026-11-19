/* br_spritesheet.h - spritesheets (PixiJS Spritesheet): TexturePacker JSON, hash and array
 * formats, trimmed and rotated frames, pivots (default anchors), 9-slice borders, animations,
 * multi-packs (meta.related_multi_packs).
 *
 * A spritesheet is an asset of type "spritesheet" (a .json file with "frames" and "meta").
 * Loading it loads its image (meta.image, relative to the json) as a texture asset and creates
 * one frame texture per frame. Unloading it destroys the frames and drops the image reference.
 *
 *   br_spritesheet *sheet = br_spritesheet_load(e, "atlas.json");
 *   br_node hero = br_sprite_create(e, br_spritesheet_frame(e, sheet, "hero.png"));
 *   const br_texture *walk = NULL;
 *   uint32_t n = br_spritesheet_animation(e, sheet, "walk", &walk);
 */
#ifndef BR_SPRITESHEET_H
#define BR_SPRITESHEET_H

#include "braty/br_render.h"

typedef struct br_spritesheet br_spritesheet;

/* Loads (or returns the cached) spritesheet and takes a reference. NULL on failure (logged).
 * Release with br_assets_unload(e, path). */
br_spritesheet *br_spritesheet_load(br_engine *e, const char *path);
/* The frame texture called `name`, searching linked multi-packs too. Zero handle if missing. */
br_texture br_spritesheet_frame(br_engine *e, const br_spritesheet *sheet, const char *name);
/* Number of frames of this sheet (linked packs not included). */
uint32_t br_spritesheet_frame_count(const br_spritesheet *sheet);
/* Name of frame `index` in file order, NULL if out of range. */
const char *br_spritesheet_frame_name(const br_spritesheet *sheet, uint32_t index);
/* 9-slice borders of a frame (left, top, right, bottom in source pixels). False if it has none. */
bool br_spritesheet_frame_borders(br_engine *e, const br_spritesheet *sheet, const char *name, float out[4]);
/* Frames of animation `name`: *frames points at `count` textures owned by the sheet. 0 if missing. */
uint32_t br_spritesheet_animation(br_engine *e, const br_spritesheet *sheet, const char *name,
                                  const br_texture **frames);
/* The atlas texture (source) of this sheet. */
br_texture br_spritesheet_texture(const br_spritesheet *sheet);

#endif
