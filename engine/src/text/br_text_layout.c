/* br_text_layout: UTF-8 decoding, white-space handling, line breaking, alignment, measuring. */
#include "text/br_text_internal.h"

/* ---- UTF-8 */

uint32_t br_text_utf8_next(const char *text, size_t length, size_t *offset)
{
    size_t i = *offset;
    if (text == NULL || i >= length || text[i] == 0) {
        return 0;
    }
    const unsigned char *s = (const unsigned char *)text;
    uint32_t c = s[i];
    if (c < 0x80) {
        *offset = i + 1;
        return c;
    }
    size_t extra;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        c &= 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        c &= 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        c &= 0x07;
        min = 0x10000;
    } else {
        *offset = i + 1; /* stray continuation byte or invalid lead byte */
        return 0xFFFD;
    }
    for (size_t k = 1; k <= extra; k++) {
        if (i + k >= length || (s[i + k] & 0xC0) != 0x80) {
            *offset = i + 1; /* truncated sequence */
            return 0xFFFD;
        }
        c = (c << 6) | (s[i + k] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *offset = i + 1; /* overlong form, surrogate or out of range */
        return 0xFFFD;
    }
    *offset = i + extra + 1;
    return c;
}

uint32_t br_text_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return 0;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* ---- decoding and white space */

static bool br_text_is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

void br_text_collapse_white_space(br_array *chars, br_text_white_space mode)
{
    if (mode == BR_TEXT_WHITE_SPACE_PRE) {
        return;
    }
    br_layout_char *c = chars->data;
    uint32_t out = 0;
    for (uint32_t i = 0; i < chars->count; i++) {
        br_layout_char ch = c[i];
        bool newline = ch.cp == '\n';
        if (mode == BR_TEXT_WHITE_SPACE_NORMAL && newline) {
            ch.cp = ' ';
            newline = false;
        }
        if (br_text_is_space(ch.cp)) {
            ch.cp = ' ';
            /* drop at the start of a line and after another space */
            if (out == 0 || c[out - 1].cp == ' ' || c[out - 1].cp == '\n') {
                continue;
            }
        } else if (newline) {
            if (out > 0 && c[out - 1].cp == ' ') {
                out--; /* no space before a newline */
            }
        }
        c[out++] = ch;
    }
    if (out > 0 && c[out - 1].cp == ' ') {
        out--;
    }
    chars->count = out;
}

void br_text_decode(const char *text, br_text_white_space mode, br_array *out_chars)
{
    br_array_clear(out_chars);
    if (text == NULL) {
        return;
    }
    size_t length = SDL_strlen(text);
    size_t offset = 0;
    for (;;) {
        size_t start = offset;
        uint32_t cp = br_text_utf8_next(text, length, &offset);
        if (cp == 0) {
            break;
        }
        if (cp == '\r') {
            continue;
        }
        br_layout_char ch;
        ch.cp = cp;
        ch.src = (uint32_t)start;
        ch.src_len = (uint8_t)(offset - start);
        ch.span = 0;
        br_array_push(out_chars, &ch);
    }
    br_text_collapse_white_space(out_chars, mode);
}

/* ---- spans */

br_text_span br_text_span_from_style(br_engine *e, const br_text_style *style)
{
    br_text_span s;
    SDL_zero(s);
    if (style->font != NULL) {
        s.font = style->font;
        s.synth_bold = style->bold;
        s.synth_italic = style->italic;
    } else {
        s.font = br_font_find(e, style->font_family, style->bold, style->italic, &s.synth_bold, &s.synth_italic);
    }
    s.size = style->font_size;
    if (!(s.size > 0.0f)) { /* NaN too */
        s.size = s.font != NULL && s.font->is_bitmap ? s.font->native_size : 26.0f;
    }
    if (s.size > BR_TEXT_MAX_FONT_SIZE) { /* rich <size=...> and tag sizes come through here */
        s.size = BR_TEXT_MAX_FONT_SIZE;
    }
    s.color = style->fill;
    s.use_gradient = style->use_gradient;
    s.bold = style->bold;
    s.italic = style->italic;
    return s;
}

/* ---- layout */

