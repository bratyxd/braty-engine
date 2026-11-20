/* test_profiler.c - scope nesting and self/total maths with injected timestamps, ring
 * wrap-around, the verdict rule for every verdict and its smoothing, the trace file (valid JSON,
 * balanced events), budgets (one warning per breach), the summary JSON, overhead at 200 scopes,
 * and the debug overlay taking clicks that land on its windows. Headless. */
#include "braty/braty.h"
#include "br_test.h"

#include "cjson/cJSON.h"

#include <SDL3/SDL.h>

/* ---- the verdict rule */

static br_profiler_verdict_input input(float interval, float update, float render)
{
    br_profiler_verdict_input in;
    SDL_zero(in);
    in.interval_ms = interval;
    in.update_ms = update;
    in.render_ms = render;
    return in;
}

static void test_verdict_rule(void)
{
    br_profiler_verdict_input in = input(0.0f, 0.0f, 0.0f);
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_NONE);

    in = input(26.0f, 25.0f, 0.8f); /* --cpu-load 25 */
    in.gpu_ms = 0.5f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_CPU_UPDATE);

    in = input(33.0f, 0.2f, 32.0f); /* many draw calls */
    in.gpu_ms = 15.0f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_CPU_RENDER);

    in = input(36.0f, 0.4f, 1.7f); /* --gpu-load: the fence estimate says so */
    in.acquire_ms = 33.5f;
    in.gpu_ms = 36.0f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_GPU);
    in.gpu_ms = 0.0f; /* no fence numbers: the CPU waiting on the GPU is enough */
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_GPU);
    in = input(0.36f, 0.01f, 0.19f); /* thousands of FPS: a fence estimate within its resolution */
    in.gpu_ms = 0.3f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_CPU_RENDER);

    in = input(16.7f, 2.0f, 1.0f); /* vsync at 60 Hz with time to spare */
    in.acquire_ms = 13.0f;
    in.refresh_ms = 16.67f;
    in.gpu_ms = 16.0f; /* inflated by display waits: ignored under vsync */
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_VSYNC);
    in = input(33.3f, 20.0f, 1.0f); /* vsync, but the CPU misses the refresh */
    in.acquire_ms = 12.0f;
    in.refresh_ms = 16.67f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_CPU_UPDATE);

    in = input(33.3f, 2.0f, 1.0f); /* capped at 30 FPS */
    in.pace_ms = 30.0f;
    in.cap_ms = 33.33f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_FPS_CAP);
    in = input(50.0f, 48.0f, 1.0f); /* a cap the game cannot reach */
    in.cap_ms = 33.33f;
    BR_CHECK(br_profiler_verdict_rule(&in) == BR_PROFILER_VERDICT_CPU_UPDATE);
    for (int v = 0; v < BR_PROFILER_VERDICT_COUNT; v++) {
        BR_CHECK(SDL_strlen(br_profiler_verdict_name((br_profiler_verdict)v)) > 0);
    }
}

#ifndef BRATY_PROFILER_OFF

/* ---- an injected clock: time moves only when a test system says so */

typedef struct fake_clock {
    uint64_t now;
} fake_clock;

static uint64_t fake_now(void *user)
{
    return ((fake_clock *)user)->now;
}

typedef struct probe {
    fake_clock *clock;
    uint64_t    update_ns;  /* advanced inside an UPDATE system */
    uint64_t    render_ns;  /* advanced inside a PRE_RENDER system */
    bool        nested;
} probe;

static void probe_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    probe *p = user;
    if (p->nested) {
        /* outer 7 ms = 1 + inner 2 + inner 3 + 1 */
        br_profile_begin(e, "outer");
        p->clock->now += 1000000;
        br_profile_begin(e, "inner");
        p->clock->now += 2000000;
        br_profile_end(e);
        br_profile_begin(e, "inner");
        p->clock->now += 3000000;
        br_profile_end(e);
        p->clock->now += 1000000;
        br_profile_end(e);
    }
    p->clock->now += p->update_ns;
}

static void probe_render(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    probe *p = user;
    p->clock->now += p->render_ns;
}

