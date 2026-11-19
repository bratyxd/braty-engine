"""Writes the generated data of examples/scenes_demo.c: 48 tile PNGs, two backgrounds and
level_a.scene.json (one sprite per tile, so its loading screen has something to count).

Run: python examples/scenes/make_scenes_data.py   (stdlib only; the output is committed)
"""
import colorsys
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
COLUMNS, ROWS, TILE = 8, 6, 96


def png(width, height, pixel):
    rows = []
    for y in range(height):
        row = bytearray(b"\x00")
        for x in range(width):
            row += bytes(pixel(x, y))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def tile(index):
    r, g, b = colorsys.hsv_to_rgb(index / (COLUMNS * ROWS), 0.55, 0.9)
    base = (int(r * 255), int(g * 255), int(b * 255))

    def pixel(x, y):
        edge = x < 2 or y < 2 or x > 29 or y > 29
        stripe = (x + y + index) % 8 < 2
        k = 0.55 if edge else (1.1 if stripe else 1.0)
        return (min(255, int(base[0] * k)), min(255, int(base[1] * k)), min(255, int(base[2] * k)), 255)

    return png(32, 32, pixel)


def gradient(top, bottom):
    def pixel(x, y):
        t = y / 35.0
        return tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)

    return png(4, 36, pixel)


os.makedirs(os.path.join(HERE, "tiles"), exist_ok=True)
children = [{"type": "sprite", "label": "background", "texture": "scenes/bg_a.png", "width": 1280, "height": 720}]
for i in range(COLUMNS * ROWS):
    name = "scenes/tiles/tile_%02d.png" % i
    with open(os.path.join(HERE, "tiles", "tile_%02d.png" % i), "wb") as f:
        f.write(tile(i))
    x, y = i % COLUMNS, i // COLUMNS
    children.append({"type": "sprite", "texture": name, "width": TILE - 8, "height": TILE - 8,
                     "position": [256 + x * TILE, 96 + y * TILE]})
children.append({"type": "text", "label": "title", "text": "LEVEL A  -  48 textures, one per tile",
                 "position": [640, 52], "anchor": [0.5, 0.5],
                 "style": {"font_size": 30, "fill": "white", "stroke": {"width": 3, "color": "#1a2030"}}})
scene = {"type": "container", "label": "level_a", "children": children}
with open(os.path.join(HERE, "level_a.scene.json"), "w", newline="\n") as f:
    f.write(json.dumps(scene, indent=1) + "\n")
with open(os.path.join(HERE, "bg_a.png"), "wb") as f:
    f.write(gradient((32, 44, 70), (12, 16, 28)))
with open(os.path.join(HERE, "bg_b.png"), "wb") as f:
    f.write(gradient((70, 40, 60), (20, 12, 24)))
print("wrote", COLUMNS * ROWS, "tiles, 2 backgrounds and level_a.scene.json to", HERE)
