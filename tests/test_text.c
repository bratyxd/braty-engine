/* test_text - UTF-8, AngelCode parsing (text + XML), measuring, wrapping, rich-text tags (headless). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>
#include <math.h>

#include "stb/stb_image_write.h"

/* A monospace bitmap font: every glyph advances 10, one kerning pair (a b = -2). */
static const char *k_fnt_text =
    "info face=\"Test\" size=16 bold=0 italic=0\n"
    "common lineHeight=20 base=16 scaleW=64 scaleH=64 pages=1 packed=0\n"
    "page id=0 file=\"test page.png\"\n"
    "chars count=8\n"
    "char id=32 x=0 y=0 width=0 height=0 xoffset=0 yoffset=0 xadvance=10 page=0 chnl=15\n"
    "char id=97 x=0 y=0 width=8 height=10 xoffset=1 yoffset=6 xadvance=10 page=0 chnl=15\n"
    "char id=98 x=8 y=0 width=8 height=14 xoffset=1 yoffset=2 xadvance=10 page=0 chnl=15\n"
    "char id=99 x=16 y=0 width=8 height=10 xoffset=1 yoffset=6 xadvance=10 page=0 chnl=15\n"
    "char id=100 x=24 y=0 width=8 height=14 xoffset=1 yoffset=2 xadvance=10 page=0 chnl=15\n"
    "char id=101 x=32 y=0 width=8 height=10 xoffset=1 yoffset=6 xadvance=10 page=0 chnl=15\n"
    "char id=233 x=40 y=0 width=8 height=14 xoffset=1 yoffset=2 xadvance=10 page=0 chnl=15\n"
    "char id=8364 x=48 y=0 width=9 height=14 xoffset=0 yoffset=2 xadvance=12 page=0 chnl=15\n"
    "kernings count=1\n"
    "kerning first=97 second=98 amount=-2\n";

static const char *k_fnt_xml =
    "<?xml version=\"1.0\"?>\n"
    "<font>\n"
    "  <info face=\"Test\" size=\"16\"/>\n"
    "  <common lineHeight=\"20\" base=\"16\" scaleW=\"64\" scaleH=\"64\" pages=\"2\"/>\n"
    "  <pages>\n"
    "    <page id=\"0\" file=\"a.png\" />\n"
    "    <page id=\"1\" file=\"b.png\" />\n"
    "  </pages>\n"
    "  <distanceField fieldType=\"msdf\" distanceRange=\"4\"/>\n"
    "  <chars count=\"3\">\n"
    "    <char id=\"98\" x=\"8\" y=\"0\" width=\"8\" height=\"14\" xoffset=\"1\" yoffset=\"2\" xadvance=\"10\" page=\"1\"/>\n"
    "    <char id=\"97\" x=\"0\" y=\"0\" width=\"8\" height=\"10\" xoffset=\"1\" yoffset=\"6\" xadvance=\"10\" page=\"0\"/>\n"
    "    <char id=\"32\" x=\"0\" y=\"0\" width=\"0\" height=\"0\" xoffset=\"0\" yoffset=\"0\" xadvance=\"10\" page=\"0\"/>\n"
    "  </chars>\n"
    "  <kernings count=\"1\">\n"
    "    <kerning first=\"97\" second=\"98\" amount=\"-2\" />\n"
    "  </kernings>\n"
    "</font>\n";

static bool near(float a, float b)
{
    return SDL_fabsf(a - b) < 0.01f;
}

static void test_utf8(void)
{
    const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"; /* a e-acute euro U+1F600 */
    size_t len = SDL_strlen(s);
    size_t off = 0;
    BR_CHECK(br_text_utf8_next(s, len, &off) == 0x61 && off == 1);
    BR_CHECK(br_text_utf8_next(s, len, &off) == 0xE9 && off == 3);
    BR_CHECK(br_text_utf8_next(s, len, &off) == 0x20AC && off == 6);
    BR_CHECK(br_text_utf8_next(s, len, &off) == 0x1F600 && off == 10);
    BR_CHECK(br_text_utf8_next(s, len, &off) == 0);

    /* invalid input decodes as U+FFFD and moves one byte */
    const char *bad = "\x80\xC3\x28\xE2\x82\xC0\xAF\xF8";
    size_t blen = SDL_strlen(bad);
    off = 0;
    BR_CHECK(br_text_utf8_next(bad, blen, &off) == 0xFFFD && off == 1);     /* stray continuation */
    BR_CHECK(br_text_utf8_next(bad, blen, &off) == 0xFFFD && off == 2);     /* bad continuation */
    BR_CHECK(br_text_utf8_next(bad, blen, &off) == '(' && off == 3);
    BR_CHECK(br_text_utf8_next(bad, blen, &off) == 0xFFFD && off == 4);     /* E2 82 then C0 */
    const char *overlong = "\xC0\xAF";
    off = 0;
    BR_CHECK(br_text_utf8_next(overlong, 2, &off) == 0xFFFD && off == 1);
    const char *truncated = "\xE2\x82";
    off = 0;
    BR_CHECK(br_text_utf8_next(truncated, 2, &off) == 0xFFFD && off == 1);

    char buf[4];
    BR_CHECK(br_text_utf8_encode(0x20AC, buf) == 3 && (uint8_t)buf[0] == 0xE2 && (uint8_t)buf[2] == 0xAC);
    BR_CHECK(br_text_utf8_encode(0x1F600, buf) == 4);
    BR_CHECK(br_text_utf8_encode(0xD800, buf) == 0);
}

