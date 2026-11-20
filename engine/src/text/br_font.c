/* br_font: TTF fonts (stb_truetype), the growing glyph atlas (stb_rect_pack), families, loaders. */
#include "text/br_text_internal.h"

extern const unsigned char br_font_default_ttf[];
extern const size_t br_font_default_ttf_size;

/* ---- helpers */

static float br_font_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float br_font_ttf_scale(br_font *font, float size)
{
    return stbtt_ScaleForMappingEmToPixels(&font->info, size);
}

void br_font_track(br_engine *e, br_font *font)
{
    if (e->text != NULL) {
        br_array_push(&e->text->fonts, &font);
    }
}

static void br_font_untrack(br_engine *e, br_font *font)
{
    br_text_state *s = e->text;
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->fonts.count; i++) {
        br_font **f = br_array_at(&s->fonts, i);
        if (*f == font) {
            br_array_remove_ordered(&s->fonts, i);
            break;
        }
    }
    for (uint32_t i = 0; i < s->families.count; i++) {
        br_font_family *fam = br_array_at(&s->families, i);
        for (int k = 0; k < 4; k++) {
            if (fam->faces[k] == font) {
                fam->faces[k] = NULL;
            }
        }
    }
    for (uint32_t i = 0; i < s->installed.count; i++) {
        br_installed_font *inst = br_array_at(&s->installed, i);
        if (inst->font == font) {
            br_array_remove_ordered(&s->installed, i);
            break;
        }
    }
    if (s->default_font == font) {
        s->default_font = NULL;
    }
}

/* ---- validation (stb_truetype trusts every offset in the file) */

static uint32_t br_font_u16(const unsigned char *p)
{
    return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

static uint32_t br_font_u32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* A table of the font at fontstart: false if missing (or, in which case it logs, if it is
 * shorter than min_len). The directory was checked against the file size already. */
static bool br_font_table(const unsigned char *d, uint32_t fontstart, const char *tag, uint32_t min_len,
                          uint32_t *off, uint32_t *len)
{
    uint32_t n = br_font_u16(d + fontstart + 4);
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *rec = d + fontstart + 12 + 16 * i;
        if (SDL_memcmp(rec, tag, 4) == 0) {
            *off = br_font_u32(rec + 8);
            *len = br_font_u32(rec + 12);
            if (*len < min_len) {
                br_log_error("br_font_create: table '%s' is too short", tag);
                return false;
            }
            return true;
        }
    }
    *off = 0;
    *len = 0;
    return false;
}

/* The cmap subtable formats stb_truetype reads: every read it can make stays inside `len`. */
static bool br_font_check_cmap_subtable(const unsigned char *s, uint64_t len)
{
    if (len < 4) {
        return false;
    }
    uint32_t format = br_font_u16(s);
    if (format == 0) {
        return br_font_u16(s + 2) <= len && len >= 6 + 256;
    }
    if (format == 6) {
        return len >= 10 && br_font_u16(s + 2) <= len && 10 + 2 * (uint64_t)br_font_u16(s + 8) <= len;
    }
    if (format == 12 || format == 13) {
        return len >= 16 && br_font_u32(s + 4) <= len && 16 + 12 * (uint64_t)br_font_u32(s + 12) <= len;
    }
    if (format != 4) {
        return true; /* stb ignores other formats */
    }
    if (len < 14 || br_font_u16(s + 2) > len) {
        return false;
    }
    uint64_t sub_len = br_font_u16(s + 2);
    uint32_t segcount = br_font_u16(s + 6) / 2;
    uint32_t range = br_font_u16(s + 8) / 2;
    uint32_t selector = br_font_u16(s + 10);
    uint32_t shift = br_font_u16(s + 12) / 2;
    /* the binary search (stbtt_FindGlyphIndex) can reach segment rangeShift + the halves */
    uint64_t item_max = shift;
    for (uint32_t k = 0; k < selector && k < 16; k++) {
        range /= 2;
        item_max += range;
    }
    if (selector > 16 || segcount == 0 || 16 + 6 * (uint64_t)segcount + 2 * item_max + 2 > sub_len) {
        return false;
    }
    /* glyph id array reads: idRangeOffset[i] + 2 * (code - start) from that entry */
    for (uint64_t i = 0; i <= item_max; i++) {
        uint64_t end = br_font_u16(s + 14 + 2 * i);
        uint64_t start = br_font_u16(s + 16 + 2 * segcount + 2 * i);
        uint64_t at = 16 + 6 * (uint64_t)segcount + 2 * i;
        uint64_t ro = br_font_u16(s + at);
        if (ro != 0 && end >= start && at + ro + 2 * (end - start) + 2 > sub_len) {
            return false;
        }
    }
    return true;
}

