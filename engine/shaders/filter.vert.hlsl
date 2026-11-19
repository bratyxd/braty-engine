// filter.vert.hlsl - one triangle covering the whole output texture (no vertex buffer).
// Vertex uniform b0: br_filter_globals (only u_content is used here).

cbuffer Globals : register(b0, space1)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
};

struct FilterVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 local : TEXCOORD1;
};

static const float2 k_corners[3] = { float2(0.0, 0.0), float2(2.0, 0.0), float2(0.0, 2.0) };

FilterVaryings main(uint id : SV_VertexID)
{
    float2 uv = k_corners[id];
    FilterVaryings output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    output.local = (uv - u_content.xy) / (u_content.zw - u_content.xy);
    return output;
}