static void test_fnt_text(br_engine *e, br_font *f)
{
    BR_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    BR_CHECK(br_font_is_bitmap(f));
    BR_CHECK(!br_font_is_sdf(f));
    BR_CHECK(br_bitmap_font_char_count(f) == 8);
    BR_CHECK(br_bitmap_font_page_count(f) == 1);
    BR_CHECK(SDL_strcmp(br_bitmap_font_page_file(f, 0), "test page.png") == 0);
    BR_CHECK(near(br_font_native_size(f), 16.0f));
    float lh, base;
    br_bitmap_font_line_metrics(f, &lh, &base);
    BR_CHECK(near(lh, 20.0f) && near(base, 16.0f));
    br_bitmap_char c;
    BR_CHECK(br_bitmap_font_get_char(f, 'b', &c));
    BR_CHECK(near(c.x, 8.0f) && near(c.height, 14.0f) && near(c.yoffset, 2.0f) && near(c.xadvance, 10.0f));
    BR_CHECK(br_bitmap_font_get_char(f, 0x20AC, &c) && near(c.xadvance, 12.0f));
    BR_CHECK(!br_bitmap_font_get_char(f, 'z', &c));
    BR_CHECK(near(br_font_kerning(f, 'a', 'b', 16.0f), -2.0f));
    BR_CHECK(near(br_font_kerning(f, 'b', 'a', 16.0f), 0.0f));
    BR_CHECK(near(br_font_kerning(f, 'a', 'b', 32.0f), -4.0f)); /* scales with size */
    br_font_metrics m;
    BR_CHECK(br_font_get_metrics(f, 32.0f, &m));
    BR_CHECK(near(m.ascent, 32.0f) && near(m.line_height, 40.0f));
    (void)e;
}

static void test_fnt_xml(br_engine *e)
{
    br_font *f = br_bitmap_font_parse(e, k_fnt_xml, SDL_strlen(k_fnt_xml), "", false);
    BR_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    BR_CHECK(br_bitmap_font_char_count(f) == 3);
    BR_CHECK(br_bitmap_font_page_count(f) == 2);
    BR_CHECK(SDL_strcmp(br_bitmap_font_page_file(f, 1), "b.png") == 0);
    BR_CHECK(br_font_is_sdf(f));
    br_bitmap_char c;
    BR_CHECK(br_bitmap_font_get_char(f, 'b', &c) && c.page == 1 && near(c.xoffset, 1.0f));
    BR_CHECK(br_bitmap_font_get_char(f, 'a', &c) && near(c.yoffset, 6.0f)); /* sorted after parsing */
    BR_CHECK(near(br_font_kerning(f, 'a', 'b', 16.0f), -2.0f));
    br_font_destroy(e, f);

    /* not a font: fails cleanly */
    const char *junk = "<?xml version=\"1.0\"?><settings volume=\"3\"/>";
    BR_CHECK(br_bitmap_font_parse(e, junk, SDL_strlen(junk), "", false) == NULL);
    BR_CHECK(br_bitmap_font_parse(e, "", 0, "", false) == NULL);
}

