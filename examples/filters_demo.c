/* filters_demo - every filter, mask and effect of the filters module on the same sprite group.
 *
 *   filters_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *                [--resize-at N]      resize the window to 1000x640 at frame N (targets are recreated)
 *                [--extract out.png]  write the generated texture of cell 18 as a PNG (frame 4)
 *
 * Grid, left to right, top to bottom (7 per row):
 *   0 no filter        1 alpha 0.4         2 blur 3           3 blur 10          4 greyscale
 *   5 sepia            6 hue 180           7 negative         8 polaroid x contrast
 *   9 displacement    10 noise            11 blur + hue (two chained filters)
 *  12 sepia parent with a blurred child   13 alpha mask       14 inverse alpha mask
 *  15 colour mask     16 cache_as_texture (rotating cached quad)  17 custom filter (wave)
 *  18 br_generate_texture of cell 0       19 blur limited to a filter area  20 blur at half resolution
 * Bottom row: the 21 advanced blend modes (BR_BLEND_COLOR .. BR_BLEND_VIVID_LIGHT) over a gradient.
 */
#include "braty/braty_main.h"

#include "filter_wave.frag.h"

/* Command-line options: br_main runs before the engine exists, so they go through file statics. */
static int g_resize_at = -1;
static const char *g_extract_path = NULL;

enum { DESIGN_W = 1280, DESIGN_H = 800, CELL_W = 180, CELL_H = 170, MAX_SPINNERS = 64 };

typedef struct demo_state {
    br_texture disc;
    br_texture white;
    br_texture soft_circle;
    br_texture rainbow;
    br_texture displacement_map;
    br_texture backdrop;
    br_node    stage;              /* scaled to fit the window */
    br_node    spinners[MAX_SPINNERS];
    uint32_t   spinner_count;
    br_node    first_card;         /* source of the generated texture */
    br_node    generated;          /* sprite showing it */
    br_texture generated_texture;
    br_node    cached;             /* cache_as_texture card, rotated every frame */
} demo_state;

/* ---- generated textures */

static void put_pixel(uint8_t *p, int w, int x, int y, br_color c)
{
    uint8_t *px = &p[(y * w + x) * 4];
    px[0] = (uint8_t)(br_clampf(c.r, 0.0f, 1.0f) * 255.0f);
    px[1] = (uint8_t)(br_clampf(c.g, 0.0f, 1.0f) * 255.0f);
    px[2] = (uint8_t)(br_clampf(c.b, 0.0f, 1.0f) * 255.0f);
    px[3] = (uint8_t)(br_clampf(c.a, 0.0f, 1.0f) * 255.0f);
}

/* A disc with diagonal hue stripes and a dark rim. */
static br_texture make_disc(br_engine *e)
{
    enum { S = 96 };
    static uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = ((float)x + 0.5f - 48.0f) / 48.0f;
            float dy = ((float)y + 0.5f - 48.0f) / 48.0f;
            float d = SDL_sqrtf(dx * dx + dy * dy);
            float a = br_clampf((1.0f - d) * 24.0f, 0.0f, 1.0f);
            int stripe = ((x + y) / 12) % 6;
            br_color c = br_color_from_hsl((float)stripe * 60.0f, 0.85f, d > 0.88f ? 0.25f : 0.55f, a);
            put_pixel(pixels, S, x, y, c);
        }
    }
    return br_texture_create_rgba8(e, S, S, pixels, NULL);
}

/* White disc with alpha falling off from the centre (alpha-mask shape). */
static br_texture make_soft_circle(br_engine *e)
{
    enum { S = 64 };
    static uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = ((float)x + 0.5f - 32.0f) / 32.0f;
            float dy = ((float)y + 0.5f - 32.0f) / 32.0f;
            float a = br_clampf((1.0f - SDL_sqrtf(dx * dx + dy * dy)) * 3.0f, 0.0f, 1.0f);
            put_pixel(pixels, S, x, y, br_color_make(1.0f, 1.0f, 1.0f, a));
        }
    }
    return br_texture_create_rgba8(e, S, S, pixels, NULL);
}

/* Red, green, blue and white vertical bands (colour-mask shape). */
static br_texture make_rainbow(br_engine *e)
{
    enum { W = 64, H = 4 };
    static uint8_t pixels[W * H * 4];
    const br_color bands[4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            put_pixel(pixels, W, x, y, bands[x / 16]);
        }
    }
    return br_texture_create_rgba8(e, W, H, pixels, NULL);
}

/* Smooth red/green waves, tiled (displacement map). */
static br_texture make_displacement_map(br_engine *e)
{
    enum { S = 64 };
    static uint8_t pixels[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float r = 0.5f + 0.5f * SDL_sinf((float)y / (float)S * 2.0f * SDL_PI_F * 2.0f);
            float g = 0.5f + 0.5f * SDL_cosf((float)x / (float)S * 2.0f * SDL_PI_F * 2.0f);
            put_pixel(pixels, S, x, y, br_color_make(r, g, 0.5f, 1.0f));
        }
    }
    br_texture_style style = { BR_FILTER_LINEAR, BR_ADDRESS_REPEAT, false, 1.0f };
    return br_texture_create_rgba8(e, S, S, pixels, &style);
}

