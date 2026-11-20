/* br_bitmap_font: AngelCode .fnt (text and XML) parsing, br_bitmap_font_install, bitmap text. */
#include "text/br_text_internal.h"

/* ---- AngelCode attribute scanning (shared by the text and XML forms) */

static bool br_fnt_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Reads the next key=value pair of [*p, end). False when none is left. */
static bool br_fnt_next_attr(const char **p, const char *end, char *key, size_t key_size, char *value,
                             size_t value_size)
{
    const char *s = *p;
    for (;;) {
        while (s < end && (br_fnt_is_space(*s) || *s == '/')) {
            s++;
        }
        if (s >= end) {
            *p = s;
            return false;
        }
        const char *k = s;
        while (s < end && *s != '=' && !br_fnt_is_space(*s)) {
            s++;
        }
        size_t klen = (size_t)(s - k);
        if (s >= end || *s != '=') {
            continue; /* a bare word: skip it */
        }
        s++;
        const char *v = s;
        const char *v_end;
        if (s < end && *s == '"') {
            v = ++s;
            while (s < end && *s != '"') {
                s++;
            }
            v_end = s;
            if (s < end) {
                s++;
            }
        } else {
            while (s < end && !br_fnt_is_space(*s) && *s != '/') {
                s++;
            }
            v_end = s;
        }
        size_t vlen = (size_t)(v_end - v);
        if (klen >= key_size) {
            klen = key_size - 1;
        }
        if (vlen >= value_size) {
            vlen = value_size - 1;
        }
        SDL_memcpy(key, k, klen);
        key[klen] = 0;
        SDL_memcpy(value, v, vlen);
        value[vlen] = 0;
        *p = s;
        return true;
    }
}

static bool br_fnt_attr(const char *start, const char *end, const char *name, char *out, size_t out_size)
{
    char key[64];
    const char *p = start;
    while (br_fnt_next_attr(&p, end, key, sizeof(key), out, out_size)) {
        if (SDL_strcmp(key, name) == 0) {
            return true;
        }
    }
    out[0] = 0;
    return false;
}

static float br_fnt_float(const char *start, const char *end, const char *name, float fallback)
{
    char value[64];
    return br_fnt_attr(start, end, name, value, sizeof(value)) ? (float)SDL_atof(value) : fallback;
}

/* Handles one element ("info", "common", "page", "char", "kerning", "distanceField"). */
static void br_fnt_element(br_font *f, const char *tag, const char *start, const char *end)
{
    if (SDL_strcmp(tag, "info") == 0) {
        f->native_size = SDL_fabsf(br_fnt_float(start, end, "size", 0.0f)); /* Hiero writes negative sizes */
    } else if (SDL_strcmp(tag, "common") == 0) {
        f->line_height = br_fnt_float(start, end, "lineHeight", 0.0f);
        f->base = br_fnt_float(start, end, "base", 0.0f);
    } else if (SDL_strcmp(tag, "page") == 0) {
        int id = (int)br_fnt_float(start, end, "id", 0.0f);
        if (id < 0 || id >= BR_FONT_MAX_PAGES) {
            br_log_warn("bitmap font: page %d ignored (max %d pages)", id, BR_FONT_MAX_PAGES);
            return;
        }
        br_fnt_attr(start, end, "file", f->page_files[id], sizeof(f->page_files[id]));
        if ((uint32_t)id + 1 > f->page_count) {
            f->page_count = (uint32_t)id + 1;
        }
    } else if (SDL_strcmp(tag, "char") == 0) {
        br_bitmap_char c;
        c.id = (uint32_t)br_fnt_float(start, end, "id", 0.0f);
        c.page = (uint32_t)br_fnt_float(start, end, "page", 0.0f);
        c.x = br_fnt_float(start, end, "x", 0.0f);
        c.y = br_fnt_float(start, end, "y", 0.0f);
        c.width = br_fnt_float(start, end, "width", 0.0f);
        c.height = br_fnt_float(start, end, "height", 0.0f);
        c.xoffset = br_fnt_float(start, end, "xoffset", 0.0f);
        c.yoffset = br_fnt_float(start, end, "yoffset", 0.0f);
        c.xadvance = br_fnt_float(start, end, "xadvance", 0.0f);
        br_array_push(&f->chars, &c);
    } else if (SDL_strcmp(tag, "kerning") == 0) {
        br_bitmap_kerning k;
        k.first = (uint32_t)br_fnt_float(start, end, "first", 0.0f);
        k.second = (uint32_t)br_fnt_float(start, end, "second", 0.0f);
        k.amount = br_fnt_float(start, end, "amount", 0.0f);
        br_array_push(&f->kernings, &k);
    } else if (SDL_strcmp(tag, "distanceField") == 0) {
        char type[32];
        br_fnt_attr(start, end, "fieldType", type, sizeof(type));
        f->distance_field = SDL_strcmp(type, "msdf") == 0 || SDL_strcmp(type, "mtsdf") == 0 ? 2 : 1;
        f->distance_range = br_fnt_float(start, end, "distanceRange", 4.0f);
    }
}

