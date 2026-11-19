// shadow3d.frag.hlsl - writes depth into the R32F shadow atlas (the depth buffer does the test).
// Alpha-masked materials discard like in the main pass: t0 = base colour, b0 = mask settings.
#include "mesh3d_varyings.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_base_tex : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_base_smp : register(s0, space2);

cbuffer Mask : register(b0, space3)
{
    float4 u_mask;     // x = alpha cutoff (0 = no test), y = base alpha, z = textured, w = uv set
    float4 u_uv[2];    // base colour uv transform rows
};

float4 main(Mesh3dVaryings input) : SV_Target0
{
    if (u_mask.x > 0.0) {
        float alpha = u_mask.y * input.color.a;
        if (u_mask.z > 0.5) {
            float2 base = u_mask.w > 0.5 ? input.uv.zw : input.uv.xy;
            float2 uv = float2(dot(u_uv[0].xyz, float3(base, 1.0)), dot(u_uv[1].xyz, float3(base, 1.0)));
            alpha *= u_base_tex.Sample(u_base_smp, uv).a;
        }
        if (alpha < u_mask.x) {
            discard;
        }
    }
    return float4(input.position.z, 0.0, 0.0, 1.0);
}
