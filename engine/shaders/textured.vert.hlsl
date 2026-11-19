// textured.vert.hlsl - position, uv and vertex colour through one matrix.
// Vertex layout: TEXCOORD0 float2 position, TEXCOORD1 float2 uv, TEXCOORD2 colour (float4 or UBYTE4_NORM).
// Uniform slot 0 (vertex): br_mat4 transform, column-major, applied as mul(transform, position).

cbuffer Uniforms : register(b0, space1)
{
    float4x4 u_transform;
};

struct Input
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input)
{
    Output output;
    output.position = mul(u_transform, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
