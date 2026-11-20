/* br_text_field: the editable text box node (br_text_field.h).
 *
 * A field is a node of type "text_field" with two children it creates itself: a text node that
 * shows the text (laid out and drawn by the public text API) and a white sprite that scissor-masks
 * the field to its box. The field's own render callback draws the background, border, selection,
 * IME underline and caret from "caret stops": one entry per code point boundary of the shown text
 * with its x and line, read from the text node's char boxes. Keys, typed text and IME events come
 * straight from SDL events (repeats included) through an engine event handler, so injected input
 * (br_input_inject_*) edits a field exactly like real input.
 */
#include "app/br_engine_internal.h"
#include "braty/braty.h"
#include "data/br_data_internal.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#define BR_TF_COMPOSITION_SIZE 256
#define BR_TF_DOUBLE_CLICK_NS  500000000ull
#define BR_TF_DOUBLE_CLICK_PX  6.0f
#define BR_TF_BLINK_PERIOD     1.0f
#define BR_TF_CARET_WIDTH      1.5f
#define BR_TF_MAX_SIZE         16384.0f
/* Labels of the two internal children: a saved and reloaded field finds copies of them among its
 * children and drops those (it made fresh ones). */
#define BR_TF_TEXT_LABEL "text_field.text"
#define BR_TF_CLIP_LABEL "text_field.clip"

/* What the last edit was: consecutive typing (or deleting) at the caret is one undo step. */
typedef enum br_tf_edit {
    BR_TF_EDIT_NONE,
    BR_TF_EDIT_TYPE,
    BR_TF_EDIT_DELETE,
    BR_TF_EDIT_OTHER
} br_tf_edit;

typedef struct br_tf_data {
    br_array text;              /* UTF-8 bytes + NUL (count includes the NUL) */
    uint32_t length;            /* code points */
    uint32_t anchor;
    uint32_t caret;
    br_array undo_text;         /* the text before the last edit (bytes + NUL), empty = no undo */
    uint32_t undo_anchor;
    uint32_t undo_caret;
    br_tf_edit last_edit;
    uint32_t edit_caret;        /* caret right after the last edit (a move in between ends a run) */
    char    *placeholder;       /* heap, NULL = none */
    char     composition[BR_TF_COMPOSITION_SIZE];
    int      composition_cursor; /* code points into the composition */
    uint32_t max_length;
    bool     multiline;
    bool     password;
    br_vec2  size;
    float    padding;
    br_text_style style;
    br_text_field_colors colors;
    br_text_field_fn fn;
    void    *user;
    br_node  text_node;
    br_node  clip;
    br_vec2  origin;            /* where the text node sits (padding, centring, scroll) */
    br_vec2  scroll;
    float    blink;             /* seconds since the caret last moved */
} br_tf_data;

/* One caret position in the shown text. */
typedef struct br_tf_stop {
    uint32_t display;   /* byte offset in the shown text */
    uint32_t content;   /* byte offset in the field's text */
    float    x;
    float    width;     /* of the glyph that starts here (0 for '\n' and the end) */
    uint32_t line;
} br_tf_stop;

typedef struct br_text_field_state {
    uint32_t type;
    br_array fields;            /* br_node, every live field */
    br_node  focused;
    bool     press_seen;        /* a pointer went down this frame ... */
    bool     press_claimed;     /* ... on a field (otherwise the focused field blurs) */
    bool     dragging;
    uint64_t last_click_ns;
    br_node  last_click_node;
    br_vec2  last_click_pos;
    int      click_count;
    br_array display;           /* scratch: the shown text */
    br_array stops;             /* scratch: br_tf_stop of the last built field */
} br_text_field_state;

static br_node br_tf_zero(void)
{
    br_node n = { 0, 0 };
    return n;
}

static br_tf_data *br_tf_get(br_engine *e, br_node n)
{
    if (e->text_field == NULL) {
        return NULL;
    }
    return br_node_data(e, n, e->text_field->type);
}

static const char *br_tf_chars(const br_tf_data *d)
{
    return d->text.count > 0 ? (const char *)d->text.data : "";
}

static uint32_t br_tf_bytes(const br_tf_data *d)
{
    return d->text.count > 0 ? d->text.count - 1 : 0;
}

/* ---- UTF-8 helpers (the buffer is always valid UTF-8) */

static uint32_t br_tf_count_cps(const char *s, uint32_t bytes)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        n += ((unsigned char)s[i] & 0xC0u) != 0x80u ? 1u : 0u;
    }
    return n;
}

static uint32_t br_tf_snap(const br_tf_data *d, uint32_t offset)
{
    const char *s = br_tf_chars(d);
    uint32_t bytes = br_tf_bytes(d);
    if (offset >= bytes) {
        return bytes;
    }
    while (offset > 0 && ((unsigned char)s[offset] & 0xC0u) == 0x80u) {
        offset--;
    }
    return offset;
}

static uint32_t br_tf_prev(const br_tf_data *d, uint32_t offset)
{
    return offset > 0 ? br_tf_snap(d, offset - 1) : 0;
}

static uint32_t br_tf_next(const br_tf_data *d, uint32_t offset)
{
    size_t off = offset;
    br_text_utf8_next(br_tf_chars(d), br_tf_bytes(d), &off);
    return (uint32_t)off;
}

static uint32_t br_tf_cp_at(const br_tf_data *d, uint32_t offset)
{
    size_t off = offset;
    return br_text_utf8_next(br_tf_chars(d), br_tf_bytes(d), &off);
}

/* 0 = space, 1 = word (letters, digits, '_', anything beyond ASCII), 2 = punctuation. */
static int br_tf_class(uint32_t cp)
{
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0x3000) {
        return 0;
    }
    if (cp >= 0x80 || cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
        return 1;
    }
    return 2;
}

static uint32_t br_tf_word_left(const br_tf_data *d, uint32_t offset)
{
    while (offset > 0 && br_tf_class(br_tf_cp_at(d, br_tf_prev(d, offset))) == 0) {
        offset = br_tf_prev(d, offset);
    }
    if (offset == 0) {
        return 0;
    }
    int cls = br_tf_class(br_tf_cp_at(d, br_tf_prev(d, offset)));
    while (offset > 0 && br_tf_class(br_tf_cp_at(d, br_tf_prev(d, offset))) == cls) {
        offset = br_tf_prev(d, offset);
    }
    return offset;
}

static uint32_t br_tf_word_right(const br_tf_data *d, uint32_t offset)
{
    uint32_t bytes = br_tf_bytes(d);
    if (offset < bytes) {
        int cls = br_tf_class(br_tf_cp_at(d, offset));
        while (offset < bytes && br_tf_class(br_tf_cp_at(d, offset)) == cls && cls != 0) {
            offset = br_tf_next(d, offset);
        }
    }
    while (offset < bytes && br_tf_class(br_tf_cp_at(d, offset)) == 0) {
        offset = br_tf_next(d, offset);
    }
    return offset;
}

