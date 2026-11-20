/* systems.c - every behaviour of Star Courier as systems over dense component arrays:
 *
 *   UPDATE  0  sc.input      actions / pointer -> pilot.intent
 *   UPDATE 10  sc.state      title -> play -> dying -> game over, pause, scene switches
 *   UPDATE 20  sc.spawner    crates, debris (difficulty curve), despawn
 *   UPDATE 30  sc.movement   pilots, bodies, station tween, starfield scroll
 *   UPDATE 40  sc.collision  circle tests -> events
 *   UPDATE 50  sc.rules      events -> cargo, score, lives, levels, sounds, bursts, flash
 *   UPDATE 60  sc.fx         particle emitters, hit flash filter, ship blink
 *   LATE   0   sc.hud        texts, life icons, pulses, blinks
 */
#include "game.h"

#define SC_PI 3.14159265f

/* The scene of each state, a name from game.json "scenes" (dying keeps the play scene). */
static const char *sc_scene_keys[] = { "title", "play", "", "gameover" };
static const char *sc_state_names[] = { "title", "play", "dying", "gameover" };

/* ---- small helpers */

float sc_random(sc_game *g)
{
    /* xorshift32: deterministic, so play-test runs give identical screenshots */
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return (float)(g->rng >> 8) / 16777216.0f;
}

static float sc_range(sc_game *g, float lo, float hi)
{
    return lo + (hi - lo) * sc_random(g);
}

