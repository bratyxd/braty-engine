/* test_app.c - the app glue (br_app_init / iterate / quit) run headless, boot options. */
#include "braty/braty.h"
#include "br_test.h"

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

int main(void)
{
    test_user_and_args();
    return br_test_finish();
}