/* Appends the valid, allowed code points of utf8 to out (bytes, no NUL), at most `room` of them.
 * Invalid bytes become U+FFFD, control characters are dropped, '\n' and '\t' are kept in
 * multi-line text and become spaces otherwise. Stops reading once room is used up, so a huge
 * paste costs only what fits. Returns the number of code points appended. */
static uint32_t br_tf_sanitize(const char *utf8, bool multiline, uint32_t room, br_array *out)
{
    if (utf8 == NULL) {
        return 0;
    }
    size_t length = SDL_strlen(utf8);
    size_t off = 0;
    uint32_t added = 0;
    while (added < room) {
        uint32_t cp = br_text_utf8_next(utf8, length, &off);
        if (cp == 0) {
            break;
        }
        if (cp == '\n' || cp == '\t') {
            cp = multiline ? cp : ' ';
        } else if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
            continue;
        }
        char bytes[4];
        uint32_t n = br_text_utf8_encode(cp, bytes);
        if (n == 0) {
            n = br_text_utf8_encode(0xFFFD, bytes);
        }
        br_array_push_n(out, bytes, n);
        added++;
    }
    return added;
}

/* ---- the shown text and caret stops */

static bool br_tf_shows_placeholder(const br_tf_data *d)
{
    return br_tf_bytes(d) == 0 && d->composition[0] == '\0';
}

static uint32_t br_tf_composition_bytes(const br_tf_data *d)
{
    return d->password ? 0 : (uint32_t)SDL_strlen(d->composition);
}

/* The bullet used in password mode ('*' if the font has no U+2022). */
static const char *br_tf_bullet(br_engine *e, const br_tf_data *d)
{
    br_font *font = d->style.font != NULL
                        ? d->style.font
                        : br_font_find(e, d->style.font_family[0] != '\0' ? d->style.font_family : "default",
                                       d->style.bold, d->style.italic, NULL, NULL);
    return font != NULL && br_font_has_glyph(font, 0x2022) ? "\xE2\x80\xA2" : "*";
}

/* Builds the shown text into s->display (with its NUL). */
static void br_tf_build_display(br_engine *e, const br_tf_data *d)
{
    br_text_field_state *s = e->text_field;
    br_array_clear(&s->display);
    const char *text = br_tf_chars(d);
    if (br_tf_shows_placeholder(d)) {
        const char *p = d->placeholder != NULL ? d->placeholder : "";
        br_array_push_n(&s->display, p, (uint32_t)SDL_strlen(p));
    } else if (d->password) {
        const char *bullet = br_tf_bullet(e, d);
        uint32_t len = (uint32_t)SDL_strlen(bullet);
        for (uint32_t i = 0; i < d->length; i++) {
            br_array_push_n(&s->display, bullet, len);
        }
    } else {
        uint32_t comp = br_tf_composition_bytes(d);
        br_array_push_n(&s->display, text, d->caret);
        br_array_push_n(&s->display, d->composition, comp);
        br_array_push_n(&s->display, text + d->caret, br_tf_bytes(d) - d->caret);
    }
    br_array_push_n(&s->display, "", 1);
}

/* Fills s->stops for the shown text (s->display, already set on the text node). */
static void br_tf_build_stops(br_engine *e, const br_tf_data *d)
{
    br_text_field_state *s = e->text_field;
    br_array_clear(&s->stops);
    br_tf_stop stop;
    SDL_zero(stop);
    if (br_tf_shows_placeholder(d)) {
        br_array_push(&s->stops, &stop);
        return;
    }
    const char *disp = (const char *)s->display.data;
    size_t disp_len = s->display.count - 1;
    uint32_t line_count = br_text_line_count(e, d->text_node);
    br_text_line_info line;
    SDL_zero(line);
    br_text_get_line(e, d->text_node, 0, &line);
    float x_end = line.x;
    uint32_t glyph = 0;
    uint32_t content = 0;
    uint32_t comp = br_tf_composition_bytes(d);
    size_t off = 0;
    for (;;) {
        size_t start = off;
        uint32_t cp = br_text_utf8_next(disp, disp_len, &off);
        stop.display = (uint32_t)start;
        if (d->password) {
            stop.content = content;
            content = br_tf_next(d, content);
        } else {
            stop.content = start <= d->caret ? (uint32_t)start
                           : start < d->caret + comp ? d->caret
                                                     : (uint32_t)start - comp;
        }
        stop.x = x_end;
        stop.width = 0.0f;
        if (cp == 0) {
            br_array_push(&s->stops, &stop);
            break;
        }
        if (cp == '\n') {
            br_array_push(&s->stops, &stop);
            stop.line++;
            if (br_text_get_line(e, d->text_node, stop.line, &line)) {
                x_end = line.x;
            }
            continue;
        }
        br_rect box;
        if (br_text_get_char_box(e, d->text_node, glyph++, &box)) {
            while (stop.line + 1 < line_count) {
                br_text_line_info next;
                if (!br_text_get_line(e, d->text_node, stop.line + 1, &next) || next.y > box.y) {
                    break;
                }
                stop.line++;
            }
            stop.x = box.x;
            stop.width = box.width;
            x_end = box.x + box.width;
        }
        br_array_push(&s->stops, &stop);
    }
}

/* The stop for a content offset (the first one that maps to it; s->stops is built). */
static const br_tf_stop *br_tf_stop_for(br_engine *e, uint32_t content)
{
    br_text_field_state *s = e->text_field;
    for (uint32_t i = 0; i < s->stops.count; i++) {
        const br_tf_stop *st = br_array_at(&s->stops, i);
        if (st->content >= content) {
            return st;
        }
    }
    return s->stops.count > 0 ? br_array_at(&s->stops, s->stops.count - 1) : NULL;
}

/* The stop where the caret is drawn (inside the composition at its cursor). */
static const br_tf_stop *br_tf_caret_stop(br_engine *e, const br_tf_data *d)
{
    br_text_field_state *s = e->text_field;
    uint32_t comp = br_tf_composition_bytes(d);
    if (comp == 0) {
        return br_tf_stop_for(e, d->caret);
    }
    int cursor = d->composition_cursor;
    for (uint32_t i = 0; i < s->stops.count; i++) {
        const br_tf_stop *st = br_array_at(&s->stops, i);
        if (st->display >= d->caret && (cursor-- <= 0 || st->display >= d->caret + comp)) {
            return st;
        }
    }
    return br_tf_stop_for(e, d->caret);
}

static br_text_line_info br_tf_line(br_engine *e, const br_tf_data *d, uint32_t index)
{
    br_text_line_info line;
    SDL_zero(line);
    if (!br_text_get_line(e, d->text_node, index, &line)) {
        line.height = br_text_measure(e, "", &d->style).line_height;
    }
    return line;
}

/* ---- sync: text node, scroll, mask */

