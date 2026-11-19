/* br_ease.h - easing functions (the standard Penner / easings.net set).
 *
 *   float k = br_ease_out_cubic(t);                        direct call
 *   float k = br_ease_apply(BR_EASE_OUT_CUBIC, t);         chosen at run time
 *   br_ease_type type = br_ease_from_name("in_out_back", &ok);   from a data file
 *
 * Every function takes t in [0, 1] (values outside are clamped) and returns exactly 0 at t = 0
 * and exactly 1 at t = 1. back and elastic overshoot below 0 / above 1 in between.
 * Names in data files are the function suffixes: "linear", "in_quad", "out_cubic", "in_out_bounce".
 */
#ifndef BR_EASE_H
#define BR_EASE_H

#include "braty/br_math.h"

typedef enum br_ease_type {
    BR_EASE_LINEAR,
    BR_EASE_IN_QUAD,
    BR_EASE_OUT_QUAD,
    BR_EASE_IN_OUT_QUAD,
    BR_EASE_IN_CUBIC,
    BR_EASE_OUT_CUBIC,
    BR_EASE_IN_OUT_CUBIC,
    BR_EASE_IN_QUART,
    BR_EASE_OUT_QUART,
    BR_EASE_IN_OUT_QUART,
    BR_EASE_IN_QUINT,
    BR_EASE_OUT_QUINT,
    BR_EASE_IN_OUT_QUINT,
    BR_EASE_IN_SINE,
    BR_EASE_OUT_SINE,
    BR_EASE_IN_OUT_SINE,
    BR_EASE_IN_EXPO,
    BR_EASE_OUT_EXPO,
    BR_EASE_IN_OUT_EXPO,
    BR_EASE_IN_CIRC,
    BR_EASE_OUT_CIRC,
    BR_EASE_IN_OUT_CIRC,
    BR_EASE_IN_BACK,
    BR_EASE_OUT_BACK,
    BR_EASE_IN_OUT_BACK,
    BR_EASE_IN_ELASTIC,
    BR_EASE_OUT_ELASTIC,
    BR_EASE_IN_OUT_ELASTIC,
    BR_EASE_IN_BOUNCE,
    BR_EASE_OUT_BOUNCE,
    BR_EASE_IN_OUT_BOUNCE,
    BR_EASE_COUNT
} br_ease_type;

/* t */
float br_ease_linear(float t);
/* t^2 */
float br_ease_in_quad(float t);
/* Decelerating quadratic. */
float br_ease_out_quad(float t);
/* Quadratic in, then out. */
float br_ease_in_out_quad(float t);
/* t^3 */
float br_ease_in_cubic(float t);
/* Decelerating cubic. */
float br_ease_out_cubic(float t);
/* Cubic in, then out. */
float br_ease_in_out_cubic(float t);
/* t^4 */
float br_ease_in_quart(float t);
/* Decelerating quartic. */
float br_ease_out_quart(float t);
/* Quartic in, then out. */
float br_ease_in_out_quart(float t);
/* t^5 */
float br_ease_in_quint(float t);
/* Decelerating quintic. */
float br_ease_out_quint(float t);
/* Quintic in, then out. */
float br_ease_in_out_quint(float t);
/* Quarter cosine. */
float br_ease_in_sine(float t);
/* Quarter sine. */
float br_ease_out_sine(float t);
/* Half cosine. */
float br_ease_in_out_sine(float t);
/* 2^(10t - 10) */
float br_ease_in_expo(float t);
/* 1 - 2^(-10t) */
float br_ease_out_expo(float t);
/* Exponential in, then out. */
float br_ease_in_out_expo(float t);
/* Quarter circle, slow start. */
float br_ease_in_circ(float t);
/* Quarter circle, fast start. */
float br_ease_out_circ(float t);
/* Circular in, then out. */
float br_ease_in_out_circ(float t);
/* Pulls back below 0 before going (overshoot 1.70158). */
float br_ease_in_back(float t);
/* Overshoots above 1 before settling. */
float br_ease_out_back(float t);
/* Back in, then back out. */
float br_ease_in_out_back(float t);
/* Growing oscillation. */
float br_ease_in_elastic(float t);
/* Decaying oscillation around 1. */
float br_ease_out_elastic(float t);
/* Elastic in, then out. */
float br_ease_in_out_elastic(float t);
/* Bounces at the start (mirror of out_bounce). */
float br_ease_in_bounce(float t);
/* Ball dropping and bouncing to rest at 1. */
float br_ease_out_bounce(float t);
/* Bounce in, then out. */
float br_ease_in_out_bounce(float t);

/* Calls the easing function `type` (linear for an out-of-range type). */
float br_ease_apply(br_ease_type type, float t);
/* Easing by name ("out_cubic"; case-insensitive, an optional "ease_" prefix is allowed). Unknown
 * names return BR_EASE_LINEAR and *ok = false (ok may be NULL). */
br_ease_type br_ease_from_name(const char *name, bool *ok);
/* The data-file name of an easing ("in_out_back"), "linear" for an out-of-range type. */
const char *br_ease_name(br_ease_type type);

#endif
