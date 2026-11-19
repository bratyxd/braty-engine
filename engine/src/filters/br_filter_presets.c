/* br_filter_presets: filter constructors, colour-matrix presets (values from Pixi's
 * ColorMatrixFilter) and blur kernel weights. Pure maths, no engine or GPU. */
#include "braty/br_filter.h"

/* ---- constructors */

static br_filter br_filter_base(br_filter_type type)
{
    br_filter f;
    SDL_zero(f);
    f.type = type;
    f.enabled = true;
    f.alpha = 1.0f;
    br_color_matrix_identity(f.matrix);
    return f;
}

br_filter br_filter_alpha(float alpha)
{
    br_filter f = br_filter_base(BR_FILTER_ALPHA);
    f.alpha = alpha;
    return f;
}

br_filter br_filter_blur(float strength, int quality, int kernel_size)
{
    br_filter f = br_filter_base(BR_FILTER_BLUR);
    f.blur_x = strength;
    f.blur_y = strength;
    f.quality = quality > 0 ? quality : 1;
    f.kernel_size = kernel_size;
    f.padding = strength; /* the Gaussian is ~gone at `strength` world units */
    return f;
}

br_filter br_filter_color_matrix(void)
{
    return br_filter_base(BR_FILTER_COLOR_MATRIX);
}

br_filter br_filter_displacement(br_texture map, float scale_x, float scale_y)
{
    br_filter f = br_filter_base(BR_FILTER_DISPLACEMENT);
    f.map = map;
    f.scale = br_vec2_make(scale_x, scale_y);
    f.padding = SDL_max(SDL_fabsf(scale_x), SDL_fabsf(scale_y)) * 0.5f;
    return f;
}

br_filter br_filter_noise(float noise, float seed)
{
    br_filter f = br_filter_base(BR_FILTER_NOISE);
    f.noise = noise;
    f.seed = seed;
    return f;
}

br_filter br_filter_custom(const br_shader_blob *fragment_shader, const void *uniforms, uint32_t uniform_size)
{
    br_filter f = br_filter_base(BR_FILTER_CUSTOM);
    f.shader = fragment_shader;
    if (uniforms != NULL && uniform_size > 0) {
        if (uniform_size > BR_FILTER_MAX_UNIFORM_BYTES) {
            br_log_error("br_filter_custom: %u uniform bytes (max %u), truncated", uniform_size,
                         BR_FILTER_MAX_UNIFORM_BYTES);
            uniform_size = BR_FILTER_MAX_UNIFORM_BYTES;
        }
        SDL_memcpy(f.uniforms, uniforms, uniform_size);
        f.uniform_size = uniform_size;
    }
    return f;
}

/* ---- colour matrix */

void br_color_matrix_identity(float m[20])
{
    SDL_memset(m, 0, 20 * sizeof(float));
    m[0] = 1.0f;
    m[6] = 1.0f;
    m[12] = 1.0f;
    m[18] = 1.0f;
}

void br_color_matrix_multiply(float out[20], const float a[20], const float b[20])
{
    /* 4x5 matrices with an implicit last row (0 0 0 0 1); result applies b, then a */
    float r[20];
    for (int row = 0; row < 4; row++) {
        const float *ar = &a[row * 5];
        for (int col = 0; col < 5; col++) {
            float v = ar[0] * b[col] + ar[1] * b[5 + col] + ar[2] * b[10 + col] + ar[3] * b[15 + col];
            if (col == 4) {
                v += ar[4];
            }
            r[row * 5 + col] = v;
        }
    }
    SDL_memcpy(out, r, sizeof(r));
}

br_color br_color_matrix_apply(const float m[20], br_color c)
{
    float in[4] = { c.r, c.g, c.b, c.a };
    float out[4];
    for (int row = 0; row < 4; row++) {
        const float *mr = &m[row * 5];
        float v = mr[0] * in[0] + mr[1] * in[1] + mr[2] * in[2] + mr[3] * in[3] + mr[4];
        out[row] = br_clampf(v, 0.0f, 1.0f);
    }
    return br_color_make(out[0], out[1], out[2], out[3]);
}

/* Replaces m with `preset`, or combines them (m = m x preset). */
static void br_color_matrix_load(float m[20], const float preset[20], bool multiply)
{
    if (multiply) {
        br_color_matrix_multiply(m, m, preset);
    } else {
        SDL_memcpy(m, preset, 20 * sizeof(float));
    }
}

