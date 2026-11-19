/* br_camera_internal.h - private state of the camera module. */
#ifndef BR_CAMERA_INTERNAL_H
#define BR_CAMERA_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/br_camera.h"
#include "braty/br_camera3d.h"
#include "braty/br_material3d.h"

/* Data of a "camera" node (dense pool: moves when cameras are created or freed). */
typedef struct br_camera_data {
    br_camera camera;
    bool      is_3d;
    br_node3d rig;       /* 3D: placed by the game */
    br_node3d node3d;    /* 3D: camera3d child of the rig (shake moves it) */
    br_node   view3d;    /* 3D: detached View3D drawn into the viewport */
    br_vec3   shake;     /* this frame's shake: x, y offset and roll */
} br_camera_data;

/* A camera enabled this frame, with its view. */
typedef struct br_camera_slot {
    br_node        camera;
    bool           is_3d;
    bool           on_texture;
    float          depth;
    uint32_t       culling_mask;
    br_camera_view view;
} br_camera_slot;

typedef struct br_camera_state {
    uint32_t type;
    uint32_t follow_id;
    uint32_t shake_id;
    uint32_t orbit_id;
    uint32_t anchor_id;
    br_array cameras;      /* br_node, creation order */
    br_array slots;        /* br_camera_slot: this frame's cameras in draw order, then the screen 2D ones again */
    uint32_t stage_first;  /* the run of screen 2D cameras (pointer mapping), bottom to top */
    uint32_t stage_count;
    br_array views;        /* br_render_view scratch */
    bool     had_views;
    br_vec2  headless_size; /* screen size without a window (br_camera_set_headless_screen) */
} br_camera_state;

#endif
