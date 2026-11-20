/* sprites_demo - nine-slice, tiling and animated sprites (+ a GIF).
 *
 *   sprites_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Row 1, nine-slice sprites of one 48x48 panel texture (12 px borders; red corners, striped
 *   edges, a dotted centre):
 *   stretch | edges + centre tiled | hidden centre | 20x16 (smaller than the borders: they
 *   shrink) and the same x3 | independent borders 4/12/20/8 | the panel as a rotated + trimmed
 *   atlas frame, tinted, anchored at its centre, rotating and resizing.
 * Row 2, tiling sprites: scrolling stripes | rotated + scaled checker | an atlas frame
 *   (rotated in the atlas) tiling with a scrolling offset | the atlas as stored, the tile frame
 *   x4 and the panel frame as plain sprites (orientation reference; the green dot is top-left).
 * Row 3, animated sprites: per-frame durations (frame 0 holds 0.3 s) | twice as fast, backwards
 *   | no loop, restarted by on_complete | a GIF written at startup and loaded as frames | a
 *   frame counter bar driven by on_frame_change.
 */
#include "braty/braty_main.h"

#include <SDL3/SDL_timer.h>

enum { PANEL = 48, BORDER = 12, ANIM_FRAMES = 8, ANIM_SIZE = 64, GIF_SIZE = 48, GIF_FRAMES = 6 };

typedef struct demo_state {
    br_texture panel;
    br_texture atlas;
    br_texture atlas_panel;     /* rotated + trimmed frame of atlas */
    br_texture atlas_tile;      /* rotated frame of atlas */
    br_texture stripes;
    br_texture checker;
    br_texture anim[ANIM_FRAMES];
    br_node    spinning_panel;
    br_node    scroll_tiles;
    br_node    rotating_tiles;
    br_node    atlas_tiles;
    br_node    one_shot;
    br_node    counter_bar;
    int        frame_changes;
} demo_state;

/* ---- generated textures */

static void put(uint8_t *p, int w, int x, int y, uint32_t rgba)
{
    uint8_t *px = &p[(y * w + x) * 4];
    px[0] = (uint8_t)(rgba >> 24);
    px[1] = (uint8_t)(rgba >> 16);
    px[2] = (uint8_t)(rgba >> 8);
    px[3] = (uint8_t)rgba;
}

/* The 48x48 panel: what each of the nine cells is, is visible at a glance. */
static void panel_pixels(uint8_t *p)
{
    for (int y = 0; y < PANEL; y++) {
        for (int x = 0; x < PANEL; x++) {
            bool edge_x = x < BORDER || x >= PANEL - BORDER;
            bool edge_y = y < BORDER || y >= PANEL - BORDER;
            uint32_t c;
            if (edge_x && edge_y) {
                int cx = x < BORDER ? x : PANEL - 1 - x;
                int cy = y < BORDER ? y : PANEL - 1 - y;
                /* the top-left dot is green: shows orientation (rotated atlas frames) */
                uint32_t dot = x < BORDER && y < BORDER ? 0x40FF40FFu : 0xFFFFFFFFu;
                c = (cx - 6) * (cx - 6) + (cy - 6) * (cy - 6) < 12 ? dot : 0xD04040FFu;
            } else if (edge_x || edge_y) {
                int along = edge_y ? x : y;
                c = (along / 4) % 2 == 0 ? 0x3070E0FFu : 0x90C0FFFFu;
                int across = edge_y ? (y < BORDER ? y : PANEL - 1 - y) : (x < BORDER ? x : PANEL - 1 - x);
                if (across < 2) {
                    c = 0x102040FFu;
                }
            } else {
                bool dot = ((x - BORDER) % 8 - 4) * ((x - BORDER) % 8 - 4) + ((y - BORDER) % 8 - 4) * ((y - BORDER) % 8 - 4) < 5;
                c = dot ? 0xF0D060FFu : 0x283040FFu;
            }
            put(p, PANEL, x, y, c);
        }
    }
}

/* 128x128 atlas: the panel stored rotated 90 degrees clockwise (TexturePacker convention) with
 * 4 px of transparent padding trimmed away, and a 16x24 striped tile, also rotated. */
