// text_sdf.frag.hlsl - distance-field glyphs (SDF and MSDF) times the vertex colour.
// Used with textured.vert.hlsl. Fragment sampler slot 0: the glyph atlas / font page.
// Uniform slot 0 (fragment, space3):
//   u_field.x = texel value on the outline, u_field.y = glyph pixels per unit of texel value,
//   u_field.z = offset in glyph pixels (grows the shape: strokes, bold, shadows),
//   u_field.w = softness in glyph pixels (0 = sharp, > 0 = blurred edge for shadows)
//   u_mode.x  = 1 for multi-channel (median of rgb), 0 for single channel (alpha)

[[vk::combinedImageSampler]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 u_field;
    float4 u_mode;
};

// Must match textured.vert.hlsl's Output exactly (SV_Position included).
struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target0
{
    float4 texel = u_texture.Sample(u_sampler, input.uv);
    float value = texel.a;
    if (u_mode.x > 0.5) {
        value = max(min(texel.r, texel.g), min(max(texel.r, texel.g), texel.b));
    }
    float dist = (value - u_field.x) * u_field.y + u_field.z;   // glyph pixels, > 0 inside
    float width = max(fwidth(dist), max(u_field.w, 0.0001));    // one screen pixel, or the blur
    float alpha = saturate(dist / width + 0.5);
    return input.color * alpha;
}
