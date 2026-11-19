"""Generates every image and sound of Star Courier (Python standard library only, deterministic).

    python sample_projects/star_courier/tools/make_assets.py

Writes into sample_projects/star_courier/data/assets/:
  sprites/atlas.png + atlas.json  TexturePacker "hash" spritesheet packed here (shelf packer,
                                  2 px gutters): ship, crate, cargo, station, three debris rocks,
                                  spark, ring, life icon and a 9-slice panel (borders 14)
  bg/stars_far.png, stars_near.png  seamless 256x256 star tiles for two tiling-sprite layers
  sounds/sfx.wav                  every sound effect in one file, one per 1-second slot (a sound-
                                  sprite sheet: game.json "audio.sprites" names the slots)
  sounds/music.wav                an 8-second seamless loop (bass, pad, arpeggio)

All art is drawn from shapes (anti-aliased by 4x4 supersampling) in one palette; all sound is
synthesised. Original work made for this project (licensed like the engine code).
"""
import json
import math
import os
import random
import struct
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "data", "assets")

# palette (RGB): deep navy space, teal ship, amber cargo, violet station, slate debris
TEAL = (61, 214, 198)
TEAL_DARK = (14, 59, 70)
GLASS = (232, 251, 255)
ORANGE = (255, 138, 76)
AMBER = (255, 181, 71)
AMBER_DARK = (122, 66, 16)
AMBER_LIGHT = (255, 224, 150)
VIOLET = (167, 139, 250)
VIOLET_DARK = (49, 32, 110)
VIOLET_LIGHT = (237, 233, 254)
SLATE = (107, 114, 128)
SLATE_DARK = (38, 43, 56)
SLATE_LIGHT = (156, 163, 175)
PANEL = (18, 26, 58)
WHITE = (255, 255, 255)


# ---- PNG writing and a tiny supersampled canvas

def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


