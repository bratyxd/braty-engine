/* br_ease: the standard easing set (formulas as on easings.net, after Robert Penner). */
#include "braty/br_ease.h"

#include <SDL3/SDL_stdinc.h>

#define BR_EASE_BACK    1.70158f
#define BR_EASE_BACK_IO (1.70158f * 1.525f)

float br_ease_linear(float t)
{
    return br_clamp01f(t);
}

float br_ease_in_quad(float t)
{
    t = br_clamp01f(t);
    return t * t;
}

float br_ease_out_quad(float t)
{
    t = br_clamp01f(t);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float br_ease_in_out_quad(float t)
{
    t = br_clamp01f(t);
    float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 2.0f * t * t : 1.0f - u * u * 0.5f;
}

float br_ease_in_cubic(float t)
{
    t = br_clamp01f(t);
    return t * t * t;
}

float br_ease_out_cubic(float t)
{
    float u = 1.0f - br_clamp01f(t);
    return 1.0f - u * u * u;
}

float br_ease_in_out_cubic(float t)
{
    t = br_clamp01f(t);
    float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - u * u * u * 0.5f;
}

float br_ease_in_quart(float t)
{
    t = br_clamp01f(t);
    return t * t * t * t;
}

float br_ease_out_quart(float t)
{
    float u = 1.0f - br_clamp01f(t);
    return 1.0f - u * u * u * u;
}

float br_ease_in_out_quart(float t)
{
    t = br_clamp01f(t);
    float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - u * u * u * u * 0.5f;
}

float br_ease_in_quint(float t)
{
    t = br_clamp01f(t);
    return t * t * t * t * t;
}

float br_ease_out_quint(float t)
{
    float u = 1.0f - br_clamp01f(t);
    return 1.0f - u * u * u * u * u;
}

float br_ease_in_out_quint(float t)
{
    t = br_clamp01f(t);
    float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - u * u * u * u * u * 0.5f;
}

/* The trig and exponential curves only reach their end values approximately in float, so their
 * ends are pinned explicitly. */

float br_ease_in_sine(float t)
{
    t = br_clamp01f(t);
    return t >= 1.0f ? 1.0f : 1.0f - SDL_cosf(t * BR_PI * 0.5f);
}

float br_ease_out_sine(float t)
{
    t = br_clamp01f(t);
    return t >= 1.0f ? 1.0f : SDL_sinf(t * BR_PI * 0.5f);
}

float br_ease_in_out_sine(float t)
{
    t = br_clamp01f(t);
    return t >= 1.0f ? 1.0f : -(SDL_cosf(BR_PI * t) - 1.0f) * 0.5f;
}

float br_ease_in_expo(float t)
{
    t = br_clamp01f(t);
    return t <= 0.0f ? 0.0f : SDL_powf(2.0f, 10.0f * t - 10.0f);
}

float br_ease_out_expo(float t)
{
    t = br_clamp01f(t);
    return t >= 1.0f ? 1.0f : 1.0f - SDL_powf(2.0f, -10.0f * t);
}

float br_ease_in_out_expo(float t)
{
    t = br_clamp01f(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    return t < 0.5f ? SDL_powf(2.0f, 20.0f * t - 10.0f) * 0.5f : (2.0f - SDL_powf(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

float br_ease_in_circ(float t)
{
    t = br_clamp01f(t);
    return 1.0f - SDL_sqrtf(1.0f - t * t);
}

float br_ease_out_circ(float t)
{
    t = br_clamp01f(t);
    return SDL_sqrtf(1.0f - (t - 1.0f) * (t - 1.0f));
}

float br_ease_in_out_circ(float t)
{
    t = br_clamp01f(t);
    float u = -2.0f * t + 2.0f;
    return t < 0.5f ? (1.0f - SDL_sqrtf(1.0f - 4.0f * t * t)) * 0.5f : (SDL_sqrtf(1.0f - u * u) + 1.0f) * 0.5f;
}

float br_ease_in_back(float t)
{
    t = br_clamp01f(t);
    return t >= 1.0f ? 1.0f : (BR_EASE_BACK + 1.0f) * t * t * t - BR_EASE_BACK * t * t;
}

float br_ease_out_back(float t)
{
    t = br_clamp01f(t);
    float u = t - 1.0f;
    return t <= 0.0f ? 0.0f : 1.0f + (BR_EASE_BACK + 1.0f) * u * u * u + BR_EASE_BACK * u * u;
}

float br_ease_in_out_back(float t)
{
    t = br_clamp01f(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    float c = BR_EASE_BACK_IO;
    float u = 2.0f * t - 2.0f;
    return t < 0.5f ? (4.0f * t * t * ((c + 1.0f) * 2.0f * t - c)) * 0.5f
                    : (u * u * ((c + 1.0f) * u + c) + 2.0f) * 0.5f;
}

float br_ease_in_elastic(float t)
{
    t = br_clamp01f(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    return -SDL_powf(2.0f, 10.0f * t - 10.0f) * SDL_sinf((10.0f * t - 10.75f) * (BR_TAU / 3.0f));
}

float br_ease_out_elastic(float t)
{
    t = br_clamp01f(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    return SDL_powf(2.0f, -10.0f * t) * SDL_sinf((10.0f * t - 0.75f) * (BR_TAU / 3.0f)) + 1.0f;
}

float br_ease_in_out_elastic(float t)
{
    t = br_clamp01f(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    float s = SDL_sinf((20.0f * t - 11.125f) * (BR_TAU / 4.5f));
    return t < 0.5f ? -(SDL_powf(2.0f, 20.0f * t - 10.0f) * s) * 0.5f
                    : (SDL_powf(2.0f, -20.0f * t + 10.0f) * s) * 0.5f + 1.0f;
}

float br_ease_out_bounce(float t)
{
    const float n1 = 7.5625f;
    const float d1 = 2.75f;
    t = br_clamp01f(t);
    if (t >= 1.0f) {
        return 1.0f;
    }
    if (t < 1.0f / d1) {
        return n1 * t * t;
    }
    if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

float br_ease_in_bounce(float t)
{
    return 1.0f - br_ease_out_bounce(1.0f - br_clamp01f(t));
}

float br_ease_in_out_bounce(float t)
{
    t = br_clamp01f(t);
    return t < 0.5f ? (1.0f - br_ease_out_bounce(1.0f - 2.0f * t)) * 0.5f
                    : (1.0f + br_ease_out_bounce(2.0f * t - 1.0f)) * 0.5f;
}

/* ---- lookup (same order as br_ease_type) */

typedef float (*br_ease_fn)(float t);

static const br_ease_fn br_ease_fns[BR_EASE_COUNT] = {
    br_ease_linear,
    br_ease_in_quad, br_ease_out_quad, br_ease_in_out_quad,
    br_ease_in_cubic, br_ease_out_cubic, br_ease_in_out_cubic,
    br_ease_in_quart, br_ease_out_quart, br_ease_in_out_quart,
    br_ease_in_quint, br_ease_out_quint, br_ease_in_out_quint,
    br_ease_in_sine, br_ease_out_sine, br_ease_in_out_sine,
    br_ease_in_expo, br_ease_out_expo, br_ease_in_out_expo,
    br_ease_in_circ, br_ease_out_circ, br_ease_in_out_circ,
    br_ease_in_back, br_ease_out_back, br_ease_in_out_back,
    br_ease_in_elastic, br_ease_out_elastic, br_ease_in_out_elastic,
    br_ease_in_bounce, br_ease_out_bounce, br_ease_in_out_bounce,
};

static const char *const br_ease_names[BR_EASE_COUNT] = {
    "linear",
    "in_quad", "out_quad", "in_out_quad",
    "in_cubic", "out_cubic", "in_out_cubic",
    "in_quart", "out_quart", "in_out_quart",
    "in_quint", "out_quint", "in_out_quint",
    "in_sine", "out_sine", "in_out_sine",
    "in_expo", "out_expo", "in_out_expo",
    "in_circ", "out_circ", "in_out_circ",
    "in_back", "out_back", "in_out_back",
    "in_elastic", "out_elastic", "in_out_elastic",
    "in_bounce", "out_bounce", "in_out_bounce",
};

float br_ease_apply(br_ease_type type, float t)
{
    if ((int)type < 0 || type >= BR_EASE_COUNT) {
        return br_ease_linear(t);
    }
    return br_ease_fns[type](t);
}

br_ease_type br_ease_from_name(const char *name, bool *ok)
{
    if (name != NULL && SDL_strncasecmp(name, "ease_", 5) == 0) {
        name += 5;
    }
    for (int i = 0; name != NULL && i < BR_EASE_COUNT; i++) {
        if (SDL_strcasecmp(name, br_ease_names[i]) == 0) {
            if (ok != NULL) {
                *ok = true;
            }
            return (br_ease_type)i;
        }
    }
    if (ok != NULL) {
        *ok = false;
    }
    return BR_EASE_LINEAR;
}

const char *br_ease_name(br_ease_type type)
{
    if ((int)type < 0 || type >= BR_EASE_COUNT) {
        return br_ease_names[BR_EASE_LINEAR];
    }
    return br_ease_names[type];
}
