/* br_camera.h - Unity-style cameras for 2D and 3D (Phase 2 Task 15).
 *
 * A camera is a 2D node of type "camera" holding a br_camera (edit it through br_camera_get).
 *   - 2D camera (br_camera_create): its world position / rotation (a node like any other, usually
 *     under the stage) is the centre / roll of what it shows of the stage.
 *   - 3D camera (br_camera_create_3d): the same data drives a camera3d node inside a 3D scene and
 *     a View3D the module owns. Place it through br_camera_node3d (a rig node; the camera3d is
 *     its child, so shake does not fight your placement).
 *
 * Unity semantics: orthographic_size is HALF the visible vertical extent, field_of_view is
 * vertical; the horizontal extent follows the aspect ratio (fit policies change that).
 * 2D units: node coordinates ("stage units"). pixels_per_unit says how many stage units one
 * world unit is, so a 2D camera shows 2 * orthographic_size * pixels_per_unit stage units
 * vertically. Screen coordinates are window coordinates (top-left origin, y down), the space
 * of br_input and of the stage without cameras.
 *
 * With no camera the stage is drawn exactly as before (Pixi-style, window coordinates). As soon
 * as an enabled 2D camera exists, the stage is drawn only through cameras, each in its viewport,
 * ordered by depth (render-texture cameras first). 3D cameras alone keep the stage as a
 * screen-space overlay drawn after them.
 */
#ifndef BR_CAMERA_H
#define BR_CAMERA_H

#include "braty/br_app.h"
#include "braty/br_view3d.h"

typedef enum br_camera_projection {
    BR_CAMERA_ORTHOGRAPHIC,
    BR_CAMERA_PERSPECTIVE     /* 3D cameras only: 2D cameras are always orthographic */
} br_camera_projection;

typedef enum br_camera_clear {
    BR_CAMERA_CLEAR_COLOR,    /* fill the viewport with background_color */
    BR_CAMERA_CLEAR_DEPTH,    /* keep what is under (3D: depth is always cleared) */
    BR_CAMERA_CLEAR_NOTHING
} br_camera_clear;

/* How the visible extent follows the viewport's aspect ratio. "design" = the rect of height
 * 2 * size and width 2 * size * reference_aspect. */
typedef enum br_camera_fit {
    BR_CAMERA_FIT_HEIGHT,     /* vertical extent fixed, horizontal follows (Unity, the default) */
    BR_CAMERA_FIT_WIDTH,      /* horizontal extent fixed (design width), vertical follows */
    BR_CAMERA_FIT_EXPAND,     /* the whole design rect always visible, more shown on one axis */
    BR_CAMERA_FIT_LETTERBOX   /* exactly the design rect; the viewport shrinks (bars) */
} br_camera_fit;

/* Camera settings (plain data: edit in place through br_camera_get). */
typedef struct br_camera {
    br_camera_projection projection;
    float           orthographic_size;  /* half the visible vertical extent, world units */
    float           field_of_view;      /* perspective: vertical, radians */
    float           near_z;             /* 3D; <= 0 = 0.1 */
    float           far_z;              /* 3D; <= near_z (e.g. 0 from SDL_zero) = near_z + 1000 */
    br_rect         viewport_rect;      /* normalised (0..1) part of the screen/target, top-left origin */
    float           depth;              /* draw order: lower first */
    uint32_t        culling_mask;       /* layer bits drawn (br_node_set_layer_bits / br_node3d_set_layer_bits) */
    br_camera_clear clear_flags;
    br_color        background_color;   /* sRGB, straight alpha */
    br_texture      target_texture;     /* zero = the window; else a br_render_texture_create texture */
    float           aspect;             /* 0 = the viewport's width / height */
    float           pixels_per_unit;    /* 2D: stage units per world unit (default 1) */
    bool            pixel_perfect;      /* 2D: whole screen pixels per stage unit, position snapped */
    br_camera_fit   fit;
    float           reference_aspect;   /* design aspect for FIT_WIDTH / EXPAND / LETTERBOX (16/9) */
    bool            enabled;
} br_camera;

/* 2D defaults: orthographic, size 360 (720 stage units tall), ppu 1, full screen, clear black. */
br_camera br_camera_default_2d(void);
/* 3D defaults: perspective 60 degrees, near 0.1, far 500, full screen, clear colour. */
br_camera br_camera_default_3d(void);

