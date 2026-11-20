/* world.c - the level's things, all from glTF extras components:
 *
 *   FIXED_UPDATE 10  cr.pickups   player vs "crate" -> score, pop tween, "+10" popup
 *   FIXED_UPDATE 20  cr.hazards   player vs "hazard" -> knockback, time penalty, shake
 *   UPDATE       20  cr.hazard_motion  saws slide and spin
 *   LATE_UPDATE  20  cr.signs     "sign" name plates (canvas3d) react to the player
 *   UPDATE       30  cr.popups    "popup" rows count down and destroy their node
 *
 * World text is a canvas3d: a nine-slice plate from the UI atlas with SDF text (sharp at any
 * distance), billboarded.
 */
#include "game.h"

static br_texture cr_ui_frame(br_engine *e, const char *name, float borders[4])
{
    const br_spritesheet *sheet = br_assets_get(e, "ui/ui.json");
    if (borders != NULL && !br_spritesheet_frame_borders(e, sheet, name, borders)) {
        borders[0] = borders[1] = borders[2] = borders[3] = 12.0f;
    }
    return br_spritesheet_frame(e, sheet, name);
}

static br_text_style cr_world_style(cr_game *g, float size, uint32_t fill)
{
    br_text_style s = br_text_style_default();
    s.font = g->sdf_font;
    s.font_size = size;
    s.fill = br_color_from_hex(fill, 1.0f);
    s.align = BR_TEXT_ALIGN_CENTER;
    return s;
}

/* A floating text that rises and fades ("+10", "-3s"). */
static void cr_popup_text(br_engine *e, cr_game *g, br_vec3 at, const char *text, uint32_t fill)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    br_text_style style = cr_world_style(g, 56.0f, fill);
    style.stroke_width = 8.0f;
    style.stroke_color = br_color_from_hex(0x2a1d10, 1.0f);
    br_node3d n = br_text3d_create(e, text, &style, 120.0f);
    br_canvas3d_get(e, n)->billboard = BR_CANVAS3D_BILLBOARD_FULL;
    br_node3d_set_position(e, n, at);
    br_node3d_add_child(e, g->level_root, n);
    float to[3] = { at.x, at.y + t->popup_rise, at.z };
    br_tween_to(e, br_anim_target_3d(n), "position", to, t->popup_time, cr_ease(t->popup_ease));
    br_tween fade = br_tween_to_f(e, br_anim_target_2d(br_canvas3d_content(e, n)), "alpha", 0.0f, t->popup_time * 0.4f,
                                  BR_EASE_IN_QUAD);
    br_tween_set_delay(e, fade, t->popup_time * 0.6f);
    cr_popup *p = br_component3d_add(e, n, g->id.popup);
    if (p != NULL) {
        p->life = t->popup_time;
    }
}

/* Sizes a sign's plate (nine-slice + canvas) around its text. */
static void cr_sign_layout(br_engine *e, br_node3d plate, br_node text)
{
    br_vec2 size = br_text_size(e, text);
    float w = size.x + 70.0f;
    float h = size.y + 44.0f;
    br_nine_slice_set_size(e, br_canvas3d_content(e, plate), w, h);
    br_canvas3d_desc *d = br_canvas3d_get(e, plate);
    d->width = w;
    d->height = h;
    br_node_set_position(e, text, br_vec2_make(w * 0.5f, h * 0.5f));
}

/* ---- setup: name plates for the signs */

void cr_world_setup(br_engine *e, cr_game *g)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.sign); i++) {
        cr_sign *s = br_component3d_at(e, g->id.sign, i);
        br_node3d owner = br_component3d_owner_at(e, g->id.sign, i);
        float b[4];
        br_texture panel = cr_ui_frame(e, t->plate_texture, b);
        br_node3d plate = br_nine_slice3d_create(e, panel, b[0], b[1], b[2], b[3], 400.0f, 100.0f, 340.0f);
        br_canvas3d_get(e, plate)->billboard = BR_CANVAS3D_BILLBOARD_Y_AXIS;
        br_text_style style = cr_world_style(g, 34.0f, 0x3a2a1a);
        br_node text = br_text_create(e, s->text, &style);
        br_text_set_anchor(e, text, 0.5f, 0.5f);
        br_node_set_label(e, text, "sign_text");
        br_node_add_child(e, br_canvas3d_root(e, plate), text);
        cr_sign_layout(e, plate, text);
        br_vec3 at = br_node3d_world_position(e, owner);
        br_node3d_set_position(e, plate, br_vec3_make(at.x, at.y + 2.1f, at.z));
        br_node3d_set_label(e, plate, "sign_plate");
        br_node3d_add_child(e, g->level_root, plate);
        s = br_component3d_at(e, g->id.sign, i); /* creating nodes may move rows */
        s->plate = plate;
        s->near = false;
    }
}

