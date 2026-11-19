/* test_pacing.c - FPS cap accuracy (headless, through the real app loop), fixed-timestep step
 * counts and alpha, vsync names and game.json keys. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

/* Runs `frames` capped iterations of the app loop and returns the average interval in ms. */
static double measure_cap(const char *fps, int frames, double *worst_ms)
{
    char *argv[] = { "test_pacing", "--headless", "--max-fps", (char *)fps };
    br_app_desc desc;
    SDL_zero(desc);
    void *state = NULL;
    BR_CHECK(br_app_init(&state, &desc, 4, argv) == SDL_APP_CONTINUE);
    br_app_iterate(state); /* the first frame starts the schedule */
    uint64_t start = SDL_GetTicksNS();
    uint64_t last = start;
    *worst_ms = 0.0;
    for (int i = 0; i < frames; i++) {
        br_app_iterate(state);
        uint64_t now = SDL_GetTicksNS();
        double ms = (double)(now - last) / 1e6;
        *worst_ms = ms > *worst_ms ? ms : *worst_ms;
        last = now;
    }
    double avg = (double)(last - start) / 1e6 / frames;
    br_app_quit(state, SDL_APP_SUCCESS);
    return avg;
}

static void test_fps_cap(void)
{
    const char *caps[] = { "60", "144" };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(caps); i++) {
        double target = 1000.0 / SDL_atof(caps[i]);
        double worst = 0.0;
        double avg = measure_cap(caps[i], 120, &worst);
        printf("cap %s fps: average interval %.4f ms (target %.4f, error %.3f%%), worst frame %.3f ms\n", caps[i],
               avg, target, 100.0 * (avg - target) / target, worst);
        BR_CHECK(avg > target * 0.98 && avg < target * 1.02);
    }
}

/* ---- fixed timestep */

typedef struct fixed_probe {
    int   calls;
    float last_dt;
    bool  dt_constant;
} fixed_probe;

static void fixed_system(br_engine *e, float dt, void *user)
{
    (void)e;
    fixed_probe *p = user;
    if (p->calls > 0 && dt != p->last_dt) {
        p->dt_constant = false;
    }
    p->calls++;
    p->last_dt = dt;
}

static void test_fixed_timestep(void)
{
    br_engine *e = br_engine_create("test_pacing", NULL);
    fixed_probe p = { 0, 0.0f, true };
    br_system_add(e, "fixed", BR_PHASE_FIXED_UPDATE, 0, fixed_system, &p);

    /* off: once per frame with the frame's dt */
    br_engine_step(e, 0.033f);
    BR_CHECK(p.calls == 1 && p.last_dt == 0.033f && br_app_fixed_steps(e) == 1);

    /* 50 Hz under 60 fps frames: 50 steps per simulated second, dt always 1/50 */
    br_app_set_fixed_timestep(e, 50.0f);
    p.calls = 0;
    p.dt_constant = true;
    uint32_t zero_frames = 0;
    for (int i = 0; i < 60; i++) {
        br_engine_step(e, 1.0f / 60.0f);
        zero_frames += br_app_fixed_steps(e) == 0 ? 1 : 0;
        BR_CHECK(br_app_fixed_alpha(e) >= 0.0f && br_app_fixed_alpha(e) < 1.0f);
    }
    BR_CHECK(p.calls == 50 && p.dt_constant && p.last_dt == 1.0f / 50.0f);
    BR_CHECK(zero_frames == 10); /* 0..N per frame: some frames have no step */

    /* 50 Hz frames at 50 Hz: exactly one step each despite float dt (0.0199999996) */
    p.calls = 0;
    br_app_set_fixed_timestep(e, 50.0f);
    for (int i = 0; i < 100; i++) {
        br_engine_step(e, 1.0f / 50.0f);
        BR_CHECK(br_app_fixed_steps(e) == 1);
    }
    BR_CHECK(p.calls == 100);

    /* a slow frame: several steps; a stall: at most 8, the backlog is dropped */
    p.calls = 0;
    br_engine_step(e, 0.1f);
    BR_CHECK(p.calls == 5 && br_app_fixed_steps(e) == 5);
    br_engine_step(e, 2.0f);
    BR_CHECK(br_app_fixed_steps(e) == 8 && br_app_fixed_alpha(e) < 1.0f);
    br_engine_step(e, 1.0f / 50.0f);
    BR_CHECK(br_app_fixed_steps(e) == 1); /* no spiral: back to normal right away */

    /* alpha is the leftover fraction */
    br_app_set_fixed_timestep(e, 10.0f);
    br_engine_step(e, 0.25f);
    BR_CHECK(br_app_fixed_steps(e) == 2 && SDL_fabsf(br_app_fixed_alpha(e) - 0.5f) < 1e-4f);
    br_engine_destroy(e);
}

