/* test_animation - the unified animation system, headless: interpolation maths for every mode,
 * loop modes, cross-fade weights, layer masks and additive layers, 1D/2D blend tree weights,
 * events (exactly once, across loops and large steps), queue, state machines, root motion,
 * bindings to destroyed nodes / missing properties (one warning, the rest keeps playing),
 * the property registry round trip for every built-in property, anim.json parsing, flipbook
 * and glTF clip sources, and determinism. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_log.h>

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

/* ---- warnings are counted through SDL's log output */

static int g_warnings = 0;
static SDL_LogOutputFunction g_prev_log = NULL;
static void *g_prev_log_user = NULL;

static void count_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user;
    if (priority == SDL_LOG_PRIORITY_WARN && SDL_strstr(message, "animation:") != NULL) {
        g_warnings++;
    }
    if (g_prev_log != NULL) {
        g_prev_log(g_prev_log_user, category, priority, message);
    }
}

/* ---- helpers */

static br_node make_node(br_engine *e, br_node parent, const char *label)
{
    br_node n = br_node_create_container(e);
    br_node_set_label(e, n, label);
    br_node_add_child(e, parent, n);
    return n;
}

/* A clip with one linear 1-float track from a to b over `duration`. */
static br_anim_clip *ramp(const char *name, const char *path, float a, float b, float duration, br_anim_loop loop)
{
    br_anim_clip *c = br_anim_clip_create(name, 0.0f, loop);
    float t[2] = { 0.0f, duration };
    float v[2] = { a, b };
    br_anim_clip_add_track(c, path, BR_ANIM_LINEAR, 1, 2, t, v);
    return c;
}

static float node_x(br_engine *e, br_node n)
{
    return br_node_position(e, n).x;
}

/* ---- interpolation maths */

static void test_sampling(void)
{
    br_anim_clip *c = br_anim_clip_create("maths", 0.0f, BR_ANIM_LOOP);
    float out[4];
    uint32_t hint = 0;

    float st[3] = { 0.0f, 1.0f, 2.0f };
    float sv[3] = { 1.0f, 2.0f, 3.0f };
    br_anim_track *step = br_anim_clip_add_track(c, "a:x", BR_ANIM_STEP, 1, 3, st, sv);
    br_anim_track_sample(step, 0.99f, false, &hint, out);
    BR_CHECK(near_eq(out[0], 1.0f));
    br_anim_track_sample(step, 1.5f, false, &hint, out);
    BR_CHECK(near_eq(out[0], 2.0f) && hint == 1);
    br_anim_track_sample(step, 9.0f, false, &hint, out);
    BR_CHECK(near_eq(out[0], 3.0f));

    float lt[3] = { 1.0f, 3.0f, 4.0f };
    float lv[3] = { 0.0f, 10.0f, 0.0f };
    br_anim_track *lin = br_anim_clip_add_track(c, "a:x", BR_ANIM_LINEAR, 1, 3, lt, lv);
    hint = 0;
    br_anim_track_sample(lin, 2.0f, false, &hint, out);
    BR_CHECK(near_eq(out[0], 5.0f));
    br_anim_track_sample(lin, 3.5f, false, &hint, out); /* next segment through the cached hint */
    BR_CHECK(near_eq(out[0], 5.0f) && hint == 1);
    br_anim_track_sample(lin, 1.5f, false, &hint, out); /* backwards: binary search */
    BR_CHECK(near_eq(out[0], 2.5f) && hint == 0);
    br_anim_track_sample(lin, 0.0f, false, NULL, out);  /* clamped before the first key */
    BR_CHECK(near_eq(out[0], 0.0f));
    lin->ease = BR_EASE_IN_QUAD;
    br_anim_track_sample(lin, 2.0f, false, NULL, out);  /* u = 0.5 eased to 0.25 */
    BR_CHECK(near_eq(out[0], 2.5f));

    /* cubic (glTF layout in, value, out): flat tangents = smoothstep */
    float ct[2] = { 0.0f, 1.0f };
    float cv[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    br_anim_track *cubic = br_anim_clip_add_track(c, "a:x", BR_ANIM_CUBIC, 1, 2, ct, cv);
    br_anim_track_sample(cubic, 0.5f, false, NULL, out);
    BR_CHECK(near_eq(out[0], 0.5f));
    br_anim_track_sample(cubic, 0.25f, false, NULL, out);
    BR_CHECK(near_eq(out[0], 0.15625f));

    /* bezier (in-handle, value, out-handle): handles at 1/3 and 2/3 make a straight line */
    float bv[6] = { 0.0f, 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f, 1.0f };
    br_anim_track *bez = br_anim_clip_add_track(c, "a:x", BR_ANIM_BEZIER, 1, 2, ct, bv);
    br_anim_track_sample(bez, 0.3f, false, NULL, out);
    BR_CHECK(near_eq(out[0], 0.3f));
    float bv2[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f }; /* handles on the keys: ease in-out */
    br_anim_track *bez2 = br_anim_clip_add_track(c, "a:x", BR_ANIM_BEZIER, 1, 2, ct, bv2);
    br_anim_track_sample(bez2, 0.25f, false, NULL, out);
    BR_CHECK(near_eq(out[0], 0.15625f));

    /* quaternions: slerp along the shortest path, normalised */
    float qv[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.70710678f, 0.0f, 0.70710678f };
    br_anim_track *rot = br_anim_clip_add_track(c, "a:rotation", BR_ANIM_LINEAR, 4, 2, ct, qv);
    br_anim_track_sample(rot, 0.5f, true, NULL, out);
    br_quat want = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.25f);
    BR_CHECK(near_eq(SDL_fabsf(out[0] * want.x + out[1] * want.y + out[2] * want.z + out[3] * want.w), 1.0f));
    float qneg[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -0.70710678f, 0.0f, -0.70710678f }; /* same rotation, flipped */
    br_anim_track *rneg = br_anim_clip_add_track(c, "a:rotation", BR_ANIM_LINEAR, 4, 2, ct, qneg);
    br_anim_track_sample(rneg, 0.5f, true, NULL, out);
    BR_CHECK(near_eq(SDL_fabsf(out[0] * want.x + out[1] * want.y + out[2] * want.z + out[3] * want.w), 1.0f));

    BR_CHECK(near_eq(c->duration, 4.0f)); /* grows to the last key */
    BR_CHECK(br_anim_clip_add_marker(c, 0.5f, "hit") && c->marker_count == 1);
    br_anim_clip_destroy(c);
}