/* Hue across, dark to light down (blend-mode backdrop). */
static br_texture make_backdrop(br_engine *e)
{
    enum { W = 128, H = 32 };
    static uint8_t pixels[W * H * 4];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float l = 0.15f + 0.7f * (float)y / (float)(H - 1);
            put_pixel(pixels, W, x, y, br_color_from_hsl((float)x * 360.0f / (float)W, 0.7f, l, 1.0f));
        }
    }
    return br_texture_create_rgba8(e, W, H, pixels, NULL);
}

/* ---- the test sprite group */

static br_node add_sprite(br_engine *e, br_node parent, br_texture t, float x, float y, float w, float h)
{
    br_node s = br_sprite_create(e, t);
    br_sprite_set_anchor(e, s, 0.5f, 0.5f);
    br_sprite_set_size(e, s, w, h);
    br_node_set_position(e, s, br_vec2_make(x, y));
    br_node_add_child(e, parent, s);
    return s;
}

/* A disc, a spinning yellow square and a red dot, centred on (x, y). */
static br_node make_card(br_engine *e, demo_state *d, br_node parent, float x, float y)
{
    br_node card = br_node_create_container(e);
    br_node_set_position(e, card, br_vec2_make(x, y));
    br_node_add_child(e, parent, card);
    add_sprite(e, card, d->disc, 0.0f, 0.0f, 96.0f, 96.0f);
    br_node square = add_sprite(e, card, d->white, 20.0f, -20.0f, 40.0f, 40.0f);
    br_node_set_tint(e, square, br_color_from_hex(0xFFE040, 1.0f));
    br_node dot = add_sprite(e, card, d->soft_circle, -24.0f, 26.0f, 30.0f, 30.0f);
    br_node_set_tint(e, dot, br_color_from_hex(0xFF3030, 1.0f));
    if (d->spinner_count < MAX_SPINNERS) {
        d->spinners[d->spinner_count++] = square;
    }
    return card;
}

static br_node make_cell_card(br_engine *e, demo_state *d, int cell)
{
    float x = (float)(cell % 7) * CELL_W + CELL_W * 0.5f + 10.0f;
    float y = (float)(cell / 7) * CELL_H + CELL_H * 0.5f + 10.0f;
    return make_card(e, d, d->stage, x, y);
}

static void set_one_filter(br_engine *e, br_node n, br_filter f)
{
    br_filters_set(e, n, &f, 1);
}

/* ---- setup */

typedef struct wave_uniforms {
    float wave[4];   /* amplitude (pixels), waves across, speed */
    float tint[4];
} wave_uniforms;

static void build_grid(br_engine *e, demo_state *d)
{
    br_node c;
    br_filter f;

    d->first_card = make_cell_card(e, d, 0);
    set_one_filter(e, make_cell_card(e, d, 1), br_filter_alpha(0.4f));
    set_one_filter(e, make_cell_card(e, d, 2), br_filter_blur(3.0f, 4, 5));
    set_one_filter(e, make_cell_card(e, d, 3), br_filter_blur(10.0f, 4, 9));

    f = br_filter_color_matrix();
    br_color_matrix_greyscale(f.matrix, 0.33f, false);
    set_one_filter(e, make_cell_card(e, d, 4), f);
    br_color_matrix_sepia(f.matrix, false);
    set_one_filter(e, make_cell_card(e, d, 5), f);
    br_color_matrix_hue(f.matrix, 180.0f, false);
    set_one_filter(e, make_cell_card(e, d, 6), f);
    br_color_matrix_negative(f.matrix, false);
    set_one_filter(e, make_cell_card(e, d, 7), f);
    br_color_matrix_polaroid(f.matrix, false);
    br_color_matrix_contrast(f.matrix, 0.5f, true);
    set_one_filter(e, make_cell_card(e, d, 8), f);

    set_one_filter(e, make_cell_card(e, d, 9), br_filter_displacement(d->displacement_map, 14.0f, 14.0f));
    set_one_filter(e, make_cell_card(e, d, 10), br_filter_noise(0.5f, 3.7f));

    /* [snippet: chained filters] */
    br_filter chain[2];
    chain[0] = br_filter_blur(4.0f, 3, 7);
    chain[1] = br_filter_color_matrix();
    br_color_matrix_hue(chain[1].matrix, 90.0f, false);
    br_filters_set(e, make_cell_card(e, d, 11), chain, 2);
    /* [/snippet] */

    /* filtered child inside a filtered parent */
    c = make_cell_card(e, d, 12);
    f = br_filter_color_matrix();
    br_color_matrix_sepia(f.matrix, false);
    set_one_filter(e, c, f);
    set_one_filter(e, d->spinners[d->spinner_count - 1], br_filter_blur(5.0f, 3, 7));

    /* [snippet: alpha mask] */
    c = make_cell_card(e, d, 13);
    br_node mask = add_sprite(e, c, d->soft_circle, 8.0f, 8.0f, 80.0f, 80.0f);
    br_node_set_mask(e, c, mask, BR_MASK_ALPHA, false);
    /* [/snippet] */
    c = make_cell_card(e, d, 14);
    mask = add_sprite(e, c, d->soft_circle, 8.0f, 8.0f, 80.0f, 80.0f);
    br_node_set_mask(e, c, mask, BR_MASK_ALPHA, true);
    c = make_cell_card(e, d, 15);
    mask = add_sprite(e, c, d->rainbow, 0.0f, 0.0f, 104.0f, 104.0f);
    br_node_set_mask(e, c, mask, BR_MASK_COLOR, false);

    d->cached = make_cell_card(e, d, 16);
    br_node_set_cache_as_texture(e, d->cached, true);

    /* [snippet: custom filter] */
    wave_uniforms wave = { { 6.0f, 2.0f, 3.0f, 0.0f }, { 0.7f, 1.0f, 1.0f, 1.0f } };
    f = br_filter_custom(&br_shader_filter_wave_frag, &wave, sizeof(wave));
    f.padding = 8.0f;
    set_one_filter(e, make_cell_card(e, d, 17), f);
    /* [/snippet] */

    d->generated = add_sprite(e, d->stage, br_texture_white(e), CELL_W * 4.5f + 10.0f, CELL_H * 2.5f + 10.0f,
                              4.0f, 4.0f);

    c = make_cell_card(e, d, 19);
    set_one_filter(e, c, br_filter_blur(8.0f, 4, 7));
    br_rect half = br_rect_make(-55.0f, -55.0f, 55.0f, 110.0f); /* left half only */
    br_filters_set_area(e, c, &half);

    c = make_cell_card(e, d, 20);
    set_one_filter(e, c, br_filter_blur(8.0f, 4, 7));
    br_filters_set_resolution(e, c, 0.5f);
}