/* A preset that only scales r, g, b. */
static void br_color_matrix_diagonal(float m[20], float r, float g, float b, bool multiply)
{
    float p[20];
    br_color_matrix_identity(p);
    p[0] = r;
    p[6] = g;
    p[12] = b;
    br_color_matrix_load(m, p, multiply);
}

/* A preset given as its 3x3 rgb part plus rgb offsets (alpha row stays identity). */
static void br_color_matrix_rgb(float m[20], const float rgb[9], const float offset[3], bool multiply)
{
    float p[20];
    br_color_matrix_identity(p);
    for (int row = 0; row < 3; row++) {
        p[row * 5 + 0] = rgb[row * 3 + 0];
        p[row * 5 + 1] = rgb[row * 3 + 1];
        p[row * 5 + 2] = rgb[row * 3 + 2];
        p[row * 5 + 4] = offset != NULL ? offset[row] : 0.0f;
    }
    br_color_matrix_load(m, p, multiply);
}

void br_color_matrix_brightness(float m[20], float b, bool multiply)
{
    br_color_matrix_diagonal(m, b, b, b, multiply);
}

void br_color_matrix_tint(float m[20], br_color color, bool multiply)
{
    br_color_matrix_diagonal(m, color.r, color.g, color.b, multiply);
}

