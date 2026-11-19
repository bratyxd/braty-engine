/* br_debug_profiler: the profiler panel (F2) and the corner readout, built with microui from
 * the profiler's frame ring (engine/src/profiler). Under --deterministic every time shown is a
 * fixed placeholder, so golden images do not depend on how fast the machine is. */
#include "debug/br_debug_internal.h"

#define BR_PROF_GRAPH_MAX_MS 40.0f   /* graph scale: the 16.6 / 33.3 ms guides stay visible */
#define BR_PROF_SPARK_FRAMES 120
#define BR_PROF_MAX_ROWS 200
#define BR_PROF_ASSET_ROWS 256
#define BR_PROF_ASSET_TYPES 16

static const int br_prof_full_row[1] = { -1 };

/* A colour per scope name for the stacked bar. */
static mu_Color br_prof_color(uint32_t id)
{
    static const mu_Color palette[8] = {
        { 90, 160, 230, 255 }, { 230, 150, 70, 255 },  { 110, 200, 120, 255 }, { 200, 100, 200, 255 },
        { 220, 200, 80, 255 }, { 100, 200, 200, 255 }, { 220, 100, 100, 255 }, { 150, 150, 160, 255 },
    };
    return palette[id % 8];
}

static float br_prof_ms(uint64_t ns)
{
    return (float)((double)ns / 1e6);
}

/* The frame the panel shows: the one picked while paused, else the newest with a known interval. */
static uint32_t br_prof_selected_age(br_engine *e, br_debug_state *s)
{
    uint32_t count = br_profiler_frame_count(e);
    if (count == 0) {
        return 0;
    }
    if (!br_profiler_paused(e)) {
        s->profiler_age = count > 1 ? 1 : 0;
    }
    if (s->profiler_age >= count) {
        s->profiler_age = count - 1;
    }
    return s->profiler_age;
}

/* ---- header: rates and the verdict */

static void br_prof_header(br_engine *e, mu_Context *ui)
{
    br_profiler_summary_info sum;
    br_profiler_get_summary(e, &sum);
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    if (br_app_deterministic(e)) {
        mu_label(ui, "FPS 60.0   frame 16.67 ms   CPU 4.00 ms   GPU 2.00 ms (est.)");
        mu_label(ui, "vsync limited   top: (placeholders under --deterministic)");
        return;
    }
    char gpu[64];
    if (sum.gpu_ms <= 0.0f) {
        SDL_snprintf(gpu, sizeof(gpu), "GPU n/a");
    } else if (sum.gpu_includes_vsync) {
        SDL_snprintf(gpu, sizeof(gpu), "GPU <= %.2f ms (est., incl. vsync)", (double)sum.gpu_ms);
    } else {
        SDL_snprintf(gpu, sizeof(gpu), "GPU %.2f ms (est.)", (double)sum.gpu_ms);
    }
    br_debug_labelf(ui, "FPS %.1f   frame %.2f ms   CPU %.2f ms   %s", (double)sum.fps, (double)sum.frame_ms,
                    (double)sum.cpu_ms, gpu);
    br_debug_labelf(ui, "%s   top: %s %.2f  %s %.2f  %s %.2f", br_profiler_verdict_name(sum.verdict), sum.top[0],
                    (double)sum.top_ms[0], sum.top[1], (double)sum.top_ms[1], sum.top[2], (double)sum.top_ms[2]);
}

/* ---- controls: pause, step, live, tabs */