/* Checks what stb_truetype reads without bounds checks: the table directory, the fixed-size
 * tables, the cmap subtables it picks, loca, hmtx and kern. Glyph outlines, GPOS and CFF data
 * are trusted (br_font.h). */
static bool br_font_check(const unsigned char *d, size_t size, int offset)
{
    if (offset < 0 || (uint64_t)offset + 12 > size) {
        return false;
    }
    uint32_t start = (uint32_t)offset;
    uint32_t tables = br_font_u16(d + start + 4);
    if ((uint64_t)start + 12 + 16 * (uint64_t)tables > size) {
        br_log_error("br_font_create: the table directory is cut off");
        return false;
    }
    for (uint32_t i = 0; i < tables; i++) {
        const unsigned char *rec = d + start + 12 + 16 * i;
        if ((uint64_t)br_font_u32(rec + 8) + br_font_u32(rec + 12) > size) {
            br_log_error("br_font_create: table '%.4s' lies past the end of the file (truncated?)", (const char *)rec);
            return false;
        }
    }
    uint32_t cmap, cmap_len, head, head_len, maxp, maxp_len, hhea, hhea_len, hmtx, hmtx_len;
    if (!br_font_table(d, start, "cmap", 4, &cmap, &cmap_len) || !br_font_table(d, start, "head", 54, &head, &head_len) ||
        !br_font_table(d, start, "maxp", 6, &maxp, &maxp_len) || !br_font_table(d, start, "hhea", 36, &hhea, &hhea_len) ||
        !br_font_table(d, start, "hmtx", 0, &hmtx, &hmtx_len)) {
        return false;
    }
    uint32_t glyphs = br_font_u16(d + maxp + 4);
    uint32_t long_metrics = br_font_u16(d + hhea + 34);
    if (long_metrics == 0 || 4 * (uint64_t)long_metrics > hmtx_len) {
        br_log_error("br_font_create: bad horizontal metrics");
        return false;
    }
    uint32_t encodings = br_font_u16(d + cmap + 2);
    if (4 + 8 * (uint64_t)encodings > cmap_len) {
        return false;
    }
    for (uint32_t i = 0; i < encodings; i++) {
        const unsigned char *rec = d + cmap + 4 + 8 * i;
        uint32_t platform = br_font_u16(rec);
        uint32_t encoding = br_font_u16(rec + 2);
        if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) {
            continue; /* stb only reads Unicode subtables */
        }
        uint64_t sub = br_font_u32(rec + 4);
        if (sub >= cmap_len || !br_font_check_cmap_subtable(d + cmap + sub, cmap_len - sub)) {
            br_log_error("br_font_create: bad cmap subtable");
            return false;
        }
    }
    uint32_t glyf, glyf_len, loca, loca_len;
    if (br_font_table(d, start, "glyf", 0, &glyf, &glyf_len)) {
        bool long_loca = br_font_u16(d + head + 50) != 0;
        uint64_t entry = long_loca ? 4 : 2;
        if (!br_font_table(d, start, "loca", 0, &loca, &loca_len) || ((uint64_t)glyphs + 1) * entry > loca_len) {
            br_log_error("br_font_create: loca is shorter than the glyph count");
            return false;
        }
        uint64_t prev = 0;
        for (uint32_t g = 0; g <= glyphs; g++) {
            const unsigned char *p = d + loca + g * entry;
            uint64_t at = long_loca ? br_font_u32(p) : 2 * (uint64_t)br_font_u16(p);
            /* a non-empty glyph needs its 10-byte header inside glyf */
            if (at > glyf_len || at < prev || (g > 0 && at > prev && at - prev < 10)) {
                br_log_error("br_font_create: bad glyph offsets (loca)");
                return false;
            }
            prev = at;
        }
    }
    uint32_t kern, kern_len;
    if (br_font_table(d, start, "kern", 0, &kern, &kern_len)) {
        if (kern_len < 4 || (br_font_u16(d + kern + 2) >= 1 && kern_len < 18) ||
            (kern_len >= 18 && 18 + 6 * (uint64_t)br_font_u16(d + kern + 10) > kern_len &&
             br_font_u16(d + kern + 8) == 1)) {
            br_log_error("br_font_create: bad kern table");
            return false;
        }
    }
    return true;
}