/* ---- pickups */

static void cr_pickup(br_engine *e, cr_game *g, br_node3d n, cr_crate *c)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    c->taken = true;
    g->crates_taken++;
    g->score += c->value;
    br_vec3 at = br_node3d_world_position(e, n);
    /* pop: a quick grow, then shrink away while lifting (easings named in tuning.json) */
    br_anim_target target = br_anim_target_3d(n);
    float big[3] = { 1.45f, 1.45f, 1.45f };
    float gone[3] = { 0.0f, 0.0f, 0.0f };
    float lift[3] = { at.x, at.y + 0.6f, at.z };
    br_tween_sequence seq = br_tween_sequence_begin(0);
    br_tween_sequence_add(e, &seq, br_tween_to(e, target, "scale", big, t->pickup_time * 0.35f, cr_ease(t->pickup_ease)));
    br_tween_sequence_add(e, &seq, br_tween_to(e, target, "scale", gone, t->pickup_time * 0.65f, BR_EASE_IN_BACK));
    br_tween_sequence_join(e, &seq, br_tween_to(e, target, "position", lift, t->pickup_time * 0.65f, BR_EASE_OUT_QUAD));
    cr_popup *p = br_component3d_add(e, n, g->id.popup);
    if (p != NULL) {
        p->life = t->pickup_time + 0.05f;
    }
    char text[16];
    SDL_snprintf(text, sizeof(text), "+%d", (int)c->value);
    cr_popup_text(e, g, br_vec3_make(at.x, at.y + 0.9f, at.z), text, 0xffd34a);
    br_camera_shake_add(e, g->camera, t->shake_pickup);
    cr_sound(e, g, "pickup");
    cr_ui_count_to(e, g, br_scene_root_2d(e, br_scene_find(e, "hud")), "score_value", (float)g->score);
}

static void cr_pickups_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    const cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL || g->state != CR_STATE_PLAYING) {
        return;
    }
    const cr_tuning *t = cr_tuning_get(e, g);
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.crate); i++) {
        cr_crate *c = br_component3d_at(e, g->id.crate, i);
        br_node3d n = br_component3d_owner_at(e, g->id.crate, i);
        if (c->taken || !br_node3d_valid(e, n)) {
            continue;
        }
        br_vec3 at = br_node3d_world_position(e, n);
        float dx = at.x - p->position.x;
        float dz = at.z - p->position.z;
        float dy = at.y - p->position.y;
        if (dx * dx + dz * dz < t->pickup_radius * t->pickup_radius && dy > -0.6f && dy < 1.0f) {
            cr_pickup(e, g, n, c);
            return; /* cr_pickup adds components: the arrays may have moved */
        }
    }
}

/* ---- hazards */

void cr_world_hit(br_engine *e, cr_game *g, br_vec3 from)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL) {
        return;
    }
    br_vec3 away = br_vec3_sub(p->position, from);
    away.y = 0.0f;
    float len = br_vec3_length(away);
    away = len > 0.001f ? br_vec3_scale(away, 1.0f / len) : br_vec3_make(0.0f, 0.0f, 1.0f);
    p->velocity = br_vec3_make(away.x * t->knockback, t->jump_speed * 0.6f, away.z * t->knockback);
    p->grounded = false;
    p->invulnerable = t->invulnerable_time;
    g->time_left -= t->hit_penalty;
    g->hits++;
    br_camera_shake_add(e, g->camera, t->shake_hit);
    br_input_gamepad_rumble(e, br_input_last_gamepad(e), 0.6f, 0.9f, 220);
    cr_sound(e, g, "hit");
    char text[16];
    SDL_snprintf(text, sizeof(text), "-%d SEC", (int)t->hit_penalty);
    cr_popup_text(e, g, br_vec3_add(p->position, br_vec3_make(0.0f, 1.3f, 0.0f)), text, 0xff5a4a);
    br_log_info("crate_rush: hit at frame %llu", (unsigned long long)br_ticker_frame(e));
}

static void cr_hazards_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    const cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL || g->state != CR_STATE_PLAYING || p->invulnerable > 0.0f) {
        return;
    }
    const cr_tuning *t = cr_tuning_get(e, g);
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.hazard); i++) {
        const cr_hazard *h = br_component3d_at(e, g->id.hazard, i);
        br_node3d n = br_component3d_owner_at(e, g->id.hazard, i);
        if (!br_node3d_valid(e, n)) {
            continue;
        }
        br_vec3 at = br_node3d_world_position(e, n);
        float dx = at.x - p->position.x;
        float dz = at.z - p->position.z;
        float reach = h->radius + t->radius;
        if (dx * dx + dz * dz < reach * reach && at.y - p->position.y < 1.0f && p->position.y - at.y < 0.8f) {
            cr_world_hit(e, g, at);
            return;
        }
    }
}

