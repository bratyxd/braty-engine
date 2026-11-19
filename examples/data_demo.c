/* data_demo - game.json and data/demo.scene.json build everything; C adds one component and one
 * system. Run with --debug to open the debug overlay (F1 toggles it). */
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity; /* units per second, bounces off the window sides */
    float   spin;     /* radians per second */
} mover;

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
    return true; /* game.json was applied before init; its start scene loads after it */
}

br_app_desc br_main(int argc, char **argv)
{
    /* BRATY_EXAMPLES_DIR (set by CMake) is the source tree, so any build directory works */
    br_app_desc desc = br_game_config_main(BRATY_EXAMPLES_DIR "/data/game.json", argc, argv, demo_init);
    desc.asset_root = BRATY_EXAMPLES_DIR;
    return desc;
}