/* ---- creation */

static br_font *br_font_create_ttf(br_engine *e, const unsigned char *data, size_t size, bool copy,
                                   const br_font_options *opts)
{
    if (data == NULL || size < 12) {
        br_log_error("br_font_create: no font data");
        return NULL;
    }
    /* a collection header needs 16 bytes before stb reads the first font's offset */
    if (SDL_memcmp(data, "ttcf", 4) == 0 && size < 16) {
        br_log_error("br_font_create: truncated font collection");
        return NULL;
    }
    if (!br_font_check(data, size, stbtt_GetFontOffsetForIndex(data, 0))) {
        br_log_error("br_font_create: not a valid TrueType/OpenType font (%u bytes)", (unsigned)size);
        return NULL;
    }
    br_font *f = br_alloc(sizeof(br_font));
    if (f == NULL) {
        return NULL;
    }
    if (copy) {
        unsigned char *bytes = br_alloc(size);
        if (bytes == NULL) {
            br_free(f);
            return NULL;
        }
        SDL_memcpy(bytes, data, size);
        f->ttf = bytes;
        f->owns_ttf = true;
    } else {
        f->ttf = data;
    }
    int offset = stbtt_GetFontOffsetForIndex(f->ttf, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->ttf, offset)) {
        br_log_error("br_font_create: not a TrueType/OpenType font");
        if (f->owns_ttf) {
            br_free((void *)f->ttf);
        }
        br_free(f);
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    if (opts != NULL) {
        f->opts = *opts;
    }
    if (!(f->opts.sdf_size > 0.0f)) { /* NaN too */
        f->opts.sdf_size = 64.0f;
    }
    if (f->opts.sdf_size > 256.0f) { /* one distance field per glyph: keep it atlas-sized */
        f->opts.sdf_size = 256.0f;
    }
    if (f->opts.sdf_padding <= 0) {
        f->opts.sdf_padding = 12;
    }
    if (f->opts.sdf_padding > 64) {
        f->opts.sdf_padding = 64;
    }
    br_array_init(&f->glyphs, sizeof(br_atlas_glyph));
    br_array_init(&f->chars, sizeof(br_bitmap_char));
    br_array_init(&f->kernings, sizeof(br_bitmap_kerning));
    br_font_track(e, f);
    return f;
}

br_font *br_font_create(br_engine *e, const void *ttf, size_t size, const br_font_options *opts)
{
    return br_font_create_ttf(e, ttf, size, true, opts);
}

const void *br_font_default_data(size_t *size)
{
    if (size != NULL) {
        *size = br_font_default_ttf_size;
    }
    return br_font_default_ttf;
}

br_font *br_font_default(br_engine *e)
{
    return e->text != NULL ? e->text->default_font : NULL;
}

void br_font_free(br_engine *e, br_font *font)
{
    if (font == NULL) {
        return;
    }
    br_text_forget_font(e, font);
    br_font_untrack(e, font);
    if (e->render != NULL) {
        if (font->atlas_texture.generation != 0) {
            br_texture_destroy(e, font->atlas_texture);
        }
        if (font->owns_pages) {
            for (uint32_t i = 0; i < font->page_count; i++) {
                br_texture_destroy(e, font->pages[i]);
            }
        }
    }
    for (uint32_t i = 0; i < font->page_count && e->assets != NULL; i++) {
        if (font->page_paths[i][0] != 0) {
            br_assets_unload(e, font->page_paths[i]); /* the page references of a loaded .fnt */
        }
    }
    if (font->owns_ttf) {
        br_free((void *)font->ttf);
    }
    br_free(font->atlas);
    br_free(font->pack_nodes);
    br_free(font->glyph_table);
    br_array_free(&font->glyphs);
    br_array_free(&font->chars);
    br_array_free(&font->kernings);
    br_free(font);
}

void br_font_destroy(br_engine *e, br_font *font)
{
    if (font == NULL) {
        return;
    }
    if (font->asset_owned || font->installed) {
        br_log_error("br_font_destroy: font belongs to the assets module or br_bitmap_font_install");
        return;
    }
    br_font_free(e, font);
}

/* ---- glyph images */

