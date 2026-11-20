/* br_text.h - the text node and text styles (PixiJS Text, TextStyle).
 *
 * A text node lays out UTF-8 text with a br_text_style (wrap, align, spacing) and draws glyph
 * quads from its font: drop shadow, then stroke, then fill (colour or gradient). Layout is
 * cached and redone only when the text or style changes. The node's local origin is the
 * top-left of the text box, moved by the anchor (0.5, 0.5 = centred).
 *
 * The same layout and drawing serve bitmap text (br_bitmap_font.h) and rich text
 * (br_rich_text.h). JSON type name: "text".
 *
 *   br_text_style st = br_text_style_default();
 *   st.font_size = 32;
 *   br_node t = br_text_create(e, "Hello", &st);
 */
#ifndef BR_TEXT_H
#define BR_TEXT_H

#include "braty/br_font.h"
#include "braty/br_render.h"

#define BR_TEXT_MAX_STOPS 8
#define BR_TEXT_FAMILY_SIZE 64

typedef enum br_text_align {
    BR_TEXT_ALIGN_LEFT,
    BR_TEXT_ALIGN_CENTER,
    BR_TEXT_ALIGN_RIGHT,
    BR_TEXT_ALIGN_JUSTIFY  /* stretches spaces on wrapped lines; the last line stays left */
} br_text_align;

/* What happens to spaces and newlines (CSS white-space). */
typedef enum br_text_white_space {
    BR_TEXT_WHITE_SPACE_PRE,      /* keep everything (default, like Pixi) */
    BR_TEXT_WHITE_SPACE_NORMAL,   /* collapse runs of spaces and newlines into one space */
    BR_TEXT_WHITE_SPACE_PRE_LINE  /* collapse spaces, keep newlines */
} br_text_white_space;

/* Which line of the text box sits at local y = 0 (before the anchor is applied). */
typedef enum br_text_baseline {
    BR_TEXT_BASELINE_TOP,         /* top of the box (default) */
    BR_TEXT_BASELINE_ALPHABETIC,  /* baseline of the first line */
    BR_TEXT_BASELINE_MIDDLE,      /* middle of the box */
    BR_TEXT_BASELINE_BOTTOM       /* bottom of the box */
} br_text_baseline;

/* Linear gradient over the text box. start/end are fractions of the box (0,0 = top-left). */
typedef struct br_text_gradient {
    uint32_t stop_count;                    /* 2..BR_TEXT_MAX_STOPS */
    float    offsets[BR_TEXT_MAX_STOPS];    /* ascending, 0..1 */
    br_color colors[BR_TEXT_MAX_STOPS];
    br_vec2  start;
    br_vec2  end;
} br_text_gradient;

/* Everything about how text looks (PixiJS TextStyle). Start from br_text_style_default(). */
typedef struct br_text_style {
    br_font *font;                          /* wins over font_family when set */
    char     font_family[BR_TEXT_FAMILY_SIZE]; /* "" = "default" */
    float    font_size;                     /* pixels; bitmap text: <= 0 = the font's native size */
    bool     bold;                          /* font weight (faked if the family has no bold face) */
    bool     italic;                        /* font style (faked by shearing if no italic face) */

    br_color fill;                          /* used when use_gradient is false */
    bool     use_gradient;
    br_text_gradient gradient;

    float    stroke_width;                  /* 0 = no stroke; centred on the outline */
    br_color stroke_color;

    bool     drop_shadow;
    br_color shadow_color;                  /* alpha = shadow alpha */
    float    shadow_angle;                  /* radians, 0 = right, pi/2 = down */
    float    shadow_distance;
    float    shadow_blur;                   /* pixels of softening */

    br_text_align       align;
    bool                word_wrap;
    float               wrap_width;
    bool                break_words;        /* split words longer than wrap_width */
    float               line_height;        /* 0 = the font's natural line height */
    float               leading;            /* extra space between lines */
    float               letter_spacing;     /* extra space after every character */
    float               padding;            /* extra space around the text in the bounds */
    br_text_white_space white_space;
    br_text_baseline    baseline;
} br_text_style;