static br_text_style br_tf_text_style(const br_tf_data *d, bool placeholder)
{
    br_text_style st = d->style;
    st.align = BR_TEXT_ALIGN_LEFT;
    st.white_space = BR_TEXT_WHITE_SPACE_PRE;
    st.baseline = BR_TEXT_BASELINE_TOP;
    st.word_wrap = d->multiline;
    st.break_words = true;
    st.wrap_width = d->size.x - 2.0f * d->padding - BR_TF_CARET_WIDTH;
    if (st.wrap_width < 1.0f) {
        st.wrap_width = 1.0f;
    }
    if (placeholder) {
        st.fill = d->colors.placeholder;
        st.use_gradient = false;
    }
    return st;
}

/* Pushes the shown text to the text node, rebuilds the stops and scrolls the caret into view. */
static void br_tf_sync(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    br_text_field_state *s = e->text_field;
    br_tf_build_display(e, d);
    br_text_style st = br_tf_text_style(d, br_tf_shows_placeholder(d));
    br_text_set_style(e, d->text_node, &st);
    br_text_set_text(e, d->text_node, (const char *)s->display.data);
    br_tf_build_stops(e, d);

    br_vec2 inner = br_vec2_make(d->size.x - 2.0f * d->padding, d->size.y - 2.0f * d->padding);
    br_vec2 content = br_text_size(e, d->text_node);
    const br_tf_stop *caret = br_tf_caret_stop(e, d);
    br_text_line_info line = br_tf_line(e, d, caret != NULL ? caret->line : 0);
    float cx = caret != NULL ? caret->x : 0.0f;
    /* keep the caret inside the box, never scroll past the text */
    if (cx - d->scroll.x > inner.x - BR_TF_CARET_WIDTH) {
        d->scroll.x = cx - inner.x + BR_TF_CARET_WIDTH;
    }
    if (cx < d->scroll.x) {
        d->scroll.x = cx;
    }
    float max_x = content.x + BR_TF_CARET_WIDTH - inner.x;
    d->scroll.x = br_clampf(d->scroll.x, 0.0f, max_x > 0.0f ? max_x : 0.0f);
    if (d->multiline) {
        if (line.y + line.height - d->scroll.y > inner.y) {
            d->scroll.y = line.y + line.height - inner.y;
        }
        if (line.y < d->scroll.y) {
            d->scroll.y = line.y;
        }
        float max_y = content.y - inner.y;
        d->scroll.y = br_clampf(d->scroll.y, 0.0f, max_y > 0.0f ? max_y : 0.0f);
        d->origin = br_vec2_make(d->padding - d->scroll.x, d->padding - d->scroll.y);
    } else {
        d->scroll.y = 0.0f;
        float h = line.height > 0.0f ? line.height : content.y;
        d->origin = br_vec2_make(d->padding - d->scroll.x, (d->size.y - h) * 0.5f);
    }
    br_node_set_position(e, d->text_node, d->origin);
    br_sprite_set_size(e, d->clip, d->size.x, d->size.y);
}

/* A caret move: restarts the blink and ends a typing / deleting run. */
static void br_tf_moved(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d != NULL) {
        d->blink = 0.0f;
        br_tf_sync(e, n);
    }
}

static void br_tf_emit(br_engine *e, br_node n, br_text_field_event event)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d != NULL && d->fn != NULL) {
        d->fn(e, n, event, d->user);
    }
}

/* ---- editing */

static void br_tf_set_bytes(br_tf_data *d, const char *bytes, uint32_t count)
{
    br_array_clear(&d->text);
    br_array_push_n(&d->text, bytes, count);
    br_array_push_n(&d->text, "", 1);
    d->length = br_tf_count_cps(bytes, count);
}

static void br_tf_snapshot(br_tf_data *d)
{
    br_array_clear(&d->undo_text);
    br_array_push_n(&d->undo_text, br_tf_chars(d), br_tf_bytes(d) + 1);
    d->undo_anchor = d->anchor;
    d->undo_caret = d->caret;
}

/* Replaces bytes [start, end) with `insert` (already sanitised, `count` bytes). */
static void br_tf_replace(br_engine *e, br_node n, uint32_t start, uint32_t end, const char *insert, uint32_t count,
                          br_tf_edit kind)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || (start == end && count == 0)) {
        return;
    }
    bool run = kind != BR_TF_EDIT_OTHER && kind == d->last_edit && d->caret == d->edit_caret &&
               d->anchor == d->caret && d->undo_text.count > 0;
    if (!run) {
        br_tf_snapshot(d);
    }
    uint32_t bytes = br_tf_bytes(d);
    uint32_t tail = bytes - end;
    uint32_t new_bytes = bytes - (end - start) + count;
    if (!br_array_resize(&d->text, new_bytes + 1)) {
        br_log_error("text_field: out of memory");
        return;
    }
    char *s = d->text.data;
    SDL_memmove(s + start + count, s + end, tail);
    SDL_memcpy(s + start, insert, count);
    s[new_bytes] = '\0';
    d->length = br_tf_count_cps(s, new_bytes);
    d->anchor = d->caret = start + count;
    d->last_edit = kind;
    d->edit_caret = d->caret;
    br_tf_moved(e, n);
    br_tf_emit(e, n, BR_TEXT_FIELD_CHANGED);
}

static void br_tf_selection_range(const br_tf_data *d, uint32_t *start, uint32_t *end)
{
    *start = d->anchor < d->caret ? d->anchor : d->caret;
    *end = d->anchor < d->caret ? d->caret : d->anchor;
}

static bool br_tf_insert(br_engine *e, br_node n, const char *utf8, br_tf_edit kind)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || utf8 == NULL) {
        return false;
    }
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    uint32_t selected = br_tf_count_cps(br_tf_chars(d) + start, end - start);
    uint32_t keep = d->length - selected;
    uint32_t room = d->max_length > keep ? d->max_length - keep : 0;
    br_array clean;
    br_array_init(&clean, 1);
    uint32_t added = br_tf_sanitize(utf8, d->multiline, room, &clean);
    if (added == 0) {
        br_array_free(&clean);
        return false;
    }
    if (start != end) {
        kind = BR_TF_EDIT_OTHER;
    }
    br_tf_replace(e, n, start, end, clean.count > 0 ? clean.data : "", clean.count, kind);
    br_array_free(&clean);
    return added > 0;
}

static void br_tf_delete(br_engine *e, br_node n, bool forward, bool word)
{
    br_tf_data *d = br_tf_get(e, n);
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    if (start == end) {
        if (forward) {
            end = word ? br_tf_word_right(d, d->caret) : br_tf_next(d, d->caret);
        } else {
            start = word ? br_tf_word_left(d, d->caret) : br_tf_prev(d, d->caret);
        }
    }
    br_tf_replace(e, n, start, end, "", 0, d->anchor != d->caret ? BR_TF_EDIT_OTHER : BR_TF_EDIT_DELETE);
}

static void br_tf_select(br_engine *e, br_node n, uint32_t anchor, uint32_t caret)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    d->anchor = br_tf_snap(d, anchor);
    d->caret = br_tf_snap(d, caret);
    br_tf_moved(e, n);
}

/* Moves the caret (extend = keep the anchor: shift held). */
static void br_tf_move(br_engine *e, br_node n, uint32_t caret, bool extend)
{
    br_tf_data *d = br_tf_get(e, n);
    br_tf_select(e, n, extend ? d->anchor : caret, caret);
}

