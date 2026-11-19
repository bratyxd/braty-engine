// canvas3d.vert.hlsl - the 2D batch vertex (br_batch_vertex) placed in the 3D world.
// Uniform b0: model-view-projection (view_projection * canvas world * 2D-to-plane * space),
// b1: model (the same without view_projection) for the world position used by lighting.

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
    uint texture_slot : TEXCOORD3;
};

CanvasVaryings main(Input input)
{
    CanvasVaryings output;
    float4 p = float4(input.position, 0.0, 1.0);
    output.position = mul(u_mvp, p);
    output.uv = input.uv;
    output.color = input.color;
    output.texture_slot = input.texture_slot;
    output.world = mul(u_model, p).xyz;
    return output;
}