/* What a camera shows on a screen of a given size (all pure maths, see br_camera_compute_view). */
typedef struct br_camera_view {
    bool    is_3d;
    br_vec2 screen_size;        /* target size in screen units */
    br_rect viewport;           /* screen units (letterbox already applied) */
    float   half_width;         /* visible half extents: 2D stage units, 3D ortho world units, */
    float   half_height;        /* perspective: tan of the half angles */
    br_mat3 screen_from_world;  /* 2D: stage -> screen */
    br_mat3 world_from_screen;  /* 2D: screen -> stage */
    br_rect visible;            /* 2D: stage-space box of the viewport (rotation included) */
    br_mat4 view;               /* world -> camera */
    br_mat4 projection;         /* camera -> clip of the viewport */
    br_mat4 clip_from_world;    /* world -> clip of the whole screen */
    br_mat4 world_from_clip;
    br_vec3 position;
    br_vec3 forward;
} br_camera_view;

/* ---- pure maths (no engine; tests use these) */

/* Visible half extents for a fit policy. base = orthographic_size (* ppu in 2D) or tan(fov/2);
 * aspect = viewport aspect (or override). */
br_vec2 br_camera_fit_extents(br_camera_fit fit, float base, float aspect, float reference_aspect);
/* Computes the view of `camera` placed at `world` (2D: br_mat4_from_mat3 of the node's world
 * matrix; 3D: the camera3d node's world matrix) on a screen of screen_size units.
 * pixel_density = pixels per screen unit (pixel-perfect 2D). */
br_camera_view br_camera_compute_view(const br_camera *camera, br_mat4 world, bool is_3d, br_vec2 screen_size,
                                      float pixel_density);
/* Screen point (z = distance in front of a 3D camera, ignored in 2D) -> world point. */
br_vec3 br_camera_view_screen_to_world(const br_camera_view *view, br_vec3 screen);
/* World point -> screen point (z = distance in front of a 3D camera, negative behind; 0 in 2D). */
br_vec3 br_camera_view_world_to_screen(const br_camera_view *view, br_vec3 world);
/* Ray through a screen point (3D: from the near plane; 2D: from the stage point along +z). */
br_ray3d br_camera_view_screen_to_ray(const br_camera_view *view, br_vec2 screen);
/* Viewport point (0..1 inside the camera's viewport, top-left; z as above) -> world point. */
br_vec3 br_camera_view_viewport_to_world(const br_camera_view *view, br_vec3 viewport);

/* ---- cameras in an engine */

/* Registers the module (camera node type, helper components and systems). Called by br_builtin.c. */
void br_camera_register(br_engine *e);
/* Type id of "camera". */
uint32_t br_camera_type(br_engine *e);
/* A 2D camera node (desc NULL = br_camera_default_2d). Add it under the stage and move it. */
br_node br_camera_create(br_engine *e, const br_camera *desc);
/* A 3D camera showing scene_root's subtree (e.g. br_view3d_root of a view you built the scene
 * in). Returns its camera node (desc NULL = br_camera_default_3d). */
br_node br_camera_create_3d(br_engine *e, br_node3d scene_root, const br_camera *desc);
/* A letterboxed 2D camera showing the stage rect (0, 0, width, height) on any window: a fixed
 * logical resolution (API_REVIEW item 17). Added under the stage. */
br_node br_camera_create_logical(br_engine *e, float width, float height);
/* The settings, NULL if cam is not a live camera. The pointer dies when cameras are created. */
br_camera *br_camera_get(br_engine *e, br_node cam);
/* True for 3D cameras. */
bool br_camera_is_3d(br_engine *e, br_node cam);
/* 3D: the rig node to place and aim (br_node3d_set_position / _look_at). Zero for 2D cameras. */
br_node3d br_camera_node3d(br_engine *e, br_node cam);
/* 3D: the View3D the camera renders through (environment, post, MSAA). Zero for 2D cameras. */
br_node br_camera_view3d(br_engine *e, br_node cam);
/* The camera's view on the current window (or its target texture). */
br_camera_view br_camera_get_view(br_engine *e, br_node cam);
/* Screen -> world (z = distance from a 3D camera). */
br_vec3 br_camera_screen_to_world(br_engine *e, br_node cam, br_vec3 screen);
/* World -> screen (z = distance from a 3D camera). */
br_vec3 br_camera_world_to_screen(br_engine *e, br_node cam, br_vec3 world);
/* Ray through a screen point. */
br_ray3d br_camera_screen_point_to_ray(br_engine *e, br_node cam, br_vec2 screen);
/* Viewport (0..1, top-left; z = distance) -> world. */
br_vec3 br_camera_viewport_to_world(br_engine *e, br_node cam, br_vec3 viewport);

/* Screen size cameras assume when there is no window (headless tests and tools; 0 = none). */
void br_camera_set_headless_screen(br_engine *e, float width, float height);

/* ---- pointer input with cameras (used by events and play-testing) */

