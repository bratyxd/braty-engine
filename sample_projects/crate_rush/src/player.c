/* player.c - the character: input, movement and collision on a fixed timestep, render
 * interpolation, facing, animator parameters and footstep sounds.
 *
 *   INPUT       100   cr.player_input    jump presses -> jump buffer (every frame, so none is lost)
 *   FIXED_UPDATE  0   cr.player_move     accelerate, gravity, jump, collide with the level boxes
 *   LATE_UPDATE -150  cr.player_present  interpolated position, facing, animator parameters
 *   LATE_UPDATE  10   cr.footsteps       "step" markers of the run clips -> footstep sounds
 *
 * Collision is deliberately simple: the player is a vertical cylinder (radius, height); every
 * solid collider is a world-space box from the level's "-colonly" nodes. A step looks at the
 * boxes near the player (candidate list in the frame arena), resolves X/Z, then Y.
 */
#include "game.h"

/* ---- spawning */

bool cr_player_spawn(br_engine *e, cr_game *g)
{
    const cr_tuning *t = cr_tuning_get(e, g);
    br_node3d inst = br_gltf_instantiate(e, t->character, g->level_root, NULL);
    if (!br_node3d_valid(e, inst)) {
        return false;
    }
    br_node3d_set_label(e, inst, "player");
    g->player = inst;
    cr_player *p = br_component3d_add(e, inst, g->id.player);
    if (p == NULL) {
        return false;
    }
    g->camera_target = br_node3d_create_group(e);
    br_node3d_set_label(e, g->camera_target, "camera_target");
    br_node3d_set_position(e, g->camera_target, br_vec3_make(0.0f, t->height, 0.0f));
    br_node3d_add_child(e, inst, g->camera_target);

    /* the state machine drives our copies of the model's clips (they carry the footstep markers) */
    g->animator = br_animator_add(e, br_anim_target_3d(inst));
    for (uint32_t i = 0; i < g->clip_count; i++) {
        br_animator_add_clip(e, g->animator, g->clips[i]);
    }
    br_animator_set_graph(e, g->animator, g->graph);
    cr_player_respawn(e, g);
    return true;
}

void cr_player_respawn(br_engine *e, cr_game *g)
{
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL) {
        return;
    }
    p->position = g->spawn;
    p->previous = g->spawn;
    p->velocity = br_vec3_make(0.0f, 0.0f, 0.0f);
    p->yaw = g->spawn_yaw;
    p->grounded = true;
    p->coyote = 0.0f;
    p->jump_buffer = 0.0f;
    br_node3d_set_position(e, g->player, g->spawn);
    br_node3d_set_rotation(e, g->player, br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), p->yaw));
}

/* ---- input */

static void cr_player_input_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p != NULL && g->state == CR_STATE_PLAYING && br_input_action_pressed(e, "jump")) {
        p->jump_buffer = cr_tuning_get(e, g)->jump_buffer;
    }
}

/* ---- movement and collision */

/* Indices of the colliders near the player (frame-arena scratch: gone next frame).
 * ponytail: a linear scan of every box (11-20 per level); a uniform grid if levels grow to hundreds. */
static uint32_t *cr_candidates(br_engine *e, cr_game *g, br_vec3 at, float reach, uint32_t *count)
{
    uint32_t *list = br_frame_alloc(e, sizeof(uint32_t) * (g->collider_count > 0 ? g->collider_count : 1));
    uint32_t n = 0;
    for (uint32_t i = 0; list != NULL && i < g->collider_count; i++) {
        const br_aabb *b = &g->colliders[i];
        if (at.x + reach > b->min.x && at.x - reach < b->max.x && at.z + reach > b->min.z && at.z - reach < b->max.z &&
            at.y + reach > b->min.y && at.y - reach < b->max.y) {
            list[n++] = i;
        }
    }
    *count = n;
    g->candidates_peak = SDL_max(g->candidates_peak, n);
    return list;
}

/* Closest point of a box's X/Z rectangle to (x, z). */
static void cr_closest_xz(const br_aabb *b, float x, float z, float *cx, float *cz)
{
    *cx = SDL_clamp(x, b->min.x, b->max.x);
    *cz = SDL_clamp(z, b->min.z, b->max.z);
}

/* ponytail: axis-aligned boxes only, X/Z then Y, no slopes or steps; sloped kit pieces would need
 * a height query (ray down against the meshes) and a step-up rule. */