uint8_t *br_font_render_glyph(br_font *font, uint32_t codepoint, int raster_size, br_glyph_effect effect, int p1,
                              int p2, int *w, int *h, float *left, float *top)
{
    *w = 0;
    *h = 0;
    *left = 0.0f;
    *top = 0.0f;
    int glyph = stbtt_FindGlyphIndex(&font->info, (int)codepoint);
    float scale = br_font_ttf_scale(font, (float)raster_size);
    if (effect == BR_GLYPH_FILL) {
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&font->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
        if (x1 <= x0 || y1 <= y0) {
            return NULL;
        }
        uint8_t *buf = br_alloc((size_t)(x1 - x0) * (size_t)(y1 - y0));
        if (buf == NULL) {
            return NULL;
        }
        stbtt_MakeGlyphBitmap(&font->info, buf, x1 - x0, y1 - y0, x1 - x0, scale, scale, glyph);
        *w = x1 - x0;
        *h = y1 - y0;
        *left = (float)x0;
        *top = (float)y0;
        return buf;
    }
    float radius = (float)p1 / 8.0f;
    float blur = (float)p2 / 8.0f;
    int pad = effect == BR_GLYPH_SDF ? font->opts.sdf_padding : (int)SDL_ceilf(radius + blur) + 2;
    float dist_scale = 127.0f / (float)pad; /* the field covers +-pad pixels in 8 bits */
    int sw, sh, xoff, yoff;
    unsigned char *sdf = stbtt_GetGlyphSDF(&font->info, scale, glyph, pad, 128, dist_scale, &sw, &sh, &xoff, &yoff);
    if (sdf == NULL) {
        return NULL;
    }
    uint8_t *buf = br_alloc((size_t)sw * (size_t)sh);
    if (buf == NULL) {
        stbtt_FreeSDF(sdf, NULL);
        return NULL;
    }
    for (int i = 0; i < sw * sh; i++) {
        if (effect == BR_GLYPH_SDF) {
            buf[i] = sdf[i];
            continue;
        }
        float dist = ((float)sdf[i] - 128.0f) / dist_scale; /* pixels, positive inside */
        float a;
        if (effect == BR_GLYPH_OUTLINE) {
            a = dist + radius + 0.5f;
        } else {
            a = (dist + radius) / (blur > 1.0f ? blur : 1.0f) + 0.5f;
        }
        buf[i] = (uint8_t)(br_font_clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    stbtt_FreeSDF(sdf, NULL);
    *w = sw;
    *h = sh;
    *left = (float)xoff;
    *top = (float)yoff;
    return buf;
}

/* ---- atlas */

static uint32_t br_font_hash(uint64_t key, uint32_t table_size)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) & (table_size - 1);
}

static void br_font_table_insert(br_font *f, uint64_t key, uint32_t index)
{
    uint32_t slot = br_font_hash(key, f->glyph_table_size);
    while (f->glyph_table[slot] != 0) {
        slot = (slot + 1) & (f->glyph_table_size - 1);
    }
    f->glyph_table[slot] = index + 1;
}

static bool br_font_table_grow(br_font *f)
{
    uint32_t size = f->glyph_table_size == 0 ? 256 : f->glyph_table_size * 2;
    uint32_t *table = br_alloc(sizeof(uint32_t) * size);
    if (table == NULL) {
        return false;
    }
    br_free(f->glyph_table);
    f->glyph_table = table;
    f->glyph_table_size = size;
    for (uint32_t i = 0; i < f->glyphs.count; i++) {
        br_atlas_glyph *g = br_array_at(&f->glyphs, i);
        br_font_table_insert(f, g->key, i);
    }
    return true;
}

static void br_font_mark_dirty(br_font *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!f->dirty) {
        f->dirty = true;
        f->dirty_x0 = x0;
        f->dirty_y0 = y0;
        f->dirty_x1 = x1;
        f->dirty_y1 = y1;
        return;
    }
    f->dirty_x0 = x0 < f->dirty_x0 ? x0 : f->dirty_x0;
    f->dirty_y0 = y0 < f->dirty_y0 ? y0 : f->dirty_y0;
    f->dirty_x1 = x1 > f->dirty_x1 ? x1 : f->dirty_x1;
    f->dirty_y1 = y1 > f->dirty_y1 ? y1 : f->dirty_y1;
}

