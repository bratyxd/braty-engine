/* br_debug: overlay state, toggle key, frame timing, log capture, microui input and drawing
 * (microui commands -> clipped quads pushed through the batcher). Panels: br_debug_panels.c. */
#include "debug/br_debug_internal.h"
#include "text/br_text_internal.h"

/* ---- log capture */

static void SDLCALL br_debug_log_output(void *user, int category, SDL_LogPriority priority, const char *message)
{
    br_debug_state *s = user;
    static const char *names[] = { "", "trace", "verbose", "debug", "info", "warn", "error", "critical" };
    const char *level = priority >= 0 && (size_t)priority < BR_ARRAY_COUNT(names) ? names[priority] : "";
    SDL_LockMutex(s->log_lock);
    uint32_t slot = (s->log_start + s->log_count) % BR_DEBUG_LOG_LINES;
    if (s->log_count == BR_DEBUG_LOG_LINES) {
        s->log_start = (s->log_start + 1) % BR_DEBUG_LOG_LINES;
    } else {
        s->log_count++;
    }
    SDL_snprintf(s->log[slot], BR_DEBUG_LOG_LINE_SIZE, "[%s] %s", level, message);
    s->log_total++;
    SDL_UnlockMutex(s->log_lock);
    if (s->prev_log_fn != NULL) {
        s->prev_log_fn(s->prev_log_user, category, priority, message);
    }
}

uint32_t br_debug_log_count(br_engine *e)
{
    br_debug_state *s = e->debug;
    if (s == NULL) {
        return 0;
    }
    SDL_LockMutex(s->log_lock);
    uint32_t count = s->log_count;
    SDL_UnlockMutex(s->log_lock);
    return count;
}

bool br_debug_log_line(br_engine *e, uint32_t i, char *out, size_t out_size)
{
    br_debug_state *s = e->debug;
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (s == NULL) {
        return false;
    }
    /* copied under the lock: any thread may log, and the next line reuses the oldest slot */
    SDL_LockMutex(s->log_lock);
    bool ok = i < s->log_count;
    if (ok) {
        br_str_copy(out, out_size, s->log[(s->log_start + i) % BR_DEBUG_LOG_LINES]);
    }
    SDL_UnlockMutex(s->log_lock);
    return ok;
}

/* ---- public state */

void br_debug_set_visible(br_engine *e, bool visible)
{
    e->debug->open = visible;
}

bool br_debug_visible(br_engine *e)
{
    return e->debug != NULL && e->debug->open;
}

void br_debug_set_toggle_key(br_engine *e, SDL_Scancode key)
{
    e->debug->toggle_key = key;
}

void br_debug_set_profiler_key(br_engine *e, SDL_Scancode key)
{
    e->debug->profiler_key = key;
}

void br_debug_set_keys_enabled(br_engine *e, bool enabled)
{
    e->debug->keys_enabled = enabled;
}

void br_debug_select(br_engine *e, br_node n)
{
    e->debug->selected = n;
}

br_node br_debug_selected(br_engine *e)
{
    br_node none = { 0, 0 };
    br_node n = e->debug->selected;
    return br_node_valid(e, n) ? n : none;
}

/* ---- per frame: toggle key and frame time (runs open or not) */

static void br_debug_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_debug_state *s = user;
    bool keys = s->keys_enabled;
    if (keys && s->toggle_key != SDL_SCANCODE_UNKNOWN && br_input_key_pressed(e, s->toggle_key)) {
        s->open = !s->open;
    }
    if (keys && e->profiler != NULL && s->profiler_key != SDL_SCANCODE_UNKNOWN &&
        br_input_key_pressed(e, s->profiler_key)) {
        br_profiler_set_visible(e, !br_profiler_visible(e));
    }
    uint64_t now = SDL_GetTicksNS();
    if (s->last_ns != 0) {
        s->frame_ms[s->frame_index] = (float)((double)(now - s->last_ns) / 1e6);
        s->frame_index = (s->frame_index + 1) % BR_DEBUG_FRAME_SAMPLES;
    }
    s->last_ns = now;
}

/* ---- font */

/* Looked up on every use, never kept: a game may uninstall or replace "braty.debug". */
static br_font *br_debug_font(br_engine *e)
{
    return br_bitmap_font_installed(e, "braty.debug");
}

