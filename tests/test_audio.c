/* test_audio - the mixer on known PCM (volume, pan, loop, speed, sprites, buses, completion) and
 * the wav/ogg loaders, all headless: br_audio_mix is called directly, no device is opened. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <math.h>

#define RAMP_FRAMES 100

static bool near(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

static int g_completed;
static br_sound_instance g_completed_handle;

static void on_complete(br_engine *e, br_sound_instance i, void *user)
{
    (void)e;
    int *count = user;
    (*count)++;
    g_completed_handle = i;
}

/* Mono ramp 0, 0.01, ..., 0.99 at the mixer rate: sample k of the sound is k / 100. */
static br_sound *make_ramp(br_engine *e)
{
    float pcm[RAMP_FRAMES];
    for (int i = 0; i < RAMP_FRAMES; i++) {
        pcm[i] = (float)i / 100.0f;
    }
    return br_sound_create_pcm(e, pcm, RAMP_FRAMES, 1, BR_AUDIO_RATE);
}

static void test_volume_pan(br_engine *e, br_sound *ramp)
{
    float out[2 * 50];
    br_audio_play_desc d = br_audio_play_defaults();
    d.volume = 0.5f;
    br_sound_instance i = br_audio_play(e, ramp, &d);
    BR_CHECK(i.generation != 0 && br_audio_is_playing(e, i));
    br_audio_mix(e, out, 50);
    BR_CHECK(near(out[20], 0.05f) && near(out[21], 0.05f));   /* frame 10: 0.1 * 0.5 both sides */
    br_audio_set_pan(e, i, -1.0f);
    br_audio_mix(e, out, 10);                                  /* frames 50..59 */
    BR_CHECK(near(out[0], 0.25f) && near(out[1], 0.0f));
    br_audio_set_pan(e, i, 0.5f);
    br_audio_mix(e, out, 10);                                  /* frames 60..69 */
    BR_CHECK(near(out[0], 0.15f) && near(out[1], 0.3f));
    br_audio_set_paused(e, i, true);
    br_audio_mix(e, out, 10);
    BR_CHECK(near(out[0], 0.0f) && near(out[1], 0.0f));
    BR_CHECK(br_audio_is_playing(e, i));
    br_audio_set_paused(e, i, false);
    br_audio_mix(e, out, 1);                                   /* resumes at frame 70 */
    BR_CHECK(near(out[0], 0.35f * 0.5f));
    BR_CHECK(near(br_audio_position(e, i), 71.0f / BR_AUDIO_RATE));
    br_audio_stop(e, i);
    BR_CHECK(!br_audio_is_playing(e, i));
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f));
}

static void test_loop_and_completion(br_engine *e, br_sound *ramp)
{
    float out[2 * 250];
    br_audio_play_desc d = br_audio_play_defaults();
    d.loop = true;
    br_sound_instance looped = br_audio_play(e, ramp, &d);
    br_audio_mix(e, out, 250);
    bool ok = true;
    for (int f = 0; f < 250; f++) {
        ok = ok && near(out[f * 2], (float)(f % RAMP_FRAMES) / 100.0f);
    }
    BR_CHECK(ok);
    BR_CHECK(br_audio_is_playing(e, looped));
    br_audio_set_loop(e, looped, false);
    br_audio_mix(e, out, 100);                         /* frames 50..99, then silence */
    BR_CHECK(near(out[0], 0.5f) && near(out[49 * 2], 0.99f) && near(out[50 * 2], 0.0f));
    BR_CHECK(!br_audio_is_playing(e, looped));

    /* completion callback fires on the main thread (audio.update system) */
    g_completed = 0;
    d = br_audio_play_defaults();
    d.on_complete = on_complete;
    d.user = &g_completed;
    br_sound_instance once = br_audio_play(e, ramp, &d);
    br_audio_mix(e, out, 150);
    BR_CHECK(near(out[99 * 2], 0.99f) && near(out[100 * 2], 0.0f));
    BR_CHECK(g_completed == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_completed == 1 && g_completed_handle.index == once.index &&
             g_completed_handle.generation == once.generation);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_completed == 1);
    BR_CHECK(br_audio_instance_count(e) == 0);   /* the looped one finished too, without a callback */
}

