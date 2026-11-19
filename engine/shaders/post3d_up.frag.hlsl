// post3d_up.frag.hlsl - bloom upsample: 9-tap tent filter of the smaller level, added
// (pipeline blend ONE + ONE) onto the next larger level.
#include "post3d_common.hlsli"

cbuffer Up : register(b0, space3)
{
    float4 u_params; // xy = source texel size
};

float4 main(PostVaryings input) : SV_Target0
{
    float2 o = u_params.xy;
    float3 c = u_input.Sample(u_input_sampler, input.uv).rgb * 4.0;
    c += u_input.Sample(u_input_sampler, input.uv + float2(-o.x, 0.0)).rgb * 2.0;
    c += u_input.Sample(u_input_sampler, input.uv + float2(o.x, 0.0)).rgb * 2.0;
    c += u_input.Sample(u_input_sampler, input.uv + float2(0.0, -o.y)).rgb * 2.0;
    c += u_input.Sample(u_input_sampler, input.uv + float2(0.0, o.y)).rgb * 2.0;
    c += u_input.Sample(u_input_sampler, input.uv + float2(-o.x, -o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(o.x, -o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(-o.x, o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(o.x, o.y)).rgb;
    return float4(c / 16.0, 0.0);
}