void br_font_flush(br_engine *e, br_font *f)
{
    if (!f->dirty) {
        return;
    }
    br_gpu *g = br_engine_gpu(e);
    br_gpu_texture *gpu = g != NULL ? br_texture_gpu(e, f->atlas_texture) : NULL;
    if (gpu == NULL) {
        f->dirty = false; /* headless: nothing to upload */
        return;
    }
    uint32_t w = f->dirty_x1 - f->dirty_x0;
    uint32_t h = f->dirty_y1 - f->dirty_y0;
    uint8_t *rgba = br_alloc((size_t)w * h * 4);
    if (rgba == NULL) {
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = &f->atlas[(size_t)(f->dirty_y0 + y) * f->atlas_w + f->dirty_x0];
        uint8_t *dst = &rgba[(size_t)y * w * 4];
        for (uint32_t x = 0; x < w; x++) {
            /* premultiplied white: coverage (or distance) in every channel */
            dst[x * 4 + 0] = src[x];
            dst[x * 4 + 1] = src[x];
            dst[x * 4 + 2] = src[x];
            dst[x * 4 + 3] = src[x];
        }
    }
    br_gpu_texture_upload(g, gpu, f->dirty_x0, f->dirty_y0, w, h, rgba);
    br_free(rgba);
    f->dirty = false;
}

/* (Re)creates the atlas at least w x h, repacking and copying every existing glyph. */
static bool br_font_atlas_resize(br_engine *e, br_font *f, uint32_t w, uint32_t h)
{
    stbrp_rect *rects = br_alloc(sizeof(stbrp_rect) * (f->glyphs.count + 1));
    if (rects == NULL) {
        return false;
    }
    int count = 0;
    for (uint32_t i = 0; i < f->glyphs.count; i++) {
        br_atlas_glyph *g = br_array_at(&f->glyphs, i);
        if (g->w > 0) {
            rects[count].id = (int)i;
            rects[count].w = g->w + 1; /* 1 px gap so linear filtering never bleeds */
            rects[count].h = g->h + 1;
            count++;
        }
    }
    /* the packer lives in the font (stbrp_context points into itself, so it must not be copied) */
    br_free(f->pack_nodes);
    for (;;) {
        f->pack_nodes = br_alloc(sizeof(stbrp_node) * w);
        if (f->pack_nodes == NULL) {
            br_free(rects);
            return false;
        }
        stbrp_init_target(&f->packer, (int)w, (int)h, f->pack_nodes, (int)w);
        if (count == 0 || stbrp_pack_rects(&f->packer, rects, count)) {
            break;
        }
        br_free(f->pack_nodes);
        f->pack_nodes = NULL;
        if (w >= BR_ATLAS_MAX_SIZE && h >= BR_ATLAS_MAX_SIZE) {
            br_free(rects);
            return false;
        }
        if (w <= h) {
            w *= 2;
        } else {
            h *= 2;
        }
    }
    uint8_t *pixels = br_alloc((size_t)w * h);
    if (pixels == NULL) {
        br_free(rects);
        return false;
    }
    for (int i = 0; i < count; i++) {
        br_atlas_glyph *g = br_array_at(&f->glyphs, (uint32_t)rects[i].id);
        for (int y = 0; y < g->h; y++) {
            SDL_memcpy(&pixels[(size_t)(rects[i].y + y) * w + (size_t)rects[i].x],
                       &f->atlas[(size_t)(g->y + y) * f->atlas_w + (size_t)g->x], (size_t)g->w);
        }
        g->x = rects[i].x;
        g->y = rects[i].y;
    }
    br_free(rects);
    /* draws pushed earlier this frame keep using the old texture: upload its pending glyphs,
     * then retire it until the next frame */
    if (f->atlas_texture.generation != 0) {
        br_font_flush(e, f);
        br_array_push(&e->text->retired, &f->atlas_texture);
    }
    br_free(f->atlas);
    f->atlas = pixels;
    f->atlas_w = w;
    f->atlas_h = h;
    f->atlas_texture = br_texture_create_rgba8(e, w, h, NULL, NULL);
    e->text->atlas_generation += 1; /* every text node's cached quads may point at the old atlas */
    f->dirty = false;
    br_font_mark_dirty(f, 0, 0, w, h);
    return true;
}

