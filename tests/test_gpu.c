/* test_gpu: the GPU layer on a hidden window. Skips (passes) when no GPU device can be created. */
#include "br_test.h"

#include "braty/br_gpu.h"

#include <SDL3/SDL.h>

/* Offsets are 16-byte aligned; allocations after this frame's upload are refused (R1). */
static void test_transient(br_gpu *g)
{
    uint32_t a = 99, b = 99;
    BR_CHECK(br_gpu_transient_alloc_vertices(g, 3, &a) != NULL && a == 0);
    BR_CHECK(br_gpu_transient_alloc_vertices(g, 5, &b) != NULL && b == 16);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(br_gpu_device(g));
    BR_CHECK(cmd != NULL && br_gpu_transient_upload(g, cmd));
    SDL_SubmitGPUCommandBuffer(cmd);
    BR_CHECK(br_gpu_transient_alloc_vertices(g, 4, &a) == NULL); /* would never reach the GPU */
    BR_CHECK(br_gpu_transient_alloc_indices(g, 4, &a) == NULL);
}

/* Uploads are bounds-checked against the texture; readback returns RGBA, BGRA swizzled. */
static void test_textures(br_gpu *g)
{
    uint8_t pixels[4 * 4 * 4];
    SDL_memset(pixels, 0, sizeof(pixels));
    br_gpu_texture *t = br_gpu_texture_create_rgba8(g, 4, 4, pixels, false);
    BR_CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    uint8_t red[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        red[i * 4 + 0] = 255;
        red[i * 4 + 1] = 0;
        red[i * 4 + 2] = 0;
        red[i * 4 + 3] = 255;
    }
    BR_CHECK(!br_gpu_texture_upload(g, t, 3, 3, 2, 2, red));          /* past the right/bottom edge */
    BR_CHECK(!br_gpu_texture_upload(g, t, 0xFFFFFFFFu, 0, 2, 2, red)); /* x + width wraps */
    BR_CHECK(!br_gpu_texture_upload(g, t, 0, 0, 0, 2, red));          /* empty region */
    BR_CHECK(br_gpu_texture_upload(g, t, 1, 1, 2, 2, red));
    uint8_t *back = NULL;
    BR_CHECK(br_gpu_texture_read(g, t, &back) && back != NULL);
    if (back != NULL) {
        BR_CHECK(back[(1 * 4 + 1) * 4 + 0] == 255 && back[(1 * 4 + 1) * 4 + 3] == 255);
        BR_CHECK(back[0] == 0 && back[(3 * 4 + 3) * 4 + 0] == 0);
        br_free(back);
    }
    br_gpu_texture_destroy(g, t);

    br_gpu_texture *bgra = br_gpu_texture_create(g, 1, 1, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                                 SDL_GPU_TEXTUREUSAGE_SAMPLER, false);
    if (bgra != NULL) {
        uint8_t blue_red_green[4] = { 10, 20, 30, 255 }; /* B, G, R, A */
        BR_CHECK(br_gpu_texture_upload(g, bgra, 0, 0, 1, 1, blue_red_green));
        BR_CHECK(br_gpu_texture_read(g, bgra, &back));
        BR_CHECK(back != NULL && back[0] == 30 && back[1] == 20 && back[2] == 10 && back[3] == 255);
        br_free(back);
        br_gpu_texture_destroy(g, bgra);
    }
}

/* One sampler per style. */
static void test_samplers(br_gpu *g)
{
    br_texture_style linear;
    SDL_zero(linear);
    br_texture_style nearest = linear;
    nearest.filter = BR_FILTER_NEAREST;
    SDL_GPUSampler *a = br_gpu_sampler_get(g, &linear);
    BR_CHECK(a != NULL && a == br_gpu_sampler_get(g, &linear));
    BR_CHECK(br_gpu_sampler_get(g, &nearest) != a);
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("test_gpu: skipped, no video: %s\n", SDL_GetError());
        return 0;
    }
    SDL_Window *window = SDL_CreateWindow("test_gpu", 64, 64, SDL_WINDOW_HIDDEN);
    br_gpu *g = window != NULL ? br_gpu_create(window, NULL) : NULL;
    if (g == NULL) {
        printf("test_gpu: skipped, no GPU device: %s\n", SDL_GetError());
        if (window != NULL) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return 0;
    }
    test_transient(g);
    test_textures(g);
    test_samplers(g);
    br_gpu_destroy(g);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return br_test_finish();
}
