/* br_noise: value, improved Perlin (Ken Perlin 2002) and simplex noise (after Stefan Gustavson's
 * "Simplex noise demystified"), all driven by Perlin's reference permutation. */
#include "braty/br_noise.h"

#include <SDL3/SDL_stdinc.h>

static const uint8_t br_perm[256] = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
    240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
    33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
    158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
    63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
    109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
    59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
    101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
    97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
    214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
    67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
};

/* Lattice offsets derived from the seed (murmur3 finaliser: seed 0 -> offsets 0 = reference noise). */
typedef struct br_noise_offset { int x, y, z; } br_noise_offset;

static br_noise_offset br_noise_seed_offset(uint32_t seed)
{
    uint32_t h = seed;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    br_noise_offset o = { (int)(h & 255u), (int)((h >> 8) & 255u), (int)((h >> 16) & 255u) };
    return o;
}

static int br_noise_hash3(int x, int y, int z)
{
    return br_perm[(br_perm[(br_perm[x & 255] + y) & 255] + z) & 255];
}

static float br_noise_fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* ---- value noise */

static float br_noise_lattice_value(int x, int y, int z)
{
    return (float)br_noise_hash3(x, y, z) * (2.0f / 255.0f) - 1.0f;
}

float br_noise_value3(float x, float y, float z, uint32_t seed)
{
    br_noise_offset o = br_noise_seed_offset(seed);
    float fx = SDL_floorf(x), fy = SDL_floorf(y), fz = SDL_floorf(z);
    int ix = (int)fx + o.x, iy = (int)fy + o.y, iz = (int)fz + o.z;
    float u = br_noise_fade(x - fx), v = br_noise_fade(y - fy), w = br_noise_fade(z - fz);
    float c00 = br_lerpf(br_noise_lattice_value(ix, iy, iz), br_noise_lattice_value(ix + 1, iy, iz), u);
    float c10 = br_lerpf(br_noise_lattice_value(ix, iy + 1, iz), br_noise_lattice_value(ix + 1, iy + 1, iz), u);
    float c01 = br_lerpf(br_noise_lattice_value(ix, iy, iz + 1), br_noise_lattice_value(ix + 1, iy, iz + 1), u);
    float c11 = br_lerpf(br_noise_lattice_value(ix, iy + 1, iz + 1), br_noise_lattice_value(ix + 1, iy + 1, iz + 1), u);
    return br_lerpf(br_lerpf(c00, c10, v), br_lerpf(c01, c11, v), w);
}

float br_noise_value2(float x, float y, uint32_t seed)
{
    return br_noise_value3(x, y, 0.0f, seed);
}

/* ---- improved Perlin noise */

