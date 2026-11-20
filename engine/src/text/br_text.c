/* br_text: the text / bitmap_text / rich_text node types, styles, and glyph drawing. */
#include "text/br_text_internal.h"

#include "text_sdf.frag.h"
#include "textured.vert.h"

/* Layers drawn for every text, back to front. */
typedef enum br_text_layer {
    BR_TEXT_LAYER_SHADOW,
    BR_TEXT_LAYER_STROKE,
    BR_TEXT_LAYER_FILL
} br_text_layer;

/* SDF shader parameters (text_sdf.frag.hlsl, fragment b0). */
typedef struct br_text_sdf_params {
    float field[4];   /* outline value, pixels per value unit, offset, softness */
    float mode[4];    /* x: 1 = MSDF */
} br_text_sdf_params;

/* One cached vertex in the node's local space: colour before the node's tint and alpha. */
typedef struct br_text_cache_vertex {
    float    x, y, u, v;
    br_color color;
} br_text_cache_vertex;

/* A run of cached quads drawn the same way: batched quads of one texture, or one SDF custom draw. */
typedef struct br_text_cache_draw {
    br_texture         texture;
    uint32_t           first;     /* first vertex in cache_local */
    uint32_t           count;     /* vertices (4 per quad) */
    bool               custom;
    br_text_sdf_params params;
} br_text_cache_draw;

/* Everything needed to turn local glyph rects into cached vertices for one layer. */
typedef struct br_text_emit {
    br_engine              *e;
    br_text_node           *node;          /* the cache being built */
    float                   ox, oy;        /* anchor/baseline shift + layer offset */
    br_color                solid;
    const br_text_gradient *gradient;      /* NULL = solid */
    float                   box_w, box_h;  /* gradient box (the text box) */
    float                   shear;         /* fake italic */
    float                   baseline;      /* shear pivot of the current glyph */
    bool                    custom;        /* following quads go to an SDF custom draw */
    bool                    open;          /* the last cached draw takes more SDF quads */
    br_texture              texture;
    br_text_sdf_params      params;
} br_text_emit;

/* ---- styles */

br_text_style br_text_style_default(void)
{
    br_text_style s;
    SDL_zero(s);
    s.font_size = 26.0f;
    s.fill = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    s.stroke_color = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    s.shadow_color = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    s.shadow_angle = SDL_PI_F / 6.0f;
    s.shadow_distance = 5.0f;
    s.wrap_width = 100.0f;
    s.gradient.stop_count = 2;
    s.gradient.offsets[1] = 1.0f;
    s.gradient.colors[0] = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
    s.gradient.colors[1] = br_color_make(0.0f, 0.0f, 0.0f, 1.0f);
    s.gradient.end = br_vec2_make(0.0f, 1.0f);
    return s;
}

void br_text_style_set_family(br_text_style *style, const char *family)
{
    br_str_copy(style->font_family, sizeof(style->font_family), family != NULL ? family : "");
}

void br_text_style_set_vertical_gradient(br_text_style *style, br_color top, br_color bottom)
{
    SDL_zero(style->gradient);
    style->use_gradient = true;
    style->gradient.stop_count = 2;
    style->gradient.offsets[0] = 0.0f;
    style->gradient.offsets[1] = 1.0f;
    style->gradient.colors[0] = top;
    style->gradient.colors[1] = bottom;
    style->gradient.start = br_vec2_make(0.0f, 0.0f);
    style->gradient.end = br_vec2_make(0.0f, 1.0f);
}

/* ---- limits */

