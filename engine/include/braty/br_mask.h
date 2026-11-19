/* br_mask.h - alpha and colour masks (drawn by the filters module through the effects hook).
 *
 * Set them with br_node_set_mask (br_node.h):
 *   br_node_set_mask(e, n, mask_node, BR_MASK_ALPHA, inverse);
 *       n's subtree is multiplied by the mask's alpha (inverse: by 1 - alpha). Any node or
 *       subtree works as the mask: a soft sprite gives soft edges.
 *   br_node_set_mask(e, n, mask_node, BR_MASK_COLOR, inverse);
 *       each colour channel of n is multiplied by the mask's matching (premultiplied) channel,
 *       alpha by the strongest one: a grey mask is a luminance mask, a red mask keeps only red
 *       (inverse: 1 - mask).
 *
 * The mask node is not drawn as content. If it is not on the stage it is still rendered with its
 * own transform. Both are rendered into pooled textures over n's bounds, so the masked result
 * can also carry filters (masks apply after filters) and blend modes.
 * Scissor and stencil masks are the render module's (br_render.h).
 */
#ifndef BR_MASK_H
#define BR_MASK_H

#include "braty/br_render.h"

#endif