/* ---- loop modes */

static void test_loop_modes(br_engine *e)
{
    br_node root = make_node(e, br_scene_root(e), "loops");
    br_anim_clip *clip = ramp("ramp", ":x", 0.0f, 10.0f, 1.0f, BR_ANIM_LOOP);
    br_animator a = br_animator_add(e, br_anim_target_2d(root));
    BR_CHECK(br_animator_valid(e, a) && br_animator_add_clip(e, a, clip));
    BR_CHECK(br_animator_get(e, br_anim_target_2d(root)).index == a.index);
    BR_CHECK(!br_animator_play(e, a, "nope"));
    BR_CHECK(br_animator_play(e, a, "ramp"));

    br_engine_step(e, 0.25f);
    BR_CHECK(near_eq(node_x(e, root), 2.5f));
    br_engine_step(e, 1.0f); /* loop: 1.25 -> 0.25 */
    BR_CHECK(near_eq(node_x(e, root), 2.5f) && near_eq(br_animator_time(e, a, 0), 0.25f));

    br_animator_set_loop(e, a, 0, BR_ANIM_PING_PONG);
    br_engine_step(e, 1.0f); /* 0.25 -> 1 -> back to 0.75 */
    BR_CHECK(near_eq(node_x(e, root), 7.5f));
    br_engine_step(e, 0.5f); /* backwards: 0.25 */
    BR_CHECK(near_eq(node_x(e, root), 2.5f));

    br_animator_play(e, a, "ramp");
    br_animator_set_loop(e, a, 0, BR_ANIM_CLAMP);
    br_engine_step(e, 5.0f);
    BR_CHECK(near_eq(node_x(e, root), 10.0f) && br_animator_finished(e, a, 0));
    br_node_set_position(e, root, 3.0f, 0.0f);
    br_engine_step(e, 0.1f); /* clamp keeps writing the end */
    BR_CHECK(near_eq(node_x(e, root), 10.0f));

    br_animator_play(e, a, "ramp");
    br_animator_set_loop(e, a, 0, BR_ANIM_ONCE);
    br_engine_step(e, 5.0f);
    BR_CHECK(near_eq(node_x(e, root), 10.0f) && br_animator_finished(e, a, 0));
    br_engine_step(e, 0.1f);
    br_node_set_position(e, root, 3.0f, 0.0f);
    br_engine_step(e, 0.1f); /* once: stopped writing */
    BR_CHECK(near_eq(node_x(e, root), 3.0f) && br_animator_current(e, a, 0) == NULL);

    /* negative speed plays backwards from the end */
    br_animator_set_speed(e, a, -1.0f);
    br_animator_play(e, a, "ramp");
    br_animator_set_loop(e, a, 0, BR_ANIM_LOOP);
    br_engine_step(e, 0.25f);
    BR_CHECK(near_eq(node_x(e, root), 7.5f));
    br_animator_set_speed(e, a, 1.0f);

    /* queue: B starts when the ONCE clip A ends */
    br_anim_clip *b = ramp("b", ":x", 100.0f, 200.0f, 1.0f, BR_ANIM_LOOP);
    br_animator_add_clip(e, a, b);
    br_animator_play(e, a, "ramp");
    br_animator_set_loop(e, a, 0, BR_ANIM_ONCE);
    BR_CHECK(br_animator_queue(e, a, "b", 0.0f));
    br_engine_step(e, 0.5f);
    BR_CHECK(SDL_strcmp(br_animator_current(e, a, 0), "ramp") == 0);
    br_engine_step(e, 0.6f); /* A ends */
    br_engine_step(e, 0.5f); /* B runs */
    BR_CHECK(SDL_strcmp(br_animator_current(e, a, 0), "b") == 0 && near_eq(node_x(e, root), 150.0f));

    br_node_destroy(e, root);
    br_engine_step(e, 0.0f);
    BR_CHECK(!br_animator_valid(e, a)); /* removed with its node */
    br_anim_clip_destroy(clip);
    br_anim_clip_destroy(b);
}

/* ---- cross-fades (2D floats and 3D quaternions) */

static void test_cross_fade(br_engine *e)
{
    br_node n = make_node(e, br_scene_root(e), "fader");
    br_anim_clip *lo = ramp("lo", ":x", 0.0f, 0.0f, 1.0f, BR_ANIM_LOOP);
    br_anim_clip *hi = ramp("hi", ":x", 10.0f, 10.0f, 1.0f, BR_ANIM_LOOP);
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    br_animator_add_clip(e, a, lo);
    br_animator_add_clip(e, a, hi);
    br_animator_play(e, a, "lo");
    br_engine_step(e, 0.1f);
    BR_CHECK(br_animator_cross_fade(e, a, "hi", 1.0f));
    float sum_ok = 1;
    for (int i = 1; i <= 3; i++) {
        br_engine_step(e, 0.25f);
        float f = br_animator_fade_progress(e, a, 0);
        /* weights (1 - f) of "lo" and f of "hi" sum to one: x = 0 * (1 - f) + 10 * f */
        sum_ok = sum_ok && near_eq(f, 0.25f * (float)i) && near_eq(node_x(e, n), 10.0f * f);
    }
    BR_CHECK(sum_ok);
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(node_x(e, n), 10.0f) && near_eq(br_animator_fade_progress(e, a, 0), 1.0f));
    BR_CHECK(br_animator_cross_fade(e, a, "hi", 1.0f)); /* already playing: nothing changes */
    BR_CHECK(near_eq(br_animator_fade_progress(e, a, 0), 1.0f));

    /* 3D: a rotation fade is a slerp */
    br_node3d n3 = br_node3d_create_group(e);
    br_anim_clip *r0 = br_anim_clip_create("r0", 0.0f, BR_ANIM_LOOP);
    br_anim_clip *r1 = br_anim_clip_create("r1", 0.0f, BR_ANIM_LOOP);
    float t[1] = { 0.0f };
    float q0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float q1[4] = { 0.0f, 1.0f, 0.0f, 0.0f }; /* 180 degrees about y */
    br_anim_clip_add_track(r0, "rotation", BR_ANIM_LINEAR, 4, 1, t, q0);
    br_anim_clip_add_track(r1, "rotation", BR_ANIM_LINEAR, 4, 1, t, q1);
    r0->duration = r1->duration = 1.0f;
    br_animator a3 = br_animator_add(e, br_anim_target_3d(n3));
    br_animator_add_clip(e, a3, r0);
    br_animator_add_clip(e, a3, r1);
    br_animator_play(e, a3, "r0");
    br_engine_step(e, 0.1f);
    br_animator_cross_fade(e, a3, "r1", 1.0f);
    br_engine_step(e, 0.5f);
    br_quat q = br_node3d_rotation(e, n3);
    br_quat want = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.5f);
    BR_CHECK(near_eq(SDL_fabsf(br_quat_dot(q, want)), 1.0f));
    br_node3d_destroy(e, n3);
    br_node_destroy(e, n);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(lo);
    br_anim_clip_destroy(hi);
    br_anim_clip_destroy(r0);
    br_anim_clip_destroy(r1);
}