/* v if finite and within [lo, hi] (clamped), else `fallback`. */
static float br_text_clamp(float v, float lo, float hi, float fallback)
{
    if (SDL_isnanf(v) || SDL_isinff(v)) {
        return fallback;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

void br_text_style_sanitize(br_text_style *s)
{
    float big = BR_TEXT_MAX_LENGTH;
    s->font_size = br_text_clamp(s->font_size, 0.0f, BR_TEXT_MAX_FONT_SIZE, 0.0f);
    s->stroke_width = br_text_clamp(s->stroke_width, 0.0f, BR_TEXT_MAX_FONT_SIZE, 0.0f);
    s->shadow_blur = br_text_clamp(s->shadow_blur, 0.0f, BR_TEXT_MAX_FONT_SIZE, 0.0f);
    s->shadow_angle = br_text_clamp(s->shadow_angle, -big, big, 0.0f);
    s->shadow_distance = br_text_clamp(s->shadow_distance, -big, big, 0.0f);
    s->wrap_width = br_text_clamp(s->wrap_width, 0.0f, big, 0.0f);
    s->line_height = br_text_clamp(s->line_height, 0.0f, big, 0.0f);
    s->leading = br_text_clamp(s->leading, -big, big, 0.0f);
    s->letter_spacing = br_text_clamp(s->letter_spacing, -big, big, 0.0f);
    s->padding = br_text_clamp(s->padding, 0.0f, big, 0.0f);
}

float br_text_effect_clamp(br_engine *e, float raster_px)
{
    if (SDL_isnanf(raster_px)) {
        return 0.0f;
    }
    if (raster_px <= BR_TEXT_MAX_EFFECT_PX) {
        return raster_px;
    }
    if (e->text != NULL && !e->text->effect_clamp_logged) {
        br_log_warn("text: stroke / shadow of %.0f raster pixels clamped to %.0f (use an SDF font or a filter "
                    "for wider ones)", (double)raster_px, (double)BR_TEXT_MAX_EFFECT_PX);
        e->text->effect_clamp_logged = true;
    }
    return BR_TEXT_MAX_EFFECT_PX;
}

int br_text_raster_size(float pixels)
{
    return (int)SDL_roundf(br_text_clamp(pixels, 1.0f, (float)BR_TEXT_MAX_RASTER, 1.0f));
}

/* ---- node data */

br_text_node *br_text_node_get(br_engine *e, br_node n)
{
    br_text_state *s = e->text;
    if (s == NULL) {
        return NULL;
    }
    uint32_t type = br_node_type_of(e, n);
    if (type == 0 || (type != s->text_type && type != s->bitmap_text_type && type != s->rich_text_type)) {
        return NULL;
    }
    return br_node_data(e, n, type);
}

static void br_text_on_create(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_text_node *t = data;
    br_text_layout_init(&t->layout);
    br_array_init(&t->spans, sizeof(br_text_span));
    br_array_init(&t->cache_local, sizeof(br_text_cache_vertex));
    br_array_init(&t->cache_draws, sizeof(br_text_cache_draw));
    br_array_init(&t->cache_world, sizeof(br_vertex2d));
    t->style = br_text_style_default();
    t->dirty = true;
}

static void br_text_on_destroy(br_engine *e, br_node n, void *data)
{
    (void)e;
    (void)n;
    br_text_node *t = data;
    br_free(t->text);
    t->text = NULL;
    br_rich_text_free(t);
    br_text_layout_free(&t->layout);
    br_array_free(&t->spans);
    br_array_free(&t->cache_local);
    br_array_free(&t->cache_draws);
    br_array_free(&t->cache_world);
}

void br_text_forget_font(br_engine *e, const br_font *font)
{
    br_text_state *s = e->text;
    if (s == NULL || e->scene == NULL) {
        return;
    }
    uint32_t types[3] = { s->text_type, s->bitmap_text_type, s->rich_text_type };
    for (int k = 0; k < 3; k++) {
        br_node_type_entry *type = br_scene_type_entry(e->scene, types[k]);
        if (type == NULL) {
            continue;
        }
        for (uint32_t i = 0; i < type->data.count; i++) {
            br_text_node *t = br_array_at(&type->data, i);
            if (t->style.font == font) {
                t->style.font = NULL; /* the family (or the default font) takes over */
                t->dirty = true;
            }
            for (uint32_t j = 0; j < t->spans.count && !t->dirty; j++) {
                const br_text_span *span = br_array_at(&t->spans, j);
                for (uint32_t k = 0; k < span->chain_count; k++) {
                    if (span->chain[k] == font) {
                        t->dirty = true; /* every reader of spans runs br_text_node_update first */
                        break;
                    }
                }
            }
        }
    }
}

void br_text_node_update(br_engine *e, br_text_node *node)
{
    if (!node->dirty && node->layout_generation == e->text->layout_generation) {
        return;
    }
    br_array chars;
    br_array_init(&chars, sizeof(br_layout_char));
    if (node->rich) {
        br_rich_text_parse(e, node, &chars);
    } else {
        br_text_decode(node->text, node->style.white_space, &chars);
        br_array_clear(&node->spans);
        br_text_span span = br_text_span_from_style(e, &node->style);
        br_array_push(&node->spans, &span);
    }
    br_text_layout_build(&node->layout, &chars, node->spans.data, &node->style);
    br_array_free(&chars);
    node->dirty = false;
    node->layout_generation = e->text->layout_generation;
    node->cache_valid = false;
}

/* Local offset of the text box: anchor and baseline mode. */
static br_vec2 br_text_origin(const br_text_node *t)
{
    float shift = br_text_baseline_shift(&t->layout, &t->style);
    return br_vec2_make(-t->anchor.x * t->layout.width, -t->anchor.y * t->layout.height - shift);
}

/* ---- vertex emission */

br_color br_text_gradient_at(const br_text_gradient *g, float nx, float ny)
{
    float dx = g->end.x - g->start.x;
    float dy = g->end.y - g->start.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((nx - g->start.x) * dx + (ny - g->start.y) * dy) / len2 : 0.0f;
    uint32_t n = g->stop_count < 1 ? 1 : (g->stop_count > BR_TEXT_MAX_STOPS ? BR_TEXT_MAX_STOPS : g->stop_count);
    if (t <= g->offsets[0] || n == 1) {
        return g->colors[0];
    }
    for (uint32_t i = 1; i < n; i++) {
        if (t <= g->offsets[i]) {
            float span = g->offsets[i] - g->offsets[i - 1];
            float f = span > 0.0f ? (t - g->offsets[i - 1]) / span : 1.0f;
            return br_color_lerp(g->colors[i - 1], g->colors[i], f);
        }
    }
    return g->colors[n - 1];
}

/* A vertex in the node's local space (shear and offsets applied), colour before tint and alpha. */
static br_text_cache_vertex br_text_vertex(const br_text_emit *em, float x, float y, float u, float v)
{
    br_color c = em->solid;
    if (em->gradient != NULL) {
        float nx = em->box_w > 0.0f ? x / em->box_w : 0.0f;
        float ny = em->box_h > 0.0f ? y / em->box_h : 0.0f;
        c = br_text_gradient_at(em->gradient, nx, ny);
    }
    br_text_cache_vertex out;
    out.x = x + (em->baseline - y) * em->shear + em->ox;
    out.y = y + em->oy;
    out.u = u;
    out.v = v;
    out.color = c;
    return out;
}

/* Ends the SDF custom draw being cached: the next SDF quad starts a new one. */
static void br_text_flush_custom(br_text_emit *em)
{
    em->open = false;
}

/* Switches the following quads to the SDF shader with these parameters (flushing a batch with others). */
static void br_text_use_sdf(br_text_emit *em, const br_text_sdf_params *params)
{
    if (!em->custom || SDL_memcmp(&em->params, params, sizeof(*params)) != 0) {
        if (em->custom) {
            br_text_flush_custom(em);
        }
        em->params = *params;
        em->custom = true;
    }
}

/* Switches the following quads back to the batcher. */
static void br_text_use_batch(br_text_emit *em)
{
    if (em->custom) {
        br_text_flush_custom(em);
        em->custom = false;
    }
}

/* One quad with uniform-per-corner colours. */
static void br_text_quad(br_text_emit *em, br_texture texture, float x0, float y0, float x1, float y1, float u0,
                         float v0, float u1, float v1)
{
    br_text_cache_vertex v[4];
    v[0] = br_text_vertex(em, x0, y0, u0, v0);
    v[1] = br_text_vertex(em, x1, y0, u1, v0);
    v[2] = br_text_vertex(em, x1, y1, u1, v1);
    v[3] = br_text_vertex(em, x0, y1, u0, v1);
    br_text_node *t = em->node;
    br_text_cache_draw *last = t->cache_draws.count > 0 ? br_array_at(&t->cache_draws, t->cache_draws.count - 1) : NULL;
    bool start = false;
    if (!em->custom) {
        /* batched quads of one texture share a draw; each is still pushed on its own */
        start = last == NULL || last->custom || last->texture.index != texture.index ||
                last->texture.generation != texture.generation;
    } else {
        if (texture.index != em->texture.index || texture.generation != em->texture.generation) {
            br_text_flush_custom(em);
            em->texture = texture;
        }
        start = !em->open;
    }
    if (start) {
        br_text_cache_draw d;
        SDL_zero(d);
        d.texture = texture;
        d.first = t->cache_local.count;
        d.custom = em->custom;
        d.params = em->params;
        last = br_array_push(&t->cache_draws, &d);
        em->open = em->custom;
    }
    if (last == NULL || br_array_push_n(&t->cache_local, v, 4) == NULL) {
        return; /* out of memory: the quad is not drawn */
    }
    last->count += 4;
}

/* A glyph quad; gradients along x or y are cut at their stops so every stop shows exactly. */
static void br_text_emit_quad(br_text_emit *em, br_texture texture, float x0, float y0, float x1, float y1,
                              float u0, float v0, float u1, float v1)
{
    const br_text_gradient *g = em->gradient;
    bool vertical = g != NULL && g->start.x == g->end.x && g->start.y != g->end.y;
    bool horizontal = g != NULL && g->start.y == g->end.y && g->start.x != g->end.x;
    if (!vertical && !horizontal) {
        /* ponytail: diagonal gradients are sampled at the corners only (exact for two stops) */
        br_text_quad(em, texture, x0, y0, x1, y1, u0, v0, u1, v1);
        return;
    }
    float a0 = vertical ? y0 : x0;
    float a1 = vertical ? y1 : x1;
    float box = vertical ? em->box_h : em->box_w;
    float from = vertical ? g->start.y : g->start.x;
    float to = vertical ? g->end.y : g->end.x;
    float cuts[BR_TEXT_MAX_STOPS + 2];
    uint32_t n = 0;
    cuts[n++] = a0;
    for (uint32_t i = 0; i < g->stop_count && i < BR_TEXT_MAX_STOPS; i++) {
        float c = box * (from + (to - from) * g->offsets[i]);
        if (c > a0 && c < a1) {
            cuts[n++] = c;
        }
    }
    cuts[n++] = a1;
    /* insertion sort: a handful of values, possibly descending when end < start */
    for (uint32_t i = 1; i < n; i++) {
        float v = cuts[i];
        uint32_t k = i;
        while (k > 0 && cuts[k - 1] > v) {
            cuts[k] = cuts[k - 1];
            k--;
        }
        cuts[k] = v;
    }
    for (uint32_t i = 0; i + 1 < n; i++) {
        float t0 = (cuts[i] - a0) / (a1 - a0);
        float t1 = (cuts[i + 1] - a0) / (a1 - a0);
        if (vertical) {
            br_text_quad(em, texture, x0, cuts[i], x1, cuts[i + 1], u0, v0 + (v1 - v0) * t0, u1, v0 + (v1 - v0) * t1);
        } else {
            br_text_quad(em, texture, cuts[i], y0, cuts[i + 1], y1, u0 + (u1 - u0) * t0, v0, u0 + (u1 - u0) * t1, v1);
        }
    }
}

/* ---- drawing one layer */

static int br_text_quantize(float pixels)
{
    return (int)SDL_roundf(pixels * 8.0f);
}

/* The box drawn for a character no font of a bitmap span has (TTF fonts draw their .notdef):
 * an outline filling the glyph's advance, from the baseline up to 70% of the ascent. */
static void br_text_missing_box(br_text_emit *em, const br_text_span *span, br_font *font, const br_layout_glyph *g,
                                const br_layout_line *line)
{
    br_font_metrics m;
    br_font_get_metrics(font, span->size, &m);
    float x0 = g->x + g->advance * 0.1f;
    float x1 = g->x + g->advance * 0.9f;
    float y1 = line->baseline;
    float y0 = y1 - m.ascent * 0.7f;
    float th = SDL_max(1.0f, span->size * 0.06f);
    if (x1 - x0 < th * 2.0f || y1 - y0 < th * 2.0f) {
        return;
    }
    br_texture white = br_texture_white(em->e);
    br_text_emit_quad(em, white, x0, y0, x1, y0 + th, 0.5f, 0.5f, 0.5f, 0.5f);
    br_text_emit_quad(em, white, x0, y1 - th, x1, y1, 0.5f, 0.5f, 0.5f, 0.5f);
    br_text_emit_quad(em, white, x0, y0 + th, x0 + th, y1 - th, 0.5f, 0.5f, 0.5f, 0.5f);
    br_text_emit_quad(em, white, x1 - th, y0 + th, x1, y1 - th, 0.5f, 0.5f, 0.5f, 0.5f);
}

/* Draws (emit) or just rasterises (!emit) every glyph of one layer. */
static void br_text_draw_layer(br_text_emit *em, br_text_node *t, br_text_layer layer, float resolution, bool emit)
{
    const br_text_style *st = &t->style;
    const br_text_span *spans = t->spans.data;
    em->custom = false;
    em->texture.index = 0;
    em->texture.generation = 0;
    br_vec2 origin = br_text_origin(t);
    em->ox = origin.x;
    em->oy = origin.y;
    if (layer == BR_TEXT_LAYER_SHADOW) {
        em->ox += SDL_cosf(st->shadow_angle) * st->shadow_distance;
        em->oy += SDL_sinf(st->shadow_angle) * st->shadow_distance;
    }
    for (uint32_t i = 0; i < t->layout.glyphs.count; i++) {
        const br_layout_glyph *g = br_array_at(&t->layout.glyphs, i);
        if (g->cp <= ' ') {
            continue;
        }
        const br_text_span *span = &spans[g->span];
        br_font *font = g->font; /* the span's font or a fallback (br_text_glyph_font) */
        if (font == NULL) {
            continue;
        }
        const br_layout_line *line = br_array_at(&t->layout.lines, g->line);
        em->baseline = line->baseline;
        em->shear = span->synth_italic ? 0.2f : 0.0f;
        em->solid = layer == BR_TEXT_LAYER_SHADOW ? st->shadow_color
                  : layer == BR_TEXT_LAYER_STROKE ? st->stroke_color
                                                  : span->color;
        em->gradient = layer == BR_TEXT_LAYER_FILL && span->use_gradient ? &st->gradient : NULL;
        float stroke_r = st->stroke_width * 0.5f; /* local units outside the outline */

        if (font->is_bitmap) {
            const br_bitmap_char *ch = br_font_bitmap_char(font, g->cp);
            if (ch == NULL) {
                if (layer == BR_TEXT_LAYER_FILL && emit) {
                    br_text_use_batch(em);
                    br_text_missing_box(em, span, font, g, line);
                }
                continue;
            }
            if (ch->width <= 0.0f || ch->page >= font->page_count) {
                continue;
            }
            bool sdf = font->distance_field != 0;
            if ((layer == BR_TEXT_LAYER_STROKE && !sdf) || (layer != BR_TEXT_LAYER_FILL && font->colored)) {
                continue; /* coverage bitmap fonts bake their stroke and shadow (br_bitmap_font_install) */
            }
            if (!emit) {
                continue;
            }
            if (layer == BR_TEXT_LAYER_FILL && font->colored) {
                em->solid = br_color_make(1.0f, 1.0f, 1.0f, span->color.a);
                em->gradient = NULL;
            }
            float k = span->size / font->native_size;
            float page_px = k * font->texel_scale; /* local units per page pixel */
            br_texture page = font->pages[ch->page];
            br_texture_info info;
            SDL_zero(info);
            br_texture_get_info(em->e, page, &info);
            float pw = (float)(info.source_width > 0 ? info.source_width : 1);
            float ph = (float)(info.source_height > 0 ? info.source_height : 1);
            float x0 = g->x + ch->xoffset * k;
            float y0 = line->baseline - font->base * k + ch->yoffset * k;
            if (sdf) {
                float offset = 0.0f;
                float soft = 0.0f;
                if (layer != BR_TEXT_LAYER_FILL) {
                    offset = stroke_r / page_px;
                }
                if (layer == BR_TEXT_LAYER_SHADOW) {
                    soft = st->shadow_blur / page_px;
                }
                if (span->synth_bold) {
                    offset += font->distance_range * 0.15f;
                }
                if (offset > font->distance_range * 0.5f) {
                    offset = font->distance_range * 0.5f;
                }
                br_text_sdf_params params;
                SDL_zero(params);
                params.field[0] = 0.5f;
                params.field[1] = font->distance_range;
                params.field[2] = offset;
                params.field[3] = soft;
                params.mode[0] = font->distance_field == 2 ? 1.0f : 0.0f;
                br_text_use_sdf(em, &params);
            } else {
                br_text_use_batch(em);
            }
            br_text_emit_quad(em, page, x0, y0, x0 + ch->width * page_px, y0 + ch->height * page_px, ch->x / pw,
                              ch->y / ph, (ch->x + ch->width) / pw, (ch->y + ch->height) / ph);
            continue;
        }

        if (font->opts.sdf) {
            const br_atlas_glyph *ag = br_font_atlas_glyph(em->e, font, g->cp, 0, BR_GLYPH_SDF, 0, 0);
            if (ag == NULL || ag->w == 0 || !emit) {
                continue;
            }
            float k = span->size / font->opts.sdf_size; /* local units per glyph pixel */
            float pad = (float)font->opts.sdf_padding;
            float offset = layer == BR_TEXT_LAYER_FILL ? 0.0f : stroke_r / k;
            if (span->synth_bold) {
                offset += font->opts.sdf_size * 0.03f;
            }
            if (offset > pad - 1.0f) {
                offset = pad - 1.0f;
            }
            br_text_sdf_params params;
            SDL_zero(params);
            params.field[0] = 128.0f / 255.0f;
            params.field[1] = 255.0f * pad / 127.0f;
            params.field[2] = offset;
            params.field[3] = layer == BR_TEXT_LAYER_SHADOW ? st->shadow_blur / k : 0.0f;
            br_text_use_sdf(em, &params);
            float aw = (float)font->atlas_w;
            float ah = (float)font->atlas_h;
            float x0 = g->x + ag->left * k;
            float y0 = line->baseline + ag->top * k;
            br_text_emit_quad(em, font->atlas_texture, x0, y0, x0 + (float)ag->w * k, y0 + (float)ag->h * k,
                              (float)ag->x / aw, (float)ag->y / ah, (float)(ag->x + ag->w) / aw,
                              (float)(ag->y + ag->h) / ah);
            continue;
        }

        /* coverage glyphs rasterised at the drawn pixel size */
        int raster = br_text_raster_size(span->size * resolution);
        float k = span->size / (float)raster; /* local units per raster pixel (of the clamped raster) */
        float bold = span->synth_bold ? SDL_max(0.5f, (float)raster * 0.03f) : 0.0f;
        br_glyph_effect effect = bold > 0.0f ? BR_GLYPH_OUTLINE : BR_GLYPH_FILL;
        float grow = bold;
        float blur = 0.0f;
        if (layer != BR_TEXT_LAYER_FILL) {
            grow = br_text_effect_clamp(em->e, grow + stroke_r / k); /* SDF cost grows with its square */
            effect = grow > 0.0f ? BR_GLYPH_OUTLINE : BR_GLYPH_FILL;
        }
        if (layer == BR_TEXT_LAYER_SHADOW && st->shadow_blur > 0.0f) {
            blur = br_text_effect_clamp(em->e, st->shadow_blur / k);
            effect = BR_GLYPH_SOFT;
        }
        const br_atlas_glyph *ag = br_font_atlas_glyph(em->e, font, g->cp, raster, effect,
                                                       effect == BR_GLYPH_FILL ? 0 : br_text_quantize(grow),
                                                       br_text_quantize(blur));
        if (ag == NULL || ag->w == 0 || !emit) {
            continue;
        }
        br_text_use_batch(em);
        float aw = (float)font->atlas_w;
        float ah = (float)font->atlas_h;
        /* snap the pen to the raster grid so 1:1 text is not resampled */
        float x0 = (SDL_roundf(g->x / k) + ag->left) * k;
        float y0 = (SDL_roundf(line->baseline / k) + ag->top) * k;
        br_text_emit_quad(em, font->atlas_texture, x0, y0, x0 + (float)ag->w * k, y0 + (float)ag->h * k,
                          (float)ag->x / aw, (float)ag->y / ah, (float)(ag->x + ag->w) / aw,
                          (float)(ag->y + ag->h) / ah);
    }
    br_text_use_batch(em);
}

/* Underline and strikethrough bars, one per run of decorated characters on a line. */
static void br_text_draw_decorations(br_text_emit *em, br_text_node *t)
{
    const br_text_span *spans = t->spans.data;
    br_vec2 origin = br_text_origin(t);
    em->ox = origin.x;
    em->oy = origin.y;
    em->shear = 0.0f;
    em->custom = false;
    em->open = false;
    br_texture white = br_texture_white(em->e);
    for (int kind = 0; kind < 2; kind++) {
        for (uint32_t li = 0; li < t->layout.lines.count; li++) {
            const br_layout_line *line = br_array_at(&t->layout.lines, li);
            uint32_t k = 0;
            while (k < line->visible) {
                const br_layout_glyph *g = br_array_at(&t->layout.glyphs, line->first + k);
                const br_text_span *span = &spans[g->span];
                bool on = kind == 0 ? span->underline : span->strike;
                if (!on) {
                    k++;
                    continue;
                }
                uint32_t end = k;
                const br_layout_glyph *last = g;
                while (end < line->visible) {
                    const br_layout_glyph *h = br_array_at(&t->layout.glyphs, line->first + end);
                    const br_text_span *hs = &spans[h->span];
                    if (!(kind == 0 ? hs->underline : hs->strike)) {
                        break;
                    }
                    last = h;
                    end++;
                }
                float thickness = SDL_max(1.0f, span->size * 0.06f);
                float y = kind == 0 ? line->baseline + span->size * 0.12f : line->baseline - span->size * 0.28f;
                em->solid = span->color;
                em->gradient = span->use_gradient ? &t->style.gradient : NULL;
                em->baseline = line->baseline;
                br_text_emit_quad(em, white, g->x, y - thickness * 0.5f, last->x + last->advance,
                                  y + thickness * 0.5f, 0.5f, 0.5f, 0.5f, 0.5f);
                k = end;
            }
        }
    }
}

/* Shadow, stroke and fill layers: rasterise only (emit false) or draw. */
static void br_text_draw_layers(br_text_emit *em, br_text_node *t, float resolution, bool emit)
{
    const br_text_style *st = &t->style;
    if (st->drop_shadow) {
        br_text_draw_layer(em, t, BR_TEXT_LAYER_SHADOW, resolution, emit);
    }
    if (st->stroke_width > 0.0f) {
        br_text_draw_layer(em, t, BR_TEXT_LAYER_STROKE, resolution, emit);
    }
    br_text_draw_layer(em, t, BR_TEXT_LAYER_FILL, resolution, emit);
}

/* Rebuilds the node's cached quads unless they are still valid for this layout, resolution and
 * every font atlas (an atlas that grew moved its glyphs and replaced its texture). */
static void br_text_cache_build(br_engine *e, br_text_node *t, float resolution)
{
    br_text_node_update(e, t);
    if (t->cache_valid && t->cache_resolution == resolution && t->cache_atlas_generation == e->text->atlas_generation) {
        return;
    }
    br_array_clear(&t->cache_local);
    br_array_clear(&t->cache_draws);
    t->world_valid = false;
    br_text_emit em;
    SDL_zero(em);
    em.e = e;
    em.node = t;
    em.box_w = t->layout.width;
    em.box_h = t->layout.height;
    /* pass 0 rasterises every glyph first: if the atlas grows, it happens before any vertex of
     * this node points into it */
    br_text_draw_layers(&em, t, resolution, false);
    br_text_draw_layers(&em, t, resolution, true);
    br_text_draw_decorations(&em, t);
    t->cache_valid = true;
    t->cache_resolution = resolution;
    t->cache_atlas_generation = e->text->atlas_generation;
    t->cache_builds += 1;
}

/* Pushes the cached quads: world vertices are recomputed only when the matrix, tint or alpha changed. */
static void br_text_cache_draw_all(br_engine *e, br_text_node *t, br_render_ctx *rc, br_mat3 m, br_color tint,
                                   float alpha)
{
    uint32_t count = t->cache_local.count;
    if (!t->world_valid || SDL_memcmp(&m, &t->world_m, sizeof(m)) != 0 ||
        SDL_memcmp(&tint, &t->world_tint, sizeof(tint)) != 0 || alpha != t->world_alpha) {
        if (!br_array_resize(&t->cache_world, count)) {
            return;
        }
        const br_text_cache_vertex *src = t->cache_local.data;
        br_vertex2d *dst = t->cache_world.data;
        for (uint32_t i = 0; i < count; i++) {
            br_color c = src[i].color;
            c = br_color_make(c.r * tint.r, c.g * tint.g, c.b * tint.b, c.a);
            br_vec2 p = br_mat3_apply(m, br_vec2_make(src[i].x, src[i].y));
            dst[i].x = p.x;
            dst[i].y = p.y;
            dst[i].u = src[i].u;
            dst[i].v = src[i].v;
            dst[i].color = br_render_pack_color(c, alpha);
        }
        t->world_valid = true;
        t->world_m = m;
        t->world_tint = tint;
        t->world_alpha = alpha;
    }
    br_text_state *s = e->text;
    const br_vertex2d *world = t->cache_world.data;
    for (uint32_t i = 0; i < t->cache_draws.count; i++) {
        const br_text_cache_draw *d = br_array_at(&t->cache_draws, i);
        if (!d->custom) {
            for (uint32_t q = 0; q < d->count; q += 4) {
                br_render_push_quad(rc, d->texture, world + d->first + q);
            }
            continue;
        }
        uint32_t index_count = d->count / 4 * 6;
        while (s->quad_indices.count < index_count) { /* 0,1,2,0,2,3 per quad, shared by every draw */
            uint32_t base = s->quad_indices.count / 6 * 4;
            uint32_t idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
            if (br_array_push_n(&s->quad_indices, idx, 6) == NULL) {
                return;
            }
        }
        br_custom_draw draw;
        SDL_zero(draw);
        draw.vertex_shader = &br_shader_textured_vert;
        draw.fragment_shader = &br_shader_text_sdf_frag;
        draw.layout.buffer_count = 1;
        draw.layout.strides[0] = sizeof(br_vertex2d);
        draw.layout.attribute_count = 3;
        draw.layout.attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };
        draw.layout.attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 };
        draw.layout.attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
        draw.vertices = world + d->first;
        draw.vertex_bytes = d->count * (uint32_t)sizeof(br_vertex2d);
        draw.indices = s->quad_indices.data;
        draw.index_count = index_count;
        draw.textures[0] = d->texture;
        draw.texture_count = 1;
        draw.fragment_uniforms = &d->params;
        draw.fragment_uniform_size = sizeof(d->params);
        br_render_push_custom(rc, &draw);
    }
}

