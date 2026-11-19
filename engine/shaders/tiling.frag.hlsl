// tiling.frag.hlsl - repeats one texture frame (atlas frames too) across a tiling sprite.
// Input matches textured.vert.hlsl's output; uv is the unwrapped tile coordinate (1 = one tile).
// Fragment uniform slot 0: where the frame sits in the source and the clamp range in tile space.

[[vk::combinedImageSampler]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler : register(s0, space2);

cbuffer Uniforms : register(b0, space3)
{
    float4 u_origin_x;   // xy = uv of the frame's top-left, zw = uv step along the frame's x
    float4 u_y;          // xy = uv step along the frame's y
    float4 u_clamp;      // xy = min, zw = max of the wrapped coordinate (clamp margin)
};

struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target0
{
    float2 f = clamp(frac(input.uv), u_clamp.xy, u_clamp.zw);
    float2 uv = u_origin_x.xy + u_origin_x.zw * f.x + u_y.xy * f.y;
    // gradients of the unwrapped coordinate: no mip seam where frac() jumps
    float2 gx = ddx(input.uv);
    float2 gy = ddy(input.uv);
    float2 dx = u_origin_x.zw * gx.x + u_y.xy * gx.y;
    float2 dy = u_origin_x.zw * gy.x + u_y.xy * gy.y;
    return u_texture.SampleGrad(u_sampler, uv, dx, dy) * input.color;
}
