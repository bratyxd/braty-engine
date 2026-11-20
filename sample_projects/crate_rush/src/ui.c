/* ui.c - screen-space UI behaviour; the layouts are scene files (menu, loading, hud, pause,
 * settings, results) drawn through the persistent UI camera and anchored with camera.anchor.
 *
 *   UPDATE       5  cr.capture    rebinding: waits for the next key / button
 *   UPDATE      10  cr.ui         focus (keyboard / pad / mouse hover), activate, settings left/right
 *   UPDATE      50  cr.loading    loading scene progress bar
 *   LATE_UPDATE 30  cr.prompts    "prompt" sprites / texts follow the device used last
 *   LATE_UPDATE 40  cr.counters   "counter" texts follow their tweened value
 *   LATE_UPDATE 50  cr.settings   settings rows show their values (br_query: button + setting)
 *   LATE_UPDATE 60  cr.hud        clock, crates, low-time warning
 */
#include "game.h"

#define CR_MAX_BUTTONS 16

void cr_ui_text(br_engine *e, br_node root, const char *label, const char *text)
{
    br_node n = br_node_find_by_label(e, root, label, true);
    if (br_node_valid(e, n) && SDL_strcmp(br_text_text(e, n), text) != 0) {
        br_text_set_text(e, n, text);
    }
}

/* Tweens a "counter" component's value (the text follows in cr.counters). */
void cr_ui_count_to(br_engine *e, cr_game *g, br_node root, const char *label, float value)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    br_node n = br_node_find_by_label(e, root, label, true);
    if (br_component_get(e, n, g->id.counter) == NULL) {
        return;
    }
    br_anim_target target = br_anim_target_2d(n);
    br_tween_kill_target(e, target);
    float to[1] = { value };
    br_tween_to(e, target, "counter.value", to, t->counter_time, (br_ease_type)t->counter_ease);
    float pop_from[2] = { 1.25f, 1.25f };
    float pop_to[2] = { 1.0f, 1.0f };
    br_tween_from_to(e, target, "scale", pop_from, pop_to, 0.3f, BR_EASE_OUT_BACK);
}

/* The scene whose buttons take input: the overlay (pause / settings) if any, else the active one. */
static br_node cr_ui_root(br_engine *e, cr_game *g)
{
    br_node none = { 0, 0 };
    if (br_scene_valid(e, g->overlay)) {
        return br_scene_root_2d(e, g->overlay);
    }
    if (g->state == CR_STATE_MENU || g->state == CR_STATE_RESULTS) {
        return br_scene_root_2d(e, br_scene_active(e));
    }
    return none;
}

/* True if n is under root and it and every ancestor up to root are visible. */
static bool cr_shown_under(br_engine *e, br_node n, br_node root)
{
    while (br_node_valid(e, n)) {
        if (!br_node_visible(e, n)) {
            return false;
        }
        if (n.index == root.index && n.generation == root.generation) {
            return true;
        }
        n = br_node_parent(e, n);
    }
    return false;
}

/* The live buttons of the input scene, sorted by "order". */
static uint32_t cr_ui_buttons(br_engine *e, cr_game *g, br_node *out)
{
    br_node root = cr_ui_root(e, g);
    uint32_t n = 0;
    if (!br_node_valid(e, root) || br_scene_switching(e)) {
        return 0;
    }
    uint32_t ids[1] = { g->id.button };
    br_query q = br_query_begin(e, ids, 1);
    while (br_query_next(&q) && n < CR_MAX_BUTTONS) {
        if (!cr_shown_under(e, q.node, root)) {
            continue;
        }
        const cr_button *b = q.data[0];
        uint32_t at = n;
        while (at > 0 && ((const cr_button *)br_component_get(e, out[at - 1], g->id.button))->order > b->order) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = q.node;
        n++;
    }
    br_query_end(&q);
    return n;
}


/* Runs a button node's action (copies first: the action may switch scenes). */
static void cr_ui_activate(br_engine *e, cr_game *g, br_node n)
{
    cr_button b = *(const cr_button *)br_component_get(e, n, g->id.button);
    const cr_setting *row_data = br_component_get(e, n, g->id.setting);
    cr_setting row = { -1 };
    if (row_data != NULL) {
        row = *row_data;
    }
    cr_button_action(e, g, &b, row_data != NULL ? &row : NULL);
}

