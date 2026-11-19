/* text_demo - a tour of the text module.
 *
 *   text_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Left column: the default font at several sizes, stroke, drop shadow, gradient fill, letter
 * spacing, non-ASCII UTF-8, rich-text tags. Middle: one paragraph wrapped with each alignment.
 * Right: bitmap fonts (one generated with br_bitmap_font_install, the hand-made
 * examples/fonts/pixel5x7.fnt and its XML twin), SDF text scaled up next to coverage text
 * scaled up, and split text with per-character transforms (animated).
 */
#include "braty/braty_main.h"

typedef struct demo_state {
    br_node  split_chars;   /* container from br_split_text_create */
    br_node  sdf_text;
    br_font *sdf_font;
} demo_state;

static br_text_style white_style(float size)
{
    br_text_style st = br_text_style_default();
    st.font_size = size;
    st.fill = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    return st;
}

static br_node add_text(br_engine *e, const char *text, const br_text_style *st, float x, float y)
{
    br_node n = br_text_create(e, text, st);
    br_node_set_position(e, n, x, y);
    br_node_add_child(e, br_scene_root(e), n);
    return n;
}

/* A flat rectangle behind things (the white texture tinted). */
static void add_panel(br_engine *e, float x, float y, float w, float h, uint32_t rgb)
{
    br_node p = br_sprite_create(e, br_texture_white(e));
    br_node_set_position(e, p, x, y);
    br_sprite_set_size(e, p, w, h);
    br_node_set_tint(e, p, br_color_from_hex(rgb, 1.0f));
    br_node_add_child(e, br_scene_root(e), p);
}

static void setup_left_column(br_engine *e)
{
    float sizes[5] = { 12.0f, 18.0f, 26.0f, 40.0f, 56.0f };
    float y = 10.0f;
    for (int i = 0; i < 5; i++) {
        char label[64];
        SDL_snprintf(label, sizeof(label), "Braty text %dpx", (int)sizes[i]);
        br_text_style st = white_style(sizes[i]);
        add_text(e, label, &st, 16.0f, y);
        y += sizes[i] * 1.4f;
    }

    /* stroke */
    br_text_style stroke = white_style(48.0f);
    stroke.fill = br_color_from_hex(0xffd23f, 1.0f);
    stroke.stroke_width = 6.0f;
    stroke.stroke_color = br_color_from_hex(0x7a2e00, 1.0f);
    add_text(e, "Stroke", &stroke, 16.0f, 232.0f);

    /* drop shadow with blur */
    br_text_style shadow = white_style(48.0f);
    shadow.drop_shadow = true;
    shadow.shadow_color = br_color_from_hex(0x000000, 0.8f);
    shadow.shadow_distance = 6.0f;
    shadow.shadow_blur = 4.0f;
    shadow.shadow_angle = SDL_PI_F / 4.0f;
    add_text(e, "Shadow", &shadow, 190.0f, 232.0f);

    /* snippet: docs/agents/text.md */
    br_text_style st = br_text_style_default();
    st.font_size = 48.0f;
    st.bold = true;
    br_text_style_set_vertical_gradient(&st, br_color_from_hex(0xfff27a, 1.0f), br_color_from_hex(0xff3d7f, 1.0f));
    st.gradient.stop_count = 3;
    st.gradient.offsets[1] = 0.5f;
    st.gradient.colors[1] = br_color_from_hex(0xff9a3d, 1.0f);
    st.gradient.offsets[2] = 1.0f;
    st.gradient.colors[2] = br_color_from_hex(0x9b3dff, 1.0f);
    st.stroke_width = 3.0f;
    st.stroke_color = br_color_from_hex(0x1a0a2a, 1.0f);
    br_node title = br_text_create(e, "Gradient", &st);
    br_node_set_position(e, title, 16.0f, 296.0f);
    br_node_add_child(e, br_scene_root(e), title);
    /* end snippet */

    br_text_style spaced = white_style(26.0f);
    spaced.letter_spacing = 9.0f;
    spaced.fill = br_color_from_hex(0x8fd3ff, 1.0f);
    add_text(e, "LETTER SPACING", &spaced, 16.0f, 364.0f);

    br_text_style intl = white_style(24.0f);
    add_text(e, "\xC3\x9Cn\xC3\xAF""c\xC3\xB6""d\xC3\xA9 \xE2\x82\xAC\xC2\xA3\xC2\xA5 \xC2\xA9 \xC2\xBD", &intl, 16.0f, 404.0f);
    add_text(e, "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC \xE2\x80\x94 "
                "\xD0\x9A\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD1\x86\xD0\xB0",
             &intl, 16.0f, 436.0f);

    /* rich text with a named tag style */
    br_text_style rich = white_style(22.0f);
    rich.word_wrap = true;
    rich.wrap_width = 420.0f;
    br_node r = br_rich_text_create(e,
                                    "Rich: <b>bold</b>, <i>italic</i>, <b><i>both</i></b>, <u>underline</u>, "
                                    "<s>strike</s>, <color=#ff6060>red</color>, <size=34>big</size>,<br>"
                                    "<font=Pixel5x7>PIXEL FONT</font> and a <title>named tag style</title> &lt;3",
                                    &rich);
    br_rich_tag_style title_tag;
    SDL_zero(title_tag);
    title_tag.set_color = true;
    title_tag.color = br_color_from_hex(0x7dff9a, 1.0f);
    title_tag.bold = true;
    title_tag.underline = true;
    br_rich_text_set_tag_style(e, r, "title", &title_tag);
    br_node_set_position(e, r, 16.0f, 476.0f);
    br_node_add_child(e, br_scene_root(e), r);
}

