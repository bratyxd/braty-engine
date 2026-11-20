/* test_debug: the debug overlay survives its font being uninstalled or replaced. Needs a window
 * and a GPU (the overlay only draws then); skips (passes) without one. Freed memory is filled
 * with 0x01 bytes and never reused, so a dangling font pointer crashes instead of reading stale
 * glyphs (0x01 reads as bool true, every pointer becomes non-canonical). */
#include "br_test.h"

#include "braty/braty.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define POISON_HEADER 16

static void *poison_malloc(size_t size)
{
    unsigned char *p = malloc(size + POISON_HEADER);
    if (p == NULL) {
        return NULL;
    }
    SDL_memcpy(p, &size, sizeof(size));
    return p + POISON_HEADER;
}

static void *poison_calloc(size_t count, size_t size)
{
    if (size != 0 && count > (SIZE_MAX - POISON_HEADER) / size) {
        return NULL;
    }
    void *p = poison_malloc(count * size);
    if (p != NULL) {
        SDL_memset(p, 0, count * size);
    }
    return p;
}

static void poison_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    unsigned char *p = (unsigned char *)ptr - POISON_HEADER;
    size_t size;
    SDL_memcpy(&size, p, sizeof(size));
    SDL_memset(ptr, 0x01, size);
    (void)p; /* never reused: a later allocation would hide the stale read (leaks, fine for a test) */
}

static void *poison_realloc(void *ptr, size_t size)
{
    void *q = poison_malloc(size);
    if (q == NULL || ptr == NULL) {
        return q;
    }
    size_t old;
    SDL_memcpy(&old, (unsigned char *)ptr - POISON_HEADER, sizeof(old));
    SDL_memcpy(q, ptr, old < size ? old : size);
    poison_free(ptr);
    return q;
}

static void steps(br_engine *e, int n)
{
    for (int i = 0; i < n; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
}

static void test_font_replaced(void)
{
    br_engine *e = br_engine_create("test_debug", NULL);
    if (!br_engine_open_window(e, 320, 240, NULL)) {
        printf("skipped: no window / GPU\n");
        br_engine_destroy(e);
        return;
    }
    br_debug_set_open(e, true);
    steps(e, 2); /* installs "braty.debug" and draws with it */
    bool synth_bold, synth_italic;
    BR_CHECK(br_font_find(e, "braty.debug", false, false, &synth_bold, &synth_italic) != br_font_default(e));
    br_bitmap_font_uninstall(e, "braty.debug");
    steps(e, 2); /* must not draw with the freed font */
    br_text_style style = br_text_style_default();
    style.font_size = 20.0f;
    BR_CHECK(br_bitmap_font_install(e, "braty.debug", &style, NULL) != NULL);
    steps(e, 2); /* the game's replacement: the older font is freed */
    br_engine_destroy(e);
}

int main(void)
{
    BR_CHECK(SDL_SetMemoryFunctions(poison_malloc, poison_calloc, poison_realloc, poison_free));
    SDL_Init(0);
    test_font_replaced();
    SDL_Quit();
    return br_test_finish();
}