/* ---- layers: masks, weights, additive */

static void test_layers(br_engine *e)
{
    br_node rig = make_node(e, br_scene_root(e), "rig");
    br_node upper = make_node(e, rig, "upper");
    br_node arm = make_node(e, upper, "arm");
    br_node leg = make_node(e, make_node(e, rig, "lower"), "leg");
    br_anim_clip *base = br_anim_clip_create("base", 1.0f, BR_ANIM_LOOP);
    br_anim_clip *wave = br_anim_clip_create("wave", 1.0f, BR_ANIM_LOOP);
    br_anim_clip *bob = br_anim_clip_create("bob", 1.0f, BR_ANIM_LOOP);
    float t2[2] = { 0.0f, 1.0f };
    float one[2] = { 1.0f, 1.0f };
    float five[2] = { 5.0f, 5.0f };
    float add[2] = { 0.0f, 4.0f };
    br_anim_clip_add_track(base, "upper/arm:rotation", BR_ANIM_LINEAR, 1, 2, t2, one);
    br_anim_clip_add_track(base, "leg:rotation", BR_ANIM_LINEAR, 1, 2, t2, one); /* found below "lower" */
    br_anim_clip_add_track(wave, "upper/arm:rotation", BR_ANIM_LINEAR, 1, 2, t2, five);
    br_anim_clip_add_track(wave, "lower/leg:rotation", BR_ANIM_LINEAR, 1, 2, t2, five);
    br_anim_clip_add_track(bob, "lower/leg:rotation", BR_ANIM_LINEAR, 1, 2, t2, add);
    br_animator a = br_animator_add(e, br_anim_target_2d(rig));
    br_animator_add_clip(e, a, base);
    br_animator_add_clip(e, a, wave);
    br_animator_add_clip(e, a, bob);
    br_animator_play(e, a, "base");
    BR_CHECK(br_animator_set_layer(e, a, 1, 1.0f, false));
    BR_CHECK(br_animator_layer_add_mask(e, a, 1, "upper"));
    BR_CHECK(!br_animator_layer_add_mask(e, a, 1, "nobody"));
    BR_CHECK(br_animator_play_layer(e, a, 1, "wave", 0.0f));
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(br_node_rotation(e, arm), 5.0f)); /* upper body: the layer */
    BR_CHECK(near_eq(br_node_rotation(e, leg), 1.0f)); /* lower body: masked out */
    br_animator_set_layer(e, a, 1, 0.5f, false);
    br_engine_step(e, 0.1f);
    BR_CHECK(near_eq(br_node_rotation(e, arm), 3.0f));
    /* additive layer 2 adds the change from the clip's first key */
    BR_CHECK(br_animator_set_layer(e, a, 2, 1.0f, true));
    br_animator_play_layer(e, a, 2, "bob", 0.0f);
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(br_node_rotation(e, leg), 1.0f + 2.0f));
    br_animator_set_layer(e, a, 2, 0.5f, true);
    br_engine_step(e, 0.0f);
    BR_CHECK(near_eq(br_node_rotation(e, leg), 1.0f + 1.0f));
    BR_CHECK(!br_animator_set_layer(e, a, 0, 1.0f, false) && !br_animator_set_layer(e, a, 9, 1.0f, false));
    br_node_destroy(e, rig);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(base);
    br_anim_clip_destroy(wave);
    br_anim_clip_destroy(bob);
}

/* ---- blend trees */