static br_engine *make_engine(fake_clock *clock, probe *p)
{
    br_engine *e = br_engine_create("test_profiler", NULL);
    br_profiler_set_clock(e, fake_now, clock);
    p->clock = clock;
    br_system_add(e, "probe.update", BR_PHASE_UPDATE, 0, probe_update, p);
    br_system_add(e, "probe.render", BR_PHASE_PRE_RENDER, 0, probe_render, p);
    return e;
}

static bool near(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-3f;
}

static void test_nesting(void)
{
    fake_clock clock = { 1000 };
    probe p;
    SDL_zero(p);
    p.nested = true;
    br_engine *e = make_engine(&clock, &p);
    br_engine_step(e, 1.0f / 60.0f);
    float total, self;
    uint32_t calls;
    BR_CHECK(br_profiler_scope_time(e, 0, "outer", &total, &self, &calls));
    BR_CHECK(near(total, 7.0f) && near(self, 2.0f) && calls == 1);
    BR_CHECK(br_profiler_scope_time(e, 0, "inner", &total, &self, &calls));
    BR_CHECK(near(total, 5.0f) && near(self, 5.0f) && calls == 2);
    BR_CHECK(br_profiler_scope_time(e, 0, "probe.update", &total, &self, &calls));
    BR_CHECK(near(total, 7.0f) && near(self, 0.0f) && calls == 1);
    /* the engine's top-level step contains the system */
    BR_CHECK(br_profiler_scope_time(e, 0, "update", &total, &self, &calls));
    BR_CHECK(near(total, 7.0f) && calls == 1);
    BR_CHECK(!br_profiler_scope_time(e, 0, "never ran", &total, &self, &calls));
    br_profiler_frame_info info;
    BR_CHECK(br_profiler_get_frame(e, 0, &info));
    BR_CHECK(near(info.wall_ms, 7.0f) && near(info.update_ms, 7.0f) && near(info.cpu_ms, 7.0f));
    BR_CHECK(info.frame_ms == 0.0f); /* the newest frame's interval is not known yet */
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_profiler_get_frame(e, 1, &info) && near(info.frame_ms, 7.0f));

    /* an extra end is ignored (warned), a missing end is closed at frame end */
    br_profiler_frame_begin(e); /* outside br_engine_step: a frame of its own */
    br_profile_end(e);
    br_profile_begin(e, "left open");
    clock.now += 500000;
    br_profiler_frame_end(e);
    BR_CHECK(br_profiler_scope_time(e, 0, "left open", &total, &self, &calls) && near(total, 0.5f));
    br_engine_destroy(e);
}

static void test_ring(void)
{
    fake_clock clock = { 1000 };
    probe p;
    SDL_zero(p);
    p.update_ns = 2000000;
    br_engine *e = make_engine(&clock, &p);
    for (int i = 0; i < BR_PROFILER_FRAMES + 50; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    BR_CHECK(br_profiler_frame_count(e) == BR_PROFILER_FRAMES);
    br_profiler_frame_info info;
    BR_CHECK(br_profiler_get_frame(e, 0, &info) && info.number == BR_PROFILER_FRAMES + 49);
    BR_CHECK(br_profiler_get_frame(e, BR_PROFILER_FRAMES - 1, &info) && info.number == 50);
    BR_CHECK(!br_profiler_get_frame(e, BR_PROFILER_FRAMES, &info));
    /* the scopes of the oldest kept frame were not overwritten by the wrap */
    float total, self;
    uint32_t calls;
    BR_CHECK(br_profiler_scope_time(e, BR_PROFILER_FRAMES - 1, "probe.update", &total, &self, &calls));
    BR_CHECK(near(total, 2.0f));
    /* pausing keeps the ring as it is */
    br_profiler_set_paused(e, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_profiler_get_frame(e, 0, &info) && info.number == BR_PROFILER_FRAMES + 49);
    br_profiler_set_paused(e, false);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_profiler_get_frame(e, 0, &info) && info.number == BR_PROFILER_FRAMES + 50);
    BR_CHECK(br_profiler_get_frame(e, 1, &info) && info.frame_ms == 0.0f); /* no interval across a pause */
    br_engine_destroy(e);
}

