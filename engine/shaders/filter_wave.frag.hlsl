// filter_wave.frag.hlsl - example custom filter (examples/filters_demo.c): a horizontal sine
// wave plus a colour shift, driven by its own uniforms after the fixed globals.
#include "filter_common.hlsli"

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_wave;  // x = amplitude (pixels), y = waves across the area, z = speed
    float4 u_tint;  // multiplied with the result (premultiplied)
};

float4 main(FilterVaryings input) : SV_Target0
{
    float phase = input.local.y * u_wave.y * 6.2831853 + u_params.x * u_wave.z;
    float2 uv = input.uv + float2(sin(phase) * u_wave.x * u_input_size.z, 0.0);
    return u_input.Sample(u_input_sampler, uv) * u_tint;
}