static void test_measure(br_engine *e, br_font *f)
{
    br_text_style st = br_text_style_default();
    st.font = f;
    st.font_size = 16.0f;
    br_text_metrics m = br_text_measure(e, "acd", &st);
    BR_CHECK(near(m.width, 30.0f));
    BR_CHECK(near(m.height, 20.0f) && m.line_count == 1);
    BR_CHECK(near(br_text_measure(e, "ab", &st).width, 18.0f));             /* kerning */
    BR_CHECK(near(br_text_measure(e, "a\xC3\xA9\xE2\x82\xAC", &st).width, 32.0f)); /* UTF-8: 10 + 10 + 12 */
    st.letter_spacing = 2.0f;
    BR_CHECK(near(br_text_measure(e, "acd", &st).width, 34.0f));            /* not after the last char */
    st.letter_spacing = 0.0f;
    st.font_size = 32.0f;
    BR_CHECK(near(br_text_measure(e, "acd", &st).width, 60.0f));            /* scaled from native 16 */
    st.font_size = 16.0f;
    m = br_text_measure(e, "ac\nd", &st);
    BR_CHECK(m.line_count == 2 && near(m.width, 20.0f) && near(m.height, 40.0f) && near(m.line_height, 20.0f));
    st.leading = 5.0f;
    BR_CHECK(near(br_text_measure(e, "ac\nd", &st).height, 45.0f));
    st.leading = 0.0f;
    st.line_height = 30.0f;
    BR_CHECK(near(br_text_measure(e, "ac\nd", &st).height, 60.0f));

    /* the TTF path: measure equals the font's own advances and kerning */
    br_font *ttf = br_font_default(e);
    BR_CHECK(ttf != NULL && !br_font_is_bitmap(ttf));
    br_text_style ts = br_text_style_default();
    ts.font_size = 40.0f;
    float expect = br_font_advance(ttf, 'A', 40.0f) + br_font_kerning(ttf, 'A', 'V', 40.0f) +
                   br_font_advance(ttf, 'V', 40.0f);
    BR_CHECK(near(br_text_measure(e, "AV", &ts).width, expect));
    BR_CHECK(br_font_kerning(ttf, 'A', 'V', 40.0f) < 0.0f); /* Noto Sans kerns AV (GPOS) */
    BR_CHECK(br_font_has_glyph(ttf, 0x0416) && br_font_has_glyph(ttf, 0x03A9)); /* Cyrillic, Greek */
    br_font_metrics fm;
    BR_CHECK(br_font_get_metrics(ttf, 40.0f, &fm));
    BR_CHECK(fm.ascent > 30.0f && fm.descent < -5.0f);
}

static void test_wrap(br_engine *e, br_font *f)
{
    br_text_style st = br_text_style_default();
    st.font = f;
    st.font_size = 0.0f; /* native 16 */
    st.word_wrap = true;
    st.wrap_width = 75.0f;
    br_node n = br_text_create(e, "aaa bbb ccc", &st);
    BR_CHECK(br_text_line_count(e, n) == 2);
    br_text_line_info li;
    BR_CHECK(br_text_get_line(e, n, 0, &li) && li.byte_start == 0 && li.byte_end == 7 && near(li.width, 70.0f));
    BR_CHECK(br_text_get_line(e, n, 1, &li) && li.byte_start == 8 && li.byte_end == 11 && near(li.width, 30.0f));
    BR_CHECK(near(li.y, 20.0f) && near(li.baseline, 36.0f));
    BR_CHECK(!br_text_get_line(e, n, 2, &li));

    /* alignment within the widest line */
    st.align = BR_TEXT_ALIGN_RIGHT;
    br_text_set_style(e, n, &st);
    BR_CHECK(br_text_get_line(e, n, 1, &li) && near(li.x, 40.0f));
    st.align = BR_TEXT_ALIGN_CENTER;
    br_text_set_style(e, n, &st);
    BR_CHECK(br_text_get_line(e, n, 1, &li) && near(li.x, 20.0f));
    st.align = BR_TEXT_ALIGN_JUSTIFY;
    br_text_set_style(e, n, &st);
    br_text_set_text(e, n, "aa b ccc dd");     /* "aa b ccc" (80 > 75) wraps: "aa b" / "ccc dd" */
    br_rect box;
    BR_CHECK(br_text_line_count(e, n) == 2);
    BR_CHECK(br_text_get_char_box(e, n, 3, &box) && near(box.x, 50.0f)); /* 'b' moved by the free 20 */

    /* a word longer than the line: overflows, or breaks with break_words */
    st.align = BR_TEXT_ALIGN_LEFT;
    st.wrap_width = 35.0f;
    br_text_set_style(e, n, &st);
    br_text_set_text(e, n, "aaaaaaaa");
    BR_CHECK(br_text_line_count(e, n) == 1);
    st.break_words = true;
    br_text_set_style(e, n, &st);
    BR_CHECK(br_text_line_count(e, n) == 3);
    BR_CHECK(br_text_get_line(e, n, 1, &li) && li.byte_start == 3 && li.byte_end == 6);

    /* white space modes */
    st.word_wrap = false;
    st.white_space = BR_TEXT_WHITE_SPACE_NORMAL;
    BR_CHECK(near(br_text_measure(e, "  a   c \n d ", &st).width, 50.0f)); /* "a c d" */
    BR_CHECK(br_text_measure(e, "  a   c \n d ", &st).line_count == 1);
    st.white_space = BR_TEXT_WHITE_SPACE_PRE_LINE;
    br_text_metrics m = br_text_measure(e, "a   c \n  d", &st);
    BR_CHECK(m.line_count == 2 && near(m.width, 30.0f));
    st.white_space = BR_TEXT_WHITE_SPACE_PRE;
    BR_CHECK(near(br_text_measure(e, "a   c", &st).width, 50.0f));

    /* anchor and padding move the bounds */
    br_text_set_text(e, n, "acd");
    br_text_set_anchor(e, n, 0.5f, 1.0f);
    st.padding = 4.0f;
    br_text_set_style(e, n, &st);
    br_rect b = br_node_local_bounds(e, n);
    BR_CHECK(near(b.x, -19.0f) && near(b.y, -24.0f) && near(b.width, 38.0f) && near(b.height, 28.0f));
    br_node_destroy(e, n);
}