/* The offset on line `line` nearest x (s->stops is built). */
static uint32_t br_tf_offset_on_line(br_engine *e, uint32_t line, float x)
{
    br_text_field_state *s = e->text_field;
    uint32_t best = 0;
    float best_dist = -1.0f;
    for (uint32_t i = 0; i < s->stops.count; i++) {
        const br_tf_stop *st = br_array_at(&s->stops, i);
        if (st->line != line) {
            continue;
        }
        float dist = SDL_fabsf(st->x - x);
        if (best_dist < 0.0f || dist < best_dist) {
            best_dist = dist;
            best = st->content;
        }
    }
    return best;
}

static uint32_t br_tf_last_line(br_engine *e)
{
    br_text_field_state *s = e->text_field;
    const br_tf_stop *last = s->stops.count > 0 ? br_array_at(&s->stops, s->stops.count - 1) : NULL;
    return last != NULL ? last->line : 0;
}

/* Home / End of the caret's line (multi-line), or of the text. */
static uint32_t br_tf_line_edge(br_engine *e, const br_tf_data *d, bool end)
{
    if (!d->multiline) {
        return end ? br_tf_bytes(d) : 0;
    }
    br_text_field_state *s = e->text_field;
    const br_tf_stop *caret = br_tf_stop_for(e, d->caret);
    uint32_t result = d->caret;
    bool found = false;
    for (uint32_t i = 0; caret != NULL && i < s->stops.count; i++) {
        const br_tf_stop *st = br_array_at(&s->stops, i);
        if (st->line == caret->line && (end || !found)) {
            result = st->content;
            found = true;
        }
    }
    return result;
}

static uint32_t br_tf_vertical(br_engine *e, const br_tf_data *d, int dir)
{
    const br_tf_stop *caret = br_tf_stop_for(e, d->caret);
    if (!d->multiline || caret == NULL) {
        return dir < 0 ? 0 : br_tf_bytes(d);
    }
    if (dir < 0 && caret->line == 0) {
        return 0;
    }
    if (dir > 0 && caret->line >= br_tf_last_line(e)) {
        return br_tf_bytes(d);
    }
    float x = caret->x;
    return br_tf_offset_on_line(e, dir < 0 ? caret->line - 1 : caret->line + 1, x);
}

static void br_tf_clear_composition(br_tf_data *d)
{
    d->composition[0] = '\0';
    d->composition_cursor = 0;
}

/* ---- focus */

static void br_tf_start_text_input(br_engine *e, const br_tf_data *d)
{
    SDL_Window *w = br_engine_window(e);
    if (w == NULL) {
        return;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                          d->password ? SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN : SDL_TEXTINPUT_TYPE_TEXT);
    SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, d->multiline);
    if (!SDL_StartTextInputWithProperties(w, props)) {
        br_log_error("text_field: SDL_StartTextInput: %s", SDL_GetError());
    }
    SDL_DestroyProperties(props);
}

void br_text_field_focus(br_engine *e, br_node n)
{
    br_text_field_state *s = e->text_field;
    if (s == NULL) {
        return;
    }
    br_tf_data *next = br_tf_get(e, n);
    br_node old = s->focused;
    if (next != NULL && br_node_equal(old, n)) {
        return;
    }
    br_tf_data *prev = br_tf_get(e, old);
    s->focused = br_tf_zero();
    s->dragging = false;
    if (prev != NULL) {
        br_tf_clear_composition(prev);   /* dropped, not committed: the field is no longer being typed into */
        br_tf_sync(e, old);
    }
    br_input_text_stop(e);
    if (next != NULL) {
        s->focused = n;
        next->blink = 0.0f;
        br_tf_start_text_input(e, next);
        br_input_set_keyboard_captured(e, true);
        br_tf_sync(e, n);
    } else {
        br_input_set_keyboard_captured(e, false);
    }
    if (prev != NULL) {
        br_tf_emit(e, old, BR_TEXT_FIELD_BLURRED);
    }
    if (next != NULL) {
        br_tf_emit(e, n, BR_TEXT_FIELD_FOCUSED);
    }
}

br_node br_text_field_focused(br_engine *e)
{
    br_text_field_state *s = e->text_field;
    if (s == NULL || br_tf_get(e, s->focused) == NULL) {
        return br_tf_zero();
    }
    return s->focused;
}

/* ---- keyboard, text and IME events */

static void br_tf_key(br_engine *e, br_node n, const SDL_KeyboardEvent *k)
{
    br_tf_data *d = br_tf_get(e, n);
    bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;
    bool ctrl = (k->mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    switch (k->scancode) {
    case SDL_SCANCODE_LEFT:
        br_tf_move(e, n, !shift && start != end ? start : ctrl ? br_tf_word_left(d, d->caret) : br_tf_prev(d, d->caret), shift);
        return;
    case SDL_SCANCODE_RIGHT:
        br_tf_move(e, n, !shift && start != end ? end : ctrl ? br_tf_word_right(d, d->caret) : br_tf_next(d, d->caret), shift);
        return;
    case SDL_SCANCODE_HOME:
        br_tf_move(e, n, ctrl ? 0 : br_tf_line_edge(e, d, false), shift);
        return;
    case SDL_SCANCODE_END:
        br_tf_move(e, n, ctrl ? br_tf_bytes(d) : br_tf_line_edge(e, d, true), shift);
        return;
    case SDL_SCANCODE_UP:
        br_tf_move(e, n, br_tf_vertical(e, d, -1), shift);
        return;
    case SDL_SCANCODE_DOWN:
        br_tf_move(e, n, br_tf_vertical(e, d, 1), shift);
        return;
    case SDL_SCANCODE_BACKSPACE:
        br_tf_delete(e, n, false, ctrl);
        return;
    case SDL_SCANCODE_DELETE:
        br_tf_delete(e, n, true, ctrl);
        return;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        if (d->multiline && !ctrl) {
            br_tf_insert(e, n, "\n", BR_TF_EDIT_TYPE);
        } else {
            br_tf_emit(e, n, BR_TEXT_FIELD_SUBMITTED);
        }
        return;
    case SDL_SCANCODE_ESCAPE:
        br_text_field_focus(e, br_tf_zero());
        return;
    default:
        break;
    }
    if (!ctrl) {
        return;
    }
    switch (k->key) {
    case SDLK_A: br_text_field_select_all(e, n); break;
    case SDLK_C: br_text_field_copy(e, n); break;
    case SDLK_X: br_text_field_cut(e, n); break;
    case SDLK_V: br_text_field_paste(e, n); break;
    case SDLK_Z:
    case SDLK_Y: br_text_field_undo(e, n); break;
    default: break;
    }
}

static void br_tf_editing(br_engine *e, br_node n, const SDL_TextEditingEvent *ev)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d->password) {
        return;   /* SDL hides IME for password input; nothing to show */
    }
    if (ev->text == NULL || ev->text[0] == '\0') {
        br_tf_clear_composition(d);   /* cancelled (or about to be committed by a TEXT_INPUT) */
        br_tf_sync(e, n);
        return;
    }
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    if (start != end) {
        br_tf_replace(e, n, start, end, "", 0, BR_TF_EDIT_OTHER);   /* the composition replaces the selection */
        d = br_tf_get(e, n);
        if (d == NULL || !br_node_equal(e->text_field->focused, n)) {
            return;
        }
    }
    br_array clean;
    br_array_init(&clean, 1);
    uint32_t cps = br_tf_sanitize(ev->text, false, BR_TF_COMPOSITION_SIZE / 4 - 1, &clean);
    br_array_push_n(&clean, "", 1);
    SDL_memcpy(d->composition, clean.data, clean.count);
    br_array_free(&clean);
    d->composition_cursor = ev->start < 0 || (uint32_t)ev->start > cps ? (int)cps : ev->start;
    d->blink = 0.0f;
    br_tf_sync(e, n);
}