static void br_prof_controls(br_engine *e, br_debug_state *s, uint32_t age)
{
    mu_Context *ui = s->ui;
    static const int row[8] = { 60, 26, 26, 40, 66, 70, 66, -1 };
    mu_layout_row(ui, 8, row, 0);
    bool paused = br_profiler_paused(e);
    if (mu_button(ui, paused ? "Resume" : "Pause")) {
        br_profiler_set_paused(e, !paused);
    }
    uint32_t count = br_profiler_frame_count(e);
    if (mu_button(ui, "<") && age + 1 < count) {
        br_profiler_set_paused(e, true);
        s->profiler_age = age + 1;
    }
    if (mu_button(ui, ">") && age > 0) {
        br_profiler_set_paused(e, true);
        s->profiler_age = age - 1;
    }
    if (mu_button(ui, "Live")) {
        br_profiler_set_paused(e, false);
    }
    static const char *tabs[3] = { "Scopes", "Counters", "Memory" };
    for (int i = 0; i < 3; i++) {
        char label[24];
        SDL_snprintf(label, sizeof(label), s->profiler_tab == i ? "[%s]" : "%s", tabs[i]);
        mu_push_id(ui, &i, sizeof(i));
        if (mu_button(ui, label)) {
            s->profiler_tab = i;
        }
        mu_pop_id(ui);
    }
    br_profiler_frame_info info;
    if (br_app_deterministic(e)) {
        mu_label(ui, "frame #-");
    } else if (br_profiler_get_frame(e, age, &info)) {
        br_debug_labelf(ui, "frame #%llu%s", (unsigned long long)info.number, paused ? " (paused)" : "");
    } else {
        mu_label(ui, "no frames yet");
    }
}

/* ---- frame-time graph: interval bars, CPU (green) and GPU (orange) marks, click to inspect */

static int br_prof_graph_y(mu_Rect r, float ms)
{
    float t = br_clampf(ms / BR_PROF_GRAPH_MAX_MS, 0.0f, 1.0f);
    return r.y + r.h - (int)(t * (float)r.h);
}

static void br_prof_graph(br_engine *e, br_debug_state *s, uint32_t age)
{
    mu_Context *ui = s->ui;
    br_profiler_state *p = e->profiler;
    bool det = br_app_deterministic(e);
    mu_layout_row(ui, 1, br_prof_full_row, 90);
    mu_Rect r = mu_layout_next(ui);
    mu_draw_rect(ui, r, mu_color(15, 16, 20, 255));
    uint32_t count = br_profiler_frame_count(e);
    for (uint32_t a = 0; a < BR_PROFILER_FRAMES; a++) {
        float interval, cpu, gpu;
        if (det) {
            interval = 16.67f;
            cpu = 4.0f + (float)(a % 10) * 0.2f;
            gpu = 2.0f;
        } else if (a < count) {
            br_profiler_frame *f = br_profiler_frame_at(p, a);
            uint64_t wall = f->end_ns - f->start_ns;
            uint64_t waits = f->acquire_ns + f->present_ns + f->pace_ns;
            interval = br_prof_ms(f->interval_ns != 0 ? f->interval_ns : wall);
            cpu = br_prof_ms(wall > waits ? wall - waits : 0);
            gpu = br_prof_ms(f->gpu_ns);
        } else {
            continue;
        }
        int x0 = r.x + (int)((int64_t)(BR_PROFILER_FRAMES - 1 - a) * r.w / BR_PROFILER_FRAMES);
        int x1 = r.x + (int)((int64_t)(BR_PROFILER_FRAMES - a) * r.w / BR_PROFILER_FRAMES);
        int w = x1 - x0 > 0 ? x1 - x0 : 1;
        int top = br_prof_graph_y(r, interval);
        mu_Color bar = a == age ? mu_color(200, 200, 210, 255) : mu_color(60, 64, 76, 255);
        mu_draw_rect(ui, mu_rect(x0, top, w, r.y + r.h - top), bar);
        mu_draw_rect(ui, mu_rect(x0, br_prof_graph_y(r, cpu) - 1, w, 2), mu_color(90, 210, 120, 255));
        if (gpu > 0.0f) {
            mu_draw_rect(ui, mu_rect(x0, br_prof_graph_y(r, gpu) - 1, w, 2), mu_color(240, 150, 60, 255));
        }
    }
    /* 16.6 ms (60 Hz) and 33.3 ms (30 Hz) guides */
    mu_draw_rect(ui, mu_rect(r.x, br_prof_graph_y(r, 16.6f), r.w, 1), mu_color(220, 220, 220, 110));
    mu_draw_rect(ui, mu_rect(r.x, br_prof_graph_y(r, 33.3f), r.w, 1), mu_color(220, 120, 120, 110));
    /* clicking a column pauses on that frame */
    if ((ui->mouse_pressed & MU_MOUSE_LEFT) != 0 && mu_mouse_over(ui, r) && count > 0) {
        int column = (ui->mouse_pos.x - r.x) * BR_PROFILER_FRAMES / (r.w > 0 ? r.w : 1);
        int picked = BR_PROFILER_FRAMES - 1 - column;
        if (picked >= 0 && (uint32_t)picked < count) {
            br_profiler_set_paused(e, true);
            s->profiler_age = (uint32_t)picked;
        }
    }
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    mu_label(ui, "bars: frame interval   green: CPU   orange: GPU (est.)   lines: 16.6 / 33.3 ms");
}