static void test_rich(br_engine *e, br_font *f)
{
    br_text_style st = br_text_style_default();
    st.font = f;
    st.font_size = 16.0f;
    st.fill = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    br_node n = br_rich_text_create(e, "<b>ab</b> <color=#ff0000>c</color><br><size=32>d</size> <x>e</x>&lt;&amp;", &st);
    br_rich_tag_style hl;
    SDL_zero(hl);
    hl.set_color = true;
    hl.color = br_color_make(0.0f, 1.0f, 0.0f, 1.0f);
    hl.underline = true;
    br_rich_text_set_tag_style(e, n, "hl", &hl);
    BR_CHECK(SDL_strcmp(br_rich_text_plain(e, n), "ab c\nd <x>e</x><&") == 0); /* <x> is unknown: literal */
    br_rich_char_style cs;
    BR_CHECK(br_rich_text_char_style(e, n, 0, &cs) && cs.bold && near(cs.color.r, 1.0f) && near(cs.font_size, 16.0f));
    BR_CHECK(br_rich_text_char_style(e, n, 2, &cs) && !cs.bold);                 /* the space */
    BR_CHECK(br_rich_text_char_style(e, n, 3, &cs) && near(cs.color.g, 0.0f) && !cs.use_gradient);
    BR_CHECK(br_rich_text_char_style(e, n, 4, &cs) && near(cs.font_size, 32.0f)); /* 'd' (newline is not a char) */
    BR_CHECK(br_text_line_count(e, n) == 2);
    br_text_line_info li;
    BR_CHECK(br_text_get_line(e, n, 1, &li) && near(li.height, 40.0f));         /* tallest span sets the line */

    br_text_set_text(e, n, "x<hl>yy</hl>z");
    BR_CHECK(SDL_strcmp(br_rich_text_plain(e, n), "xyyz") == 0);
    BR_CHECK(br_rich_text_char_style(e, n, 1, &cs) && cs.underline && near(cs.color.g, 1.0f));
    BR_CHECK(br_rich_text_char_style(e, n, 3, &cs) && !cs.underline);
    br_text_set_text(e, n, "<i><b>a</i>b</b>"); /* closing an outer tag also closes the inner one */
    BR_CHECK(br_rich_text_char_style(e, n, 0, &cs) && cs.bold && cs.italic);
    BR_CHECK(br_rich_text_char_style(e, n, 1, &cs) && !cs.bold && !cs.italic);
    br_text_set_text(e, n, "<color=nonsense>a");
    BR_CHECK(SDL_strcmp(br_rich_text_plain(e, n), "<color=nonsense>a") == 0);
    br_node_destroy(e, n);
}

