/* br_audio: sounds (wav, ogg), instances, buses, the mixer on SDL's audio thread.
 *
 * Threading: the mixer (br_audio_mix) runs on SDL's audio thread. Everything it reads that the
 * main thread can change (voices, buses, master volume) is in br_audio_state and guarded by
 * `lock`. Sound sample data is immutable after creation, so it is read without the lock.
 */
#include "app/br_engine_internal.h"
#include "braty/br_assets.h"
#include "braty/br_audio.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

#define BR_AUDIO_MAX_SPRITES   16
#define BR_AUDIO_NAME_SIZE     32
#define BR_AUDIO_CHUNK_FRAMES  1024

typedef struct br_sound_sprite {
    char     name[BR_AUDIO_NAME_SIZE];
    uint32_t start;              /* mixer frames */
    uint32_t end;
} br_sound_sprite;

struct br_sound {
    float   *samples;            /* stereo frames at BR_AUDIO_RATE; NULL when streaming */
    uint32_t frames;             /* length in mixer frames */
    bool     streaming;
    uint8_t *ogg;                /* streaming: the compressed file */
    size_t   ogg_size;
    int      src_channels;       /* streaming: source format */
    int      src_rate;
    br_sound_sprite sprites[BR_AUDIO_MAX_SPRITES];
    uint32_t sprite_count;
};

typedef struct br_voice {
    bool      active;
    bool      finished;          /* set by the mixer; the main thread fires the callback and frees */
    uint32_t  generation;
    br_sound *sound;
    double    pos;               /* mixer frames from `start` */
    uint32_t  start;             /* range in mixer frames */
    uint32_t  end;
    float     volume;
    float     pan;
    float     speed;
    bool      loop;
    bool      paused;
    int       bus;
    br_sound_complete_fn on_complete;
    void     *user;
    /* streaming */
    stb_vorbis      *vorbis;
    SDL_AudioStream *convert;    /* source format -> mixer format, frequency ratio = speed */
    uint32_t         src_pos;    /* next source sample to decode */
    bool             eof;
} br_voice;

typedef struct br_bus {
    bool  used;
    char  name[BR_AUDIO_NAME_SIZE];
    float volume;
    bool  muted;
    bool  paused;
} br_bus;

typedef struct br_audio_state {
    SDL_Mutex       *lock;
    br_voice         voices[BR_AUDIO_MAX_INSTANCES];
    br_bus           buses[BR_AUDIO_MAX_BUSES];   /* [0] = master-only, never named */
    float            master_volume;
    bool             muted;
    float            stream_seconds;  /* main thread only (loaders) */
    SDL_AudioStream *device;
    bool             device_failed;
    double           game_clock_frames; /* no device + deterministic: mixer frames owed to game time */
    /* audio-thread scratch (only br_audio_mix touches these) */
    float            mix_buf[BR_AUDIO_CHUNK_FRAMES * BR_AUDIO_CHANNELS];
    float            stream_buf[BR_AUDIO_CHUNK_FRAMES * BR_AUDIO_CHANNELS];
    float            decode_buf[BR_AUDIO_CHUNK_FRAMES * 8];
} br_audio_state;

static const SDL_AudioSpec br_audio_mixer_spec = { SDL_AUDIO_F32, BR_AUDIO_CHANNELS, BR_AUDIO_RATE };

/* ---- sounds */

/* Converts any PCM to the mixer format; sound->samples is SDL-allocated (free with SDL_free). */
static br_sound *br_sound_from_pcm(const SDL_AudioSpec *spec, const void *data, int bytes)
{
    Uint8 *out = NULL;
    int out_len = 0;
    if (!SDL_ConvertAudioSamples(spec, data, bytes, &br_audio_mixer_spec, &out, &out_len)) {
        br_log_error("audio: cannot convert samples: %s", SDL_GetError());
        return NULL;
    }
    br_sound *sound = br_alloc(sizeof(br_sound));
    if (sound == NULL) {
        SDL_free(out);
        return NULL;
    }
    sound->samples = (float *)out;
    sound->frames = (uint32_t)out_len / (sizeof(float) * BR_AUDIO_CHANNELS);
    return sound;
}