static void make_atlas(br_engine *e, demo_state *s)
{
    enum { A = 128 };
    static uint8_t atlas[A * A * 4];
    static uint8_t panel[PANEL * PANEL * 4];
    SDL_memset(atlas, 0, sizeof(atlas));
    panel_pixels(panel);
    /* logical (lx, ly) of a w x h image -> atlas (x + h - 1 - ly, y + lx) */
    for (int ly = 0; ly < PANEL; ly++) {
        for (int lx = 0; lx < PANEL; lx++) {
            SDL_memcpy(&atlas[((0 + lx) * A + (64 + PANEL - 1 - ly)) * 4], &panel[(ly * PANEL + lx) * 4], 4);
        }
    }
    enum { TW = 16, TH = 24 };
    for (int ly = 0; ly < TH; ly++) {
        for (int lx = 0; lx < TW; lx++) {
            uint32_t c = (lx < 8) != (ly < 12) ? 0x40C080FFu : 0xE0F0E0FFu;
            if (lx == 0 || ly == 0) {
                c = 0x206040FFu;
            }
            put(atlas, A, 8 + TH - 1 - ly, 64 + lx, c);
        }
    }
    s->atlas = br_texture_create_rgba8(e, A, A, atlas, NULL);
    br_texture_frame f;
    SDL_zero(f);
    f.frame = br_rect_make(64, 0, PANEL, PANEL);
    f.rotated = true;
    f.orig = br_rect_make(0, 0, PANEL + 8, PANEL + 8);
    f.trim = br_rect_make(4, 4, PANEL, PANEL);
    s->atlas_panel = br_texture_create_frame(e, s->atlas, &f);
    SDL_zero(f);
    f.frame = br_rect_make(8, 64, TW, TH);
    f.rotated = true;
    s->atlas_tile = br_texture_create_frame(e, s->atlas, &f);
}

static br_texture make_stripes(br_engine *e)
{
    enum { S = 32 };
    uint8_t p[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            put(p, S, x, y, ((x + y) / 8) % 2 == 0 ? 0xE06020FFu : 0xF0C040FFu);
        }
    }
    return br_texture_create_rgba8(e, S, S, p, NULL);
}

static br_texture make_checker(br_engine *e)
{
    enum { S = 32 };
    uint8_t p[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            bool light = ((x / 16) + (y / 16)) % 2 == 0;
            put(p, S, x, y, light ? 0xE8E8F0FFu : (0x404080FFu + ((uint32_t)(x * 4) << 16)));
        }
    }
    return br_texture_create_rgba8(e, S, S, p, NULL);
}

/* Frame i of a spinner: a ring with one bright segment at i * 45 degrees. */
static br_texture make_anim_frame(br_engine *e, int i)
{
    static uint8_t p[ANIM_SIZE * ANIM_SIZE * 4];
    SDL_memset(p, 0, sizeof(p));
    for (int y = 0; y < ANIM_SIZE; y++) {
        for (int x = 0; x < ANIM_SIZE; x++) {
            float dx = (float)x - 31.5f;
            float dy = (float)y - 31.5f;
            float r = SDL_sqrtf(dx * dx + dy * dy);
            if (r < 18.0f || r > 30.0f) {
                continue;
            }
            float a = SDL_atan2f(dy, dx) / (2.0f * BR_PI) * (float)ANIM_FRAMES;
            int segment = ((int)SDL_floorf(a) + ANIM_FRAMES) % ANIM_FRAMES;
            put(p, ANIM_SIZE, x, y, segment == i ? 0xFFF060FFu : 0x506080FFu);
        }
    }
    /* a number of dots = frame index + 1, so screenshots show which frame is up */
    for (int d = 0; d <= i; d++) {
        for (int y = 29; y < 34; y++) {
            for (int x = 0; x < 3; x++) {
                put(p, ANIM_SIZE, 20 + d * 3 + x - (i * 3) / 2, y, d % 2 == 0 ? 0xFFFFFFFFu : 0x80FF80FFu);
            }
        }
    }
    return br_texture_create_rgba8(e, ANIM_SIZE, ANIM_SIZE, p, NULL);
}

/* ---- a tiny GIF writer (uncompressed LZW: 9-bit literal codes with regular clear codes) */

typedef struct gif_writer {
    uint8_t *bytes;
    size_t   size;
    size_t   capacity;
    uint32_t bit_buffer;
    int      bit_count;
    uint8_t  block[255];
    int      block_size;
} gif_writer;