static void br_text_render(br_engine *e, br_node n, const void *data, br_render_ctx *rc)
{
    (void)data;
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return;
    }
    br_text_node_update(e, t);
    float alpha = br_node_world_alpha(e, n);
    if (t->layout.glyphs.count == 0 || alpha <= 0.0f) {
        return;
    }
    float resolution = t->resolution > 0.0f ? t->resolution : br_render_pixel_ratio(rc);
    br_text_cache_build(e, t, resolution);
    br_text_cache_draw_all(e, t, rc, br_render_node_transform(rc, n), br_node_world_tint(e, n), alpha);
}

void br_text_prepare(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return;
    }
    float resolution = t->resolution > 0.0f ? t->resolution : br_engine_pixel_density(e);
    br_text_cache_build(e, t, resolution > 0.0f ? resolution : 1.0f); /* no render context needed */
}

uint32_t br_text_quad_builds(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    return t != NULL ? t->cache_builds : 0;
}

static bool br_text_local_bounds(br_engine *e, br_node n, const void *data, br_rect *out)
{
    (void)data;
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return false;
    }
    br_text_node_update(e, t);
    br_vec2 o = br_text_origin(t);
    float pad = t->style.padding;
    *out = br_rect_make(o.x - pad, o.y - pad, t->layout.width + pad * 2.0f, t->layout.height + pad * 2.0f);
    return true;
}

