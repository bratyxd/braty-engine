/* Frame pacing: vsync (present mode with fallbacks), FPS cap (deadline + precise delay),
 * fixed-timestep settings. The fixed-step loop itself runs in br_engine_step. */
#include "app/br_engine_internal.h"

/* ---- vsync */

static const char *const br_vsync_names[] = { "off", "on", "adaptive", "mailbox" };

bool br_vsync_from_name(const char *name, br_vsync *out)
{
    for (int i = 0; i < (int)BR_ARRAY_COUNT(br_vsync_names); i++) {
        if (name != NULL && SDL_strcmp(name, br_vsync_names[i]) == 0) {
            *out = (br_vsync)i;
            return true;
        }
    }
    return false;
}

const char *br_vsync_name(br_vsync mode)
{
    if ((int)mode < 0 || (int)mode >= (int)BR_ARRAY_COUNT(br_vsync_names)) {
        return "?";
    }
    return br_vsync_names[mode];
}

/* Tries the wanted mode, then its fallbacks in order; VSYNC is always supported by SDL. */
br_vsync br_app_set_vsync(br_engine *e, br_vsync mode)
{
    if (e->gpu == NULL) {
        e->vsync = mode; /* headless: applied when a window opens */
        return mode;
    }
    br_vsync chain[3];
    uint32_t count = 0;
    chain[count++] = mode;
    if (mode == BR_VSYNC_OFF) {
        chain[count++] = BR_VSYNC_MAILBOX;
    }
    chain[count++] = BR_VSYNC_ON;
    for (uint32_t i = 0; i < count; i++) {
        SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
        if (chain[i] == BR_VSYNC_OFF) {
            present = SDL_GPU_PRESENTMODE_IMMEDIATE;
        } else if (chain[i] == BR_VSYNC_MAILBOX) {
            present = SDL_GPU_PRESENTMODE_MAILBOX;
        } else if (chain[i] == BR_VSYNC_ADAPTIVE) {
            continue; /* SDL_GPUPresentMode has no relaxed FIFO */
        }
        if (br_gpu_set_present_mode(e->gpu, present)) {
            if (chain[i] != mode) {
                br_log_info("vsync '%s' is not supported by this window/driver; using '%s'", br_vsync_name(mode),
                            br_vsync_name(chain[i]));
            }
            e->vsync = chain[i];
            return chain[i];
        }
    }
    br_log_error("vsync: cannot set any present mode");
    return e->vsync;
}

br_vsync br_app_vsync(br_engine *e)
{
    return e->vsync;
}

/* ---- FPS cap */

void br_app_set_max_fps(br_engine *e, float fps)
{
    e->ticker.max_fps = fps > 0.0f ? fps : 0.0f;
    e->ticker.deadline_ns = 0; /* restart the schedule */
}

float br_app_max_fps(br_engine *e)
{
    return e->ticker.max_fps;
}

void br_app_pace_frame(br_engine *e)
{
    br_ticker_state *t = &e->ticker;
    if (t->max_fps <= 0.0f) {
        return;
    }
    uint64_t interval = (uint64_t)(1e9 / (double)t->max_fps);
    uint64_t now = SDL_GetTicksNS(); /* performance counter underneath */
    if (t->deadline_ns == 0 || now >= t->deadline_ns + interval) {
        /* first frame, or more than a whole frame late: restart instead of rushing to catch up */
        t->deadline_ns = now;
    }
    if (now < t->deadline_ns) {
        /* sleeps in 1 ms slices, then spins the last part */
        SDL_DelayPrecise(t->deadline_ns - now);
    }
    t->deadline_ns += interval;
}

/* ---- fixed timestep */

void br_app_set_fixed_timestep(br_engine *e, float hz)
{
    e->ticker.fixed_hz = hz > 0.0f ? hz : 0.0f;
    e->ticker.fixed_accumulator = 0.0;
    e->ticker.fixed_alpha = 0.0f;
}

float br_app_fixed_timestep(br_engine *e)
{
    return e->ticker.fixed_hz;
}

float br_app_fixed_alpha(br_engine *e)
{
    return e->ticker.fixed_alpha;
}

uint32_t br_app_fixed_steps(br_engine *e)
{
    return e->ticker.fixed_steps;
}

float br_app_frame_budget_ms(br_engine *e)
{
    float budget = 0.0f;
    if (e->ticker.max_fps > 0.0f) {
        budget = 1000.0f / e->ticker.max_fps;
    }
    bool waits_for_display = e->vsync == BR_VSYNC_ON || e->vsync == BR_VSYNC_ADAPTIVE;
    if (e->window != NULL && waits_for_display) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(e->window));
        if (mode != NULL && mode->refresh_rate > 0.0f && 1000.0f / mode->refresh_rate > budget) {
            budget = 1000.0f / mode->refresh_rate;
        }
    }
    return budget > 0.0f ? budget : 1000.0f / 60.0f;
}
