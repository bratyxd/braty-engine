/* br_sprite.h - the sprite node type (PixiJS Sprite): a textured quad with an anchor.
 *
 * Tint, alpha, blend mode and transform are node fields (br_node.h). JSON type name: "sprite".
 */
#ifndef BR_SPRITE_H
#define BR_SPRITE_H

#include "braty/br_node.h"
#include "braty/br_texture.h"

/* Per-node data of a sprite (br_node_data(e, n, br_sprite_type(e))). */
typedef struct br_sprite {
    br_texture texture;
    br_vec2    anchor;        /* 0,0 = top-left at the position, 0.5,0.5 = centred */
    bool       round_pixels;  /* snap vertices to whole pixels */
} br_sprite;

/* Type id of sprites. */
uint32_t br_sprite_type(br_engine *e);
/* Creates a detached sprite showing `texture` (anchor = the texture's default anchor). */
br_node br_sprite_create(br_engine *e, br_texture texture);
/* Changes the texture (anchor kept). */
void br_sprite_set_texture(br_engine *e, br_node sprite, br_texture texture);
/* The texture (zero handle if not a sprite). */
br_texture br_sprite_texture(br_engine *e, br_node sprite);
/* Sets the anchor (fraction of the texture size). */
void br_sprite_set_anchor(br_engine *e, br_node sprite, float x, float y);
/* The anchor. */
br_vec2 br_sprite_anchor(br_engine *e, br_node sprite);
/* Sets width and height in local units by changing the scale (Pixi sprite.width/height). */
void br_sprite_set_size(br_engine *e, br_node sprite, float width, float height);
/* Displayed size: texture size times |scale|. */
br_vec2 br_sprite_size(br_engine *e, br_node sprite);
/* Snaps vertices to whole pixels (crisp pixel art). */
void br_sprite_set_round_pixels(br_engine *e, br_node sprite, bool round_pixels);

#endif
