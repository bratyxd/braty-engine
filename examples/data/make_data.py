"""Generates the art of examples/data_demo.c (stdlib only, deterministic).

    python examples/data/make_data.py

Writes into this directory:
  atlas.png + atlas.json   TexturePacker hash spritesheet: hero, gem, spark, a 4-frame "walk"
                           animation and a 9-slice "panel" (borders 8)
  stripes.png              a 32x32 tile for the tiling sprite, the mesh and the displacement map
The scene files and game.json next to it are written by hand.
"""
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


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


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [0] * (w * h * 4)

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[(y * self.w + x) * 4:(y * self.w + x) * 4 + 4] = list(c)

    def rect(self, x0, y0, w, h, c):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                self.set(x, y, c)

    def disc(self, cx, cy, r, c, soft=False):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if d <= r:
                    a = c[3]
                    if soft:
                        a = int(c[3] * (1.0 - d / r) ** 1.5)
                    self.set(x, y, (c[0], c[1], c[2], a))


def main():
    atlas = Canvas(160, 64)  # frames sit 4 px apart so linear filtering does not bleed
    frames = {}

    def frame(name, x, y, w, h, extra=None):
        entry = {
            "frame": {"x": x, "y": y, "w": w, "h": h},
            "rotated": False,
            "trimmed": False,
            "spriteSourceSize": {"x": 0, "y": 0, "w": w, "h": h},
            "sourceSize": {"w": w, "h": h},
        }
        if extra:
            entry.update(extra)
        frames[name] = entry

    # hero: a round face, 32x32 at (0, 0)
    atlas.disc(16, 16, 15, (255, 200, 60, 255))
    atlas.disc(11, 12, 3, (40, 30, 20, 255))
    atlas.disc(21, 12, 3, (40, 30, 20, 255))
    for x in range(9, 24):
        atlas.set(x, 22 + int(2 * math.sin((x - 9) / 14 * math.pi)), (120, 40, 20, 255))
    frame("hero.png", 0, 0, 32, 32, {"anchor": {"x": 0.5, "y": 0.5}})

    # gem: a diamond, 24x24 at (36, 0)
    for y in range(24):
        half = 12 - abs(y - 12)
        for x in range(12 - half, 12 + half):
            shade = 160 + (x * 4) % 90
            atlas.set(36 + x, y, (60, shade, 255, 255))
    frame("gem.png", 36, 0, 24, 24)

    # spark: a soft dot, 16x16 at (64, 0)
    atlas.disc(64 + 8, 8, 8, (255, 255, 255, 255), soft=True)
    frame("spark.png", 64, 0, 16, 16)

    # panel: 32x32 at (84, 0), an 8 px border for 9-slice scaling
    atlas.rect(84, 0, 32, 32, (30, 60, 110, 255))
    atlas.rect(84 + 3, 3, 26, 26, (70, 120, 190, 255))
    atlas.rect(84 + 8, 8, 16, 16, (230, 240, 255, 255))
    frame("panel.png", 84, 0, 32, 32, {"borders": {"left": 8, "top": 8, "right": 8, "bottom": 8}})

    # walk: 4 frames of 16x16 at y = 36, 20 px apart, a dot circling inside a box
    for i in range(4):
        x0 = i * 20
        atlas.rect(x0, 36, 16, 16, (90, 200, 120, 255))
        angle = i * math.pi / 2
        atlas.disc(x0 + 8 + 4 * math.cos(angle), 36 + 8 + 4 * math.sin(angle), 3, (20, 60, 30, 255))
        frame("walk_%d.png" % i, x0, 36, 16, 16)

    write_png(os.path.join(HERE, "atlas.png"), atlas.w, atlas.h, atlas.px)
    sheet = {
        "frames": frames,
        "animations": {"walk": ["walk_0.png", "walk_1.png", "walk_2.png", "walk_3.png"]},
        "meta": {"app": "examples/data/make_data.py", "image": "atlas.png", "format": "RGBA8888",
                 "size": {"w": atlas.w, "h": atlas.h}, "scale": "1"},
    }
    with open(os.path.join(HERE, "atlas.json"), "w") as f:
        json.dump(sheet, f, indent=1)
        f.write("\n")

    stripes = Canvas(32, 32)
    for y in range(32):
        for x in range(32):
            v = 128 + int(127 * math.sin((x + y) / 32 * 2 * math.pi))
            stripes.set(x, y, (v, 255 - v, 200, 255))
    write_png(os.path.join(HERE, "stripes.png"), 32, 32, stripes.px)


if __name__ == "__main__":
    main()