static bool br_debug_font_ready(br_engine *e, br_debug_state *s)
{
    br_font *font = br_debug_font(e);
    if (font == NULL && !s->font_failed) {
        br_text_style style = br_text_style_default();
        style.font_size = 14.0f;
        style.fill = br_color_make(1.0f, 1.0f, 1.0f, 1.0f);
        // ponytail: glyphs rasterised at 1 pixel per unit (soft on HiDPI); install with opts.resolution = pixel density and scale metrics if that matters.
        font = br_bitmap_font_install(e, "braty.debug", &style, NULL);
        s->font_failed = font == NULL;
    }
    if (font != NULL) {
        float base = 0.0f;
        br_bitmap_font_line_metrics(font, &s->line_height, &base);
    }
    return font != NULL;
}

static int br_debug_text_width(mu_Font font, const char *str, int len)
{
    br_engine *e = font;
    br_font *f = br_debug_font(e);
    size_t length = len < 0 ? SDL_strlen(str) : (size_t)len;
    size_t offset = 0;
    float width = 0.0f;
    uint32_t cp;
    while ((cp = br_text_utf8_next(str, length, &offset)) != 0) {
        br_bitmap_char ch;
        if (f != NULL && br_bitmap_font_get_char(f, cp, &ch)) {
            width += ch.xadvance;
        }
    }
    return (int)(width + 0.5f);
}

static int br_debug_text_height(mu_Font font)
{
    br_engine *e = font;
    return (int)(e->debug->line_height + 0.5f);
}

/* ---- drawing microui commands */

/* Pushes the axis-aligned rect r (with uvs) clipped to clip. */
static void br_debug_quad(br_render_ctx *rc, br_texture tex, float x0, float y0, float x1, float y1, float u0,
                          float v0, float u1, float v1, mu_Rect clip, uint32_t color)
{
    float cx0 = (float)clip.x;
    float cy0 = (float)clip.y;
    float cx1 = (float)(clip.x + clip.w);
    float cy1 = (float)(clip.y + clip.h);
    if (x1 <= cx0 || y1 <= cy0 || x0 >= cx1 || y0 >= cy1 || x1 <= x0 || y1 <= y0) {
        return;
    }
    /* cut each side, moving the uvs by the same fraction */
    if (x0 < cx0) {
        u0 += (u1 - u0) * (cx0 - x0) / (x1 - x0);
        x0 = cx0;
    }
    if (x1 > cx1) {
        u1 -= (u1 - u0) * (x1 - cx1) / (x1 - x0);
        x1 = cx1;
    }
    if (y0 < cy0) {
        v0 += (v1 - v0) * (cy0 - y0) / (y1 - y0);
        y0 = cy0;
    }
    if (y1 > cy1) {
        v1 -= (v1 - v0) * (y1 - cy1) / (y1 - y0);
        y1 = cy1;
    }
    br_vertex2d v[4] = {
        { x0, y0, u0, v0, color },
        { x1, y0, u1, v0, color },
        { x1, y1, u1, v1, color },
        { x0, y1, u0, v1, color },
    };
    br_render_push_quad(rc, tex, v);
}