/* An action without a button (Escape / B on a menu). */
static void cr_ui_do(br_engine *e, cr_game *g, cr_action action)
{
    cr_button b;
    SDL_zero(b);
    b.action = (int32_t)action;
    cr_button_action(e, g, &b, NULL);
}

/* ---- mouse: scene files attach these by name ("on": { "click": "button_click", ... }) */

static void cr_on_button_click(br_engine *e, br_event *ev, void *user)
{
    cr_game *g = user;
    br_node buttons[CR_MAX_BUTTONS];
    uint32_t n = cr_ui_buttons(e, g, buttons);
    for (uint32_t i = 0; i < n && !g->capturing; i++) {
        if (buttons[i].index == ev->current_target.index && buttons[i].generation == ev->current_target.generation) {
            g->focus = (int32_t)i;
            cr_ui_activate(e, g, buttons[i]);
            return;
        }
    }
}

static void cr_on_button_over(br_engine *e, br_event *ev, void *user)
{
    cr_game *g = user;
    br_node buttons[CR_MAX_BUTTONS];
    uint32_t n = cr_ui_buttons(e, g, buttons);
    for (uint32_t i = 0; i < n; i++) {
        if (buttons[i].index == ev->current_target.index && g->focus != (int32_t)i) {
            g->focus = (int32_t)i;
            cr_ui_sound(e, g, "focus");
        }
    }
}

/* ---- keyboard / gamepad focus */

static void cr_ui_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    const cr_tuning *t = cr_tuning_get(e, g);
    br_node buttons[CR_MAX_BUTTONS];
    uint32_t n = cr_ui_buttons(e, g, buttons);
    if (n == 0 || g->capturing) {
        return;
    }
    int32_t before = g->focus;
    g->focus = (g->focus % (int32_t)n + (int32_t)n) % (int32_t)n;
    /* left / right change a settings row's value; on other buttons they move like up / down */
    bool on_row = br_component_get(e, buttons[g->focus], g->id.setting) != NULL;
    if (br_input_action_pressed(e, "nav_down") || (!on_row && br_input_action_pressed(e, "nav_right"))) {
        g->focus++;
    } else if (br_input_action_pressed(e, "nav_up") || (!on_row && br_input_action_pressed(e, "nav_left"))) {
        g->focus--;
    }
    g->focus = (g->focus % (int32_t)n + (int32_t)n) % (int32_t)n;
    if (g->focus != before) {
        cr_ui_sound(e, g, "focus");
    }
    br_node focused = buttons[g->focus];
    const cr_setting *row = br_component_get(e, focused, g->id.setting);
    if (row != NULL && on_row && (br_input_action_pressed(e, "nav_left") || br_input_action_pressed(e, "nav_right"))) {
        cr_setting_change(e, g, row->key, br_input_action_pressed(e, "nav_left") ? -1 : 1);
        cr_ui_sound(e, g, "focus");
    }
    /* the focused button grows a little (easing from tuning.json); the others settle back */
    for (uint32_t i = 0; i < n; i++) {
        cr_button *b = br_component_get(e, buttons[i], g->id.button);
        if (!b->ready) {
            b->base_scale = br_node_scale(e, buttons[i]);
            b->ready = true;
        }
        bool focus = (int32_t)i == g->focus;
        if (focus != b->focused) {
            b->focused = focus;
            float k = focus ? 1.08f : 1.0f;
            float to[2] = { b->base_scale.x * k, b->base_scale.y * k };
            br_anim_target target = br_anim_target_2d(buttons[i]);
            br_tween_kill_target(e, target);
            /* unscaled: the pause menu's buttons animate while game time is stopped */
            br_tween_set_unscaled(e, br_tween_to(e, target, "scale", to, 0.18f, (br_ease_type)t->focus_ease), true);
            br_node_set_tint(e, buttons[i], br_color_from_hex(focus ? 0xffffff : 0xd8d8e0, 1.0f));
        }
    }
    if (br_input_action_pressed(e, "confirm")) {
        cr_ui_activate(e, g, focused); /* may switch scenes: nothing of this frame's list is used after */
    } else if (br_input_action_pressed(e, "back") && g->state == CR_STATE_MENU) {
        cr_ui_do(e, g, br_scene_valid(e, g->overlay) ? CR_ACTION_CLOSE_SETTINGS : CR_ACTION_MAIN);
    } else if (br_input_action_pressed(e, "back") && g->state == CR_STATE_RESULTS) {
        cr_ui_do(e, g, CR_ACTION_MENU);
    }
}

/* ---- rebinding "jump" */

