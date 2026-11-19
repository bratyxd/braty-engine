/* br_color: hex, HSL, CSS-style parsing, premultiply. */
#include "braty/br_math.h"

#include <SDL3/SDL_stdinc.h>

br_color br_color_make(float r, float g, float b, float a)
{
    br_color c = { r, g, b, a };
    return c;
}

br_color br_color_from_hex(uint32_t rgb, float alpha)
{
    return br_color_make((float)((rgb >> 16) & 0xff) / 255.0f, (float)((rgb >> 8) & 0xff) / 255.0f,
                         (float)(rgb & 0xff) / 255.0f, alpha);
}

br_color br_color_from_hex_rgba(uint32_t rgba)
{
    return br_color_from_hex(rgba >> 8, (float)(rgba & 0xff) / 255.0f);
}

static uint32_t br_color_byte(float v)
{
    return (uint32_t)(br_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

uint32_t br_color_to_hex(br_color c)
{
    return (br_color_byte(c.r) << 16) | (br_color_byte(c.g) << 8) | br_color_byte(c.b);
}

uint32_t br_color_to_rgba8(br_color c)
{
    /* build the bytes explicitly so the memory order is R,G,B,A on any endianness */
    uint8_t bytes[4];
    bytes[0] = (uint8_t)br_color_byte(c.r);
    bytes[1] = (uint8_t)br_color_byte(c.g);
    bytes[2] = (uint8_t)br_color_byte(c.b);
    bytes[3] = (uint8_t)br_color_byte(c.a);
    uint32_t packed;
    SDL_memcpy(&packed, bytes, 4);
    return packed;
}

static float br_hue_to_rgb(float p, float q, float t)
{
    if (t < 0.0f) {
        t += 1.0f;
    }
    if (t > 1.0f) {
        t -= 1.0f;
    }
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

br_color br_color_from_hsl(float h, float s, float l, float alpha)
{
    if (s <= 0.0f) {
        return br_color_make(l, l, l, alpha);
    }
    float hue = SDL_fmodf(h, 360.0f);
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    hue /= 360.0f;
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    return br_color_make(br_hue_to_rgb(p, q, hue + 1.0f / 3.0f), br_hue_to_rgb(p, q, hue),
                         br_hue_to_rgb(p, q, hue - 1.0f / 3.0f), alpha);
}

void br_color_to_hsl(br_color c, float *h, float *s, float *l)
{
    float max = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    float min = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    *l = (max + min) * 0.5f;
    if (max == min) {
        *h = 0.0f;
        *s = 0.0f;
        return;
    }
    float d = max - min;
    *s = *l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
    float hue;
    if (max == c.r) {
        hue = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
    } else if (max == c.g) {
        hue = (c.b - c.r) / d + 2.0f;
    } else {
        hue = (c.r - c.g) / d + 4.0f;
    }
    *h = hue * 60.0f;
}

br_color br_color_premultiply(br_color c)
{
    return br_color_make(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
}

br_color br_color_multiply(br_color a, br_color b)
{
    return br_color_make(a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a);
}

br_color br_color_lerp(br_color a, br_color b, float t)
{
    return br_color_make(br_lerpf(a.r, b.r, t), br_lerpf(a.g, b.g, t), br_lerpf(a.b, b.b, t),
                         br_lerpf(a.a, b.a, t));
}

/* ---- parsing */

typedef struct br_named_color {
    const char *name;
    uint32_t    rgb;
} br_named_color;

/* The 148 CSS Color Module Level 4 names. */
static const br_named_color br_css_colors[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff },
    { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff }, { "beige", 0xf5f5dc },
    { "bisque", 0xffe4c4 }, { "black", 0x000000 }, { "blanchedalmond", 0xffebcd },
    { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
    { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 },
    { "chocolate", 0xd2691e }, { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed },
    { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c }, { "cyan", 0x00ffff },
    { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
    { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 },
    { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f },
    { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 },
    { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b },
    { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 },
    { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1e90ff },
    { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 },
    { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff },
    { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 },
    { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c },
    { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c },
    { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 },
    { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 },
    { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
    { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 },
    { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa },
    { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xb0c4de },
    { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
    { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
    { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee },
    { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 },
    { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 },
    { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 },
    { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 },
    { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee },
    { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 },
    { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd },
    { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
    { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 },
    { "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 },
    { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee }, { "sienna", 0xa0522d },
    { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa },
    { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
    { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 },
    { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee }, { "wheat", 0xf5deb3 },
    { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 },
    { "yellowgreen", 0x9acd32 },
};

static int br_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* Parses `count` hex digits into *value. */
static bool br_parse_hex_digits(const char *s, size_t count, uint32_t *value)
{
    uint32_t v = 0;
    for (size_t i = 0; i < count; i++) {
        int d = br_hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *value = v;
    return true;
}

static bool br_color_parse_hex(const char *hex, br_color *out)
{
    size_t len = SDL_strlen(hex);
    uint32_t v;
    if (!br_parse_hex_digits(hex, len, &v)) {
        return false;
    }
    if (len == 3 || len == 4) {
        /* short form: each digit is doubled, "f80" = "ff8800" */
        float scale = 1.0f / 15.0f;
        uint32_t shift = len == 4 ? 4u : 0u;
        float a = len == 4 ? (float)(v & 0xf) * scale : 1.0f;
        *out = br_color_make((float)((v >> (8 + shift)) & 0xf) * scale,
                             (float)((v >> (4 + shift)) & 0xf) * scale,
                             (float)((v >> shift) & 0xf) * scale, a);
        return true;
    }
    if (len == 6) {
        *out = br_color_from_hex(v, 1.0f);
        return true;
    }
    if (len == 8) {
        *out = br_color_from_hex_rgba(v);
        return true;
    }
    return false;
}

/* Parses "r, g, b[, a)" after "rgb(" or "rgba(": r,g,b in 0..255, a in 0..1. */
static bool br_color_parse_rgb_func(const char *args, br_color *out)
{
    float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const char *p = args;
    int n = 0;
    while (n < 4) {
        char *end = NULL;
        double value = SDL_strtod(p, &end);
        if (end == p) {
            break;
        }
        v[n] = (float)value;
        n++;
        p = end;
        while (*p == ' ' || *p == ',') {
            p++;
        }
    }
    if (n < 3 || *p != ')') {
        return false;
    }
    *out = br_color_make(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3]);
    return true;
}

bool br_color_parse(const char *text, br_color *out)
{
    if (text == NULL) {
        return false;
    }
    while (*text == ' ') {
        text++;
    }
    if (text[0] == '#') {
        return br_color_parse_hex(text + 1, out);
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return br_color_parse_hex(text + 2, out);
    }
    if (SDL_strncasecmp(text, "rgba(", 5) == 0) {
        return br_color_parse_rgb_func(text + 5, out);
    }
    if (SDL_strncasecmp(text, "rgb(", 4) == 0) {
        return br_color_parse_rgb_func(text + 4, out);
    }
    if (SDL_strcasecmp(text, "transparent") == 0) {
        *out = br_color_make(0.0f, 0.0f, 0.0f, 0.0f);
        return true;
    }
    for (size_t i = 0; i < sizeof(br_css_colors) / sizeof(br_css_colors[0]); i++) {
        if (SDL_strcasecmp(text, br_css_colors[i].name) == 0) {
            *out = br_color_from_hex(br_css_colors[i].rgb, 1.0f);
            return true;
        }
    }
    return false;
}