/* ---- stacked bar: the selected frame split by top-level scope */

static void br_prof_stacked(br_engine *e, br_debug_state *s, br_profiler_frame *f)
{
    mu_Context *ui = s->ui;
    br_profiler_state *p = e->profiler;
    bool det = br_app_deterministic(e);
    mu_layout_row(ui, 1, br_prof_full_row, 14);
    mu_Rect r = mu_layout_next(ui);
    mu_draw_rect(ui, r, mu_color(15, 16, 20, 255));
    const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
    uint64_t span = f->interval_ns != 0 ? f->interval_ns : f->end_ns - f->start_ns;
    char legend[256] = "";
    uint32_t tops = 0;
    for (uint32_t i = 0; i < f->scope_count && span > 0; i++) {
        if (scopes[i].parent != BR_PROFILER_NONE) {
            continue;
        }
        tops += 1;
        float start = det ? (float)(tops - 1) / 6.0f : (float)scopes[i].start / (float)span;
        float width = det ? 1.0f / 6.0f : (float)(scopes[i].end - scopes[i].start) / (float)span;
        int x0 = r.x + (int)(br_clampf(start, 0.0f, 1.0f) * (float)r.w);
        int w = (int)(br_clampf(width, 0.0f, 1.0f) * (float)r.w);
        mu_draw_rect(ui, mu_rect(x0, r.y, w > 0 ? w : 1, r.h), br_prof_color(scopes[i].name));
        size_t used = SDL_strlen(legend);
        SDL_snprintf(legend + used, sizeof(legend) - used, "%s%s %.2f", used > 0 ? "  " : "",
                     br_profiler_name(p, scopes[i].name), det ? 1.0 : (double)br_prof_ms(scopes[i].end - scopes[i].start));
    }
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    mu_label(ui, legend);
}

/* ---- scope tree */

struct br_prof_rows {
    float    total[BR_PROFILER_MAX_PATHS];   /* selected frame */
    float    self[BR_PROFILER_MAX_PATHS];
    uint32_t calls[BR_PROFILER_MAX_PATHS];
    float    wmin[BR_PROFILER_MAX_PATHS];    /* per-frame totals over the window */
    float    wmax[BR_PROFILER_MAX_PATHS];
    float    wsum[BR_PROFILER_MAX_PATHS];
    uint32_t wn[BR_PROFILER_MAX_PATHS];
    bool     shown[BR_PROFILER_MAX_PATHS];
    uint64_t self_ns[BR_PROFILER_MAX_SCOPES];
    float    frame_total[BR_PROFILER_MAX_PATHS];
};

static void br_prof_gather(br_profiler_state *p, uint32_t age, struct br_prof_rows *rows)
{
    SDL_zerop(rows);
    for (uint32_t back = 0; back < BR_PROFILER_WINDOW; back++) {
        br_profiler_frame *f = br_profiler_frame_at(p, age + back);
        if (f == NULL) {
            break;
        }
        const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
        SDL_zeroa(rows->frame_total);
        br_profiler_self_times(scopes, f->scope_count, rows->self_ns);
        for (uint32_t i = 0; i < f->scope_count; i++) {
            uint16_t path = scopes[i].path;
            float total = br_prof_ms(scopes[i].end - scopes[i].start);
            rows->frame_total[path] += total;
            if (back == 0) {
                rows->total[path] += total;
                rows->self[path] += br_prof_ms(rows->self_ns[i]);
                rows->calls[path] += 1;
            }
        }
        for (uint32_t path = 0; path < p->path_count; path++) {
            float t = rows->frame_total[path];
            if (t <= 0.0f) {
                continue;
            }
            rows->wmin[path] = rows->wn[path] == 0 || t < rows->wmin[path] ? t : rows->wmin[path];
            rows->wmax[path] = t > rows->wmax[path] ? t : rows->wmax[path];
            rows->wsum[path] += t;
            rows->wn[path] += 1;
        }
    }
}