static bool br_font_atlas_place(br_engine *e, br_font *f, int w, int h, int *x, int *y)
{
    if (f->atlas == NULL && !br_font_atlas_resize(e, f, BR_ATLAS_START_SIZE, BR_ATLAS_START_SIZE)) {
        return false;
    }
    if (f->pack_nodes == NULL) {
        return false; /* an earlier resize ran out of memory */
    }
    if (w + 1 > BR_ATLAS_MAX_SIZE || h + 1 > BR_ATLAS_MAX_SIZE) {
        /* can never fit: do not grow the atlas to its maximum for nothing */
        if (!f->too_big_logged) {
            br_log_error("font: a glyph image of %dx%d px is too large for the atlas; it is not drawn", w, h);
            f->too_big_logged = true;
        }
        return false;
    }
    for (;;) {
        stbrp_rect r;
        SDL_zero(r);
        r.w = w + 1;
        r.h = h + 1;
        if (stbrp_pack_rects(&f->packer, &r, 1) && r.was_packed) {
            *x = r.x;
            *y = r.y;
            return true;
        }
        uint32_t nw = f->atlas_w;
        uint32_t nh = f->atlas_h;
        if (nw <= nh && nw < BR_ATLAS_MAX_SIZE) {
            nw *= 2;
        } else if (nh < BR_ATLAS_MAX_SIZE) {
            nh *= 2;
        } else {
            /* ponytail: a full 4096x4096 atlas stops caching new glyphs; evicting unused sizes
             * would need per-glyph frame stamps */
            if (!f->atlas_full_logged) {
                br_log_error("font atlas is full (%dx%d); new glyphs are not drawn", BR_ATLAS_MAX_SIZE,
                             BR_ATLAS_MAX_SIZE);
                f->atlas_full_logged = true;
            }
            return false;
        }
        if (!br_font_atlas_resize(e, f, nw, nh)) {
            return false;
        }
    }
}

const br_atlas_glyph *br_font_atlas_glyph(br_engine *e, br_font *font, uint32_t codepoint, int raster_size,
                                          br_glyph_effect effect, int p1, int p2)
{
    if (font->is_bitmap || e->text == NULL) {
        return NULL;
    }
    if (effect == BR_GLYPH_SDF) {
        raster_size = (int)font->opts.sdf_size;
    }
    if (raster_size < 1) {
        raster_size = 1;
    }
    if (raster_size > 4095) {
        raster_size = 4095;
    }
    p1 = p1 < 0 ? 0 : (p1 > 65535 ? 65535 : p1);
    p2 = p2 < 0 ? 0 : (p2 > 65535 ? 65535 : p2);
    uint32_t glyph = (uint32_t)stbtt_FindGlyphIndex(&font->info, (int)codepoint) & 0xFFFFu;
    uint64_t key = (uint64_t)glyph | ((uint64_t)raster_size << 16) | ((uint64_t)effect << 28) |
                   ((uint64_t)p1 << 32) | ((uint64_t)p2 << 48);
    if (font->glyph_table_size != 0) {
        uint32_t slot = br_font_hash(key, font->glyph_table_size);
        while (font->glyph_table[slot] != 0) {
            br_atlas_glyph *g = br_array_at(&font->glyphs, font->glyph_table[slot] - 1);
            if (g->key == key) {
                return g;
            }
            slot = (slot + 1) & (font->glyph_table_size - 1);
        }
    }
    br_atlas_glyph entry;
    SDL_zero(entry);
    entry.key = key;
    uint8_t *image = br_font_render_glyph(font, codepoint, raster_size, effect, p1, p2, &entry.w, &entry.h,
                                          &entry.left, &entry.top);
    if (image != NULL && !br_font_atlas_place(e, font, entry.w, entry.h, &entry.x, &entry.y)) {
        /* remembered as an empty glyph so it is not rasterised again every frame */
        br_free(image);
        image = NULL;
        entry.w = 0;
        entry.h = 0;
    }
    if (image != NULL) {
        for (int y = 0; y < entry.h; y++) {
            SDL_memcpy(&font->atlas[(size_t)(entry.y + y) * font->atlas_w + (size_t)entry.x],
                       &image[(size_t)y * (size_t)entry.w], (size_t)entry.w);
        }
        br_free(image);
        br_font_mark_dirty(font, (uint32_t)entry.x, (uint32_t)entry.y, (uint32_t)(entry.x + entry.w),
                           (uint32_t)(entry.y + entry.h));
    }
    if ((font->glyphs.count + 1) * 2 > font->glyph_table_size && !br_font_table_grow(font)) {
        return NULL;
    }
    if (br_array_push(&font->glyphs, &entry) == NULL) {
        return NULL;
    }
    br_font_table_insert(font, key, font->glyphs.count - 1);
    return br_array_at(&font->glyphs, font->glyphs.count - 1);
}

/* ---- bitmap lookups (br_bitmap_font.c fills the tables) */