br_sound *br_sound_create_pcm(br_engine *e, const float *samples, uint32_t frames, int channels, int rate)
{
    (void)e;
    if (samples == NULL || frames == 0 || channels <= 0 || rate <= 0) {
        br_log_error("audio: br_sound_create_pcm: bad arguments");
        return NULL;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_F32, channels, rate };
    return br_sound_from_pcm(&spec, samples, (int)(frames * (uint32_t)channels * sizeof(float)));
}

static void br_audio_free_voice(br_voice *v)
{
    if (v->vorbis != NULL) {
        stb_vorbis_close(v->vorbis);
    }
    if (v->convert != NULL) {
        SDL_DestroyAudioStream(v->convert);
    }
    uint32_t generation = v->generation + 1;
    SDL_zerop(v);
    v->generation = generation == 0 ? 1 : generation;
}

static void br_sound_free(br_engine *e, br_sound *sound)
{
    br_audio_state *s = e->audio;
    if (s != NULL) {
        SDL_LockMutex(s->lock);
        for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
            if (s->voices[i].active && s->voices[i].sound == sound) {
                br_audio_free_voice(&s->voices[i]);
            }
        }
        SDL_UnlockMutex(s->lock);
    }
    SDL_free(sound->samples); /* from SDL_ConvertAudioSamples, not br_alloc */
    br_free(sound->ogg);
    br_free(sound);
}

void br_sound_destroy(br_engine *e, br_sound *sound)
{
    if (sound != NULL) {
        br_sound_free(e, sound);
    }
}

float br_sound_duration(const br_sound *sound)
{
    return sound != NULL ? (float)sound->frames / (float)BR_AUDIO_RATE : 0.0f;
}

bool br_sound_is_streaming(const br_sound *sound)
{
    return sound != NULL && sound->streaming;
}

bool br_sound_add_sprite(br_engine *e, br_sound *sound, const char *name, float start, float duration)
{
    (void)e;
    if (sound == NULL || name == NULL || sound->sprite_count == BR_AUDIO_MAX_SPRITES || start < 0.0f ||
        duration <= 0.0f) {
        br_log_error("audio: cannot add sprite '%s'", name != NULL ? name : "");
        return false;
    }
    br_sound_sprite *sp = &sound->sprites[sound->sprite_count];
    br_str_copy(sp->name, sizeof(sp->name), name);
    double first = (double)start * BR_AUDIO_RATE + 0.5;   /* rounded to whole frames */
    double last = ((double)start + (double)duration) * BR_AUDIO_RATE + 0.5;
    sp->start = first < sound->frames ? (uint32_t)first : sound->frames;
    sp->end = last < sound->frames ? (uint32_t)last : sound->frames;
    if (sp->end <= sp->start) {
        br_log_error("audio: sprite '%s' is outside the sound", name);
        return false;
    }
    sound->sprite_count++;
    return true;
}

/* ---- loaders */

static void *br_audio_load_wav(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    SDL_AudioSpec spec;
    Uint8 *data = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(bytes, size), true, &spec, &data, &len)) {
        br_log_error("audio: '%s': %s", path, SDL_GetError());
        return NULL;
    }
    br_sound *sound = br_sound_from_pcm(&spec, data, (int)len);
    SDL_free(data);
    return sound;
}