static void br_fnt_parse_text(br_font *f, const char *s, const char *end)
{
    while (s < end) {
        const char *line_end = s;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }
        const char *p = s;
        while (p < line_end && br_fnt_is_space(*p)) {
            p++;
        }
        char tag[32];
        size_t n = 0;
        while (p < line_end && !br_fnt_is_space(*p) && n + 1 < sizeof(tag)) {
            tag[n++] = *p++;
        }
        tag[n] = 0;
        if (n > 0) {
            br_fnt_element(f, tag, p, line_end);
        }
        s = line_end + 1;
    }
}

static void br_fnt_parse_xml(br_font *f, const char *s, const char *end)
{
    while (s < end) {
        while (s < end && *s != '<') {
            s++;
        }
        if (s >= end) {
            break;
        }
        s++;
        const char *close = s;
        while (close < end && *close != '>') {
            close++;
        }
        if (s < end && *s != '?' && *s != '!' && *s != '/') {
            char tag[32];
            size_t n = 0;
            while (s < close && !br_fnt_is_space(*s) && *s != '/' && n + 1 < sizeof(tag)) {
                tag[n++] = *s++;
            }
            tag[n] = 0;
            br_fnt_element(f, tag, s, close);
        }
        s = close + 1;
    }
}

static int br_bitmap_char_compare(const void *a, const void *b)
{
    const br_bitmap_char *x = a;
    const br_bitmap_char *y = b;
    return x->id < y->id ? -1 : (x->id > y->id ? 1 : 0);
}

static int br_bitmap_kerning_compare(const void *a, const void *b)
{
    const br_bitmap_kerning *x = a;
    const br_bitmap_kerning *y = b;
    if (x->first != y->first) {
        return x->first < y->first ? -1 : 1;
    }
    return x->second < y->second ? -1 : (x->second > y->second ? 1 : 0);
}

static br_font *br_bitmap_font_alloc(void)
{
    br_font *f = br_alloc(sizeof(br_font));
    if (f == NULL) {
        return NULL;
    }
    f->is_bitmap = true;
    f->texel_scale = 1.0f;
    br_array_init(&f->glyphs, sizeof(br_atlas_glyph));
    br_array_init(&f->chars, sizeof(br_bitmap_char));
    br_array_init(&f->kernings, sizeof(br_bitmap_kerning));
    return f;
}

static void br_bitmap_font_sort(br_font *f)
{
    if (f->chars.count > 1) {
        SDL_qsort(f->chars.data, f->chars.count, sizeof(br_bitmap_char), br_bitmap_char_compare);
    }
    if (f->kernings.count > 1) {
        SDL_qsort(f->kernings.data, f->kernings.count, sizeof(br_bitmap_kerning), br_bitmap_kerning_compare);
    }
}

