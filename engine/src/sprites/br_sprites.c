/* br_sprites: module registration for nine-slice, tiling and animated sprites. */
#include "sprites/br_sprites_internal.h"

static void br_sprites_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_animated_sprite_shutdown(e);
    br_array_free(&e->sprites->frames_by_node);
    br_array_free(&e->sprites->scratch);
    br_free(e->sprites);
    e->sprites = NULL;
}

void br_sprites_register(br_engine *e)
{
    e->sprites = br_alloc(sizeof(br_sprites_state));
    if (e->sprites == NULL) {
        return;
    }
    br_array_init(&e->sprites->frames_by_node, sizeof(br_anim_frame *));
    br_array_init(&e->sprites->scratch, sizeof(br_vertex2d));
    br_engine_on_shutdown(e, br_sprites_shutdown, NULL);
    br_nine_slice_register(e);
    br_tiling_sprite_register(e);
    br_animated_sprite_register(e);
}
