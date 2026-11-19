// post3d.vert.hlsl - one triangle covering the whole target (no vertex buffer, 3 vertices).

struct PostVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 k_corners[3] = { float2(0.0, 0.0), float2(2.0, 0.0), float2(0.0, 2.0) };

PostVaryings main(uint id : SV_VertexID)
{
    float2 uv = k_corners[id];
    PostVaryings output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
