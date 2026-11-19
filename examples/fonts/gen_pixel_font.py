"""Writes the hand-made 5x7 pixel bitmap font used by examples/text_demo.c (stdlib only).

    python examples/fonts/gen_pixel_font.py

Outputs next to this script: pixel5x7.png (one white page), pixel5x7.fnt (AngelCode text
format) and pixel5x7.xml (the same font in AngelCode XML format).
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 2           # every font pixel becomes 2x2 page pixels
CELL_W, CELL_H = 5, 7
PAGE_W, PAGE_H = 256, 64

GLYPHS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "D": ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "J": ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "10001", "11001", "10101", "10011", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    "!": ["00100", "00100", "00100", "00100", "00100", "00000", "00100"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ":": ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "?": ["01110", "10001", "00001", "00010", "00100", "00000", "00100"],
}


def write_png(path, width, height, rgba):
    rows = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    pixels = bytearray(PAGE_W * PAGE_H * 4)
    gw, gh = CELL_W * SCALE, CELL_H * SCALE
    chars = []
    x, y = 1, 1
    for ch in sorted(GLYPHS):
        if x + gw + 1 > PAGE_W:
            x, y = 1, y + gh + 2
        for row, bits in enumerate(GLYPHS[ch]):
            for col, bit in enumerate(bits):
                if bit != "1":
                    continue
                for sy in range(SCALE):
                    for sx in range(SCALE):
                        p = ((y + row * SCALE + sy) * PAGE_W + x + col * SCALE + sx) * 4
                        pixels[p:p + 4] = b"\xff\xff\xff\xff"
        chars.append((ord(ch), x, y))
        x += gw + 2
    write_png(os.path.join(HERE, "pixel5x7.png"), PAGE_W, PAGE_H, pixels)

    advance = (CELL_W + 1) * SCALE
    line_height = (CELL_H + 3) * SCALE
    base = (CELL_H + 1) * SCALE
    entries = [(32, 0, 0, 0, 0)] + [(cid, cx, cy, gw, gh) for cid, cx, cy in chars]
    # a few kerning pairs so the demo shows them: tighten around "1" and "A"/"V"
    kernings = [(ord("1"), ord("1"), -SCALE), (ord("A"), ord("V"), -SCALE), (ord("V"), ord("A"), -SCALE)]

    lines = [
        'info face="Pixel5x7" size=%d bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=0 aa=1 '
        'padding=0,0,0,0 spacing=2,2' % line_height,
        "common lineHeight=%d base=%d scaleW=%d scaleH=%d pages=1 packed=0" % (line_height, base, PAGE_W, PAGE_H),
        'page id=0 file="pixel5x7.png"',
        "chars count=%d" % len(entries),
    ]
    for cid, cx, cy, w, h in entries:
        lines.append("char id=%d x=%d y=%d width=%d height=%d xoffset=0 yoffset=%d xadvance=%d page=0 chnl=15"
                     % (cid, cx, cy, w, h, SCALE, advance))
    lines.append("kernings count=%d" % len(kernings))
    for a, b, amount in kernings:
        lines.append("kerning first=%d second=%d amount=%d" % (a, b, amount))
    with open(os.path.join(HERE, "pixel5x7.fnt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    xml = ['<?xml version="1.0"?>', "<font>",
           '  <info face="Pixel5x7" size="%d" bold="0" italic="0"/>' % line_height,
           '  <common lineHeight="%d" base="%d" scaleW="%d" scaleH="%d" pages="1" packed="0"/>'
           % (line_height, base, PAGE_W, PAGE_H),
           "  <pages>", '    <page id="0" file="pixel5x7.png"/>', "  </pages>",
           '  <chars count="%d">' % len(entries)]
    for cid, cx, cy, w, h in entries:
        xml.append('    <char id="%d" x="%d" y="%d" width="%d" height="%d" xoffset="0" yoffset="%d" xadvance="%d" '
                   'page="0" chnl="15"/>' % (cid, cx, cy, w, h, SCALE, advance))
    xml.append("  </chars>")
    xml.append('  <kernings count="%d">' % len(kernings))
    for a, b, amount in kernings:
        xml.append('    <kerning first="%d" second="%d" amount="%d"/>' % (a, b, amount))
    xml += ["  </kernings>", "</font>"]
    with open(os.path.join(HERE, "pixel5x7.xml"), "w", newline="\n") as f:
        f.write("\n".join(xml) + "\n")
    print("wrote pixel5x7.png/.fnt/.xml (%d glyphs)" % len(entries))


if __name__ == "__main__":
    main()
