/* br_audio.h - sounds, playing instances, buses, mixer (@pixi/sound).
 *
 *   br_sound *beep = br_sound_load(e, "sounds/beep.wav");
 *   br_audio_play_desc play = br_audio_play_defaults();
 *   play.volume = 0.5f;
 *   br_sound_instance i = br_audio_play(e, beep, &play);
 *
 * Sounds are asset-loaded (wav via SDL, ogg via stb_vorbis) and decoded to the mixer format
 * (float32 stereo, 48 kHz). Ogg files longer than the stream threshold (BR_AUDIO_STREAM_SECONDS by
 * default) stay compressed and are decoded while playing (streaming). The mixer runs on SDL's audio thread; all shared state sits
 * behind one mutex, so every function here is safe to call from the main thread at any time.
 *
 * The playback device opens on the first br_audio_play when the engine has a window; headless
 * engines never open one. Without an open device (headless, or opening it failed) instances
 * advance by game time in "audio.update", so they still finish and call back - unless the code
 * calls br_audio_mix itself (tests), which then drives the mixer alone.
 *
 * Every float argument is sanitized here: NaN / INF become the default, then values are clamped
 * (volume 0..16, pan -1..1, speed 0.01..16, start / seek within the sound).
 *
 * Volume: sample * instance volume * bus volume * master volume (muted = 0). Pan is a balance:
 * -1 = left only, 0 = unchanged, +1 = right only. Completion callbacks run on the main thread
 * (system "audio.update", UPDATE phase).
 */
#ifndef BR_AUDIO_H
#define BR_AUDIO_H

#include "braty/br_render.h"

#define BR_AUDIO_RATE           48000
#define BR_AUDIO_CHANNELS       2
#define BR_AUDIO_MAX_INSTANCES  64
#define BR_AUDIO_MAX_BUSES      16
#define BR_AUDIO_STREAM_SECONDS 10.0f

/* A decoded (or streamed) sound. Owned by the asset cache when loaded through it. */
typedef struct br_sound br_sound;

/* A playing instance. Stale handles (finished, stopped) are ignored by every function. */
typedef struct br_sound_instance { uint32_t index; uint32_t generation; } br_sound_instance;

/* Called on the main thread when an instance finishes (not when stopped). */
typedef void (*br_sound_complete_fn)(br_engine *e, br_sound_instance instance, void *user);

/* Options for br_audio_play. Start from br_audio_play_defaults(). */
typedef struct br_audio_play_desc {
    float       volume;        /* default 1 */
    float       pan;           /* -1..1, default 0 */
    float       speed;         /* playback rate (pitch changes too), default 1 */
    bool        loop;
    bool        paused;        /* start paused */
    float       start;         /* seconds into the sound (or sprite) */
    const char *sprite;        /* play only this named range (br_sound_add_sprite); NULL = whole sound */
    int         bus;           /* br_audio_bus_create id, 0 = master only */
    br_sound_complete_fn on_complete;
    void       *user;
} br_audio_play_desc;

/* Registers the module (wav/ogg loaders, mixer state, update system). Called once by br_builtin.c. */
void br_audio_register(br_engine *e);

/* ---- sounds */

/* Loads a sound through the asset cache (wav, ogg). NULL on failure (logged). */
br_sound *br_sound_load(br_engine *e, const char *path);
/* Creates a sound from interleaved float PCM (copied, converted to the mixer format). Free with br_sound_destroy. */
br_sound *br_sound_create_pcm(br_engine *e, const float *samples, uint32_t frames, int channels, int rate);
/* Stops its instances and frees a sound made with br_sound_create_pcm (not asset-loaded ones). */
void br_sound_destroy(br_engine *e, br_sound *sound);
/* Length in seconds. */
float br_sound_duration(const br_sound *sound);
/* True if the sound is decoded while playing. */
bool br_sound_is_streaming(const br_sound *sound);
/* Names a range [start, start + duration) in seconds (sound sprite). False if full or bad range. */
bool br_sound_add_sprite(br_engine *e, br_sound *sound, const char *name, float start, float duration);

/* ---- playing */

/* Options with the defaults filled in. */
br_audio_play_desc br_audio_play_defaults(void);
/* Starts an instance (desc NULL = defaults). Zero handle if all BR_AUDIO_MAX_INSTANCES are busy. */
br_sound_instance br_audio_play(br_engine *e, br_sound *sound, const br_audio_play_desc *desc);
/* Stops an instance (no completion callback). */
void br_audio_stop(br_engine *e, br_sound_instance i);
/* Stops every instance. */
void br_audio_stop_all(br_engine *e);
/* True while the instance exists (playing or paused). */
bool br_audio_is_playing(br_engine *e, br_sound_instance i);
/* Pauses or resumes an instance. */
void br_audio_set_paused(br_engine *e, br_sound_instance i, bool paused);
/* Sets an instance's volume (0..16; non-finite = 1). */
void br_audio_set_volume(br_engine *e, br_sound_instance i, float volume);
/* Sets an instance's pan (-1..1; non-finite = 0). */
void br_audio_set_pan(br_engine *e, br_sound_instance i, float pan);
/* Sets an instance's speed (0.01..16; non-finite = 1). */
void br_audio_set_speed(br_engine *e, br_sound_instance i, float speed);
/* Sets whether an instance loops. */
void br_audio_set_loop(br_engine *e, br_sound_instance i, bool loop);
/* Jumps to a position in seconds (relative to the sprite start). */
void br_audio_seek(br_engine *e, br_sound_instance i, float seconds);
/* Current position in seconds (relative to the sprite start), 0 if stale. */
float br_audio_position(br_engine *e, br_sound_instance i);
/* Number of live instances. */
uint32_t br_audio_instance_count(br_engine *e);

/* ---- buses (groups) and global controls */

/* Creates a named bus (e.g. "music", "sfx"); returns its id (> 0), 0 if full. Same name = same id. */
int br_audio_bus_create(br_engine *e, const char *name);
/* Finds a bus by name, 0 if none. */
int br_audio_bus_find(br_engine *e, const char *name);
/* Sets a bus volume. */
void br_audio_bus_set_volume(br_engine *e, int bus, float volume);
/* Mutes or unmutes a bus. */
void br_audio_bus_set_muted(br_engine *e, int bus, bool muted);
/* Pauses or resumes every instance on a bus. */
void br_audio_bus_set_paused(br_engine *e, int bus, bool paused);
/* Sets the master volume (default 1). */
void br_audio_set_master_volume(br_engine *e, float volume);
/* Mutes or unmutes everything. */
void br_audio_set_muted(br_engine *e, bool muted);

/* ---- mixer and device */

/* Mixes `frames` stereo frames into out (overwritten, float32 interleaved L R). What the audio
 * thread runs; tests call it directly on headless engines (which turns off game-clock mixing). */
void br_audio_mix(br_engine *e, float *out, uint32_t frames);
/* Ogg files longer than this many seconds load as streaming sounds (affects later loads). */
void br_audio_set_stream_threshold(br_engine *e, float seconds);
/* True once the playback device is open. */
bool br_audio_device_open(br_engine *e);
/* Pauses or resumes the whole output device (the app does this while inactive). */
void br_audio_set_device_paused(br_engine *e, bool paused);
/* True while the output device is paused by br_audio_set_device_paused. */
bool br_audio_device_paused(br_engine *e);

#endif