static void gif_byte(gif_writer *g, uint8_t b)
{
    if (g->size == g->capacity) {
        g->capacity = g->capacity > 0 ? g->capacity * 2 : 4096;
        g->bytes = br_realloc(g->bytes, g->capacity);
    }
    g->bytes[g->size++] = b;
}

static void gif_u16(gif_writer *g, int v)
{
    gif_byte(g, (uint8_t)(v & 0xFF));
    gif_byte(g, (uint8_t)(v >> 8));
}

static void gif_flush_block(gif_writer *g)
{
    if (g->block_size > 0) {
        gif_byte(g, (uint8_t)g->block_size);
        for (int i = 0; i < g->block_size; i++) {
            gif_byte(g, g->block[i]);
        }
        g->block_size = 0;
    }
}

static void gif_code(gif_writer *g, uint32_t code)
{
    g->bit_buffer |= code << g->bit_count;
    g->bit_count += 9;
    while (g->bit_count >= 8) {
        g->block[g->block_size++] = (uint8_t)(g->bit_buffer & 0xFF);
        g->bit_buffer >>= 8;
        g->bit_count -= 8;
        if (g->block_size == 255) {
            gif_flush_block(g);
        }
    }
}

/* Writes a looping GIF of GIF_FRAMES frames (a bouncing ball) with uneven delays. */
static bool write_gif(br_engine *e, const char *path)
{
    gif_writer g;
    SDL_zero(g);
    const char *header = "GIF89a";
    for (int i = 0; i < 6; i++) {
        gif_byte(&g, (uint8_t)header[i]);
    }
    gif_u16(&g, GIF_SIZE);
    gif_u16(&g, GIF_SIZE);
    gif_byte(&g, 0xF7); /* global colour table, 256 entries */
    gif_byte(&g, 0);
    gif_byte(&g, 0);
    for (int i = 0; i < 256; i++) { /* palette: 0 = dark blue, 1 = white, 2.. = a red ramp */
        gif_byte(&g, (uint8_t)(i == 0 ? 20 : i == 1 ? 255 : i));
        gif_byte(&g, (uint8_t)(i == 0 ? 30 : i == 1 ? 255 : i / 4));
        gif_byte(&g, (uint8_t)(i == 0 ? 70 : i == 1 ? 255 : 40));
    }
    const uint8_t loop_ext[] = { 0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0 };
    for (size_t i = 0; i < sizeof(loop_ext); i++) {
        gif_byte(&g, loop_ext[i]);
    }
    for (int f = 0; f < GIF_FRAMES; f++) {
        int delay_cs = f == 0 ? 40 : 8; /* centiseconds */
        gif_byte(&g, 0x21);
        gif_byte(&g, 0xF9);
        gif_byte(&g, 4);
        gif_byte(&g, 0);
        gif_u16(&g, delay_cs);
        gif_byte(&g, 0);
        gif_byte(&g, 0);
        gif_byte(&g, 0x2C);
        gif_u16(&g, 0);
        gif_u16(&g, 0);
        gif_u16(&g, GIF_SIZE);
        gif_u16(&g, GIF_SIZE);
        gif_byte(&g, 0);
        gif_byte(&g, 8); /* LZW minimum code size */
        float by = 10.0f + 28.0f * SDL_fabsf(SDL_sinf((float)f / GIF_FRAMES * BR_PI));
        int written = 0;
        for (int y = 0; y < GIF_SIZE; y++) {
            for (int x = 0; x < GIF_SIZE; x++) {
                if (written % 128 == 0) {
                    gif_code(&g, 256); /* clear: keeps codes 9 bits wide */
                }
                float dx = (float)x - 24.0f;
                float dy = (float)y - by;
                uint32_t idx = dx * dx + dy * dy < 64.0f ? (uint32_t)(120 + f * 20) : (y == 46 ? 1u : 0u);
                gif_code(&g, idx);
                written++;
            }
        }
        gif_code(&g, 257); /* end of information */
        if (g.bit_count > 0) {
            g.block[g.block_size++] = (uint8_t)(g.bit_buffer & 0xFF);
            g.bit_buffer = 0;
            g.bit_count = 0;
        }
        gif_flush_block(&g);
        gif_byte(&g, 0);
    }
    gif_byte(&g, 0x3B);
    bool ok = g.bytes != NULL && br_file_write_all(e, path, g.bytes, g.size);
    br_free(g.bytes);
    return ok;
}

/* ---- callbacks */

