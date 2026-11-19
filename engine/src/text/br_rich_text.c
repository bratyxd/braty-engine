/* br_rich_text: tag parsing for rich text, and split text (one node per char / word / line). */
#include "text/br_text_internal.h"

#define BR_RICH_MAX_DEPTH 32

/* The formatting in effect at one point of the markup. */
typedef struct br_rich_state {
    char     family[BR_TEXT_FAMILY_SIZE];
    float    size;
    bool     set_color;
    br_color color;
    bool     bold, italic, underline, strike;
} br_rich_state;

typedef struct br_rich_open_tag {
    char          name[BR_RICH_TAG_NAME_SIZE];
    br_rich_state saved;     /* state before the tag opened */
} br_rich_open_tag;

typedef struct br_rich_parser {
    br_engine        *e;
    br_text_node     *node;
    br_rich_state     state;
    br_rich_open_tag  stack[BR_RICH_MAX_DEPTH];
    uint32_t          depth;
    bool              span_dirty;
    uint16_t          span;
    br_array          plain;     /* char */
    br_array         *chars;     /* br_layout_char */
} br_rich_parser;

static br_text_span br_rich_make_span(br_rich_parser *p)
{
    br_text_style st = p->node->style;
    if (SDL_strcmp(p->state.family, st.font_family) != 0) {
        st.font = NULL; /* <font=...> switches to family lookup */
        br_text_style_set_family(&st, p->state.family);
    }
    st.font_size = p->state.size;
    st.bold = p->state.bold;
    st.italic = p->state.italic;
    br_text_span span = br_text_span_from_style(p->e, &st);
    if (p->state.set_color) {
        span.color = p->state.color;
        span.use_gradient = false;
    }
    span.underline = p->state.underline;
    span.strike = p->state.strike;
    return span;
}

static void br_rich_emit(br_rich_parser *p, uint32_t cp, const char *bytes, uint32_t len)
{
    if (p->span_dirty || p->node->spans.count == 0) {
        br_text_span span = br_rich_make_span(p);
        if (p->node->spans.count < 65535) {
            br_array_push(&p->node->spans, &span);
        }
        p->span = (uint16_t)(p->node->spans.count - 1);
        p->span_dirty = false;
    }
    br_layout_char ch;
    ch.cp = cp;
    ch.src = p->plain.count;
    ch.src_len = (uint8_t)len;
    ch.span = p->span;
    br_array_push_n(&p->plain, bytes, len);
    br_array_push(p->chars, &ch);
}

static const br_rich_tag_style *br_rich_find_tag(br_text_node *node, const char *name)
{
    for (uint32_t i = 0; i < node->tag_count; i++) {
        if (SDL_strcmp(node->tags[i].name, name) == 0) {
            return &node->tags[i].style;
        }
    }
    return NULL;
}

