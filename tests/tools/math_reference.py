"""Reference values for tests/test_math.c (stdlib only). Run: python tests/tools/math_reference.py

PCG32 is O'Neill's pcg32 (XSH RR, 64-bit state) as in pcg32-demo; OKLab uses Bjorn Ottosson's
published matrices. The printed numbers are pasted into test_math.c.
"""
import colorsys
import math

MASK64 = (1 << 64) - 1


class Pcg32:
    def __init__(self, seed, stream):
        self.state = 0
        self.inc = ((stream << 1) | 1) & MASK64
        self.next()
        self.state = (self.state + seed) & MASK64
        self.next()

    def next(self):
        old = self.state
        self.state = (old * 6364136223846793005 + self.inc) & MASK64
        xorshifted = (((old >> 18) ^ old) >> 27) & 0xFFFFFFFF
        rot = old >> 59
        return ((xorshifted >> rot) | (xorshifted << ((-rot) & 31))) & 0xFFFFFFFF


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c):
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def linear_to_oklab(r, g, b):
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (math.copysign(abs(v) ** (1 / 3), v) for v in (l, m, s))
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def oklab_to_linear(L, a, b):
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)


if __name__ == "__main__":
    rng = Pcg32(42, 54)
    print("pcg32(42, 54):", " ".join("0x%08x" % rng.next() for _ in range(6)))
    print("srgb_to_linear(0.5) = %.7f" % srgb_to_linear(0.5))
    print("linear_to_srgb(0.5) = %.7f" % linear_to_srgb(0.5))
    for name, rgb in (("white", (1, 1, 1)), ("red", (1, 0, 0)), ("0x6495ed", (0x64 / 255, 0x95 / 255, 0xed / 255))):
        lin = [srgb_to_linear(c) for c in rgb]
        print("oklab(%s) = %.6f %.6f %.6f" % ((name,) + linear_to_oklab(*lin)))
    # perceptual midpoint of red and blue (straight sRGB in, straight sRGB out)
    a = linear_to_oklab(1, 0, 0)
    b = linear_to_oklab(0, 0, 1)
    mid = [(x + y) / 2 for x, y in zip(a, b)]
    print("lerp_oklab(red, blue, 0.5) = %.6f %.6f %.6f" % tuple(linear_to_srgb(c) for c in oklab_to_linear(*mid)))
    h, s, v = colorsys.rgb_to_hsv(0x64 / 255, 0x95 / 255, 0xed / 255)
    print("hsv(0x6495ed) = %.4f deg %.6f %.6f" % (h * 360, s, v))