static void *br_audio_load_ogg(br_engine *e, const char *path, const void *bytes, size_t size)
{
    int error = 0;
    stb_vorbis *v = stb_vorbis_open_memory(bytes, (int)size, &error, NULL);
    if (v == NULL) {
        br_log_error("audio: '%s': not a valid Ogg Vorbis file (stb_vorbis error %d)", path, error);
        return NULL;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    uint32_t samples = stb_vorbis_stream_length_in_samples(v);
    br_sound *sound = NULL;
    if (samples == 0 || info.channels <= 0 || info.sample_rate == 0) {
        br_log_error("audio: '%s': empty or unsupported Ogg stream", path);
    } else if ((float)samples / (float)info.sample_rate > e->audio->stream_seconds) {
        /* long file: keep it compressed and decode while playing */
        sound = br_alloc(sizeof(br_sound));
        uint8_t *copy = br_alloc(size);
        if (sound != NULL && copy != NULL) {
            SDL_memcpy(copy, bytes, size);
            sound->streaming = true;
            sound->ogg = copy;
            sound->ogg_size = size;
            sound->src_channels = info.channels;
            sound->src_rate = (int)info.sample_rate;
            sound->frames = (uint32_t)((uint64_t)samples * BR_AUDIO_RATE / info.sample_rate);
        } else {
            br_free(copy);
            br_free(sound);
            sound = NULL;
        }
    } else {
        size_t floats = (size_t)samples * (size_t)info.channels;
        float *pcm = br_alloc(floats * sizeof(float));
        if (pcm != NULL) {
            size_t done = 0;
            while (done < floats) {
                int n = stb_vorbis_get_samples_float_interleaved(v, info.channels, pcm + done, (int)(floats - done));
                if (n <= 0) {
                    break;
                }
                done += (size_t)n * (size_t)info.channels;
            }
            SDL_AudioSpec spec = { SDL_AUDIO_F32, info.channels, (int)info.sample_rate };
            sound = br_sound_from_pcm(&spec, pcm, (int)(done * sizeof(float)));
            br_free(pcm);
        }
    }
    stb_vorbis_close(v);
    return sound;
}

static void br_audio_unload(br_engine *e, void *asset)
{
    br_sound_free(e, asset);
}

br_sound *br_sound_load(br_engine *e, const char *path)
{
    void *asset = br_assets_load(e, path);
    if (asset == NULL) {
        return NULL;
    }
    if (!br_str_equal(br_assets_type(e, path), "sound")) {
        br_log_error("audio: '%s' is not a sound", path);
        return NULL;
    }
    return asset;
}

/* ---- mixer (audio thread, lock held) */

static void br_audio_voice_gains(br_audio_state *s, const br_voice *v, float *left, float *right)
{
    const br_bus *bus = &s->buses[v->bus];
    float volume = v->volume * bus->volume * s->master_volume;
    if (s->muted || bus->muted) {
        volume = 0.0f;
    }
    *left = volume * (v->pan > 0.0f ? 1.0f - v->pan : 1.0f);
    *right = volume * (v->pan < 0.0f ? 1.0f + v->pan : 1.0f);
}

static void br_audio_mix_memory(br_audio_state *s, br_voice *v, float *out, uint32_t frames)
{
    float gl, gr;
    br_audio_voice_gains(s, v, &gl, &gr);
    const float *data = v->sound->samples;
    double length = (double)(v->end - v->start);
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t index = (uint32_t)v->pos;
        float frac = (float)(v->pos - (double)index);
        uint32_t a = v->start + index;
        uint32_t b = a + 1;
        if (b >= v->end) {
            b = v->loop ? v->start : a;
        }
        float l = data[a * 2] + (data[b * 2] - data[a * 2]) * frac;
        float r = data[a * 2 + 1] + (data[b * 2 + 1] - data[a * 2 + 1]) * frac;
        out[i * 2] += l * gl;
        out[i * 2 + 1] += r * gr;
        v->pos += v->speed;
        if (v->pos >= length) {
            if (!v->loop) {
                v->finished = true;
                return;
            }
            while (v->pos >= length) {
                v->pos -= length;
            }
        }
    }
}

static uint32_t br_audio_src_sample(const br_sound *sound, uint32_t mixer_frame)
{
    return (uint32_t)((uint64_t)mixer_frame * (uint64_t)sound->src_rate / BR_AUDIO_RATE);
}

/* Decodes one chunk into the converter. False when nothing more will come. */
static bool br_audio_stream_feed(br_audio_state *s, br_voice *v)
{
    const br_sound *sound = v->sound;
    uint32_t end_src = br_audio_src_sample(sound, v->end);
    int channels = sound->src_channels;
    int max_frames = (int)(BR_ARRAY_COUNT(s->decode_buf) / (size_t)channels);
    if (end_src > v->src_pos && (uint32_t)max_frames > end_src - v->src_pos) {
        max_frames = (int)(end_src - v->src_pos);
    }
    int n = 0;
    if (v->src_pos < end_src) {
        n = stb_vorbis_get_samples_float_interleaved(v->vorbis, channels, s->decode_buf, max_frames * channels);
    }
    if (n > 0) {
        SDL_PutAudioStreamData(v->convert, s->decode_buf, n * channels * (int)sizeof(float));
        v->src_pos += (uint32_t)n;
    }
    if (n > 0 && v->src_pos < end_src) {
        return true;
    }
    if (v->loop && (n > 0 || v->src_pos != br_audio_src_sample(sound, v->start))) {
        v->src_pos = br_audio_src_sample(sound, v->start);
        stb_vorbis_seek(v->vorbis, v->src_pos);
        return true;
    }
    SDL_FlushAudioStream(v->convert);
    v->eof = true;
    return false;
}