br_font *br_bitmap_font_parse(br_engine *e, const char *data, size_t size, const char *page_dir, bool load_pages)
{
    if (data == NULL || size == 0) {
        br_log_error("bitmap font: no data");
        return NULL;
    }
    br_font *f = br_bitmap_font_alloc();
    if (f == NULL) {
        return NULL;
    }
    const char *s = data;
    const char *end = data + size;
    while (s < end && br_fnt_is_space(*s)) {
        s++;
    }
    if (s < end && *s == '<') {
        br_fnt_parse_xml(f, s, end);
    } else {
        br_fnt_parse_text(f, s, end);
    }
    if (f->chars.count == 0 || f->line_height <= 0.0f) {
        br_log_error("bitmap font: not an AngelCode font (no chars or no common lineHeight)");
        br_font_free(e, f);
        return NULL;
    }
    if (f->native_size <= 0.0f) {
        f->native_size = f->line_height;
    }
    br_bitmap_font_sort(f);
    if (load_pages) {
        for (uint32_t i = 0; i < f->page_count; i++) {
            char path[512];
            if (page_dir != NULL && page_dir[0] != 0) {
                br_path_join(path, sizeof(path), page_dir, f->page_files[i]);
            } else {
                br_str_copy(path, sizeof(path), f->page_files[i]);
            }
            f->pages[i] = br_texture_load(e, path); /* cached by the assets module, magenta if missing */
        }
    }
    br_font_track(e, f);
    return f;
}

/* ---- assets */

static void *br_bitmap_font_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    char dir[512];
    br_str_copy(dir, sizeof(dir), path);
    char *slash = SDL_strrchr(dir, '/');
    if (slash != NULL) {
        *slash = 0;
    } else {
        dir[0] = 0;
    }
    br_font *f = br_bitmap_font_parse(e, bytes, size, dir, true);
    if (f == NULL) {
        br_log_error("cannot load bitmap font '%s'", path);
        return NULL;
    }
    f->asset_owned = true;
    return f;
}

static void br_bitmap_font_asset_unload(br_engine *e, void *asset)
{
    br_font_free(e, asset);
}

br_font *br_bitmap_font_load(br_engine *e, const char *path)
{
    br_font *f = br_font_load(e, path);
    if (f != NULL && !f->is_bitmap) {
        br_log_error("br_bitmap_font_load: '%s' is not a bitmap font", path);
        return NULL;
    }
    return f;
}

void br_bitmap_font_module_init(br_engine *e)
{
    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "bitmap_font";
    desc.extensions = "fnt,xml";
    desc.load = br_bitmap_font_asset_load;
    desc.unload = br_bitmap_font_asset_unload;
    br_assets_register_loader(e, &desc);
}

/* ---- install: TTF + style -> bitmap font */

typedef struct br_install_glyph {
    uint32_t cp;
    int      w, h;
    float    left, top;   /* raster pixels from the pen on the baseline */
    float    advance;     /* local units */
    uint8_t *rgba;        /* straight alpha */
    uint32_t page;
    int      x, y;
} br_install_glyph;

/* Composites a coverage image over the premultiplied float canvas with a colour (or gradient). */
static void br_install_layer(float *canvas, int cw, int ch, int cx, int cy, const uint8_t *img, int w, int h, int ix,
                             int iy, br_color color, const br_text_gradient *gradient, float line_top, float line_h)
{
    for (int y = 0; y < h; y++) {
        int py = iy - cy + y;
        if (py < 0 || py >= ch) {
            continue;
        }
        br_color c = color;
        if (gradient != NULL) {
            float ny = line_h > 0.0f ? ((float)(iy + y) - line_top) / line_h : 0.0f;
            c = br_text_gradient_at(gradient, 0.0f, ny);
        }
        for (int x = 0; x < w; x++) {
            int px = ix - cx + x;
            if (px < 0 || px >= cw) {
                continue;
            }
            float a = (float)img[y * w + x] / 255.0f * c.a;
            float *d = &canvas[(py * cw + px) * 4];
            d[0] = c.r * a + d[0] * (1.0f - a);
            d[1] = c.g * a + d[1] * (1.0f - a);
            d[2] = c.b * a + d[2] * (1.0f - a);
            d[3] = a + d[3] * (1.0f - a);
        }
    }
}

