/* braty.h - the whole engine API in one include.
 *
 * Games usually include braty/braty_main.h (which includes this) in their main file and
 * braty/braty.h everywhere else. Every module header is listed here once; module tasks fill
 * their own header and never edit this file.
 */
#ifndef BRATY_H
#define BRATY_H

/* foundation (Task 1) */
#include "braty/br_core.h"
#include "braty/br_math.h"
#include "braty/br_gpu.h"
#include "braty/br_app.h"

/* scene graph, 2D render core, assets core (Task 2) */
#include "braty/br_node.h"
#include "braty/br_texture.h"
#include "braty/br_render.h"
#include "braty/br_sprite.h"
#include "braty/br_assets.h"

/* graphics (Task 3) */
#include "braty/br_graphics.h"

/* text (Task 4) */
#include "braty/br_text.h"
#include "braty/br_font.h"
#include "braty/br_bitmap_font.h"
#include "braty/br_rich_text.h"

/* assets: spritesheets (Task 5) */
#include "braty/br_spritesheet.h"

/* sprite variants, meshes, particles (Task 6) */
#include "braty/br_sprites.h"
#include "braty/br_mesh.h"
#include "braty/br_particles.h"

/* input, events, audio (Task 7) */
#include "braty/br_input.h"
#include "braty/br_events.h"
#include "braty/br_audio.h"

/* filters, masks, extract (Task 8) */
#include "braty/br_filter.h"
#include "braty/br_mask.h"
#include "braty/br_extract.h"

/* 3D (Task 9) */
#include "braty/br_view3d.h"
#include "braty/br_mesh3d.h"
#include "braty/br_material3d.h"
#include "braty/br_light3d.h"
#include "braty/br_camera3d.h"
#include "braty/br_post3d.h"

/* scene files, prefabs, game config, debug overlay (Task 10) */
#include "braty/br_scene_file.h"
#include "braty/br_prefab.h"
#include "braty/br_game_config.h"
#include "braty/br_debug.h"

/* glTF and 3D animation (Task 11) */
#include "braty/br_gltf.h"
#include "braty/br_anim3d.h"

#endif
