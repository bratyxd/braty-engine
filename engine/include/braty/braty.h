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

#endif