static void test_vsync_settings(void)
{
    br_vsync mode = BR_VSYNC_ON;
    BR_CHECK(br_vsync_from_name("mailbox", &mode) && mode == BR_VSYNC_MAILBOX);
    BR_CHECK(br_vsync_from_name("off", &mode) && mode == BR_VSYNC_OFF);
    BR_CHECK(!br_vsync_from_name("fast", &mode) && mode == BR_VSYNC_OFF);
    BR_CHECK(SDL_strcmp(br_vsync_name(BR_VSYNC_ADAPTIVE), "adaptive") == 0);

    br_engine *e = br_engine_create("test_pacing", NULL);
    BR_CHECK(br_app_vsync(e) == BR_VSYNC_ON);
    BR_CHECK(br_app_set_vsync(e, BR_VSYNC_OFF) == BR_VSYNC_OFF); /* headless: remembered */
    br_app_set_max_fps(e, 144.0f);
    BR_CHECK(br_app_max_fps(e) == 144.0f);

    /* game.json: window.vsync as bool or name, ticker.max_fps and ticker.fixed_hz */
    br_game_config c;
    const char *json = "{ \"window\": { \"vsync\": \"mailbox\" }, \"ticker\": { \"max_fps\": 30, \"fixed_hz\": 50 } }";
    BR_CHECK(br_game_config_parse(json, SDL_strlen(json), "pacing.json", &c));
    BR_CHECK(c.vsync == BR_VSYNC_MAILBOX && c.max_fps == 30.0f && c.fixed_hz == 50.0f);
    br_game_config_apply(e, &c);
    BR_CHECK(br_app_vsync(e) == BR_VSYNC_MAILBOX && br_app_max_fps(e) == 30.0f && br_app_fixed_timestep(e) == 50.0f);
    const char *off = "{ \"window\": { \"vsync\": false } }";
    BR_CHECK(br_game_config_parse(off, SDL_strlen(off), "pacing.json", &c) && c.vsync == BR_VSYNC_OFF);
    br_game_config_free(&c);
    br_engine_destroy(e);
}

/* API_REVIEW item 21: the frame graph's "slow" line follows the pacing target. */
static void test_frame_budget(void)
{
    br_engine *e = br_engine_create("test_pacing", NULL);
    BR_CHECK(SDL_fabsf(br_app_frame_budget_ms(e) - 1000.0f / 60.0f) < 1e-3f); /* nothing known */
    br_app_set_max_fps(e, 144.0f);
    BR_CHECK(SDL_fabsf(br_app_frame_budget_ms(e) - 1000.0f / 144.0f) < 1e-3f);
    br_app_set_max_fps(e, 30.0f);
    BR_CHECK(SDL_fabsf(br_app_frame_budget_ms(e) - 1000.0f / 30.0f) < 1e-3f);
    br_engine_destroy(e);
}

int main(void)
{
    test_frame_budget();
    test_vsync_settings();
    test_fixed_timestep();
    test_fps_cap();
    return br_test_finish();
}