static void cr_hazard_motion_system(br_engine *e, float dt, void *user)
{
    cr_game *g = user;
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.hazard); i++) {
        cr_hazard *h = br_component3d_at(e, g->id.hazard, i);
        br_node3d n = br_component3d_owner_at(e, g->id.hazard, i);
        if (!br_node3d_valid(e, n) || (h->speed == 0.0f && h->spin == 0.0f)) {
            continue;
        }
        if (!h->ready) {
            h->base = br_node3d_position(e, n);
            h->ready = true;
        }
        float s = SDL_sinf(g->level_time * h->speed + h->phase);
        br_node3d_set_position(e, n, br_vec3_add(h->base, br_vec3_scale(h->move, s)));
        if (h->spin != 0.0f) {
            br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 0.0f, 1.0f), h->spin * dt);
            br_node3d_set_rotation(e, n, br_quat_mul(br_node3d_rotation(e, n), turn));
        }
    }
}

/* ---- signs: a world-space plate that switches to its hint while the player is near */

static void cr_signs_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    const cr_player *p = br_component3d_get(e, g->player, g->id.player);
    for (uint32_t i = 0; p != NULL && i < br_component3d_count(e, g->id.sign); i++) {
        cr_sign *s = br_component3d_at(e, g->id.sign, i);
        if (!br_node3d_valid(e, s->plate)) {
            continue;
        }
        br_vec3 at = br_node3d_world_position(e, br_component3d_owner_at(e, g->id.sign, i));
        float dx = at.x - p->position.x;
        float dz = at.z - p->position.z;
        bool near = dx * dx + dz * dz < s->radius * s->radius;
        br_node text = br_node_find_by_label(e, br_canvas3d_root(e, s->plate), "sign_text", true);
        if (near) {
            char hint[160];
            br_input_format_prompt(e, s->hint, hint, sizeof(hint)); /* "{jump}" -> "Space" / "A" / "Cross" */
            if (SDL_strcmp(br_text_text(e, text), hint) != 0) {
                br_text_set_text(e, text, hint);
                cr_sign_layout(e, s->plate, text);
            }
        }
        if (near == s->near) {
            continue;
        }
        s->near = near;
        if (!near) {
            br_text_set_text(e, text, s->text);
            cr_sign_layout(e, s->plate, text);
        }
        br_canvas3d_get(e, s->plate)->tint = br_color_from_hex(near ? 0xffe9a8 : 0xffffff, 1.0f);
        float from[3] = { 1.25f, 1.25f, 1.25f };
        float to[3] = { 1.0f, 1.0f, 1.0f };
        br_tween_kill_target(e, br_anim_target_3d(s->plate));
        br_tween_from_to(e, br_anim_target_3d(s->plate), "scale", from, to, 0.35f, BR_EASE_OUT_BACK);
    }
}

/* ---- popups (and crates after their pop) */

static void cr_popups_system(br_engine *e, float dt, void *user)
{
    cr_game *g = user;
    for (uint32_t i = 0; i < br_component3d_count(e, g->id.popup); i++) {
        cr_popup *p = br_component3d_at(e, g->id.popup, i);
        br_node3d n = br_component3d_owner_at(e, g->id.popup, i);
        p->life -= dt;
        if (p->life <= 0.0f && br_node3d_valid(e, n)) {
            br_node3d_destroy(e, n); /* deferred: its row goes at the next flush */
        }
    }
}

void cr_world_register(br_engine *e, cr_game *g)
{
    br_system_add(e, "cr.pickups", BR_PHASE_FIXED_UPDATE, 10, cr_pickups_system, g);
    br_system_add(e, "cr.hazards", BR_PHASE_FIXED_UPDATE, 20, cr_hazards_system, g);
    br_system_add(e, "cr.hazard_motion", BR_PHASE_UPDATE, 20, cr_hazard_motion_system, g);
    br_system_add(e, "cr.popups", BR_PHASE_UPDATE, 30, cr_popups_system, g);
    br_system_add(e, "cr.signs", BR_PHASE_LATE_UPDATE, 20, cr_signs_system, g);
    br_system_set_group(e, "cr.pickups", "gameplay");
    br_system_set_group(e, "cr.hazards", "gameplay");
    br_system_set_group(e, "cr.hazard_motion", "gameplay");
}