class Image:
    """RGBA float image; draw() shades every pixel from a function of (x, y) with 4x4 samples."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [(0.0, 0.0, 0.0, 0.0)] * (w * h)

    def draw(self, shade, ss=4):
        """shade(x, y) -> (r, g, b, a) in 0..255 / 0..1 or None; composited over the image."""
        for y in range(self.h):
            for x in range(self.w):
                r = g = b = a = 0.0
                for sy in range(ss):
                    for sx in range(ss):
                        c = shade(x + (sx + 0.5) / ss, y + (sy + 0.5) / ss)
                        if c is not None:
                            r += c[0] * c[3]
                            g += c[1] * c[3]
                            b += c[2] * c[3]
                            a += c[3]
                n = ss * ss
                if a <= 0.0:
                    continue
                src = (r / a, g / a, b / a, a / n)
                dr, dg, db, da = self.px[y * self.w + x]
                out_a = src[3] + da * (1.0 - src[3])
                mix = src[3] / out_a
                self.px[y * self.w + x] = (dr + (src[0] - dr) * mix, dg + (src[1] - dg) * mix,
                                           db + (src[2] - db) * mix, out_a)

    def rgba8(self):
        out = []
        for r, g, b, a in self.px:
            out += [int(min(255, max(0, r))), int(min(255, max(0, g))), int(min(255, max(0, b))),
                    int(min(255, max(0, a * 255.0 + 0.5)))]
        return out


# ---- shape helpers (signed distances: negative inside)

def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def poly_sdf(px, py, pts):
    inside = False
    d = 1e9
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            inside = not inside
        d = min(d, seg_dist(px, py, xi, yi, xj, yj))
        j = i
    return -d if inside else d


def circle_sdf(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def round_rect_sdf(px, py, x0, y0, x1, y1, r):
    qx = max(x0 + r - px, px - (x1 - r), 0.0)
    qy = max(y0 + r - py, py - (y1 - r), 0.0)
    inner = min(max(x0 + r - px, px - (x1 - r), y0 + r - py, py - (y1 - r)), 0.0)
    return math.hypot(qx, qy) + inner - r


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rgba(c, a=1.0):
    return (c[0], c[1], c[2], a)


# ---- sprites (each a function returning an Image)

def ship():
    img = Image(48, 48)
    hull = [(24, 3), (41, 41), (24, 33), (7, 41)]
    fins = [(24, 20), (45, 44), (36, 44), (24, 36), (12, 44), (3, 44)]

    def shade(x, y):
        d = poly_sdf(x, y, fins)
        if d < 0:
            return rgba(ORANGE if d < -1.5 else TEAL_DARK)
        return None

    img.draw(shade)

    def shade_hull(x, y):
        d = poly_sdf(x, y, hull)
        if d >= 0:
            return None
        if d > -2.0:
            return rgba(TEAL_DARK)
        light = max(0.0, min(1.0, (24 - x) / 16.0 + 0.5))   # lit from the left
        return rgba(lerp(TEAL, (180, 255, 245), light * 0.45))

    img.draw(shade_hull)

    def shade_glass(x, y):
        d = math.hypot((x - 24) / 4.0, (y - 19) / 7.0) - 1.0
        if d < 0:
            return rgba(lerp(GLASS, (120, 200, 230), max(0.0, (y - 14) / 12.0)))
        if d < 0.35:
            return rgba(TEAL_DARK)
        return None

    img.draw(shade_glass)
    return img


def crate(size, border):
    img = Image(size, size)
    s = float(size)

    def shade(x, y):
        d = round_rect_sdf(x, y, 1, 1, s - 1, s - 1, size * 0.16)
        if d >= 0:
            return None
        if d > -border:
            return rgba(AMBER_DARK)
        strap = abs(x - y) < size * 0.09 or abs(x + y - s) < size * 0.09
        if strap:
            return rgba(AMBER_LIGHT)
        return rgba(lerp(AMBER, (255, 150, 40), y / s))

    img.draw(shade)
    return img


def station():
    img = Image(112, 112)
    c = 56.0

    def shade(x, y):
        r = math.hypot(x - c, y - c)
        a = math.atan2(y - c, x - c)
        if abs(r - 46) < 6:                                   # outer ring
            edge = abs(r - 46) > 4.2
            lights = abs(r - 46) < 1.6 and (int((a + math.pi) / (2 * math.pi) * 16) % 2 == 0)
            if edge:
                return rgba(VIOLET_DARK)
            return rgba(VIOLET_LIGHT if lights else VIOLET)
        for k in range(4):                                     # spokes
            sa = k * math.pi / 2 + math.pi / 4
            ex, ey = c + 41 * math.cos(sa), c + 41 * math.sin(sa)
            if seg_dist(x, y, c, c, ex, ey) < 3.0 and r > 12:
                return rgba(VIOLET_DARK if seg_dist(x, y, c, c, ex, ey) > 1.8 else VIOLET)
        if r < 15:                                             # hub
            if r > 12.5:
                return rgba(VIOLET_DARK)
            return rgba(lerp(VIOLET_LIGHT, VIOLET, r / 12.5))
        return None

    img.draw(shade)
    return img


def debris(size, seed):
    rnd = random.Random(seed)
    img = Image(size, size)
    c = size / 2.0
    n = 9
    pts = []
    for i in range(n):
        a = i / n * 2 * math.pi + rnd.uniform(-0.2, 0.2)
        r = c * rnd.uniform(0.62, 0.92)
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    craters = [(c + rnd.uniform(-c * 0.35, c * 0.35), c + rnd.uniform(-c * 0.35, c * 0.35),
                rnd.uniform(c * 0.1, c * 0.2)) for _ in range(3)]

    def shade(x, y):
        d = poly_sdf(x, y, pts)
        if d >= 0:
            return None
        if d > -2.2:
            return rgba(SLATE_DARK)
        for cx, cy, cr in craters:
            cd = circle_sdf(x, y, cx, cy, cr)
            if cd < 0:
                return rgba(SLATE_DARK if cd > -1.2 else lerp(SLATE, SLATE_DARK, 0.5))
        light = max(0.0, min(1.0, ((c - x) + (c - y)) / size + 0.5))
        return rgba(lerp(SLATE, SLATE_LIGHT, light * 0.8))

    img.draw(shade)
    return img


def soft_dot(size, power):
    img = Image(size, size)
    c = size / 2.0

    def shade(x, y):
        d = math.hypot(x - c, y - c) / c
        if d >= 1:
            return None
        return rgba(WHITE, (1.0 - d) ** power)

    img.draw(shade)
    return img


def ring(size, width):
    img = Image(size, size)
    c = size / 2.0

    def shade(x, y):
        d = abs(math.hypot(x - c, y - c) - (c - width - 1))
        if d >= width:
            return None
        return rgba(WHITE, (1.0 - d / width) ** 1.5)

    img.draw(shade)
    return img


def life_icon():
    img = Image(20, 20)
    hull = [(10, 1), (18, 18), (10, 14), (2, 18)]

    def shade(x, y):
        d = poly_sdf(x, y, hull)
        if d >= 0:
            return None
        return rgba(TEAL_DARK if d > -1.2 else TEAL)

    img.draw(shade)
    return img


def panel():
    img = Image(48, 48)

    def shade(x, y):
        d = round_rect_sdf(x, y, 1, 1, 47, 47, 10)
        if d >= 0:
            return None
        if d > -2.0:
            return rgba(TEAL, 0.95)
        if d > -4.0:
            return rgba(TEAL_DARK, 0.9)
        return rgba(lerp(PANEL, (30, 42, 86), y / 48.0), 0.86)

    img.draw(shade)
    return img


def star_tile(seed, count, big):
    rnd = random.Random(seed)
    size = 256
    img = Image(size, size)
    stars = []
    for _ in range(count):
        tint = rnd.choice([(255, 255, 255), (200, 220, 255), (255, 236, 200), (190, 255, 245)])
        r = rnd.uniform(1.6, 2.8) if big else rnd.uniform(0.5, 1.1)
        stars.append((rnd.uniform(0, size), rnd.uniform(0, size), r, tint, rnd.uniform(0.35, 1.0)))

    def shade(x, y):
        best = None
        for sx, sy, r, tint, bright in stars:
            dx = (x - sx + size / 2) % size - size / 2       # wraps: the tile is seamless
            dy = (y - sy + size / 2) % size - size / 2
            if abs(dx) > 7 or abs(dy) > 7:
                continue
            d = math.hypot(dx, dy)
            a = max(0.0, 1.0 - d / r) * bright
            if big and (abs(dx) < 0.5 or abs(dy) < 0.5):      # a faint cross on the near stars
                a = max(a, bright * 0.5 * max(0.0, 1.0 - d / (r * 3.0)))
            if a > 0 and (best is None or a > best[3]):
                best = (tint[0], tint[1], tint[2], a)
        return best

    img.draw(shade, ss=2)
    return img


# ---- spritesheet packing (shelves, tallest first)

def pack(images, width, gutter=2):
    order = sorted(images, key=lambda name: -images[name][0].h)
    x = y = shelf = gutter
    placed = {}
    for name in order:
        img = images[name][0]
        if x + img.w + gutter > width:
            x = gutter
            y += shelf + gutter
            shelf = 0
        placed[name] = (x, y)
        x += img.w + gutter
        shelf = max(shelf, img.h)
    height = y + shelf + gutter
    h = 1
    while h < height:
        h *= 2
    return placed, h


def write_atlas():
    images = {
        "ship.png": (ship(), {"anchor": {"x": 0.5, "y": 0.55}}),
        "crate.png": (crate(28, 2.2), {"anchor": {"x": 0.5, "y": 0.5}}),
        "cargo.png": (crate(14, 1.4), {"anchor": {"x": 0.5, "y": 0.5}}),
        "station.png": (station(), {"anchor": {"x": 0.5, "y": 0.5}}),
        "debris_a.png": (debris(40, 11), {"anchor": {"x": 0.5, "y": 0.5}}),
        "debris_b.png": (debris(52, 23), {"anchor": {"x": 0.5, "y": 0.5}}),
        "debris_c.png": (debris(64, 37), {"anchor": {"x": 0.5, "y": 0.5}}),
        "spark.png": (soft_dot(16, 1.6), {"anchor": {"x": 0.5, "y": 0.5}}),
        "ring.png": (ring(64, 5), {"anchor": {"x": 0.5, "y": 0.5}}),
        "life.png": (life_icon(), {"anchor": {"x": 0.5, "y": 0.5}}),
        "panel.png": (panel(), {"borders": {"left": 14, "top": 14, "right": 14, "bottom": 14}}),
    }
    width = 256
    placed, height = pack(images, width)
    atlas = [0] * (width * height * 4)
    frames = {}
    for name, (img, extra) in images.items():
        ox, oy = placed[name]
        px = img.rgba8()
        for y in range(img.h):
            row = (oy + y) * width + ox
            atlas[row * 4:(row + img.w) * 4] = px[y * img.w * 4:(y + 1) * img.w * 4]
        entry = {
            "frame": {"x": ox, "y": oy, "w": img.w, "h": img.h},
            "rotated": False,
            "trimmed": False,
            "spriteSourceSize": {"x": 0, "y": 0, "w": img.w, "h": img.h},
            "sourceSize": {"w": img.w, "h": img.h},
        }
        entry.update(extra)
        frames[name] = entry
    os.makedirs(os.path.join(OUT, "sprites"), exist_ok=True)
    write_png(os.path.join(OUT, "sprites", "atlas.png"), width, height, atlas)
    sheet = {
        "frames": dict(sorted(frames.items())),
        "meta": {"app": "sample_projects/star_courier/tools/make_assets.py", "image": "atlas.png",
                 "format": "RGBA8888", "size": {"w": width, "h": height}, "scale": "1"},
    }
    with open(os.path.join(OUT, "sprites", "atlas.json"), "w", newline="\n") as f:
        json.dump(sheet, f, indent=1)
        f.write("\n")


def write_stars():
    os.makedirs(os.path.join(OUT, "bg"), exist_ok=True)
    far = star_tile(5, 110, False)
    write_png(os.path.join(OUT, "bg", "stars_far.png"), far.w, far.h, far.rgba8())
    near = star_tile(9, 14, True)
    write_png(os.path.join(OUT, "bg", "stars_near.png"), near.w, near.h, near.rgba8())


# ---- sounds

RATE = 22050


def env(i, n, attack, release):
    t = i / RATE
    end = (n - i) / RATE
    return min(1.0, t / attack if attack > 0 else 1.0, end / release if release > 0 else 1.0)


def note(freq, seconds, volume, shape="sine", attack=0.005, release=0.05, slide=0.0):
    n = int(seconds * RATE)
    out = []
    phase = 0.0
    for i in range(n):
        f = freq * (1.0 + slide * i / n)
        phase += f / RATE
        p = phase % 1.0
        if shape == "square":
            v = 1.0 if p < 0.5 else -1.0
            v *= 0.35
        elif shape == "tri":
            v = 4.0 * abs(p - 0.5) - 1.0
        else:
            v = math.sin(2 * math.pi * p)
        out.append(v * volume * env(i, n, attack, release))
    return out


def mix(dst, src, at):
    start = int(at * RATE)
    if len(dst) < start + len(src):
        dst.extend([0.0] * (start + len(src) - len(dst)))
    for i, v in enumerate(src):
        dst[start + i] += v
    return dst


def noise(seconds, volume, seed, lowpass=0.2):
    rnd = random.Random(seed)
    n = int(seconds * RATE)
    out = []
    y = 0.0
    for i in range(n):
        y += (rnd.uniform(-1, 1) - y) * lowpass
        out.append(y * volume * (1.0 - i / n) ** 2)
    return out


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767)) for v in samples))


# Sound-sprite slots of sfx.wav: (name, seconds). Slot i starts at i seconds; game.json lists them.
SFX = ["pickup", "deliver", "hit", "levelup", "start", "gameover"]


def sfx_sound(name):
    if name == "pickup":
        s = note(660, 0.08, 0.35, "square", release=0.03)
        return mix(s, note(990, 0.12, 0.35, "square", release=0.08), 0.06)
    if name == "deliver":
        s = []
        for k, f in enumerate([523.25, 659.25, 783.99, 1046.5]):
            mix(s, note(f, 0.35, 0.28, "tri", release=0.25), k * 0.06)
        return s
    if name == "hit":
        s = noise(0.45, 0.9, 3, 0.25)
        return mix(s, note(110, 0.35, 0.6, "sine", release=0.3, slide=-0.5), 0.0)
    if name == "levelup":
        s = []
        for k, f in enumerate([392.0, 523.25, 659.25, 783.99, 1046.5]):
            mix(s, note(f, 0.18, 0.25, "square", release=0.1), k * 0.07)
        return s
    if name == "start":
        return note(220, 0.5, 0.4, "tri", attack=0.02, release=0.2, slide=2.0)
    s = []
    for k, f in enumerate([392.0, 329.63, 261.63, 196.0]):
        mix(s, note(f, 0.32, 0.3, "tri", release=0.2), k * 0.16)
    return s


def write_sounds():
    os.makedirs(os.path.join(OUT, "sounds"), exist_ok=True)
    sheet = []
    for i, name in enumerate(SFX):
        s = sfx_sound(name)[:RATE]          # each sound fits its 1-second slot
        mix(sheet, s, float(i))
        print("sfx slot %-8s [%d, %.2f]" % (name, i, len(s) / RATE))
    sheet.extend([0.0] * (len(SFX) * RATE - len(sheet)))
    write_wav(os.path.join(OUT, "sounds", "sfx.wav"), sheet)

    # music: 8 s loop in A minor at 120 bpm, 4 chords x 2 s
    music = [0.0] * (8 * RATE)
    chords = [(110.0, [220.0, 261.63, 329.63]), (87.31, [174.61, 220.0, 261.63]),
              (130.81, [261.63, 329.63, 392.0]), (98.0, [196.0, 246.94, 293.66])]
    for c, (bass, pad) in enumerate(chords):
        t0 = c * 2.0
        for beat in range(4):
            mix(music, note(bass, 0.45, 0.30, "tri", release=0.2), t0 + beat * 0.5)
        for f in pad:
            mix(music, note(f, 2.0, 0.06, "sine", attack=0.3, release=0.4), t0)
        for step in range(8):
            f = pad[step % 3] * 2.0
            mix(music, note(f, 0.2, 0.07, "square", release=0.12), t0 + step * 0.25)
    music = music[:8 * RATE]
    write_wav(os.path.join(OUT, "sounds", "music.wav"), music)


def main():
    write_atlas()
    write_stars()
    write_sounds()


if __name__ == "__main__":
    main()