static void br_prof_tree(br_engine *e, br_debug_state *s, uint32_t age, br_profiler_frame *f)
{
    mu_Context *ui = s->ui;
    br_profiler_state *p = e->profiler;
    bool det = br_app_deterministic(e);
    if (s->prof_rows == NULL) {
        s->prof_rows = br_alloc(sizeof(struct br_prof_rows)); /* ~40 KB: not on the stack */
        if (s->prof_rows == NULL) {
            return;
        }
    }
    struct br_prof_rows *rows = s->prof_rows;
    br_prof_gather(p, age, rows);
    static const int header[7] = { 214, 56, 56, 40, 56, 56, 56 };
    mu_layout_row(ui, 7, header, 0);
    mu_label(ui, "scope (self/total ms, this frame)");
    mu_label(ui, "self");
    mu_label(ui, "total");
    mu_label(ui, "calls");
    mu_label(ui, "min");
    mu_label(ui, "avg");
    mu_label(ui, "max");
    const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
    int skip_deeper = -1;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < f->scope_count && emitted < BR_PROF_MAX_ROWS; i++) {
        const br_profiler_scope *sc = &scopes[i];
        if (skip_deeper >= 0 && (int)sc->depth > skip_deeper) {
            continue;
        }
        skip_deeper = -1;
        uint16_t path = sc->path;
        bool has_children = i + 1 < f->scope_count && scopes[i + 1].depth > sc->depth;
        bool expanded = (sc->depth == 0) != s->profiler_toggled[path];
        if (!rows->shown[path]) {
            rows->shown[path] = true;
            emitted += 1;
            int indent = (int)sc->depth * 10 + 1;
            int row[9] = { indent, 18, 214 - indent - 18 - 2 * ui->style->spacing, 56, 56, 40, 56, 56, 56 };
            mu_layout_row(ui, 9, row, 0);
            mu_layout_next(ui);
            mu_push_id(ui, &path, sizeof(path));
            if (has_children) {
                if (mu_button_ex(ui, expanded ? "-" : "+", 0, MU_OPT_ALIGNCENTER)) {
                    s->profiler_toggled[path] = !s->profiler_toggled[path];
                    expanded = !expanded;
                }
            } else {
                mu_layout_next(ui);
            }
            mu_pop_id(ui);
            mu_label(ui, br_profiler_name(p, sc->name));
            if (det) {
                mu_label(ui, "0.10");
                mu_label(ui, "0.10");
                br_debug_labelf(ui, "%u", rows->calls[path]);
                mu_label(ui, "0.10");
                mu_label(ui, "0.10");
                mu_label(ui, "0.10");
            } else {
                float avg = rows->wn[path] > 0 ? rows->wsum[path] / (float)rows->wn[path] : 0.0f;
                br_debug_labelf(ui, "%.3f", (double)rows->self[path]);
                br_debug_labelf(ui, "%.3f", (double)rows->total[path]);
                br_debug_labelf(ui, "%u", rows->calls[path]);
                br_debug_labelf(ui, "%.3f", (double)rows->wmin[path]);
                br_debug_labelf(ui, "%.3f", (double)avg);
                br_debug_labelf(ui, "%.3f", (double)rows->wmax[path]);
            }
        }
        if (!expanded) {
            skip_deeper = sc->depth;
        }
    }
    if (f->dropped > 0) {
        mu_layout_row(ui, 1, br_prof_full_row, 0);
        br_debug_labelf(ui, "%u scopes dropped (over %d per frame or %d deep)", f->dropped, BR_PROFILER_MAX_SCOPES,
                        BR_PROFILER_MAX_DEPTH);
    }
}

/* ---- counters with sparklines */

static bool br_prof_counter_is_timing(int c)
{
    return c == BR_PROFILER_HEAP_BYTES || c == BR_PROFILER_HEAP_ALLOCS;
}

