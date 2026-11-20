/* br_camera3d.h - 3D camera nodes, projection maths, rays, frustums.
 *
 * An orbiting mouse camera is the "camera.orbit" component on a br_camera (br_camera.h).
 *
 * Unity-style projection: the VERTICAL extent is what you set (vertical field of view, or the
 * orthographic size = half the visible height); the horizontal extent follows the aspect ratio.
 * A camera is a 3D node of type "camera3d" looking down its local -Z.
 *
 * The maths below is pure (no engine) so tests and other modules can use it.
 */
#ifndef BR_CAMERA3D_H
#define BR_CAMERA3D_H

#include "braty/br_view3d.h"

typedef enum br_projection3d {
    BR_PROJECTION_PERSPECTIVE,
    BR_PROJECTION_ORTHOGRAPHIC
} br_projection3d;

/* Data of a "camera3d" node: edit fields through br_camera3d_get. */
typedef struct br_camera3d {
    br_projection3d projection;
    float           fov_y;        /* perspective: vertical field of view, radians */
    float           ortho_size;   /* orthographic: half the visible height, world units */
    float           near_z;
    float           far_z;
    float           aspect;       /* 0 = the view's width / height */
} br_camera3d;

/* A perspective camera description (fov_y in radians). */
br_camera3d br_camera3d_perspective(float fov_y, float near_z, float far_z);
/* An orthographic camera description (size = half the visible height). */
br_camera3d br_camera3d_orthographic(float size, float near_z, float far_z);
/* Type id of "camera3d". */
uint32_t br_camera3d_type(br_engine *e);
/* Creates a detached camera node. */
br_node3d br_camera3d_create(br_engine *e, const br_camera3d *camera);
/* The camera data, NULL if n is not a live camera3d. */
br_camera3d *br_camera3d_get(br_engine *e, br_node3d n);
/* Projection matrix for an aspect ratio (width / height; camera->aspect overrides when > 0). */
br_mat4 br_camera3d_projection(const br_camera3d *camera, float aspect);

/* ---- pure geometry helpers */

/* Frustums, ray vs box and ray vs triangle are in br_math.h: br_frustum_planes +
 * br_frustum_planes_aabb, br_ray3d_box, br_ray_triangle. */
/* An empty box (min = +inf, max = -inf), ready for br_aabb_extend. */
br_aabb br_aabb_empty(void);
/* Grows a box to contain p. */
br_aabb br_aabb_extend(br_aabb box, br_vec3 p);
/* Smallest box containing both. */
br_aabb br_aabb_union(br_aabb a, br_aabb b);
/* True if min <= max on every axis. */
bool br_aabb_is_valid(br_aabb box);
/* Box of the 8 transformed corners. */
br_aabb br_aabb_transform(br_aabb box, br_mat4 m);
/* World ray through normalised device coordinates (x, y in -1..1, y up). */
br_ray3d br_ray3d_from_ndc(br_mat4 inverse_view_projection, float ndc_x, float ndc_y);

#endif