static void cr_capture_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    br_input_binding got;
    if (!g->capturing || !br_input_capture_poll(e, &got)) {
        return;
    }
    g->capturing = false;
    if (got.kind == BR_INPUT_BINDING_KEY && got.code == SDL_SCANCODE_ESCAPE) {
        return; /* Escape cancels */
    }
    const char *taken = br_input_binding_conflict(e, got, "jump");
    if (taken != NULL) {
        br_log_info("crate_rush: that input is already used by '%s'", taken);
        cr_ui_sound(e, g, "back");
        return;
    }
    /* replace jump's binding of the same device family (keyboard/mouse or pad) */
    bool pad = got.kind == BR_INPUT_BINDING_GAMEPAD_BUTTON || got.kind == BR_INPUT_BINDING_GAMEPAD_AXIS;
    uint32_t count = br_input_action_binding_count(e, "jump");
    for (uint32_t i = 0; i < count; i++) {
        br_input_binding old;
        br_input_action_binding_get(e, "jump", i, &old);
        bool old_pad = old.kind == BR_INPUT_BINDING_GAMEPAD_BUTTON || old.kind == BR_INPUT_BINDING_GAMEPAD_AXIS;
        if (old_pad == pad) {
            br_input_action_binding_set(e, "jump", i, got);
            cr_settings_save(e, g);
            cr_ui_sound(e, g, "confirm");
            return;
        }
    }
    br_input_action_bind(e, "jump", got);
    cr_settings_save(e, g);
}

/* ---- prompts, counters, settings rows */

static void cr_prompts_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    const br_spritesheet *glyphs = br_assets_get(e, "glyphs/glyphs.json");
    for (uint32_t i = 0; i < br_component_count(e, g->id.prompt); i++) {
        cr_prompt *p = br_component_at(e, g->id.prompt, i);
        br_node n = br_component_owner_at(e, g->id.prompt, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        if (p->key[0] != '\0') {
            /* the key's text in the current language, "{jump}" -> the glyph label of the device used last */
            char text[512];
            br_input_format_prompt(e, br_loc_get(e, p->key), text, sizeof(text));
            if (SDL_strcmp(text, br_text_text(e, n)) != 0) {
                br_text_set_text(e, n, text);
            }
            continue;
        }
        br_glyph glyph;
        if (!br_input_glyph(e, p->action, &glyph) || SDL_strcmp(glyph.frame, p->shown) == 0) {
            continue;
        }
        br_texture tex = br_spritesheet_frame(e, glyphs, glyph.frame);
        if (tex.index != 0) {
            br_sprite_set_texture(e, n, tex);
            br_str_copy(p->shown, sizeof(p->shown), glyph.frame);
        }
    }
}

static void cr_counters_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->id.counter); i++) {
        cr_counter *c = br_component_at(e, g->id.counter, i);
        br_node n = br_component_owner_at(e, g->id.counter, i);
        int32_t v = (int32_t)SDL_floorf(c->value + 0.5f);
        if (!br_node_valid(e, n) || (c->ready && v == c->shown)) {
            continue;
        }
        c->ready = true;
        c->shown = v;
        char text[64];
        SDL_snprintf(text, sizeof(text), c->format[0] != '\0' ? c->format : "%d", (int)v);
        br_text_set_text(e, n, text);
    }
}

static void cr_settings_rows_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    uint32_t ids[2] = { g->id.button, g->id.setting };
    br_query q = br_query_begin(e, ids, 2);
    while (br_query_next(&q)) {
        const cr_setting *s = q.data[1];
        char number[16];
        br_glyph glyph;
        const char *text = "";
        switch ((cr_setting_key)s->key) {
        case CR_SETTING_LANGUAGE:
            text = br_loc_get(e, "language.name"); /* each table names its own language */
            break;
        case CR_SETTING_VSYNC: {
            static const char *keys[] = { "settings.off", "settings.on", "settings.adaptive", "settings.mailbox" };
            text = br_loc_get(e, keys[SDL_clamp(g->settings.vsync, 0, 3)]);
            break;
        }
        case CR_SETTING_FPS:
            SDL_snprintf(number, sizeof(number), "%d", (int)g->settings.max_fps);
            text = g->settings.max_fps > 0 ? number : br_loc_get(e, "settings.uncapped");
            break;
        case CR_SETTING_VOLUME:
            SDL_snprintf(number, sizeof(number), "%d", (int)g->settings.volume); /* the font has no percent sign */
            text = number;
            break;
        case CR_SETTING_PROFILER:
            text = br_loc_get(e, g->settings.profiler_hud ? "settings.on" : "settings.off");
            break;
        case CR_SETTING_BIND_JUMP:
            if (g->capturing) {
                text = br_loc_get(e, "settings.press");
            } else if (br_input_glyph(e, "jump", &glyph)) {
                text = glyph.label;
            }
            break;
        case CR_SETTING_COUNT:
            break;
        }
        cr_ui_text(e, q.node, "value", text);
    }
    br_query_end(&q);
}