static void setup_alignment(br_engine *e)
{
    const char *paragraph = "The quick brown fox jumps over the lazy dog, then naps in the warm sun.";
    const char *names[4] = { "left", "center", "right", "justify" };
    br_text_align aligns[4] = { BR_TEXT_ALIGN_LEFT, BR_TEXT_ALIGN_CENTER, BR_TEXT_ALIGN_RIGHT, BR_TEXT_ALIGN_JUSTIFY };
    for (int i = 0; i < 4; i++) {
        float x = 470.0f;
        float y = 10.0f + (float)i * 128.0f;
        add_panel(e, x - 6.0f, y - 4.0f, 252.0f, 116.0f, 0x232a40);
        br_text_style label = white_style(13.0f);
        label.fill = br_color_from_hex(0x9aa4c8, 1.0f);
        add_text(e, names[i], &label, x, y);
        br_text_style st = white_style(18.0f);
        st.word_wrap = true;
        st.wrap_width = 240.0f;
        st.align = aligns[i];
        add_text(e, paragraph, &st, x, y + 18.0f);
    }
}

static void setup_bitmap_fonts(br_engine *e)
{
    /* generated from the TTF with fill, gradient, stroke and shadow baked in */
    br_text_style bake = br_text_style_default();
    bake.font_size = 32.0f;
    bake.bold = true;
    br_text_style_set_vertical_gradient(&bake, br_color_from_hex(0xffffff, 1.0f), br_color_from_hex(0x4fc3ff, 1.0f));
    bake.stroke_width = 4.0f;
    bake.stroke_color = br_color_from_hex(0x06243a, 1.0f);
    bake.drop_shadow = true;
    bake.shadow_distance = 3.0f;
    bake.shadow_color = br_color_from_hex(0x000000, 0.7f);
    br_bitmap_font_options opts;
    SDL_zero(opts);
    opts.chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !:";
    opts.resolution = 1.0f;
    br_font *installed = br_bitmap_font_install(e, "Installed", &bake, &opts);
    br_node b = br_bitmap_text_create(e, "INSTALLED 2026!", installed, 0.0f);
    br_node_set_position(e, b, 760.0f, 10.0f);
    br_node_add_child(e, br_scene_root(e), b);

    /* hand-made AngelCode files (examples/fonts/gen_pixel_font.py) */
    br_font *pixel = br_bitmap_font_load(e, "fonts/pixel5x7.fnt");
    br_font *pixel_xml = br_bitmap_font_load(e, "fonts/pixel5x7.xml");
    if (pixel != NULL) {
        br_texture_style nearest;
        SDL_zero(nearest);
        nearest.filter = BR_FILTER_NEAREST;
        br_texture_set_style(e, br_bitmap_font_page_texture(pixel, 0), &nearest);
        br_font_add_family(e, "Pixel5x7", pixel, false, false);
        br_node p = br_bitmap_text_create(e, "PIXEL FNT: AVA 1111", pixel, 0.0f);
        br_node_set_tint(e, p, br_color_from_hex(0xffd23f, 1.0f));
        br_node_set_position(e, p, 760.0f, 60.0f);
        br_node_add_child(e, br_scene_root(e), p);
        br_node big = br_bitmap_text_create(e, "SCALED 2X", pixel, 40.0f);
        br_node_set_tint(e, big, br_color_from_hex(0x7dff9a, 1.0f));
        br_node_set_position(e, big, 760.0f, 86.0f);
        br_node_add_child(e, br_scene_root(e), big);
    }
    if (pixel_xml != NULL) {
        br_node x = br_bitmap_text_create(e, "XML FNT: HELLO?", pixel_xml, 0.0f);
        br_node_set_tint(e, x, br_color_from_hex(0xff8fd0, 1.0f));
        br_node_set_position(e, x, 760.0f, 134.0f);
        br_node_add_child(e, br_scene_root(e), x);
    }
}