/* Applies the tag text between '<' and '>'. False if it is not a known tag (drawn as text). */
static bool br_rich_apply_tag(br_rich_parser *p, const char *tag, size_t len)
{
    char buf[128];
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    SDL_memcpy(buf, tag, len);
    buf[len] = 0;
    if (buf[0] == '/') {
        const char *name = buf + 1;
        for (uint32_t i = p->depth; i > 0; i--) {
            if (SDL_strcmp(p->stack[i - 1].name, name) == 0) {
                p->state = p->stack[i - 1].saved;
                p->depth = i - 1;
                p->span_dirty = true;
                return true;
            }
        }
        return br_rich_find_tag(p->node, name) != NULL || SDL_strcmp(name, "b") == 0 ||
               SDL_strcmp(name, "i") == 0 || SDL_strcmp(name, "u") == 0 || SDL_strcmp(name, "s") == 0 ||
               SDL_strcmp(name, "color") == 0 || SDL_strcmp(name, "size") == 0 || SDL_strcmp(name, "font") == 0;
    }
    if (SDL_strcmp(buf, "br") == 0 || SDL_strcmp(buf, "br/") == 0 || SDL_strcmp(buf, "br /") == 0) {
        br_rich_emit(p, '\n', "\n", 1);
        return true;
    }
    char *value = SDL_strchr(buf, '=');
    if (value != NULL) {
        *value++ = 0;
        size_t vlen = SDL_strlen(value);
        if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') && value[vlen - 1] == value[0]) {
            value[vlen - 1] = 0;
            value++;
        }
    }
    br_rich_state next = p->state;
    if (value != NULL && SDL_strcmp(buf, "color") == 0) {
        if (!br_color_parse(value, &next.color)) {
            return false;
        }
        next.set_color = true;
    } else if (value != NULL && SDL_strcmp(buf, "size") == 0) {
        next.size = (float)SDL_atof(value);
        if (next.size <= 0.0f) {
            return false;
        }
    } else if (value != NULL && SDL_strcmp(buf, "font") == 0) {
        br_str_copy(next.family, sizeof(next.family), value);
    } else if (value != NULL) {
        return false;
    } else if (SDL_strcmp(buf, "b") == 0) {
        next.bold = true;
    } else if (SDL_strcmp(buf, "i") == 0) {
        next.italic = true;
    } else if (SDL_strcmp(buf, "u") == 0) {
        next.underline = true;
    } else if (SDL_strcmp(buf, "s") == 0) {
        next.strike = true;
    } else {
        const br_rich_tag_style *ts = br_rich_find_tag(p->node, buf);
        if (ts == NULL) {
            return false;
        }
        if (ts->font_family[0] != 0) {
            br_str_copy(next.family, sizeof(next.family), ts->font_family);
        }
        if (ts->font_size > 0.0f) {
            next.size = ts->font_size;
        }
        if (ts->set_color) {
            next.set_color = true;
            next.color = ts->color;
        }
        next.bold = next.bold || ts->bold;
        next.italic = next.italic || ts->italic;
        next.underline = next.underline || ts->underline;
        next.strike = next.strike || ts->strike;
    }
    if (p->depth >= BR_RICH_MAX_DEPTH) {
        br_log_warn("rich text: tags nested deeper than %d are ignored", BR_RICH_MAX_DEPTH);
        return true;
    }
    br_rich_open_tag *open = &p->stack[p->depth++];
    br_str_copy(open->name, sizeof(open->name), buf);
    open->saved = p->state;
    p->state = next;
    p->span_dirty = true;
    return true;
}

void br_rich_text_parse(br_engine *e, br_text_node *node, br_array *chars)
{
    br_rich_parser p;
    SDL_zero(p);
    p.e = e;
    p.node = node;
    p.chars = chars;
    br_array_init(&p.plain, 1);
    br_array_clear(chars);
    br_array_clear(&node->spans);
    br_str_copy(p.state.family, sizeof(p.state.family), node->style.font_family);
    p.state.size = node->style.font_size;
    p.state.bold = node->style.bold;
    p.state.italic = node->style.italic;

    const char *s = node->text != NULL ? node->text : "";
    size_t length = SDL_strlen(s);
    size_t i = 0;
    while (i < length) {
        if (s[i] == '<') {
            const char *close = SDL_strchr(s + i + 1, '>');
            if (close != NULL && br_rich_apply_tag(&p, s + i + 1, (size_t)(close - (s + i + 1)))) {
                i = (size_t)(close - s) + 1;
                continue;
            }
        } else if (s[i] == '&') {
            const char *entities[3] = { "&lt;", "&gt;", "&amp;" };
            const char *chars_out[3] = { "<", ">", "&" };
            bool matched = false;
            for (int k = 0; k < 3 && !matched; k++) {
                size_t n = SDL_strlen(entities[k]);
                if (SDL_strncmp(s + i, entities[k], n) == 0) {
                    br_rich_emit(&p, (uint32_t)chars_out[k][0], chars_out[k], 1);
                    i += n;
                    matched = true;
                }
            }
            if (matched) {
                continue;
            }
        }
        size_t start = i;
        uint32_t cp = br_text_utf8_next(s, length, &i);
        if (cp == '\r') {
            continue;
        }
        br_rich_emit(&p, cp, s + start, (uint32_t)(i - start));
    }
    if (node->spans.count == 0) {
        br_text_span span = br_rich_make_span(&p); /* empty text still needs a line height */
        br_array_push(&node->spans, &span);
    }
    br_text_collapse_white_space(chars, node->style.white_space);
    br_free(node->plain);
    node->plain = br_alloc(p.plain.count + 1);
    if (node->plain != NULL && p.plain.count > 0) {
        SDL_memcpy(node->plain, p.plain.data, p.plain.count);
    }
    br_array_free(&p.plain);
}

void br_rich_text_free(br_text_node *node)
{
    br_free(node->tags);
    node->tags = NULL;
    node->tag_count = 0;
    br_free(node->plain);
    node->plain = NULL;
}

/* ---- rich text API */

uint32_t br_rich_text_type(br_engine *e)
{
    return e->text != NULL ? e->text->rich_text_type : 0;
}

br_node br_rich_text_create(br_engine *e, const char *markup, const br_text_style *style)
{
    return br_text_node_create(e, br_rich_text_type(e), markup, style, true);
}

