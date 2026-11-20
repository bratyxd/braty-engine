/* br_bitmap_font.h - bitmap fonts and bitmap text (PixiJS BitmapFont, BitmapText).
 *
 * A bitmap font is a br_font with fixed page textures: AngelCode BMFont files (text or XML
 * .fnt, multi-page, kerning, SDF / MSDF distance fields from msdf-bmfont or Hiero), or one
 * generated from a TTF font and a text style with br_bitmap_font_install (fill, gradient,
 * stroke and shadow baked into the pages). Bitmap text is laid out like br_text and scaled
 * from the font's native size; plain-white fonts are tinted by the style's fill colour,
 * installed coloured fonts draw as baked. JSON type name: "bitmap_text".
 *
 *   br_font *f = br_bitmap_font_load(e, "fonts/pixel.fnt");      (pages next to the .fnt)
 *   br_node t = br_bitmap_text_create(e, "SCORE 100", f, 0);     (0 = native size)
 */
#ifndef BR_BITMAP_FONT_H
#define BR_BITMAP_FONT_H

#include "braty/br_font.h"
#include "braty/br_render.h"
#include "braty/br_text.h"

/* One glyph of a bitmap font (AngelCode "char" line), in the font's native pixels. */
typedef struct br_bitmap_char {
    uint32_t id;           /* codepoint */
    uint32_t page;
    float    x, y, width, height;   /* rect in the page texture */
    float    xoffset, yoffset;      /* from the pen position / the line top */
    float    xadvance;
} br_bitmap_char;

/* Options for br_bitmap_font_install. Zero = ASCII 32..126, resolution 1, padding 2, 512 pages. */
typedef struct br_bitmap_font_options {
    const char *chars;       /* UTF-8 list of characters to include; NULL = printable ASCII */
    float       resolution;  /* raster pixels per unit (2 = crisp when scaled up 2x) */
    int         padding;     /* empty pixels around every glyph in the pages */
    uint32_t    page_size;   /* page width and height in pixels */
} br_bitmap_font_options;

/* ---- loading */

/* Loads a .fnt / .xml AngelCode font and its pages (cached by the assets module). NULL on failure. */
br_font *br_bitmap_font_load(br_engine *e, const char *path);
/* Parses AngelCode text or XML from memory; page files resolve against page_dir ("" = asset root).
 * load_pages false skips the page textures (tools, tests). Free with br_font_destroy. */
br_font *br_bitmap_font_parse(br_engine *e, const char *data, size_t size, const char *page_dir, bool load_pages);

/* ---- generating (PixiJS BitmapFont.install) */

/* Rasterises `chars` of style->font (or its family) with the style's fill/gradient, stroke and
 * drop shadow into pages, registers the result as family `name` and returns it. The engine
 * owns it (br_bitmap_font_uninstall or shutdown frees it). An older font installed under the
 * same name is freed (text using it by family switches to the new one). NULL on failure. */
br_font *br_bitmap_font_install(br_engine *e, const char *name, const br_text_style *style,
                                const br_bitmap_font_options *opts);
/* Frees an installed font. Text nodes using it fall back to the default font (br_font.h). */
void br_bitmap_font_uninstall(br_engine *e, const char *name);

/* ---- queries */

/* Copies the glyph of a codepoint. False if the font has none (or is not a bitmap font). */
bool br_bitmap_font_get_char(br_font *font, uint32_t codepoint, br_bitmap_char *out);
/* Number of glyphs. */
uint32_t br_bitmap_font_char_count(br_font *font);
/* Number of pages. */
uint32_t br_bitmap_font_page_count(br_font *font);
/* Page file name as written in the .fnt ("" for installed fonts). */
const char *br_bitmap_font_page_file(br_font *font, uint32_t page);
/* Page texture (zero handle when pages were not loaded). */
br_texture br_bitmap_font_page_texture(br_font *font, uint32_t page);
/* AngelCode "lineHeight" and "base" in native pixels. */
void br_bitmap_font_line_metrics(br_font *font, float *line_height, float *base);

/* ---- the bitmap text node */

/* Type id of bitmap text nodes ("bitmap_text"). */
uint32_t br_bitmap_text_type(br_engine *e);
/* Creates detached bitmap text. size <= 0 = the font's native size. Use the br_text_* functions on it. */
br_node br_bitmap_text_create(br_engine *e, const char *utf8, br_font *font, float size);

#endif