const char *sc_config_string(br_engine *e, const char *section, const char *key)
{
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game");
    if (section != NULL) {
        game = cJSON_GetObjectItemCaseSensitive(game, section);
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(game, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

const sc_tuning *sc_tuning_get(br_engine *e, sc_game *g)
{
    static const sc_tuning none; /* all zero if tuning.json failed to load (already logged) */
    const sc_tuning *t = br_component_get(e, g->tuning_node, g->id.tuning);
    return t != NULL ? t : &none;
}

static br_node sc_find(br_engine *e, br_node root, const char *label)
{
    return br_node_find_by_label(e, root, label, true);
}

static void sc_play_sfx(br_engine *e, sc_game *g, const char *name)
{
    if (g->sfx == NULL) {
        return;
    }
    br_audio_play_desc play = br_audio_play_defaults();
    play.sprite = name;
    play.bus = g->sfx_bus;
    br_audio_play(e, g->sfx, &play);
}

/* A score / hint / level-up kick: scale jumps to base * (1 + kick) and eases back in 1 / decay
 * seconds (out-quad = the old hand-rolled base * (1 + kick * (1 - decay * t)^2)). */
static void sc_kick(br_engine *e, sc_game *g, br_node n)
{
    sc_pulse *p = br_component_get(e, n, g->id.pulse);
    if (p == NULL || p->kick <= 0.0f || p->decay <= 0.0f) {
        return;
    }
    if (!p->ready) {
        p->base = br_node_scale(e, n);
        p->ready = true;
    }
    br_anim_target t = br_anim_target_2d(n);
    float from[2] = { p->base.x * (1.0f + p->kick), p->base.y * (1.0f + p->kick) };
    float to[2] = { p->base.x, p->base.y };
    br_tween_kill_target(e, t);
    br_tween_from_to(e, t, "scale", from, to, 1.0f / p->decay, BR_EASE_OUT_QUAD);
}

static float sc_distance(br_vec2 a, br_vec2 b)
{
    return br_vec2_length(br_vec2_sub(a, b));
}

static br_vec2 sc_random_spot(sc_game *g, const sc_tuning *t, float margin)
{
    return br_vec2_make(sc_range(g, t->field_min.x + margin, t->field_max.x - margin),
                        sc_range(g, t->field_min.y + margin, t->field_max.y - margin));
}

void sc_log_stats(br_engine *e, const char *when)
{
    br_memory_stats heap;
    br_memory_get_stats(&heap);
    br_log_info("star_courier stats (%s): nodes %u, textures %u, assets %u (atlas refs %u), sounds playing %u, "
                "heap %llu blocks / %llu KB, data errors %u",
                when, br_node_count(e), br_texture_count(e), br_assets_count(e),
                br_assets_ref_count(e, "sprites/atlas.json"), br_audio_instance_count(e),
                (unsigned long long)heap.live_allocations, (unsigned long long)(heap.live_bytes / 1024),
                br_scene_file_error_count(e));
}

/* ---- particles: one emitter component per particle_container node */

static void sc_emitter_prepare(br_engine *e, br_node n, sc_emitter *em)
{
    if (em->ready) {
        return;
    }
    /* every slot is allocated once and reused: dead particles are invisible (alpha 0) */
    br_particle *p = br_particle_container_add(e, n, SC_EMITTER_MAX);
    for (uint32_t i = 0; p != NULL && i < SC_EMITTER_MAX; i++) {
        p[i].alpha = 0.0f;
        p[i].anchor_x = 0.5f;
        p[i].anchor_y = 0.5f;
        em->age[i] = em->life;
    }
    em->ready = true;
}

/* Starts one particle in a dead slot at `at` (container space). */
static void sc_emit(sc_game *g, sc_emitter *em, br_particle *p, uint32_t count, br_vec2 at, float direction,
                    br_color color)
{
    for (uint32_t i = 0; i < count && i < SC_EMITTER_MAX; i++) {
        if (em->age[i] < em->life) {
            continue;
        }
        float angle = direction + (sc_random(g) - 0.5f) * em->spread;
        float speed = sc_range(g, em->speed.x, em->speed.y);
        em->age[i] = sc_random(g) * 0.05f;
        em->velocity[i] = br_vec2_make(SDL_cosf(angle) * speed, SDL_sinf(angle) * speed);
        em->color[i] = color;
        p[i].x = at.x;
        p[i].y = at.y;
        p[i].rotation = angle;
        return;
    }
}

/* A burst of `count` particles of `color` from the play scene's burst emitter. */
static void sc_burst(br_engine *e, sc_game *g, br_vec2 world, int32_t count, br_color color)
{
    sc_emitter *em = br_component_get(e, g->burst_fx, g->id.emitter);
    if (em == NULL) {
        return;
    }
    sc_emitter_prepare(e, g->burst_fx, em);
    uint32_t n = 0;
    br_particle *p = br_particle_container_particles(e, g->burst_fx, &n);
    br_vec2 at = br_node_to_local(e, g->burst_fx, world);
    for (int32_t k = 0; p != NULL && k < count; k++) {
        sc_emit(g, em, p, n, at, 0.0f, color);
    }
}

/* ---- scenes and states */

static void sc_lookup(br_engine *e, sc_game *g)
{
    br_node s = g->scene;
    g->field = sc_find(e, s, "field");
    g->ship = sc_find(e, s, "ship");
    g->station = sc_find(e, s, "station");
    g->crates = sc_find(e, s, "crates");
    g->debris = sc_find(e, s, "debris");
    g->burst_fx = sc_find(e, s, "burst_fx");
    g->score_text = sc_find(e, s, "score_text");
    g->level_text = sc_find(e, s, "level_text");
    g->hint = sc_find(e, s, "hint");
    g->pause_panel = sc_find(e, s, "pause_panel");
    g->press_start = sc_find(e, s, "press_start");
    g->best_text = sc_find(e, s, "best_text");
    g->final_score = sc_find(e, s, "final_score");
    for (int i = 0; i < SC_MAX_LIFE_ICONS; i++) {
        char label[16];
        SDL_snprintf(label, sizeof(label), "life_%d", i + 1);
        g->life_icons[i] = sc_find(e, s, label);
    }
}

static void sc_save_best(br_engine *e, sc_game *g)
{
    if (br_app_deterministic(e)) {
        return; /* play-test runs must not change what the next run shows */
    }
    char text[32];
    SDL_snprintf(text, sizeof(text), "%d\n", (int)g->best);
    br_save_write(e, "best.txt", text, SDL_strlen(text));
}

void sc_enter_state(br_engine *e, sc_game *g, sc_state state)
{
    const sc_tuning *t = sc_tuning_get(e, g);
    if (sc_scene_keys[state][0] != '\0') {
        /* single: the previous screen and its asset references go; the tuning node is persistent */
        br_scene scene = br_scene_load(e, sc_scene_keys[state], BR_SCENE_SINGLE);
        g->scene = br_scene_root_2d(e, scene);
        sc_lookup(e, g);
    }
    g->state = state;
    br_str_copy(g->state_name, sizeof(g->state_name), sc_state_names[state]);
    g->state_time = 0.0f;
    g->wall_time = 0.0f;
    g->paused = false;
    g->event_count = 0;
    br_audio_bus_set_paused(e, g->sfx_bus, false);

    char text[64];
    if (state == SC_STATE_PLAY) {
        sc_log_stats(e, "round start"); /* the soak script compares these between rounds */
        g->round++;
        g->score = 0;
        g->lives = t->lives;
        g->level = 1;
        g->deliveries = 0;
        g->spawn_timer = 1.0f;
        g->flash = 0.0f;
        sc_play_sfx(e, g, "start");
    } else if (state == SC_STATE_DYING) {
        br_node_set_visible(e, g->ship, false);
        sc_burst(e, g, br_node_position(e, g->ship), t->hit_burst * 2, br_color_from_hex(0xff8a4c, 1.0f));
        sc_play_sfx(e, g, "gameover");
    } else if (state == SC_STATE_GAMEOVER) {
        bool record = g->score > g->best;
        if (record) {
            g->best = g->score;
            sc_save_best(e, g);
        }
        SDL_snprintf(text, sizeof(text), "%d", (int)g->score);
        br_loc_arg score = { "score", text };
        br_text_set_text(e, g->final_score, br_loc_format(e, "gameover.score", &score, 1));
        sc_kick(e, g, g->final_score);
        SDL_snprintf(text, sizeof(text), "%d", (int)g->best);
        br_text_set_text(e, g->best_text, record && g->score > 0 ? br_loc_get(e, "gameover.new_best")
                                                                 : br_loc_format(e, "gameover.best", &score, 1));
        br_log_info("star_courier: round %u over, score %d, level %d", g->round, (int)g->score, (int)g->level);
    }
    if (state == SC_STATE_TITLE && g->best > 0) {
        SDL_snprintf(text, sizeof(text), "%d", (int)g->best);
        br_loc_arg score = { "score", text };
        br_text_set_text(e, g->best_text, br_loc_format(e, "title.best", &score, 1));
    }
}

static void sc_state_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    g->wall_time += dt;
    if (!g->paused) {
        g->state_time += dt;
    }
    switch (g->state) {
    case SC_STATE_TITLE:
        if (g->assets_ready) {
            if (SDL_strcmp(br_loc_text_key(e, g->press_start), "title.press_start") != 0) {
                br_loc_bind_text(e, g->press_start, "title.press_start"); /* was "title.loading" */
            }
            if (br_input_action_pressed(e, "start")) {
                sc_enter_state(e, g, SC_STATE_PLAY);
            }
        }
        break;
    case SC_STATE_PLAY:
        if (br_input_action_pressed(e, "pause")) {
            g->paused = !g->paused;
            br_node_set_visible(e, g->pause_panel, g->paused);
            br_audio_bus_set_paused(e, g->sfx_bus, g->paused);
        }
        break;
    case SC_STATE_DYING:
        if (g->state_time >= sc_tuning_get(e, g)->gameover_delay) {
            sc_enter_state(e, g, SC_STATE_GAMEOVER);
        }
        break;
    case SC_STATE_GAMEOVER:
        if (g->state_time < 0.6f) {
            break; /* do not skip the screen with the key that was still held when the ship blew up */
        }
        if (br_input_action_pressed(e, "start")) {
            sc_enter_state(e, g, SC_STATE_PLAY);
        } else if (br_input_action_pressed(e, "pause")) {
            sc_enter_state(e, g, SC_STATE_TITLE);
        }
        break;
    }
}

static bool sc_gameplay(const sc_game *g)
{
    return g->state == SC_STATE_PLAY && !g->paused;
}

/* ---- input -> intent */

static void sc_input_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    sc_game *g = user;
    bool active = sc_gameplay(g);
    br_vec2 stick = br_vec2_make(br_input_action_value(e, "right") - br_input_action_value(e, "left"),
                                 br_input_action_value(e, "down") - br_input_action_value(e, "up"));
    bool pointing = br_input_action_down(e, "steer");
    br_vec2 pointer = br_input_mouse_position(e);
    br_touch touch;
    if (br_input_touch_get(e, 0, &touch)) {
        pointing = true;
        pointer = touch.position;
    }
    for (uint32_t i = 0; i < br_component_count(e, g->id.pilot); i++) {
        sc_pilot *pilot = br_component_at(e, g->id.pilot, i);
        br_node n = br_component_owner_at(e, g->id.pilot, i);
        br_vec2 intent = active ? stick : br_vec2_make(0.0f, 0.0f);
        if (active && pointing) {
            /* steer toward the pointer, full thrust from 120 units away */
            br_vec2 d = br_vec2_sub(pointer, br_node_position(e, n));
            float len = br_vec2_length(d);
            intent = len > 8.0f ? br_vec2_scale(d, SDL_min(1.0f, len / 120.0f) / len) : br_vec2_make(0.0f, 0.0f);
        }
        if (br_vec2_length(intent) > 1.0f) {
            intent = br_vec2_normalize(intent);
        }
        pilot->intent = intent;
    }
}

/* ---- spawner */

static void sc_spawn_crate(br_engine *e, sc_game *g, const sc_tuning *t)
{
    br_vec2 at = sc_random_spot(g, t, 40.0f);
    for (int tries = 0; tries < 12; tries++) {
        if (sc_distance(at, br_node_position(e, g->station)) > 160.0f &&
            sc_distance(at, br_node_position(e, g->ship)) > 160.0f) {
            break;
        }
        at = sc_random_spot(g, t, 40.0f);
    }
    br_node c = br_prefab_instantiate_at(e, t->crate_prefab, g->crates, at);
    sc_crate *crate = br_component_get(e, c, g->id.crate);
    if (crate != NULL) {
        crate->value = t->crate_value;
    }
}

static void sc_spawn_debris(br_engine *e, sc_game *g, const sc_tuning *t)
{
    int w = 1280;
    int h = 720;
    float roll = sc_random(g);
    const char *prefab = roll < 0.45f ? t->debris_small : roll < 0.8f ? t->debris_medium : t->debris_large;
    br_vec2 at;
    int edge = (int)(sc_random(g) * 4.0f);
    if (edge == 0) {
        at = br_vec2_make(-60.0f, sc_range(g, 0.0f, (float)h));
    } else if (edge == 1) {
        at = br_vec2_make((float)w + 60.0f, sc_range(g, 0.0f, (float)h));
    } else if (edge == 2) {
        at = br_vec2_make(sc_range(g, 0.0f, (float)w), -60.0f);
    } else {
        at = br_vec2_make(sc_range(g, 0.0f, (float)w), (float)h + 60.0f);
    }
    br_vec2 aim = sc_random_spot(g, t, 120.0f);
    float speed = sc_range(g, t->debris_speed.x, t->debris_speed.y) *
                  SDL_powf(t->debris_speed_scale, (float)(g->level - 1));
    br_node d = br_prefab_instantiate_at(e, prefab, g->debris, at);
    br_node_set_rotation(e, d, sc_range(g, 0.0f, 2.0f * SC_PI));
    sc_body *body = br_component_get(e, d, g->id.body);
    if (body != NULL) {
        body->velocity = br_vec2_scale(br_vec2_normalize(br_vec2_sub(aim, at)), speed);
        body->spin = sc_range(g, -1.5f, 1.5f);
    }
}

static void sc_spawner_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    if (!sc_gameplay(g)) {
        return;
    }
    const sc_tuning *t = sc_tuning_get(e, g);
    int32_t crates = 0;
    for (uint32_t i = 0; i < br_component_count(e, g->id.crate); i++) {
        crates += br_node_valid(e, br_component_owner_at(e, g->id.crate, i)) ? 1 : 0;
    }
    for (; crates < t->crates_on_field; crates++) {
        sc_spawn_crate(e, g, t);
    }

    int32_t debris = 0;
    for (uint32_t i = 0; i < br_component_count(e, g->id.debris); i++) {
        br_node n = br_component_owner_at(e, g->id.debris, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        br_vec2 p = br_node_position(e, n);
        if (p.x < -150.0f || p.x > 1430.0f || p.y < -150.0f || p.y > 870.0f) {
            br_node_destroy(e, n); /* deferred: the array keeps its entry until next frame */
        } else {
            debris++;
        }
    }
    g->spawn_timer -= dt;
    if (g->spawn_timer <= 0.0f) {
        float interval = t->debris_interval * SDL_powf(t->debris_interval_scale, (float)(g->level - 1));
        g->spawn_timer = SDL_max(interval, t->debris_interval_min);
        if (debris < t->debris_max) {
            sc_spawn_debris(e, g, t);
        }
    }
}

/* ---- movement */

static float sc_wrap_angle(float a)
{
    while (a > SC_PI) {
        a -= 2.0f * SC_PI;
    }
    while (a < -SC_PI) {
        a += 2.0f * SC_PI;
    }
    return a;
}

static void sc_movement_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    if (g->paused) {
        return;
    }
    const sc_tuning *t = sc_tuning_get(e, g);

    /* pilots: thrust from intent, speed cap, nose follows the velocity */
    for (uint32_t i = 0; i < br_component_count(e, g->id.pilot); i++) {
        sc_pilot *pilot = br_component_at(e, g->id.pilot, i);
        br_node n = br_component_owner_at(e, g->id.pilot, i);
        sc_body *body = br_component_get(e, n, g->id.body);
        if (body == NULL) {
            continue;
        }
        body->velocity = br_vec2_add(body->velocity, br_vec2_scale(pilot->intent, pilot->thrust * dt));
        float speed = br_vec2_length(body->velocity);
        if (speed > pilot->max_speed) {
            body->velocity = br_vec2_scale(body->velocity, pilot->max_speed / speed);
        }
        if (speed > 30.0f) {
            float heading = SDL_atan2f(body->velocity.y, body->velocity.x) + SC_PI * 0.5f;
            float r = br_node_rotation(e, n);
            br_node_set_rotation(e, n, r + sc_wrap_angle(heading - r) * SDL_min(1.0f, pilot->turn_rate * dt));
        }
    }

    /* every body: drag, integrate, spin, wrap */
    for (uint32_t i = 0; i < br_component_count(e, g->id.body); i++) {
        sc_body *body = br_component_at(e, g->id.body, i);
        br_node n = br_component_owner_at(e, g->id.body, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        body->velocity = br_vec2_scale(body->velocity, SDL_expf(-body->drag * dt));
        br_vec2 p = br_vec2_add(br_node_position(e, n), br_vec2_scale(body->velocity, dt));
        if (body->wrap) {
            p.x = p.x < -80.0f ? 1360.0f : p.x > 1360.0f ? -80.0f : p.x;
            p.y = p.y < -80.0f ? 800.0f : p.y > 800.0f ? -80.0f : p.y;
        }
        br_node_set_position(e, n, p);
        if (body->spin != 0.0f) {
            br_node_set_rotation(e, n, br_node_rotation(e, n) + body->spin * dt);
        }
    }

    /* pilots stay inside the play field */
    for (uint32_t i = 0; i < br_component_count(e, g->id.pilot); i++) {
        br_node n = br_component_owner_at(e, g->id.pilot, i);
        sc_body *body = br_component_get(e, n, g->id.body);
        br_vec2 p = br_node_position(e, n);
        br_vec2 c = br_vec2_make(SDL_clamp(p.x, t->field_min.x, t->field_max.x),
                                 SDL_clamp(p.y, t->field_min.y, t->field_max.y));
        if (body != NULL && c.x != p.x) {
            body->velocity.x = 0.0f;
        }
        if (body != NULL && c.y != p.y) {
            body->velocity.y = 0.0f;
        }
        br_node_set_position(e, n, c);
    }

    /* the station glides to its next spot between rounds */
    for (uint32_t i = 0; i < br_component_count(e, g->id.station); i++) {
        sc_station *st = br_component_at(e, g->id.station, i);
        br_node n = br_component_owner_at(e, g->id.station, i);
        if (!st->moving) {
            continue;
        }
        st->move_time += dt;
        float k = SDL_min(1.0f, st->move_time / SDL_max(t->station_move_time, 0.01f));
        k = k * k * (3.0f - 2.0f * k);
        br_node_set_position(e, n, br_vec2_make(st->from.x + (st->to.x - st->from.x) * k, st->from.y + (st->to.y - st->from.y) * k));
        st->moving = k < 1.0f;
    }

    /* starfield layers */
    for (uint32_t i = 0; i < br_component_count(e, g->id.scroll); i++) {
        sc_scroll *s = br_component_at(e, g->id.scroll, i);
        br_tiling_sprite *ts = br_tiling_sprite_get(e, br_component_owner_at(e, g->id.scroll, i));
        if (ts != NULL) {
            ts->tile_position = br_vec2_add(ts->tile_position, br_vec2_scale(s->velocity, dt));
        }
    }
}

/* ---- collision: circle tests over the dense arrays, results as events */

static void sc_add_event(sc_game *g, sc_event_kind kind, br_node other, br_vec2 at)
{
    if (g->event_count < SC_MAX_EVENTS) {
        sc_event ev = { kind, other, at };
        g->events[g->event_count++] = ev;
    }
}

static void sc_collision_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    sc_game *g = user;
    if (!sc_gameplay(g)) {
        return;
    }
    sc_pilot *pilot = br_component_get(e, g->ship, g->id.pilot);
    if (pilot == NULL) {
        return;
    }
    br_vec2 ship = br_node_position(e, g->ship);
    if (!pilot->carrying) {
        for (uint32_t i = 0; i < br_component_count(e, g->id.crate); i++) {
            sc_crate *c = br_component_at(e, g->id.crate, i);
            br_node n = br_component_owner_at(e, g->id.crate, i);
            if (br_node_valid(e, n) && sc_distance(ship, br_node_position(e, n)) < pilot->radius + c->radius) {
                sc_add_event(g, SC_EVENT_PICKUP, n, br_node_position(e, n));
                break;
            }
        }
    } else {
        sc_station *st = br_component_get(e, g->station, g->id.station);
        br_vec2 at = br_node_position(e, g->station);
        if (st != NULL && sc_distance(ship, at) < pilot->radius + st->radius) {
            sc_add_event(g, SC_EVENT_DELIVER, g->station, at);
        }
    }
    if (pilot->invulnerable > 0.0f) {
        return;
    }
    for (uint32_t i = 0; i < br_component_count(e, g->id.debris); i++) {
        sc_debris *d = br_component_at(e, g->id.debris, i);
        br_node n = br_component_owner_at(e, g->id.debris, i);
        /* 0.85: forgive grazes, the rocks are not perfect circles */
        if (br_node_valid(e, n) && sc_distance(ship, br_node_position(e, n)) < (pilot->radius + d->radius) * 0.85f) {
            sc_add_event(g, SC_EVENT_HIT, n, ship);
            break;
        }
    }
}

