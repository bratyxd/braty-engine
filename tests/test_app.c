/* test_app.c - the app glue (br_app_init / iterate / quit) run headless, boot options. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

typedef struct boot_probe {
    int      init_calls;
    void    *seen_user;
    int      seen_argc;
    bool     saw_flag;
} boot_probe;

static boot_probe s_probe;

static bool probe_init(br_engine *e)
{
    s_probe.init_calls++;
    s_probe.seen_user = br_engine_app_user(e);
    char **argv = br_engine_args(e, &s_probe.seen_argc);
    for (int i = 0; i < s_probe.seen_argc; i++) {
        s_probe.saw_flag = s_probe.saw_flag || SDL_strcmp(argv[i], "--my-flag") == 0;
    }
    return true;
}

/* API_REVIEW item 1: init gets the user pointer and the command line. */
static void test_user_and_args(void)
{
    int marker = 42;
    char *argv[] = { "test_app", "--headless", "--my-flag", "--frames", "3" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.init = probe_init;
    desc.user = &marker;
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, (int)BR_ARRAY_COUNT(argv), argv) == SDL_APP_CONTINUE);
    BR_CHECK(s_probe.init_calls == 1);
    BR_CHECK(s_probe.seen_user == &marker);
    BR_CHECK(s_probe.seen_argc == 5);
    BR_CHECK(s_probe.saw_flag);
    br_engine *e = state;
    BR_CHECK(br_engine_window(e) == NULL); /* --headless */
    SDL_AppResult r = SDL_APP_CONTINUE;
    int iterations = 0;
    while (r == SDL_APP_CONTINUE && iterations < 10) {
        r = br_app_iterate(state);
        iterations++;
    }
    BR_CHECK(r == SDL_APP_SUCCESS && iterations == 3); /* --frames 3, headless */
    br_app_quit(state, r);

    /* engines made without the app have neither */
    br_engine *plain = br_engine_create("test_app", NULL);
    int argc = -1;
    BR_CHECK(br_engine_args(plain, &argc) == NULL && argc == 0);
    BR_CHECK(br_engine_app_user(plain) == NULL);
    br_engine_destroy(plain);
}

/* ---- API_REVIEW item 2: br_game_config_main keeps the config; apply before init, scene after */

typedef struct probe_component {
    int32_t value;
} probe_component;

typedef struct config_probe {
    bool     json_seen_in_init;
    uint32_t component_id;
} config_probe;

static config_probe s_config;

static bool config_init(br_engine *e)
{
    s_config.json_seen_in_init = br_game_config_json(e) != NULL; /* applied before init */
    static const br_field_desc fields[] = { { "value", BR_FIELD_INT, offsetof(probe_component, value) } };
    br_component_desc desc = { "probe", sizeof(probe_component), fields, 1 };
    s_config.component_id = br_component_register(e, &desc);
    return true;
}

static bool write_next_to_exe(const char *name, const char *text)
{
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%s", SDL_GetBasePath(), name);
    return SDL_SaveFile(path, text, SDL_strlen(text));
}

static void test_game_config_main(void)
{
    BR_CHECK(write_next_to_exe("test_app_game.json",
                               "{ \"window\": { \"title\": \"boot test\" }, \"start_scene\": \"test_app_scene.json\","
                               "  \"game\": { \"level\": 3 } }"));
    /* the scene uses a component that only exists once init ran */
    BR_CHECK(write_next_to_exe("test_app_scene.json",
                               "{ \"type\": \"container\", \"children\": [ { \"type\": \"container\","
                               "  \"label\": \"booted\", \"components\": { \"probe\": { \"value\": 7 } } } ] }"));
    char *argv[] = { "test_app", "--headless" };
    br_app_desc desc = br_game_config_main("test_app_game.json", 2, argv, config_init);
    BR_CHECK(desc.config != NULL && SDL_strcmp(desc.title, "boot test") == 0);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    br_engine *e = state;
    BR_CHECK(s_config.json_seen_in_init);
    br_node booted = br_node_find_by_label(e, br_scene_root(e), "booted", true);
    probe_component *p = br_component_get(e, booted, s_config.component_id);
    BR_CHECK(p != NULL && p->value == 7);
    const cJSON *level = cJSON_GetObjectItem(cJSON_GetObjectItem(br_game_config_json(e), "game"), "level");
    BR_CHECK(cJSON_IsNumber(level) && level->valueint == 3);
    br_app_quit(state, SDL_APP_SUCCESS);
}

/* API_REVIEW item 4: --assets / BRATY_ASSETS point the asset root anywhere (cwd-relative). */
static void test_assets_override(void)
{
    char *cwd = SDL_GetCurrentDirectory();
    char expected[1024];
    SDL_snprintf(expected, sizeof(expected), "%sdata", cwd);
    char *argv[] = { "test_app", "--headless", "--assets", "data" };
    br_app_desc desc;
    SDL_zero(desc);
    desc.asset_root = "somewhere/else";
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 4, argv) == SDL_APP_CONTINUE);
    BR_CHECK(SDL_strcmp(br_engine_asset_root(state), expected) == 0);
    br_app_quit(state, SDL_APP_SUCCESS);

    SDL_setenv_unsafe("BRATY_ASSETS", "data", 1);
    state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 2, argv) == SDL_APP_CONTINUE);
    BR_CHECK(SDL_strcmp(br_engine_asset_root(state), expected) == 0);
    br_app_quit(state, SDL_APP_SUCCESS);
    SDL_unsetenv_unsafe("BRATY_ASSETS");
    SDL_free(cwd);
}

int main(void)
{
    test_assets_override();
    test_user_and_args();
    test_game_config_main();
    return br_test_finish();
}