static uint32_t br_debug_color(mu_Color c)
{
    br_color color = br_color_make(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
    return br_render_pack_color(color, 1.0f);
}

static void br_debug_draw_text(br_engine *e, br_render_ctx *rc, const char *str, float x, float y, mu_Rect clip,
                               uint32_t color)
{
    br_font *font = br_debug_font(e);
    size_t length = SDL_strlen(str);
    size_t offset = 0;
    uint32_t cp;
    while ((cp = br_text_utf8_next(str, length, &offset)) != 0) {
        br_bitmap_char ch;
        if (!br_bitmap_font_get_char(font, cp, &ch)) {
            continue;
        }
        br_texture page = br_bitmap_font_page_texture(font, ch.page);
        const br_texture_info *info = br_render_texture_info(rc, page);
        if (ch.width > 0.0f && info->source_width > 0) {
            float sw = (float)info->source_width;
            float sh = (float)info->source_height;
            float gx = x + ch.xoffset;
            float gy = y + ch.yoffset;
            br_debug_quad(rc, page, gx, gy, gx + ch.width, gy + ch.height, ch.x / sw, ch.y / sh,
                          (ch.x + ch.width) / sw, (ch.y + ch.height) / sh, clip, color);
        }
        x += ch.xadvance;
    }
}

static void br_debug_draw_icon(br_engine *e, br_render_ctx *rc, int id, mu_Rect r, mu_Rect clip, uint32_t color)
{
    if (id == MU_ICON_CHECK) {
        br_texture white = br_texture_white(e);
        br_debug_quad(rc, white, (float)r.x + 6, (float)r.y + 6, (float)(r.x + r.w) - 6, (float)(r.y + r.h) - 6, 0, 0,
                      1, 1, clip, color);
        return;
    }
    const char *glyph = id == MU_ICON_CLOSE ? "x" : id == MU_ICON_COLLAPSED ? "+" : "-";
    float w = (float)br_debug_text_width(e, glyph, -1);
    float h = e->debug->line_height;
    br_debug_draw_text(e, rc, glyph, (float)r.x + ((float)r.w - w) * 0.5f, (float)r.y + ((float)r.h - h) * 0.5f, clip,
                       color);
}

static void br_debug_draw(br_engine *e, br_render_ctx *rc)
{
    mu_Context *ui = e->debug->ui;
    br_texture white = br_texture_white(e);
    mu_Rect clip = mu_rect(0, 0, 1 << 20, 1 << 20);
    mu_Command *cmd = NULL;
    while (mu_next_command(ui, &cmd)) {
        if (cmd->type == MU_COMMAND_CLIP) {
            clip = cmd->clip.rect;
        } else if (cmd->type == MU_COMMAND_RECT) {
            mu_Rect r = cmd->rect.rect;
            br_debug_quad(rc, white, (float)r.x, (float)r.y, (float)(r.x + r.w), (float)(r.y + r.h), 0, 0, 1, 1, clip,
                          br_debug_color(cmd->rect.color));
        } else if (cmd->type == MU_COMMAND_TEXT) {
            br_debug_draw_text(e, rc, cmd->text.str, (float)cmd->text.pos.x, (float)cmd->text.pos.y, clip,
                               br_debug_color(cmd->text.color));
        } else if (cmd->type == MU_COMMAND_ICON) {
            br_debug_draw_icon(e, rc, cmd->icon.id, cmd->icon.rect, clip, br_debug_color(cmd->icon.color));
        }
    }
}

/* ---- input: input-module state -> microui events */

static void br_debug_input(br_engine *e, mu_Context *ui)
{
    br_vec2 mouse = br_input_mouse_position(e);
    int mx = (int)mouse.x;
    int my = (int)mouse.y;
    mu_input_mousemove(ui, mx, my);
    static const int buttons[3][2] = { { SDL_BUTTON_LEFT, MU_MOUSE_LEFT },
                                       { SDL_BUTTON_RIGHT, MU_MOUSE_RIGHT },
                                       { SDL_BUTTON_MIDDLE, MU_MOUSE_MIDDLE } };
    for (int i = 0; i < 3; i++) {
        if (br_input_mouse_pressed(e, buttons[i][0])) {
            mu_input_mousedown(ui, mx, my, buttons[i][1]);
        }
        if (br_input_mouse_released(e, buttons[i][0])) {
            mu_input_mouseup(ui, mx, my, buttons[i][1]);
        }
    }
    br_vec2 wheel = br_input_mouse_wheel(e);
    if (wheel.y != 0.0f) {
        mu_input_scroll(ui, 0, (int)(-wheel.y * 30.0f));
    }
    static const int keys[][3] = { { SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, MU_KEY_SHIFT },
                                   { SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, MU_KEY_CTRL },
                                   { SDL_SCANCODE_LALT, SDL_SCANCODE_RALT, MU_KEY_ALT },
                                   { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_BACKSPACE, MU_KEY_BACKSPACE },
                                   { SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, MU_KEY_RETURN } };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(keys); i++) {
        if (br_input_key_pressed(e, (SDL_Scancode)keys[i][0]) || br_input_key_pressed(e, (SDL_Scancode)keys[i][1])) {
            mu_input_keydown(ui, keys[i][2]);
        }
        if (br_input_key_released(e, (SDL_Scancode)keys[i][0]) || br_input_key_released(e, (SDL_Scancode)keys[i][1])) {
            mu_input_keyup(ui, keys[i][2]);
        }
    }
    const char *text = br_input_text(e);
    if (text[0] != '\0') {
        mu_input_text(ui, text);
    }
}

