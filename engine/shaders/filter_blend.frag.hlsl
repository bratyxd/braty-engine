// filter_blend.frag.hlsl - advanced blend modes (pixi.js/advanced-blend-modes, W3C formulas).
// Drawn with textured.vert.hlsl and blending off: t0 = the node (premultiplied), t1 = a copy of
// the target under it (same uv). Writes the finished composite.

[[vk::combinedImageSampler]] Texture2D<float4> u_source : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_source_sampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_backdrop : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_backdrop_sampler : register(s1, space2);

cbuffer Blend : register(b0, space3)
{
    float4 u_mode; // x = mode index (br_blend_mode - BR_BLEND_ADVANCED_FIRST)
};

// Must match textured.vert.hlsl's Output exactly.
struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float lum(float3 c)
{
    return dot(c, float3(0.3, 0.59, 0.11));
}

float3 clip_color(float3 c)
{
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) {
        c = l + (c - l) * l / (l - n);
    }
    if (x > 1.0) {
        c = l + (c - l) * (1.0 - l) / (x - l);
    }
    return c;
}

float3 set_lum(float3 c, float l)
{
    return clip_color(c + (l - lum(c)));
}

float sat(float3 c)
{
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 set_sat(float3 c, float s)
{
    float lo = min(c.r, min(c.g, c.b));
    float hi = max(c.r, max(c.g, c.b));
    if (hi <= lo) {
        return float3(0.0, 0.0, 0.0);
    }
    return (c - lo) * s / (hi - lo);
}

float color_burn(float b, float s)
{
    if (b >= 1.0) {
        return 1.0;
    }
    if (s <= 0.0) {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - b) / s);
}

float color_dodge(float b, float s)
{
    if (b <= 0.0) {
        return 0.0;
    }
    if (s >= 1.0) {
        return 1.0;
    }
    return min(1.0, b / (1.0 - s));
}

float hard_light(float b, float s)
{
    if (s <= 0.5) {
        return b * 2.0 * s;
    }
    float t = 2.0 * s - 1.0;
    return b + t - b * t;
}

float soft_light(float b, float s)
{
    if (s <= 0.5) {
        return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    }
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}

float linear_light(float b, float s)
{
    return saturate(b + 2.0 * s - 1.0);
}

float pin_light(float b, float s)
{
    return s < 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0);
}

float vivid_light(float b, float s)
{
    return s < 0.5 ? color_burn(b, 2.0 * s) : color_dodge(b, 2.0 * (s - 0.5));
}

// One channel of the separable modes.
float blend_channel(int mode, float b, float s)
{
    switch (mode) {
    case 1: return color_burn(b, s);
    case 2: return color_dodge(b, s);
    case 3: return min(b, s);
    case 4: return abs(b - s);
    case 5: return s > 0.0 ? min(1.0, b / s) : 1.0;
    case 6: return b + s - 2.0 * b * s;
    case 7: return hard_light(b, s);
    case 8: return b + s >= 1.0 ? 1.0 : 0.0;
    case 9: return max(b, s);
    case 10: return max(b + s - 1.0, 0.0);
    case 11: return min(b + s, 1.0);
    case 12: return linear_light(b, s);
    case 14: return 1.0 - abs(1.0 - b - s);
    case 15: return hard_light(s, b);
    case 16: return pin_light(b, s);
    case 18: return soft_light(b, s);
    case 19: return max(b - s, 0.0);
    case 20: return vivid_light(b, s);
    default: return s;
    }
}

float3 blend(int mode, float3 b, float3 s)
{
    if (mode == 0) {
        return set_lum(s, lum(b));                    // color
    }
    if (mode == 13) {
        return set_lum(b, lum(s));                    // luminosity
    }
    if (mode == 17) {
        return set_lum(set_sat(b, sat(s)), lum(b));   // saturation
    }
    return float3(blend_channel(mode, b.r, s.r), blend_channel(mode, b.g, s.g), blend_channel(mode, b.b, s.b));
}

float4 main(Input input) : SV_Target0
{
    float4 src = u_source.Sample(u_source_sampler, input.uv);
    float4 dst = u_backdrop.Sample(u_backdrop_sampler, input.uv);
    float3 cs = src.a > 0.0 ? src.rgb / src.a : float3(0.0, 0.0, 0.0);
    float3 cb = dst.a > 0.0 ? dst.rgb / dst.a : float3(0.0, 0.0, 0.0);
    float3 mixed = saturate(blend((int)u_mode.x, cb, cs));
    float3 rgb = src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a) + src.a * dst.a * mixed;
    float a = src.a + dst.a * (1.0 - src.a);
    return float4(rgb, a);
}