static void br_prof_counters(br_engine *e, br_debug_state *s, uint32_t age, br_profiler_frame *f)
{
    mu_Context *ui = s->ui;
    br_profiler_state *p = e->profiler;
    bool det = br_app_deterministic(e);
    static const int row[3] = { 130, 90, -1 };
    for (int c = 0; c < BR_PROFILER_COUNTER_COUNT; c++) {
        mu_layout_row(ui, 3, row, 0);
        mu_label(ui, br_profiler_counter_name((br_profiler_counter)c));
        bool placeholder = det && br_prof_counter_is_timing(c);
        if (placeholder) {
            mu_label(ui, "-");
        } else {
            br_debug_labelf(ui, "%lld", (long long)f->counters[c]);
        }
        mu_Rect r = mu_layout_next(ui);
        r.y += 2;
        r.h -= 4;
        mu_draw_rect(ui, r, mu_color(15, 16, 20, 255));
        if (placeholder) {
            continue;
        }
        int64_t max = 1;
        for (uint32_t back = 0; back < BR_PROF_SPARK_FRAMES; back++) {
            br_profiler_frame *g = br_profiler_frame_at(p, age + back);
            if (g != NULL && g->counters[c] > max) {
                max = g->counters[c];
            }
        }
        for (uint32_t back = 0; back < BR_PROF_SPARK_FRAMES; back++) {
            br_profiler_frame *g = br_profiler_frame_at(p, age + back);
            if (g == NULL) {
                break;
            }
            int x0 = r.x + (int)((int64_t)(BR_PROF_SPARK_FRAMES - 1 - back) * r.w / BR_PROF_SPARK_FRAMES);
            int x1 = r.x + (int)((int64_t)(BR_PROF_SPARK_FRAMES - back) * r.w / BR_PROF_SPARK_FRAMES);
            int h = (int)((double)(g->counters[c] > 0 ? g->counters[c] : 0) / (double)max * (double)r.h);
            mu_draw_rect(ui, mu_rect(x0, r.y + r.h - h, x1 - x0 > 0 ? x1 - x0 : 1, h), mu_color(100, 170, 230, 255));
        }
    }
}

/* ---- memory: heap, arenas, assets by type */

static void br_prof_memory(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    bool det = br_app_deterministic(e);
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    if (det) {
        mu_label(ui, "heap: (placeholder under --deterministic)");
    } else {
        br_memory_stats mem;
        br_memory_get_stats(&mem);
        br_debug_labelf(ui, "heap: %.1f KB live in %llu blocks, peak %.1f KB, %llu allocations since start",
                        (double)mem.live_bytes / 1024.0, (unsigned long long)mem.live_allocations,
                        (double)mem.peak_bytes / 1024.0, (unsigned long long)mem.total_allocations);
    }
    static const int arena_row[4] = { 200, 110, 110, -1 };
    mu_layout_row(ui, 4, arena_row, 0);
    mu_label(ui, "arena");
    mu_label(ui, "used KB");
    mu_label(ui, "peak KB");
    mu_label(ui, "capacity KB");
    uint32_t arenas = br_arena_count(e);
    for (uint32_t i = 0; i < arenas; i++) {
        const br_arena_stats *a = br_arena_at(e, i);
        mu_layout_row(ui, 4, arena_row, 0);
        mu_label(ui, a->name);
        br_debug_labelf(ui, "%.1f", (double)a->used / 1024.0);
        br_debug_labelf(ui, "%.1f", (double)a->peak / 1024.0);
        br_debug_labelf(ui, "%.1f", (double)a->capacity / 1024.0);
    }
    /* assets by loader type */
    if (s->prof_assets == NULL) {
        s->prof_assets = br_alloc(sizeof(br_asset_info) * BR_PROF_ASSET_ROWS);
        if (s->prof_assets == NULL) {
            return;
        }
    }
    br_asset_info *list = s->prof_assets;
    uint32_t total = br_assets_list(e, list, BR_PROF_ASSET_ROWS);
    uint32_t listed = total < BR_PROF_ASSET_ROWS ? total : BR_PROF_ASSET_ROWS;
    const char *types[BR_PROF_ASSET_TYPES];
    uint64_t bytes[BR_PROF_ASSET_TYPES];
    uint32_t counts[BR_PROF_ASSET_TYPES];
    uint32_t type_count = 0;
    uint32_t loading = 0;
    for (uint32_t i = 0; i < listed; i++) {
        const char *type = list[i].type != NULL ? list[i].type : "(loading)";
        loading += list[i].loading ? 1u : 0u;
        uint32_t t = 0;
        while (t < type_count && SDL_strcmp(types[t], type) != 0) {
            t++;
        }
        if (t == type_count) {
            if (type_count == BR_PROF_ASSET_TYPES) {
                continue;
            }
            types[t] = type;
            bytes[t] = 0;
            counts[t] = 0;
            type_count++;
        }
        bytes[t] += list[i].byte_size;
        counts[t] += 1;
    }
    static const int asset_row[3] = { 200, 110, -1 };
    mu_layout_row(ui, 3, asset_row, 0);
    mu_label(ui, "assets by type");
    mu_label(ui, "count");
    mu_label(ui, "KB (approx.)");
    for (uint32_t t = 0; t < type_count; t++) {
        mu_layout_row(ui, 3, asset_row, 0);
        mu_label(ui, types[t]);
        br_debug_labelf(ui, "%u", counts[t]);
        br_debug_labelf(ui, "%.1f", (double)bytes[t] / 1024.0);
    }
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    br_debug_labelf(ui, "%u assets cached (%u listed), %u loading", total, listed, loading);
}