const br_bitmap_char *br_font_bitmap_char(br_font *font, uint32_t codepoint)
{
    uint32_t lo = 0;
    uint32_t hi = font->chars.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        br_bitmap_char *c = br_array_at(&font->chars, mid);
        if (c->id == codepoint) {
            return c;
        }
        if (c->id < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static float br_font_bitmap_kerning(br_font *font, uint32_t first, uint32_t second)
{
    uint32_t lo = 0;
    uint32_t hi = font->kernings.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        br_bitmap_kerning *k = br_array_at(&font->kernings, mid);
        if (k->first == first && k->second == second) {
            return k->amount;
        }
        if (k->first < first || (k->first == first && k->second < second)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0.0f;
}

/* ---- queries */

static float br_font_bitmap_scale(br_font *font, float size)
{
    return font->native_size > 0.0f && size > 0.0f ? size / font->native_size : 1.0f;
}

bool br_font_get_metrics(br_font *font, float size, br_font_metrics *out)
{
    br_font_metrics m;
    SDL_zero(m);
    if (font == NULL) {
        *out = m;
        return false;
    }
    if (font->is_bitmap) {
        float k = br_font_bitmap_scale(font, size);
        m.ascent = font->base * k;
        m.descent = -(font->line_height - font->base) * k;
        m.line_height = font->line_height * k;
        *out = m;
        return true;
    }
    float scale = br_font_ttf_scale(font, size);
    m.ascent = (float)font->ascent * scale;
    m.descent = (float)font->descent * scale;
    m.line_gap = (float)font->line_gap * scale;
    m.line_height = m.ascent - m.descent;
    *out = m;
    return true;
}

float br_font_advance(br_font *font, uint32_t codepoint, float size)
{
    if (font == NULL) {
        return 0.0f;
    }
    if (font->is_bitmap) {
        const br_bitmap_char *c = br_font_bitmap_char(font, codepoint);
        if (c == NULL) {
            c = br_font_bitmap_char(font, ' ');
        }
        return c != NULL ? c->xadvance * br_font_bitmap_scale(font, size) : 0.0f;
    }
    /* advance only: the left side bearing of a glyph id past the font's glyph count would be
     * read past hmtx (br_font_check bounds only the advances) */
    int advance = 0;
    stbtt_GetGlyphHMetrics(&font->info, stbtt_FindGlyphIndex(&font->info, (int)codepoint), &advance, NULL);
    return (float)advance * br_font_ttf_scale(font, size);
}

float br_font_kerning(br_font *font, uint32_t first, uint32_t second, float size)
{
    if (font == NULL) {
        return 0.0f;
    }
    if (font->is_bitmap) {
        return br_font_bitmap_kerning(font, first, second) * br_font_bitmap_scale(font, size);
    }
    return (float)stbtt_GetCodepointKernAdvance(&font->info, (int)first, (int)second) * br_font_ttf_scale(font, size);
}

bool br_font_has_glyph(br_font *font, uint32_t codepoint)
{
    if (font == NULL) {
        return false;
    }
    if (font->is_bitmap) {
        return br_font_bitmap_char(font, codepoint) != NULL;
    }
    return stbtt_FindGlyphIndex(&font->info, (int)codepoint) != 0;
}

bool br_font_is_bitmap(br_font *font)
{
    return font != NULL && font->is_bitmap;
}

bool br_font_is_sdf(br_font *font)
{
    if (font == NULL) {
        return false;
    }
    return font->is_bitmap ? font->distance_field != 0 : font->opts.sdf;
}

float br_font_native_size(br_font *font)
{
    return font != NULL && font->is_bitmap ? font->native_size : 0.0f;
}

void br_font_atlas_size(br_font *font, uint32_t *width, uint32_t *height)
{
    *width = font != NULL ? font->atlas_w : 0;
    *height = font != NULL ? font->atlas_h : 0;
}

uint32_t br_font_cached_glyph_count(br_font *font)
{
    return font != NULL ? font->glyphs.count : 0;
}

/* ---- families */

static br_font_family *br_font_family_get(br_engine *e, const char *family, bool create)
{
    br_text_state *s = e->text;
    if (s == NULL) {
        return NULL;
    }
    if (family == NULL || family[0] == 0) {
        family = "default";
    }
    for (uint32_t i = 0; i < s->families.count; i++) {
        br_font_family *f = br_array_at(&s->families, i);
        if (SDL_strcmp(f->name, family) == 0) {
            return f;
        }
    }
    if (!create) {
        return NULL;
    }
    br_font_family *f = br_array_push(&s->families, NULL);
    if (f != NULL) {
        br_str_copy(f->name, sizeof(f->name), family);
    }
    return f;
}

void br_font_add_family(br_engine *e, const char *family, br_font *font, bool bold, bool italic)
{
    br_font_family *f = br_font_family_get(e, family, true);
    if (f != NULL) {
        f->faces[(bold ? 1 : 0) + (italic ? 2 : 0)] = font;
    }
}

br_font *br_font_find(br_engine *e, const char *family, bool bold, bool italic, bool *synth_bold, bool *synth_italic)
{
    bool sb = false;
    bool si = false;
    br_font *found = NULL;
    br_font_family *f = br_font_family_get(e, family, false);
    if (f == NULL) {
        if (family != NULL && family[0] != 0) {
            br_log_warn("unknown font family '%s', using the default font", family);
        }
        f = br_font_family_get(e, "default", false);
    }
    if (f != NULL) {
        found = f->faces[(bold ? 1 : 0) + (italic ? 2 : 0)];
        if (found == NULL && bold && italic && f->faces[1] != NULL) {
            found = f->faces[1];
            si = true;
        }
        if (found == NULL && bold && italic && f->faces[2] != NULL) {
            found = f->faces[2];
            sb = true;
        }
        if (found == NULL) {
            found = f->faces[0];
            sb = bold;
            si = italic;
        }
    }
    if (found == NULL) {
        found = br_font_default(e);
        sb = bold;
        si = italic;
    }
    if (synth_bold != NULL) {
        *synth_bold = sb;
    }
    if (synth_italic != NULL) {
        *synth_italic = si;
    }
    return found;
}

/* ---- assets */

static void *br_font_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    br_font *f = br_font_create_ttf(e, bytes, size, true, NULL);
    if (f == NULL) {
        br_log_error("cannot load font '%s'", path);
        return NULL;
    }
    f->asset_owned = true;
    return f;
}

static void br_font_asset_unload(br_engine *e, void *asset)
{
    br_font_free(e, asset);
}

br_font *br_font_load(br_engine *e, const char *path)
{
    br_font *f = br_assets_load(e, path);
    const char *type = br_assets_type(e, path);
    if (f != NULL && type != NULL && SDL_strcmp(type, "font") != 0 && SDL_strcmp(type, "bitmap_font") != 0) {
        br_log_error("br_font_load: '%s' is a %s, not a font", path, type);
        return NULL;
    }
    return f;
}

/* ---- systems */

static void br_font_flush_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    for (uint32_t i = 0; i < e->text->fonts.count; i++) {
        br_font **f = br_array_at(&e->text->fonts, i);
        br_font_flush(e, *f);
    }
}

static void br_font_retire_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    for (uint32_t i = 0; i < e->text->retired.count; i++) {
        br_texture *t = br_array_at(&e->text->retired, i);
        br_texture_destroy(e, *t);
    }
    br_array_clear(&e->text->retired);
}

