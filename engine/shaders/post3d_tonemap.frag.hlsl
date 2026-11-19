// post3d_tonemap.frag.hlsl - HDR (premultiplied, linear) + bloom -> exposure -> tone map ->
// sRGB encode -> premultiplied 8-bit output for the 2D renderer.
#include "post3d_common.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_bloom : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_bloom_sampler : register(s1, space2);

cbuffer Tonemap : register(b0, space3)
{
    float4 u_params; // x exposure, y mode (0 ACES, 1 Reinhard, 2 linear), z bloom intensity (0 = off)
};

float3 aces(float3 x)
{
    // Narkowicz 2015 fit
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 tonemap(float3 c)
{
    c *= u_params.x;
    if (u_params.y < 0.5) {
        return aces(c);
    }
    if (u_params.y < 1.5) {
        return c / (1.0 + c);
    }
    return saturate(c);
}

float3 linear_to_srgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float4 main(PostVaryings input) : SV_Target0
{
    float4 hdr = u_input.Sample(u_input_sampler, input.uv);
    float3 glow = float3(0.0, 0.0, 0.0);
    if (u_params.z > 0.0) {
        glow = u_bloom.Sample(u_bloom_sampler, input.uv).rgb * u_params.z;
    }
    if (hdr.a >= 0.002) {
        // tone map the straight colour, then premultiply again; glow over it stays additive
        float3 straight = (hdr.rgb + glow) / hdr.a;
        return float4(linear_to_srgb(tonemap(straight)) * hdr.a, hdr.a);
    }
    // empty background: only glow, drawn additively (alpha 0)
    return float4(linear_to_srgb(tonemap(glow)), 0.0);
}
