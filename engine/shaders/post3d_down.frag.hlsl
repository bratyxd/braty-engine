// post3d_down.frag.hlsl - bloom downsample: 4 bilinear taps (a 4x4 texel box) of the source.
// The first level also applies the bright-pass threshold (soft knee).
#include "post3d_common.hlsli"

cbuffer Down : register(b0, space3)
{
    float4 u_params; // xy = source texel size, z = threshold, w = 1 to apply it
};

float4 main(PostVaryings input) : SV_Target0
{
    float2 o = u_params.xy;
    float3 c = u_input.Sample(u_input_sampler, input.uv + float2(-o.x, -o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(o.x, -o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(-o.x, o.y)).rgb;
    c += u_input.Sample(u_input_sampler, input.uv + float2(o.x, o.y)).rgb;
    c *= 0.25;
    if (u_params.w > 0.5) {
        float brightness = max(c.r, max(c.g, c.b));
        float knee = u_params.z * 0.5;
        float soft = clamp(brightness - u_params.z + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        float contribution = max(soft, brightness - u_params.z) / max(brightness, 1e-4);
        c *= contribution;
    }
    return float4(c, 1.0);
}
