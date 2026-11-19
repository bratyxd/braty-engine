/* br_sprites_internal.h - state of the sprite-variants module. Engine-private. */
#ifndef BR_SPRITES_INTERNAL_H
#define BR_SPRITES_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_sprites.h"

/* The animation component on an animated sprite (dense: the animate system iterates it). */
typedef struct br_anim_state {
    br_anim_frame *frames;        /* owned through br_sprites_state.frames_by_node */
    uint32_t       count;
    uint32_t       current;
    float          time;          /* seconds into the current frame */
    float          speed;
    bool           loop;
    bool           playing;
    bool           update_anchor;
    br_anim_fn     on_complete;
    br_anim_fn     on_frame_change;
    br_anim_fn     on_loop;
    void          *user;
} br_anim_state;

typedef struct br_sprites_state {
    uint32_t nine_slice_type;
    uint32_t tiling_type;
    uint32_t anim_component;
    br_array frames_by_node;      /* br_anim_frame * per node index (freed when the node dies) */
    br_array scratch;             /* br_vertex2d, reused by nine-slice rendering */
} br_sprites_state;

/* br_nine_slice.c: registers the "nine_slice" node type. */
void br_nine_slice_register(br_engine *e);
/* br_tiling_sprite.c: registers the "tiling_sprite" node type. */
void br_tiling_sprite_register(br_engine *e);
/* br_animated_sprite.c: registers the animation component, its system and the gif loader. */
void br_animated_sprite_register(br_engine *e);
/* br_animated_sprite.c: frees frames still owned at shutdown. */
void br_animated_sprite_shutdown(br_engine *e);

#endif