static bool br_text_hit_test(br_engine *e, br_node n, const void *data, br_vec2 p)
{
    br_rect r;
    return br_text_local_bounds(e, n, data, &r) && br_rect_contains(r, p.x, p.y);
}

/* ---- public API */

br_node br_text_node_create(br_engine *e, uint32_t type, const char *text, const br_text_style *style, bool rich)
{
    br_node n = br_node_create(e, type);
    br_text_node *t = br_text_node_get(e, n);
    if (t != NULL) {
        t->text = br_strdup(text != NULL ? text : "");
        t->style = style != NULL ? *style : br_text_style_default();
        br_text_style_sanitize(&t->style);
        t->rich = rich;
        t->dirty = true;
    }
    return n;
}

uint32_t br_text_type(br_engine *e)
{
    return e->text != NULL ? e->text->text_type : 0;
}

br_node br_text_create(br_engine *e, const char *utf8, const br_text_style *style)
{
    return br_text_node_create(e, br_text_type(e), utf8, style, false);
}

void br_text_set_text(br_engine *e, br_node n, const char *utf8)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return;
    }
    if (t->text != NULL && utf8 != NULL && SDL_strcmp(t->text, utf8) == 0) {
        return;
    }
    br_free(t->text);
    t->text = br_strdup(utf8 != NULL ? utf8 : "");
    t->dirty = true;
}

