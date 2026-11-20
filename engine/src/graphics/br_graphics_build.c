/* br_graphics_build: turns a context's instructions into triangles (fills by ear clipping
 * with holes, strokes with joins/caps/dashes), cached per context until it changes. */
#include "graphics/br_graphics_internal.h"

/* ---- small geometry helpers */

/* Twice the signed area of triangle o, a, b; > 0 when a -> b turns clockwise on screen. */
static float cross3(br_vec2 o, br_vec2 a, br_vec2 b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static bool same_point(br_vec2 a, br_vec2 b)
{
    return SDL_fabsf(a.x - b.x) < 1e-5f && SDL_fabsf(a.y - b.y) < 1e-5f;
}

/* True if segments a-b and c-d cross at a point inside both (touching does not count). */
static bool segments_cross(br_vec2 a, br_vec2 b, br_vec2 c, br_vec2 d)
{
    float d1 = cross3(a, b, c);
    float d2 = cross3(a, b, d);
    float d3 = cross3(c, d, a);
    float d4 = cross3(c, d, b);
    bool split_cd = (d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f);
    bool split_ab = (d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f);
    return split_cd && split_ab;
}

uint32_t br_gfx_arc_segments(float radius, float sweep)
{
    /* chord error of at most a quarter unit */
    float r = SDL_fabsf(radius);
    float tolerance = 0.25f;
    if (!(r > tolerance)) { /* NaN too */
        return 1;
    }
    float step = 2.0f * SDL_acosf(1.0f - tolerance / r);
    float n = SDL_ceilf(SDL_fabsf(sweep) / step);
    if (!(n >= 1.0f)) { /* NaN too: (uint32_t)NaN is undefined */
        n = 1.0f;
    }
    if (n > 256.0f) {
        n = 256.0f;
    }
    return (uint32_t)n;
}

/* Copies points without consecutive duplicates (and without a closing duplicate). */
static uint32_t clean_ring(const br_vec2 *src, uint32_t count, br_array *out)
{
    br_array_clear(out);
    for (uint32_t i = 0; i < count; i++) {
        if (out->count > 0) {
            br_vec2 *last = br_array_at(out, out->count - 1);
            if (same_point(*last, src[i])) {
                continue;
            }
        }
        br_array_push(out, &src[i]);
    }
    if (out->count > 1) {
        br_vec2 *first = br_array_at(out, 0);
        br_vec2 *last = br_array_at(out, out->count - 1);
        if (same_point(*first, *last)) {
            br_array_pop(out);
        }
    }
    return out->count;
}

static void reverse_points(br_vec2 *p, uint32_t n)
{
    for (uint32_t i = 0; i < n / 2; i++) {
        br_vec2 t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

static float ring_area(const br_vec2 *p, uint32_t n)
{
    br_polygon poly = { p, n };
    return br_polygon_signed_area(poly);
}

/* ---- path helpers */

void br_gfx_path_init(br_gfx_path *p)
{
    br_array_init(&p->points, sizeof(br_vec2));
    br_array_init(&p->subpaths, sizeof(br_gfx_subpath));
}

void br_gfx_path_free(br_gfx_path *p)
{
    br_array_free(&p->points);
    br_array_free(&p->subpaths);
}

void br_gfx_path_copy(br_gfx_path *dst, const br_gfx_path *src)
{
    br_gfx_path_init(dst);
    br_gfx_path_append(dst, src);
}

void br_gfx_path_append(br_gfx_path *dst, const br_gfx_path *src)
{
    uint32_t base = dst->points.count;
    if (src->points.count > 0) {
        br_array_push_n(&dst->points, src->points.data, src->points.count);
    }
    for (uint32_t i = 0; i < src->subpaths.count; i++) {
        br_gfx_subpath s = *(br_gfx_subpath *)br_array_at(&src->subpaths, i);
        s.first += base;
        br_array_push(&dst->subpaths, &s);
    }
}

/* ---- ear clipping with holes */

/* The interior of a clockwise ring at vertex v (neighbours p, n) contains direction v -> m. */
static bool wedge_contains(br_vec2 p, br_vec2 v, br_vec2 n, br_vec2 m)
{
    bool left_of_in = cross3(p, v, m) > 0.0f;   /* inside of edge p -> v */
    bool left_of_out = cross3(v, n, m) > 0.0f;  /* inside of edge v -> n */
    if (cross3(p, v, n) >= 0.0f) {
        return left_of_in && left_of_out; /* convex corner */
    }
    return left_of_in || left_of_out;     /* reflex corner */
}

/* True if p lies on segment a-b, strictly between its ends. */
static bool on_segment_inside(br_vec2 p, br_vec2 a, br_vec2 b)
{
    if (same_point(p, a) || same_point(p, b)) {
        return false;
    }
    br_vec2 ab = br_vec2_sub(b, a);
    br_vec2 ap = br_vec2_sub(p, a);
    float len2 = br_vec2_dot(ab, ab);
    if (len2 <= 0.0f || SDL_fabsf(br_vec2_cross(ab, ap)) > 1e-4f * SDL_sqrtf(len2)) {
        return false;
    }
    float t = br_vec2_dot(ap, ab) / len2;
    return t > 0.0f && t < 1.0f;
}

/* True if segment a-b crosses an edge of ring r or runs through one of its vertices. */
static bool crosses_ring(const br_vec2 *r, uint32_t n, br_vec2 a, br_vec2 b)
{
    for (uint32_t i = 0; i < n; i++) {
        if (segments_cross(a, b, r[i], r[(i + 1) % n]) || on_segment_inside(r[i], a, b)) {
            return true;
        }
    }
    return false;
}

typedef struct br_gfx_hole {
    uint32_t first;   /* into the holes scratch array */
    uint32_t count;
    float    max_x;
} br_gfx_hole;

/* Joins one hole into the ring with a two-way bridge from the hole's rightmost vertex to the
 * nearest ring vertex it can see. False if no bridge was found (hole skipped). */
static bool bridge_hole(br_array *ring, const br_vec2 *hole, uint32_t hole_count, const br_gfx_hole *others,
                        uint32_t other_count, const br_vec2 *all_holes, br_array *tmp)
{
    const br_vec2 *r = ring->data;
    uint32_t n = ring->count;
    uint32_t mi = 0;
    for (uint32_t i = 1; i < hole_count; i++) {
        if (hole[i].x > hole[mi].x) {
            mi = i;
        }
    }
    br_vec2 m = hole[mi];
    /* ponytail: O(ring * edges) visibility search; earcut's ray cast if huge polygons get slow */
    int best = -1;
    float best_d2 = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        br_vec2 v = r[k];
        float dx = v.x - m.x;
        float dy = v.y - m.y;
        float d2 = dx * dx + dy * dy;
        if (best >= 0 && d2 >= best_d2) {
            continue;
        }
        if (!wedge_contains(r[(k + n - 1) % n], v, r[(k + 1) % n], m)) {
            continue;
        }
        if (crosses_ring(r, n, m, v) || crosses_ring(hole, hole_count, m, v)) {
            continue;
        }
        bool blocked = false;
        for (uint32_t h = 0; h < other_count && !blocked; h++) {
            blocked = crosses_ring(all_holes + others[h].first, others[h].count, m, v);
        }
        if (blocked) {
            continue;
        }
        best = (int)k;
        best_d2 = d2;
    }
    if (best < 0) {
        return false;
    }
    /* ring[0..best], hole from m all the way round back to m, ring[best], ring[best+1..] */
    br_array_clear(tmp);
    br_array_push_n(tmp, r, (uint32_t)best + 1);
    for (uint32_t i = 0; i <= hole_count; i++) {
        br_array_push(tmp, &hole[(mi + i) % hole_count]);
    }
    br_array_push_n(tmp, &r[best], n - (uint32_t)best);
    br_array_clear(ring);
    br_array_push_n(ring, tmp->data, tmp->count);
    return true;
}

/* True if no other remaining vertex lies in triangle a, b, c (clockwise). */
static bool is_ear(const br_vec2 *pt, const uint32_t *next, uint32_t ia, uint32_t ib, uint32_t ic)
{
    br_vec2 a = pt[ia];
    br_vec2 b = pt[ib];
    br_vec2 c = pt[ic];
    for (uint32_t j = next[ic]; j != ia; j = next[j]) {
        br_vec2 p = pt[j];
        if (same_point(p, a) || same_point(p, b) || same_point(p, c)) {
            continue; /* bridge duplicates */
        }
        if (cross3(a, b, p) >= 0.0f && cross3(b, c, p) >= 0.0f && cross3(c, a, p) >= 0.0f) {
            return false;
        }
    }
    return true;
}

/* Ear-clips a simple clockwise ring; indices relative to the ring. */
static void ear_clip(const br_vec2 *pt, uint32_t n, br_array *out_indices)
{
    if (n < 3) {
        return;
    }
    uint32_t *prev = br_alloc(sizeof(uint32_t) * n);
    uint32_t *next = br_alloc(sizeof(uint32_t) * n);
    if (prev == NULL || next == NULL) {
        br_free(prev);
        br_free(next);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
    }
    br_rect b = br_polygon_bounds((br_polygon){ pt, n });
    float eps = (b.width * b.width + b.height * b.height) * 1e-9f;
    /* ponytail: about O(n^2) in practice (O(n^3) worst case), hence BR_GFX_MAX_EAR_CLIP_POINTS;
     * z-order hashing like earcut for big polygons */
    uint32_t remaining = n;
    uint32_t i = 0;
    uint32_t misses = 0;
    while (remaining > 3) {
        uint32_t p = prev[i];
        uint32_t q = next[i];
        float c = cross3(pt[p], pt[i], pt[q]);
        bool degenerate = SDL_fabsf(c) <= eps;
        bool ear = !degenerate && c > 0.0f && is_ear(pt, next, p, i, q);
        if (!degenerate && !ear && misses <= remaining) {
            i = q;
            misses++;
            continue;
        }
        /* an ear, a zero-area spike (dropped), or no ear left at all (self-intersecting
         * input: clip anyway so the loop ends) */
        if (!degenerate) {
            uint32_t tri[3] = { p, i, q };
            br_array_push_n(out_indices, tri, 3);
        }
        next[p] = q;
        prev[q] = p;
        remaining--;
        misses = 0;
        i = q;
    }
    uint32_t p = prev[i];
    uint32_t q = next[i];
    if (SDL_fabsf(cross3(pt[p], pt[i], pt[q])) > eps) {
        uint32_t tri[3] = { p, i, q };
        br_array_push_n(out_indices, tri, 3);
    }
    br_free(prev);
    br_free(next);
}

static int compare_holes(const void *a, const void *b)
{
    const br_gfx_hole *ha = a;
    const br_gfx_hole *hb = b;
    if (ha->max_x > hb->max_x) {
        return -1;
    }
    return ha->max_x < hb->max_x ? 1 : 0;
}

/* Collects the holes inside `outer` as counter-clockwise rings, rightmost first. */
static void collect_holes(const br_vec2 *outer, uint32_t outer_count, const br_gfx_path *holes, br_array *hole_points,
                          br_array *hole_list, br_array *tmp)
{
    br_polygon outer_poly = { outer, outer_count };
    for (uint32_t s = 0; holes != NULL && s < holes->subpaths.count; s++) {
        const br_gfx_subpath *sp = br_array_at(&holes->subpaths, s);
        const br_vec2 *hp = br_array_at(&holes->points, sp->first);
        if (sp->count < 3 || !br_polygon_contains(outer_poly, hp[0].x, hp[0].y)) {
            continue;
        }
        if (clean_ring(hp, sp->count, tmp) < 3) {
            continue;
        }
        if (ring_area(tmp->data, tmp->count) > 0.0f) {
            reverse_points(tmp->data, tmp->count);
        }
        br_gfx_hole h;
        h.first = hole_points->count;
        h.count = tmp->count;
        h.max_x = -3.0e38f;
        for (uint32_t i = 0; i < tmp->count; i++) {
            br_vec2 *p = br_array_at(tmp, i);
            if (p->x > h.max_x) {
                h.max_x = p->x;
            }
        }
        br_array_push_n(hole_points, tmp->data, tmp->count);
        br_array_push(hole_list, &h);
    }
    if (hole_list->count > 1) {
        SDL_qsort(hole_list->data, hole_list->count, sizeof(br_gfx_hole), compare_holes);
    }
}

void br_gfx_triangulate(const br_vec2 *outer, uint32_t outer_count, const br_gfx_path *holes, br_array *out_points,
                        br_array *out_indices)
{
    br_array ring;
    br_array hole_points;
    br_array hole_list;
    br_array tmp;
    br_array_init(&ring, sizeof(br_vec2));
    br_array_init(&hole_points, sizeof(br_vec2));
    br_array_init(&hole_list, sizeof(br_gfx_hole));
    br_array_init(&tmp, sizeof(br_vec2));
    uint32_t n = clean_ring(outer, outer_count, &ring);
    if (n >= 3) {
        collect_holes(outer, outer_count, holes, &hole_points, &hole_list, &tmp);
    }
    /* ponytail: ear clipping (and hole bridging) is about O(n^2), so big concave fills are
     * refused; earcut's z-order hash would lift the cap */
    if (n + hole_points.count > BR_GFX_MAX_EAR_CLIP_POINTS) {
        br_log_error("graphics: a concave fill with %u points (max %u) is not filled", n + hole_points.count,
                     BR_GFX_MAX_EAR_CLIP_POINTS);
        n = 0;
    }
    if (n >= 3) {
        if (ring_area(ring.data, ring.count) < 0.0f) {
            reverse_points(ring.data, ring.count);
        }
        for (uint32_t h = 0; h < hole_list.count; h++) {
            const br_gfx_hole *hole = br_array_at(&hole_list, h);
            const br_vec2 *hp = br_array_at(&hole_points, hole->first);
            const br_gfx_hole *rest = hole + 1; /* holes not merged yet */
            uint32_t rest_count = hole_list.count - h - 1;
            if (!bridge_hole(&ring, hp, hole->count, rest, rest_count, hole_points.data, &tmp)) {
                br_log_warn("graphics: could not connect a hole to its shape (skipped)");
            }
        }
        br_array_push_n(out_points, ring.data, ring.count);
        ear_clip(ring.data, ring.count, out_indices);
    }
    br_array_free(&ring);
    br_array_free(&hole_points);
    br_array_free(&hole_list);
    br_array_free(&tmp);
}

/* ---- strokes */

typedef struct br_gfx_out {
    br_array *points;
    br_array *indices;
    uint32_t  base;   /* points->count when this output started */
} br_gfx_out;

static void out_tri(br_gfx_out *o, br_vec2 a, br_vec2 b, br_vec2 c)
{
    uint32_t i = o->points->count - o->base;
    uint32_t tri[3] = { i, i + 1, i + 2 };
    br_array_push(o->points, &a);
    br_array_push(o->points, &b);
    br_array_push(o->points, &c);
    br_array_push_n(o->indices, tri, 3);
}

static void out_quad(br_gfx_out *o, br_vec2 a, br_vec2 b, br_vec2 c, br_vec2 d)
{
    uint32_t i = o->points->count - o->base;
    uint32_t idx[6] = { i, i + 1, i + 2, i, i + 2, i + 3 };
    br_array_push(o->points, &a);
    br_array_push(o->points, &b);
    br_array_push(o->points, &c);
    br_array_push(o->points, &d);
    br_array_push_n(o->indices, idx, 6);
}

/* Triangle fan around c from offset `from` to offset `to` (same length), bulging toward `bulge`. */
static void out_round(br_gfx_out *o, br_vec2 c, br_vec2 from, br_vec2 to, br_vec2 bulge)
{
    float radius = br_vec2_length(from);
    float a1 = SDL_atan2f(from.y, from.x);
    float delta = SDL_atan2f(to.y, to.x) - a1;
    while (delta > BR_PI) {
        delta -= BR_TAU;
    }
    while (delta < -BR_PI) {
        delta += BR_TAU;
    }
    float mid = a1 + delta * 0.5f;
    if (SDL_cosf(mid) * bulge.x + SDL_sinf(mid) * bulge.y < 0.0f) {
        delta = delta > 0.0f ? delta - BR_TAU : delta + BR_TAU;
    }
    uint32_t steps = br_gfx_arc_segments(radius, delta);
    br_vec2 last = br_vec2_add(c, from);
    for (uint32_t s = 1; s <= steps; s++) {
        float a = a1 + delta * (float)s / (float)steps;
        br_vec2 p = br_vec2_make(c.x + SDL_cosf(a) * radius, c.y + SDL_sinf(a) * radius);
        out_tri(o, c, last, p);
        last = p;
    }
}

/* Outward normal of direction d (sgn flips it for counter-clockwise rings). */
static br_vec2 stroke_normal(br_vec2 d, float sgn)
{
    return br_vec2_scale(br_vec2_perp(d), -sgn);
}

static void stroke_cap(br_gfx_out *o, const br_stroke_style *st, br_vec2 p, br_vec2 d, float lo, float hi, bool end)
{
    br_vec2 n = stroke_normal(d, 1.0f);
    float half = (hi - lo) * 0.5f;
    br_vec2 out_dir = end ? d : br_vec2_scale(d, -1.0f);
    if (st->cap == BR_GRAPHICS_CAP_SQUARE) {
        br_vec2 ext = br_vec2_scale(out_dir, half);
        br_vec2 a = br_vec2_add(p, br_vec2_scale(n, lo));
        br_vec2 b = br_vec2_add(p, br_vec2_scale(n, hi));
        out_quad(o, a, br_vec2_add(a, ext), br_vec2_add(b, ext), b);
    } else if (st->cap == BR_GRAPHICS_CAP_ROUND) {
        br_vec2 c = br_vec2_add(p, br_vec2_scale(n, (lo + hi) * 0.5f));
        out_round(o, c, br_vec2_scale(n, half), br_vec2_scale(n, -half), out_dir);
    }
}

/* How the stroke turns at one vertex. The inner side of a turn shares one mitred point
 * between both segments (no overlap, so translucent strokes blend once); the outer side
 * gets the join. */
typedef struct br_gfx_corner {
    bool    join;        /* the path turns here */
    bool    shared;      /* both segments end at `inner` on the inner side */
    bool    gap_is_lo;   /* the join opens on the lo side */
    br_vec2 inner;
} br_gfx_corner;

static br_gfx_corner stroke_corner(br_vec2 before, br_vec2 at, br_vec2 after, float lo, float hi, float sgn)
{
    br_gfx_corner c;
    SDL_zero(c);
    br_vec2 d1 = br_vec2_normalize(br_vec2_sub(at, before));
    br_vec2 d2 = br_vec2_normalize(br_vec2_sub(after, at));
    if (SDL_fabsf(br_vec2_cross(d1, d2)) < 1e-6f && br_vec2_dot(d1, d2) > 0.0f) {
        return c; /* straight on */
    }
    br_vec2 n1 = stroke_normal(d1, sgn);
    br_vec2 n2 = stroke_normal(d2, sgn);
    c.join = true;
    c.gap_is_lo = br_vec2_dot(d2, n1) > 0.0f; /* turning towards +n opens the -n side */
    float inner = c.gap_is_lo ? hi : lo;
    br_vec2 m = br_vec2_normalize(br_vec2_add(n1, n2));
    float cosine = br_vec2_dot(m, n1);
    float reach = SDL_min(br_vec2_distance(before, at), br_vec2_distance(at, after));
    /* a very sharp turn on short segments would push the shared point past the segments */
    if (cosine > 1e-3f && SDL_fabsf(inner / cosine) <= reach) {
        c.shared = true;
        c.inner = br_vec2_add(at, br_vec2_scale(m, inner / cosine));
    }
    return c;
}

/* Where a segment with normal n ends at vertex p on the side at offset `off`. */
static br_vec2 stroke_end(const br_gfx_corner *c, br_vec2 p, br_vec2 n, float off, bool side_is_lo)
{
    if (c->join && c->shared && c->gap_is_lo != side_is_lo) {
        return c->inner;
    }
    return br_vec2_add(p, br_vec2_scale(n, off));
}

static void stroke_join(br_gfx_out *o, const br_stroke_style *st, const br_gfx_corner *c, br_vec2 before, br_vec2 p,
                        br_vec2 after, float lo, float hi, float sgn)
{
    br_vec2 d1 = br_vec2_normalize(br_vec2_sub(p, before));
    br_vec2 d2 = br_vec2_normalize(br_vec2_sub(after, p));
    br_vec2 n1 = stroke_normal(d1, sgn);
    br_vec2 n2 = stroke_normal(d2, sgn);
    float e = c->gap_is_lo ? lo : hi;
    br_vec2 pivot = c->shared ? c->inner : p;
    if (SDL_fabsf(e) < 1e-6f) {
        return; /* alignment puts the whole stroke on the inner side */
    }
    br_vec2 a = br_vec2_add(p, br_vec2_scale(n1, e));
    br_vec2 b = br_vec2_add(p, br_vec2_scale(n2, e));
    if (st->join == BR_GRAPHICS_JOIN_ROUND) {
        if (c->shared) {
            out_tri(o, pivot, a, p);
            out_tri(o, pivot, p, b);
        }
        out_round(o, p, br_vec2_scale(n1, e), br_vec2_scale(n2, e), br_vec2_sub(d1, d2));
        return;
    }
    if (st->join == BR_GRAPHICS_JOIN_MITER) {
        float limit = st->miter_limit > 0.0f ? st->miter_limit : 10.0f;
        br_vec2 m = br_vec2_normalize(br_vec2_add(n1, n2));
        float cosine = br_vec2_dot(m, n1);
        if (cosine > 1e-3f && 1.0f / cosine <= limit) {
            br_vec2 tip = br_vec2_add(p, br_vec2_scale(m, e / cosine));
            out_tri(o, pivot, a, tip);
            out_tri(o, pivot, tip, b);
            return;
        }
    }
    out_tri(o, pivot, a, b);
}

void br_gfx_stroke_polyline(const br_vec2 *points, uint32_t count, bool closed, const br_stroke_style *style,
                            br_array *out_points, br_array *out_indices)
{
    br_array ring;
    br_array_init(&ring, sizeof(br_vec2));
    uint32_t n = clean_ring(points, count, &ring);
    if (!closed && n >= 2 && same_point(points[0], points[count - 1])) {
        br_array_push(&ring, &points[0]); /* an open path that ends where it started */
        n = ring.count;
    }
    if (n < 2 || !(style->width > 0.0f)) {
        br_array_free(&ring);
        return;
    }
    if (n < 3) {
        closed = false;
    }
    br_gfx_corner *corners = br_alloc(sizeof(br_gfx_corner) * n);
    if (corners == NULL) {
        br_array_free(&ring);
        return;
    }
    const br_vec2 *p = ring.data;
    br_gfx_out o = { out_points, out_indices, out_points->count };
    float w = style->width;
    float align = closed ? br_clampf(style->alignment, 0.0f, 1.0f) : 0.5f;
    float lo = -w * (1.0f - align);
    float hi = w * align;
    float sgn = 1.0f;
    if (closed && ring_area(p, n) < 0.0f) {
        sgn = -1.0f;
    }
    for (uint32_t i = 0; i < n; i++) {
        SDL_zero(corners[i]);
        if (closed || (i > 0 && i + 1 < n)) {
            corners[i] = stroke_corner(p[(i + n - 1) % n], p[i], p[(i + 1) % n], lo, hi, sgn);
        }
    }
    uint32_t segs = closed ? n : n - 1;
    for (uint32_t i = 0; i < segs; i++) {
        uint32_t j = (i + 1) % n;
        br_vec2 nn = stroke_normal(br_vec2_normalize(br_vec2_sub(p[j], p[i])), sgn);
        out_quad(&o, stroke_end(&corners[i], p[i], nn, lo, true), stroke_end(&corners[j], p[j], nn, lo, true),
                 stroke_end(&corners[j], p[j], nn, hi, false), stroke_end(&corners[i], p[i], nn, hi, false));
    }
    for (uint32_t i = 0; i < n; i++) {
        if (corners[i].join) {
            stroke_join(&o, style, &corners[i], p[(i + n - 1) % n], p[i], p[(i + 1) % n], lo, hi, sgn);
        }
    }
    if (!closed) {
        stroke_cap(&o, style, p[0], br_vec2_normalize(br_vec2_sub(p[1], p[0])), lo, hi, false);
        stroke_cap(&o, style, p[n - 1], br_vec2_normalize(br_vec2_sub(p[n - 1], p[n - 2])), lo, hi, true);
    }
    br_free(corners);
    br_array_free(&ring);
}

/* ---- dashes */

static void dash_point(br_gfx_path *out, br_vec2 p, bool start)
{
    if (start) {
        br_gfx_subpath s = { out->points.count, 0, false, false };
        br_array_push(&out->subpaths, &s);
    }
    br_gfx_subpath *last = br_array_at(&out->subpaths, out->subpaths.count - 1);
    br_array_push(&out->points, &p);
    last->count++;
}

void br_gfx_dash_polyline(const br_vec2 *points, uint32_t count, bool closed, const br_stroke_style *style,
                          br_gfx_path *out)
{
    float dash[BR_GRAPHICS_MAX_DASHES * 2];
    uint32_t m = style->dash_count > BR_GRAPHICS_MAX_DASHES ? BR_GRAPHICS_MAX_DASHES : style->dash_count;
    float total = 0.0f;
    for (uint32_t i = 0; i < m; i++) {
        dash[i] = SDL_fabsf(style->dash[i]);
        total += dash[i];
    }
    if (m % 2 == 1) { /* SVG: an odd list is repeated to make it even */
        for (uint32_t i = 0; i < m; i++) {
            dash[m + i] = dash[i];
        }
        m *= 2;
        total *= 2.0f;
    }
    if (count < 2 || !(total > 1e-4f) || !br_gfx_finite(total)) {
        return;
    }
    float offset = SDL_fmodf(br_gfx_finite(style->dash_offset) ? style->dash_offset : 0.0f, total);
    if (offset < 0.0f) {
        offset += total;
    }
    uint32_t idx = 0;
    while (offset >= dash[idx]) {
        offset -= dash[idx];
        idx = (idx + 1) % m;
    }
    /* distances in double: a float `t` stops growing once t + rem == t (a hang on long lines) */
    double rem = (double)dash[idx] - (double)offset;
    bool on = idx % 2 == 0;
    if (on) {
        dash_point(out, points[0], true);
    }
    uint32_t segs = closed ? count : count - 1;
    for (uint32_t i = 0; i < segs; i++) {
        br_vec2 a = points[i];
        br_vec2 b = points[(i + 1) % count];
        double len = (double)br_vec2_distance(a, b);
        if (!(len > 0.0) || !br_gfx_finite((float)len)) {
            continue;
        }
        br_vec2 d = br_vec2_scale(br_vec2_sub(b, a), (float)(1.0 / len));
        double t = 0.0;
        while (len - t > rem) {
            /* each pass adds one point: the cap bounds time and memory per stroke */
            if (out->points.count >= 2u * BR_GFX_MAX_DASH_PIECES) {
                br_log_error("graphics: a dashed stroke needs more than %u dash pieces; the rest is not drawn",
                             BR_GFX_MAX_DASH_PIECES);
                return;
            }
            t += rem;
            br_vec2 p = br_vec2_add(a, br_vec2_scale(d, (float)t));
            dash_point(out, p, !on);
            on = !on;
            idx = (idx + 1) % m;
            rem = dash[idx];
        }
        rem -= len - t;
        if (on) {
            dash_point(out, b, false);
        }
    }
}

/* ---- building the cache */

static br_vec2 fill_uv(const br_graphics_context *ctx, const br_fill_style *fill, br_rect shape, br_vec2 p)
{
    const br_fill_gradient *g = &fill->gradient;
    br_vec2 q = p;
    if (fill->kind != BR_GRAPHICS_FILL_PATTERN && g->space == BR_GRAPHICS_SPACE_LOCAL) {
        q.x = shape.width > 0.0f ? (p.x - shape.x) / shape.width : 0.0f;
        q.y = shape.height > 0.0f ? (p.y - shape.y) / shape.height : 0.0f;
    }
    if (fill->kind == BR_GRAPHICS_FILL_LINEAR) {
        br_vec2 axis = br_vec2_sub(g->end, g->start);
        float len2 = br_vec2_dot(axis, axis);
        float u = len2 > 0.0f ? br_vec2_dot(br_vec2_sub(q, g->start), axis) / len2 : 0.0f;
        return br_vec2_make(u, 0.5f);
    }
    if (fill->kind == BR_GRAPHICS_FILL_RADIAL) {
        float r = g->outer_radius > 0.0f ? g->outer_radius : 1.0f;
        return br_vec2_make((q.x - g->center.x) / (2.0f * r) + 0.5f, (q.y - g->center.y) / (2.0f * r) + 0.5f);
    }
    if (fill->kind == BR_GRAPHICS_FILL_PATTERN) {
        br_texture_info info;
        br_texture_get_info(ctx->e, fill->pattern.texture, &info);
        br_vec2 t = br_mat3_apply_inverse(fill->pattern.matrix, p);
        float w = info.source_width > 0 ? (float)info.source_width : 1.0f;
        float h = info.source_height > 0 ? (float)info.source_height : 1.0f;
        return br_vec2_make(t.x / w, t.y / h);
    }
    return br_vec2_make(0.5f, 0.5f);
}

static void grow_bounds(br_graphics_context *ctx, br_vec2 p)
{
    if (!ctx->has_bounds) {
        ctx->bounds = br_rect_make(p.x, p.y, 0.0f, 0.0f);
        ctx->has_bounds = true;
        return;
    }
    ctx->bounds = br_rect_union(ctx->bounds, br_rect_make(p.x, p.y, 0.0f, 0.0f));
}

/* The group to append `count` vertices with `texture` to (a new one if needed). */
static br_gfx_group *group_for(br_graphics_context *ctx, br_texture texture, uint32_t count)
{
    if (ctx->groups.count > 0) {
        br_gfx_group *g = br_array_at(&ctx->groups, ctx->groups.count - 1);
        bool same = g->texture.index == texture.index && g->texture.generation == texture.generation;
        if (same && g->line_count == 0 && g->vertex_count + count <= BR_RENDER_MAX_BATCH_VERTICES) {
            return g;
        }
    }
    br_gfx_group g;
    SDL_zero(g);
    g.texture = texture;
    g.first_vertex = ctx->vertices.count;
    g.vertex_count = 0;
    g.first_index = ctx->indices.count;
    g.index_count = 0;
    return br_array_push(&ctx->groups, &g);
}

/* The pixel-line group at the end of the group list (a new one after triangles), so lines keep
 * their place between the triangles of earlier and later instructions. */
static br_gfx_group *line_group(br_graphics_context *ctx)
{
    if (ctx->groups.count > 0) {
        br_gfx_group *g = br_array_at(&ctx->groups, ctx->groups.count - 1);
        if (g->line_count > 0) {
            return g;
        }
    }
    br_gfx_group g;
    SDL_zero(g);
    g.first_vertex = ctx->vertices.count;
    g.first_index = ctx->indices.count;
    g.first_line = ctx->lines.count;
    return br_array_push(&ctx->groups, &g);
}

static void emit_vertex(br_graphics_context *ctx, br_gfx_group *g, br_vec2 p, br_vec2 uv, uint32_t color)
{
    br_vertex2d v = { p.x, p.y, uv.x, uv.y, color };
    br_array_push(&ctx->vertices, &v);
    g->vertex_count++;
    grow_bounds(ctx, p);
}

/* Appends triangles (points + uint32 indices) to the cache with uvs from the fill style. */
static void emit(br_graphics_context *ctx, br_texture texture, const br_fill_style *fill, br_rect shape,
                 const br_array *points, const br_array *indices, const br_vec2 *fixed_uvs)
{
    uint32_t color = br_render_pack_color(fill->color, 1.0f);
    const br_vec2 *pt = points->data;
    const uint32_t *idx = indices->data;
    if (points->count == 0 || indices->count == 0) {
        return;
    }
    if (points->count <= BR_RENDER_MAX_BATCH_VERTICES) {
        br_gfx_group *g = group_for(ctx, texture, points->count);
        uint32_t base = g->vertex_count;
        for (uint32_t i = 0; i < points->count; i++) {
            br_vec2 uv = fixed_uvs != NULL ? fixed_uvs[i] : fill_uv(ctx, fill, shape, pt[i]);
            emit_vertex(ctx, g, pt[i], uv, color);
        }
        for (uint32_t i = 0; i < indices->count; i++) {
            uint16_t v = (uint16_t)(base + idx[i]);
            br_array_push(&ctx->indices, &v);
        }
        g->index_count += indices->count;
        return;
    }
    /* too many shared vertices for one 16-bit batch: copy triangle by triangle */
    for (uint32_t t = 0; t + 2 < indices->count; t += 3) {
        br_gfx_group *g = group_for(ctx, texture, 3);
        for (uint32_t k = 0; k < 3; k++) {
            br_vec2 p = pt[idx[t + k]];
            uint16_t v = (uint16_t)g->vertex_count;
            emit_vertex(ctx, g, p, fill_uv(ctx, fill, shape, p), color);
            br_array_push(&ctx->indices, &v);
        }
        g->index_count += 3;
    }
}

static br_rect path_bounds(const br_gfx_path *path)
{
    br_polygon poly = { path->points.data, path->points.count };
    return br_polygon_bounds(poly);
}

static void build_fill(br_graphics_context *ctx, const br_gfx_instruction *in, br_array *pts, br_array *idx)
{
    bool has_holes = in->hole.subpaths.count > 0;
    br_array_clear(pts);
    br_array_clear(idx);
    for (uint32_t s = 0; s < in->path.subpaths.count; s++) {
        const br_gfx_subpath *sp = br_array_at(&in->path.subpaths, s);
        const br_vec2 *p = br_array_at(&in->path.points, sp->first);
        if (sp->count < 3) {
            continue;
        }
        uint32_t base = pts->count;
        uint32_t first_index = idx->count;
        if (sp->convex && !has_holes) {
            /* dedicated builder for rect / circle / ellipse / rounded rect / regular polygons */
            br_array_push_n(pts, p, sp->count);
            for (uint32_t i = 1; i + 1 < sp->count; i++) {
                uint32_t tri[3] = { 0, i, i + 1 };
                br_array_push_n(idx, tri, 3);
            }
        } else {
            br_gfx_triangulate(p, sp->count, has_holes ? &in->hole : NULL, pts, idx);
        }
        for (uint32_t i = first_index; i < idx->count; i++) {
            uint32_t *v = br_array_at(idx, i);
            *v += base;
        }
    }
    emit(ctx, in->texture, &in->style.fill, path_bounds(&in->path), pts, idx, NULL);
}

static void stroke_path(br_graphics_context *ctx, const br_gfx_instruction *in, const br_gfx_path *path, br_array *pts,
                        br_array *idx)
{
    const br_stroke_style *st = &in->style;
    for (uint32_t s = 0; s < path->subpaths.count; s++) {
        const br_gfx_subpath *sp = br_array_at(&path->subpaths, s);
        const br_vec2 *p = br_array_at(&path->points, sp->first);
        if (st->pixel_line) {
            uint32_t color = br_render_pack_color(st->fill.color, 1.0f);
            uint32_t segs = sp->closed ? sp->count : sp->count - 1;
            for (uint32_t i = 0; sp->count >= 2 && i < segs; i++) {
                br_vec2 a = p[i];
                br_vec2 b = p[(i + 1) % sp->count];
                br_vertex2d va = { a.x, a.y, 0.5f, 0.5f, color };
                br_vertex2d vb = { b.x, b.y, 0.5f, 0.5f, color };
                br_gfx_group *lines = line_group(ctx);
                if (lines == NULL) {
                    break;
                }
                lines->line_count += 2;
                br_array_push(&ctx->lines, &va);
                br_array_push(&ctx->lines, &vb);
                grow_bounds(ctx, a);
                grow_bounds(ctx, b);
            }
            continue;
        }
        uint32_t base = pts->count;
        uint32_t first_index = idx->count;
        br_gfx_stroke_polyline(p, sp->count, sp->closed, st, pts, idx);
        for (uint32_t i = first_index; i < idx->count; i++) {
            uint32_t *v = br_array_at(idx, i);
            *v += base;
        }
    }
}

static void build_stroke(br_graphics_context *ctx, const br_gfx_instruction *in, br_array *pts, br_array *idx)
{
    br_gfx_path all;
    br_gfx_path_copy(&all, &in->path);
    br_gfx_path_append(&all, &in->hole); /* holes get their outline stroked too */
    br_array_clear(pts);
    br_array_clear(idx);
    if (in->style.dash_count > 0) {
        br_gfx_path dashed;
        br_gfx_path_init(&dashed);
        for (uint32_t s = 0; s < all.subpaths.count; s++) {
            const br_gfx_subpath *sp = br_array_at(&all.subpaths, s);
            br_array ring;
            br_array_init(&ring, sizeof(br_vec2));
            uint32_t n = clean_ring(br_array_at(&all.points, sp->first), sp->count, &ring);
            br_gfx_dash_polyline(ring.data, n, sp->closed && n >= 3, &in->style, &dashed);
            br_array_free(&ring);
        }
        stroke_path(ctx, in, &dashed, pts, idx);
        br_gfx_path_free(&dashed);
    } else {
        stroke_path(ctx, in, &all, pts, idx);
    }
    emit(ctx, in->texture, &in->style.fill, path_bounds(&in->path), pts, idx, NULL);
    br_gfx_path_free(&all);
}

static void build_texture(br_graphics_context *ctx, const br_gfx_instruction *in, br_array *pts, br_array *idx)
{
    br_texture_info info;
    br_texture_get_info(ctx->e, in->texture, &info);
    br_array_clear(pts);
    br_array_clear(idx);
    br_array_push_n(pts, in->quad, 4);
    uint32_t quad[6] = { 0, 1, 2, 0, 2, 3 };
    br_array_push_n(idx, quad, 6);
    emit(ctx, in->texture, &in->style.fill, br_rect_make(0, 0, 0, 0), pts, idx, info.uvs);
}

void br_gfx_build(br_graphics_context *ctx)
{
    if (ctx->built_version == ctx->version) {
        return;
    }
    br_array_clear(&ctx->vertices);
    br_array_clear(&ctx->indices);
    br_array_clear(&ctx->groups);
    br_array_clear(&ctx->lines);
    ctx->has_bounds = false;
    ctx->bounds = br_rect_make(0, 0, 0, 0);
    br_array pts;
    br_array idx;
    br_array_init(&pts, sizeof(br_vec2));
    br_array_init(&idx, sizeof(uint32_t));
    for (uint32_t i = 0; i < ctx->instructions.count; i++) {
        const br_gfx_instruction *in = br_array_at(&ctx->instructions, i);
        if (in->kind == BR_GFX_FILL) {
            build_fill(ctx, in, &pts, &idx);
        } else if (in->kind == BR_GFX_STROKE) {
            build_stroke(ctx, in, &pts, &idx);
        } else {
            build_texture(ctx, in, &pts, &idx);
        }
    }
    br_array_free(&pts);
    br_array_free(&idx);
    ctx->built_version = ctx->version;
}

/* ---- queries */

void br_graphics_context_get_geometry(br_graphics_context *ctx, br_graphics_geometry_info *out)
{
    SDL_zerop(out);
    if (ctx == NULL) {
        return;
    }
    br_gfx_build(ctx);
    out->vertex_count = ctx->vertices.count;
    out->triangle_count = ctx->indices.count / 3;
    out->line_count = ctx->lines.count / 2;
    out->bounds = ctx->bounds;
    const br_vertex2d *v = ctx->vertices.data;
    const uint16_t *idx = ctx->indices.data;
    for (uint32_t g = 0; g < ctx->groups.count; g++) {
        const br_gfx_group *grp = br_array_at(&ctx->groups, g);
        for (uint32_t i = 0; i + 2 < grp->index_count; i += 3) {
            const br_vertex2d *a = &v[grp->first_vertex + idx[grp->first_index + i]];
            const br_vertex2d *b = &v[grp->first_vertex + idx[grp->first_index + i + 1]];
            const br_vertex2d *c = &v[grp->first_vertex + idx[grp->first_index + i + 2]];
            float area2 = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
            out->area += SDL_fabsf(area2) * 0.5f;
        }
    }
}

bool br_graphics_context_contains(br_graphics_context *ctx, br_vec2 p)
{
    if (ctx == NULL) {
        return false;
    }
    br_gfx_build(ctx);
    if (!ctx->has_bounds || !br_rect_contains(br_rect_pad(ctx->bounds, 1e-3f), p.x, p.y)) {
        return false;
    }
    /* ponytail: linear scan of the cached triangles; a grid if hit tests get hot */
    const br_vertex2d *v = ctx->vertices.data;
    const uint16_t *idx = ctx->indices.data;
    for (uint32_t g = 0; g < ctx->groups.count; g++) {
        const br_gfx_group *grp = br_array_at(&ctx->groups, g);
        for (uint32_t i = 0; i + 2 < grp->index_count; i += 3) {
            const br_vertex2d *a = &v[grp->first_vertex + idx[grp->first_index + i]];
            const br_vertex2d *b = &v[grp->first_vertex + idx[grp->first_index + i + 1]];
            const br_vertex2d *c = &v[grp->first_vertex + idx[grp->first_index + i + 2]];
            br_triangle t = { a->x, a->y, b->x, b->y, c->x, c->y };
            if (br_triangle_contains(t, p.x, p.y)) {
                return true;
            }
        }
    }
    return false;
}