/* ---- the panel */

void br_debug_profiler_panel(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    int ww = 0;
    int wh = 0;
    br_engine_window_size(e, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        ww = 1280;
        wh = 720;
    }
    int w = 640;
    int h = wh - 90 < 620 ? wh - 90 : 620;
    if (!mu_begin_window_ex(ui, "Profiler", mu_rect(ww - w - 10, 10, w, h > 200 ? h : 200), MU_OPT_NOCLOSE)) {
        return;
    }
    uint32_t age = br_prof_selected_age(e, s);
    br_prof_header(e, ui);
    br_prof_controls(e, s, age);
    br_prof_graph(e, s, age);
    br_profiler_frame *f = br_profiler_frame_at(e->profiler, age);
    if (f != NULL) {
        br_prof_stacked(e, s, f);
        mu_layout_row(ui, 1, br_prof_full_row, -1);
        mu_begin_panel_ex(ui, "profiler tab", 0);
        if (s->profiler_tab == 0) {
            br_prof_tree(e, s, age, f);
        } else if (s->profiler_tab == 1) {
            br_prof_counters(e, s, age, f);
        } else {
            br_prof_memory(e, s);
        }
        mu_end_panel(ui);
    }
    mu_end_window(ui);
}

void br_debug_profiler_hud(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    int ww = 0;
    int wh = 0;
    br_engine_window_size(e, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        ww = 1280;
        wh = 720;
    }
    mu_Rect where = mu_rect(ww - 350, wh - 62, 340, 52);
    int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOSCROLL | MU_OPT_NOCLOSE;
    if (!mu_begin_window_ex(ui, "profiler hud", where, opt)) {
        return;
    }
    s->hud = mu_get_current_container(ui);
    s->hud->rect = where; /* stays in the corner when the window is resized (from next frame) */
    mu_layout_row(ui, 1, br_prof_full_row, 0);
    if (br_app_deterministic(e)) {
        mu_label(ui, "FPS 60.0   CPU 4.00 ms   GPU 2.00 ms");
        mu_label(ui, "vsync limited");
    } else {
        br_profiler_summary_info sum;
        br_profiler_get_summary(e, &sum);
        char gpu[32];
        if (sum.gpu_ms > 0.0f) {
            SDL_snprintf(gpu, sizeof(gpu), "%s%.2f ms", sum.gpu_includes_vsync ? "<=" : "", (double)sum.gpu_ms);
        } else {
            SDL_snprintf(gpu, sizeof(gpu), "n/a");
        }
        br_debug_labelf(ui, "FPS %.1f   CPU %.2f ms   GPU %s", (double)sum.fps, (double)sum.cpu_ms, gpu);
        br_debug_labelf(ui, "%s   %s", br_profiler_verdict_name(sum.verdict), sum.top[0]);
    }
    mu_end_window(ui);
}