static void test_split(br_engine *e, br_font *f)
{
    br_text_style st = br_text_style_default();
    st.font = f;
    st.font_size = 0.0f;
    br_node chars = br_split_text_create(e, "ab cd", &st, BR_SPLIT_CHARS);
    BR_CHECK(br_node_child_count(e, chars) == 4);
    br_node c = br_node_child_at(e, chars, 2); /* 'c' */
    BR_CHECK(SDL_strcmp(br_text_text(e, c), "c") == 0);
    br_vec2 p = br_node_position(e, c);
    BR_CHECK(near(p.x, 18.0f + 10.0f + 5.0f) && near(p.y, 10.0f)); /* after kerned "ab" and a space */
    br_node words = br_split_text_create(e, "ab cd", &st, BR_SPLIT_WORDS);
    BR_CHECK(br_node_child_count(e, words) == 2);
    BR_CHECK(SDL_strcmp(br_text_text(e, br_node_child_at(e, words, 1)), "cd") == 0);
    br_node lines = br_split_text_create(e, "ab\ncd e", &st, BR_SPLIT_LINES);
    BR_CHECK(br_node_child_count(e, lines) == 2);
    BR_CHECK(SDL_strcmp(br_text_text(e, br_node_child_at(e, lines, 1)), "cd e") == 0);
    br_node_destroy(e, chars);
    br_node_destroy(e, words);
    br_node_destroy(e, lines);

    /* pieces of any length (a long line or URL was dropped from 1024 bytes on) */
    char *long_line = SDL_malloc(2001);
    for (int i = 0; i < 2000; i++) {
        long_line[i] = i % 7 == 6 ? ' ' : 'a';
    }
    long_line[1999] = 'b';
    long_line[2000] = 0;
    lines = br_split_text_create(e, long_line, &st, BR_SPLIT_LINES);
    BR_CHECK(br_node_child_count(e, lines) == 1);
    BR_CHECK(SDL_strcmp(br_text_text(e, br_node_child_at(e, lines, 0)), long_line) == 0);
    for (int i = 0; i < 2000; i++) {
        long_line[i] = 'c';
    }
    words = br_split_text_create(e, long_line, &st, BR_SPLIT_WORDS);
    BR_CHECK(br_node_child_count(e, words) == 1);
    BR_CHECK(SDL_strlen(br_text_text(e, br_node_child_at(e, words, 0))) == 2000);
    br_node_destroy(e, lines);
    br_node_destroy(e, words);
    SDL_free(long_line);
}

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The table directory record of `tag` in a font file (NULL if missing). */
static uint8_t *font_record(uint8_t *font, const char *tag)
{
    uint32_t n = be16(font + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *rec = font + 12 + 16 * i;
        if (SDL_memcmp(rec, tag, 4) == 0) {
            return rec;
        }
    }
    return NULL;
}

/* Truncated and corrupted TTF files fail cleanly instead of reading past the buffer. */
static void test_bad_fonts(br_engine *e)
{
    size_t size = 0;
    const uint8_t *data = br_font_default_data(&size);
    BR_CHECK(br_font_create(e, data, 300, NULL) == NULL);
    BR_CHECK(br_font_create(e, data, 4096, NULL) == NULL);
    BR_CHECK(br_font_create(e, data, size / 2, NULL) == NULL);
    BR_CHECK(br_font_create(e, data, size - 1, NULL) == NULL);

    uint8_t *copy = SDL_malloc(size);
    BR_CHECK(copy != NULL);
    if (copy == NULL) {
        return;
    }
    br_font *ok = br_font_create(e, data, size, NULL);
    BR_CHECK(ok != NULL); /* the real font passes the checks */
    br_font_destroy(e, ok);

    /* table directory: cmap far past the end; head length past the end */
    SDL_memcpy(copy, data, size);
    put32(font_record(copy, "cmap") + 8, 0x7FFFFF00u);
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);
    SDL_memcpy(copy, data, size);
    put32(font_record(copy, "head") + 12, 0x7FFFFFFFu);
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);

    /* a cmap encoding record pointing outside cmap */
    SDL_memcpy(copy, data, size);
    uint8_t *cmap = copy + be32(font_record(copy, "cmap") + 8);
    uint32_t encodings = be16(cmap + 2);
    for (uint32_t i = 0; i < encodings; i++) {
        put32(cmap + 4 + 8 * i + 4, 0x7FFFFF00u);
    }
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);

    /* format 4 binary-search fields that walk off the subtable */
    SDL_memcpy(copy, data, size);
    cmap = copy + be32(font_record(copy, "cmap") + 8);
    bool patched = false;
    for (uint32_t i = 0; i < encodings; i++) {
        uint8_t *sub = cmap + be32(cmap + 4 + 8 * i + 4);
        if (be16(sub) == 4) {
            put16(sub + 12, 0xFFFE); /* rangeShift */
            put16(sub + 10, 15);     /* entrySelector */
            patched = true;
        }
    }
    BR_CHECK(patched);
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);

    /* no long horizontal metrics (stb reads hmtx[-1]); more glyphs than loca has entries */
    SDL_memcpy(copy, data, size);
    put16(copy + be32(font_record(copy, "hhea") + 8) + 34, 0);
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);
    SDL_memcpy(copy, data, size);
    put16(copy + be32(font_record(copy, "maxp") + 8) + 4, 0xFFFF);
    BR_CHECK(br_font_create(e, copy, size, NULL) == NULL);
    SDL_free(copy);
}