static void test_speed_seek_sprite(br_engine *e, br_sound *ramp)
{
    float out[2 * 60];
    br_audio_play_desc d = br_audio_play_defaults();
    d.speed = 2.0f;
    br_sound_instance i = br_audio_play(e, ramp, &d);
    br_audio_mix(e, out, 10);
    BR_CHECK(near(out[3 * 2], 0.06f));          /* frame 3 at double speed = sample 6 */
    br_audio_set_speed(e, i, 0.5f);
    br_audio_seek(e, i, 40.0f / BR_AUDIO_RATE);
    br_audio_mix(e, out, 3);
    BR_CHECK(near(out[0], 0.40f) && near(out[2], 0.405f) && near(out[4], 0.41f));   /* interpolated */
    br_audio_stop_all(e);

    /* sprite: frames 48..71 of the ramp */
    BR_CHECK(br_sound_add_sprite(e, ramp, "mid", 48.0f / BR_AUDIO_RATE, 24.0f / BR_AUDIO_RATE));
    BR_CHECK(!br_sound_add_sprite(e, ramp, "outside", 1.0f, 1.0f));
    d = br_audio_play_defaults();
    d.sprite = "mid";
    br_audio_play(e, ramp, &d);
    br_audio_mix(e, out, 30);
    BR_CHECK(near(out[0], 0.48f) && near(out[23 * 2], 0.71f) && near(out[24 * 2], 0.0f));
    d.sprite = "missing";
    BR_CHECK(br_audio_play(e, ramp, &d).generation == 0);
    br_engine_step(e, 1.0f / 60.0f);
}

static void test_buses(br_engine *e, br_sound *ramp)
{
    float out[2 * 1];
    int sfx = br_audio_bus_create(e, "sfx");
    BR_CHECK(sfx > 0 && br_audio_bus_create(e, "sfx") == sfx && br_audio_bus_find(e, "sfx") == sfx);
    br_audio_play_desc d = br_audio_play_defaults();
    d.bus = sfx;
    d.start = 50.0f / BR_AUDIO_RATE;
    d.loop = true;
    br_audio_play(e, ramp, &d);
    br_audio_bus_set_volume(e, sfx, 0.5f);
    br_audio_set_master_volume(e, 0.5f);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.125f));
    br_audio_bus_set_muted(e, sfx, true);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f));
    br_audio_bus_set_muted(e, sfx, false);
    br_audio_set_muted(e, true);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f));
    br_audio_set_muted(e, false);
    br_audio_bus_set_paused(e, sfx, true);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f));
    br_audio_bus_set_paused(e, sfx, false);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.53f * 0.25f));   /* resumed where it paused (frame 53) */
    br_audio_set_master_volume(e, 1.0f);

    /* destroying a playing sound stops its instances */
    BR_CHECK(br_audio_instance_count(e) == 1);
    br_sound_destroy(e, ramp);
    BR_CHECK(br_audio_instance_count(e) == 0);
}

/* volume getters, and a global pause that composes with the app's device pause */
static void test_getters_and_global_pause(br_engine *e, br_sound *ramp)
{
    float out[2 * 1];
    int music = br_audio_bus_create(e, "music");
    BR_CHECK(near(br_audio_master_volume(e), 1.0f) && near(br_audio_bus_volume(e, music), 1.0f));
    br_audio_set_master_volume(e, 0.25f);
    br_audio_bus_set_volume(e, music, 0.75f);
    br_audio_bus_set_muted(e, music, true);
    BR_CHECK(near(br_audio_master_volume(e), 0.25f) && near(br_audio_bus_volume(e, music), 0.75f));
    BR_CHECK(br_audio_bus_muted(e, music) && !br_audio_muted(e));
    BR_CHECK(!br_audio_bus_paused(e, music) && !br_audio_bus_paused(e, 99));
    br_audio_bus_set_paused(e, music, true);
    BR_CHECK(br_audio_bus_paused(e, music));
    br_audio_bus_set_paused(e, music, false);
    BR_CHECK(near(br_audio_bus_volume(e, 99), 0.0f)); /* no such bus */
    br_audio_bus_set_muted(e, music, false);
    br_audio_set_master_volume(e, 1.0f);
    br_audio_play_desc d = br_audio_play_defaults();
    d.volume = 0.5f;
    d.loop = true;
    d.start = 10.0f / BR_AUDIO_RATE;
    br_sound_instance i = br_audio_play(e, ramp, &d);
    BR_CHECK(near(br_audio_volume(e, i), 0.5f));
    br_audio_set_volume(e, i, 2.0f);
    BR_CHECK(near(br_audio_volume(e, i), 2.0f));

    /* paused for two reasons, resumed for one: still paused, and the voice does not advance */
    br_audio_set_global_paused(e, true);
    br_audio_set_device_paused(e, true);
    BR_CHECK(br_audio_global_paused(e));
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f));
    br_audio_set_device_paused(e, false); /* the window came back; the game's pause menu is still up */
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.0f) && br_audio_global_paused(e) && near(br_audio_position(e, i), 10.0f / BR_AUDIO_RATE));
    br_audio_set_device_paused(e, true);
    br_audio_set_global_paused(e, false); /* menu closed while the window is away */
    BR_CHECK(br_audio_device_paused(e) && !br_audio_global_paused(e));
    br_audio_set_device_paused(e, false);
    br_audio_mix(e, out, 1);
    BR_CHECK(near(out[0], 0.10f * 2.0f)); /* resumes at frame 10 */
    br_audio_stop(e, i);
    BR_CHECK(near(br_audio_volume(e, i), 0.0f)); /* stale handle */
}

