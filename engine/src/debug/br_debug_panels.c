/* br_debug_panels: the overlay's windows (stats + memory, scene tree, inspector, systems, log). */
#include "debug/br_debug_internal.h"

#include <stdarg.h>

/* Children listed per node in the tree (big particle-like trees would bury the panel). */
#define BR_DEBUG_TREE_MAX_CHILDREN 64
#define BR_DEBUG_ASSET_ROWS 64

static const int br_debug_full_row[1] = { -1 };

static void br_debug_labelf(mu_Context *ui, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    mu_label(ui, text);
}

/* ---- stats */

static void br_debug_frame_graph(br_engine *e, mu_Context *ui, const br_debug_state *s)
{
    /* red = slower than the pacing target (+10%), not a fixed 17 ms that 60 Hz vsync already hits */
    float slow_ms = br_app_frame_budget_ms(e) * 1.1f;
    mu_layout_row(ui, 1, br_debug_full_row, 50);
    mu_Rect r = mu_layout_next(ui);
    mu_draw_rect(ui, r, mu_color(15, 16, 20, 255));
    float max_ms = 33.4f; /* two 60 Hz frames: taller bars are clipped */
    float bar = (float)r.w / BR_DEBUG_FRAME_SAMPLES;
    for (uint32_t i = 0; i < BR_DEBUG_FRAME_SAMPLES; i++) {
        float ms = s->frame_ms[(s->frame_index + i) % BR_DEBUG_FRAME_SAMPLES];
        int h = (int)(br_clampf(ms / max_ms, 0.0f, 1.0f) * (float)r.h);
        mu_Color c = ms > slow_ms ? mu_color(230, 90, 70, 255) : mu_color(90, 200, 120, 255);
        int x = r.x + (int)((float)i * bar);
        int w = (int)((float)(i + 1) * bar) - (int)((float)i * bar);
        mu_draw_rect(ui, mu_rect(x, r.y + r.h - h, w > 0 ? w : 1, h), c);
    }
    /* 16.7 ms line */
    int y = r.y + r.h - (int)(16.7f / max_ms * (float)r.h);
    mu_draw_rect(ui, mu_rect(r.x, y, r.w, 1), mu_color(200, 200, 200, 120));
}

static void br_debug_stats(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    if (!mu_begin_window_ex(ui, "Stats", mu_rect(10, 10, 330, 300), MU_OPT_NOCLOSE)) {
        return;
    }
    float last = s->frame_ms[(s->frame_index + BR_DEBUG_FRAME_SAMPLES - 1) % BR_DEBUG_FRAME_SAMPLES];
    mu_layout_row(ui, 1, br_debug_full_row, 0);
    br_debug_labelf(ui, "FPS %.1f   frame %.2f ms", (double)br_ticker_fps(e), (double)last);
    br_debug_frame_graph(e, ui, s);
    br_render_stats rs;
    br_render_get_stats(e, &rs);
    mu_layout_row(ui, 1, br_debug_full_row, 0);
    br_debug_labelf(ui, "draw calls %u   batches %u   passes %u", rs.draw_calls, rs.batches, rs.passes);
    br_debug_labelf(ui, "vertices %u   nodes drawn %u", rs.vertices, rs.nodes_drawn);
    br_debug_labelf(ui, "nodes %u   textures %u   assets %u", br_scene_node_count(e), br_texture_count(e),
                    br_assets_count(e));
    br_debug_labelf(ui, "render-target pool %u   systems %u", br_filters_pool_count(e), br_system_count(e));
    br_memory_stats mem;
    br_memory_get_stats(&mem);
    br_debug_labelf(ui, "heap %llu KB in %llu blocks   peak %llu KB", (unsigned long long)(mem.live_bytes / 1024),
                    (unsigned long long)mem.live_allocations, (unsigned long long)(mem.peak_bytes / 1024));
    if (mu_header_ex(ui, "Arenas", MU_OPT_EXPANDED)) {
        uint32_t count = br_arena_count(e);
        for (uint32_t i = 0; i < count; i++) {
            const br_arena_stats *a = br_arena_at(e, i);
            br_debug_labelf(ui, "%s: %u / %u KB, peak %u KB", a->name, (unsigned)(a->used / 1024),
                            (unsigned)(a->capacity / 1024), (unsigned)(a->peak / 1024));
        }
    }
    if (mu_header_ex(ui, "Assets", 0)) {
        br_asset_info list[BR_DEBUG_ASSET_ROWS];
        uint32_t total = br_assets_list(e, list, BR_DEBUG_ASSET_ROWS);
        uint32_t shown = total < BR_DEBUG_ASSET_ROWS ? total : BR_DEBUG_ASSET_ROWS;
        for (uint32_t i = 0; i < shown; i++) {
            br_debug_labelf(ui, "%s [%s] refs %u, %u KB", list[i].path, list[i].type ? list[i].type : "loading",
                            list[i].ref_count, (unsigned)(list[i].byte_size / 1024));
        }
        if (total > shown) {
            br_debug_labelf(ui, "... %u more", total - shown);
        }
    }
    mu_end_window(ui);
}