static void test_blend_trees(br_engine *e)
{
    br_node n = make_node(e, br_scene_root(e), "blend");
    br_anim_clip *c0 = ramp("c0", ":x", 0.0f, 0.0f, 1.0f, BR_ANIM_LOOP);
    br_anim_clip *c1 = ramp("c1", ":x", 10.0f, 10.0f, 2.0f, BR_ANIM_LOOP);
    br_anim_clip *c2 = ramp("c2", ":x", 30.0f, 30.0f, 3.0f, BR_ANIM_LOOP);
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    br_animator_add_clip(e, a, c0);
    br_animator_add_clip(e, a, c1);
    br_animator_add_clip(e, a, c2);
    const char *clips[3] = { "c2", "c0", "c1" }; /* unsorted on purpose */
    float at[3] = { 2.0f, 0.0f, 1.0f };
    BR_CHECK(br_animator_add_blend1d(e, a, "move", "speed", clips, at, 3));
    BR_CHECK(br_animator_play(e, a, "move"));
    br_animator_set_float(e, a, "speed", 1.5f);
    br_engine_step(e, 0.1f);
    float w0 = br_animator_blend_weight(e, a, "move", 0);
    float w1 = br_animator_blend_weight(e, a, "move", 1);
    float w2 = br_animator_blend_weight(e, a, "move", 2);
    BR_CHECK(near_eq(w0, 0.0f) && near_eq(w1, 0.5f) && near_eq(w2, 0.5f) && near_eq(w0 + w1 + w2, 1.0f));
    BR_CHECK(near_eq(node_x(e, n), 20.0f));
    br_animator_set_float(e, a, "speed", -3.0f); /* clamped to the ends */
    br_engine_step(e, 0.1f);
    BR_CHECK(near_eq(node_x(e, n), 0.0f) && near_eq(br_animator_blend_weight(e, a, "move", 0), 1.0f));
    BR_CHECK(near_eq(br_animator_get_float(e, a, "speed"), -3.0f));

    br_vec2 pos[3] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
    const char *clips2[3] = { "c0", "c1", "c2" };
    BR_CHECK(br_animator_add_blend2d(e, a, "aim", "vx", "vy", clips2, pos, 3));
    br_animator_play(e, a, "aim");
    br_animator_set_float(e, a, "vx", 0.3f);
    br_animator_set_float(e, a, "vy", 0.4f);
    br_engine_step(e, 0.1f);
    float sum = 0.0f;
    for (uint32_t i = 0; i < 3; i++) {
        sum += br_animator_blend_weight(e, a, "aim", i);
    }
    BR_CHECK(near_eq(sum, 1.0f));
    br_animator_set_float(e, a, "vx", 1.0f);
    br_animator_set_float(e, a, "vy", 0.0f);
    br_engine_step(e, 0.1f); /* exactly on a sample: it takes everything */
    BR_CHECK(near_eq(br_animator_blend_weight(e, a, "aim", 1), 1.0f) && near_eq(node_x(e, n), 10.0f));
    BR_CHECK(!br_animator_add_blend1d(e, a, "bad", "speed", clips, at, 0));
    br_node_destroy(e, n);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(c0);
    br_anim_clip_destroy(c1);
    br_anim_clip_destroy(c2);
}

/* ---- events */

static int g_callback_hits = 0;

static void on_anim_event(br_engine *e, const br_anim_event *ev, void *user)
{
    (void)e;
    (void)user;
    if (SDL_strcmp(ev->name, "half") == 0) {
        g_callback_hits++;
    }
}

static void count_events(br_engine *e, int *start, int *half)
{
    uint32_t n = 0;
    const br_anim_event *ev = br_animation_events(e, &n);
    for (uint32_t i = 0; i < n; i++) {
        *start += SDL_strcmp(ev[i].name, "start") == 0 ? 1 : 0;
        *half += SDL_strcmp(ev[i].name, "half") == 0 ? 1 : 0;
    }
}

static void test_events(br_engine *e)
{
    br_node n = make_node(e, br_scene_root(e), "events");
    br_anim_clip *c = ramp("ev", ":x", 0.0f, 1.0f, 1.0f, BR_ANIM_LOOP);
    br_anim_clip_add_marker(c, 0.0f, "start");
    br_anim_clip_add_marker(c, 0.5f, "half");
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    br_animator_add_clip(e, a, c);
    br_animator_set_event_callback(e, a, on_anim_event, NULL);
    br_animator_play(e, a, "ev");
    int start = 0;
    int half = 0;
    for (int i = 0; i < 8; i++) { /* two loops in quarter steps, landing exactly on the markers */
        br_engine_step(e, 0.25f);
        count_events(e, &start, &half);
    }
    BR_CHECK(start == 3 && half == 2 && g_callback_hits == 2); /* "start" at 0, 1 and 2 s */
    /* one big step over 2.5 loops: each marker once per pass, none twice */
    start = half = 0;
    br_animator_play(e, a, "ev");
    br_engine_step(e, 2.5f);
    count_events(e, &start, &half);
    BR_CHECK(start == 3 && half == 3);
    /* ping-pong: the marker is passed once each way */
    start = half = 0;
    br_animator_play(e, a, "ev");
    br_animator_set_loop(e, a, 0, BR_ANIM_PING_PONG);
    for (int i = 0; i < 20; i++) { /* 0 -> 1 -> 0 */
        br_engine_step(e, 0.1f);
        count_events(e, &start, &half);
    }
    BR_CHECK(half == 2 && start == 2);
    /* events of the last update only */
    br_animator_stop(e, a);
    br_engine_step(e, 0.1f);
    uint32_t count = 99;
    br_animation_events(e, &count);
    BR_CHECK(count == 0);
    br_node_destroy(e, n);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(c);
}

/* ---- state machine */

static const char *g_graph =
    "{ \"parameters\": { \"speed\": 0, \"jump\": \"trigger\" },"
    "  \"entry\": \"idle\","
    "  \"states\": ["
    "    { \"name\": \"idle\", \"clip\": \"c0\" },"
    "    { \"name\": \"walk\", \"clip\": \"c1\", \"speed\": 2 },"
    "    { \"name\": \"run\", \"blend\": { \"parameter\": \"speed\", \"motions\": [ { \"clip\": \"c1\", \"at\": 1 }, { \"clip\": \"c2\", \"at\": 3 } ] } },"
    "    { \"name\": \"jump\", \"clip\": \"c2\", \"loop\": \"once\" } ],"
    "  \"transitions\": ["
    "    { \"from\": \"idle\", \"to\": \"walk\", \"when\": [[\"speed\", \">\", 0.1]], \"duration\": 0.2 },"
    "    { \"from\": \"walk\", \"to\": \"run\", \"when\": [[\"speed\", \">=\", 2]], \"duration\": 0.2 },"
    "    { \"from\": \"run\", \"to\": \"idle\", \"when\": [[\"speed\", \"<\", 0.1]], \"duration\": 0.2 },"
    "    { \"from\": \"*\", \"to\": \"jump\", \"when\": [[\"jump\"]], \"duration\": 0.1 },"
    "    { \"from\": \"jump\", \"to\": \"idle\", \"exit_time\": 1.0, \"duration\": 0.1 } ] }";