static void test_resample(br_engine *e)
{
    float pcm[2 * 240];
    for (int i = 0; i < 240; i++) {
        pcm[i * 2] = 0.5f;
        pcm[i * 2 + 1] = -0.5f;
    }
    br_sound *s = br_sound_create_pcm(e, pcm, 240, 2, 24000);   /* 10 ms at 24 kHz */
    BR_CHECK(s != NULL && near(br_sound_duration(s), 0.01f));
    float out[2 * 100];
    br_audio_play(e, s, NULL);
    br_audio_mix(e, out, 100);
    BR_CHECK(near(out[100], 0.5f) && near(out[101], -0.5f));
    br_sound_destroy(e, s);
    BR_CHECK(br_sound_create_pcm(e, NULL, 10, 1, 48000) == NULL);
}

static float peak(const float *out, uint32_t count)
{
    float p = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        p = SDL_fabsf(out[i]) > p ? SDL_fabsf(out[i]) : p;
    }
    return p;
}

/* The loaders, on the demo files (tests run with tests/ as the working directory). */
static void test_loaders(void)
{
    char *cwd = SDL_GetCurrentDirectory();
    char root[1024];
    SDL_snprintf(root, sizeof(root), "%s../examples/audio", cwd != NULL ? cwd : "");
    SDL_free(cwd);
    br_engine *e = br_engine_create("test_audio", root);

    br_sound *beep = br_sound_load(e, "beep.wav");
    BR_CHECK(beep != NULL && SDL_fabsf(br_sound_duration(beep) - 0.15f) < 0.001f);
    BR_CHECK(br_sound_load(e, "missing.wav") == NULL);

    br_sound *gong = br_sound_load(e, "gong.ogg");   /* 5.6 s: decoded in memory */
    BR_CHECK(gong != NULL && !br_sound_is_streaming(gong) && SDL_fabsf(br_sound_duration(gong) - 5.6f) < 0.01f);
    static float out[2 * 48000];
    br_audio_play(e, gong, NULL);
    br_audio_mix(e, out, 48000);
    float memory_peak = peak(out, 2 * 48000);
    BR_CHECK(memory_peak > 0.05f);
    br_audio_stop_all(e);

    br_assets_unload(e, "gong.ogg");
    br_audio_set_stream_threshold(e, 1.0f);
    gong = br_sound_load(e, "gong.ogg");             /* now streamed */
    BR_CHECK(gong != NULL && br_sound_is_streaming(gong));
    br_audio_play_desc d = br_audio_play_defaults();
    d.on_complete = on_complete;
    d.user = &g_completed;
    g_completed = 0;
    br_sound_instance i = br_audio_play(e, gong, &d);
    br_audio_mix(e, out, 48000);
    float stream_peak = peak(out, 2 * 48000);
    BR_CHECK(SDL_fabsf(stream_peak - memory_peak) < 0.02f);   /* same audio, decoded while playing */
    BR_CHECK(near(br_audio_position(e, i), 1.0f));
    br_audio_seek(e, i, 5.5f);
    br_audio_mix(e, out, 48000);                     /* runs past the end */
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(g_completed == 1 && br_audio_instance_count(e) == 0);
    br_engine_destroy(e);                            /* asset unload after audio shutdown is safe */
}

/* ---- streaming voices decode outside the audio lock */

typedef struct mixer_thread {
    br_engine    *e;
    SDL_AtomicInt running;  /* 1 while the thread mixes */
    SDL_AtomicInt stop;
    uint64_t      mix_ns;   /* longest br_audio_mix call */
    bool          finite;
} mixer_thread;

static bool all_finite_mix(const float *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (SDL_isnan(out[i]) || SDL_isinf(out[i])) {
            return false;
        }
    }
    return true;
}

