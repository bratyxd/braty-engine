// post3d_common.hlsli - what every 3D post pass shares (see engine/include/braty/br_post3d.h).
// The vertex shader is post3d.vert.hlsl (one full-target triangle). A custom pass includes this,
// reads u_input (the current HDR image) and declares its own block at register(b0, space3).

[[vk::combinedImageSampler]] Texture2D<float4> u_input : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_input_sampler : register(s0, space2);

// Output of post3d.vert.hlsl; a post fragment shader's input must be exactly this.
struct PostVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;    // 0,0 = top-left of the target
};
