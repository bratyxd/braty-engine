/* br_font.h - fonts: TTF/OTF through stb_truetype, AngelCode bitmap fonts, font families.
 *
 * One br_font type covers both kinds. A TTF font rasterises glyphs on demand into a growing
 * atlas (one cache entry per glyph, pixel size and effect), so any size works. With
 * br_font_options.sdf the TTF path stores signed-distance glyphs at one base size instead and
 * text drawn with it stays sharp at any scale (3D text, zooming). Bitmap fonts (br_bitmap_font.h)
 * have fixed pages and are scaled.
 *
 * Text styles pick a font either directly (br_text_style.font) or by family name plus bold /
 * italic (br_font_add_family). A missing bold or italic face is synthesised (outline / shear).
 * The family "default" is the embedded Noto Sans (SIL OFL, engine/assets/fonts/OFL.txt).
 *
 * Asset loaders: "ttf", "otf" -> br_font (coverage glyphs); "fnt", "xml" -> bitmap br_font.
 *   br_font *f = br_assets_load(e, "fonts/Title.ttf");
 */
#ifndef BR_FONT_H
#define BR_FONT_H

#include "braty/br_render.h"

typedef struct br_font br_font;

/* How a TTF font stores its glyphs. Zero = coverage glyphs rasterised per pixel size. */
typedef struct br_font_options {
    bool  sdf;          /* signed-distance glyphs drawn with the SDF shader (sharp at any scale) */
    float sdf_size;     /* base pixel size the distance field is generated at (0 = 64) */
    int   sdf_padding;  /* distance range in base pixels; caps stroke/shadow width (0 = 12) */
} br_font_options;

/* Vertical metrics of a font at a pixel size (y down, ascent > 0, descent < 0). */
typedef struct br_font_metrics {
    float ascent;
    float descent;
    float line_gap;
    float line_height;  /* ascent - descent: the natural distance between baselines */
} br_font_metrics;

/* ---- creating fonts */

/* Creates a TTF/OTF font from file bytes (copied). opts NULL = coverage glyphs. NULL on failure. */
br_font *br_font_create(br_engine *e, const void *ttf, size_t size, const br_font_options *opts);
/* Loads a TTF/OTF file (cached through the assets module). NULL on failure (logged). */
br_font *br_font_load(br_engine *e, const char *path);
/* Destroys a font made with br_font_create (assets free their own fonts on unload). */
void br_font_destroy(br_engine *e, br_font *font);
/* The embedded default font (Noto Sans Regular, coverage glyphs). Never NULL with the text module. */
br_font *br_font_default(br_engine *e);
/* The embedded font file, e.g. to make an SDF copy with br_font_create. */
const void *br_font_default_data(size_t *size);

/* ---- families (font_family + bold/italic in br_text_style) */

/* Registers `font` as the face of `family` with the given weight/style (replaces an old one). */
void br_font_add_family(br_engine *e, const char *family, br_font *font, bool bold, bool italic);
/* Finds the face of a family; falls back to the family's regular face, then to the default font.
 * *synth_bold / *synth_italic (may be NULL) say whether the caller must fake bold/italic. */
br_font *br_font_find(br_engine *e, const char *family, bool bold, bool italic, bool *synth_bold,
                      bool *synth_italic);

/* ---- queries (all sizes in pixels; bitmap fonts scale from their native size) */

/* Vertical metrics at `size`. */
br_font_metrics br_font_get_metrics(br_font *font, float size);
/* Horizontal advance of a codepoint at `size`. */
float br_font_advance(br_font *font, uint32_t codepoint, float size);
/* Kerning adjustment between two codepoints at `size` (usually <= 0). */
float br_font_kerning(br_font *font, uint32_t first, uint32_t second, float size);
/* True if the font has a glyph for the codepoint. */
bool br_font_has_glyph(br_font *font, uint32_t codepoint);
/* True for bitmap (AngelCode / installed) fonts. */
bool br_font_is_bitmap(br_font *font);
/* True if glyphs are distance fields (SDF TTF fonts, SDF/MSDF bitmap fonts). */
bool br_font_is_sdf(br_font *font);
/* Native size of a bitmap font (pixels), 0 for TTF fonts. */
float br_font_native_size(br_font *font);
/* Current atlas size of a TTF font in pixels (grows as glyphs are added). */
void br_font_atlas_size(br_font *font, uint32_t *width, uint32_t *height);
/* Number of cached glyph images (all sizes and effects) of a TTF font. */
uint32_t br_font_cached_glyph_count(br_font *font);

#endif
