"""Generates the example assets of examples/assets_demo.c (stdlib only, deterministic).

    python examples/assets/generate_assets.py

Writes into this directory:
  atlas.png + atlas.json   TexturePacker-style spritesheet (hash format): plain frames, a trimmed
                           frame next to its untrimmed twin, a frame stored rotated 90 degrees
                           clockwise next to its unrotated twin, a 4-frame "walk" animation and
                           a 9-slice panel with borders
  checker.dds / .png       128x128 BC1 (legacy DXT1 header) with a full mip chain, and a PNG twin
                           (the manifest picks one through the format preference "dds,png")
  gradient_bc7.dds         128x128 BC7 (mode 6) behind a DX10 header
  badge_bc1.ktx            64x64 BC1 in a KTX 1 container with mips
  config.json              plain JSON (not a spritesheet)
  tiles/tile_NN.png        small textures that give the loading bar something to count
  manifest.json            one bundle "game" listing all of the above plus a missing file
"""
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


class Image:
    def __init__(self, w, h, rgba=(0, 0, 0, 0)):
        self.w = w
        self.h = h
        self.px = [list(rgba) for _ in range(w * h)]

    def get(self, x, y):
        return self.px[y * self.w + x]

    def set(self, x, y, rgba):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = list(rgba)

    def fill(self, x0, y0, x1, y1, rgba):
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.set(x, y, rgba)

    def shape(self, inside, rgba):
        """Fills every pixel whose centre satisfies inside(x, y)."""
        for y in range(self.h):
            for x in range(self.w):
                if inside(x + 0.5, y + 0.5):
                    self.set(x, y, rgba)

    def blit(self, src, dx, dy):
        for y in range(src.h):
            for x in range(src.w):
                self.set(dx + x, dy + y, src.get(x, y))

    def crop(self, x0, y0, x1, y1):
        out = Image(x1 - x0, y1 - y0)
        for y in range(y0, y1):
            for x in range(x0, x1):
                out.set(x - x0, y - y0, self.get(x, y))
        return out

    def rotated_cw(self):
        """90 degrees clockwise: (x, y) -> (h - 1 - y, x), as TexturePacker stores rotated frames."""
        out = Image(self.h, self.w)
        for y in range(self.h):
            for x in range(self.w):
                out.set(self.h - 1 - y, x, self.get(x, y))
        return out

    def alpha_bounds(self):
        xs = [i % self.w for i, p in enumerate(self.px) if p[3] > 0]
        ys = [i // self.w for i, p in enumerate(self.px) if p[3] > 0]
        return min(xs), min(ys), max(xs) + 1, max(ys) + 1

    def half(self):
        """Box-filtered half-size copy (for mip chains)."""
        w = max(1, self.w // 2)
        h = max(1, self.h // 2)
        out = Image(w, h)
        for y in range(h):
            for x in range(w):
                acc = [0, 0, 0, 0]
                for sy in (0, 1):
                    for sx in (0, 1):
                        p = self.get(min(self.w - 1, x * 2 + sx), min(self.h - 1, y * 2 + sy))
                        for c in range(4):
                            acc[c] += p[c]
                out.set(x, y, [v // 4 for v in acc])
        return out


def write_png(path, img):
    raw = bytearray()
    for y in range(img.h):
        raw.append(0)
        for x in range(img.w):
            raw.extend(bytes(img.get(x, y)))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def point_in_polygon(pts, x, y):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


# ---- block compression

def to565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def from565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]


def block_pixels(img, bx, by):
    return [img.get(min(img.w - 1, bx + x), min(img.h - 1, by + y)) for y in range(4) for x in range(4)]


def bc1_block(pixels):
    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in pixels]
    c0 = to565(pixels[lum.index(max(lum))])
    c1 = to565(pixels[lum.index(min(lum))])
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<HHI", c0, c1, 0)
    e0 = from565(c0)
    e1 = from565(c1)
    palette = [e0, e1,
               [(2 * a + b) // 3 for a, b in zip(e0, e1)],
               [(a + 2 * b) // 3 for a, b in zip(e0, e1)]]
    bits = 0
    for i, p in enumerate(pixels):
        dist = [sum((p[c] - q[c]) ** 2 for c in range(3)) for q in palette]
        bits |= dist.index(min(dist)) << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


BC7_WEIGHTS = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]


def bc7_mode6_block(pixels):
    """BC7 mode 6: one subset, RGBA 7-bit endpoints + p-bit, 4-bit indices."""
    lo = [min(p[c] for p in pixels) for c in range(4)]
    hi = [max(p[c] for p in pixels) for c in range(4)]
    q0 = [v >> 1 for v in lo]                       # endpoint 0 with p-bit 0
    q1 = [max(0, (v - 1)) >> 1 for v in hi]         # endpoint 1 with p-bit 1
    p0, p1 = 0, 1
    e0 = [(v << 1) | p0 for v in q0]
    e1 = [(v << 1) | p1 for v in q1]
    palette = [[((64 - w) * a + w * b + 32) >> 6 for a, b in zip(e0, e1)] for w in BC7_WEIGHTS]
    indices = []
    for p in pixels:
        dist = [sum((p[c] - q[c]) ** 2 for c in range(4)) for q in palette]
        indices.append(dist.index(min(dist)))
    if indices[0] >= 8:  # the anchor index stores 3 bits: its top bit must be 0
        q0, q1 = q1, q0
        p0, p1 = p1, p0
        indices = [15 - i for i in indices]
    value = 1 << 6  # mode 6
    pos = 7
    for c in range(4):
        value |= q0[c] << pos
        pos += 7
        value |= q1[c] << pos
        pos += 7
    value |= p0 << pos
    pos += 1
    value |= p1 << pos
    pos += 1
    for i, idx in enumerate(indices):
        value |= idx << pos
        pos += 3 if i == 0 else 4
    assert pos == 128
    return value.to_bytes(16, "little")


def compress(img, block_fn):
    out = bytearray()
    for by in range(0, max(4, img.h), 4):
        for bx in range(0, max(4, img.w), 4):
            out += block_fn(block_pixels(img, bx, by))
    return bytes(out)


def mip_chain(img):
    levels = [img]
    while levels[-1].w > 1 or levels[-1].h > 1:
        levels.append(levels[-1].half())
    return levels


def write_dds_bc1(path, img):
    levels = mip_chain(img)
    data = [compress(level, bc1_block) for level in levels]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, img.h, img.w, len(data[0]), 0, len(levels))
    header += b"\0" * 44
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    assert len(header) == 128
    with open(path, "wb") as f:
        f.write(header + b"".join(data))


def write_dds_bc7(path, img):
    data = compress(img, bc7_mode6_block)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, img.h, img.w, len(data), 0, 1)
    header += b"\0" * 44
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 98, 3, 0, 1, 0)  # DXGI_FORMAT_BC7_UNORM, TEXTURE2D, array size 1
    assert len(header) == 148
    with open(path, "wb") as f:
        f.write(header + data)


def write_ktx_bc1(path, img):
    levels = mip_chain(img)
    ident = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    header = ident + struct.pack("<13I", 0x04030201, 0, 1, 0, 0x83F1, 0x1908, img.w, img.h, 0, 0, 1,
                                 len(levels), 0)
    body = bytearray()
    for level in levels:
        data = compress(level, bc1_block)
        body += struct.pack("<I", len(data)) + data  # BC1 sizes are multiples of 4: no padding
    with open(path, "wb") as f:
        f.write(header + bytes(body))


# ---- the spritesheet

def make_star():
    img = Image(64, 64)
    pts = []
    for i in range(10):
        r = 26 if i % 2 == 0 else 11
        a = -math.pi / 2 + i * math.pi / 5
        pts.append((36 + r * math.cos(a), 38 + r * math.sin(a)))  # off-centre: trimming moves it
    img.shape(lambda x, y: point_in_polygon(pts, x, y), (255, 210, 40, 255))
    img.shape(lambda x, y: (x - 36) ** 2 + (y - 38) ** 2 < 25, (255, 120, 20, 255))
    return img


def make_arrow():
    img = Image(64, 32)
    img.fill(0, 0, 64, 32, (40, 60, 90, 255))
    img.fill(4, 12, 40, 20, (90, 220, 120, 255))
    pts = [(38, 4), (60, 16), (38, 28)]
    img.shape(lambda x, y: point_in_polygon(pts, x, y), (90, 220, 120, 255))
    img.fill(0, 0, 8, 8, (240, 70, 70, 255))  # red corner marks the top-left
    return img


def make_walk(i):
    img = Image(32, 32)
    heights = [22, 14, 8, 14]
    colors = [(240, 90, 90, 255), (240, 170, 60, 255), (110, 200, 240, 255), (180, 120, 240, 255)]
    cy = heights[i]
    img.shape(lambda x, y: (x - 16) ** 2 + (y - cy) ** 2 < 49, colors[i])
    img.fill(4, 29, 28, 31, (200, 200, 200, 255))
    return img


def make_gem():
    img = Image(32, 32)
    pts = [(16, 2), (30, 14), (16, 30), (2, 14)]
    img.shape(lambda x, y: point_in_polygon(pts, x, y), (60, 200, 220, 255))
    img.shape(lambda x, y: point_in_polygon([(16, 2), (30, 14), (16, 14)], x, y), (170, 240, 250, 255))
    return img


def make_panel():
    img = Image(48, 48)
    img.fill(0, 0, 48, 48, (230, 230, 240, 255))
    img.fill(4, 4, 44, 44, (70, 80, 140, 255))
    return img


def build_atlas():
    atlas = Image(256, 128)
    frames = {}

    def place(name, img, x, y, rotated=False, trim=None, source=None, pivot=None, borders=None):
        stored = img.rotated_cw() if rotated else img
        atlas.blit(stored, x, y)
        entry = {
            "frame": {"x": x, "y": y, "w": img.w, "h": img.h},
            "rotated": rotated,
            "trimmed": trim is not None,
            "spriteSourceSize": {"x": trim[0], "y": trim[1], "w": img.w, "h": img.h} if trim
            else {"x": 0, "y": 0, "w": img.w, "h": img.h},
            "sourceSize": {"w": source[0], "h": source[1]} if source else {"w": img.w, "h": img.h},
        }
        if pivot:
            entry["pivot"] = {"x": pivot[0], "y": pivot[1]}
        if borders:
            entry["borders"] = dict(zip(("left", "top", "right", "bottom"), borders))
        frames[name] = entry

    star = make_star()
    place("star_full.png", star, 0, 0)
    x0, y0, x1, y1 = star.alpha_bounds()
    place("star_trimmed.png", star.crop(x0, y0, x1, y1), 64, 0, trim=(x0, y0), source=(64, 64))
    arrow = make_arrow()
    place("arrow_full.png", arrow, 0, 64)
    place("arrow_rotated.png", arrow, 64, 64, rotated=True)  # occupies 32x64 at (64, 64)
    for i in range(4):
        place("walk_%d.png" % i, make_walk(i), 128 + 32 * i, 0)
    place("gem.png", make_gem(), 128, 32, pivot=(0.5, 0.5))
    place("panel.png", make_panel(), 160, 32, borders=(12, 12, 12, 12))
    sheet = {
        "frames": frames,
        "animations": {"walk": ["walk_0.png", "walk_1.png", "walk_2.png", "walk_3.png"]},
        "meta": {"app": "examples/assets/generate_assets.py", "image": "atlas.png", "format": "RGBA8888",
                 "size": {"w": atlas.w, "h": atlas.h}, "scale": "1"},
    }
    write_png(os.path.join(HERE, "atlas.png"), atlas)
    with open(os.path.join(HERE, "atlas.json"), "w", newline="\n") as f:
        json.dump(sheet, f, indent=1)
        f.write("\n")


def make_checker():
    img = Image(128, 128)
    for y in range(128):
        for x in range(128):
            on = ((x // 16) + (y // 16)) % 2 == 0
            quad = (x // 64) + 2 * (y // 64)
            base = [(230, 80, 80), (80, 200, 90), (80, 120, 230), (230, 200, 70)][quad]
            img.set(x, y, list(base) + [255] if on else [30, 30, 40, 255])
    return img


def make_gradient():
    img = Image(128, 128)
    for y in range(128):
        for x in range(128):
            img.set(x, y, [x * 2, y * 2, 255 - x, 255])
    img.shape(lambda x, y: (x - 64) ** 2 + (y - 64) ** 2 < 30 ** 2, (250, 250, 250, 255))
    img.shape(lambda x, y: (x - 64) ** 2 + (y - 64) ** 2 < 22 ** 2, (30, 30, 30, 255))
    return img


def make_badge():
    img = Image(64, 64)
    img.fill(0, 0, 64, 64, (240, 140, 40, 255))
    img.shape(lambda x, y: (x - 32) ** 2 + (y - 32) ** 2 < 24 ** 2, (40, 80, 200, 255))
    img.fill(28, 12, 36, 52, (250, 250, 250, 255))
    return img


def make_tile(i):
    img = Image(64, 64)
    for y in range(64):
        for x in range(64):
            img.set(x, y, [(x * 4 + i * 20) % 256, (y * 4) % 256, (i * 37) % 256, 255])
    return img


def main():
    build_atlas()
    checker = make_checker()
    write_dds_bc1(os.path.join(HERE, "checker.dds"), checker)
    write_png(os.path.join(HERE, "checker.png"), checker)
    write_dds_bc7(os.path.join(HERE, "gradient_bc7.dds"), make_gradient())
    write_ktx_bc1(os.path.join(HERE, "badge_bc1.ktx"), make_badge())
    os.makedirs(os.path.join(HERE, "tiles"), exist_ok=True)
    tile_count = 12
    for i in range(tile_count):
        write_png(os.path.join(HERE, "tiles", "tile_%02d.png" % i), make_tile(i))
    with open(os.path.join(HERE, "config.json"), "w", newline="\n") as f:
        json.dump({"title": "assets demo", "background": [0.08, 0.09, 0.12]}, f, indent=1)
        f.write("\n")
    assets = [
        {"alias": "atlas", "src": "atlas.json"},
        {"alias": "checker", "src": "checker.{png,dds}"},
        {"alias": "gradient", "src": "gradient_bc7.dds"},
        {"alias": "badge", "src": "badge_bc1.ktx"},
        {"alias": "config", "src": "config.json"},
        {"alias": "missing", "src": "does_not_exist.png"},
    ]
    for i in range(tile_count):
        assets.append({"alias": "tile%d" % i, "src": "tiles/tile_%02d.png" % i})
    with open(os.path.join(HERE, "manifest.json"), "w", newline="\n") as f:
        json.dump({"bundles": [{"name": "game", "assets": assets}]}, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
