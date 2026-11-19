"""Generates the sounds used by examples/audio_demo.c (Python standard library only).

    python examples/audio/make_sounds.py

beep.wav   - 0.15 s 880 Hz sine with a short fade (16-bit mono, 44.1 kHz: tests resampling)
tones.wav  - three 0.2 s notes C5 E5 G5 at 0.0 / 0.25 / 0.5 s (a sound-sprite sheet, 48 kHz stereo)
hum.wav    - 1.0 s seamless 110 Hz + 220 Hz loop (16-bit stereo, 48 kHz)
gong.ogg   - not synthesised (a Vorbis encoder is out of scope for a stdlib script): a CC0 file
             from Wikimedia Commons, downloaded once and checked by SHA-256. See LICENSE-gong.txt.
"""
import hashlib
import math
import os
import struct
import urllib.request
import wave

HERE = os.path.dirname(os.path.abspath(__file__))

GONG_URL = "https://upload.wikimedia.org/wikipedia/commons/4/42/Gong_or_bell_vibrant_%28short%29.ogg"
GONG_SHA256 = "f57283d08a0ecb052425af1ac1457f827fa27f423d49158963b3370c89fc942d"


def write_wav(name, rate, channels, frames):
    """frames: list of per-frame tuples of floats in -1..1."""
    with wave.open(os.path.join(HERE, name), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for frame in frames:
            for v in frame:
                data += struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767))
        w.writeframes(bytes(data))


def tone(freq, seconds, rate, volume=0.5, fade=0.01):
    n = int(seconds * rate)
    fade_n = max(1, int(fade * rate))
    out = []
    for i in range(n):
        env = min(1.0, i / fade_n, (n - 1 - i) / fade_n)
        out.append(volume * env * math.sin(2.0 * math.pi * freq * i / rate))
    return out


def main():
    beep = tone(880.0, 0.15, 44100)
    write_wav("beep.wav", 44100, 1, [(v,) for v in beep])

    rate = 48000
    sheet = [0.0] * int(0.75 * rate)
    for start, freq in ((0.0, 523.25), (0.25, 659.25), (0.5, 783.99)):
        note = tone(freq, 0.2, rate, volume=0.4)
        first = int(start * rate)
        for i, v in enumerate(note):
            sheet[first + i] = v
    write_wav("tones.wav", rate, 2, [(v, v) for v in sheet])

    # whole periods of both frequencies fit in 1 s, so the loop point is seamless
    hum = []
    for i in range(rate):
        t = i / rate
        hum.append(0.2 * math.sin(2 * math.pi * 110 * t) + 0.1 * math.sin(2 * math.pi * 220 * t))
    write_wav("hum.wav", rate, 2, [(v, v * 0.8) for v in hum])

    gong = os.path.join(HERE, "gong.ogg")
    if not os.path.exists(gong):
        req = urllib.request.Request(GONG_URL, headers={"User-Agent": "braty-make-sounds/1.0"})
        with urllib.request.urlopen(req) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != GONG_SHA256:
            raise SystemExit("gong.ogg: unexpected SHA-256, not saved")
        with open(gong, "wb") as f:
            f.write(data)
    print("sounds written to", HERE)


if __name__ == "__main__":
    main()