void br_color_matrix_greyscale(float m[20], float scale, bool multiply)
{
    const float rgb[9] = { scale, scale, scale, scale, scale, scale, scale, scale, scale };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_black_and_white(float m[20], bool multiply)
{
    const float rgb[9] = { 0.3f, 0.6f, 0.1f, 0.3f, 0.6f, 0.1f, 0.3f, 0.6f, 0.1f };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_hue(float m[20], float degrees, bool multiply)
{
    /* rotation around the grey axis (Pixi's hue) */
    float rad = degrees / 180.0f * SDL_PI_F;
    float c = SDL_cosf(rad);
    float s = SDL_sinf(rad);
    float w = 1.0f / 3.0f;
    float sw = SDL_sqrtf(w);
    float a00 = c + (1.0f - c) * w;
    float a01 = w * (1.0f - c) - sw * s;
    float a02 = w * (1.0f - c) + sw * s;
    const float rgb[9] = { a00, a01, a02, a02, a00, a01, a01, a02, a00 };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_contrast(float m[20], float amount, bool multiply)
{
    float v = amount + 1.0f;
    float o = -0.5f * (v - 1.0f);
    const float rgb[9] = { v, 0, 0, 0, v, 0, 0, 0, v };
    const float offset[3] = { o, o, o };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_saturate(float m[20], float amount, bool multiply)
{
    float x = amount * 2.0f / 3.0f + 1.0f;
    float y = (x - 1.0f) * -0.5f;
    const float rgb[9] = { x, y, y, y, x, y, y, y, x };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_desaturate(float m[20])
{
    br_color_matrix_saturate(m, -1.0f, false);
}

void br_color_matrix_negative(float m[20], bool multiply)
{
    /* r' = a - r: keeps premultiplied-style results inside 0..a */
    float p[20];
    br_color_matrix_identity(p);
    p[0] = -1.0f;
    p[3] = 1.0f;
    p[6] = -1.0f;
    p[8] = 1.0f;
    p[12] = -1.0f;
    p[13] = 1.0f;
    br_color_matrix_load(m, p, multiply);
}

void br_color_matrix_sepia(float m[20], bool multiply)
{
    const float rgb[9] = { 0.393f, 0.7689999f, 0.18899999f, 0.349f, 0.6859999f, 0.16799999f,
                           0.272f, 0.5339999f, 0.13099999f };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_technicolor(float m[20], bool multiply)
{
    const float rgb[9] = { 1.9125277891456083f, -0.8545344976951645f, -0.09155508482755585f,
                           -0.3087833385928097f, 1.7658908555458428f, -0.10601743074722245f,
                           -0.231103377548616f, -0.7501899197440212f, 1.847597816108189f };
    const float offset[3] = { 11.793603434377337f / 255.0f, -70.35205161461398f / 255.0f,
                              30.950940869491138f / 255.0f };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_polaroid(float m[20], bool multiply)
{
    const float rgb[9] = { 1.438f, -0.062f, -0.062f, -0.122f, 1.378f, -0.122f, -0.016f, -0.016f, 1.483f };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_to_bgr(float m[20], bool multiply)
{
    const float rgb[9] = { 0, 0, 1, 0, 1, 0, 1, 0, 0 };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_kodachrome(float m[20], bool multiply)
{
    const float rgb[9] = { 1.1285582396593525f, -0.3967382283601348f, -0.03992559172921793f,
                           -0.16404339962244616f, 1.0835251566291304f, -0.05498805115633132f,
                           -0.16786010706155763f, -0.5603416277695248f, 1.6014850761964943f };
    const float offset[3] = { 63.72958762196502f / 255.0f, 24.732407896706203f / 255.0f,
                              35.62982807460946f / 255.0f };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_browni(float m[20], bool multiply)
{
    const float rgb[9] = { 0.5997023498159715f, 0.34553243048391263f, -0.2708298674538042f,
                           -0.037703249837783157f, 0.8609577587992641f, 0.15059552388459913f,
                           0.24113635128153335f, -0.07441037908422492f, 0.44972182064877153f };
    const float offset[3] = { 47.43192855600873f / 255.0f, -36.96841498319127f / 255.0f,
                              -7.562075277591283f / 255.0f };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_vintage(float m[20], bool multiply)
{
    const float rgb[9] = { 0.6279345635605994f, 0.3202183420819367f, -0.03965408211312453f,
                           0.02578397704808868f, 0.6441188644374771f, 0.03259127616149294f,
                           0.0466055556782719f, -0.0851232987247891f, 0.5241648018700465f };
    const float offset[3] = { 9.651285835294123f / 255.0f, 7.462829176470591f / 255.0f,
                              5.159190588235296f / 255.0f };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_color_tone(float m[20], float desaturation, float toned, br_color light, br_color dark,
                                bool multiply)
{
    /* Pixi's colorTone, entry for entry (defaults there: 0.2, 0.15, #FFE580, #338000) */
    const float p[20] = {
        0.3f, 0.59f, 0.11f, 0.0f, 0.0f,
        light.r, light.g, light.b, desaturation, 0.0f,
        dark.r, dark.g, dark.b, toned, 0.0f,
        light.r - dark.r, light.g - dark.g, light.b - dark.b, 0.0f, 0.0f,
    };
    br_color_matrix_load(m, p, multiply);
}

void br_color_matrix_night(float m[20], float intensity, bool multiply)
{
    const float rgb[9] = { intensity * -2.0f, -intensity, 0.0f, -intensity, 0.0f, intensity,
                           0.0f, intensity, intensity * 2.0f };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

void br_color_matrix_predator(float m[20], float amount, bool multiply)
{
    const float rgb[9] = { 11.224130630493164f * amount, -4.794486999511719f * amount,
                           -2.8746118545532227f * amount, -3.6330697536468506f * amount,
                           9.193157196044922f * amount, -2.951810836791992f * amount,
                           -3.2184197902679443f * amount, -4.2375030517578125f * amount,
                           7.476448059082031f * amount };
    const float offset[3] = { 0.40342438220977783f * amount, -1.316135048866272f * amount,
                              0.8044459223747253f * amount };
    br_color_matrix_rgb(m, rgb, offset, multiply);
}

void br_color_matrix_lsd(float m[20], bool multiply)
{
    const float rgb[9] = { 2.0f, -0.4f, 0.5f, -0.5f, 2.0f, -0.4f, -0.4f, -0.5f, 3.0f };
    br_color_matrix_rgb(m, rgb, NULL, multiply);
}

/* ---- blur kernel */

uint32_t br_filter_blur_kernel(int kernel_size, float weights[8])
{
    if (kernel_size < 5) {
        kernel_size = 5;
    }
    if (kernel_size > 15) {
        kernel_size = 15;
    }
    if (kernel_size % 2 == 0) {
        kernel_size += 1;
    }
    uint32_t half = (uint32_t)kernel_size / 2;
    /* sigma so the last tap is ~2 sigma out, matching the shape of Pixi's tables */
    float sigma = (float)half / 2.0f;
    if (sigma < 1.0f) {
        sigma = 1.0f;
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i <= half; i++) {
        float x = (float)i;
        weights[i] = SDL_expf(-(x * x) / (2.0f * sigma * sigma));
        sum += i == 0 ? weights[i] : 2.0f * weights[i];
    }
    for (uint32_t i = 0; i <= half; i++) {
        weights[i] /= sum;
    }
    return half + 1;
}