static void br_audio_mix_stream(br_audio_state *s, br_voice *v, float *out, uint32_t frames)
{
    float gl, gr;
    br_audio_voice_gains(s, v, &gl, &gr);
    double length = (double)(v->end - v->start);
    uint32_t produced = 0;
    while (produced < frames) {
        int available = SDL_GetAudioStreamAvailable(v->convert) / (int)(sizeof(float) * BR_AUDIO_CHANNELS);
        if (available <= 0) {
            if (v->eof || !br_audio_stream_feed(s, v)) {
                if (SDL_GetAudioStreamAvailable(v->convert) <= 0) {
                    v->finished = true;
                    return;
                }
            }
            continue;
        }
        uint32_t want = frames - produced;
        if (want > (uint32_t)available) {
            want = (uint32_t)available;
        }
        if (want > BR_AUDIO_CHUNK_FRAMES) {
            want = BR_AUDIO_CHUNK_FRAMES;
        }
        int got = SDL_GetAudioStreamData(v->convert, s->stream_buf, (int)(want * sizeof(float) * BR_AUDIO_CHANNELS));
        uint32_t got_frames = got > 0 ? (uint32_t)got / (sizeof(float) * BR_AUDIO_CHANNELS) : 0;
        if (got_frames == 0) {
            return;
        }
        for (uint32_t i = 0; i < got_frames; i++) {
            out[(produced + i) * 2] += s->stream_buf[i * 2] * gl;
            out[(produced + i) * 2 + 1] += s->stream_buf[i * 2 + 1] * gr;
        }
        produced += got_frames;
        v->pos += (double)got_frames * v->speed;
        while (v->loop && v->pos >= length && length > 0.0) {
            v->pos -= length;
        }
    }
}

void br_audio_mix(br_engine *e, float *out, uint32_t frames)
{
    br_audio_state *s = e->audio;
    SDL_memset(out, 0, (size_t)frames * BR_AUDIO_CHANNELS * sizeof(float));
    if (s == NULL) {
        return;
    }
    SDL_LockMutex(s->lock);
    for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        br_voice *v = &s->voices[i];
        if (!v->active || v->finished || v->paused || s->buses[v->bus].paused) {
            continue;
        }
        if (v->sound->streaming) {
            br_audio_mix_stream(s, v, out, frames);
        } else {
            br_audio_mix_memory(s, v, out, frames);
        }
    }
    SDL_UnlockMutex(s->lock);
    for (uint32_t i = 0; i < frames * BR_AUDIO_CHANNELS; i++) {
        out[i] = br_clampf(out[i], -1.0f, 1.0f);
    }
}

static void SDLCALL br_audio_device_callback(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)total;
    br_engine *e = user;
    br_audio_state *s = e->audio;
    int frame_bytes = (int)(sizeof(float) * BR_AUDIO_CHANNELS);
    int frames = additional / frame_bytes;
    while (frames > 0) {
        int chunk = frames < BR_AUDIO_CHUNK_FRAMES ? frames : BR_AUDIO_CHUNK_FRAMES;
        br_audio_mix(e, s->mix_buf, (uint32_t)chunk);
        SDL_PutAudioStreamData(stream, s->mix_buf, chunk * frame_bytes);
        frames -= chunk;
    }
}

/* ---- device */

static void br_audio_open_device(br_engine *e)
{
    br_audio_state *s = e->audio;
    if (s->device != NULL || s->device_failed || br_engine_window(e) == NULL) {
        return;
    }
    s->device_failed = true;   /* try once */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        br_log_error("audio: cannot init SDL audio: %s", SDL_GetError());
        return;
    }
    s->device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &br_audio_mixer_spec,
                                          br_audio_device_callback, e);
    if (s->device == NULL) {
        br_log_error("audio: cannot open playback device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    s->device_failed = false;
    SDL_ResumeAudioStreamDevice(s->device);
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s->device));
    br_log_info("audio: device opened (%s, %d Hz, %d channels, driver %s)", name != NULL ? name : "default",
                BR_AUDIO_RATE, BR_AUDIO_CHANNELS, SDL_GetCurrentAudioDriver());
}

