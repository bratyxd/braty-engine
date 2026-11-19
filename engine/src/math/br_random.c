/* br_random: PCG32 (Melissa O'Neill, pcg-random.org), the 64-bit-state XSH RR variant. */
#include "braty/br_random.h"

#include "app/br_engine_internal.h"

#include <SDL3/SDL_stdinc.h>

br_random *br_random_engine(br_engine *e)
{
    return &e->rng; /* one generator per engine, also behind br_engine_random */
}

void br_random_seed(br_random *r, uint64_t seed, uint64_t stream)
{
    r->state = 0u;
    r->inc = (stream << 1u) | 1u;
    br_random_u32(r);
    r->state += seed;
    br_random_u32(r);
}

uint32_t br_random_u32(br_random *r)
{
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ull + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

uint32_t br_random_bounded(br_random *r, uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    /* reject the few low values that would make r % n biased */
    uint32_t threshold = (0u - n) % n;
    for (;;) {
        uint32_t v = br_random_u32(r);
        if (v >= threshold) {
            return v % n;
        }
    }
}

float br_random_float(br_random *r)
{
    return (float)(br_random_u32(r) >> 8) * (1.0f / 16777216.0f);
}

float br_random_range(br_random *r, float lo, float hi)
{
    return lo + (hi - lo) * br_random_float(r);
}

int32_t br_random_range_int(br_random *r, int32_t lo, int32_t hi)
{
    if (hi < lo) {
        return lo;
    }
    uint32_t span = (uint32_t)((int64_t)hi - (int64_t)lo + 1);
    if (span == 0) { /* the whole int32 range */
        return (int32_t)br_random_u32(r);
    }
    return (int32_t)((int64_t)lo + (int64_t)br_random_bounded(r, span));
}

bool br_random_bool(br_random *r, float p)
{
    return br_random_float(r) < p;
}

float br_random_gaussian(br_random *r, float mean, float stddev)
{
    float u1 = 1.0f - br_random_float(r); /* (0, 1]: log stays finite */
    float u2 = br_random_float(r);
    return mean + stddev * SDL_sqrtf(-2.0f * SDL_logf(u1)) * SDL_cosf(BR_TAU * u2);
}

br_vec2 br_random_in_circle(br_random *r)
{
    float radius = SDL_sqrtf(br_random_float(r)); /* sqrt: uniform by area, not crowded at the centre */
    return br_vec2_scale(br_random_on_circle(r), radius);
}

br_vec2 br_random_on_circle(br_random *r)
{
    float a = br_random_float(r) * BR_TAU;
    return br_vec2_make(SDL_cosf(a), SDL_sinf(a));
}

br_vec3 br_random_on_sphere(br_random *r)
{
    /* Archimedes: z uniform in [-1, 1] and the angle uniform give a uniform sphere */
    float z = br_random_range(r, -1.0f, 1.0f);
    float a = br_random_float(r) * BR_TAU;
    float ring = SDL_sqrtf(br_maxf(0.0f, 1.0f - z * z));
    return br_vec3_make(ring * SDL_cosf(a), ring * SDL_sinf(a), z);
}

br_vec3 br_random_in_sphere(br_random *r)
{
    float radius = SDL_powf(br_random_float(r), 1.0f / 3.0f); /* cube root: uniform by volume */
    return br_vec3_scale(br_random_on_sphere(r), radius);
}

void br_random_shuffle(br_random *r, void *items, uint32_t count, uint32_t item_size)
{
    uint8_t *bytes = items;
    for (uint32_t i = count; i > 1; i--) {
        uint32_t j = br_random_bounded(r, i);
        uint8_t *a = bytes + (size_t)(i - 1) * item_size;
        uint8_t *b = bytes + (size_t)j * item_size;
        for (uint32_t k = 0; k < item_size && a != b; k++) {
            uint8_t tmp = a[k];
            a[k] = b[k];
            b[k] = tmp;
        }
    }
}

int br_random_weighted(br_random *r, const float *weights, uint32_t count)
{
    float total = 0.0f;
    int last = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (weights[i] > 0.0f) {
            total += weights[i];
            last = (int)i;
        }
    }
    if (last < 0) {
        return -1;
    }
    float pick = br_random_float(r) * total;
    for (uint32_t i = 0; i < count; i++) {
        if (weights[i] > 0.0f) {
            if (pick < weights[i]) {
                return (int)i;
            }
            pick -= weights[i];
        }
    }
    return last; /* rounding left a sliver past the end */
}
