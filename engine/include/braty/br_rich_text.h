/* br_rich_text.h - tagged rich text (replaces PixiJS HTMLText) and split text (SplitText).
 *
 * Rich text takes markup and a base br_text_style. Tags:
 *   <b> <i> <u> <s>             bold, italic, underline, strikethrough
 *   <br>                        line break (also "<br/>")
 *   <color=#ff8800> <color=red> fill colour (any br_color_parse string)
 *   <size=32>                   font size in pixels
 *   <font=family>               font family (br_font_add_family / br_bitmap_font_install names)
 *   <name>                      a named tag style (Pixi tagStyles, br_rich_text_set_tag_style)
 * Close with </b>, </color>, </name> ... Unknown tags are drawn as text. Entities: &lt; &gt; &amp;.
 * Wrap, align, spacing, stroke and shadow come from the base style. JSON type name: "rich_text".
 *
 * Split text lays out text once and creates one br_text child per character, word or line at
 * the place it had, anchored at its centre, so each piece can be moved, rotated and scaled.
 */
#ifndef BR_RICH_TEXT_H
#define BR_RICH_TEXT_H

#include "braty/br_render.h"
#include "braty/br_text.h"

/* A named tag style. Zero/false fields inherit from the enclosing text. */
typedef struct br_rich_tag_style {
    char     font_family[BR_TEXT_FAMILY_SIZE];  /* "" = inherit */
    float    font_size;                         /* 0 = inherit */
    bool     set_color;
    br_color color;
    bool     bold, italic, underline, strike;   /* true turns it on */
} br_rich_tag_style;

/* What one character of rich text resolved to. */
typedef struct br_rich_char_style {
    br_font *font;
    float    font_size;
    br_color color;
    bool     use_gradient;   /* no colour tag: the base style's gradient applies */
    bool     bold, italic, underline, strike;
} br_rich_char_style;

/* ---- rich text */

/* Type id of rich text nodes ("rich_text"). */
uint32_t br_rich_text_type(br_engine *e);
/* Creates detached rich text from markup. style NULL = br_text_style_default(). */
br_node br_rich_text_create(br_engine *e, const char *markup, const br_text_style *style);
/* Adds or replaces a named tag style on this node (tag name copied, max 31 bytes). */
void br_rich_text_set_tag_style(br_engine *e, br_node n, const char *tag, const br_rich_tag_style *style);
/* The text with tags removed and entities decoded (what br_text_get_line offsets refer to). */
const char *br_rich_text_plain(br_engine *e, br_node n);
/* Resolved style of laid-out character i (as br_text_char_count counts). False if out of range. */
bool br_rich_text_char_style(br_engine *e, br_node n, uint32_t i, br_rich_char_style *out);

/* ---- split text (PixiJS SplitText) */

typedef enum br_split_mode {
    BR_SPLIT_CHARS,   /* one node per visible character */
    BR_SPLIT_WORDS,   /* one node per run of non-space characters */
    BR_SPLIT_LINES    /* one node per laid-out line */
} br_split_mode;

/* Creates a container with one centred-anchor br_text child per piece, in reading order.
 * The container's local space matches a br_text node of the same text and style. */
br_node br_split_text_create(br_engine *e, const char *utf8, const br_text_style *style, br_split_mode mode);

#endif