typedef struct br_install_image {
    uint8_t *pixels;
    int      w, h;
    int      x, y;   /* top-left in raster pixels from the pen */
} br_install_image;

static br_install_image br_install_render(br_font *font, uint32_t cp, int raster, br_glyph_effect effect, float grow,
                                          float blur, int dx, int dy)
{
    br_install_image im;
    SDL_zero(im);
    if (effect == BR_GLYPH_OUTLINE && grow <= 0.0f) {
        effect = BR_GLYPH_FILL;
    }
    float left;
    float top;
    im.pixels = br_font_render_glyph(font, cp, raster, effect, (int)SDL_roundf(grow * 8.0f),
                                     (int)SDL_roundf(blur * 8.0f), &im.w, &im.h, &left, &top);
    im.x = (int)left + dx;
    im.y = (int)top + dy;
    return im;
}

static void br_install_bounds(const br_install_image *im, int *x0, int *y0, int *x1, int *y1, bool *any)
{
    if (im->pixels == NULL) {
        return;
    }
    if (!*any) {
        *x0 = im->x;
        *y0 = im->y;
        *x1 = im->x + im->w;
        *y1 = im->y + im->h;
        *any = true;
        return;
    }
    *x0 = SDL_min(*x0, im->x);
    *y0 = SDL_min(*y0, im->y);
    *x1 = SDL_max(*x1, im->x + im->w);
    *y1 = SDL_max(*y1, im->y + im->h);
}

/* Packs glyphs into as many pages as needed; false if one glyph is bigger than a page. */
static bool br_install_pack(br_install_glyph *glyphs, uint32_t count, uint32_t page_size, int padding,
                            uint32_t *page_count)
{
    stbrp_rect *rects = br_alloc(sizeof(stbrp_rect) * (count + 1));
    stbrp_node *nodes = br_alloc(sizeof(stbrp_node) * page_size);
    if (rects == NULL || nodes == NULL) {
        br_free(rects);
        br_free(nodes);
        return false;
    }
    uint32_t placed = 0;
    uint32_t pages = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (glyphs[i].rgba == NULL) {
            placed++;
        }
    }
    while (placed < count && pages < BR_FONT_MAX_PAGES) {
        stbrp_context ctx;
        stbrp_init_target(&ctx, (int)page_size, (int)page_size, nodes, (int)page_size);
        int n = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (glyphs[i].rgba != NULL && glyphs[i].page == UINT32_MAX) {
                rects[n].id = (int)i;
                rects[n].w = glyphs[i].w + padding;
                rects[n].h = glyphs[i].h + padding;
                rects[n].was_packed = 0;
                n++;
            }
        }
        stbrp_pack_rects(&ctx, rects, n);
        uint32_t before = placed;
        for (int r = 0; r < n; r++) {
            if (rects[r].was_packed) {
                br_install_glyph *g = &glyphs[rects[r].id];
                g->page = pages;
                g->x = rects[r].x + padding / 2;
                g->y = rects[r].y + padding / 2;
                placed++;
            }
        }
        pages++;
        if (placed == before) {
            break; /* nothing fits an empty page */
        }
    }
    br_free(rects);
    br_free(nodes);
    *page_count = pages;
    return placed == count;
}

static void br_install_free(br_install_glyph *glyphs, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        br_free(glyphs[i].rgba);
    }
    br_free(glyphs);
}

