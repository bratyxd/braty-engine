/* br_anim_components: the "animator" and "tween" components, so scene files and glTF node extras
 * can start animation through the data layer's component fields (no data-layer code needed):
 *
 *   "components": { "animator": { "clips": "hero.anim.json", "play": "walk" } }
 *   "components": { "tween": { "property": "alpha", "from": 0, "to": 1, "mode": "from_to", "duration": 0.5 } }
 *   glTF node extras: { "animator.clips": "door.anim.json", "animator.play": "open" }
 *
 * Each row (2D or 3D node) is read once, the first frame after it appears, and turned into a real
 * animator or tween. The animator keeps the asset references the row took and releases them when
 * it is removed. */
#include "animation/br_animation_internal.h"

typedef struct br_anim_animator_config {
    char  clips[BR_FIELD_STRING_SIZE];   /* *.anim.json */
    char  graph[BR_FIELD_STRING_SIZE];   /* *.animgraph.json (optional) */
    char  play[BR_FIELD_STRING_SIZE];    /* clip to play (optional) */
    char  loop[BR_FIELD_STRING_SIZE];    /* loop / once / ping_pong / clamp (optional) */
    float speed;                         /* 0 = 1 */
    bool  unscaled;                      /* br_animator_set_unscaled */
    bool  started;                       /* not a field: set once handled */
} br_anim_animator_config;

typedef struct br_anim_tween_config {
    char     property[BR_FIELD_STRING_SIZE];
    br_vec2  to;                         /* float, vec2, bool and int properties */
    br_vec2  from;
    br_vec3  to3;                        /* vec3 properties (3D position, scale) */
    br_vec3  from3;
    br_color to_color;                   /* colour properties (tint) use these */
    br_color from_color;
    char     mode[BR_FIELD_STRING_SIZE]; /* "to" (default), "from", "from_to" */
    float    duration;
    float    delay;
    char     ease[BR_FIELD_STRING_SIZE];
    int32_t  repeat;
    bool     yoyo;
    bool     unscaled;                   /* br_tween_set_unscaled */
    bool     started;
} br_anim_tween_config;


void br_anim_components_register(br_engine *e)
{
    static const br_field_desc animator_fields[] = {
        { "clips", BR_FIELD_ASSET, offsetof(br_anim_animator_config, clips) },
        { "graph", BR_FIELD_ASSET, offsetof(br_anim_animator_config, graph) },
        { "play", BR_FIELD_STRING, offsetof(br_anim_animator_config, play) },
        { "loop", BR_FIELD_STRING, offsetof(br_anim_animator_config, loop) },
        { "speed", BR_FIELD_FLOAT, offsetof(br_anim_animator_config, speed) },
        { "unscaled", BR_FIELD_BOOL, offsetof(br_anim_animator_config, unscaled) },
    };
    static const br_field_desc tween_fields[] = {
        { "property", BR_FIELD_STRING, offsetof(br_anim_tween_config, property) },
        { "to", BR_FIELD_VEC2, offsetof(br_anim_tween_config, to) },
        { "from", BR_FIELD_VEC2, offsetof(br_anim_tween_config, from) },
        { "to3", BR_FIELD_VEC3, offsetof(br_anim_tween_config, to3) },
        { "from3", BR_FIELD_VEC3, offsetof(br_anim_tween_config, from3) },
        { "to_color", BR_FIELD_COLOR, offsetof(br_anim_tween_config, to_color) },
        { "from_color", BR_FIELD_COLOR, offsetof(br_anim_tween_config, from_color) },
        { "mode", BR_FIELD_STRING, offsetof(br_anim_tween_config, mode) },
        { "duration", BR_FIELD_FLOAT, offsetof(br_anim_tween_config, duration) },
        { "delay", BR_FIELD_FLOAT, offsetof(br_anim_tween_config, delay) },
        { "ease", BR_FIELD_STRING, offsetof(br_anim_tween_config, ease) },
        { "repeat", BR_FIELD_INT, offsetof(br_anim_tween_config, repeat) },
        { "yoyo", BR_FIELD_BOOL, offsetof(br_anim_tween_config, yoyo) },
        { "unscaled", BR_FIELD_BOOL, offsetof(br_anim_tween_config, unscaled) },
    };
    br_component_desc a = { "animator", sizeof(br_anim_animator_config), animator_fields, BR_ARRAY_COUNT(animator_fields) };
    br_component_desc t = { "tween", sizeof(br_anim_tween_config), tween_fields, BR_ARRAY_COUNT(tween_fields) };
    e->animation->animator_component = br_component_register(e, &a);
    e->animation->tween_component = br_component_register(e, &t);
}