/* The shown verdict follows the window average and ignores short spikes. */
static void test_verdict_smoothing(void)
{
    fake_clock clock = { 1000 };
    probe p;
    SDL_zero(p);
    p.update_ns = 10000000;
    br_engine *e = make_engine(&clock, &p);
    br_profiler_summary_info sum;
    for (int i = 0; i < 40; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    br_profiler_get_summary(e, &sum);
    BR_CHECK(sum.verdict == BR_PROFILER_VERDICT_CPU_UPDATE);
    BR_CHECK(SDL_strcmp(sum.top[0], "probe.update") == 0 && near(sum.top_ms[0], 10.0f));
    BR_CHECK(near(sum.frame_ms, 10.0f) && near(sum.fps, 100.0f));
    /* three render-heavy frames do not flip it */
    p.update_ns = 0;
    p.render_ns = 30000000;
    for (int i = 0; i < 3; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    br_profiler_get_summary(e, &sum);
    BR_CHECK(sum.verdict == BR_PROFILER_VERDICT_CPU_UPDATE);
    /* a lasting change does */
    for (int i = 0; i < 40; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    br_profiler_get_summary(e, &sum);
    BR_CHECK(sum.verdict == BR_PROFILER_VERDICT_CPU_RENDER);
    br_profiler_frame_info info;
    BR_CHECK(br_profiler_get_frame(e, 0, &info) && info.verdict == BR_PROFILER_VERDICT_CPU_RENDER);
    br_engine_destroy(e);
}

/* ---- trace file and summaries */

static bool trace_balanced(const cJSON *events)
{
    /* per thread: every E closes the latest open B of the same name, nothing stays open */
    const char *open[2][64];
    int depth[2] = { 0, 0 };
    const cJSON *ev = NULL;
    cJSON_ArrayForEach(ev, events)
    {
        const char *ph = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "ph"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "name"));
        const cJSON *tid = cJSON_GetObjectItem(ev, "tid");
        if (ph == NULL || name == NULL || (ph[0] != 'B' && ph[0] != 'E')) {
            continue;
        }
        int t = cJSON_IsNumber(tid) && tid->valueint == 2 ? 1 : 0;
        if (ph[0] == 'B') {
            if (depth[t] >= 64) {
                return false;
            }
            open[t][depth[t]++] = name;
        } else if (depth[t] == 0 || SDL_strcmp(open[t][--depth[t]], name) != 0) {
            return false;
        }
    }
    return depth[0] == 0 && depth[1] == 0;
}

static void test_trace_and_summary(void)
{
    fake_clock clock = { 1000 };
    probe p;
    SDL_zero(p);
    p.nested = true;
    p.render_ns = 1000000;
    br_engine *e = make_engine(&clock, &p);
    for (int i = 0; i < 20; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    char path[1024];
    const char *base = SDL_GetBasePath();
    SDL_snprintf(path, sizeof(path), "%stest_profiler_trace.json", base != NULL ? base : "");
    BR_CHECK(br_profiler_save(e, path));
    void *bytes = NULL;
    size_t size = 0;
    BR_CHECK(br_file_read_all(e, path, &bytes, &size));
    cJSON *root = cJSON_Parse(bytes);
    BR_CHECK(root != NULL);
    const cJSON *events = cJSON_GetObjectItem(root, "traceEvents");
    BR_CHECK(cJSON_IsArray(events) && cJSON_GetArraySize(events) > 100);
    BR_CHECK(trace_balanced(events));
    int outer = 0;
    const cJSON *ev = NULL;
    cJSON_ArrayForEach(ev, events)
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "name"));
        outer += name != NULL && SDL_strcmp(name, "outer") == 0 ? 1 : 0;
    }
    BR_CHECK(outer == 2 * 20); /* one B and one E per frame */
    cJSON_Delete(root);
    br_free(bytes);
    char summary_path[1100];
    SDL_snprintf(summary_path, sizeof(summary_path), "%s.txt", path);
    BR_CHECK(br_file_exists(e, summary_path));

    char text[2048];
    BR_CHECK(br_profiler_summary(e, 10, text, sizeof(text)));
    BR_CHECK(SDL_strstr(text, "frame ms: avg 8.000") != NULL);
    BR_CHECK(SDL_strstr(text, "verdict:  CPU bound (update)") != NULL);

    cJSON *json = cJSON_CreateObject();
    BR_CHECK(br_profiler_summary_json(e, 10, json));
    BR_CHECK(cJSON_GetObjectItem(json, "frames")->valuedouble == 10.0);
    BR_CHECK(near((float)cJSON_GetObjectItem(json, "frame_ms_p95")->valuedouble, 8.0f));
    BR_CHECK(near((float)cJSON_GetObjectItem(json, "update_ms_avg")->valuedouble, 7.0f));
    BR_CHECK(cJSON_IsNull(cJSON_GetObjectItem(json, "gpu_ms_avg"))); /* headless */
    const cJSON *top = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "top_scopes"), 0);
    BR_CHECK(SDL_strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(top, "name")), "inner") == 0);
    cJSON_Delete(json);
    br_engine_destroy(e);
}

