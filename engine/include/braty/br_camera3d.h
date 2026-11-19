/* br_camera3d.h - 3D cameras, projection maths, rays, frustums, orbit camera.
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

/* Six planes (xyz = inward normal, w = distance): left, right, bottom, top, near, far. */
typedef struct br_frustum { br_vec4 planes[6]; } br_frustum;

/* Frustum of a view-projection matrix (depth 0..1 clip space). */
br_frustum br_frustum_from_matrix(br_mat4 view_projection);
/* False only if the box is fully outside one plane (conservative). */
bool br_frustum_test_aabb(const br_frustum *f, br_aabb box);
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
/* Ray vs triangle (both sides). t = ray parameter; u, v barycentrics of b and c (may be NULL). */
bool br_ray3d_triangle(br_ray3d ray, br_vec3 a, br_vec3 b, br_vec3 c, float *t, float *u, float *v);
/* Ray vs box; t = entry parameter (0 if the origin is inside). */
bool br_ray3d_aabb(br_ray3d ray, br_aabb box, float *t);

/* ---- orbit camera (PixiJS 3D OrbitCameraControl) */

typedef struct br_orbit_camera {
    br_vec3 target;
    float   distance;
    float   yaw;            /* radians around +Y */
    float   pitch;          /* radians, positive looks down */
    float   min_distance;
    float   max_distance;
    float   rotate_speed;   /* radians per mouse pixel (default 0.01) */
    float   zoom_speed;     /* fraction of distance per wheel step (default 0.1) */
} br_orbit_camera;

/* An orbit at distance around target with default limits and speeds. */
br_orbit_camera br_orbit_camera_make(br_vec3 target, float distance, float yaw, float pitch);
/* Places and aims the camera node from the orbit values. */
void br_orbit_camera_apply(br_engine *e, const br_orbit_camera *orbit, br_node3d camera);
/* Left-drag rotates, wheel zooms (reads br_input), then applies. Call from an UPDATE system. */
void br_orbit_camera_update(br_engine *e, br_orbit_camera *orbit, br_node3d camera);

#endif
