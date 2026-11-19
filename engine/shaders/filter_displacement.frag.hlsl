// filter_displacement.frag.hlsl - moves pixels by a map's red/green (Pixi DisplacementFilter).
#include "filter_common.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_map : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_map_sampler : register(s1, space2);

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_scale;      // xy = displacement in input uv per unit of (map - 0.5); zw = map repeats per area
    float4 u_map_offset; // xy = map uv offset
};

float4 main(FilterVaryings input) : SV_Target0
{
    float2 map_uv = input.local * u_scale.zw + u_map_offset.xy;
    float4 map = u_map.Sample(u_map_sampler, map_uv);
    float2 offset = (map.rg - 0.5) * u_scale.xy;
    return u_input.Sample(u_input_sampler, input.uv + offset);
}
