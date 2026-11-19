/* br_random.h - seeded random numbers (PCG32). See docs/agents/math.md. */
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

#endif