void br_font_module_init(br_engine *e)
{
    br_text_state *s = e->text;
    br_array_init(&s->fonts, sizeof(br_font *));
    br_array_init(&s->families, sizeof(br_font_family));
    br_array_init(&s->installed, sizeof(br_installed_font));
    br_array_init(&s->retired, sizeof(br_texture));
    s->default_font = br_font_create_ttf(e, br_font_default_ttf, br_font_default_ttf_size, false, NULL);
    br_font_add_family(e, "default", s->default_font, false, false);
    br_font_add_family(e, "Noto Sans", s->default_font, false, false);

    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "font";
    desc.extensions = "ttf,otf";
    desc.load = br_font_asset_load;
    desc.unload = br_font_asset_unload;
    br_assets_register_loader(e, &desc);

    /* before render.submit (RENDER 0): upload new glyphs; before render.collect: drop old atlases */
    br_system_add(e, "text.atlas_flush", BR_PHASE_RENDER, -100, br_font_flush_system, NULL);
    br_system_add(e, "text.atlas_retire", BR_PHASE_PRE_RENDER, -2000, br_font_retire_system, NULL);
}

void br_font_module_shutdown(br_engine *e)
{
    br_text_state *s = e->text;
    br_font_retire_system(e, 0.0f, NULL);
    /* fonts owned by the assets module are freed by its unload, which runs after this */
    uint32_t i = 0;
    while (i < s->fonts.count) {
        br_font **f = br_array_at(&s->fonts, i);
        if ((*f)->asset_owned) {
            i++;
        } else {
            br_font_free(e, *f); /* removes itself from the list */
        }
    }
    br_array_free(&s->fonts);
    br_array_free(&s->families);
    br_array_free(&s->installed);
    br_array_free(&s->retired);
}