/* Window point -> stage point: through the top-most 2D camera under the point that shows a
 * node there (falls back to the top-most camera under the point). Identity without cameras. */
br_vec2 br_camera_screen_to_stage(br_engine *e, br_vec2 screen);
/* Stage point -> window point through the top-most 2D camera that draws n (zero n = any) and
 * shows the point. Identity without cameras. */
br_vec2 br_camera_stage_to_screen(br_engine *e, br_node n, br_vec2 stage);
/* The window's safe area in screen units (SDL_GetWindowSafeArea; the whole window if none). */
br_rect br_camera_safe_area(br_engine *e);

/* ---- helper components (data + one system each; add them to a camera node) */

/* "camera.follow": move toward a target. 2D: target node; 3D: target3d (C only). */
typedef struct br_camera_follow {
    br_node   target;
    br_vec2   offset;      /* 2D: added to the target position */
    float     damping;     /* seconds to close ~63% of the gap; 0 = snap */
    br_vec2   dead_zone;   /* 2D: half size of the box the target may move in without the camera moving */
    br_rect   bounds;      /* 2D: the camera's visible rect stays inside (when use_bounds) */
    bool      use_bounds;
    br_node3d target3d;    /* 3D: rig follows target3d + offset3d and looks at the target */
    br_vec3   offset3d;
} br_camera_follow;

/* "camera.shake": trauma-based shake (offset and roll grow with trauma squared). */
typedef struct br_camera_shake {
    float    trauma;       /* 0..1, add with br_camera_shake_add */
    float    decay;        /* trauma lost per second (default 1) */
    float    max_offset;   /* at trauma 1: world units on a 3D camera (default 0.3), stage units in 2D (default 12) */
    float    max_angle;    /* radians at trauma 1 */
    float    frequency;    /* noise samples per second (default 15) */
    uint32_t seed;         /* 0 = drawn from the engine RNG on first use (deterministic) */
    float    time;
} br_camera_shake;

/* "camera.orbit": left-drag turns a 3D camera around target, the wheel zooms (PixiJS 3D
 * OrbitCameraControl). Fill it with br_camera_orbit_make: zeroed limits and speeds do not move. */
typedef struct br_camera_orbit {
    br_vec3 target;
    float   distance;
    float   yaw;            /* radians around +Y */
    float   pitch;          /* radians, positive looks down */
    float   min_distance;
    float   max_distance;
    float   rotate_speed;   /* radians per mouse pixel (default 0.01) */
    float   zoom_speed;     /* fraction of distance per wheel step (default 0.1) */
} br_camera_orbit;

/* "camera.anchor": keeps a UI node at a point of a camera's viewport (or of the screen). */
typedef struct br_camera_anchor {
    br_node camera;        /* zero = the screen without a camera (stage = window coordinates) */
    br_vec2 anchor;        /* 0..1 of the viewport (0,0 top-left, 1,1 bottom-right) */
    br_vec2 offset;        /* stage units added after anchoring */
    bool    safe_area;     /* anchor inside the window's safe area */
} br_camera_anchor;

/* Component id of "camera.follow". */
uint32_t br_camera_follow_component(br_engine *e);
/* Component id of "camera.shake". */
uint32_t br_camera_shake_component(br_engine *e);
/* Component id of "camera.orbit" (data: br_camera_orbit, drives a 3D camera's rig). */
uint32_t br_camera_orbit_component(br_engine *e);
/* Component id of "camera.anchor". */
uint32_t br_camera_anchor_component(br_engine *e);
/* An orbit at distance around target with default limits (0.5..500) and speeds. */
br_camera_orbit br_camera_orbit_make(br_vec3 target, float distance, float yaw, float pitch);
/* Adds trauma (clamped to 1) to a camera, adding a shake component with defaults if missing. */
void br_camera_shake_add(br_engine *e, br_node cam, float trauma);

/* Pure follow step: the new camera position for a target, with dead zone and damping. */
br_vec2 br_camera_follow_step(br_vec2 camera, br_vec2 target, br_vec2 dead_zone, float damping, float dt);
/* Pure: camera position moved so a view of half extents `half` stays inside bounds (centred if
 * the bounds are smaller than the view). */
br_vec2 br_camera_clamp_to_bounds(br_vec2 camera, br_vec2 half, br_rect bounds);
/* Pure: shake offset (x, y) and roll (z) for the current trauma and time. */
br_vec3 br_camera_shake_sample(const br_camera_shake *shake);
/* Pure: the screen point an anchor sits at: inside `area` (a viewport), or inside its
 * intersection with `safe` when use_safe. */
br_vec2 br_camera_anchor_point(br_rect area, br_rect safe, br_vec2 anchor, bool use_safe);

#endif