static void br_anim_start_animator(br_engine *e, br_anim_target root, const br_anim_animator_config *c)
{
    br_animator a = br_animator_add(e, root);
    if (a.generation == 0) {
        return;
    }
    if (c->clips[0] != '\0') {
        const br_anim_library *lib = br_anim_library_load(e, c->clips);
        if (lib != NULL) {
            br_animator_add_library(e, a, lib);
            br_animator_own_asset(e, a, c->clips);
        }
    }
    br_animator_set_speed(e, a, c->speed != 0.0f ? c->speed : 1.0f);
    br_animator_set_unscaled(e, a, c->unscaled);
    if (c->graph[0] != '\0') {
        const br_anim_graph *g = br_anim_graph_load(e, c->graph);
        if (g != NULL) {
            br_animator_own_asset(e, a, c->graph);
            br_animator_set_graph(e, a, g);
        }
    }
    if (c->play[0] != '\0') {
        br_animator_play(e, a, c->play);
    }
    br_anim_loop loop;
    if (c->loop[0] != '\0') {
        if (br_anim_loop_from_name(c->loop, &loop)) {
            br_animator_set_loop(e, a, 0, loop);
        } else {
            br_log_error("animator component: loop '%s' is not loop, once, ping_pong or clamp", c->loop);
        }
    }
}

static void br_anim_start_tween(br_engine *e, br_anim_target target, const br_anim_tween_config *c)
{
    br_anim_target node;
    br_anim_prop_ref prop;
    if (!br_anim_resolve(e, target, c->property, &node, &prop)) {
        br_log_error("tween component: '%s' does not resolve on this node", c->property);
        return;
    }
    float to[BR_ANIM_MAX_COMPONENTS] = { 0 };
    float from[BR_ANIM_MAX_COMPONENTS] = { 0 };
    switch (prop.type) {
    case BR_ANIM_COLOR:
        to[0] = c->to_color.r;
        to[1] = c->to_color.g;
        to[2] = c->to_color.b;
        to[3] = c->to_color.a;
        from[0] = c->from_color.r;
        from[1] = c->from_color.g;
        from[2] = c->from_color.b;
        from[3] = c->from_color.a;
        break;
    case BR_ANIM_VEC3:
        to[0] = c->to3.x;
        to[1] = c->to3.y;
        to[2] = c->to3.z;
        from[0] = c->from3.x;
        from[1] = c->from3.y;
        from[2] = c->from3.z;
        break;
    case BR_ANIM_FLOAT:
    case BR_ANIM_VEC2:
    case BR_ANIM_BOOL:
    case BR_ANIM_INT:
        to[0] = c->to.x;
        to[1] = c->to.y;
        from[0] = c->from.x;
        from[1] = c->from.y;
        break;
    default:
        br_log_error("tween component: '%s' (a rotation, weights or texture) cannot be tweened from a component",
                     c->property);
        return;
    }
    br_ease_type ease = BR_EASE_LINEAR;
    br_tween t;
    if (SDL_strcmp(c->mode, "from") == 0) {
        t = br_tween_from(e, target, c->property, from, c->duration, ease);
    } else if (SDL_strcmp(c->mode, "from_to") == 0) {
        t = br_tween_from_to(e, target, c->property, from, to, c->duration, ease);
    } else {
        if (c->mode[0] != '\0' && SDL_strcmp(c->mode, "to") != 0) {
            br_log_error("tween component: mode '%s' is not to, from or from_to", c->mode);
        }
        t = br_tween_to(e, target, c->property, to, c->duration, ease);
    }
    if (c->ease[0] != '\0') {
        br_tween_set_ease_name(e, t, c->ease);
    }
    br_tween_set_delay(e, t, c->delay);
    br_tween_set_repeat(e, t, c->repeat, c->yoyo);
    br_tween_set_unscaled(e, t, c->unscaled);
}

void br_anim_components_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_animation_state *s = e->animation;
    /* copies: loading assets or starting animators may move the component arrays */
    uint32_t count = br_component_count(e, s->animator_component);
    for (uint32_t i = 0; i < count; i++) {
        br_anim_animator_config *c = br_component_at(e, s->animator_component, i);
        br_node n = br_component_owner_at(e, s->animator_component, i);
        if (!c->started && br_node_valid(e, n)) {
            c->started = true;
            br_anim_animator_config copy = *c;
            br_anim_start_animator(e, br_anim_target_2d(n), &copy);
        }
    }
    count = br_component3d_count(e, s->animator_component);
    for (uint32_t i = 0; i < count; i++) {
        br_anim_animator_config *c = br_component3d_at(e, s->animator_component, i);
        br_node3d n = br_component3d_owner_at(e, s->animator_component, i);
        if (!c->started && br_node3d_valid(e, n)) {
            c->started = true;
            br_anim_animator_config copy = *c;
            br_anim_start_animator(e, br_anim_target_3d(n), &copy);
        }
    }
    count = br_component_count(e, s->tween_component);
    for (uint32_t i = 0; i < count; i++) {
        br_anim_tween_config *c = br_component_at(e, s->tween_component, i);
        br_node n = br_component_owner_at(e, s->tween_component, i);
        if (!c->started && br_node_valid(e, n)) {
            c->started = true;
            br_anim_tween_config copy = *c;
            br_anim_start_tween(e, br_anim_target_2d(n), &copy);
        }
    }
    count = br_component3d_count(e, s->tween_component);
    for (uint32_t i = 0; i < count; i++) {
        br_anim_tween_config *c = br_component3d_at(e, s->tween_component, i);
        br_node3d n = br_component3d_owner_at(e, s->tween_component, i);
        if (!c->started && br_node3d_valid(e, n)) {
            c->started = true;
            br_anim_tween_config copy = *c;
            br_anim_start_tween(e, br_anim_target_3d(n), &copy);
        }
    }
}