static void br_tf_sdl_event(br_engine *e, const SDL_Event *ev, void *user)
{
    (void)user;
    br_text_field_state *s = e->text_field;
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev->type == SDL_EVENT_FINGER_DOWN || ev->type == SDL_EVENT_PEN_DOWN) {
        s->press_seen = true;
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP || ev->type == SDL_EVENT_FINGER_UP) {
        s->dragging = false;
    }
    br_node n = br_text_field_focused(e);
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        br_tf_key(e, n, &ev->key);
        break;
    case SDL_EVENT_TEXT_INPUT: {
        bool committed = d->composition[0] != '\0';
        br_tf_clear_composition(d);
        if (!br_tf_insert(e, n, ev->text.text, committed ? BR_TF_EDIT_OTHER : BR_TF_EDIT_TYPE)) {
            br_tf_sync(e, n);
        }
        break;
    }
    case SDL_EVENT_TEXT_EDITING:
        br_tf_editing(e, n, &ev->edit);
        break;
    default:
        break;
    }
}

/* ---- pointer listeners (on every field) */

static void br_tf_pointer(br_engine *e, br_event *ev, void *user)
{
    (void)user;
    br_text_field_state *s = e->text_field;
    br_node n = ev->current_target;
    if (br_tf_get(e, n) == NULL) {
        return;
    }
    if (ev->type == BR_EVENT_POINTER_UP || ev->type == BR_EVENT_POINTER_UP_OUTSIDE) {
        s->dragging = false;
        return;
    }
    br_vec2 local = br_event_local(e, ev);
    if (ev->type == BR_EVENT_POINTER_MOVE || ev->type == BR_EVENT_GLOBAL_MOVE) {
        if (s->dragging && br_node_equal(s->focused, n)) {
            br_tf_move(e, n, br_text_field_offset_at(e, n, local), true);
        }
        return;
    }
    /* POINTER_DOWN */
    s->press_claimed = true;
    br_event_stop_propagation(ev);
    br_text_field_focus(e, n);
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || !br_node_equal(s->focused, n)) {
        return;   /* a FOCUSED listener moved focus or destroyed the field */
    }
    br_vec2 delta = br_vec2_sub(ev->global, s->last_click_pos);
    bool again = br_node_equal(s->last_click_node, n) && ev->timestamp_ns - s->last_click_ns < BR_TF_DOUBLE_CLICK_NS &&
                 br_vec2_length(delta) < BR_TF_DOUBLE_CLICK_PX;
    s->click_count = again ? s->click_count + 1 : 1;
    s->last_click_ns = ev->timestamp_ns;
    s->last_click_node = n;
    s->last_click_pos = ev->global;
    uint32_t at = br_text_field_offset_at(e, n, local);
    if (s->click_count == 2) {
        uint32_t start = at;
        uint32_t end = at;
        int cls = br_tf_class(br_tf_cp_at(d, at < br_tf_bytes(d) ? at : br_tf_prev(d, at)));
        while (start > 0 && br_tf_class(br_tf_cp_at(d, br_tf_prev(d, start))) == cls) {
            start = br_tf_prev(d, start);
        }
        while (end < br_tf_bytes(d) && br_tf_class(br_tf_cp_at(d, end)) == cls) {
            end = br_tf_next(d, end);
        }
        br_tf_select(e, n, start, end);
    } else if (s->click_count >= 3) {
        br_text_field_select_all(e, n);
    } else {
        bool shift = (br_input_key_mods(e) & SDL_KMOD_SHIFT) != 0;
        br_tf_move(e, n, at, shift);
        s->dragging = true;
    }
}

/* ---- systems */

/* Drops copies of the internal children (a saved field reloaded brings them back as children). */
static void br_tf_drop_stale_children(br_engine *e, br_node n, const br_tf_data *d)
{
    for (uint32_t i = br_node_child_count(e, n); i-- > 0;) {
        br_node c = br_node_child_at(e, n, i);
        const char *label = br_node_label(e, c);
        if (!br_node_equal(c, d->text_node) && !br_node_equal(c, d->clip) &&
            (br_str_equal(label, BR_TF_TEXT_LABEL) || br_str_equal(label, BR_TF_CLIP_LABEL))) {
            br_node_destroy(e, c);
        }
    }
}

static void br_tf_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_text_field_state *s = user;
    for (uint32_t i = 0; i < s->fields.count; i++) {
        br_node n = *(br_node *)br_array_at(&s->fields, i);
        const br_tf_data *d = br_tf_get(e, n);
        if (d != NULL && br_node_child_count(e, n) > 2) {
            br_tf_drop_stale_children(e, n, d);
        }
    }
    bool blur = s->press_seen && !s->press_claimed;
    s->press_seen = false;
    s->press_claimed = false;
    if (blur) {
        br_text_field_focus(e, br_tf_zero());
    }
    br_node n = br_text_field_focused(e);
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    d->blink += br_ticker_unscaled_delta(e);
    d->blink = SDL_fmodf(d->blink, 1000.0f * BR_TF_BLINK_PERIOD);
    br_rect caret;
    if (br_engine_window(e) != NULL && br_text_field_get_caret_rect(e, n, &caret)) {
        /* the IME candidate list goes under the field, next to the caret */
        br_vec2 a = br_camera_stage_to_screen(e, n, br_node_to_global(e, n, br_vec2_make(0.0f, 0.0f)));
        br_vec2 b = br_camera_stage_to_screen(e, n, br_node_to_global(e, n, d->size));
        br_vec2 c = br_camera_stage_to_screen(e, n, br_node_to_global(e, n, br_vec2_make(caret.x, caret.y)));
        br_input_text_set_area(e, br_rect_make(a.x, a.y, b.x - a.x, b.y - a.y), (int)(c.x - a.x));
    }
}

/* ---- node type */

