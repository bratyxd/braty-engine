/* br_text_internal.h - private state of the text module (fonts, atlases, layout, text nodes). */
#ifndef BR_TEXT_INTERNAL_H
#define BR_TEXT_INTERNAL_H

#include "app/br_engine_internal.h"
#include "scene/br_scene_internal.h"
#include "braty/br_assets.h"
#include "braty/br_bitmap_font.h"
#include "braty/br_font.h"
#include "braty/br_rich_text.h"
#include "braty/br_text.h"

#include "stb/stb_rect_pack.h"
#include "stb/stb_truetype.h"

#define BR_FONT_MAX_PAGES 16
#define BR_FONT_PAGE_FILE_SIZE 128
#define BR_ATLAS_START_SIZE 256
#define BR_ATLAS_MAX_SIZE 4096
#define BR_RICH_TAG_NAME_SIZE 32
/* Limits on numbers from styles (scene JSON included), checked inside the module. */
#define BR_TEXT_MAX_FONT_SIZE 4096.0f   /* font_size, rich <size=>; also stroke_width and shadow_blur */
#define BR_TEXT_MAX_LENGTH 1.0e6f       /* spacing, leading, padding, wrap width, shadow distance */
#define BR_TEXT_MAX_RASTER 1024         /* em size coverage glyphs rasterise at; bigger text is scaled */
#define BR_TEXT_MAX_EFFECT_PX 64.0f     /* stroke / shadow growth and blur in raster pixels */

/* What a cached TTF glyph image holds. */
typedef enum br_glyph_effect {
    BR_GLYPH_FILL,     /* antialiased coverage */
    BR_GLYPH_OUTLINE,  /* coverage grown by p1/8 pixels (strokes, fake bold) */
    BR_GLYPH_SOFT,     /* grown by p1/8 and softened over p2/8 pixels (drop shadows) */
    BR_GLYPH_SDF       /* signed distance field (SDF fonts) */
} br_glyph_effect;

/* One glyph image in a TTF font's atlas. */
typedef struct br_atlas_glyph {
    uint64_t key;
    int      x, y, w, h;   /* atlas rect; w == 0 means nothing to draw (space) */
    float    left, top;    /* raster pixels from the pen (on the baseline) to the image's top-left */
} br_atlas_glyph;

typedef struct br_bitmap_kerning {
    uint32_t first;
    uint32_t second;
    float    amount;
} br_bitmap_kerning;

struct br_font {
    bool is_bitmap;
    bool asset_owned;      /* freed by the assets module's unload */
    bool installed;        /* freed by br_bitmap_font_uninstall / shutdown */

    /* ---- TTF */
    const unsigned char *ttf;
    bool                 owns_ttf;
    stbtt_fontinfo       info;
    int                  ascent, descent, line_gap;   /* font units */
    br_font_options      opts;
    /* atlas: 8-bit coverage/distance on the CPU, uploaded as premultiplied white RGBA8 */
    uint8_t      *atlas;
    uint32_t      atlas_w, atlas_h;
    stbrp_context packer;
    stbrp_node   *pack_nodes;
    br_texture    atlas_texture;
    bool          atlas_full_logged;
    bool          too_big_logged;  /* a glyph larger than the maximum atlas was refused */
    bool          dirty;
    uint32_t      dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    br_array      glyphs;      /* br_atlas_glyph */
    uint32_t     *glyph_table; /* open addressing: glyph index + 1, 0 = empty */
    uint32_t      glyph_table_size;

    /* ---- bitmap */
    float      native_size;
    float      line_height, base;     /* native units */
    float      texel_scale;           /* native units per page pixel */
    br_array   chars;                 /* br_bitmap_char sorted by id */
    br_array   kernings;              /* br_bitmap_kerning sorted by (first, second) */
    uint32_t   page_count;
    br_texture pages[BR_FONT_MAX_PAGES];
    char       page_files[BR_FONT_MAX_PAGES][BR_FONT_PAGE_FILE_SIZE];
    bool       owns_pages;
    int        distance_field;        /* 0 none, 1 sdf, 2 msdf */
    float      distance_range;        /* page pixels */
    bool       colored;               /* baked colours: draw white instead of the fill colour */
};

typedef struct br_font_family {
    char     name[BR_TEXT_FAMILY_SIZE];
    br_font *faces[4];                /* [bold + 2 * italic] */
} br_font_family;

typedef struct br_installed_font {
    char     name[BR_TEXT_FAMILY_SIZE];
    br_font *font;
} br_installed_font;

/* Resolved style of a run of characters. */
typedef struct br_text_span {
    br_font *font;
    float    size;
    br_color color;
    bool     use_gradient;
    bool     bold, italic, underline, strike;
    bool     synth_bold, synth_italic;
} br_text_span;

/* Layout input: one decoded character. */
typedef struct br_layout_char {
    uint32_t cp;
    uint32_t src;        /* byte offset in the source text */
    uint16_t span;
    uint8_t  src_len;
} br_layout_char;

/* Layout output: one placed character (spaces included). */
typedef struct br_layout_glyph {
    uint32_t cp;
    uint32_t src;
    uint16_t span;
    uint16_t line;
    uint8_t  src_len;
    float    x;          /* pen position, after alignment */
    float    advance;    /* without letter spacing */
} br_layout_glyph;

typedef struct br_layout_line {
    uint32_t first;      /* first glyph */
    uint32_t count;      /* glyphs on the line (trailing spaces included) */
    uint32_t visible;    /* glyphs up to the last non-space one */
    bool     wrapped;    /* ended by word wrap (justify stretches it), not by a newline or the end */
    float    x, top, width, height, baseline;
} br_layout_line;