/* Plays the audio thread: mixes 0.25 s per call (a long callback: many decodes per call). */
static int SDLCALL mixer_main(void *user)
{
    mixer_thread *m = user;
    static float buf[2 * 12000];
    SDL_SetAtomicInt(&m->running, 1);
    while (SDL_GetAtomicInt(&m->stop) == 0) {
        uint64_t t0 = SDL_GetTicksNS();
        br_audio_mix(m->e, buf, 12000);
        uint64_t t = SDL_GetTicksNS() - t0;
        m->mix_ns = t > m->mix_ns ? t : m->mix_ns;
        m->finite = m->finite && all_finite_mix(buf, 2 * 12000);
    }
    SDL_SetAtomicInt(&m->running, 0);
    return 0;
}

/* Many plays / stops / seeks / speed changes on streaming voices while another thread mixes.
 * The main thread's calls must never wait for a whole decode: the longest seek / speed / volume /
 * pause call stays far below the longest mix (which is mostly Vorbis decoding). Before the fix
 * the lock was held for the whole mix, so a call could wait as long as one (measured: 10.6 ms
 * call, 11.2 ms mix). */
static void test_stream_stress(void)
{
    char *cwd = SDL_GetCurrentDirectory();
    char root[1024];
    SDL_snprintf(root, sizeof(root), "%s../examples/audio", cwd != NULL ? cwd : "");
    SDL_free(cwd);
    br_engine *e = br_engine_create("test_audio_stream", root);
    br_audio_set_stream_threshold(e, 1.0f);
    br_sound *gong = br_sound_load(e, "gong.ogg");
    BR_CHECK(gong != NULL && br_sound_is_streaming(gong));
    if (gong == NULL) {
        br_engine_destroy(e);
        return;
    }
    br_audio_play_desc d = br_audio_play_defaults();
    d.loop = true;
    br_sound_instance voices[24];
    for (int i = 0; i < 24; i++) {
        d.start = (float)i * 0.2f;
        voices[i] = br_audio_play(e, gong, &d);
    }
    mixer_thread m;
    SDL_zero(m);
    m.e = e;
    m.finite = true;
    SDL_Thread *thread = SDL_CreateThread(mixer_main, "test_mixer", &m);
    BR_CHECK(thread != NULL);
    while (thread != NULL && SDL_GetAtomicInt(&m.running) == 0) {
        SDL_Delay(0);
    }
    uint64_t worst = 0;
    uint32_t rng = 12345u;
    uint64_t until = SDL_GetTicksNS() + 1500000000ull; /* 1.5 s of churn */
    while (SDL_GetTicksNS() < until) {
        rng = rng * 1664525u + 1013904223u;
        int k = (int)((rng >> 8) % 24u);
        uint64_t t0 = SDL_GetTicksNS();
        switch ((rng >> 4) % 5u) {
        case 0:
            br_audio_stop(e, voices[k]);
            voices[k] = br_audio_play(e, gong, &d);
            break;
        case 1:
            br_audio_seek(e, voices[k], (float)((rng >> 12) % 50u) * 0.1f);
            break;
        case 2:
            br_audio_set_speed(e, voices[k], 0.5f + (float)((rng >> 12) % 4u) * 0.5f);
            break;
        case 3:
            br_audio_set_volume(e, voices[k], 0.5f);
            break;
        default:
            br_audio_set_paused(e, voices[k], ((rng >> 12) & 1u) != 0);
            break;
        }
        uint64_t t = SDL_GetTicksNS() - t0;
        if ((rng >> 4) % 5u != 0) { /* play opens and seeks a decoder itself (no lock): not a wait */
            worst = t > worst ? t : worst;
        }
    }
    SDL_SetAtomicInt(&m.stop, 1);
    SDL_WaitThread(thread, NULL);
    SDL_Log("stream stress: longest main-thread call %.3f ms, longest mix %.3f ms", (double)worst / 1e6,
            (double)m.mix_ns / 1e6);
    BR_CHECK(m.finite && br_audio_instance_count(e) <= 24);
    BR_CHECK(worst * 4 < m.mix_ns);
    /* destroying the sound with voices on it while nothing mixes: all gone */
    br_assets_unload(e, "gong.ogg");
    BR_CHECK(br_audio_instance_count(e) == 0);
    br_engine_destroy(e);
}

/* Headless + deterministic: no device mixes, so instances advance by game time and finish
 * (a headless soak used to keep every sound "playing" until the 64 slots ran out). */