/* ---- HUD and loading screen */

static void cr_hud_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    br_scene hud = br_scene_find(e, "hud");
    if (!br_scene_is_loaded(e, hud)) {
        return;
    }
    br_node root = br_scene_root_2d(e, hud);
    char text[64];
    int32_t secs = (int32_t)SDL_ceilf(SDL_max(g->time_left, 0.0f));
    SDL_snprintf(text, sizeof(text), "%d:%02d", (int)(secs / 60), (int)(secs % 60));
    cr_ui_text(e, root, "timer", text);
    SDL_snprintf(text, sizeof(text), "%d / %d", (int)g->crates_taken, (int)g->crates_total);
    cr_ui_text(e, root, "crates", text);
    /* the level's title is a string-table key ("level1.title") */
    cr_ui_text(e, root, "level_title", g->level_title[0] != '\0' ? br_loc_get(e, g->level_title) : "");
    br_node timer = br_node_find_by_label(e, root, "timer", true);
    bool low = secs <= 10 && g->state == CR_STATE_PLAYING;
    float pulse = low ? 0.5f + 0.5f * SDL_sinf(g->time_left * 12.0f) : 0.0f;
    br_node_set_tint(e, timer, br_color_lerp(br_color_from_hex(0xffffff, 1.0f), br_color_from_hex(0xff5a4a, 1.0f),
                                             low ? 0.6f + 0.4f * pulse : 0.0f));
    /* the prompts sit at the right edge: a long jump word ("SPRINGEN") moves the jump prompt left
     * so it does not run into the pause prompt at x 150 (the layout's own spacing for "JUMP") */
    br_node jump = br_node_find_by_label(e, root, "jump_text", true);
    float shift = SDL_max(0.0f, 52.0f + br_text_size(e, jump).x + 20.0f - 150.0f);
    br_node_set_position(e, jump, br_vec2_make(52.0f - shift, 0.0f));
    br_node_set_position(e, br_node_find_by_label(e, root, "jump_glyph", true), br_vec2_make(-shift, 0.0f));
}

static void cr_loading_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    (void)user;
    br_scene loading = br_scene_find(e, "loading");
    if (!br_scene_is_loaded(e, loading)) {
        return;
    }
    br_node bar = br_node_find_by_label(e, br_scene_root_2d(e, loading), "bar_fill", true);
    br_nine_slice *ns = br_nine_slice_get(e, bar);
    if (ns == NULL) {
        return;
    }
    float progress = br_scene_progress(e, br_scene_switch_target(e));
    float width = 34.0f + (560.0f - 34.0f) * SDL_clamp(progress, 0.0f, 1.0f);
    /* ease toward the target so one big asset does not jump from empty to full */
    float shown = ns->width + (width - ns->width) * 0.25f;
    br_nine_slice_set_size(e, bar, shown, ns->height);
}

void cr_ui_register(br_engine *e, cr_game *g)
{
    br_scene_file_add_handler(e, "button_click", cr_on_button_click, g);
    br_scene_file_add_handler(e, "button_over", cr_on_button_over, g);
    br_system_add(e, "cr.capture", BR_PHASE_UPDATE, 5, cr_capture_system, g);
    br_system_add(e, "cr.ui", BR_PHASE_UPDATE, 10, cr_ui_system, g);
    br_system_add(e, "cr.loading", BR_PHASE_UPDATE, 50, cr_loading_system, g);
    br_system_add(e, "cr.prompts", BR_PHASE_LATE_UPDATE, 30, cr_prompts_system, g);
    br_system_add(e, "cr.counters", BR_PHASE_LATE_UPDATE, 40, cr_counters_system, g);
    br_system_add(e, "cr.settings", BR_PHASE_LATE_UPDATE, 50, cr_settings_rows_system, g);
    br_system_add(e, "cr.hud", BR_PHASE_LATE_UPDATE, 60, cr_hud_system, g);
}