static void br_tf_quad(br_render_ctx *rc, br_texture white, br_mat3 m, br_rect r, br_color color, float alpha)
{
    if (color.a <= 0.0f || r.width <= 0.0f || r.height <= 0.0f) {
        return;
    }
    uint32_t packed = br_render_pack_color(color, alpha);
    br_vec2 p[4] = { br_vec2_make(r.x, r.y), br_vec2_make(r.x + r.width, r.y),
                     br_vec2_make(r.x + r.width, r.y + r.height), br_vec2_make(r.x, r.y + r.height) };
    br_vertex2d v[4];
    for (int i = 0; i < 4; i++) {
        br_vec2 q = br_mat3_apply(m, p[i]);
        v[i].x = q.x;
        v[i].y = q.y;
        v[i].u = 0.5f;
        v[i].v = 0.5f;
        v[i].color = packed;
    }
    br_render_push_quad(rc, white, v);
}

static void br_tf_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    const br_tf_data *d = data;
    br_text_field_state *s = e->text_field;
    br_mat3 m = br_render_node_transform(rc, n);
    br_texture white = br_texture_white(e);
    uint32_t packed = br_render_node_color(rc, n);
    uint8_t rgba[4];
    SDL_memcpy(rgba, &packed, 4);
    float alpha = (float)rgba[3] / 255.0f;
    bool focused = br_node_equal(s->focused, n);
    float w = d->size.x;
    float h = d->size.y;

    br_tf_quad(rc, white, m, br_rect_make(0.0f, 0.0f, w, h), d->colors.background, alpha);
    br_color border = focused ? d->colors.focus_border : d->colors.border;
    br_tf_quad(rc, white, m, br_rect_make(0.0f, 0.0f, w, 1.0f), border, alpha);
    br_tf_quad(rc, white, m, br_rect_make(0.0f, h - 1.0f, w, 1.0f), border, alpha);
    br_tf_quad(rc, white, m, br_rect_make(0.0f, 1.0f, 1.0f, h - 2.0f), border, alpha);
    br_tf_quad(rc, white, m, br_rect_make(w - 1.0f, 1.0f, 1.0f, h - 2.0f), border, alpha);

    /* stops of this field (the scratch array holds whichever field was built last)
     * ponytail: rebuilt every frame, O(text length) per field; cache them per field if long
     * multi-line fields show up in profiles. */
    br_tf_build_display(e, d);
    br_tf_build_stops(e, d);
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    uint32_t comp = br_tf_composition_bytes(d);
    for (uint32_t i = 0; i < s->stops.count; i++) {
        const br_tf_stop *st = br_array_at(&s->stops, i);
        br_text_line_info line = br_tf_line(e, d, st->line);
        float x = d->origin.x + st->x;
        float y = d->origin.y + line.y;
        if (start != end && st->content >= start && st->content < end) {
            float width = st->width > 0.0f ? st->width : line.height * 0.25f;   /* a selected '\n' shows as a sliver */
            br_tf_quad(rc, white, m, br_rect_make(x, y, width, line.height), d->colors.selection,
                       alpha * (focused ? 1.0f : 0.5f));
        }
        if (comp > 0 && st->display >= d->caret && st->display < d->caret + comp) {
            br_tf_quad(rc, white, m, br_rect_make(x, y + line.height - 2.0f, st->width, 1.5f), d->style.fill, alpha);
        }
    }
    if (focused && d->blink - SDL_floorf(d->blink / BR_TF_BLINK_PERIOD) * BR_TF_BLINK_PERIOD < 0.5f * BR_TF_BLINK_PERIOD) {
        const br_tf_stop *st = br_tf_caret_stop(e, d);
        br_text_line_info line = br_tf_line(e, d, st != NULL ? st->line : 0);
        float x = d->origin.x + (st != NULL ? st->x : 0.0f);
        br_tf_quad(rc, white, m, br_rect_make(x, d->origin.y + line.y, BR_TF_CARET_WIDTH, line.height), d->colors.caret,
                   alpha);
    }
}

static bool br_tf_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)e;
    (void)n;
    const br_tf_data *d = data;
    *out = br_rect_make(0.0f, 0.0f, d->size.x, d->size.y);
    return true;
}

static bool br_tf_hit(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    (void)e;
    (void)n;
    const br_tf_data *d = data;
    return p.x >= 0.0f && p.y >= 0.0f && p.x < d->size.x && p.y < d->size.y;
}

static void br_tf_on_create(br_engine *e, br_node n, void *data)
{
    br_text_field_state *s = e->text_field;
    br_tf_data *d = data;
    br_array_init(&d->text, 1);
    br_array_init(&d->undo_text, 1);
    br_array_push_n(&d->text, "", 1);
    d->max_length = 256;
    d->size = br_vec2_make(200.0f, 40.0f);
    d->padding = 8.0f;
    d->style = br_text_style_default();
    d->colors = br_text_field_colors_default();
    br_text_style st = br_tf_text_style(d, true);
    br_node text = br_text_create(e, "", &st);
    br_node clip = br_sprite_create(e, br_texture_white(e));
    br_node_set_label(e, text, BR_TF_TEXT_LABEL);
    br_node_set_label(e, clip, BR_TF_CLIP_LABEL);
    br_node_add_child(e, n, clip);
    br_node_add_child(e, n, text);
    br_node_set_mask(e, n, clip, BR_MASK_SCISSOR, false);
    br_node_set_event_mode(e, n, BR_EVENT_MODE_STATIC);
    br_node_set_cursor(e, n, SDL_SYSTEM_CURSOR_TEXT);
    br_event_type types[] = { BR_EVENT_POINTER_DOWN, BR_EVENT_POINTER_MOVE, BR_EVENT_GLOBAL_MOVE, BR_EVENT_POINTER_UP,
                              BR_EVENT_POINTER_UP_OUTSIDE };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(types); i++) {
        br_node_on(e, n, types[i], br_tf_pointer, NULL);
    }
    br_array_push(&s->fields, &n);
    d = br_tf_get(e, n);   /* creating the children may not move it, but stay safe */
    d->text_node = text;
    d->clip = clip;
    br_tf_sync(e, n);
}

static void br_tf_on_destroy(br_engine *e, br_node n, void *data)
{
    br_text_field_state *s = e->text_field;
    br_tf_data *d = data;
    if (br_node_equal(s->focused, n)) {
        s->focused = br_tf_zero();
        s->dragging = false;
        br_input_text_stop(e);
        br_input_set_keyboard_captured(e, false);
    }
    for (uint32_t i = 0; i < s->fields.count; i++) {
        if (br_node_equal(*(br_node *)br_array_at(&s->fields, i), n)) {
            br_array_remove_swap(&s->fields, i);
            break;
        }
    }
    br_array_free(&d->text);
    br_array_free(&d->undo_text);
    br_free(d->placeholder);
    d->placeholder = NULL;
}

/* ---- scene files */

static void br_tf_color(br_engine *e, const cJSON *json, const char *key, br_color *out)
{
    br_data_get_color(e, json, key, out);
}