/* Freeing a font that text nodes use: the nodes drop it and fall back, never keep the pointer. */
static void test_font_lifetime(br_engine *e)
{
    br_text_style src = br_text_style_default(); /* the default TTF font */
    src.font_size = 20.0f;
    br_bitmap_font_options digits;
    SDL_zero(digits);
    digits.chars = "0123456789";
    br_font *a = br_bitmap_font_install(e, "score", &src, &digits);
    BR_CHECK(a != NULL);

    br_text_style st = br_text_style_default();
    br_text_style_set_family(&st, "score");
    br_node by_family = br_text_create(e, "11", &st);
    br_node direct = br_bitmap_text_create(e, "11", a, 0.0f);
    float wide = br_text_size(e, by_family).x;
    BR_CHECK(wide > 0.0f && br_text_size(e, direct).x > 0.0f);
    br_rich_char_style cs;
    BR_CHECK(br_rich_text_char_style(e, by_family, 0, &cs) && cs.font == a);

    /* reinstall under the same name: the family node follows the new font (no '1', no space:
     * zero width), the node that held the old font directly falls back to the default font */
    digits.chars = "AB";
    br_font *b = br_bitmap_font_install(e, "score", &src, &digits);
    BR_CHECK(b != NULL);
    BR_CHECK(br_text_size(e, by_family).x == 0.0f);
    BR_CHECK(br_rich_text_char_style(e, by_family, 0, &cs) && cs.font == b);
    BR_CHECK(br_rich_text_char_style(e, direct, 0, &cs) && cs.font == br_font_default(e));
    BR_CHECK(br_text_size(e, direct).x != wide);

    /* uninstall: the family node falls back to the default font */
    br_bitmap_font_uninstall(e, "score");
    BR_CHECK(br_rich_text_char_style(e, by_family, 0, &cs) && cs.font == br_font_default(e));
    br_text_style got;
    BR_CHECK(br_text_get_style(e, direct, &got) && got.font == NULL);

    /* a font asset unloaded while a node uses it */
    const char *fnt = "test_text_data/lifetime.fnt";
    BR_CHECK(br_file_write_all(e, fnt, k_fnt_text, SDL_strlen(k_fnt_text)));
    br_font *asset = br_bitmap_font_load(e, fnt);
    BR_CHECK(asset != NULL);
    br_node from_asset = br_bitmap_text_create(e, "ab", asset, 16.0f);
    BR_CHECK(near(br_text_size(e, from_asset).x, 18.0f)); /* kerned */
    br_assets_unload(e, fnt);
    BR_CHECK(br_assets_get(e, fnt) == NULL);
    BR_CHECK(br_rich_text_char_style(e, from_asset, 0, &cs) && cs.font == br_font_default(e));

    br_node_destroy(e, by_family);
    br_node_destroy(e, direct);
    br_node_destroy(e, from_asset);
}