/* ---- rules: cargo, score, lives, difficulty */

static void sc_move_station(br_engine *e, sc_game *g, const sc_tuning *t)
{
    sc_station *st = br_component_get(e, g->station, g->id.station);
    if (st == NULL) {
        return;
    }
    br_vec2 here = br_node_position(e, g->station);
    br_vec2 to = sc_random_spot(g, t, 90.0f);
    for (int tries = 0; tries < 12 && sc_distance(to, here) < 380.0f; tries++) {
        to = sc_random_spot(g, t, 90.0f);
    }
    st->from = here;
    st->to = to;
    st->move_time = 0.0f;
    st->moving = true;
}

static void sc_rules_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    if (!sc_gameplay(g)) {
        return;
    }
    const sc_tuning *t = sc_tuning_get(e, g);
    sc_pilot *pilot = br_component_get(e, g->ship, g->id.pilot);
    if (pilot == NULL) {
        return;
    }
    pilot->invulnerable = SDL_max(0.0f, pilot->invulnerable - dt);
    for (uint32_t i = 0; i < g->event_count; i++) {
        const sc_event *ev = &g->events[i];
        if (ev->kind == SC_EVENT_PICKUP) {
            pilot->carrying = true;
            br_node_destroy(e, ev->other);
            sc_play_sfx(e, g, "pickup");
            sc_burst(e, g, ev->position, t->pickup_burst, br_color_from_hex(0xffb547, 1.0f));
            br_loc_bind_text(e, g->hint, "hud.hint_deliver");
            sc_kick(e, g, g->hint);
        } else if (ev->kind == SC_EVENT_DELIVER) {
            pilot->carrying = false;
            g->deliveries++;
            g->score += (int32_t)((float)t->crate_value * (1.0f + t->level_bonus * (float)(g->level - 1)));
            sc_kick(e, g, g->score_text);
            sc_play_sfx(e, g, "deliver");
            sc_burst(e, g, ev->position, t->deliver_burst, br_color_from_hex(0xa78bfa, 1.0f));
            sc_move_station(e, g, t);
            br_loc_bind_text(e, g->hint, "hud.hint_pickup");
            sc_kick(e, g, g->hint);
            if (t->deliveries_per_level > 0 && g->deliveries % t->deliveries_per_level == 0) {
                g->level++;
                sc_kick(e, g, g->level_text);
                sc_play_sfx(e, g, "levelup");
            }
        } else {
            g->lives--;
            pilot->invulnerable = t->invulnerable_time;
            g->flash = t->flash_time;
            br_node_destroy(e, ev->other);
            sc_play_sfx(e, g, "hit");
            sc_burst(e, g, ev->position, t->hit_burst, br_color_from_hex(0xff4060, 1.0f));
            br_input_gamepad_rumble(e, 0, 0.5f, 0.9f, 250);
            br_log_info("star_courier: hit at frame %llu, %d lives left", (unsigned long long)br_ticker_frame(e),
                        (int)g->lives);
            if (g->lives <= 0) {
                sc_enter_state(e, g, SC_STATE_DYING);
                return; /* sc_enter_state cleared the events */
            }
        }
    }
    g->event_count = 0;
}