br_font *br_bitmap_font_install(br_engine *e, const char *name, const br_text_style *style_in,
                                const br_bitmap_font_options *opts)
{
    if (e->text == NULL || name == NULL || style_in == NULL) {
        br_log_error("br_bitmap_font_install: missing name or style");
        return NULL;
    }
    br_text_style clean = *style_in;
    br_text_style_sanitize(&clean);
    const br_text_style *style = &clean;
    br_text_span span = br_text_span_from_style(e, style);
    br_font *src = span.font;
    if (src == NULL || src->is_bitmap) {
        br_log_error("br_bitmap_font_install('%s'): the style's font must be a TTF/OTF font", name);
        return NULL;
    }
    br_bitmap_font_options o;
    SDL_zero(o);
    if (opts != NULL) {
        o = *opts;
    }
    float res = o.resolution > 0.0f ? o.resolution : 1.0f;
    int padding = o.padding > 0 ? SDL_min(o.padding, 64) : 2;
    uint32_t page_size = o.page_size > 0 ? SDL_min(o.page_size, 4096u) : 512;
    int raster = br_text_raster_size(span.size * res);
    float texel = span.size / (float)raster; /* local units per raster pixel */

    /* the character set, without duplicates */
    br_array cps;
    br_array_init(&cps, sizeof(uint32_t));
    if (o.chars != NULL) {
        size_t len = SDL_strlen(o.chars);
        size_t off = 0;
        uint32_t cp;
        while ((cp = br_text_utf8_next(o.chars, len, &off)) != 0) {
            bool dup = false;
            for (uint32_t i = 0; i < cps.count && !dup; i++) {
                dup = *(uint32_t *)br_array_at(&cps, i) == cp;
            }
            if (!dup) {
                br_array_push(&cps, &cp);
            }
        }
    } else {
        for (uint32_t cp = 32; cp < 127; cp++) {
            br_array_push(&cps, &cp);
        }
    }
    uint32_t count = cps.count;
    br_install_glyph *glyphs = br_alloc(sizeof(br_install_glyph) * (count + 1));
    if (glyphs == NULL) {
        br_array_free(&cps);
        return NULL;
    }
    br_font_metrics fm = br_font_get_metrics(src, (float)raster);
    float bold = span.synth_bold ? SDL_max(0.5f, (float)raster * 0.03f) : 0.0f;
    float stroke = br_text_effect_clamp(e, style->stroke_width * 0.5f / texel);
    /* shadow offset in raster pixels, bounded so the glyph canvas stays page-sized */
    float reach = (float)BR_ATLAS_MAX_SIZE;
    int sdx = (int)SDL_roundf(SDL_clamp(SDL_cosf(style->shadow_angle) * style->shadow_distance / texel, -reach, reach));
    int sdy = (int)SDL_roundf(SDL_clamp(SDL_sinf(style->shadow_angle) * style->shadow_distance / texel, -reach, reach));
    float blur = br_text_effect_clamp(e, style->shadow_blur / texel);
    const br_text_gradient *gradient = span.use_gradient ? &style->gradient : NULL;

    for (uint32_t i = 0; i < count; i++) {
        br_install_glyph *g = &glyphs[i];
        g->cp = *(uint32_t *)br_array_at(&cps, i);
        g->page = UINT32_MAX;
        g->advance = br_font_advance(src, g->cp, span.size);
        br_install_image fill = br_install_render(src, g->cp, raster, BR_GLYPH_OUTLINE, bold, 0.0f, 0, 0);
        br_install_image outline;
        br_install_image shadow;
        SDL_zero(outline);
        SDL_zero(shadow);
        if (style->stroke_width > 0.0f) {
            outline = br_install_render(src, g->cp, raster, BR_GLYPH_OUTLINE, stroke + bold, 0.0f, 0, 0);
        }
        if (style->drop_shadow) {
            float grow = bold + (style->stroke_width > 0.0f ? stroke : 0.0f);
            shadow = blur > 0.0f ? br_install_render(src, g->cp, raster, BR_GLYPH_SOFT, grow, blur, sdx, sdy)
                                 : br_install_render(src, g->cp, raster, BR_GLYPH_OUTLINE, grow, 0.0f, sdx, sdy);
        }
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool any = false;
        br_install_bounds(&fill, &x0, &y0, &x1, &y1, &any);
        br_install_bounds(&outline, &x0, &y0, &x1, &y1, &any);
        br_install_bounds(&shadow, &x0, &y0, &x1, &y1, &any);
        if (any) {
            int cw = x1 - x0;
            int ch = y1 - y0;
            float *canvas = br_alloc(sizeof(float) * 4 * (size_t)cw * (size_t)ch);
            g->rgba = br_alloc((size_t)cw * (size_t)ch * 4);
            if (canvas != NULL && g->rgba != NULL) {
                float line_top = -fm.ascent;
                if (shadow.pixels != NULL) {
                    br_install_layer(canvas, cw, ch, x0, y0, shadow.pixels, shadow.w, shadow.h, shadow.x, shadow.y,
                                     style->shadow_color, NULL, line_top, fm.line_height);
                }
                if (outline.pixels != NULL) {
                    br_install_layer(canvas, cw, ch, x0, y0, outline.pixels, outline.w, outline.h, outline.x,
                                     outline.y, style->stroke_color, NULL, line_top, fm.line_height);
                }
                if (fill.pixels != NULL) {
                    br_install_layer(canvas, cw, ch, x0, y0, fill.pixels, fill.w, fill.h, fill.x, fill.y, span.color,
                                     gradient, line_top, fm.line_height);
                }
                for (int p = 0; p < cw * ch; p++) {
                    float a = canvas[p * 4 + 3];
                    float inv = a > 0.0f ? 1.0f / a : 0.0f; /* back to straight alpha for the texture */
                    g->rgba[p * 4 + 0] = (uint8_t)(SDL_min(canvas[p * 4 + 0] * inv, 1.0f) * 255.0f + 0.5f);
                    g->rgba[p * 4 + 1] = (uint8_t)(SDL_min(canvas[p * 4 + 1] * inv, 1.0f) * 255.0f + 0.5f);
                    g->rgba[p * 4 + 2] = (uint8_t)(SDL_min(canvas[p * 4 + 2] * inv, 1.0f) * 255.0f + 0.5f);
                    g->rgba[p * 4 + 3] = (uint8_t)(SDL_min(a, 1.0f) * 255.0f + 0.5f);
                }
                g->w = cw;
                g->h = ch;
                g->left = (float)x0;
                g->top = (float)y0;
            } else {
                br_free(g->rgba);
                g->rgba = NULL;
            }
            br_free(canvas);
        }
        br_free(fill.pixels);
        br_free(outline.pixels);
        br_free(shadow.pixels);
    }

    uint32_t page_count = 0;
    if (!br_install_pack(glyphs, count, page_size, padding, &page_count)) {
        br_log_error("br_bitmap_font_install('%s'): glyphs do not fit in %d pages of %u px", name, BR_FONT_MAX_PAGES,
                     page_size);
        br_install_free(glyphs, count);
        br_array_free(&cps);
        return NULL;
    }
    br_font *f = br_bitmap_font_alloc();
    if (f == NULL) {
        br_install_free(glyphs, count);
        br_array_free(&cps);
        return NULL;
    }
    for (uint32_t p = 0; p < page_count; p++) {
        uint8_t *pixels = br_alloc((size_t)page_size * page_size * 4);
        if (pixels == NULL) {
            continue;
        }
        for (uint32_t i = 0; i < count; i++) {
            br_install_glyph *g = &glyphs[i];
            if (g->rgba == NULL || g->page != p) {
                continue;
            }
            for (int y = 0; y < g->h; y++) {
                SDL_memcpy(&pixels[((size_t)(g->y + y) * page_size + (size_t)g->x) * 4], &g->rgba[(size_t)y * g->w * 4],
                           (size_t)g->w * 4);
            }
        }
        f->pages[p] = br_texture_create_rgba8(e, page_size, page_size, pixels, NULL);
        br_free(pixels);
    }
    f->page_count = page_count;
    f->owns_pages = true;
    f->installed = true;
    f->native_size = span.size;
    f->texel_scale = texel;
    f->base = fm.ascent * texel;
    f->line_height = fm.line_height * texel;
    f->colored = style->stroke_width > 0.0f || style->drop_shadow || gradient != NULL || span.color.r < 1.0f ||
                 span.color.g < 1.0f || span.color.b < 1.0f;
    for (uint32_t i = 0; i < count; i++) {
        br_install_glyph *g = &glyphs[i];
        br_bitmap_char c;
        SDL_zero(c);
        c.id = g->cp;
        c.xadvance = g->advance;
        if (g->rgba != NULL) {
            c.page = g->page;
            c.x = (float)g->x;
            c.y = (float)g->y;
            c.width = (float)g->w;
            c.height = (float)g->h;
            c.xoffset = g->left * texel;
            c.yoffset = (g->top + fm.ascent) * texel;
        }
        br_array_push(&f->chars, &c);
    }
    /* ponytail: kerning pairs are only collected for sets up to 256 characters (n^2 lookups) */
    if (count <= 256) {
        for (uint32_t a = 0; a < count; a++) {
            for (uint32_t b = 0; b < count; b++) {
                float k = br_font_kerning(src, glyphs[a].cp, glyphs[b].cp, span.size);
                if (k != 0.0f) {
                    br_bitmap_kerning kern = { glyphs[a].cp, glyphs[b].cp, k };
                    br_array_push(&f->kernings, &kern);
                }
            }
        }
    }
    br_bitmap_font_sort(f);
    br_install_free(glyphs, count);
    br_array_free(&cps);

    br_bitmap_font_uninstall(e, name); /* replaces an older font of that name */
    br_font_track(e, f);
    br_installed_font inst;
    SDL_zero(inst);
    br_str_copy(inst.name, sizeof(inst.name), name);
    inst.font = f;
    br_array_push(&e->text->installed, &inst);
    br_font_add_family(e, name, f, false, false);
    return f;
}