static void test_state_machine(br_engine *e)
{
    br_anim_graph *g = br_anim_graph_parse(e, "test.animgraph.json", g_graph, SDL_strlen(g_graph));
    BR_CHECK(g != NULL);
    if (g == NULL) {
        return;
    }
    br_node n = make_node(e, br_scene_root(e), "sm");
    br_anim_clip *c0 = ramp("c0", ":x", 0.0f, 0.0f, 1.0f, BR_ANIM_LOOP);
    br_anim_clip *c1 = ramp("c1", ":x", 10.0f, 10.0f, 1.0f, BR_ANIM_LOOP);
    br_anim_clip *c2 = ramp("c2", ":x", 30.0f, 30.0f, 0.5f, BR_ANIM_LOOP);
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    br_animator_add_clip(e, a, c0);
    br_animator_add_clip(e, a, c1);
    br_animator_add_clip(e, a, c2);
    BR_CHECK(br_animator_set_graph(e, a, g));
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "idle") == 0);
    br_engine_step(e, 0.1f);
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "idle") == 0 && near_eq(node_x(e, n), 0.0f));
    br_animator_set_float(e, a, "speed", 1.0f);
    br_engine_step(e, 0.1f);
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "walk") == 0);
    br_animator_set_float(e, a, "speed", 2.0f);
    br_engine_step(e, 0.1f); /* still blending into walk: no interruption */
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "walk") == 0);
    br_engine_step(e, 0.2f);
    br_engine_step(e, 0.1f);
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "run") == 0);
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(node_x(e, n), 20.0f)); /* run tree at speed 2: c1 and c2 half each */
    br_animator_set_trigger(e, a, "jump");
    br_engine_step(e, 0.05f);
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "jump") == 0);
    BR_CHECK(near_eq(br_animator_get_float(e, a, "jump"), 0.0f)); /* consumed */
    br_animator_set_float(e, a, "speed", 0.0f);
    for (int i = 0; i < 10; i++) {
        br_engine_step(e, 0.1f); /* the once clip ends, exit time 1 leads back to idle */
    }
    BR_CHECK(SDL_strcmp(br_animator_state(e, a), "idle") == 0);
    BR_CHECK(br_anim_graph_parse(e, "bad.animgraph.json", "{ \"states\": [] }", 16) == NULL);
    br_node_destroy(e, n);
    br_engine_step(e, 0.0f);
    br_anim_graph_free(g);
    br_anim_clip_destroy(c0);
    br_anim_clip_destroy(c1);
    br_anim_clip_destroy(c2);
}

/* ---- Review Focus 5: destroyed nodes and missing properties */

static void test_dead_bindings(br_engine *e)
{
    br_node root = make_node(e, br_scene_root(e), "dead");
    br_node a_node = make_node(e, root, "a");
    br_node b_node = make_node(e, root, "b");
    br_anim_clip *c = br_anim_clip_create("multi", 0.0f, BR_ANIM_LOOP);
    float t[2] = { 0.0f, 1.0f };
    float v[2] = { 0.0f, 10.0f };
    br_anim_clip_add_track(c, "a:x", BR_ANIM_LINEAR, 1, 2, t, v);
    br_anim_clip_add_track(c, "b:x", BR_ANIM_LINEAR, 1, 2, t, v);
    br_anim_clip_add_track(c, "missing:x", BR_ANIM_LINEAR, 1, 2, t, v);
    br_anim_clip_add_track(c, "a:no_such_property", BR_ANIM_LINEAR, 1, 2, t, v);
    br_anim_clip_add_track(c, "a:position", BR_ANIM_LINEAR, 1, 2, t, v); /* 1 value for a vec2 */
    br_animator an = br_animator_add(e, br_anim_target_2d(root));
    br_animator_add_clip(e, an, c);
    g_warnings = 0;
    br_animator_play(e, an, "multi");
    BR_CHECK(g_warnings == 3); /* one per bad track, at bind time */
    br_engine_step(e, 0.25f);
    BR_CHECK(near_eq(node_x(e, a_node), 2.5f) && near_eq(node_x(e, b_node), 2.5f) && g_warnings == 3);
    br_node_destroy(e, b_node);
    for (int i = 0; i < 4; i++) {
        br_engine_step(e, 0.1f);
    }
    BR_CHECK(g_warnings == 4);                      /* the destroyed node: one warning, once */
    BR_CHECK(near_eq(node_x(e, a_node), 6.5f));     /* the rest keeps playing */
    BR_CHECK(br_animator_valid(e, an));
    br_node_destroy(e, root);
    br_engine_step(e, 0.0f);
    BR_CHECK(!br_animator_valid(e, an) && g_warnings == 4);
    br_anim_clip_destroy(c);
}

/* ---- property registry round trip */

typedef struct hp { float value; int32_t level; bool alive; br_vec2 offset; br_color glow; } hp;

static float g_custom = 0.0f;
static void custom_get(br_engine *e, br_anim_target t, br_anim_value *out)
{
    (void)e;
    (void)t;
    out->f[0] = g_custom;
}
static void custom_set(br_engine *e, br_anim_target t, const br_anim_value *v)
{
    (void)e;
    (void)t;
    g_custom = v->f[0];
}

static bool round_trip(br_engine *e, br_anim_target root, const char *path, const float *value, uint32_t count)
{
    br_anim_value in;
    SDL_zero(in);
    SDL_memcpy(in.f, value, sizeof(float) * count);
    in.count = count;
    br_anim_value out;
    if (!br_anim_set(e, root, path, &in) || !br_anim_get(e, root, path, &out)) {
        printf("  %s does not resolve\n", path);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (SDL_fabsf(out.f[i] - value[i]) > 1e-3f) {
            printf("  %s[%u]: wrote %f, read %f\n", path, i, (double)value[i], (double)out.f[i]);
            return false;
        }
    }
    return true;
}