/* The glyph atlas through br_text_prepare (the rasterise pass of a draw). */
static void test_atlas(br_engine *e)
{
    size_t size = 0;
    const void *data = br_font_default_data(&size);
    br_font *font = br_font_create(e, data, size, NULL);
    br_text_style st = br_text_style_default();
    st.font = font;
    st.font_size = 32.0f;
    uint32_t w, h;

    /* growth: glyphs are added on demand and the atlas doubles when full */
    br_node n = br_text_create(e, "abc", &st);
    br_text_set_resolution(e, n, 1.0f);
    br_text_prepare(e, n);
    br_font_atlas_size(font, &w, &h);
    BR_CHECK(w == 256 && h == 256 && br_font_cached_glyph_count(font) == 3);
    br_text_prepare(e, n);
    BR_CHECK(br_font_cached_glyph_count(font) == 3); /* cached */
    st.font_size = 96.0f;
    br_text_set_style(e, n, &st);
    br_text_set_text(e, n, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    br_text_prepare(e, n);
    br_font_atlas_size(font, &w, &h);
    BR_CHECK(w * h > 256 * 256 && br_font_cached_glyph_count(font) == 3 + 62);

    /* a huge font size is rasterised at most at 1024 px (scaled when drawn), not at 4095:
     * the atlas does not jump to its 4096 maximum for one glyph */
    st.font_size = 4095.0f;
    br_text_set_style(e, n, &st);
    br_text_set_text(e, n, "|");
    br_text_prepare(e, n);
    br_font_atlas_size(font, &w, &h);
    BR_CHECK(w < 4096 || h < 4096);
    st.font_size = NAN;
    br_text_set_style(e, n, &st);
    br_text_prepare(e, n); /* NaN: the default size, no undefined float-to-int conversion */
    BR_CHECK(br_text_size(e, n).y > 0.0f && br_text_size(e, n).y < 100.0f);

    /* a stroke / shadow far wider than the glyph is clamped: no seconds-long SDF per glyph */
    st.font_size = 26.0f;
    st.stroke_width = 1.0e4f;
    st.drop_shadow = true;
    st.shadow_blur = 1.0e4f;
    br_text_set_style(e, n, &st);
    br_text_set_text(e, n, "W");
    uint64_t t0 = SDL_GetTicks();
    br_text_prepare(e, n);
    BR_CHECK(SDL_GetTicks() - t0 < 500);
    br_node_destroy(e, n);
    br_font_destroy(e, font);

    /* a full atlas: glyphs that do not fit are remembered, not rasterised again every frame */
    font = br_font_create(e, data, size, NULL);
    st = br_text_style_default();
    st.font = font;
    st.font_size = 1024.0f;
    n = br_text_create(e, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", &st);
    br_text_set_resolution(e, n, 1.0f);
    br_text_prepare(e, n);
    br_font_atlas_size(font, &w, &h);
    BR_CHECK(w == 4096 && h == 4096);
    BR_CHECK(br_font_cached_glyph_count(font) == 36); /* placed or remembered as missing */
    t0 = SDL_GetTicks();
    br_text_prepare(e, n);
    BR_CHECK(br_font_cached_glyph_count(font) == 36);
    BR_CHECK(SDL_GetTicks() - t0 < 20);
    br_node_destroy(e, n);
    br_font_destroy(e, font);

    /* SDF font: one distance field per glyph whatever the drawn size */
    br_font_options opts;
    SDL_zero(opts);
    opts.sdf = true;
    font = br_font_create(e, data, size, &opts);
    BR_CHECK(br_font_is_sdf(font));
    st = br_text_style_default();
    st.font = font;
    st.font_size = 20.0f;
    st.stroke_width = 4.0f;
    n = br_text_create(e, "Hi", &st);
    br_text_prepare(e, n);
    BR_CHECK(br_font_cached_glyph_count(font) == 2); /* stroke and fill share the field */
    st.font_size = 200.0f;
    br_text_set_style(e, n, &st);
    br_text_prepare(e, n);
    BR_CHECK(br_font_cached_glyph_count(font) == 2);
    br_node_destroy(e, n);
    br_font_destroy(e, font);
}

/* The per-node quad cache: rebuilt on every change that moves a quad, never otherwise. */
static void test_quad_cache(br_engine *e)
{
    size_t size = 0;
    const void *data = br_font_default_data(&size);
    br_font *font = br_font_create(e, data, size, NULL);
    br_text_style st = br_text_style_default();
    st.font = font;
    st.font_size = 16.0f;
    br_node n = br_text_create(e, "abc", &st);
    br_text_set_resolution(e, n, 1.0f);
    BR_CHECK(br_text_quad_builds(e, n) == 0);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 1);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 1); /* unchanged: the cache is reused */
    br_text_set_text(e, n, "abc");
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 1); /* the same text is no change */
    br_text_set_text(e, n, "abd");
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 2); /* text */
    st.stroke_width = 2.0f;
    br_text_set_style(e, n, &st);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 3); /* style */
    br_text_set_anchor(e, n, 0.5f, 0.5f);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 4); /* anchor (part of the cached positions) */
    br_text_set_resolution(e, n, 2.0f);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 5); /* resolution (device scale): other raster sizes */
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 5);

    /* another node grows the shared atlas: every glyph moved, the cached UVs are stale */
    uint32_t w0, h0, w1, h1;
    br_font_atlas_size(font, &w0, &h0);
    br_text_style big = st;
    big.font_size = 96.0f;
    br_node other = br_text_create(e, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", &big);
    br_text_set_resolution(e, other, 1.0f);
    br_text_prepare(e, other);
    br_font_atlas_size(font, &w1, &h1);
    BR_CHECK(w1 * h1 > w0 * h0);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 6);

    /* the font is freed: the node falls back to the default font and builds again */
    br_font_destroy(e, font);
    br_text_prepare(e, n);
    BR_CHECK(br_text_quad_builds(e, n) == 7);
    br_text_style got;
    BR_CHECK(br_text_get_style(e, n, &got) && got.font == NULL);
    br_node_destroy(e, n);
    br_node_destroy(e, other);
}

typedef struct png_buffer {
    uint8_t data[4096];
    size_t  size;
} png_buffer;

static void png_append(void *context, void *data, int size)
{
    png_buffer *b = context;
    if (b->size + (size_t)size <= sizeof(b->data)) {
        SDL_memcpy(b->data + b->size, data, (size_t)size);
        b->size += (size_t)size;
    }
}