const char *br_text_text(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    return t != NULL && t->text != NULL ? t->text : "";
}

void br_text_set_style(br_engine *e, br_node n, const br_text_style *style)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t != NULL && style != NULL) {
        t->style = *style;
        br_text_style_sanitize(&t->style);
        t->dirty = true;
    }
}

bool br_text_get_style(br_engine *e, br_node n, br_text_style *out)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return false;
    }
    *out = t->style;
    return true;
}

void br_text_set_anchor(br_engine *e, br_node n, float x, float y)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t != NULL) {
        t->anchor = br_vec2_make(x, y);
        t->cache_valid = false; /* cached vertices include the anchor offset */
    }
}

void br_text_set_resolution(br_engine *e, br_node n, float resolution)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t != NULL) {
        t->resolution = resolution > 0.0f && resolution <= 16.0f ? resolution : 0.0f; /* NaN = auto */
    }
}

br_vec2 br_text_size(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return br_vec2_make(0.0f, 0.0f);
    }
    br_text_node_update(e, t);
    return br_vec2_make(t->layout.width, t->layout.height);
}

uint32_t br_text_line_count(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return 0;
    }
    br_text_node_update(e, t);
    return t->layout.lines.count;
}

bool br_text_get_line(br_engine *e, br_node n, uint32_t i, br_text_line_info *out)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return false;
    }
    br_text_node_update(e, t);
    const br_layout_line *line = br_array_at(&t->layout.lines, i);
    if (line == NULL) {
        return false;
    }
    SDL_zero(*out);
    if (line->count > 0) {
        const br_layout_glyph *first = br_array_at(&t->layout.glyphs, line->first);
        out->byte_start = first->src;
        out->byte_end = first->src;
    }
    if (line->visible > 0) {
        const br_layout_glyph *last = br_array_at(&t->layout.glyphs, line->first + line->visible - 1);
        out->byte_end = last->src + last->src_len;
    }
    out->x = line->x;
    out->y = line->top;
    out->width = line->width;
    out->height = line->height;
    out->baseline = line->baseline;
    return true;
}

