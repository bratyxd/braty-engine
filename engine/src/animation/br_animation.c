/* br_animation: module state, registration and shutdown of the unified animation system. */
#include "animation/br_animation_internal.h"

static void br_animation_shutdown(br_engine *e, void *user)
{
    br_animation_state *s = user;
    br_tween_free_all(e);
    br_animator_free_all(e);
    for (uint32_t i = 0; i < s->properties.count; i++) {
        br_anim_property_row *row = br_array_at(&s->properties, i);
        br_free((char *)row->desc.name);
    }
    br_array_free(&s->properties);
    br_array_free(&s->animators);
    br_pool_free(&s->handles);
    br_array_free(&s->by_node2d);
    br_array_free(&s->by_node3d);
    br_array_free(&s->events);
    br_array_free(&s->model_clips);
    br_array_free(&s->tweens);
    br_pool_free(&s->tween_handles);
    br_free(s);
    e->animation = NULL;
}

/* Scene-file components first, then tweens: animators (sampled after) override tweened values. */
static void br_animation_tween_system(br_engine *e, float dt, void *user)
{
    br_anim_components_system(e, dt, user);
    br_tween_system(e, dt, user);
}

void br_animation_register(br_engine *e)
{
    br_animation_state *s = br_alloc(sizeof(br_animation_state));
    if (s == NULL) {
        br_log_error("animation: out of memory");
        return;
    }
    br_array_init(&s->properties, sizeof(br_anim_property_row));
    br_array_init(&s->animators, sizeof(br_animator_data));
    br_pool_init(&s->handles, sizeof(uint32_t));
    br_array_init(&s->by_node2d, sizeof(uint32_t));
    br_array_init(&s->by_node3d, sizeof(uint32_t));
    br_array_init(&s->events, sizeof(br_anim_event));
    br_array_init(&s->model_clips, sizeof(br_anim_model_clips));
    br_array_init(&s->tweens, sizeof(br_tween_data));
    br_pool_init(&s->tween_handles, sizeof(uint32_t));
    e->animation = s;
    br_engine_on_shutdown(e, br_animation_shutdown, s);

    br_anim_property_register_builtins(e);
    br_anim_clip_register_loader(e);
    br_anim_graph_register_loader(e);
    br_anim_components_register(e);
    br_scene_on_node_destroy(e, br_animator_on_node_destroy, NULL);
    br_node3d_on_destroy(e, br_animator_on_node3d_destroy, NULL);
    br_system_add(e, "animation.tweens", BR_PHASE_LATE_UPDATE, -200, br_animation_tween_system, NULL);
    br_system_add(e, "animation.update", BR_PHASE_LATE_UPDATE, -100, br_animator_system, NULL);
}