void br_audio_set_stream_threshold(br_engine *e, float seconds)
{
    e->audio->stream_seconds = seconds < 0.0f ? 0.0f : seconds;
}

bool br_audio_device_open(br_engine *e)
{
    return e->audio != NULL && e->audio->device != NULL;
}

/* ---- instances */

br_audio_play_desc br_audio_play_defaults(void)
{
    br_audio_play_desc d;
    SDL_zero(d);
    d.volume = 1.0f;
    d.speed = 1.0f;
    return d;
}

static br_voice *br_audio_voice(br_audio_state *s, br_sound_instance i)
{
    if (i.index >= BR_AUDIO_MAX_INSTANCES) {
        return NULL;
    }
    br_voice *v = &s->voices[i.index];
    return v->active && !v->finished && v->generation == i.generation ? v : NULL;
}

static bool br_audio_start_stream(br_voice *v)
{
    const br_sound *sound = v->sound;
    int error = 0;
    v->vorbis = stb_vorbis_open_memory(sound->ogg, (int)sound->ogg_size, &error, NULL);
    SDL_AudioSpec src = { SDL_AUDIO_F32, sound->src_channels, sound->src_rate };
    v->convert = SDL_CreateAudioStream(&src, &br_audio_mixer_spec);
    if (v->vorbis == NULL || v->convert == NULL) {
        br_log_error("audio: cannot start stream (vorbis error %d, %s)", error, SDL_GetError());
        return false;
    }
    SDL_SetAudioStreamFrequencyRatio(v->convert, v->speed);
    v->src_pos = br_audio_src_sample(sound, v->start + (uint32_t)v->pos);
    stb_vorbis_seek(v->vorbis, v->src_pos);
    return true;
}

br_sound_instance br_audio_play(br_engine *e, br_sound *sound, const br_audio_play_desc *desc)
{
    br_sound_instance handle = { 0, 0 };
    br_audio_state *s = e->audio;
    if (s == NULL || sound == NULL || sound->frames == 0) {
        br_log_error("audio: play: no sound");
        return handle;
    }
    br_audio_play_desc d = desc != NULL ? *desc : br_audio_play_defaults();
    uint32_t start = 0;
    uint32_t end = sound->frames;
    if (d.sprite != NULL) {
        uint32_t k = 0;
        while (k < sound->sprite_count && !br_str_equal(sound->sprites[k].name, d.sprite)) {
            k++;
        }
        if (k == sound->sprite_count) {
            br_log_error("audio: play: unknown sprite '%s'", d.sprite);
            return handle;
        }
        start = sound->sprites[k].start;
        end = sound->sprites[k].end;
    }
    br_audio_open_device(e);   /* outside the lock: the device callback takes it */

    br_voice fresh;
    SDL_zero(fresh);
    fresh.active = true;
    fresh.sound = sound;
    fresh.start = start;
    fresh.end = end;
    fresh.pos = br_clampf(d.start, 0.0f, br_sound_duration(sound)) * (double)BR_AUDIO_RATE;
    if (fresh.pos >= (double)(end - start)) {
        fresh.pos = 0.0;
    }
    fresh.volume = d.volume < 0.0f ? 0.0f : d.volume;
    fresh.pan = br_clampf(d.pan, -1.0f, 1.0f);
    fresh.speed = d.speed > 0.01f ? d.speed : 0.01f;
    fresh.loop = d.loop;
    fresh.paused = d.paused;
    fresh.bus = d.bus > 0 && d.bus < BR_AUDIO_MAX_BUSES && s->buses[d.bus].used ? d.bus : 0;
    fresh.on_complete = d.on_complete;
    fresh.user = d.user;
    if (sound->streaming && !br_audio_start_stream(&fresh)) {
        br_audio_free_voice(&fresh);
        return handle;
    }

    SDL_LockMutex(s->lock);
    for (uint32_t i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        br_voice *v = &s->voices[i];
        if (!v->active) {
            fresh.generation = v->generation;
            *v = fresh;
            handle.index = i;
            handle.generation = v->generation;
            break;
        }
    }
    SDL_UnlockMutex(s->lock);
    if (handle.generation == 0) {
        br_log_warn("audio: all %d instances busy, sound not played", BR_AUDIO_MAX_INSTANCES);
        br_audio_free_voice(&fresh);
    }
    return handle;
}