/* ---- fx: emitters, hit flash, ship blink */

static void sc_emitters_update(br_engine *e, sc_game *g, float dt)
{
    for (uint32_t i = 0; i < br_component_count(e, g->id.emitter); i++) {
        sc_emitter *em = br_component_at(e, g->id.emitter, i);
        br_node n = br_component_owner_at(e, g->id.emitter, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        sc_emitter_prepare(e, n, em);
        uint32_t count = 0;
        br_particle *p = br_particle_container_particles(e, n, &count);
        if (p == NULL) {
            continue;
        }
        if (em->rate > 0.0f && br_node_valid(e, em->follow) && br_node_visible(e, em->follow)) {
            const sc_pilot *pilot = br_component_get(e, em->follow, g->id.pilot);
            bool thrusting = pilot != NULL && br_vec2_length(pilot->intent) > 0.1f;
            if (!em->needs_thrust || thrusting) {
                em->accumulator += em->rate * dt;
            }
            br_vec2 at = br_node_to_local(e, n, br_node_to_global(e, em->follow, em->offset));
            float direction = br_node_rotation(e, em->follow) + em->direction;
            for (; em->accumulator >= 1.0f; em->accumulator -= 1.0f) {
                sc_emit(g, em, p, count, at, direction, em->color_start);
            }
        }
        float damp = SDL_expf(-em->drag * dt);
        for (uint32_t k = 0; k < count && k < SC_EMITTER_MAX; k++) {
            if (em->age[k] >= em->life) {
                p[k].alpha = 0.0f;
                continue;
            }
            em->age[k] += dt;
            float f = SDL_min(1.0f, em->age[k] / em->life);
            em->velocity[k] = br_vec2_scale(em->velocity[k], damp);
            p[k].x += em->velocity[k].x * dt;
            p[k].y += em->velocity[k].y * dt;
            p[k].scale_x = p[k].scale_y = em->scale.x + (em->scale.y - em->scale.x) * f;
            p[k].tint = br_color_to_hex(br_color_lerp(em->color[k], em->color_end, f));
            p[k].alpha = 1.0f - f;
        }
    }
}

static void sc_fx_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    if (g->paused) {
        return;
    }
    sc_emitters_update(e, g, dt);

    /* hit flash: the field's data-defined blur + colour-matrix filters, off unless flashing */
    const sc_tuning *t = sc_tuning_get(e, g);
    br_filter *blur = br_filters_get(e, g->field, 0);
    br_filter *tint = br_filters_get(e, g->field, 1);
    if (blur != NULL && tint != NULL) {
        g->flash = SDL_max(0.0f, g->flash - dt);
        float k = t->flash_time > 0.0f ? g->flash / t->flash_time : 0.0f;
        blur->enabled = k > 0.0f;
        tint->enabled = k > 0.0f;
        blur->blur_x = blur->blur_y = t->flash_blur * k;
        blur->padding = blur->blur_x;
        br_color_matrix_tint(tint->matrix, t->flash_color, false);
        tint->alpha = 0.7f * k;
    }

    sc_pilot *pilot = br_component_get(e, g->ship, g->id.pilot);
    if (pilot != NULL) {
        bool blink = pilot->invulnerable > 0.0f && SDL_sinf(pilot->invulnerable * 40.0f) > 0.0f;
        br_node_set_alpha(e, g->ship, blink ? 0.3f : 1.0f);
        br_node_set_visible(e, pilot->cargo_icon, pilot->carrying);
    }
}