static void cr_resolve_xz(cr_game *g, const cr_tuning *t, cr_player *p, const uint32_t *list, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        const br_aabb *b = &g->colliders[list[k]];
        /* only boxes the body is beside: not ones it stands on or passes over */
        if (b->max.y <= p->position.y + 0.05f || b->min.y >= p->position.y + t->height) {
            continue;
        }
        float cx = 0.0f;
        float cz = 0.0f;
        cr_closest_xz(b, p->position.x, p->position.z, &cx, &cz);
        float dx = p->position.x - cx;
        float dz = p->position.z - cz;
        float d2 = dx * dx + dz * dz;
        if (d2 >= t->radius * t->radius) {
            continue;
        }
        if (d2 > 1e-8f) {
            float d = SDL_sqrtf(d2);
            float push = t->radius - d;
            dx /= d;
            dz /= d;
            p->position.x += dx * push;
            p->position.z += dz * push;
            float into = p->velocity.x * dx + p->velocity.z * dz;
            if (into < 0.0f) {
                p->velocity.x -= dx * into;
                p->velocity.z -= dz * into;
            }
        } else {
            /* centre inside the box: leave by the nearest side */
            float left = p->position.x - b->min.x;
            float right = b->max.x - p->position.x;
            float back = p->position.z - b->min.z;
            float front = b->max.z - p->position.z;
            float m = SDL_min(SDL_min(left, right), SDL_min(back, front));
            if (m == left) {
                p->position.x = b->min.x - t->radius;
            } else if (m == right) {
                p->position.x = b->max.x + t->radius;
            } else if (m == back) {
                p->position.z = b->min.z - t->radius;
            } else {
                p->position.z = b->max.z + t->radius;
            }
        }
    }
}

static void cr_resolve_y(cr_game *g, const cr_tuning *t, cr_player *p, float previous_y, const uint32_t *list, uint32_t n)
{
    bool was_grounded = p->grounded;
    p->grounded = false;
    /* feet need real support: a narrower circle than the body, so edges are not ledges */
    float support = t->radius * 0.6f;
    for (uint32_t k = 0; k < n; k++) {
        const br_aabb *b = &g->colliders[list[k]];
        float cx = 0.0f;
        float cz = 0.0f;
        cr_closest_xz(b, p->position.x, p->position.z, &cx, &cz);
        float dx = p->position.x - cx;
        float dz = p->position.z - cz;
        bool over = dx * dx + dz * dz < support * support;
        if (!over) {
            continue;
        }
        if (p->velocity.y <= 0.0f && previous_y >= b->max.y - 0.05f && p->position.y <= b->max.y + 0.02f) {
            p->position.y = b->max.y;
            p->velocity.y = 0.0f;
            p->grounded = true;
        } else if (p->velocity.y > 0.0f && previous_y + t->height <= b->min.y && p->position.y + t->height > b->min.y) {
            p->position.y = b->min.y - t->height;
            p->velocity.y = 0.0f;
        }
    }
    p->landed = p->grounded && !was_grounded;
}

static void cr_player_move_system(br_engine *e, float dt, void *user)
{
    cr_game *g = user;
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL || (g->state != CR_STATE_PLAYING && g->state != CR_STATE_LEVEL_END)) {
        return;
    }
    const cr_tuning *t = cr_tuning_get(e, g);
    p->previous = p->position;

    /* the camera looks down -Z, so stick up = -Z: screen-relative without any rotation */
    br_vec2 stick = g->state == CR_STATE_PLAYING ? br_input_action_vec2(e, "move_left", "move_right", "move_up", "move_down")
                                                 : br_vec2_make(0.0f, 0.0f);
    float want_x = stick.x * t->run_speed;
    float want_z = stick.y * t->run_speed;
    float rate = p->grounded ? (br_vec2_length(stick) > 0.05f ? t->accel : t->decel) : t->air_accel;
    float k = SDL_min(1.0f, rate * dt);
    p->velocity.x += (want_x - p->velocity.x) * k;
    p->velocity.z += (want_z - p->velocity.z) * k;

    /* jump: buffered presses, a little coyote time after walking off an edge */
    p->coyote = p->grounded ? t->coyote_time : SDL_max(0.0f, p->coyote - dt);
    p->jump_buffer = SDL_max(0.0f, p->jump_buffer - dt);
    p->jumped = false;
    if (p->jump_buffer > 0.0f && p->coyote > 0.0f) {
        p->velocity.y = t->jump_speed;
        p->jump_buffer = 0.0f;
        p->coyote = 0.0f;
        p->grounded = false;
        p->jumped = true;
    }
    p->velocity.y = SDL_max(p->velocity.y - t->gravity * dt, -t->max_fall);

    float speed = br_vec3_length(p->velocity);
    uint32_t n = 0;
    uint32_t *list = cr_candidates(e, g, p->position, t->radius + t->height + speed * dt + 0.5f, &n);
    p->position.x += p->velocity.x * dt;
    p->position.z += p->velocity.z * dt;
    cr_resolve_xz(g, t, p, list, n);
    float previous_y = p->position.y;
    p->position.y += p->velocity.y * dt;
    cr_resolve_y(g, t, p, previous_y, list, n);
    p->invulnerable = SDL_max(0.0f, p->invulnerable - dt);

    if (p->position.y < g->kill_y) {
        /* into the water: back to the start, minus some time */
        g->time_left -= t->fall_penalty;
        g->falls++;
        cr_player_respawn(e, g);
        br_camera_shake_add(e, g->camera, t->shake_fall);
        cr_sound(e, g, "fall");
        br_log_info("crate_rush: fell in at frame %llu", (unsigned long long)br_ticker_frame(e));
    }
}

