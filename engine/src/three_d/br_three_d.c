/* br_three_d: module registration, shutdown and per-frame systems of the 3D module. */
#include "three_d/br_three_d_internal.h"

/* INPUT, right after the 2D scene flush: frees 3D nodes destroyed last frame (including roots
 * of views destroyed by that flush). */
static void br_three_d_flush_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_node3d_flush(e);
}

static void br_three_d_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_three_d_state *s = e->three_d;
    if (s == NULL) {
        return;
    }
    br_node3d_table_free(e);
    br_array_free(&s->listeners);
    br_geometry3d_free_all(e);
    br_material3d_free_all(e);
    br_array_free(&s->views);
    br_occlusion3d_free(e);
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        br_gpu_buffer_destroy(g, s->dummy_storage);
    }
    br_free(s);
    e->three_d = NULL;
}

void br_three_d_register(br_engine *e)
{
    br_three_d_state *s = br_alloc(sizeof(br_three_d_state));
    if (s == NULL) {
        br_log_error("out of memory registering the 3D module");
        return;
    }
    e->three_d = s;
    br_array_init(&s->listeners, sizeof(br_listener3d));
    br_array_init(&s->views, sizeof(br_view3d_state *));
    br_pool_init(&s->geometries, sizeof(br_geometry3d_data));
    br_pool_init(&s->materials, sizeof(br_material3d_desc));
    br_node3d_table_init(e);
    br_node_types3d_register(e);
    br_view3d_register_type(e);
    br_engine_on_shutdown(e, br_three_d_shutdown, NULL);
    br_system_add(e, "three_d.flush_destroyed", BR_PHASE_INPUT, -999000, br_three_d_flush_system, NULL);
}