static bool br_tf_from_json(br_engine *e, br_node n, void *data, const cJSON *json)
{
    (void)data;
    br_vec2 size = br_text_field_size(e, n);
    br_data_get_vec2(e, json, "size", &size);
    br_text_field_set_size(e, n, size);
    float padding;
    if (br_data_get_float(e, json, "padding", &padding)) {
        br_text_field_set_padding(e, n, padding);
    }
    uint32_t max_length;
    if (br_data_get_count(e, json, "max_length", 1, BR_TEXT_FIELD_MAX_LENGTH, &max_length)) {
        br_text_field_set_max_length(e, n, max_length);
    }
    bool flag;
    if (br_data_get_bool(e, json, "multiline", &flag)) {
        br_text_field_set_multiline(e, n, flag);
    }
    if (br_data_get_bool(e, json, "password", &flag)) {
        br_text_field_set_password(e, n, flag);
    }
    const cJSON *style_json = cJSON_GetObjectItemCaseSensitive(json, "style");
    if (style_json != NULL) {
        br_text_style style;
        br_text_field_get_style(e, n, &style);
        size_t mark = br_data_path_push_key(e, "style");
        br_node_json_text_style(e, style_json, &style);
        br_data_path_pop(e, mark);
        br_text_field_set_style(e, n, &style);
    }
    const cJSON *colors_json = cJSON_GetObjectItemCaseSensitive(json, "colors");
    if (colors_json != NULL) {
        size_t mark = br_data_path_push_key(e, "colors");
        if (!cJSON_IsObject(colors_json)) {
            br_data_errorf(e, "expected an object of colours");
        } else {
            br_text_field_colors c;
            br_text_field_get_colors(e, n, &c);
            br_tf_color(e, colors_json, "background", &c.background);
            br_tf_color(e, colors_json, "border", &c.border);
            br_tf_color(e, colors_json, "focus_border", &c.focus_border);
            br_tf_color(e, colors_json, "selection", &c.selection);
            br_tf_color(e, colors_json, "caret", &c.caret);
            br_tf_color(e, colors_json, "placeholder", &c.placeholder);
            br_text_field_set_colors(e, n, &c);
        }
        br_data_path_pop(e, mark);
    }
    const cJSON *placeholder = cJSON_GetObjectItemCaseSensitive(json, "placeholder");
    if (cJSON_IsString(placeholder)) {
        br_text_field_set_placeholder(e, n, placeholder->valuestring);
    } else if (placeholder != NULL) {
        br_data_error(e, "placeholder", "expected a string");
    }
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
    if (cJSON_IsString(text)) {
        br_text_field_set_text(e, n, text->valuestring);
    } else if (text != NULL) {
        br_data_error(e, "text", "expected a string");
    }
    return true;
}

static void br_tf_write(br_engine *e, br_node n, cJSON *out)
{
    br_data_json_set(out, "text", cJSON_CreateString(br_text_field_text(e, n)));
}

/* ---- module */

static void br_tf_shutdown(br_engine *e, void *user)
{
    br_text_field_state *s = user;
    br_array_free(&s->fields);
    br_array_free(&s->display);
    br_array_free(&s->stops);
    br_free(s);
    e->text_field = NULL;
}

void br_text_field_register(br_engine *e)
{
    br_text_field_state *s = br_alloc(sizeof(br_text_field_state));
    if (s == NULL) {
        return;
    }
    SDL_zerop(s);
    br_array_init(&s->fields, sizeof(br_node));
    br_array_init(&s->display, 1);
    br_array_init(&s->stops, sizeof(br_tf_stop));
    e->text_field = s;
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = "text_field";
    desc.data_size = sizeof(br_tf_data);
    desc.on_create = br_tf_on_create;
    desc.on_destroy = br_tf_on_destroy;
    desc.local_bounds = br_tf_bounds;
    desc.render = br_tf_render;
    desc.hit_test = br_tf_hit;
    desc.from_json = br_tf_from_json;
    s->type = br_node_type_register(e, &desc);
    br_data_add_type(e, "text_field", s->type, NULL, br_tf_write);
    br_engine_add_event_handler(e, br_tf_sdl_event, NULL);
    /* after the INPUT phase, where pointer presses (2D and canvas3d picks) reach the fields */
    br_system_add(e, "text_field.update", BR_PHASE_LATE_UPDATE, 0, br_tf_update, s);
    br_engine_on_shutdown(e, br_tf_shutdown, s);
}

/* ---- API */

uint32_t br_text_field_type(br_engine *e)
{
    return e->text_field != NULL ? e->text_field->type : 0;
}

br_text_field_colors br_text_field_colors_default(void)
{
    br_text_field_colors c;
    c.background = br_color_from_hex(0xffffff, 1.0f);
    c.border = br_color_from_hex(0x8a8f99, 1.0f);
    c.focus_border = br_color_from_hex(0x2f7fe0, 1.0f);
    c.selection = br_color_from_hex(0x9cc4ff, 1.0f);
    c.caret = br_color_from_hex(0x111111, 1.0f);
    c.placeholder = br_color_from_hex(0x9097a0, 1.0f);
    return c;
}

br_node br_text_field_create(br_engine *e, br_vec2 size, const br_text_style *style)
{
    br_node n = br_node_create(e, br_text_field_type(e));
    if (br_tf_get(e, n) == NULL) {
        return br_tf_zero();
    }
    br_text_field_set_size(e, n, size);
    if (style != NULL) {
        br_text_field_set_style(e, n, style);
    }
    return n;
}

void br_text_field_set_text(br_engine *e, br_node n, const char *utf8)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    br_array clean;
    br_array_init(&clean, 1);
    br_tf_sanitize(utf8, d->multiline, d->max_length, &clean);
    br_tf_set_bytes(d, clean.count > 0 ? clean.data : "", clean.count);
    br_array_free(&clean);
    br_array_clear(&d->undo_text);
    d->last_edit = BR_TF_EDIT_NONE;
    br_tf_clear_composition(d);
    d->anchor = d->caret = br_tf_bytes(d);
    br_tf_moved(e, n);
}

const char *br_text_field_text(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL ? br_tf_chars(d) : "";
}

bool br_text_field_insert(br_engine *e, br_node n, const char *utf8)
{
    return br_tf_insert(e, n, utf8, BR_TF_EDIT_OTHER);
}

void br_text_field_set_placeholder(br_engine *e, br_node n, const char *utf8)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    br_array clean;
    br_array_init(&clean, 1);
    br_tf_sanitize(utf8, d->multiline, BR_TEXT_FIELD_MAX_LENGTH, &clean);
    br_array_push_n(&clean, "", 1);
    br_free(d->placeholder);
    d->placeholder = br_strdup(clean.data);
    br_array_free(&clean);
    br_tf_sync(e, n);
}

const char *br_text_field_placeholder(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL && d->placeholder != NULL ? d->placeholder : "";
}

void br_text_field_set_max_length(br_engine *e, br_node n, uint32_t max_length)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    if (max_length < 1 || max_length > BR_TEXT_FIELD_MAX_LENGTH) {
        br_log_error("text_field: max length %u outside 1..%d", max_length, BR_TEXT_FIELD_MAX_LENGTH);
        return;
    }
    d->max_length = max_length;
    if (d->length > max_length) {
        uint32_t anchor = d->anchor;
        uint32_t caret = d->caret;
        br_array copy;
        br_array_init(&copy, 1);
        br_array_push_n(&copy, br_tf_chars(d), br_tf_bytes(d) + 1);
        br_text_field_set_text(e, n, copy.data);
        br_array_free(&copy);
        br_text_field_set_selection(e, n, anchor, caret);
    }
}