/* ---- presentation: interpolated position, facing, animator parameters */

static float cr_wrap(float a)
{
    while (a > BR_PI) {
        a -= 2.0f * BR_PI;
    }
    while (a < -BR_PI) {
        a += 2.0f * BR_PI;
    }
    return a;
}

static void cr_player_present_system(br_engine *e, float dt, void *user)
{
    cr_game *g = user;
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    if (p == NULL || g->state == CR_STATE_PAUSED) {
        return;
    }
    const cr_tuning *t = cr_tuning_get(e, g);
    float alpha = br_app_fixed_alpha(e);
    br_node3d_set_position(e, g->player, br_vec3_lerp(p->previous, p->position, alpha));
    float flat = SDL_sqrtf(p->velocity.x * p->velocity.x + p->velocity.z * p->velocity.z);
    if (flat > 0.4f) {
        float want = SDL_atan2f(p->velocity.x, p->velocity.z); /* the model faces +Z */
        p->yaw = cr_wrap(p->yaw + cr_wrap(want - p->yaw) * SDL_min(1.0f, t->turn_rate * dt));
    }
    br_node3d_set_rotation(e, g->player, br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), p->yaw));
    /* blink while invulnerable */
    br_node3d_set_visible(e, g->player, p->invulnerable <= 0.0f || SDL_fmodf(p->invulnerable, 0.16f) > 0.06f);

    br_animator_set_float(e, g->animator, "speed", t->run_speed > 0.0f ? flat / t->run_speed : 0.0f);
    br_animator_set_bool(e, g->animator, "grounded", p->grounded);
    br_animator_set_float(e, g->animator, "vy", p->velocity.y);
    const char *state = br_animator_state(e, g->animator);
    br_str_copy(g->anim_state, sizeof(g->anim_state), state != NULL ? state : "");
    /* the run clips are authored for a slower walk: play them faster with speed so feet keep up */
    float run = SDL_strcmp(g->anim_state, "run") == 0 && t->run_speed > 0.0f ? flat / t->run_speed : 0.0f;
    br_animator_set_speed(e, g->animator, 1.0f + (t->run_anim_speed - 1.0f) * SDL_min(run, 1.0f));
    if (p->jumped) {
        br_animator_set_trigger(e, g->animator, "jump");
        p->jumped = false;
        cr_sound(e, g, "jump");
    }
    if (p->landed) {
        p->landed = false;
        cr_sound(e, g, "land");
    }
}

/* ---- footsteps: the "step" markers of the walk / sprint clips (animation events) */

static void cr_footsteps_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    cr_game *g = user;
    cr_player *p = br_component3d_get(e, g->player, g->id.player);
    uint32_t count = 0;
    const br_anim_event *events = br_animation_events(e, &count);
    for (uint32_t i = 0; p != NULL && i < count; i++) {
        if (events[i].animator.index != g->animator.index || SDL_strcmp(events[i].name, "step") != 0 || !p->grounded) {
            continue;
        }
        char path[96];
        g->footstep_index = (g->footstep_index + 1 + (uint32_t)(cr_random(g) * 3.0f)) % 5;
        SDL_snprintf(path, sizeof(path), "%s_%03u.ogg", g->footsteps, g->footstep_index);
        cr_play(e, g, path, 0.5f);
    }
}

void cr_player_register(br_engine *e, cr_game *g)
{
    br_system_add(e, "cr.player_input", BR_PHASE_INPUT, 100, cr_player_input_system, g);
    br_system_add(e, "cr.player_move", BR_PHASE_FIXED_UPDATE, 0, cr_player_move_system, g);
    br_system_set_group(e, "cr.player_move", "gameplay");
    br_system_add(e, "cr.player_present", BR_PHASE_LATE_UPDATE, -150, cr_player_present_system, g);
    br_system_add(e, "cr.footsteps", BR_PHASE_LATE_UPDATE, 10, cr_footsteps_system, g);
}