/* ---- scene tree */

static void br_debug_node_name(br_engine *e, br_node n, char *out, size_t size)
{
    const br_node_type_desc *type = br_node_type_get(e, br_node_type_of(e, n));
    const char *label = br_node_label(e, n);
    if (label[0] != '\0') {
        SDL_snprintf(out, size, "%s  %s", label, type != NULL ? type->name : "?");
    } else {
        SDL_snprintf(out, size, "#%u  %s", n.index, type != NULL ? type->name : "?");
    }
}

static bool br_debug_expanded(const br_debug_state *s, br_node n, uint32_t *slot)
{
    for (uint32_t i = 0; i < s->expanded.count; i++) {
        br_node *x = br_array_at(&s->expanded, i);
        if (br_node_equal(*x, n)) {
            *slot = i;
            return true;
        }
    }
    return false;
}

/* One row: [indent] [+/-] [name]; the name button selects. The stage is always open. */
static void br_debug_tree_node(br_engine *e, br_debug_state *s, br_node n, int depth)
{
    mu_Context *ui = s->ui;
    char name[160];
    char text[176];
    br_debug_node_name(e, n, name, sizeof(name));
    bool selected = br_node_equal(n, s->selected);
    SDL_snprintf(text, sizeof(text), "%s%s", selected ? "> " : "", name);
    uint32_t count = br_node_child_count(e, n);
    uint32_t slot = 0;
    bool open = depth == 0 || br_debug_expanded(s, n, &slot);

    mu_push_id(ui, &n.index, sizeof(n.index));
    int row[3] = { depth * 12 + 1, 20, -1 };
    mu_layout_row(ui, 3, row, 0);
    mu_layout_next(ui); /* indent cell */
    if (count > 0 && depth > 0) {
        if (mu_button_ex(ui, open ? "-" : "+", 0, MU_OPT_ALIGNCENTER)) {
            if (open) {
                br_array_remove_swap(&s->expanded, slot);
            } else {
                br_array_push(&s->expanded, &n);
            }
            open = !open;
        }
    } else {
        mu_layout_next(ui);
    }
    if (mu_button_ex(ui, text, 0, 0)) {
        s->selected = n;
    }
    if (open && depth < 32) {
        uint32_t shown = 0;
        for (uint32_t i = 0; i < count && shown < BR_DEBUG_TREE_MAX_CHILDREN; i++) {
            br_node child = br_node_child_at(e, n, i);
            if (br_node_valid(e, child)) {
                br_debug_tree_node(e, s, child, depth + 1);
                shown++;
            }
        }
        if (count > shown) {
            mu_layout_row(ui, 1, br_debug_full_row, 0);
            br_debug_labelf(ui, "... %u more children", count - shown);
        }
    }
    mu_pop_id(ui);
}

static void br_debug_scene(br_engine *e, br_debug_state *s)
{
    if (!mu_begin_window_ex(s->ui, "Scene", mu_rect(350, 10, 330, 400), MU_OPT_NOCLOSE)) {
        return;
    }
    br_debug_tree_node(e, s, br_scene_root(e), 0);
    mu_end_window(s->ui);
}

/* ---- inspector */