/* ---- hud */

static void sc_hud_system(br_engine *e, float dt, void *user)
{
    sc_game *g = user;
    char text[64];
    if (g->state == SC_STATE_PLAY || g->state == SC_STATE_DYING) {
        SDL_snprintf(text, sizeof(text), "%d", (int)g->score);
        br_loc_arg score = { "score", text };
        br_text_set_text(e, g->score_text, br_loc_format(e, "hud.score", &score, 1));
        SDL_snprintf(text, sizeof(text), "%d", (int)g->level);
        br_loc_arg level = { "level", text };
        br_text_set_text(e, g->level_text, br_loc_format(e, "hud.level", &level, 1));
        for (int i = 0; i < SC_MAX_LIFE_ICONS; i++) {
            br_node_set_visible(e, g->life_icons[i], i < g->lives);
        }
        sc_station *st = br_component_get(e, g->station, g->id.station);
        const sc_pilot *pilot = br_component_get(e, g->ship, g->id.pilot);
        if (st != NULL && pilot != NULL) {
            float glow = pilot->carrying ? 0.45f + 0.3f * SDL_sinf(g->state_time * 6.0f) : 0.18f;
            br_node_set_alpha(e, st->halo, glow);
        }
    }

    for (uint32_t i = 0; i < br_component_count(e, g->id.pulse); i++) {
        sc_pulse *p = br_component_at(e, g->id.pulse, i);
        br_node n = br_component_owner_at(e, g->id.pulse, i);
        if (!br_node_valid(e, n)) {
            continue;
        }
        if (!p->ready) {
            p->base = br_node_scale(e, n);
            p->ready = true;
        }
        if (p->wave == 0.0f) {
            continue; /* kick-only: a tween owns the scale (sc_kick) */
        }
        p->time += dt;
        float s = 1.0f + p->wave * SDL_sinf(p->time * p->speed);
        br_node_set_scale(e, n, br_vec2_make(p->base.x * s, p->base.y * s));
    }
    for (uint32_t i = 0; i < br_component_count(e, g->id.blink); i++) {
        sc_blink *b = br_component_at(e, g->id.blink, i);
        b->time += dt;
        float wave = 0.5f + 0.5f * SDL_cosf(b->time * 2.0f * SC_PI / SDL_max(b->period, 0.01f));
        br_node_set_alpha(e, br_component_owner_at(e, g->id.blink, i), b->min_alpha + (1.0f - b->min_alpha) * wave);
    }
}

void sc_systems_register(br_engine *e, sc_game *g)
{
    br_system_add(e, "sc.input", BR_PHASE_UPDATE, 0, sc_input_system, g);
    br_system_add(e, "sc.state", BR_PHASE_UPDATE, 10, sc_state_system, g);
    br_system_add(e, "sc.spawner", BR_PHASE_UPDATE, 20, sc_spawner_system, g);
    br_system_add(e, "sc.movement", BR_PHASE_UPDATE, 30, sc_movement_system, g);
    br_system_add(e, "sc.collision", BR_PHASE_UPDATE, 40, sc_collision_system, g);
    br_system_add(e, "sc.rules", BR_PHASE_UPDATE, 50, sc_rules_system, g);
    br_system_add(e, "sc.fx", BR_PHASE_UPDATE, 60, sc_fx_system, g);
    br_system_add(e, "sc.hud", BR_PHASE_LATE_UPDATE, 0, sc_hud_system, g);
}