/* ---- budgets */

typedef struct log_counter {
    int budget_lines;
    SDL_LogOutputFunction prev;
    void *prev_user;
} log_counter;

static void SDLCALL count_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    log_counter *c = user;
    if (SDL_strstr(message, "budget") != NULL) {
        c->budget_lines++;
    }
    c->prev(c->prev_user, category, priority, message);
}

static void test_budget(void)
{
    fake_clock clock = { 1000 };
    probe p;
    SDL_zero(p);
    p.update_ns = 6000000;
    br_engine *e = make_engine(&clock, &p);
    log_counter counter;
    SDL_zero(counter);
    SDL_GetLogOutputFunction(&counter.prev, &counter.prev_user);
    SDL_SetLogOutputFunction(count_log, &counter);
    br_profiler_set_budget(e, "probe.update", 5.0f);
    for (int i = 0; i < 20; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    BR_CHECK(counter.budget_lines == 1); /* once per breach, not once per frame */
    p.update_ns = 1000000;
    for (int i = 0; i < 70; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    p.update_ns = 6000000;
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(counter.budget_lines == 2); /* armed again after 60 frames under budget */
    br_profiler_set_budget(e, "probe.update", 0.0f);
    p.update_ns = 1000000;
    for (int i = 0; i < 70; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    p.update_ns = 6000000;
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(counter.budget_lines == 2); /* removed */
    SDL_SetLogOutputFunction(counter.prev, counter.prev_user);
    br_engine_destroy(e);
}

/* ---- overhead: 200 scopes per frame on the real clock */

static void empty_system(br_engine *e, float dt, void *user)
{
    (void)e;
    (void)dt;
    (void)user;
}

static void scopes_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    static const char *names[4] = { "a", "b", "c", "d" };
    for (int i = 0; i < 100; i++) {
        br_profile_begin(e, names[i % 4]);
        br_profile_begin(e, "leaf");
        br_profile_end(e);
        br_profile_end(e);
    }
}

static double frame_cost_us(bool scopes)
{
    br_engine *e = br_engine_create("test_profiler", NULL);
    br_system_add(e, "work", BR_PHASE_UPDATE, 0, scopes ? scopes_system : empty_system, NULL);
    for (int i = 0; i < 50; i++) {
        br_engine_step(e, 1.0f / 60.0f); /* warm up */
    }
    const int frames = 2000;
    uint64_t start = SDL_GetTicksNS();
    for (int i = 0; i < frames; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    double us = (double)(SDL_GetTicksNS() - start) / 1000.0 / frames;
    br_engine_destroy(e);
    return us;
}

static void test_overhead(void)
{
    double base = frame_cost_us(false);
    double with = frame_cost_us(true);
    double cost = with - base;
    printf("profiler overhead: 200 scopes cost %.1f us per frame (%.3f%% of 16.6 ms, %.0f ns per scope); "
           "a headless frame without them %.1f us\n",
           cost, cost / 16666.0 * 100.0, cost * 1000.0 / 200.0, base);
    /* measured here well under 1% of a 60 Hz frame (166 us); the check allows 1 ms so a shared CI
     * runner or a Debug build does not flake, and still fails a 10x regression */
    BR_CHECK(cost < 1000.0);
    /* the fence thread polls every 0.1 ms; how long that sleep really is sets the GPU estimate's resolution */
    uint64_t slept[51];
    for (int i = 0; i < 51; i++) {
        uint64_t start = SDL_GetTicksNS();
        SDL_DelayNS(100000);
        slept[i] = SDL_GetTicksNS() - start;
    }
    for (int i = 0; i < 51; i++) {
        for (int j = i + 1; j < 51; j++) {
            if (slept[j] < slept[i]) {
                uint64_t t = slept[i];
                slept[i] = slept[j];
                slept[j] = t;
            }
        }
    }
    printf("fence poll period (SDL_DelayNS(100 us)): median %.0f us, max %.0f us\n", (double)slept[25] / 1000.0,
           (double)slept[50] / 1000.0);
}

#endif

/* ---- the overlay takes clicks on its windows */

static SDL_Event mouse_event(SDL_EventType type, float x, float y)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = type;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = x;
    ev.button.y = y;
    return ev;
}

static void test_overlay_input(void)
{
    br_engine *e = br_engine_create("test_profiler", NULL);
    SDL_Event down = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, 20.0f, 20.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_debug_wants_event(e, &down)); /* nothing shown */

    br_debug_set_visible(e, true);
    br_engine_step(e, 1.0f / 60.0f); /* the UI is built (headless too) */
    BR_CHECK(br_debug_wants_event(e, &down)); /* on the Stats window */
    SDL_Event up = mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, 600.0f, 700.0f);
    BR_CHECK(br_debug_wants_event(e, &up)); /* the release of that press, wherever it is */
    BR_CHECK(!br_debug_wants_event(e, &up)); /* only once */
    SDL_Event away = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, 1200.0f, 700.0f);
    BR_CHECK(!br_debug_wants_event(e, &away)); /* empty screen: the game gets it */
    SDL_Event wheel;
    SDL_zero(wheel);
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.mouse_x = 20.0f;
    wheel.wheel.mouse_y = 400.0f; /* Systems window */
    wheel.wheel.y = 1.0f;
    BR_CHECK(br_debug_wants_event(e, &wheel));
    SDL_Event key;
    SDL_zero(key);
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_SPACE;
    BR_CHECK(!br_debug_wants_event(e, &key)); /* no field is being edited */

    /* the corner readout is display only; the profiler panel takes clicks */
    br_debug_set_visible(e, false);
    br_profiler_set_hud(e, true);
    br_engine_step(e, 1.0f / 60.0f);
    SDL_Event on_hud = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, 1100.0f, 690.0f);
    BR_CHECK(!br_debug_wants_event(e, &on_hud));
    BR_CHECK(!br_debug_wants_event(e, &down)); /* Stats is closed now */
    br_profiler_set_visible(e, true);
    br_engine_step(e, 1.0f / 60.0f);
    SDL_Event on_panel = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, 1000.0f, 100.0f);
    BR_CHECK(br_debug_wants_event(e, &on_panel));
    br_engine_destroy(e);
}

