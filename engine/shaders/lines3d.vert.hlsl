// lines3d.vert.hlsl - coloured 3D lines/triangles for custom 3D draw items (examples/view3d_demo.c
// draws its axis gizmo with it). Vertex: float3 position, float4 linear colour.
// Uniform b0: view-projection (br_draw3d_custom_info.view_projection).

cbuffer View : register(b0, space1)
{
    float4x4 u_view_projection;
};

struct Input
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct Output
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

Output main(Input input)
{
    Output output;
    output.position = mul(u_view_projection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}
