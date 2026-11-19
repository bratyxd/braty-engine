// canvas3d_custom.vert.hlsl - textured.vert's layout (position, uv, colour) placed in the 3D
// world, for custom 2D draws replayed by br_canvas3d (SDF text). Uniforms as canvas3d.vert.

#include "canvas3d_varyings.hlsli"

cbuffer Transform : register(b0, space1)
{
    float4x4 u_mvp;
};

cbuffer Model : register(b1, space1)
{
    float4x4 u_model;
};

struct Input
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

CanvasVaryings main(Input input)
{
    CanvasVaryings output;
    float4 p = float4(input.position, 0.0, 1.0);
    output.position = mul(u_mvp, p);
    output.uv = input.uv;
    output.color = input.color;
    output.texture_slot = 0;
    output.world = mul(u_model, p).xyz;
    return output;
}
