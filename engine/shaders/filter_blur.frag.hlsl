// filter_blur.frag.hlsl - one direction of a separable Gaussian blur.
#include "filter_common.hlsli"

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_weights[8]; // .x = weight of the taps at +-i (0 = centre)
    float4 u_step;       // xy = uv offset between taps, z = taps per side
};

float4 main(FilterVaryings input) : SV_Target0
{
    float4 sum = u_input.Sample(u_input_sampler, input.uv) * u_weights[0].x;
    int taps = (int)u_step.z;
    for (int i = 1; i <= taps; i++) {
        float2 offset = u_step.xy * (float)i;
        sum += u_input.Sample(u_input_sampler, input.uv + offset) * u_weights[i].x;
        sum += u_input.Sample(u_input_sampler, input.uv - offset) * u_weights[i].x;
    }
    return sum;
}
