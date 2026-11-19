/* br_noise.h - coherent noise: value, Perlin (improved, 2002), simplex, and fBm over any of them.
 *
 *   float h = br_noise_fbm2(BR_NOISE_SIMPLEX, x * 0.01f, y * 0.01f, seed, 5, 2.0f, 0.5f);
 *
 * Every function is deterministic, stateless and returns roughly [-1, 1] (Perlin and simplex can
 * touch slightly past +-1 at rare points; clamp if you need a hard bound). The pattern repeats
 * every 256 units. seed 0 is Ken Perlin's reference permutation; other seeds shift the lattice
 * through a 32-bit hash, so every seed gives a different field.
 */
#ifndef BR_NOISE_H
#define BR_NOISE_H

#include "braty/br_math.h"

typedef enum br_noise_type {
    BR_NOISE_VALUE,    /* random values at lattice points, smoothly blended: blocky, cheap */
    BR_NOISE_PERLIN,   /* gradient noise, zero at every integer lattice point */
    BR_NOISE_SIMPLEX   /* gradient noise on a simplex grid: fewer axis-aligned artefacts */
} br_noise_type;

/* 2D value noise. */
float br_noise_value2(float x, float y, uint32_t seed);
/* 3D value noise. */
float br_noise_value3(float x, float y, float z, uint32_t seed);
/* 2D Perlin noise (the 3D function at z = 0). */
float br_noise_perlin2(float x, float y, uint32_t seed);
/* 3D improved Perlin noise. */
float br_noise_perlin3(float x, float y, float z, uint32_t seed);
/* 2D simplex noise. */
float br_noise_simplex2(float x, float y, uint32_t seed);
/* 3D simplex noise. */
float br_noise_simplex3(float x, float y, float z, uint32_t seed);
/* Fractal sum of `octaves` layers of 2D noise; each layer has lacunarity x the frequency and
 * gain x the amplitude of the previous one (2 and 0.5 are typical). Normalised to about [-1, 1]. */
float br_noise_fbm2(br_noise_type type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain);
/* Fractal sum of 3D noise, as br_noise_fbm2. */
float br_noise_fbm3(br_noise_type type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity,
                    float gain);

#endif