/* Two editable numbers on one row; true if either changed. */
static bool br_debug_edit_vec2(mu_Context *ui, const char *label, br_vec2 *v, float step)
{
    static const int row[3] = { 70, 95, -1 };
    mu_layout_row(ui, 3, row, 0);
    mu_label(ui, label);
    bool changed = false;
    mu_push_id(ui, label, (int)SDL_strlen(label));
    changed = (mu_number_ex(ui, &v->x, step, "%.2f", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) != 0;
    changed = (mu_number_ex(ui, &v->y, step, "%.2f", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) != 0 || changed;
    mu_pop_id(ui);
    return changed;
}

static void br_debug_inspector(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    if (!mu_begin_window_ex(ui, "Inspector", mu_rect(690, 10, 300, 230), MU_OPT_NOCLOSE)) {
        return;
    }
    br_node n = br_debug_selected(e);
    mu_layout_row(ui, 1, br_debug_full_row, 0);
    if (!br_node_valid(e, n)) {
        mu_label(ui, "click a node in Scene to inspect it");
        mu_end_window(ui);
        return;
    }
    char name[160];
    br_debug_node_name(e, n, name, sizeof(name));
    mu_label(ui, name);
    br_vec2 position = br_node_position(e, n);
    if (br_debug_edit_vec2(ui, "position", &position, 1.0f)) {
        br_node_set_position(e, n, position.x, position.y);
    }
    br_vec2 scale = br_node_scale(e, n);
    if (br_debug_edit_vec2(ui, "scale", &scale, 0.01f)) {
        br_node_set_scale(e, n, scale.x, scale.y);
    }
    static const int row[2] = { 70, -1 };
    mu_layout_row(ui, 2, row, 0);
    mu_label(ui, "rotation");
    float rotation = br_node_rotation(e, n);
    if (mu_number_ex(ui, &rotation, 0.01f, "%.3f", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) {
        br_node_set_rotation(e, n, rotation);
    }
    mu_label(ui, "alpha");
    float alpha = br_node_alpha(e, n);
    if (mu_slider_ex(ui, &alpha, 0.0f, 1.0f, 0.0f, "%.2f", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) {
        br_node_set_alpha(e, n, alpha);
    }
    mu_label(ui, "");
    int visible = br_node_visible(e, n) ? 1 : 0;
    if (mu_checkbox(ui, "visible", &visible) & MU_RES_CHANGE) {
        br_node_set_visible(e, n, visible != 0);
    }
    mu_end_window(ui);
}

/* ---- systems */

static void br_debug_systems(br_engine *e, br_debug_state *s)
{
    mu_Context *ui = s->ui;
    if (!mu_begin_window_ex(ui, "Systems", mu_rect(10, 320, 330, 390), MU_OPT_NOCLOSE)) {
        return;
    }
    static const char *phases[BR_PHASE_COUNT] = { "input", "fixed", "update", "late", "pre-render", "render", "post" };
    static const int row[3] = { 170, 72, -1 };
    mu_layout_row(ui, 3, row, 0);
    mu_label(ui, "system (on/off)");
    mu_label(ui, "phase");
    mu_label(ui, "last ms");
    uint32_t count = br_system_count(e);
    for (uint32_t i = 0; i < count; i++) {
        br_system_info info;
        if (!br_system_get_info(e, i, &info)) {
            continue;
        }
        mu_layout_row(ui, 3, row, 0);
        int enabled = info.enabled ? 1 : 0;
        if (mu_checkbox(ui, info.name, &enabled) & MU_RES_CHANGE) {
            br_system_set_enabled(e, info.name, enabled != 0);
        }
        mu_label(ui, phases[info.phase]);
        br_debug_labelf(ui, "%.3f", (double)info.last_time_ns / 1e6);
    }
    mu_end_window(ui);
}

/* ---- log console */

static void br_debug_log(br_engine *e, br_debug_state *s)
{
    (void)e;
    mu_Context *ui = s->ui;
    if (!mu_begin_window_ex(ui, "Log", mu_rect(350, 420, 640, 290), MU_OPT_NOCLOSE)) {
        return;
    }
    mu_layout_row(ui, 1, br_debug_full_row, -1);
    mu_begin_panel_ex(ui, "log lines", 0);
    mu_Container *panel = mu_get_current_container(ui);
    mu_layout_row(ui, 1, br_debug_full_row, 0);
    SDL_LockMutex(s->log_lock);
    for (uint32_t i = 0; i < s->log_count; i++) {
        mu_label(ui, s->log[(s->log_start + i) % BR_DEBUG_LOG_LINES]);
    }
    bool scroll = s->log_seen != s->log_total;
    s->log_seen = s->log_total;
    SDL_UnlockMutex(s->log_lock);
    mu_end_panel(ui);
    if (scroll) {
        panel->scroll.y = panel->content_size.y; /* follow new lines */
    }
    mu_end_window(ui);
}

void br_debug_panels(br_engine *e, br_debug_state *s)
{
    br_debug_stats(e, s);
    br_debug_systems(e, s);
    br_debug_scene(e, s);
    br_debug_inspector(e, s);
    br_debug_log(e, s);
}