typedef struct br_text_layout {
    br_array glyphs;     /* br_layout_glyph */
    br_array lines;      /* br_layout_line */
    float    width, height;
} br_text_layout;

typedef struct br_rich_tag_entry {
    char              name[BR_RICH_TAG_NAME_SIZE];
    br_rich_tag_style style;
} br_rich_tag_entry;

/* Per-node data shared by the "text", "bitmap_text" and "rich_text" node types. */
typedef struct br_text_node {
    char              *text;
    br_text_style      style;
    br_vec2            anchor;
    float              resolution;   /* 0 = window pixel density */
    bool               rich;
    bool               dirty;
    br_rich_tag_entry *tags;
    uint32_t           tag_count;
    char              *plain;        /* rich: text without tags */
    br_text_layout     layout;
    br_array           spans;        /* br_text_span */
} br_text_node;

typedef struct br_text_state {
    uint32_t text_type;
    uint32_t bitmap_text_type;
    uint32_t rich_text_type;
    br_font *default_font;
    br_array fonts;        /* br_font *: every live TTF font (atlas flush) */
    br_array families;     /* br_font_family */
    br_array installed;    /* br_installed_font */
    br_array retired;      /* br_texture: old atlas textures, destroyed next frame */
    br_array vertices;     /* br_vertex2d scratch for SDF draws */
    br_array indices;      /* uint32_t scratch */
    bool     effect_clamp_logged;   /* a stroke / shadow was clamped to BR_TEXT_MAX_EFFECT_PX */
} br_text_state;

/* ---- br_font.c */
void br_font_module_init(br_engine *e);
void br_font_module_shutdown(br_engine *e);
/* Frees any font (assets unload, uninstall, shutdown). Safe after the text state is gone. */
void br_font_free(br_engine *e, br_font *font);
/* Font-unit scale for an em size in pixels. */
float br_font_ttf_scale(br_font *font, float size);
/* Finds or rasterises a glyph image. A glyph that does not fit (atlas at max size, image too
 * large) is cached as empty (w == 0) so it is not rasterised again. NULL only without memory. */
const br_atlas_glyph *br_font_atlas_glyph(br_engine *e, br_font *font, uint32_t codepoint, int raster_size,
                                          br_glyph_effect effect, int p1, int p2);
/* Renders a glyph image into new memory (free with br_free); NULL with *w == 0 for empty glyphs. */
uint8_t *br_font_render_glyph(br_font *font, uint32_t codepoint, int raster_size, br_glyph_effect effect, int p1,
                              int p2, int *w, int *h, float *left, float *top);
/* Bitmap glyph lookup (NULL if missing). */
const br_bitmap_char *br_font_bitmap_char(br_font *font, uint32_t codepoint);
/* Uploads atlas changes now (RENDER system and before an atlas texture is replaced). */
void br_font_flush(br_engine *e, br_font *font);
/* Adds a font to the live list (TTF) so its atlas is flushed. */
void br_font_track(br_engine *e, br_font *font);

/* ---- br_bitmap_font.c */
void br_bitmap_font_module_init(br_engine *e);

/* ---- br_text_layout.c */
void br_text_layout_init(br_text_layout *l);
void br_text_layout_free(br_text_layout *l);
/* Decodes text into layout chars (all span 0), applying the style's white-space mode. */
void br_text_decode(const char *text, br_text_white_space mode, br_array *out_chars);
/* Applies white-space collapsing to already decoded chars (in place). */
void br_text_collapse_white_space(br_array *chars, br_text_white_space mode);
/* Lays out chars (br_layout_char) with their spans and the paragraph style. */
void br_text_layout_build(br_text_layout *out, const br_array *chars, const br_text_span *spans,
                          const br_text_style *style);
/* The plain span of a style (font resolved through families, synth flags set). */
br_text_span br_text_span_from_style(br_engine *e, const br_text_style *style);
/* Y shift that puts the style's baseline mode at local y = 0. */
float br_text_baseline_shift(const br_text_layout *l, const br_text_style *style);

/* ---- br_rich_text.c */
/* Parses markup into node->plain, chars (br_layout_char) and node->spans. */
void br_rich_text_parse(br_engine *e, br_text_node *node, br_array *chars);
/* Frees rich-only data of a node. */
void br_rich_text_free(br_text_node *node);

/* ---- br_text.c */
/* Makes every number of a style finite and in range (where styles enter the module). */
void br_text_style_sanitize(br_text_style *style);
/* Clamps a stroke / shadow growth or blur in raster pixels to BR_TEXT_MAX_EFFECT_PX (logged once). */
float br_text_effect_clamp(br_engine *e, float raster_px);
/* Raster pixel size of coverage glyphs for a drawn size (1..BR_TEXT_MAX_RASTER, NaN-safe). */
int br_text_raster_size(float pixels);
/* The text-node data of n for any of the three types, NULL otherwise. */
br_text_node *br_text_node_get(br_engine *e, br_node n);
/* Called before a font is freed: text nodes holding it in their style drop it (the family or
 * the default font takes over) and nodes whose layout used it lay out again. */
void br_text_forget_font(br_engine *e, const br_font *font);
/* Rebuilds the cached layout if text or style changed. */
void br_text_node_update(br_engine *e, br_text_node *node);
/* Colour of a gradient at a point given as a fraction of the text box. */
br_color br_text_gradient_at(const br_text_gradient *g, float nx, float ny);
/* Shared create for the three types. */
br_node br_text_node_create(br_engine *e, uint32_t type, const char *text, const br_text_style *style, bool rich);

#endif