void br_audio_stop(br_engine *e, br_sound_instance i)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        br_audio_free_voice(v);
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_stop_all(br_engine *e)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        if (s->voices[i].active) {
            br_audio_free_voice(&s->voices[i]);
        }
    }
    SDL_UnlockMutex(s->lock);
}

bool br_audio_is_playing(br_engine *e, br_sound_instance i)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    bool playing = br_audio_voice(s, i) != NULL;
    SDL_UnlockMutex(s->lock);
    return playing;
}

void br_audio_set_paused(br_engine *e, br_sound_instance i, bool paused)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        v->paused = paused;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_volume(br_engine *e, br_sound_instance i, float volume)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        v->volume = volume < 0.0f ? 0.0f : volume;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_pan(br_engine *e, br_sound_instance i, float pan)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        v->pan = br_clampf(pan, -1.0f, 1.0f);
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_speed(br_engine *e, br_sound_instance i, float speed)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        v->speed = speed > 0.01f ? speed : 0.01f;
        if (v->convert != NULL) {
            SDL_SetAudioStreamFrequencyRatio(v->convert, v->speed);
        }
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_loop(br_engine *e, br_sound_instance i, bool loop)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        v->loop = loop;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_seek(br_engine *e, br_sound_instance i, float seconds)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    if (v != NULL) {
        double frame = (double)(seconds < 0.0f ? 0.0f : seconds) * BR_AUDIO_RATE;
        double length = (double)(v->end - v->start);
        v->pos = frame < length ? frame : length - 1.0;
        if (v->vorbis != NULL) {
            v->src_pos = br_audio_src_sample(v->sound, v->start + (uint32_t)v->pos);
            stb_vorbis_seek(v->vorbis, v->src_pos);
            SDL_ClearAudioStream(v->convert);
            v->eof = false;
        }
    }
    SDL_UnlockMutex(s->lock);
}

float br_audio_position(br_engine *e, br_sound_instance i)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_voice *v = br_audio_voice(s, i);
    float seconds = v != NULL ? (float)(v->pos / BR_AUDIO_RATE) : 0.0f;
    SDL_UnlockMutex(s->lock);
    return seconds;
}

uint32_t br_audio_instance_count(br_engine *e)
{
    br_audio_state *s = e->audio;
    uint32_t count = 0;
    SDL_LockMutex(s->lock);
    for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        count += s->voices[i].active && !s->voices[i].finished ? 1u : 0u;
    }
    SDL_UnlockMutex(s->lock);
    return count;
}

/* ---- buses */

int br_audio_bus_find(br_engine *e, const char *name)
{
    br_audio_state *s = e->audio;
    for (int i = 1; i < BR_AUDIO_MAX_BUSES; i++) {
        if (s->buses[i].used && br_str_equal(s->buses[i].name, name)) {
            return i;
        }
    }
    return 0;
}

int br_audio_bus_create(br_engine *e, const char *name)
{
    br_audio_state *s = e->audio;
    int found = br_audio_bus_find(e, name);
    if (found != 0 || name == NULL) {
        return found;
    }
    for (int i = 1; i < BR_AUDIO_MAX_BUSES; i++) {
        if (!s->buses[i].used) {
            SDL_LockMutex(s->lock);
            s->buses[i].used = true;
            s->buses[i].volume = 1.0f;
            br_str_copy(s->buses[i].name, sizeof(s->buses[i].name), name);
            SDL_UnlockMutex(s->lock);
            return i;
        }
    }
    br_log_error("audio: more than %d buses", BR_AUDIO_MAX_BUSES - 1);
    return 0;
}

static br_bus *br_audio_bus(br_audio_state *s, int bus)
{
    return bus > 0 && bus < BR_AUDIO_MAX_BUSES && s->buses[bus].used ? &s->buses[bus] : NULL;
}

