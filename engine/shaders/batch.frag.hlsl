// batch.frag.hlsl - samples one of 8 textures (slot chosen per vertex) times the vertex colour.
// Textures are premultiplied. Gradients are taken before the branch so sampling inside the
// branch is valid even when neighbouring pixels pick another slot.

[[vk::combinedImageSampler]] Texture2D<float4> u_texture0 : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler0 : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture1 : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler1 : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture2 : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler2 : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture3 : register(t3, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler3 : register(s3, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture4 : register(t4, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler4 : register(s4, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture5 : register(t5, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler5 : register(s5, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture6 : register(t6, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler6 : register(s6, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_texture7 : register(t7, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler7 : register(s7, space2);

// Must match batch.vert.hlsl's Output exactly (SV_Position included).
struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint texture_slot : TEXCOORD2;
};

float4 main(Input input) : SV_Target0
{
    float2 dx = ddx(input.uv);
    float2 dy = ddy(input.uv);
    float4 texel;
    if (input.texture_slot == 0) {
        texel = u_texture0.SampleGrad(u_sampler0, input.uv, dx, dy);
    } else if (input.texture_slot == 1) {
        texel = u_texture1.SampleGrad(u_sampler1, input.uv, dx, dy);
    } else if (input.texture_slot == 2) {
        texel = u_texture2.SampleGrad(u_sampler2, input.uv, dx, dy);
    } else if (input.texture_slot == 3) {
        texel = u_texture3.SampleGrad(u_sampler3, input.uv, dx, dy);
    } else if (input.texture_slot == 4) {
        texel = u_texture4.SampleGrad(u_sampler4, input.uv, dx, dy);
    } else if (input.texture_slot == 5) {
        texel = u_texture5.SampleGrad(u_sampler5, input.uv, dx, dy);
    } else if (input.texture_slot == 6) {
        texel = u_texture6.SampleGrad(u_sampler6, input.uv, dx, dy);
    } else {
        texel = u_texture7.SampleGrad(u_sampler7, input.uv, dx, dy);
    }
    return texel * input.color;
}