void br_text_layout_init(br_text_layout *l)
{
    br_array_init(&l->glyphs, sizeof(br_layout_glyph));
    br_array_init(&l->lines, sizeof(br_layout_line));
    l->width = 0.0f;
    l->height = 0.0f;
}

void br_text_layout_free(br_text_layout *l)
{
    br_array_free(&l->glyphs);
    br_array_free(&l->lines);
}

/* Closes the line made of glyphs [first, end). */
static void br_text_finish_line(br_text_layout *l, uint32_t first, uint32_t end, bool wrapped)
{
    br_layout_line line;
    SDL_zero(line);
    line.first = first;
    line.count = end - first;
    line.wrapped = wrapped;
    uint32_t line_index = l->lines.count;
    for (uint32_t i = first; i < end; i++) {
        br_layout_glyph *g = br_array_at(&l->glyphs, i);
        g->line = (uint16_t)(line_index < 65535 ? line_index : 65535);
        if (!br_text_is_space(g->cp)) {
            line.visible = i - first + 1;
            line.width = g->x + g->advance;
        }
    }
    br_array_push(&l->lines, &line);
}

static float br_text_char_advance(const br_text_span *span, uint32_t cp)
{
    if (cp == '\t') {
        return 4.0f * br_font_advance(span->font, ' ', span->size);
    }
    return br_font_advance(span->font, cp, span->size);
}

void br_text_layout_build(br_text_layout *out, const br_array *chars, const br_text_span *spans,
                          const br_text_style *style)
{
    br_array_clear(&out->glyphs);
    br_array_clear(&out->lines);
    float wrap = style->word_wrap ? style->wrap_width : 0.0f;
    uint32_t line_first = 0;
    float x = 0.0f;
    int64_t last_break = -1; /* glyph index of the last space on the current line */
    uint32_t prev_cp = 0;
    uint32_t prev_span = 0;

    for (uint32_t i = 0; i < chars->count; i++) {
        const br_layout_char *c = br_array_at(chars, i);
        if (c->cp == '\n') {
            br_text_finish_line(out, line_first, out->glyphs.count, false);
            line_first = out->glyphs.count;
            x = 0.0f;
            last_break = -1;
            prev_cp = 0;
            continue;
        }
        const br_text_span *span = &spans[c->span];
        float advance = br_text_char_advance(span, c->cp);
        float kern = 0.0f;
        if (prev_cp != 0 && prev_span == c->span) {
            kern = br_font_kerning(span->font, prev_cp, c->cp, span->size);
        }
        bool space = br_text_is_space(c->cp);
        uint32_t count = out->glyphs.count;
        if (wrap > 0.0f && !space && count > line_first && x + kern + advance > wrap) {
            if (last_break >= 0 && (uint32_t)last_break + 1 < count) {
                /* move the word in progress to a new line */
                uint32_t word = (uint32_t)last_break + 1;
                br_layout_glyph *first = br_array_at(&out->glyphs, word);
                float shift = first->x;
                br_text_finish_line(out, line_first, word, true);
                line_first = word;
                for (uint32_t k = word; k < count; k++) {
                    br_layout_glyph *g = br_array_at(&out->glyphs, k);
                    g->x -= shift;
                }
                x -= shift;
                last_break = -1;
            } else if (last_break >= 0 || style->break_words) {
                /* right after a space, or a word longer than the line: break here */
                br_text_finish_line(out, line_first, count, true);
                line_first = count;
                x = 0.0f;
                kern = 0.0f;
                last_break = -1;
            }
        }
        br_layout_glyph g;
        g.cp = c->cp;
        g.src = c->src;
        g.span = c->span;
        g.line = 0;
        g.src_len = c->src_len;
        g.x = x + kern;
        g.advance = advance;
        br_array_push(&out->glyphs, &g);
        x = g.x + advance + style->letter_spacing;
        if (space) {
            last_break = out->glyphs.count - 1;
        }
        prev_cp = c->cp;
        prev_span = c->span;
    }
    br_text_finish_line(out, line_first, out->glyphs.count, false);

    /* vertical: line boxes from the tallest span on each line */
    float top = 0.0f;
    float width = 0.0f;
    for (uint32_t li = 0; li < out->lines.count; li++) {
        br_layout_line *line = br_array_at(&out->lines, li);
        float ascent = 0.0f;
        float descent = 0.0f;
        uint32_t n = line->count;
        for (uint32_t k = 0; k <= n; k++) {
            const br_text_span *span;
            if (k < n) {
                br_layout_glyph *g = br_array_at(&out->glyphs, line->first + k);
                span = &spans[g->span];
            } else if (n == 0) {
                span = &spans[0]; /* empty line: height of the base style */
            } else {
                break;
            }
            br_font_metrics m = br_font_get_metrics(span->font, span->size);
            ascent = m.ascent > ascent ? m.ascent : ascent;
            descent = m.descent < descent ? m.descent : descent;
        }
        float natural = ascent - descent;
        line->height = style->line_height > 0.0f ? style->line_height : natural;
        line->top = top;
        line->baseline = top + (line->height - natural) * 0.5f + ascent;
        top += line->height + style->leading;
        width = line->width > width ? line->width : width;
    }
    out->width = width;
    out->height = out->lines.count > 0 ? top - style->leading : 0.0f;

    /* horizontal alignment within the widest line */
    for (uint32_t li = 0; li < out->lines.count; li++) {
        br_layout_line *line = br_array_at(&out->lines, li);
        float free_space = width - line->width;
        float offset = 0.0f;
        float per_space = 0.0f;
        if (style->align == BR_TEXT_ALIGN_CENTER) {
            offset = free_space * 0.5f;
        } else if (style->align == BR_TEXT_ALIGN_RIGHT) {
            offset = free_space;
        } else if (style->align == BR_TEXT_ALIGN_JUSTIFY && line->wrapped) {
            uint32_t spaces = 0;
            for (uint32_t k = 0; k < line->visible; k++) {
                br_layout_glyph *g = br_array_at(&out->glyphs, line->first + k);
                spaces += br_text_is_space(g->cp) ? 1u : 0u;
            }
            per_space = spaces > 0 ? free_space / (float)spaces : 0.0f;
        }
        line->x = offset;
        float extra = offset;
        for (uint32_t k = 0; k < line->count; k++) {
            br_layout_glyph *g = br_array_at(&out->glyphs, line->first + k);
            g->x += extra;
            if (k < line->visible && br_text_is_space(g->cp)) {
                extra += per_space;
            }
        }
        if (per_space > 0.0f) {
            line->width = width;
        }
    }
}