void br_bitmap_font_uninstall(br_engine *e, const char *name)
{
    if (e->text == NULL || name == NULL) {
        return;
    }
    for (uint32_t i = 0; i < e->text->installed.count; i++) {
        br_installed_font *inst = br_array_at(&e->text->installed, i);
        if (SDL_strcmp(inst->name, name) == 0) {
            br_font_free(e, inst->font); /* also removes it from the list and families */
            return;
        }
    }
}

/* ---- queries */

bool br_bitmap_font_get_char(br_font *font, uint32_t codepoint, br_bitmap_char *out)
{
    if (font == NULL || !font->is_bitmap) {
        return false;
    }
    const br_bitmap_char *c = br_font_bitmap_char(font, codepoint);
    if (c == NULL) {
        return false;
    }
    *out = *c;
    return true;
}

uint32_t br_bitmap_font_char_count(br_font *font)
{
    return font != NULL && font->is_bitmap ? font->chars.count : 0;
}

uint32_t br_bitmap_font_page_count(br_font *font)
{
    return font != NULL && font->is_bitmap ? font->page_count : 0;
}

const char *br_bitmap_font_page_file(br_font *font, uint32_t page)
{
    if (font == NULL || !font->is_bitmap || page >= font->page_count) {
        return "";
    }
    return font->page_files[page];
}

br_texture br_bitmap_font_page_texture(br_font *font, uint32_t page)
{
    br_texture none = { 0, 0 };
    if (font == NULL || !font->is_bitmap || page >= font->page_count) {
        return none;
    }
    return font->pages[page];
}

void br_bitmap_font_line_metrics(br_font *font, float *line_height, float *base)
{
    *line_height = font != NULL ? font->line_height : 0.0f;
    *base = font != NULL ? font->base : 0.0f;
}

/* ---- bitmap text node */

uint32_t br_bitmap_text_type(br_engine *e)
{
    return e->text != NULL ? e->text->bitmap_text_type : 0;
}

br_node br_bitmap_text_create(br_engine *e, const char *utf8, br_font *font, float size)
{
    br_text_style style = br_text_style_default();
    style.font = font;
    style.font_size = size;
    style.fill = br_color_make(1.0f, 1.0f, 1.0f, 1.0f); /* white: the page colours show, tint with the node */
    return br_text_node_create(e, br_bitmap_text_type(e), utf8, &style, false);
}
