/* br_sprites.h - sprite variants: NineSliceSprite, TilingSprite, AnimatedSprite (+ GIF frames).
 *
 * Nine-slice ("nine_slice" node type): a texture cut into 3x3 cells by four independent
 * borders. Corners keep their size, edges and centre stretch or tile to fill width x height.
 * When width/height are smaller than the two borders, the borders shrink proportionally.
 * Works with atlas frames (trimmed and rotated). Tint/alpha/blend come from the node.
 *
 * Tiling ("tiling_sprite"): a texture repeated over width x height with a tile transform
 * (position, scale, rotation). Any texture works, atlas frames included (a shader wraps
 * inside the frame), so no REPEAT address mode is needed.
 *
 * Animated sprites are ordinary sprites ("sprite" type, so every br_sprite_* call works) with
 * an animation component; one system ("sprites.animated", UPDATE 1000) advances all of them.
 * Frames are textures with per-frame durations in seconds. The "gif" asset loader decodes a
 * GIF into a br_frame_set (note: br_texture_load on a .gif therefore fails; load it as frames).
 */
#ifndef BR_SPRITES_H
#define BR_SPRITES_H

#include "braty/br_render.h"
#include "braty/br_sprite.h"

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_sprites_register(br_engine *e);

/* ---- nine-slice sprite (PixiJS NineSliceSprite) */

/* How an edge or the centre fills its cell. */
typedef enum br_nine_slice_fill {
    BR_NINE_SLICE_STRETCH,   /* scale the source cell to the target cell */
    BR_NINE_SLICE_TILE       /* repeat the source cell at border scale; the last tile is cut */
} br_nine_slice_fill;

/* Per-node data of a nine-slice sprite (edit through br_nine_slice_get). */
typedef struct br_nine_slice {
    br_texture         texture;
    float              left, top, right, bottom;  /* borders in texture units (untrimmed frame) */
    float              width, height;             /* drawn size in local units */
    br_vec2            anchor;                    /* fraction of width/height at the position */
    br_nine_slice_fill edge_fill;                 /* the four edges */
    br_nine_slice_fill center_fill;
    bool               hide_center;               /* draw only the frame (borders) */
} br_nine_slice;

/* Type id of nine-slice sprites. */
uint32_t br_nine_slice_type(br_engine *e);
/* Creates a nine-slice sprite with the given borders, sized like the texture. */
br_node br_nine_slice_create(br_engine *e, br_texture texture, float left, float top, float right, float bottom);
/* The data to read or change in place (NULL if not a nine-slice). Pointer dies when nine-slices are created/freed. */
br_nine_slice *br_nine_slice_get(br_engine *e, br_node n);
/* Sets the drawn size in local units. */
void br_nine_slice_set_size(br_engine *e, br_node n, float width, float height);
/* Sets the four borders (texture units). */
void br_nine_slice_set_borders(br_engine *e, br_node n, float left, float top, float right, float bottom);
/* Appends the geometry (4 br_vertex2d per quad: TL, TR, BR, BL; local space, white) to
 * out (elem_size sizeof(br_vertex2d)). Returns the number of quads. Pure: used by render and tests. */
uint32_t br_nine_slice_geometry(const br_nine_slice *ns, const br_texture_info *info, br_array *out);

/* ---- tiling sprite (PixiJS TilingSprite) */

/* Per-node data of a tiling sprite (edit through br_tiling_sprite_get). */
typedef struct br_tiling_sprite {
    br_texture texture;
    float      width, height;   /* drawn size in local units */
    br_vec2    anchor;
    br_vec2    tile_position;   /* offset of the pattern in local units */
    br_vec2    tile_scale;      /* default 1,1 */
    float      tile_rotation;   /* radians */
    float      clamp_margin;    /* texels kept away from the frame edge (default 0.5: no bleeding) */
} br_tiling_sprite;

/* Type id of tiling sprites. */
uint32_t br_tiling_sprite_type(br_engine *e);
/* Creates a tiling sprite of width x height showing `texture` repeated. */
br_node br_tiling_sprite_create(br_engine *e, br_texture texture, float width, float height);
/* The data to read or change in place (NULL if not a tiling sprite). */
br_tiling_sprite *br_tiling_sprite_get(br_engine *e, br_node n);

/* ---- animated sprite (PixiJS AnimatedSprite) */

/* One frame: a texture shown for `duration` seconds. */
typedef struct br_anim_frame {
    br_texture texture;
    float      duration;
} br_anim_frame;

/* Animation callback (on_complete, on_frame_change, on_loop). */
typedef void (*br_anim_fn)(br_engine *e, br_node sprite, void *user);

/* Creates a stopped sprite showing frames[0]. Frames are copied (textures must outlive it). */
br_node br_animated_sprite_create(br_engine *e, const br_anim_frame *frames, uint32_t count);
/* Same, every frame lasting 1/fps seconds. */
br_node br_animated_sprite_create_uniform(br_engine *e, const br_texture *textures, uint32_t count, float fps);
/* Replaces the frames (copied) and goes to frame 0. */
bool br_animated_sprite_set_frames(br_engine *e, br_node n, const br_anim_frame *frames, uint32_t count);
/* True if n is an animated sprite. */
bool br_animated_sprite_is(br_engine *e, br_node n);
/* Starts playing from the current frame. */
void br_animated_sprite_play(br_engine *e, br_node n);
/* Stops on the current frame. */
void br_animated_sprite_stop(br_engine *e, br_node n);
/* Jumps to a frame and plays. */
void br_animated_sprite_goto_and_play(br_engine *e, br_node n, uint32_t frame);
/* Jumps to a frame and stops. */
void br_animated_sprite_goto_and_stop(br_engine *e, br_node n, uint32_t frame);
/* Playback rate (1 = durations as given, 2 = twice as fast, negative = backwards). */
void br_animated_sprite_set_speed(br_engine *e, br_node n, float speed);
/* Loop (default true) or stop on the last frame and fire on_complete. */
void br_animated_sprite_set_loop(br_engine *e, br_node n, bool loop);
/* Apply each frame texture's default anchor when the frame changes (default false). */
void br_animated_sprite_set_update_anchor(br_engine *e, br_node n, bool update_anchor);
/* Sets the callbacks (any may be NULL). They run inside the animate system. */
void br_animated_sprite_set_callbacks(br_engine *e, br_node n, br_anim_fn on_complete, br_anim_fn on_frame_change,
                                      br_anim_fn on_loop, void *user);
/* Index of the frame shown. */
uint32_t br_animated_sprite_current_frame(br_engine *e, br_node n);
/* Number of frames. */
uint32_t br_animated_sprite_total_frames(br_engine *e, br_node n);
/* True while playing. */
bool br_animated_sprite_playing(br_engine *e, br_node n);

/* ---- frame sets (GIF asset) */

/* Decoded animation frames; owns its textures. The "gif" loader's asset type is "frames". */
typedef struct br_frame_set {
    br_anim_frame *frames;
    uint32_t       count;
    uint32_t       width, height;   /* pixels of one frame */
} br_frame_set;

/* Loads (or returns the cached) GIF as frames. NULL on failure (logged). Unload with br_assets_unload. */
const br_frame_set *br_frame_set_load(br_engine *e, const char *path);

#endif
