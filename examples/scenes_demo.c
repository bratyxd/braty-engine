/* scenes_demo - first-class scenes and asset groups, all data in examples/scenes/:
 * menu -> (fade, loading scene with a progress bar) -> level A -> overlay loaded additively ->
 * (fade, loading scene) -> level B (3D from scene JSON) -> menu ...
 *
 * Each step is the "next" action (Enter / Space / pad A). Without --manual the demo injects it
 * itself, a fixed number of frames after the previous switch finished, so a deterministic run
 * (--deterministic, the golden image) always shows the same frame. The asset report is logged
 * after every scene load. */
#include "braty/braty_main.h"

typedef struct demo {
    bool     manual;   /* --manual: wait for real input */
    int      step;     /* 0 menu, 1 level A, 2 level A + overlay, 3 level B */
    uint32_t idle;     /* frames since the last switch finished */
} demo;

static demo g_demo;

/* Scene system of the loading scene: the bar follows the switch target's progress. */
static void progress_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_node root = br_scene_root_2d(e, br_scene_find(e, "loading"));
    float progress = br_scene_progress(e, br_scene_switch_target(e));
    br_node_set_scale(e, br_node_find_by_label(e, root, "bar", true), br_vec2_make(progress, 1.0f));
    char text[32];
    SDL_snprintf(text, sizeof(text), "LOADING %d%%", (int)(progress * 100.0f + 0.5f));
    br_text_set_text(e, br_node_find_by_label(e, root, "loading_text", true), text);
}

static void on_scene_event(br_engine *e, br_scene scene, br_scene_event event, void *user)
{
    (void)user;
    if (event != BR_SCENE_EVENT_LOADED) {
        return;
    }
    if (SDL_strcmp(br_scene_name(e, scene), "loading") == 0) {
        /* runs only while the loading scene exists; the name is reused by the next one */
        br_system_add_for_scene(e, scene, "demo.progress", BR_PHASE_UPDATE, 0, progress_system, NULL);
    }
    br_log_info("scenes_demo: frame %llu, scene '%s' loaded (%u scenes)", (unsigned long long)br_ticker_frame(e),
                br_scene_name(e, scene), br_scene_count(e));
    br_assets_report(e);
}

/* INPUT, before the latch: presses "next" 40 frames after each switch settles. */
static void autoplay_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo *d = user;
    if (d->manual || br_scene_switching(e)) {
        d->idle = 0;
        return;
    }
    d->idle++;
    if (d->idle == 40) {
        br_input_inject_action(e, "next", 1.0f, 1);
    }
}

static void flow_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo *d = user;
    if (!br_input_action_pressed(e, "next") || br_scene_switching(e)) {
        return;
    }
    d->idle = 0;
    d->step = (d->step + 1) % 4;
    if (d->step == 1) {
        br_scene_switch(e, "level_a", BR_TRANSITION_FADE, 0.2f);
    } else if (d->step == 2) {
        br_scene_load(e, "overlay", BR_SCENE_ADDITIVE); /* on top of level A, level A stays active */
    } else if (d->step == 3) {
        br_scene_switch(e, "level_b", BR_TRANSITION_FADE, 0.2f); /* unloads level A and the overlay */
    } else {
        br_scene_switch(e, "menu", BR_TRANSITION_FADE, 0.2f);
    }
}

static bool demo_init(br_engine *e)
{
    demo *d = &g_demo;
    int argc = 0;
    char **argv = br_engine_args(e, &argc);
    for (int i = 1; i < argc; i++) {
        d->manual = d->manual || SDL_strcmp(argv[i], "--manual") == 0;
    }
    br_scene_on_event(e, on_scene_event, d);
    br_scene_set_loading_scene(e, "loading");
    br_system_add(e, "demo.autoplay", BR_PHASE_INPUT, BR_INPUT_PRIORITY_LATCH - 1000, autoplay_system, d);
    br_system_add(e, "demo.flow", BR_PHASE_UPDATE, 0, flow_system, d);
    return true; /* game.json's start_scene ("menu") loads as a br_scene after init */
}

br_app_desc br_main(int argc, char **argv)
{
    br_app_desc desc = br_game_config_main(BRATY_EXAMPLES_DIR "/scenes/game.json", argc, argv, demo_init);
    desc.asset_root = BRATY_EXAMPLES_DIR;
    return desc;
}