static void restart_on_complete(br_engine *e, br_node sprite, void *user)
{
    (void)user;
    br_animated_sprite_goto_and_play(e, sprite, 0);
}

static void count_frame_change(br_engine *e, br_node sprite, void *user)
{
    (void)sprite;
    demo_state *s = user;
    s->frame_changes += 1;
    br_node_set_scale(e, s->counter_bar, br_vec2_make((float)(s->frame_changes % 40) * 4.0f, 6.0f));
}

/* ---- setup */

static br_node add_nine(br_engine *e, br_texture t, float x, float y, float w, float h)
{
    br_node n = br_nine_slice_create(e, t, BORDER, BORDER, BORDER, BORDER);
    br_nine_slice_set_size(e, n, w, h);
    br_node_set_position(e, n, br_vec2_make(x, y));
    br_node_add_child(e, br_stage_root(e), n);
    return n;
}

static void setup_nine_slices(br_engine *e, demo_state *s)
{
    add_nine(e, s->panel, 20, 20, 220, 120);

    br_node tiled = add_nine(e, s->panel, 260, 20, 220, 120);
    br_nine_slice *ns = br_nine_slice_get(e, tiled);
    ns->edge_fill = BR_NINE_SLICE_TILE;
    ns->center_fill = BR_NINE_SLICE_TILE;

    br_node frame_only = add_nine(e, s->panel, 500, 20, 140, 120);
    br_nine_slice_get(e, frame_only)->hide_center = true;

    add_nine(e, s->panel, 660, 20, 20, 16);           /* smaller than 12 + 12: borders shrink */
    br_node big = add_nine(e, s->panel, 660, 56, 20, 16);
    br_node_set_scale(e, big, br_vec2_make(3.0f, 3.0f));

    br_node uneven = add_nine(e, s->panel, 770, 20, 150, 120);
    br_nine_slice_set_borders(e, uneven, 4, 12, 20, 8);

    /* snippet: docs/agents/sprites.md */
    br_node n = br_nine_slice_create(e, s->atlas_panel, 12, 12, 12, 12);
    br_nine_slice *atlas_ns = br_nine_slice_get(e, n);
    atlas_ns->width = 200;
    atlas_ns->height = 110;
    atlas_ns->anchor = br_vec2_make(0.5f, 0.5f);
    atlas_ns->edge_fill = BR_NINE_SLICE_TILE;
    br_node_set_tint(e, n, br_color_from_hex(0xA0FFA0, 1.0f));
    br_node_set_position(e, n, br_vec2_make(1080, 80));
    br_node_add_child(e, br_stage_root(e), n);
    /* end snippet */
    s->spinning_panel = n;
}

static void setup_tiling(br_engine *e, demo_state *s)
{
    s->scroll_tiles = br_tiling_sprite_create(e, s->stripes, 300, 150);
    br_node_set_position(e, s->scroll_tiles, br_vec2_make(20, 180));
    br_node_add_child(e, br_stage_root(e), s->scroll_tiles);

    s->rotating_tiles = br_tiling_sprite_create(e, s->checker, 300, 150);
    br_tiling_sprite *ts = br_tiling_sprite_get(e, s->rotating_tiles);
    ts->tile_scale = br_vec2_make(1.5f, 0.75f);
    br_node_set_position(e, s->rotating_tiles, br_vec2_make(340, 180));
    br_node_add_child(e, br_stage_root(e), s->rotating_tiles);

    s->atlas_tiles = br_tiling_sprite_create(e, s->atlas_tile, 300, 150);
    br_node_set_position(e, s->atlas_tiles, br_vec2_make(660, 180));
    br_node_add_child(e, br_stage_root(e), s->atlas_tiles);

    /* the whole atlas, for reference: the rotated panel and tile as stored */
    br_node ref = br_sprite_create(e, s->atlas);
    br_node_set_position(e, ref, br_vec2_make(990, 180));
    br_node_add_child(e, br_stage_root(e), ref);
    /* both frames as plain sprites (the sprite type's frame handling is the reference) */
    br_node tile_ref = br_sprite_create(e, s->atlas_tile);
    br_node_set_position(e, tile_ref, br_vec2_make(1130, 180));
    br_node_set_scale(e, tile_ref, br_vec2_make(4.0f, 4.0f));
    br_node_add_child(e, br_stage_root(e), tile_ref);
    br_node panel_ref = br_sprite_create(e, s->atlas_panel);
    br_node_set_position(e, panel_ref, br_vec2_make(990, 260));
    br_node_add_child(e, br_stage_root(e), panel_ref);
}