uint32_t br_text_field_max_length(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL ? d->max_length : 0;
}

void br_text_field_set_selection(br_engine *e, br_node n, uint32_t anchor, uint32_t caret)
{
    br_tf_select(e, n, anchor, caret);
}

bool br_text_field_get_selection(br_engine *e, br_node n, uint32_t *anchor, uint32_t *caret)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return false;
    }
    if (anchor != NULL) {
        *anchor = d->anchor;
    }
    if (caret != NULL) {
        *caret = d->caret;
    }
    return true;
}

void br_text_field_select_all(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d != NULL) {
        br_tf_select(e, n, 0, br_tf_bytes(d));
    }
}

bool br_text_field_get_caret_rect(br_engine *e, br_node n, br_rect *out)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return false;
    }
    br_tf_sync(e, n);
    d = br_tf_get(e, n);
    const br_tf_stop *st = br_tf_caret_stop(e, d);
    br_text_line_info line = br_tf_line(e, d, st != NULL ? st->line : 0);
    *out = br_rect_make(d->origin.x + (st != NULL ? st->x : 0.0f), d->origin.y + line.y, 1.0f, line.height);
    return true;
}

uint32_t br_text_field_offset_at(br_engine *e, br_node n, br_vec2 local_point)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return 0;
    }
    br_tf_sync(e, n);
    d = br_tf_get(e, n);
    br_vec2 p = br_vec2_sub(local_point, d->origin);
    uint32_t last = br_tf_last_line(e);
    uint32_t line = 0;
    while (line < last) {
        br_text_line_info info = br_tf_line(e, d, line);
        if (p.y < info.y + info.height) {
            break;
        }
        line++;
    }
    return br_tf_offset_on_line(e, line, p.x);
}

const char *br_text_field_composition(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL ? d->composition : "";
}

bool br_text_field_copy(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || d->password) {
        return false;
    }
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    if (start == end) {
        return false;
    }
    char *copy = br_alloc(end - start + 1);
    if (copy == NULL) {
        return false;
    }
    SDL_memcpy(copy, br_tf_chars(d) + start, end - start);
    copy[end - start] = '\0';
    bool ok = SDL_SetClipboardText(copy);
    if (!ok) {
        br_log_error("text_field: copy: %s", SDL_GetError());
    }
    br_free(copy);
    return ok;
}

bool br_text_field_cut(br_engine *e, br_node n)
{
    if (!br_text_field_copy(e, n)) {
        return false;
    }
    br_tf_data *d = br_tf_get(e, n);
    uint32_t start;
    uint32_t end;
    br_tf_selection_range(d, &start, &end);
    br_tf_replace(e, n, start, end, "", 0, BR_TF_EDIT_OTHER);
    return true;
}

bool br_text_field_paste(br_engine *e, br_node n)
{
    if (br_tf_get(e, n) == NULL) {
        return false;
    }
    char *text = SDL_GetClipboardText();   /* "" (not NULL) when empty or unavailable */
    bool ok = text != NULL && br_tf_insert(e, n, text, BR_TF_EDIT_OTHER);
    SDL_free(text);
    return ok;
}

bool br_text_field_undo(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || d->undo_text.count == 0) {
        return false;
    }
    /* swap: the undone text becomes the redo step */
    br_array current = d->text;
    uint32_t anchor = d->anchor;
    uint32_t caret = d->caret;
    d->text = d->undo_text;
    d->undo_text = current;
    d->length = br_tf_count_cps(br_tf_chars(d), br_tf_bytes(d));
    d->anchor = d->undo_anchor;
    d->caret = d->undo_caret;
    d->undo_anchor = anchor;
    d->undo_caret = caret;
    d->last_edit = BR_TF_EDIT_NONE;
    br_tf_clear_composition(d);
    br_tf_moved(e, n);
    br_tf_emit(e, n, BR_TEXT_FIELD_CHANGED);
    return true;
}

void br_text_field_set_multiline(br_engine *e, br_node n, bool multiline)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || d->multiline == multiline) {
        return;
    }
    d->multiline = multiline;
    if (!multiline) {
        char *s = d->text.data;
        for (uint32_t i = 0; i < br_tf_bytes(d); i++) {
            s[i] = s[i] == '\n' || s[i] == '\t' ? ' ' : s[i];
        }
        d->scroll.y = 0.0f;
    }
    br_tf_sync(e, n);
}

bool br_text_field_multiline(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL && d->multiline;
}

void br_text_field_set_password(br_engine *e, br_node n, bool password)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    d->password = password;
    br_tf_clear_composition(d);
    br_tf_sync(e, n);
}

bool br_text_field_password(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL && d->password;
}

void br_text_field_set_size(br_engine *e, br_node n, br_vec2 size)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    if (!(size.x >= 1.0f && size.x <= BR_TF_MAX_SIZE && size.y >= 1.0f && size.y <= BR_TF_MAX_SIZE)) {
        br_log_error("text_field: size %g x %g outside 1..%g", (double)size.x, (double)size.y, (double)BR_TF_MAX_SIZE);
        return;
    }
    d->size = size;
    br_tf_sync(e, n);
}

br_vec2 br_text_field_size(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL ? d->size : br_vec2_make(0.0f, 0.0f);
}

void br_text_field_set_padding(br_engine *e, br_node n, float padding)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return;
    }
    if (!(padding >= 0.0f && padding <= 1000.0f)) {
        br_log_error("text_field: padding %g outside 0..1000", (double)padding);
        return;
    }
    d->padding = padding;
    br_tf_sync(e, n);
}

float br_text_field_padding(br_engine *e, br_node n)
{
    br_tf_data *d = br_tf_get(e, n);
    return d != NULL ? d->padding : 0.0f;
}

void br_text_field_set_style(br_engine *e, br_node n, const br_text_style *style)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || style == NULL) {
        return;
    }
    d->style = *style;
    br_tf_sync(e, n);
}

bool br_text_field_get_style(br_engine *e, br_node n, br_text_style *out)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return false;
    }
    *out = d->style;
    return true;
}

void br_text_field_set_colors(br_engine *e, br_node n, const br_text_field_colors *colors)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL || colors == NULL) {
        return;
    }
    d->colors = *colors;
    br_tf_sync(e, n);
}

bool br_text_field_get_colors(br_engine *e, br_node n, br_text_field_colors *out)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d == NULL) {
        return false;
    }
    *out = d->colors;
    return true;
}

void br_text_field_set_listener(br_engine *e, br_node n, br_text_field_fn fn, void *user)
{
    br_tf_data *d = br_tf_get(e, n);
    if (d != NULL) {
        d->fn = fn;
        d->user = user;
    }
}
