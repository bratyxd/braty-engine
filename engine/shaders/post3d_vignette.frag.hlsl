// post3d_vignette.frag.hlsl - example custom 3D post pass (used by examples/view3d_demo.c):
// darkens the corners of the HDR image.
#include "post3d_common.hlsli"

cbuffer Vignette : register(b0, space3)
{
    float4 u_vignette; // x = strength, y = radius where darkening starts
};

float4 main(PostVaryings input) : SV_Target0
{
    float4 c = u_input.Sample(u_input_sampler, input.uv);
    float d = length(input.uv - 0.5) * 1.4142;
    float k = 1.0 - u_vignette.x * smoothstep(u_vignette.y, 1.0, d);
    return float4(c.rgb * k, c.a);
}