/* Shift-click a number in the Inspector, then: key presses go to the field, key releases still
 * reach the game (a key held before the edit used to stay down forever). */
static void test_overlay_key_release(void)
{
    br_engine *e = br_engine_create("test_profiler_keys", NULL);
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    br_debug_set_visible(e, true);
    br_debug_select(e, n);
    br_input_inject_key(e, SDL_SCANCODE_W, true); /* walking */
    br_engine_step(e, 1.0f / 60.0f);
    /* the position x field: Inspector at (690, 10), title 24, padding 5, a label row, then
     * "position" (70 wide) and the number (95 wide); headless text is 0 high, rows 10 */
    br_input_inject_pointer_move(e, 810.0f, 66.0f);
    br_engine_step(e, 1.0f / 60.0f);
    br_input_inject_key(e, SDL_SCANCODE_LSHIFT, true);
    br_engine_step(e, 1.0f / 60.0f);
    br_input_inject_pointer_down(e, 810.0f, 66.0f, SDL_BUTTON_LEFT);
    br_engine_step(e, 1.0f / 60.0f);
    br_input_inject_pointer_up(e, 810.0f, 66.0f, SDL_BUTTON_LEFT);
    br_input_inject_key(e, SDL_SCANCODE_LSHIFT, false);
    br_engine_step(e, 1.0f / 60.0f);
    SDL_Event key;
    SDL_zero(key);
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_1;
    BR_CHECK(br_debug_wants_event(e, &key)); /* the field is being edited: typing goes there */
    key.type = SDL_EVENT_KEY_UP;
    key.key.scancode = SDL_SCANCODE_W;
    BR_CHECK(!br_debug_wants_event(e, &key)); /* but W's release reaches the game */

    /* shipped games switch the keys off; code can still open things */
    br_debug_set_visible(e, false);
    br_debug_set_keys_enabled(e, false);
    br_input_inject_key(e, SDL_SCANCODE_F1, true);
    br_input_inject_key(e, SDL_SCANCODE_F2, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_debug_visible(e) && !br_profiler_visible(e));
    br_input_inject_key(e, SDL_SCANCODE_F1, false);
    br_input_inject_key(e, SDL_SCANCODE_F2, false);
    br_debug_set_keys_enabled(e, true);
    br_debug_set_profiler_key(e, SDL_SCANCODE_UNKNOWN);
    br_input_inject_key(e, SDL_SCANCODE_F1, true);
    br_input_inject_key(e, SDL_SCANCODE_F2, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_debug_visible(e) && !br_profiler_visible(e));
    br_engine_destroy(e);
}