static float br_noise_grad(int hash, float x, float y, float z)
{
    /* the 12 cube-edge gradients (+4 repeats) of the 2002 paper */
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float br_noise_perlin3(float x, float y, float z, uint32_t seed)
{
    br_noise_offset o = br_noise_seed_offset(seed);
    float fx = SDL_floorf(x), fy = SDL_floorf(y), fz = SDL_floorf(z);
    int ix = (int)fx + o.x, iy = (int)fy + o.y, iz = (int)fz + o.z;
    x -= fx;
    y -= fy;
    z -= fz;
    float u = br_noise_fade(x), v = br_noise_fade(y), w = br_noise_fade(z);
    float x1 = br_lerpf(br_noise_grad(br_noise_hash3(ix, iy, iz), x, y, z),
                        br_noise_grad(br_noise_hash3(ix + 1, iy, iz), x - 1.0f, y, z), u);
    float x2 = br_lerpf(br_noise_grad(br_noise_hash3(ix, iy + 1, iz), x, y - 1.0f, z),
                        br_noise_grad(br_noise_hash3(ix + 1, iy + 1, iz), x - 1.0f, y - 1.0f, z), u);
    float y1 = br_lerpf(x1, x2, v);
    x1 = br_lerpf(br_noise_grad(br_noise_hash3(ix, iy, iz + 1), x, y, z - 1.0f),
                  br_noise_grad(br_noise_hash3(ix + 1, iy, iz + 1), x - 1.0f, y, z - 1.0f), u);
    x2 = br_lerpf(br_noise_grad(br_noise_hash3(ix, iy + 1, iz + 1), x, y - 1.0f, z - 1.0f),
                  br_noise_grad(br_noise_hash3(ix + 1, iy + 1, iz + 1), x - 1.0f, y - 1.0f, z - 1.0f), u);
    return br_lerpf(y1, br_lerpf(x1, x2, v), w);
}

float br_noise_perlin2(float x, float y, uint32_t seed)
{
    return br_noise_perlin3(x, y, 0.0f, seed);
}

/* ---- simplex noise */

static const float br_grad3[12][3] = {
    { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
    { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
};

/* Contribution of one simplex corner at offset (x, y, z) with gradient index g. */
static float br_noise_corner(float r2, int g, float x, float y, float z)
{
    float t = r2 - x * x - y * y - z * z;
    if (t < 0.0f) {
        return 0.0f;
    }
    t *= t;
    return t * t * (br_grad3[g][0] * x + br_grad3[g][1] * y + br_grad3[g][2] * z);
}

float br_noise_simplex2(float x, float y, uint32_t seed)
{
    const float f2 = 0.36602540378f; /* (sqrt(3) - 1) / 2 */
    const float g2 = 0.21132486540f; /* (3 - sqrt(3)) / 6 */
    br_noise_offset o = br_noise_seed_offset(seed);
    float s = (x + y) * f2;
    float fi = SDL_floorf(x + s), fj = SDL_floorf(y + s);
    float t = (fi + fj) * g2;
    float x0 = x - (fi - t), y0 = y - (fj - t);
    int i1 = x0 > y0 ? 1 : 0;
    int j1 = x0 > y0 ? 0 : 1;
    float x1 = x0 - (float)i1 + g2, y1 = y0 - (float)j1 + g2;
    float x2 = x0 - 1.0f + 2.0f * g2, y2 = y0 - 1.0f + 2.0f * g2;
    int i = (int)fi + o.x, j = (int)fj + o.y;
    float n = br_noise_corner(0.5f, br_noise_hash3(i, j, o.z) % 12, x0, y0, 0.0f) +
              br_noise_corner(0.5f, br_noise_hash3(i + i1, j + j1, o.z) % 12, x1, y1, 0.0f) +
              br_noise_corner(0.5f, br_noise_hash3(i + 1, j + 1, o.z) % 12, x2, y2, 0.0f);
    return 70.0f * n;
}

float br_noise_simplex3(float x, float y, float z, uint32_t seed)
{
    const float f3 = 1.0f / 3.0f;
    const float g3 = 1.0f / 6.0f;
    br_noise_offset o = br_noise_seed_offset(seed);
    float s = (x + y + z) * f3;
    float fi = SDL_floorf(x + s), fj = SDL_floorf(y + s), fk = SDL_floorf(z + s);
    float t = (fi + fj + fk) * g3;
    float x0 = x - (fi - t), y0 = y - (fj - t), z0 = z - (fk - t);
    /* which of the six tetrahedra of the skewed cube we are in */
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1;
        } else {
            i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1;
        }
    } else {
        if (y0 < z0) {
            i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1;
        } else if (x0 < z0) {
            i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1;
        } else {
            i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
        }
    }
    float x1 = x0 - (float)i1 + g3, y1 = y0 - (float)j1 + g3, z1 = z0 - (float)k1 + g3;
    float x2 = x0 - (float)i2 + 2.0f * g3, y2 = y0 - (float)j2 + 2.0f * g3, z2 = z0 - (float)k2 + 2.0f * g3;
    float x3 = x0 - 1.0f + 3.0f * g3, y3 = y0 - 1.0f + 3.0f * g3, z3 = z0 - 1.0f + 3.0f * g3;
    int i = (int)fi + o.x, j = (int)fj + o.y, k = (int)fk + o.z;
    float n = br_noise_corner(0.6f, br_noise_hash3(i, j, k) % 12, x0, y0, z0) +
              br_noise_corner(0.6f, br_noise_hash3(i + i1, j + j1, k + k1) % 12, x1, y1, z1) +
              br_noise_corner(0.6f, br_noise_hash3(i + i2, j + j2, k + k2) % 12, x2, y2, z2) +
              br_noise_corner(0.6f, br_noise_hash3(i + 1, j + 1, k + 1) % 12, x3, y3, z3);
    return 32.0f * n;
}

/* ---- fBm */

static float br_noise_sample(br_noise_type type, float x, float y, float z, uint32_t seed, bool three_d)
{
    switch (type) {
    case BR_NOISE_VALUE:
        return br_noise_value3(x, y, z, seed);
    case BR_NOISE_PERLIN:
        return br_noise_perlin3(x, y, z, seed);
    case BR_NOISE_SIMPLEX:
        return three_d ? br_noise_simplex3(x, y, z, seed) : br_noise_simplex2(x, y, seed);
    }
    return 0.0f;
}

static float br_noise_fbm(br_noise_type type, float x, float y, float z, uint32_t seed, int octaves,
                          float lacunarity, float gain, bool three_d)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; i++) {
        /* each octave its own seed, so the lattice points of the layers do not line up */
        sum += amplitude * br_noise_sample(type, x * frequency, y * frequency, z * frequency, seed + (uint32_t)i, three_d);
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float br_noise_fbm2(br_noise_type type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain)
{
    return br_noise_fbm(type, x, y, 0.0f, seed, octaves, lacunarity, gain, false);
}

float br_noise_fbm3(br_noise_type type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity,
                    float gain)
{
    return br_noise_fbm(type, x, y, z, seed, octaves, lacunarity, gain, true);
}
