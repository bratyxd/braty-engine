/* br_light3d.h - 3D lights (directional, point, spot), hemisphere ambient, shadows.
 *
 * A light is a 3D node of type "light3d": its world position and its -Z axis (aim it with
 * br_node3d_look_at) place it. Per view and frame the first BR_LIGHT3D_MAX visible lights are
 * uploaded as plain arrays in the fragment uniform block; extra lights are ignored.
 *
 * Shadows: the first directional light with cast_shadows gets 1-4 cascades (splits from
 * br_shadow3d_cascade_splits); up to BR_SHADOW3D_MAX_SPOT spot lights with cast_shadows get a
 * perspective shadow map each. All shadow maps are tiles of one atlas per view (4 x 2 tiles,
 * br_view3d_set_shadow_resolution), sampled with 3x3 PCF. Point lights do not cast shadows.
 * Ambient light is the view's environment (br_view3d_set_environment).
 */
#ifndef BR_LIGHT3D_H
#define BR_LIGHT3D_H

#include "braty/br_view3d.h"

/* Lights uploaded per view (directional + point + spot). */
#define BR_LIGHT3D_MAX 16
/* Cascades of the directional shadow. */
#define BR_SHADOW3D_MAX_CASCADES 4
/* Spot lights with shadow maps per view. */
#define BR_SHADOW3D_MAX_SPOT 4

typedef enum br_light3d_kind {
    BR_LIGHT3D_DIRECTIONAL,
    BR_LIGHT3D_POINT,
    BR_LIGHT3D_SPOT
} br_light3d_kind;

/* Data of a "light3d" node: edit fields through br_light3d_get. */
typedef struct br_light3d {
    br_light3d_kind kind;
    br_color        color;              /* linear */
    float           intensity;          /* multiplies color (lux-like for directional, any unit) */
    float           range;              /* point/spot: light reaches 0 here (0 = infinite) */
    float           inner_angle;        /* spot: full intensity inside (half angle, radians) */
    float           outer_angle;        /* spot: zero outside (half angle, radians) */
    bool            cast_shadows;
    float           shadow_bias;        /* depth bias, world units along the light (default 0.05) */
    float           shadow_normal_bias; /* receiver offset along its normal, world units (default 0.04) */
    uint32_t        cascade_count;      /* directional: 1..4 */
    float           shadow_distance;    /* directional: shadows end here (camera distance) */
    float           cascade_lambda;     /* 0 = uniform splits, 1 = logarithmic (default 0.75) */
} br_light3d;

/* Sensible defaults for a kind (white, intensity 1 or 3, 3 cascades over 60 units). */
br_light3d br_light3d_default(br_light3d_kind kind);
/* Type id of "light3d". */
uint32_t br_light3d_type(br_engine *e);
/* Creates a detached light node. */
br_node3d br_light3d_create(br_engine *e, const br_light3d *light);
/* The light data, NULL if n is not a live light3d. */
/* Pointer rule (br_core.h): valid until the next create or destroy of this kind of object. */
br_light3d *br_light3d_get(br_engine *e, br_node3d n);
/* The spot cone as the shader gets it: (cos outer, cos inner), the inner cosine kept at least
 * 1e-4 above the outer one (the shader's smoothstep divides by their difference: equal angles
 * gave NaN), non-finite angles replaced by the defaults. */
br_vec2 br_light3d_spot_cone(const br_light3d *light);
/* Far distance of each cascade (practical split scheme: lambda blends logarithmic and uniform). */
void br_shadow3d_cascade_splits(float near_z, float far_z, uint32_t count, float lambda, float *out_far);

#endif