void br_audio_bus_set_volume(br_engine *e, int bus, float volume)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_bus *b = br_audio_bus(s, bus);
    if (b != NULL) {
        b->volume = volume < 0.0f ? 0.0f : volume;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_bus_set_muted(br_engine *e, int bus, bool muted)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_bus *b = br_audio_bus(s, bus);
    if (b != NULL) {
        b->muted = muted;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_bus_set_paused(br_engine *e, int bus, bool paused)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    br_bus *b = br_audio_bus(s, bus);
    if (b != NULL) {
        b->paused = paused;
    }
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_master_volume(br_engine *e, float volume)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    s->master_volume = volume < 0.0f ? 0.0f : volume;
    SDL_UnlockMutex(s->lock);
}

void br_audio_set_muted(br_engine *e, bool muted)
{
    br_audio_state *s = e->audio;
    SDL_LockMutex(s->lock);
    s->muted = muted;
    SDL_UnlockMutex(s->lock);
}

/* ---- main-thread update: completion callbacks */

typedef struct br_audio_done {
    br_sound_complete_fn fn;
    void *user;
    br_sound_instance instance;
} br_audio_done;

static void br_audio_update(br_engine *e, float dt, void *user)
{
    (void)user;
    br_audio_state *s = e->audio;
    /* Without a device (headless, or it failed to open) nothing pulls samples, so instances
     * never finished: a headless soak kept every sound "playing" until the 64 slots ran out.
     * Under --deterministic they advance by game time here instead, so they end (and call back)
     * on the same frame every run. Other headless engines keep driving br_audio_mix themselves. */
    // ponytail: a deterministic run *with* a device still plays on the wall clock (fast-forwarded
    // runs leave sounds playing); mute the device and use this path if that ever matters.
    if (s->device == NULL && br_app_deterministic(e) && dt > 0.0f) {
        s->game_clock_frames += (double)dt * BR_AUDIO_RATE;
        uint32_t frames = (uint32_t)s->game_clock_frames;
        s->game_clock_frames -= (double)frames;
        while (frames > 0) {
            uint32_t chunk = frames < BR_AUDIO_CHUNK_FRAMES ? frames : BR_AUDIO_CHUNK_FRAMES;
            br_audio_mix(e, s->mix_buf, chunk); /* no audio thread: the scratch is free */
            frames -= chunk;
        }
    }
    br_audio_done done[BR_AUDIO_MAX_INSTANCES];
    uint32_t count = 0;
    SDL_LockMutex(s->lock);
    for (uint32_t i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        br_voice *v = &s->voices[i];
        if (v->active && v->finished) {
            done[count].fn = v->on_complete;
            done[count].user = v->user;
            done[count].instance.index = i;
            done[count].instance.generation = v->generation;
            count++;
            br_audio_free_voice(v);
        }
    }
    SDL_UnlockMutex(s->lock);
    /* outside the lock: callbacks may play new sounds */
    for (uint32_t i = 0; i < count; i++) {
        if (done[i].fn != NULL) {
            done[i].fn(e, done[i].instance, done[i].user);
        }
    }
}

/* ---- module */

static void br_audio_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_audio_state *s = e->audio;
    if (s->device != NULL) {
        SDL_DestroyAudioStream(s->device);   /* stops the callback before the state goes */
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        if (s->voices[i].active) {
            br_audio_free_voice(&s->voices[i]);
        }
    }
    SDL_DestroyMutex(s->lock);
    br_free(s);
    e->audio = NULL;   /* sounds unloaded later by the asset cache skip the voice cleanup */
}

void br_audio_register(br_engine *e)
{
    br_audio_state *s = br_alloc(sizeof(br_audio_state));
    if (s == NULL) {
        return;
    }
    s->lock = SDL_CreateMutex();
    if (s->lock == NULL) {
        br_log_error("audio: cannot create mutex: %s", SDL_GetError());
        br_free(s);
        return;
    }
    e->audio = s;
    s->master_volume = 1.0f;
    s->stream_seconds = BR_AUDIO_STREAM_SECONDS;
    s->buses[0].used = true;
    s->buses[0].volume = 1.0f;
    for (int i = 0; i < BR_AUDIO_MAX_INSTANCES; i++) {
        s->voices[i].generation = 1;
    }

    br_asset_loader_desc wav;
    SDL_zero(wav);
    wav.type_name = "sound";
    wav.extensions = "wav";
    wav.load = br_audio_load_wav;
    wav.unload = br_audio_unload;
    br_assets_register_loader(e, &wav);
    br_asset_loader_desc ogg = wav;
    ogg.extensions = "ogg";
    ogg.load = br_audio_load_ogg;
    br_assets_register_loader(e, &ogg);

    br_system_add(e, "audio.update", BR_PHASE_UPDATE, -1000, br_audio_update, NULL);
    br_engine_on_shutdown(e, br_audio_shutdown, NULL);
}
