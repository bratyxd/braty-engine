/* data_demo - game.json and data/demo.scene.json build everything; C adds one component and one
 * system. Run with --debug to open the debug overlay (F1 toggles it). */
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity; /* units per second, bounces off the window sides */
    float   spin;     /* radians per second */
} mover;

static br_game_config config; /* br_main's desc points into it */

static void mover_system(br_engine *e, float dt, void *user)
{
    (void)user;
    uint32_t id = br_component_find(e, "mover");
    for (uint32_t i = 0; i < br_component_count(e, id); i++) {
        mover *m = br_component_at(e, id, i);
        br_node n = br_component_owner_at(e, id, i);
        br_vec2 p = br_node_position(e, n);
        if ((p.x < 40.0f && m->velocity.x < 0.0f) || (p.x > 1240.0f && m->velocity.x > 0.0f)) {
            m->velocity.x = -m->velocity.x;
        }
        br_node_set_position(e, n, p.x + m->velocity.x * dt, p.y + m->velocity.y * dt);
        br_node_set_rotation(e, n, br_node_rotation(e, n) + m->spin * dt);
    }
}

static bool demo_init(br_engine *e)
{
    static const br_field_desc fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
        { "spin", BR_FIELD_FLOAT, offsetof(mover, spin) },
    };
    br_component_desc desc = { "mover", sizeof(mover), fields, BR_ARRAY_COUNT(fields) };
    br_component_register(e, &desc);
    br_system_add(e, "demo.mover", BR_PHASE_UPDATE, 0, mover_system, NULL);
    return br_node_valid(e, br_game_config_apply(e, &config)); /* loads the start scene */
}

br_app_desc br_main(int argc, char **argv)
{
    br_game_config_load(NULL, "../../examples/data/game.json", &config); /* relative to the exe */
    br_game_config_args(&config, argc, argv);                             /* --debug, --scene */
    br_app_desc desc = br_game_config_app_desc(&config);
    desc.init = demo_init;
    return desc;
}
