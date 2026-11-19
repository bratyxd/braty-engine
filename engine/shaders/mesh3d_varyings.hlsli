// mesh3d_varyings.hlsli - vertex inputs and the vertex -> fragment struct of every 3D mesh shader.
// mesh3d.vert / mesh3d_deform.vert write Mesh3dVaryings; every 3D fragment shader (mesh3d.frag,
// shadow3d.frag, custom materials) must take exactly this struct as input.

struct Mesh3dInput
{
    // per vertex (buffer 0, br_vertex3d)
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv0 : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float4 color : TEXCOORD5;
    uint4 joints : TEXCOORD6;
    float4 weights : TEXCOORD7;
    // per instance (buffer 1, br_instance3d): model matrix columns, colour
    float4 model0 : TEXCOORD8;
    float4 model1 : TEXCOORD9;
    float4 model2 : TEXCOORD10;
    float4 model3 : TEXCOORD11;
    float4 instance_color : TEXCOORD12;
    uint vertex_id : SV_VertexID;
};

struct Mesh3dVaryings
{
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float4 uv : TEXCOORD3;     // uv0 in xy, uv1 in zw
    float4 color : TEXCOORD4;  // vertex colour * instance colour (linear, straight alpha)
};

// Vertex uniform b0: the camera (or the shadow tile's light) view-projection.
cbuffer View : register(b0, space1)
{
    float4x4 u_view_projection;
};

// Model matrix columns -> world position / normal. The normal uses the inverse-transpose of a
// rotation * scale matrix (each column divided by its squared length): exact without shear.
Mesh3dVaryings mesh3d_finish(Mesh3dInput input, float3 position, float3 normal, float3 tangent)
{
    float3 c0 = input.model0.xyz;
    float3 c1 = input.model1.xyz;
    float3 c2 = input.model2.xyz;
    float3 world = c0 * position.x + c1 * position.y + c2 * position.z + input.model3.xyz;
    float3 n = c0 * (normal.x / dot(c0, c0)) + c1 * (normal.y / dot(c1, c1)) + c2 * (normal.z / dot(c2, c2));
    float3 t = c0 * tangent.x + c1 * tangent.y + c2 * tangent.z;

    Mesh3dVaryings output;
    output.position = mul(u_view_projection, float4(world, 1.0));
    output.world_position = world;
    output.normal = n;
    output.tangent = float4(t, input.tangent.w);
    output.uv = float4(input.uv0, input.uv1);
    output.color = input.color * input.instance_color;
    return output;
}