static void test_properties(br_engine *e)
{
    br_texture white = br_texture_white(e);
    br_texture other = br_texture_create_rgba8(e, 4, 4, NULL, NULL);
    br_node holder = make_node(e, br_scene_root(e), "holder");
    br_node s = br_sprite_create(e, white);
    br_node_set_label(e, s, "spr");
    br_node_add_child(e, holder, s);
    br_anim_target t = br_anim_target_2d(holder);
    float v2[2] = { 3.0f, -4.0f };
    float v1[1] = { 0.75f };
    float c4[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    float on[1] = { 0.0f };
    float z[1] = { 7.0f };
    BR_CHECK(round_trip(e, t, "spr:position", v2, 2));
    BR_CHECK(round_trip(e, t, "spr:x", v1, 1));
    BR_CHECK(round_trip(e, t, "spr:y", v1, 1));
    BR_CHECK(round_trip(e, t, "spr:scale", v2, 2));
    BR_CHECK(round_trip(e, t, "spr:rotation", v1, 1));
    BR_CHECK(round_trip(e, t, "spr:skew", v2, 2));
    BR_CHECK(round_trip(e, t, "spr:pivot", v2, 2));
    BR_CHECK(round_trip(e, t, "spr:alpha", v1, 1));
    float tint[4] = { 0.2f, 0.4f, 0.6f, 1.0f }; /* tint has no alpha of its own (that is "alpha") */
    BR_CHECK(round_trip(e, t, "spr:tint", tint, 4));
    BR_CHECK(round_trip(e, t, "spr:visible", on, 1));
    BR_CHECK(round_trip(e, t, "spr:z_index", z, 1));
    BR_CHECK(round_trip(e, t, ":alpha", v1, 1)); /* the root itself */
    br_anim_value tex;
    SDL_zero(tex);
    tex.texture = other;
    br_anim_value back;
    BR_CHECK(br_anim_set(e, t, "spr:sprite.texture", &tex) && br_anim_get(e, t, "spr:sprite.texture", &back));
    BR_CHECK(back.texture.index == other.index && back.texture.generation == other.generation);
    /* animated sprite frame */
    br_texture frames[3] = { white, other, white };
    br_node anim = br_animated_sprite_create_uniform(e, frames, 3, 10.0f);
    br_node_set_label(e, anim, "anim");
    br_node_add_child(e, holder, anim);
    float f2[1] = { 2.0f };
    BR_CHECK(round_trip(e, t, "anim:sprite.frame", f2, 1));
    /* component fields */
    static const br_field_desc fields[] = {
        { "value", BR_FIELD_FLOAT, offsetof(hp, value) },   { "level", BR_FIELD_INT, offsetof(hp, level) },
        { "alive", BR_FIELD_BOOL, offsetof(hp, alive) },    { "offset", BR_FIELD_VEC2, offsetof(hp, offset) },
        { "glow", BR_FIELD_COLOR, offsetof(hp, glow) },
    };
    br_component_desc desc = { "hp", sizeof(hp), fields, BR_ARRAY_COUNT(fields) };
    uint32_t hp_id = br_component_register(e, &desc);
    br_component_add(e, s, hp_id);
    float lvl[1] = { 3.0f };
    float yes[1] = { 1.0f };
    BR_CHECK(round_trip(e, t, "spr:hp.value", v1, 1));
    BR_CHECK(round_trip(e, t, "spr:hp.level", lvl, 1));
    BR_CHECK(round_trip(e, t, "spr:hp.alive", yes, 1));
    BR_CHECK(round_trip(e, t, "spr:hp.offset", v2, 2));
    BR_CHECK(round_trip(e, t, "spr:hp.glow", c4, 4));
    BR_CHECK(((hp *)br_component_get(e, s, hp_id))->level == 3);
    br_anim_value dummy;
    BR_CHECK(!br_anim_get(e, t, "spr:hp.nope", &dummy) && !br_anim_get(e, t, "spr:nope.value", &dummy));
    BR_CHECK(!br_anim_get(e, t, "ghost:x", &dummy));
    /* a property row added by "another module" */
    br_anim_property_desc custom = { "demo.glow", BR_ANIM_TARGET_2D, BR_ANIM_FLOAT, custom_get, custom_set };
    uint32_t id = br_anim_property_register(e, &custom);
    BR_CHECK(id != UINT32_MAX && br_anim_property_find(e, BR_ANIM_TARGET_2D, "demo.glow") == id);
    BR_CHECK(br_anim_property_register(e, &custom) == id); /* same name: replaced, not added */
    BR_CHECK(round_trip(e, t, "spr:demo.glow", v1, 1) && near_eq(g_custom, 0.75f));
    BR_CHECK(br_anim_property_get_desc(e, id) != NULL && br_anim_property_count(e) > id);

    /* 3D: node, material, morph weights */
    br_node3d g = br_node3d_create_group(e);
    br_material3d_desc md = br_material3d_default(BR_MATERIAL3D_PBR);
    br_node3d mesh = br_mesh3d_create(e, br_geometry3d_cube(e, br_vec3_make(1.0f, 1.0f, 1.0f)), br_material3d_create(e, &md));
    br_node3d_set_label(e, mesh, "box");
    br_node3d_add_child(e, g, mesh);
    br_anim_target t3 = br_anim_target_3d(g);
    float p3[3] = { 1.0f, 2.0f, 3.0f };
    br_quat rq = br_quat_from_axis_angle(br_vec3_make(1.0f, 0.0f, 0.0f), 0.5f);
    float q4[4] = { rq.x, rq.y, rq.z, rq.w };
    float w8[3] = { 0.1f, 0.2f, 0.3f };
    BR_CHECK(round_trip(e, t3, "box:position", p3, 3));
    BR_CHECK(round_trip(e, t3, "box:rotation", q4, 4));
    BR_CHECK(round_trip(e, t3, "box:scale", p3, 3));
    BR_CHECK(round_trip(e, t3, "box:visible", on, 1));
    BR_CHECK(round_trip(e, t3, "box:material.base_color", c4, 4));
    BR_CHECK(round_trip(e, t3, "box:material.emissive", c4, 4));
    BR_CHECK(round_trip(e, t3, "box:material.roughness", v1, 1));
    BR_CHECK(round_trip(e, t3, "box:material.metallic", v1, 1));
    BR_CHECK(round_trip(e, t3, "box:material.alpha", v1, 1));
    BR_CHECK(round_trip(e, t3, "box:morph_weights", w8, 3));
    BR_CHECK(br_mesh3d_get(e, mesh)->morph_weight_count == 3);
    BR_CHECK(round_trip(e, t3, ":material.roughness", v1, 1)); /* a group: its first mesh child */
    BR_CHECK(!br_anim_get(e, t3, "box:alpha", &dummy));      /* 2D-only properties do not resolve on 3D nodes */
    br_node3d_destroy(e, g);
    br_node_destroy(e, holder);
    br_engine_step(e, 0.0f);
}

/* ---- *.anim.json */

static const char *g_anim_json =
    "{ \"clips\": ["
    "  { \"name\": \"wave\", \"loop\": \"ping_pong\", \"tracks\": ["
    "      { \"target\": \"arm:rotation\", \"ease\": \"in_out_sine\", \"keys\": [[0, 0], [1, 0.8]] },"
    "      { \"target\": \"arm:position\", \"keys\": [[0, [0, 0]], [0.5, [0, -10]]] },"
    "      { \"target\": \"arm:tint\", \"interpolation\": \"step\", \"keys\": [[0, \"#ffffff\"], [0.5, \"#ff0000\"]] },"
    "      { \"target\": \"arm:x\", \"interpolation\": \"bezier\", \"keys\": [ { \"time\": 0, \"value\": 0, \"out\": 1 }, { \"time\": 2, \"value\": 3, \"in\": 2 } ] },"
    "      { \"target\": \"bad\", \"keys\": [[0, 1], [1, [1, 2]]] },"
    "      { \"target\": \"bad2\", \"keys\": [[1, 1], [0, 1]] },"
    "      { \"keys\": [] } ],"
    "    \"events\": [ { \"time\": 0.5, \"name\": \"peak\" }, { \"name\": \"no time\" } ] },"
    "  { \"name\": \"idle\", \"duration\": 3, \"tracks\": [] } ] }";

static void test_anim_json(br_engine *e)
{
    br_anim_library *lib = br_anim_library_parse(e, "test.anim.json", g_anim_json, SDL_strlen(g_anim_json));
    BR_CHECK(lib != NULL && lib->count == 2);
    if (lib == NULL) {
        return;
    }
    const br_anim_clip *wave = br_anim_library_find(lib, "wave");
    BR_CHECK(wave != NULL && wave->loop == BR_ANIM_PING_PONG && wave->track_count == 4);
    BR_CHECK(wave != NULL && near_eq(wave->duration, 2.0f) && wave->marker_count == 1);
    if (wave != NULL && wave->track_count == 4) {
        BR_CHECK(wave->tracks[0].ease == BR_EASE_IN_OUT_SINE && wave->tracks[1].components == 2);
        BR_CHECK(wave->tracks[2].components == 4 && wave->tracks[2].interpolation == BR_ANIM_STEP);
        BR_CHECK(wave->tracks[3].interpolation == BR_ANIM_BEZIER && wave->tracks[3].key_count == 2);
        float out[1];
        br_anim_track_sample(&wave->tracks[3], 0.0f, false, NULL, out);
        BR_CHECK(near_eq(out[0], 0.0f));
        br_anim_track_sample(&wave->tracks[3], 2.0f, false, NULL, out);
        BR_CHECK(near_eq(out[0], 3.0f));
    }
    const br_anim_clip *idle = br_anim_library_find(lib, "idle");
    BR_CHECK(idle != NULL && near_eq(idle->duration, 3.0f) && br_anim_library_find(lib, "x") == NULL);
    /* plays on a node tree */
    br_node root = make_node(e, br_scene_root(e), "json_rig");
    br_node arm = make_node(e, root, "arm");
    br_animator a = br_animator_add(e, br_anim_target_2d(root));
    BR_CHECK(br_animator_add_library(e, a, lib) == 2);
    br_animator_play(e, a, "wave");
    br_engine_step(e, 0.5f);
    BR_CHECK(near_eq(br_node_position(e, arm).y, -10.0f));
    BR_CHECK(near_eq(br_node_tint(e, arm).g, 0.0f));
    br_node_destroy(e, root);
    br_engine_step(e, 0.0f);
    br_anim_library_free(lib);
    /* a single clip at the top level, and garbage */
    const char *single = "{ \"name\": \"one\", \"tracks\": [ { \"target\": \":alpha\", \"keys\": [[0, 1]] } ] }";
    lib = br_anim_library_parse(e, "one.anim.json", single, SDL_strlen(single));
    BR_CHECK(lib != NULL && lib->count == 1);
    br_anim_library_free(lib);
    BR_CHECK(br_anim_library_parse(e, "junk.anim.json", "{ nope", 6) == NULL);
    BR_CHECK(br_anim_library_parse(e, "empty.anim.json", "{}", 2) == NULL);
}

/* ---- clip sources: flipbooks and glTF */

static void test_sources(br_engine *e)
{
    br_texture t0 = br_texture_create_rgba8(e, 2, 2, NULL, NULL);
    br_texture t1 = br_texture_create_rgba8(e, 2, 2, NULL, NULL);
    br_anim_frame frames[3] = { { t0, 0.2f }, { t1, 0.0f }, { t1, 0.3f } }; /* the 0 s frame is skipped */
    br_anim_clip *flip = br_anim_clip_from_frames("flip", frames, 3);
    BR_CHECK(flip != NULL && near_eq(flip->duration, 0.5f) && flip->texture_count == 2);
    br_node s = br_sprite_create(e, br_texture_white(e));
    br_node_add_child(e, br_scene_root(e), s);
    br_animator a = br_animator_add(e, br_anim_target_2d(s));
    br_animator_add_clip(e, a, flip);
    br_animator_play(e, a, "flip");
    br_engine_step(e, 0.1f);
    BR_CHECK(br_sprite_texture(e, s).index == t0.index);
    br_engine_step(e, 0.2f);
    BR_CHECK(br_sprite_texture(e, s).index == t1.index);
    br_engine_step(e, 0.3f); /* loops back to frame 0 */
    BR_CHECK(br_sprite_texture(e, s).index == t0.index);
    BR_CHECK(br_anim_clip_from_frames("none", frames + 1, 1) == NULL);
    br_node_destroy(e, s);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(flip);

    /* glTF: tests/data/tiny.gltf has a clip "Move" (node 1 "Tri": x 0 -> 2 over 1 s) */
    br_model *m = br_model_load(e, "tiny.gltf");
    BR_CHECK(m != NULL);
    if (m == NULL) {
        return;
    }
    br_anim_clip *gc = br_anim_clip_from_gltf(m, 0);
    BR_CHECK(gc != NULL && gc->track_count == 1 && SDL_strcmp(gc->tracks[0].path, "#1:position") == 0);
    BR_CHECK(gc != NULL && !gc->tracks[0].owns_keys && gc->tracks[0].times == m->clips[0].channels[0].times);
    br_anim_clip_destroy(gc);
    br_node3d inst = br_model_instantiate(e, m);
    br_animator ga = br_animator_add(e, br_anim_target_3d(inst));
    BR_CHECK(br_animator_add_model_clips(e, ga, m) == 1);
    BR_CHECK(br_animator_play(e, ga, "Move"));
    br_engine_step(e, 0.25f);
    br_vec3 p = br_node3d_position(e, br_model3d_node(e, inst, 1));
    BR_CHECK(near_eq(p.x, 0.5f) && near_eq(p.y, 0.0f));
    /* a second instance shares the converted clips */
    br_node3d inst2 = br_model_instantiate(e, m);
    br_animator ga2 = br_animator_add(e, br_anim_target_3d(inst2));
    BR_CHECK(br_animator_add_model_clips(e, ga2, m) == 1);
    br_node3d_destroy(e, inst);
    br_node3d_destroy(e, inst2);
    br_engine_step(e, 0.0f);
    BR_CHECK(!br_animator_valid(e, ga) && !br_animator_valid(e, ga2));
    br_assets_unload(e, "tiny.gltf");
}

/* ---- root motion */

static void test_root_motion(br_engine *e)
{
    br_node n = make_node(e, br_scene_root(e), "walker");
    br_node_set_position(e, n, 5.0f, 7.0f);
    br_anim_clip *c = br_anim_clip_create("walk", 0.0f, BR_ANIM_LOOP);
    float t[2] = { 0.0f, 1.0f };
    float v[4] = { 0.0f, 0.0f, 10.0f, 4.0f };
    br_anim_clip_add_track(c, ":position", BR_ANIM_LINEAR, 2, 2, t, v);
    br_animator a = br_animator_add(e, br_anim_target_2d(n));
    br_animator_add_clip(e, a, c);
    BR_CHECK(br_animator_set_root_motion(e, a, ""));
    br_animator_play(e, a, "walk");
    br_engine_step(e, 0.25f);
    float dx = 0.0f;
    for (int i = 0; i < 2; i++) {
        br_engine_step(e, 0.25f);
        dx += br_animator_root_motion(e, a).x;
    }
    BR_CHECK(near_eq(dx, 5.0f) && near_eq(br_animator_root_motion(e, a).y, 1.0f));
    BR_CHECK(near_eq(node_x(e, n), 5.0f) && near_eq(br_node_position(e, n).y, 7.0f)); /* kept in place */
    BR_CHECK(!br_animator_set_root_motion(e, a, "nobody"));
    br_node_destroy(e, n);
    br_engine_step(e, 0.0f);
    br_anim_clip_destroy(c);
}

/* ---- determinism: the same script twice gives the same bits */

static void run_script(br_engine *e, float *out)
{
    br_node root = make_node(e, br_scene_root(e), "det");
    br_node arm = make_node(e, root, "arm");
    br_anim_library *lib = br_anim_library_parse(e, "det.anim.json", g_anim_json, SDL_strlen(g_anim_json));
    br_animator a = br_animator_add(e, br_anim_target_2d(root));
    br_animator_add_library(e, a, lib);
    br_animator_play(e, a, "wave");
    for (int i = 0; i < 200; i++) {
        br_engine_step(e, 1.0f / 60.0f);
        if (i == 50) {
            br_animator_cross_fade(e, a, "idle", 0.3f);
        }
        if (i == 90) {
            br_animator_cross_fade(e, a, "wave", 0.3f);
        }
    }
    out[0] = br_node_rotation(e, arm);
    out[1] = br_node_position(e, arm).x;
    out[2] = br_node_position(e, arm).y;
    out[3] = br_animator_time(e, a, 0);
    br_node_destroy(e, root);
    br_engine_step(e, 0.0f);
    br_anim_library_free(lib);
}

static void test_determinism(void)
{
    float a[4];
    float b[4];
    br_engine *e1 = br_engine_create("det1", NULL);
    br_engine *e2 = br_engine_create("det2", NULL);
    run_script(e1, a);
    run_script(e2, b);
    BR_CHECK(SDL_memcmp(a, b, sizeof(a)) == 0);
    br_engine_destroy(e1);
    br_engine_destroy(e2);
}

int main(void)
{
    SDL_GetLogOutputFunction(&g_prev_log, &g_prev_log_user);
    SDL_SetLogOutputFunction(count_log, NULL);
    test_sampling();
    br_engine *e = br_engine_create("test_animation", "../../tests/data"); /* relative to the exe */
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_loop_modes(e);
        test_cross_fade(e);
        test_layers(e);
        test_blend_trees(e);
        test_events(e);
        test_state_machine(e);
        test_dead_bindings(e);
        test_properties(e);
        test_anim_json(e);
        test_sources(e);
        test_root_motion(e);
        BR_CHECK(br_animator_count(e) == 0);
        br_engine_destroy(e);
    }
    test_determinism();
    return br_test_finish();
}
