"""Writes the small PNGs used by tests/test_scene.c and tests/test_asset_group.c.

Run: python tests/data/scenes/make_scene_data.py   (stdlib only; the output is committed)
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def png(width, height, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * width for _ in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


COLORS = {
    "red": (220, 40, 40, 255),
    "green": (40, 200, 60, 255),
    "blue": (40, 80, 220, 255),
    "shared": (240, 200, 40, 255),
    "extra": (200, 40, 200, 255),
}

for name, color in COLORS.items():
    with open(os.path.join(HERE, name + ".png"), "wb") as f:
        f.write(png(8, 8, color))
print("wrote", len(COLORS), "PNGs to", HERE)