float br_text_baseline_shift(const br_text_layout *l, const br_text_style *style)
{
    switch (style->baseline) {
    case BR_TEXT_BASELINE_ALPHABETIC: {
        const br_layout_line *line = br_array_at(&l->lines, 0);
        return line != NULL ? line->baseline : 0.0f;
    }
    case BR_TEXT_BASELINE_MIDDLE:
        return l->height * 0.5f;
    case BR_TEXT_BASELINE_BOTTOM:
        return l->height;
    default:
        return 0.0f;
    }
}

br_text_metrics br_text_measure(br_engine *e, const char *utf8, const br_text_style *style)
{
    br_text_metrics m;
    SDL_zero(m);
    br_text_style st = style != NULL ? *style : br_text_style_default();
    br_text_style_sanitize(&st);
    br_array chars;
    br_array_init(&chars, sizeof(br_layout_char));
    br_text_decode(utf8, st.white_space, &chars);
    br_text_span span = br_text_span_from_style(e, &st);
    br_text_layout layout;
    br_text_layout_init(&layout);
    br_text_layout_build(&layout, &chars, &span, &st);
    m.width = layout.width;
    m.height = layout.height;
    m.line_count = layout.lines.count;
    if (layout.lines.count > 1) {
        br_layout_line *a = br_array_at(&layout.lines, 0);
        br_layout_line *b = br_array_at(&layout.lines, 1);
        m.line_height = b->baseline - a->baseline;
    } else if (layout.lines.count == 1) {
        br_layout_line *a = br_array_at(&layout.lines, 0);
        m.line_height = a->height;
    }
    br_text_layout_free(&layout);
    br_array_free(&chars);
    return m;
}