/* AngelCode loading through the assets module: pages, .xml claims, bad numbers. */
static void test_fnt_assets(br_engine *e)
{
    uint8_t pixels[8 * 8 * 4];
    SDL_memset(pixels, 255, sizeof(pixels));
    png_buffer png;
    png.size = 0;
    stbi_write_png_to_func(png_append, &png, 8, 8, 4, pixels, 8 * 4);
    BR_CHECK(br_file_write_all(e, "test_text_data/test page.png", png.data, png.size));
    BR_CHECK(br_file_write_all(e, "test_text_data/paged.fnt", k_fnt_text, SDL_strlen(k_fnt_text)));

    /* the page textures are released with the font */
    br_font *f = br_bitmap_font_load(e, "test_text_data/paged.fnt");
    BR_CHECK(f != NULL && br_bitmap_font_page_texture(f, 0).generation != 0);
    BR_CHECK(br_assets_ref_count(e, "test_text_data/test page.png") == 1);
    br_assets_unload(e, "test_text_data/paged.fnt");
    BR_CHECK(br_assets_get(e, "test_text_data/test page.png") == NULL);

    /* .xml: an AngelCode font loads; any other XML is not claimed by the font loader */
    BR_CHECK(br_file_write_all(e, "test_text_data/font.xml", k_fnt_xml, SDL_strlen(k_fnt_xml)));
    const char *settings = "<?xml version=\"1.0\"?><settings volume=\"3\"/>";
    BR_CHECK(br_file_write_all(e, "test_text_data/settings.xml", settings, SDL_strlen(settings)));
    f = br_bitmap_font_load(e, "test_text_data/font.xml");
    BR_CHECK(f != NULL && br_bitmap_font_char_count(f) == 3);
    br_assets_unload(e, "test_text_data/font.xml");
    BR_CHECK(br_assets_load(e, "test_text_data/settings.xml") == NULL);

    /* numbers: strict (no nan / hex), integers in range, distanceRange > 0 */
    const char *bad =
        "common lineHeight=20 base=16\n"
        "distanceField fieldType=sdf distanceRange=0\n"
        "char id=97 width=8 height=10 xadvance=10\n"
        "char id=-5 width=8 height=10 xadvance=10\n"
        "char id=1e20 width=8 height=10 xadvance=10\n"
        "char id=0x62 width=8 height=10 xadvance=10\n"
        "char id=99 width=nan height=10 xadvance=inf\n";
    f = br_bitmap_font_parse(e, bad, SDL_strlen(bad), "", false);
    BR_CHECK(f != NULL);
    br_bitmap_char c;
    BR_CHECK(br_bitmap_font_char_count(f) == 2); /* 97 and 99 */
    BR_CHECK(br_bitmap_font_get_char(f, 99, &c) && c.width == 0.0f && c.xadvance == 0.0f);
    br_text_style st = br_text_style_default();
    st.font = f;
    st.font_size = 20.0f;
    BR_CHECK(br_text_measure(e, "ac", &st).width < 1000.0f);
    br_font_destroy(e, f);

    /* rich <size=...>: the same number grammar */
    br_text_style rs = br_text_style_default();
    br_node n = br_rich_text_create(e, "<size=0x20>a</size><size=nan>b</size><size=1e9>c</size>", &rs);
    BR_CHECK(SDL_strcmp(br_rich_text_plain(e, n), "<size=0x20>a<size=nan>bc") == 0);
    br_rich_char_style cs;
    uint32_t last = br_text_char_count(e, n) - 1;
    BR_CHECK(br_rich_text_char_style(e, n, last, &cs) && cs.font_size <= 4096.0f);
    br_node_destroy(e, n);
}

int main(void)
{
    test_utf8();
    br_engine *e = br_engine_create("test_text", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    char dir[512]; /* fixtures written at startup: <executable dir>/test_text_data/ */
    BR_CHECK(br_file_resolve(e, "test_text_data", dir, sizeof(dir)));
    SDL_CreateDirectory(dir);
    br_font *f = br_bitmap_font_parse(e, k_fnt_text, SDL_strlen(k_fnt_text), "", false);
    test_fnt_text(e, f);
    test_fnt_xml(e);
    test_bad_fonts(e);
    test_font_lifetime(e);
    test_atlas(e);
    test_quad_cache(e);
    test_fnt_assets(e);
    if (f != NULL) {
        test_measure(e, f);
        test_wrap(e, f);
        test_rich(e, f);
        test_split(e, f);
    }
    br_engine_destroy(e); /* frees f and the default font */
    return br_test_finish();
}