#ifndef BRATY_PROFILER_OFF
/* The GPU estimate (fence thread + a fence per frame) runs only while something uses it. Needs a
 * window and a GPU: skipped where none can be opened. */
static void test_fence_thread_on_demand(void)
{
    br_engine *e = br_engine_create("test_profiler_fences", NULL);
    if (!br_engine_open_window(e, 64, 64, NULL)) {
        printf("skipped the fence-thread test: no window / GPU\n");
        br_engine_destroy(e);
        return;
    }
    br_engine_step(e, 1.0f / 60.0f);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_profiler_gpu_estimating(e)); /* nobody looks: no thread, no fences */
    br_profiler_set_hud(e, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_profiler_gpu_estimating(e));
    br_profiler_set_hud(e, false);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(!br_profiler_gpu_estimating(e));
    br_profiler_set_visible(e, true);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_profiler_gpu_estimating(e)); /* started again */
    br_engine_destroy(e);
}

#endif

#ifndef BRATY_PROFILER_OFF
/* Counters the animation module feeds: animators and tweens alive this frame. */
static void test_animation_counters(void)
{
    br_engine *e = br_engine_create("test_profiler_counters", NULL);
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    BR_CHECK(br_animator_valid(e, a));
    br_tween_to_f(e, br_anim_target_2d(n), "alpha", 0.0f, 10.0f, BR_EASE_LINEAR);
    br_tween_to_f(e, br_anim_target_2d(n), "x", 5.0f, 10.0f, BR_EASE_LINEAR);
    br_engine_step(e, 1.0f / 60.0f);
    br_profiler_frame_info info;
    BR_CHECK(br_profiler_get_frame(e, 0, &info));
    BR_CHECK(info.counters[BR_PROFILER_ANIMATORS] == 1);
    BR_CHECK(info.counters[BR_PROFILER_TWEENS] == 2);
    BR_CHECK(info.counters[BR_PROFILER_ITEMS_3D] == 0 && info.counters[BR_PROFILER_PARTICLES] == 0);
    BR_CHECK(SDL_strcmp(br_profiler_counter_name(BR_PROFILER_SHADOW_CASTERS_3D), "3D shadow casters") == 0);
    br_engine_destroy(e);
}
#endif

int main(void)
{
    SDL_Init(0);
    test_verdict_rule();
#ifndef BRATY_PROFILER_OFF
    test_nesting();
    test_ring();
    test_verdict_smoothing();
    test_trace_and_summary();
    test_budget();
    test_overhead();
    test_animation_counters();
#endif
    test_overlay_input();
    test_overlay_key_release();
#ifndef BRATY_PROFILER_OFF
    test_fence_thread_on_demand();
#endif
    SDL_Quit();
    return br_test_finish();
}
