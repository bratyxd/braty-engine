// batch.vert.hlsl - the 2D batcher: position, uv, premultiplied colour, texture slot.
// Vertex layout (br_batch_vertex, 24 bytes): TEXCOORD0 float2 position, TEXCOORD1 float2 uv,
// TEXCOORD2 colour (UBYTE4_NORM), TEXCOORD3 uint texture slot 0..7.
// Uniform slot 0 (vertex): br_mat4 transform = ortho(area) * space, column-major.

cbuffer Uniforms : register(b0, space1)
{
    float4x4 u_transform;
};

struct Input
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
    uint texture_slot : TEXCOORD3;
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint texture_slot : TEXCOORD2;
};

Output main(Input input)
{
    Output output;
    output.position = mul(u_transform, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.texture_slot = input.texture_slot;
    return output;
}