uint32_t br_text_char_count(br_engine *e, br_node n)
{
    br_text_node *t = br_text_node_get(e, n);
    if (t == NULL) {
        return 0;
    }
    br_text_node_update(e, t);
    return t->layout.glyphs.count;
}

bool br_text_get_char_box(br_engine *e, br_node n, uint32_t i, br_rect *out)
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
    const br_layout_line *line = br_array_at(&t->layout.lines, g->line);
    *out = br_rect_make(g->x, line->top, g->advance, line->height);
    return true;
}

/* ---- module */

static void br_text_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_text_state *s = e->text;
    br_font_module_shutdown(e);
    br_array_free(&s->quad_indices);
    br_free(s);
    e->text = NULL;
}

static uint32_t br_text_register_type(br_engine *e, const char *name)
{
    br_node_type_desc desc;
    SDL_zero(desc);
    desc.name = name;
    desc.data_size = sizeof(br_text_node);
    desc.on_create = br_text_on_create;
    desc.on_destroy = br_text_on_destroy;
    desc.local_bounds = br_text_local_bounds;
    desc.render = br_text_render;
    desc.hit_test = br_text_hit_test;
    return br_node_type_register(e, &desc);
}

void br_text_register(br_engine *e)
{
    br_text_state *s = br_alloc(sizeof(br_text_state));
    if (s == NULL) {
        return;
    }
    e->text = s;
    br_array_init(&s->quad_indices, sizeof(uint32_t));
    s->text_type = br_text_register_type(e, "text");
    s->bitmap_text_type = br_text_register_type(e, "bitmap_text");
    s->rich_text_type = br_text_register_type(e, "rich_text");
    br_font_module_init(e);
    br_bitmap_font_module_init(e);
    br_engine_on_shutdown(e, br_text_shutdown, NULL);
}