static void test_deterministic_game_clock(void)
{
    br_engine *e = br_engine_create("test_audio_clock", NULL);
    br_app_set_deterministic(e, true, 1);
    br_sound *ramp = make_ramp(e);
    g_completed = 0;
    br_audio_play_desc play = br_audio_play_defaults();
    play.on_complete = on_complete;
    play.user = &g_completed;
    for (int i = 0; i < 100; i++) { /* more than BR_AUDIO_MAX_INSTANCES over the run */
        BR_CHECK(br_audio_play(e, ramp, &play).generation != 0);
        br_engine_step(e, 1.0f / 60.0f);
    }
    float seconds = br_sound_duration(ramp);
    int frames = (int)(seconds * 60.0f) + 2;
    for (int i = 0; i < frames; i++) {
        br_engine_step(e, 1.0f / 60.0f);
    }
    BR_CHECK(br_audio_instance_count(e) == 0);
    BR_CHECK(g_completed == 100);
    br_engine_destroy(e);
}

static bool all_finite(const float *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (SDL_isnan(out[i]) || SDL_isinf(out[i])) {
            return false;
        }
    }
    return true;
}

/* Non-finite / extreme floats from game code: the mixer must neither hang (an INF speed on a
 * looping voice spun forever with the lock held) nor turn the whole mix into NaN. */
static void test_bad_floats(void)
{
    br_engine *e = br_engine_create("test_audio_floats", NULL);
    br_sound *ramp = make_ramp(e);
    float out[2 * 64];

    br_audio_play_desc d = br_audio_play_defaults();
    d.loop = true;
    d.speed = INFINITY;
    br_sound_instance fast = br_audio_play(e, ramp, &d);
    br_audio_mix(e, out, 64);                       /* returns */
    BR_CHECK(br_audio_is_playing(e, fast) && all_finite(out, 128));
    br_audio_set_speed(e, fast, 1e38f);
    br_audio_mix(e, out, 64);
    br_audio_set_speed(e, fast, NAN);
    br_audio_mix(e, out, 64);
    BR_CHECK(br_audio_is_playing(e, fast) && all_finite(out, 128));
    br_audio_stop_all(e);

    /* NaN start: the voice still ends */
    d = br_audio_play_defaults();
    d.start = NAN;
    br_sound_instance nan_start = br_audio_play(e, ramp, &d);
    static float long_out[2 * 6400];
    br_audio_mix(e, long_out, 6400);
    BR_CHECK(!br_audio_is_playing(e, nan_start));

    /* NaN volume / pan on one voice leaves the other voice audible and the mix finite */
    d = br_audio_play_defaults();
    d.start = 10.0f / BR_AUDIO_RATE;
    d.loop = true;
    br_audio_play(e, ramp, &d);                     /* sample 0.1 */
    d.volume = NAN;
    d.pan = NAN;
    br_sound_instance bad = br_audio_play(e, ramp, &d);
    br_audio_set_volume(e, bad, INFINITY);
    br_audio_set_pan(e, bad, NAN);
    br_audio_set_master_volume(e, NAN);
    br_audio_mix(e, out, 1);
    BR_CHECK(all_finite(out, 2) && out[0] > 0.09f);
    br_audio_seek(e, bad, NAN);
    br_audio_mix(e, out, 64);
    BR_CHECK(all_finite(out, 128));
    br_engine_destroy(e);
}

/* No device open and nobody calling br_audio_mix (a window whose device failed, or a headless
 * game run): voices advance by game time, finish and call back. */
static void test_no_device_game_clock(void)
{
    br_engine *e = br_engine_create("test_audio_nodev", NULL);
    br_sound *ramp = make_ramp(e);
    g_completed = 0;
    br_audio_play_desc play = br_audio_play_defaults();
    play.on_complete = on_complete;
    play.user = &g_completed;
    for (int i = 0; i < 100; i++) {
        BR_CHECK(br_audio_play(e, ramp, &play).generation != 0);
        br_engine_step(e, 1.0f / 60.0f);
    }
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(br_audio_instance_count(e) == 0 && g_completed == 100);
    br_engine_destroy(e);
}

int main(void)
{
    br_engine *e = br_engine_create("test_audio", NULL);
    br_sound *ramp = make_ramp(e);
    BR_CHECK(ramp != NULL && br_sound_duration(ramp) > 0.0f);
    BR_CHECK(!br_audio_device_open(e));
    test_volume_pan(e, ramp);
    test_loop_and_completion(e, ramp);
    test_speed_seek_sprite(e, ramp);
    test_getters_and_global_pause(e, ramp);
    test_buses(e, ramp);
    test_resample(e);
    BR_CHECK(!br_audio_device_open(e));   /* headless: never opened */
    br_engine_destroy(e);
    test_loaders();
    test_deterministic_game_clock();
    test_no_device_game_clock();
    test_bad_floats();
    test_stream_stress();
    return br_test_finish();
}
