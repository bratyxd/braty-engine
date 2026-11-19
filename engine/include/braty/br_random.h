/* br_random.h - seeded random numbers: PCG32 (O'Neill, XSH RR) with explicit state.
 *
 *   br_random rng;
 *   br_random_seed(&rng, 42, 54);
 *   float x = br_random_range(&rng, -1.0f, 1.0f);
 *   int pick = br_random_weighted(&rng, weights, 3);
 *
 * There is no hidden global generator. br_random_engine(e) is the engine's own instance, seeded
 * with a fixed value at startup so --frames runs repeat. Same seed + stream = same sequence on
 * every platform. See docs/agents/math.md.
 */
#ifndef BR_RANDOM_H
#define BR_RANDOM_H

#include "braty/br_core.h"
#include "braty/br_math.h"

/* PCG32 generator state. Plain value: copy it to fork a sequence, zero-init is NOT valid (seed it). */
typedef struct br_random {
    uint64_t state;
    uint64_t inc;   /* stream selector, always odd */
} br_random;

/* The engine's own generator (seeded with a fixed value at startup, so --frames runs repeat). */
br_random *br_random_engine(br_engine *e);

/* Seeds a generator. Different streams give independent sequences for the same seed. */
void br_random_seed(br_random *r, uint64_t seed, uint64_t stream);
/* Next 32 random bits. */
uint32_t br_random_u32(br_random *r);
/* Uniform integer in [0, n), without modulo bias (0 when n == 0). */
uint32_t br_random_bounded(br_random *r, uint32_t n);
/* Uniform float in [0, 1) (24 random bits, never 1). */
float br_random_float(br_random *r);
/* Uniform float in [lo, hi). */
float br_random_range(br_random *r, float lo, float hi);
/* Uniform integer in [lo, hi], both inclusive (lo when hi < lo). */
int32_t br_random_range_int(br_random *r, int32_t lo, int32_t hi);
/* True with probability p. */
bool br_random_bool(br_random *r, float p);
/* Normally distributed value (Box-Muller). */
float br_random_gaussian(br_random *r, float mean, float stddev);
/* Uniform point inside the unit disc. */
br_vec2 br_random_in_circle(br_random *r);
/* Uniform point on the unit circle. */
br_vec2 br_random_on_circle(br_random *r);
/* Uniform point inside the unit ball. */
br_vec3 br_random_in_sphere(br_random *r);
/* Uniform point on the unit sphere. */
br_vec3 br_random_on_sphere(br_random *r);
/* Shuffles count items of item_size bytes in place (Fisher-Yates). */
void br_random_shuffle(br_random *r, void *items, uint32_t count, uint32_t item_size);
/* Index picked with probability weight[i] / sum (negative weights count as 0).
 * Returns -1 when no weight is positive. */
int br_random_weighted(br_random *r, const float *weights, uint32_t count);

#endif
