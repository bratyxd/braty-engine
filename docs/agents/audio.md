# audio - sounds, instances, buses, mixer (@pixi/sound)

Header: `engine/include/braty/br_audio.h`. Source: `engine/src/audio/br_audio.c`.
Test: `tests/test_audio.c` (headless mixer). Example: `examples/audio_demo.c`.

## The model

- A `br_sound` is loaded through the asset cache (`wav` via SDL, `ogg` via stb_vorbis) and
  converted once to the mixer format: float32 stereo, 48 kHz. Ogg files longer than the stream
  threshold (10 s default, `br_audio_set_stream_threshold`) stay compressed and every playing
  instance decodes its own copy while it plays.
- `br_audio_play` starts an instance (`br_sound_instance` handle; stale handles are ignored).
  Options: volume, pan (balance: -1 left only, 0 unchanged, +1 right only), speed (linear
  interpolation, pitch changes), loop, start offset, sprite (named range), bus, completion callback.
- Buses are flat named groups with volume / mute / pause; plus master volume and mute.
- The mixer `br_audio_mix` runs on SDL's audio thread. All state it shares with the main thread
  sits behind one mutex. Completion callbacks run on the main thread (`audio.update`, UPDATE).
- The device opens on the first `br_audio_play` when the engine has a window. Headless engines
  never open one. Whenever no device is open (headless, or opening it failed: no output device,
  RDP, a bad `SDL_AUDIO_DRIVER`) the engine mixes by game time in `audio.update`, so instances
  end and call back (on the same frame every `--deterministic` run). Code that calls
  `br_audio_mix` itself (tests) turns that off and drives the mixer alone. A windowed run with a
  device plays on the device's wall clock.
- Every float argument is sanitized at the API: NaN / INF -> default (volume 1, pan 0, speed 1,
  start / seek 0), then clamped (volume 0..16, pan -1..1, speed 0.01..16). The loop wrap is one
  `SDL_fmod`, so no speed can stall the audio thread; NaN samples in PCM data mix as silence.

## Functions you actually need

| Function | What for |
|---|---|
| `br_sound_load(e, path)` | wav/ogg through the asset cache (NULL + log on failure). |
| `br_sound_create_pcm(e, samples, frames, channels, rate)` | Sounds from code. |
| `br_sound_add_sprite(e, sound, name, start, duration)` | Sound sprites. |
| `br_audio_play_defaults()`, `br_audio_play(e, sound, &desc)` | Play. |
| `br_audio_stop / set_paused / set_volume / set_pan / set_speed / set_loop / seek / position` | Control an instance. |
| `br_audio_bus_create(e, "music")`, `br_audio_bus_set_volume/muted/paused` | Groups. |
| `br_audio_set_master_volume`, `br_audio_set_muted` | Global. |
| `br_audio_mix(e, out, frames)` | The mixer (tests). |

## Snippet (from examples/audio_demo.c)

```c
br_audio_set_stream_threshold(e, 2.0f);   /* stream the 5.6 s gong to show streaming */
s->beep = br_sound_load(e, "beep.wav");
s->tones = br_sound_load(e, "tones.wav");
s->gong = br_sound_load(e, "gong.ogg");
s->hum = br_sound_load(e, "hum.wav");
if (s->beep == NULL || s->tones == NULL || s->gong == NULL || s->hum == NULL) {
    return false;
}
br_sound_add_sprite(e, s->tones, "c", 0.0f, 0.2f);
br_sound_add_sprite(e, s->tones, "e", 0.25f, 0.2f);
br_sound_add_sprite(e, s->tones, "g", 0.5f, 0.2f);
s->music_bus = br_audio_bus_create(e, "music");
```

```c
static void play_pad(br_engine *e, demo_state *s, int pad)
{
    br_audio_play_desc play = br_audio_play_defaults();
    play.on_complete = on_complete;
    play.user = s;
    if (pad == 0) {
        play.volume = 0.5f;
        s->playing[0] = br_audio_play(e, s->beep, &play);
    } else if (pad == 1) {
        const char *names[3] = { "c", "e", "g" };
        play.sprite = names[s->next_tone % 3];
        play.pan = (float)(s->next_tone % 3) - 1.0f;   /* left, centre, right */
        s->next_tone++;
        s->playing[1] = br_audio_play(e, s->tones, &play);
    } else if (pad == 2) {
        s->playing[2] = br_audio_play(e, s->gong, &play);
    } else if (br_audio_is_playing(e, s->playing[3])) {
        br_audio_stop(e, s->playing[3]);
    } else {
        play.loop = true;
        play.bus = s->music_bus;
        play.on_complete = NULL;   /* loops never complete */
        s->playing[3] = br_audio_play(e, s->hum, &play);
    }
    br_log_info("audio_demo: pad '%s' (%u instances playing)", g_pad_names[pad], br_audio_instance_count(e));
}
```

Demo sounds are made by `examples/audio/make_sounds.py` (stdlib only); `gong.ogg` is a CC0
download (see `examples/audio/LICENSE-gong.txt`).

## Gotchas

- Limits: 64 instances, 15 buses, 16 sprites per sound (`BR_AUDIO_MAX_*`). Play returns a zero
  handle when all instances are busy.
- `br_sound_destroy` is only for `br_sound_create_pcm` sounds; asset-loaded sounds are freed by
  `br_assets_unload`. Either way playing instances of that sound stop first.
- Stopping an instance does not fire its completion callback; finishing does. Loops never finish.
- Speed on streaming instances uses the SDL audio stream's frequency ratio; on memory sounds it
  is linear interpolation (no anti-alias filter: fine for 0.5x-2x).
- Output is clamped to -1..1 after summing (no limiter).