static void setup_sdf(br_engine *e, demo_state *s)
{
    size_t size;
    const void *data = br_font_default_data(&size);
    br_font_options opts;
    SDL_zero(opts);
    opts.sdf = true;
    s->sdf_font = br_font_create(e, data, size, &opts);

    br_text_style st = white_style(22.0f);
    st.font = s->sdf_font;
    st.stroke_width = 1.5f;
    st.stroke_color = br_color_from_hex(0x203060, 1.0f);
    s->sdf_text = add_text(e, "SDF", &st, 760.0f, 168.0f);
    br_node_set_scale(e, s->sdf_text, 6.0f, 6.0f); /* 22 px glyphs drawn 132 px tall, still sharp */

    br_text_style coverage = white_style(22.0f);
    br_node c = add_text(e, "px", &coverage, 1050.0f, 168.0f);
    br_node_set_scale(e, c, 6.0f, 6.0f); /* coverage glyphs scaled up: soft edges, for comparison */

    br_text_style label = white_style(13.0f);
    label.fill = br_color_from_hex(0x9aa4c8, 1.0f);
    add_text(e, "SDF font x6 (left) vs coverage x6 (right)", &label, 760.0f, 330.0f);
}

static void setup_split(br_engine *e, demo_state *s)
{
    br_text_style st = white_style(40.0f);
    st.bold = true;
    st.fill = br_color_from_hex(0xffb347, 1.0f);
    st.drop_shadow = true;
    st.shadow_distance = 3.0f;
    s->split_chars = br_split_text_create(e, "Split text!", &st, BR_SPLIT_CHARS);
    br_node_set_position(e, s->split_chars, 760.0f, 380.0f);
    br_node_add_child(e, br_scene_root(e), s->split_chars);

    br_text_style words = white_style(20.0f);
    words.fill = br_color_from_hex(0x8fd3ff, 1.0f);
    br_node w = br_split_text_create(e, "words can move on their own", &words, BR_SPLIT_WORDS);
    br_node_set_position(e, w, 760.0f, 450.0f);
    br_node_add_child(e, br_scene_root(e), w);
    for (uint32_t i = 0; i < br_node_child_count(e, w); i++) {
        br_node piece = br_node_child_at(e, w, i);
        br_vec2 p = br_node_position(e, piece);
        br_node_set_position(e, piece, p.x, p.y + (float)(i % 2) * 14.0f);
        br_node_set_rotation(e, piece, (i % 2 == 0 ? -0.08f : 0.08f));
    }
}

/* Per-character wave: each split piece rotates and scales around its own centre. */
static void wave_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    float t = (float)br_ticker_elapsed(e);
    uint32_t n = br_node_child_count(e, s->split_chars);
    for (uint32_t i = 0; i < n; i++) {
        br_node c = br_node_child_at(e, s->split_chars, i);
        float phase = t * 4.0f + (float)i * 0.7f;
        br_node_set_rotation(e, c, SDL_sinf(phase) * 0.35f);
        float k = 1.0f + 0.25f * SDL_sinf(phase + 1.0f);
        br_node_set_scale(e, c, k, k);
    }
}

static void demo_shutdown(br_engine *e, void *user)
{
    demo_state *s = user;
    uint32_t w, h;
    br_font_atlas_size(br_font_default(e), &w, &h);
    br_log_info("default font atlas %ux%u with %u glyph images", w, h, br_font_cached_glyph_count(br_font_default(e)));
    br_font_destroy(e, s->sdf_font);
    br_free(s);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    if (br_engine_gpu(e) != NULL) { /* NULL under --headless: nodes still update, nothing is drawn */
        br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x101422, 1.0f));
    }
    setup_bitmap_fonts(e); /* first: the rich text uses the Pixel5x7 family */
    setup_left_column(e);
    setup_alignment(e);
    setup_sdf(e, s);
    setup_split(e, s);
    br_system_add(e, "text_demo.wave", BR_PHASE_UPDATE, 0, wave_system, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - text";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR; /* the source tree (CMake), any build dir works */
    desc.init = demo_init;
    return desc;
}
