"""Writes three one-glyph distance-field fonts for examples/sdf_fonts_demo.c (stdlib only).

    python examples/fonts/gen_circle_sdf.py

The glyph "O" is a circle of radius RADIUS page pixels, so every field type must draw the same
disc. Outputs next to this script:
  circle_field.png  every channel (rgb and alpha) holds the distance: an MTSDF page whose alpha
                    is also a plain SDF
  circle_msdf.png   rgb holds the distance, alpha is 255: an MSDF page
  circle_sdf.fnt / circle_msdf.fnt / circle_mtsdf.fnt  AngelCode text files naming those pages
"""
import math
import os

from gen_pixel_font import write_png

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = 64
RADIUS = 24.0
RANGE = 8.0  # distanceRange: page pixels from value 0 to value 1


def field(alpha_is_distance):
    pixels = bytearray(PAGE * PAGE * 4)
    for y in range(PAGE):
        for x in range(PAGE):
            d = RADIUS - math.hypot(x + 0.5 - PAGE / 2, y + 0.5 - PAGE / 2)  # > 0 inside
            v = max(0, min(255, round((0.5 + d / RANGE) * 255)))
            p = (y * PAGE + x) * 4
            pixels[p:p + 4] = bytes((v, v, v, v if alpha_is_distance else 255))
    return pixels


def write_fnt(name, field_type, page):
    lines = [
        'info face="Circle" size=%d bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=1 aa=1 '
        'padding=0,0,0,0 spacing=0,0' % PAGE,
        "common lineHeight=%d base=%d scaleW=%d scaleH=%d pages=1 packed=0" % (PAGE, PAGE, PAGE, PAGE),
        'page id=0 file="%s"' % page,
        "distanceField fieldType=%s distanceRange=%d" % (field_type, RANGE),
        "chars count=1",
        "char id=79 x=0 y=0 width=%d height=%d xoffset=0 yoffset=0 xadvance=%d page=0 chnl=15" % (PAGE, PAGE, PAGE),
    ]
    with open(os.path.join(HERE, name), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    write_png(os.path.join(HERE, "circle_field.png"), PAGE, PAGE, field(True))
    write_png(os.path.join(HERE, "circle_msdf.png"), PAGE, PAGE, field(False))
    write_fnt("circle_sdf.fnt", "sdf", "circle_field.png")
    write_fnt("circle_msdf.fnt", "msdf", "circle_msdf.png")
    write_fnt("circle_mtsdf.fnt", "mtsdf", "circle_field.png")
    print("wrote circle_field.png, circle_msdf.png and circle_{sdf,msdf,mtsdf}.fnt")


if __name__ == "__main__":
    main()