static void setup_animation(br_engine *e, demo_state *s)
{
    br_anim_frame frames[ANIM_FRAMES];
    for (int i = 0; i < ANIM_FRAMES; i++) {
        frames[i].texture = s->anim[i];
        frames[i].duration = i == 0 ? 0.3f : 0.08f;
    }
    br_node a = br_animated_sprite_create(e, frames, ANIM_FRAMES);
    br_node_set_position(e, a, br_vec2_make(40, 380));
    br_node_add_child(e, br_stage_root(e), a);
    br_animated_sprite_set_callbacks(e, a, NULL, count_frame_change, NULL, s);
    br_animated_sprite_play(e, a);

    br_node b = br_animated_sprite_create_uniform(e, s->anim, ANIM_FRAMES, 12.0f);
    br_node_set_position(e, b, br_vec2_make(140, 380));
    br_node_add_child(e, br_stage_root(e), b);
    br_animated_sprite_set_speed(e, b, -2.0f);
    br_animated_sprite_play(e, b);

    s->one_shot = br_animated_sprite_create_uniform(e, s->anim, ANIM_FRAMES, 20.0f);
    br_node_set_position(e, s->one_shot, br_vec2_make(240, 380));
    br_node_set_tint(e, s->one_shot, br_color_from_hex(0xFF8080, 1.0f));
    br_node_add_child(e, br_stage_root(e), s->one_shot);
    br_animated_sprite_set_loop(e, s->one_shot, false);
    br_animated_sprite_set_callbacks(e, s->one_shot, restart_on_complete, NULL, NULL, s);
    br_animated_sprite_play(e, s->one_shot);

    if (write_gif(e, "sprites_demo.gif")) {
        const br_frame_set *gif = br_frame_set_load(e, "sprites_demo.gif");
        if (gif != NULL) {
            br_node g = br_animated_sprite_create(e, gif->frames, gif->count);
            br_node_set_position(e, g, br_vec2_make(340, 380));
            br_node_set_scale(e, g, br_vec2_make(1.5f, 1.5f));
            br_node_add_child(e, br_stage_root(e), g);
            br_animated_sprite_play(e, g);
            br_log_info("sprites_demo: GIF %ux%u, %u frames, first delay %.2f s", gif->width, gif->height,
                        gif->count, (double)gif->frames[0].duration);
        }
    }

    s->counter_bar = br_sprite_create(e, br_texture_white(e));
    br_node_set_position(e, s->counter_bar, br_vec2_make(40, 460));
    br_node_set_tint(e, s->counter_bar, br_color_from_hex(0x60E0FF, 1.0f));
    br_node_add_child(e, br_stage_root(e), s->counter_bar);
}

static void animate_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);
    br_node_set_rotation(e, s->spinning_panel, SDL_sinf(t * 0.7f) * 0.3f);
    br_nine_slice_set_size(e, s->spinning_panel, 170.0f + 50.0f * SDL_sinf(t), 110.0f);
    br_tiling_sprite_get(e, s->scroll_tiles)->tile_position = br_vec2_make(t * 40.0f, t * 10.0f);
    br_tiling_sprite_get(e, s->rotating_tiles)->tile_rotation = t * 0.4f + 0.3f;
    br_tiling_sprite_get(e, s->atlas_tiles)->tile_position = br_vec2_make(-t * 25.0f, 0.0f);
}

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    if (br_engine_gpu(e) != NULL) { /* NULL under --headless: nodes still update, nothing is drawn */
        br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x181C28, 1.0f));
    }
    static uint8_t panel[PANEL * PANEL * 4];
    panel_pixels(panel);
    s->panel = br_texture_create_rgba8(e, PANEL, PANEL, panel, NULL);
    make_atlas(e, s);
    s->stripes = make_stripes(e);
    s->checker = make_checker(e);
    for (int i = 0; i < ANIM_FRAMES; i++) {
        s->anim[i] = make_anim_frame(e, i);
    }
    setup_nine_slices(e, s);
    setup_tiling(e, s);
    setup_animation(e, s);
    br_system_add(e, "demo.animate", BR_PHASE_UPDATE, 0, animate_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - sprites demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