void br_rich_text_set_tag_style(br_engine *e, br_node n, const char *tag, const br_rich_tag_style *style)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL || tag == NULL || style == NULL) {
        return;
    }
    for (uint32_t i = 0; i < t->tag_count; i++) {
        if (SDL_strcmp(t->tags[i].name, tag) == 0) {
            t->tags[i].style = *style;
            t->dirty = true;
            return;
        }
    }
    br_rich_tag_entry *tags = br_realloc(t->tags, sizeof(br_rich_tag_entry) * (t->tag_count + 1));
    if (tags == NULL) {
        return;
    }
    t->tags = tags;
    br_rich_tag_entry *entry = &t->tags[t->tag_count++];
    SDL_zero(*entry);
    br_str_copy(entry->name, sizeof(entry->name), tag);
    entry->style = *style;
    t->dirty = true;
}

const char *br_rich_text_plain(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return "";
    }
    br_text_node_update(e, t);
    if (!t->rich) {
        return t->text != NULL ? t->text : "";
    }
    return t->plain != NULL ? t->plain : "";
}

bool br_rich_text_char_style(br_engine *e, br_node n, uint32_t i, br_rich_char_style *out)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return false;
    }
    br_text_node_update(e, t);
    const br_layout_glyph *g = br_array_at(&t->layout.glyphs, i);
    if (g == NULL) {
        return false;
    }
    const br_text_span *span = br_array_at(&t->spans, g->span);
    out->font = span->font;
    out->font_size = span->size;
    out->color = span->color;
    out->use_gradient = span->use_gradient;
    out->bold = span->bold;
    out->italic = span->italic;
    out->underline = span->underline;
    out->strike = span->strike;
    return true;
}

/* ---- split text */

static void br_split_add_piece(br_engine *e, br_node container, const char *text, uint32_t start, uint32_t end,
                               const br_text_style *style, float x, float y, float w, float h)
{
    char buf[1024];
    uint32_t len = end - start;
    if (len == 0 || len >= sizeof(buf)) {
        return;
    }
    SDL_memcpy(buf, text + start, len);
    buf[len] = 0;
    br_node piece = br_text_create(e, buf, style);
    br_text_set_anchor(e, piece, 0.5f, 0.5f);
    br_node_set_position(e, piece, x + w * 0.5f, y + h * 0.5f);
    br_node_add_child(e, container, piece);
}

br_node br_split_text_create(br_engine *e, const char *utf8, const br_text_style *style, br_split_mode mode)
{
    br_node container = br_node_create_container(e);
    if (utf8 == NULL || e->text == NULL) {
        return container;
    }
    br_text_style st = style != NULL ? *style : br_text_style_default();
    br_array chars;
    br_array_init(&chars, sizeof(br_layout_char));
    br_text_decode(utf8, st.white_space, &chars);
    br_text_span span = br_text_span_from_style(e, &st);
    br_text_layout layout;
    br_text_layout_init(&layout);
    br_text_layout_build(&layout, &chars, &span, &st);
    float shift = br_text_baseline_shift(&layout, &st);

    /* pieces never wrap; the gradient is per piece */
    br_text_style piece = st;
    piece.word_wrap = false;
    piece.align = BR_TEXT_ALIGN_LEFT;
    piece.baseline = BR_TEXT_BASELINE_TOP;

    for (uint32_t li = 0; li < layout.lines.count; li++) {
        const br_layout_line *line = br_array_at(&layout.lines, li);
        float top = line->top - shift;
        uint32_t k = 0;
        while (k < line->visible) {
            const br_layout_glyph *g = br_array_at(&layout.glyphs, line->first + k);
            if (g->cp == ' ' || g->cp == '\t') {
                k++;
                continue;
            }
            uint32_t end = k + 1;
            if (mode == BR_SPLIT_WORDS) {
                while (end < line->visible) {
                    const br_layout_glyph *h = br_array_at(&layout.glyphs, line->first + end);
                    if (h->cp == ' ' || h->cp == '\t') {
                        break;
                    }
                    end++;
                }
            } else if (mode == BR_SPLIT_LINES) {
                end = line->visible;
            }
            const br_layout_glyph *last = br_array_at(&layout.glyphs, line->first + end - 1);
            br_split_add_piece(e, container, utf8, g->src, last->src + last->src_len, &piece, g->x, top,
                               last->x + last->advance - g->x, line->height);
            k = end;
        }
    }
    br_text_layout_free(&layout);
    br_array_free(&chars);
    return container;
}
