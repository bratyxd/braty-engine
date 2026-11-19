// textured.frag.hlsl - texture sample multiplied by the vertex colour.
// Fragment sampler slot 0: the texture (SDL: fragment textures/samplers live in space2).

[[vk::combinedImageSampler]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler : register(s0, space2);

// Must match the vertex shader's output exactly, SV_Position included: D3D12 links the
// stages by register order, not by name.
struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target0
{
    return u_texture.Sample(u_sampler, input.uv) * input.color;
}