/* ---- taking input that lands on the overlay */

static bool br_debug_over_window(br_debug_state *s, float x, float y)
{
    mu_Context *ui = s->ui;
    for (int i = 0; i < ui->root_list.idx; i++) {
        mu_Container *cnt = ui->root_list.items[i];
        mu_Rect r = cnt->rect;
        if (cnt->open && cnt != s->hud && x >= (float)r.x && y >= (float)r.y && x < (float)(r.x + r.w) &&
            y < (float)(r.y + r.h)) {
            return true;
        }
    }
    return false;
}

static bool br_debug_is_toggle(br_debug_state *s, SDL_Scancode key)
{
    return key == s->toggle_key || key == s->profiler_key;
}

bool br_debug_wants_event(br_engine *e, const SDL_Event *ev)
{
    br_debug_state *s = e->debug;
    if (s == NULL || !s->ui_live) {
        return false;
    }
    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->button.button < 32 && br_debug_over_window(s, ev->button.x, ev->button.y)) {
            s->captured |= 1u << ev->button.button;
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button < 32 && (s->captured & (1u << ev->button.button)) != 0) {
            s->captured &= ~(1u << ev->button.button); /* the release of a press the overlay took */
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_WHEEL:
        return br_debug_over_window(s, ev->wheel.mouse_x, ev->wheel.mouse_y);
    case SDL_EVENT_KEY_DOWN:
        /* typing into a number field (shift-click on it) */
        return s->ui->number_edit != 0 && !br_debug_is_toggle(s, ev->key.scancode);
    case SDL_EVENT_KEY_UP:
        /* never taken: a key held before the edit began (W to walk) must reach the game's input
         * state when released, or it stays down; microui reads releases from br_input */
        return false;
    case SDL_EVENT_TEXT_INPUT:
        return s->ui->number_edit != 0;
    default:
        return false;
    }
}

/* Feeds an event the game will not see to microui (the input module never gets it). */
static void br_debug_feed(br_debug_state *s, const SDL_Event *ev)
{
    mu_Context *ui = s->ui;
    int button = ev->button.button == SDL_BUTTON_LEFT    ? MU_MOUSE_LEFT
                 : ev->button.button == SDL_BUTTON_RIGHT ? MU_MOUSE_RIGHT
                                                         : MU_MOUSE_MIDDLE;
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        mu_input_mousedown(ui, (int)ev->button.x, (int)ev->button.y, button);
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        mu_input_mouseup(ui, (int)ev->button.x, (int)ev->button.y, button);
    } else if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        mu_input_scroll(ui, 0, (int)(-ev->wheel.y * 30.0f));
    } else if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) {
        int key = 0;
        SDL_Scancode sc = ev->key.scancode;
        if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) {
            key = MU_KEY_SHIFT;
        } else if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL) {
            key = MU_KEY_CTRL;
        } else if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) {
            key = MU_KEY_ALT;
        } else if (sc == SDL_SCANCODE_BACKSPACE) {
            key = MU_KEY_BACKSPACE;
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            key = MU_KEY_RETURN;
        }
        if (key != 0 && ev->type == SDL_EVENT_KEY_DOWN) {
            mu_input_keydown(ui, key);
        } else if (key != 0) {
            mu_input_keyup(ui, key);
        }
    } else if (ev->type == SDL_EVENT_TEXT_INPUT) {
        mu_input_text(ui, ev->text.text);
    }
}

/* SDL event filter: events the overlay takes never reach the queue, so neither the input
 * module, pointer events nor game systems see them. */
static bool SDLCALL br_debug_event_filter(void *user, SDL_Event *ev)
{
    br_engine *e = user;
    br_debug_state *s = e->debug;
    if (br_debug_wants_event(e, ev)) {
        br_debug_feed(s, ev);
        return false;
    }
    return s->prev_filter != NULL ? s->prev_filter(s->prev_filter_user, ev) : true;
}

