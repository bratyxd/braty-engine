// filter_mask.frag.hlsl - alpha mask / colour mask (t1 = the mask rendered over the same area).
#include "filter_common.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_mask : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_mask_sampler : register(s1, space2);

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_mode; // x = 0 alpha mask, 1 colour mask; y = 1 inverse
};

float4 main(FilterVaryings input) : SV_Target0
{
    float4 color = u_input.Sample(u_input_sampler, input.uv);
    float4 mask = u_mask.Sample(u_mask_sampler, input.uv);
    if (u_mode.x < 0.5) {
        float k = u_mode.y > 0.5 ? 1.0 - mask.a : mask.a;
        return color * k;
    }
    float3 k3 = u_mode.y > 0.5 ? 1.0 - mask.rgb : mask.rgb;
    float k = max(k3.r, max(k3.g, k3.b));
    return float4(color.rgb * k3, color.a * k);
}
