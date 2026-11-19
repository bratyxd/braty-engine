// filter_noise.frag.hlsl - adds grain (Pixi NoiseFilter).
#include "filter_common.hlsli"

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_noise; // x = amount, y = seed
};

float random(float2 co)
{
    return frac(sin(dot(co, float2(12.9898, 78.233))) * 43758.5453);
}

float4 main(FilterVaryings input) : SV_Target0
{
    float4 color = u_input.Sample(u_input_sampler, input.uv);
    float diff = (random(input.local * 512.0 + u_noise.y) - 0.5) * u_noise.x;
    // premultiplied: adding diff * a equals un-premultiply, add, premultiply
    color.rgb = saturate(color.rgb + diff * color.a);
    color.rgb = min(color.rgb, color.a);
    return color;
}