/* ---- the UI: built every frame something is shown (also headless), drawn after the stage */

static void br_debug_ui_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    br_debug_state *s = user;
    bool show = s->open || br_profiler_visible(e) || br_profiler_hud(e);
    s->ui_live = false;
    s->hud = NULL;
    if (!show) {
        s->ui->root_list.idx = 0;
        s->captured = 0;
        return;
    }
    if (e->window != NULL && !br_debug_font_ready(e, s)) {
        return;
    }
    if (e->window != NULL && !s->filter_set) {
        // ponytail: SDL's event filter is process-wide (chained to the previous one), like the log hook; injected input (br_input_inject_*) skips it.
        SDL_GetEventFilter(&s->prev_filter, &s->prev_filter_user);
        SDL_SetEventFilter(br_debug_event_filter, e);
        s->filter_set = true;
    }
    br_debug_input(e, s->ui);
    mu_begin(s->ui);
    if (s->open) {
        br_debug_panels(e, s);
    }
    if (br_profiler_visible(e)) {
        br_debug_profiler_panel(e, s);
    }
    if (br_profiler_hud(e)) {
        br_debug_profiler_hud(e, s);
    }
    mu_end(s->ui);
    s->ui_live = true;
}

static void br_debug_overlay(br_engine *e, br_render_ctx *rc, void *user)
{
    br_debug_state *s = user;
    if (!s->ui_live || br_debug_font(e) == NULL) {
        return;
    }
    br_render_set_blend(rc, BR_BLEND_NORMAL);
    br_debug_draw(e, rc);
}

/* ---- module */

static void br_debug_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_debug_state *s = e->debug;
    if (s->filter_set) {
        SDL_EventFilter filter = NULL;
        void *filter_user = NULL;
        if (SDL_GetEventFilter(&filter, &filter_user) && filter == br_debug_event_filter && filter_user == e) {
            SDL_SetEventFilter(s->prev_filter, s->prev_filter_user);
        }
    }
    SDL_LogOutputFunction fn = NULL;
    void *fn_user = NULL;
    SDL_GetLogOutputFunction(&fn, &fn_user);
    if (fn == br_debug_log_output && fn_user == s) {
        SDL_SetLogOutputFunction(s->prev_log_fn, s->prev_log_user);
    }
    SDL_DestroyMutex(s->log_lock);
    br_array_free(&s->expanded);
    br_free(s->prof_rows);
    br_free(s->prof_assets);
    br_free(s->ui);
    br_free(s);
    e->debug = NULL;
}

void br_debug_register(br_engine *e)
{
    br_debug_state *s = br_alloc(sizeof(br_debug_state));
    mu_Context *ui = br_alloc(sizeof(mu_Context));
    if (s == NULL || ui == NULL) {
        br_free(s);
        br_free(ui);
        return;
    }
    e->debug = s;
    s->ui = ui;
    s->keys_enabled = true;
    s->toggle_key = SDL_SCANCODE_F1;
    s->profiler_key = SDL_SCANCODE_F2;
    br_array_init(&s->expanded, sizeof(br_node));
    s->log_lock = SDL_CreateMutex();
    mu_init(ui);
    ui->text_width = br_debug_text_width;
    ui->text_height = br_debug_text_height;
    ui->style->font = e;
    ui->style->colors[MU_COLOR_WINDOWBG] = mu_color(40, 42, 48, 225);
    ui->style->colors[MU_COLOR_PANELBG] = mu_color(25, 27, 32, 200);
    // ponytail: SDL's log output hook is process-wide; with several engines the last one registered captures. Fine for one game per process.
    SDL_GetLogOutputFunction(&s->prev_log_fn, &s->prev_log_user);
    SDL_SetLogOutputFunction(br_debug_log_output, s);
    br_system_add(e, "debug.toggle", BR_PHASE_UPDATE, -1000, br_debug_system, s);
    br_system_add(e, "debug.ui", BR_PHASE_PRE_RENDER, 900, br_debug_ui_system, s); /* before render.collect */
    br_render_add_overlay(e, br_debug_overlay, s);
    br_engine_on_shutdown(e, br_debug_shutdown, NULL);
}