/* Size of laid-out text (br_text_measure). */
typedef struct br_text_metrics {
    float    width;           /* widest line */
    float    height;          /* all lines including leading */
    float    line_height;     /* distance between the first two baselines (or the only line's height) */
    uint32_t line_count;
} br_text_metrics;

/* One laid-out line of a text node (local coordinates, before the anchor). */
typedef struct br_text_line_info {
    uint32_t byte_start;      /* offset of the line's first character in the (plain) text */
    uint32_t byte_end;        /* offset just after its last visible character */
    float    x, y;            /* top-left of the line box after alignment */
    float    width, height;
    float    baseline;        /* y of the baseline */
} br_text_line_info;

/* ---- styles */

/* Pixi defaults: default font, 26 px, black fill, left aligned, no wrap. */
br_text_style br_text_style_default(void);
/* Sets the font family name (copied, truncated to BR_TEXT_FAMILY_SIZE - 1). */
void br_text_style_set_family(br_text_style *style, const char *family);
/* Makes a two-stop gradient from `top` to `bottom` (vertical over the text box). */
void br_text_style_set_vertical_gradient(br_text_style *style, br_color top, br_color bottom);

/* ---- the text node */

/* Type id of text nodes ("text"). */
uint32_t br_text_type(br_engine *e);
/* Creates a detached text node. style NULL = br_text_style_default(). */
br_node br_text_create(br_engine *e, const char *utf8, const br_text_style *style);
/* Replaces the text (copied). Works on text, bitmap text and rich text nodes. */
void br_text_set_text(br_engine *e, br_node n, const char *utf8);
/* The text as set ("" for non-text nodes). */
const char *br_text_get_text(br_engine *e, br_node n);
/* Replaces the style (copied). */
void br_text_set_style(br_engine *e, br_node n, const br_text_style *style);
/* Copies the style out. False for non-text nodes. */
bool br_text_get_style(br_engine *e, br_node n, br_text_style *out);
/* Sets the anchor (fraction of the measured size; 0.5, 0.5 = centred on the position). */
void br_text_set_anchor(br_engine *e, br_node n, float x, float y);
/* Raster pixels per local unit for coverage glyphs (0 = the window's pixel density). */
void br_text_set_resolution(br_engine *e, br_node n, float resolution);
/* Measured size of the node's text box (padding excluded). */
br_vec2 br_text_size(br_engine *e, br_node n);
/* Number of laid-out lines. */
uint32_t br_text_line_count(br_engine *e, br_node n);
/* Fills *out for line i. False if out of range. */
bool br_text_get_line(br_engine *e, br_node n, uint32_t i, br_text_line_info *out);
/* Number of laid-out characters (spaces included, newlines and tags excluded). */
uint32_t br_text_char_count(br_engine *e, br_node n);
/* Local box of character i (before the anchor): x at its pen position, full line height. */
bool br_text_get_char_box(br_engine *e, br_node n, uint32_t i, br_rect *out);
/* Lays the node out and rasterises the glyphs its next draw needs (at the window's pixel
 * density unless br_text_set_resolution), so the first frame showing it does not stall. */
void br_text_prepare(br_engine *e, br_node n);

/* ---- measuring without a node */

/* Lays out text with a style and reports its size. */
br_text_metrics br_text_measure(br_engine *e, const char *utf8, const br_text_style *style);

/* ---- UTF-8 */

/* Decodes the codepoint at text[*offset] and advances *offset (by 1 for invalid bytes, which
 * decode as U+FFFD). Returns 0 at the end (offset >= length or a NUL byte). */
uint32_t br_text_utf8_next(const char *text, size_t length, size_t *offset);
/* Encodes a codepoint into out (no terminator); returns the byte count (1-4, 0 if invalid). */
uint32_t br_text_utf8_encode(uint32_t codepoint, char out[4]);

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_text_register(br_engine *e);

#endif