static void build_blend_row(br_engine *e, demo_state *d)
{
    float top = 3.0f * CELL_H + 30.0f;
    add_sprite(e, d->stage, d->backdrop, DESIGN_W * 0.5f, top + 100.0f, DESIGN_W - 20.0f, 200.0f);
    for (int i = 0; i < BR_BLEND_ADVANCED_END - BR_BLEND_ADVANCED_FIRST; i++) {
        float x = 10.0f + 30.0f + (float)i * 60.0f;
        float y = top + 50.0f + (float)(i % 2) * 100.0f;
        br_node card = make_card(e, d, d->stage, x, y);
        br_node_set_scale(e, card, br_vec2_make(0.6f, 0.6f));
        br_node_set_blend_mode(e, card, BR_BLEND_ADVANCED_FIRST + i);
    }
}

/* ---- per frame */

static void demo_update(br_engine *e, float dt, void *user)
{
    demo_state *d = user;
    uint64_t frame = br_ticker_frame(e);
    for (uint32_t i = 0; i < d->spinner_count; i++) {
        br_node_set_rotation(e, d->spinners[i], br_node_rotation(e, d->spinners[i]) + dt * 1.5f);
    }
    /* the cached card keeps its first frame's content but follows its transform */
    br_node_set_rotation(e, d->cached, (float)br_ticker_elapsed(e) * 0.8f);

    int w, h;
    br_engine_window_size(e, &w, &h);
    float fit = SDL_min((float)w / DESIGN_W, (float)h / DESIGN_H);
    if (fit > 0.0f) {
        br_node_set_scale(e, d->stage, br_vec2_make(fit, fit));
    }
    if (g_resize_at >= 0 && frame == (uint64_t)g_resize_at) {
        SDL_SetWindowSize(br_engine_window(e), 1000, 640);
    }
    if (frame == 2) {
        /* [snippet: generate texture] */
        d->generated_texture = br_generate_texture(e, d->first_card, NULL);
        br_sprite_set_texture(e, d->generated, d->generated_texture);
        br_sprite_set_size(e, d->generated, 120.0f, 120.0f);
        /* [/snippet] */
    }
    if (frame == 4 && g_extract_path != NULL) {
        br_extract_image_png(e, d->generated_texture, g_extract_path);
    }
}

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *d = br_alloc(sizeof(demo_state));
    if (d == NULL) {
        return false;
    }
    SDL_zerop(d);
    br_engine_on_shutdown(e, demo_shutdown, d);
    br_gpu *g = br_engine_gpu(e);
    if (g != NULL) {
        br_gpu_set_clear_color(g, br_color_from_hex(0x30343C, 1.0f));
    }
    d->disc = make_disc(e);
    d->white = br_texture_white(e);
    d->soft_circle = make_soft_circle(e);
    d->rainbow = make_rainbow(e);
    d->displacement_map = make_displacement_map(e);
    d->backdrop = make_backdrop(e);
    d->stage = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), d->stage);
    build_grid(e, d);
    build_blend_row(e, d);
    br_system_add(e, "filters_demo.update", BR_PHASE_UPDATE, 0, demo_update, d);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--resize-at") == 0 && i + 1 < argc) {
            g_resize_at = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            g_extract_path = argv[++i];
        }
    }
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - filters";
    desc.width = DESIGN_W;
    desc.height = DESIGN_H;
    desc.init = demo_init;
    return desc;
}
