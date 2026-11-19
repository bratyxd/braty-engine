// particles.vert.hlsl - one textured quad per instance, expanded on the GPU (no vertex buffer).
// Per-instance attributes (br_particles.c packs them into a dynamic and a static buffer):
//   TEXCOORD0 float4 rect: local quad x0, y0, x1, y1 (anchor, scale and frame size applied)
//   TEXCOORD1 float2 position
//   TEXCOORD2 float  rotation (radians, clockwise on screen)
//   TEXCOORD3 float2 uv of the frame's top-left corner
//   TEXCOORD4 float4 uv step along the frame's x (xy) and y (zw): handles rotated atlas frames
//   TEXCOORD5 float4 premultiplied tint * alpha (UBYTE4_NORM)
// Uniform slot 0 (vertex): transform = ortho * space * container matrix, container colour.
// The output matches textured.frag.hlsl's input.

cbuffer Uniforms : register(b0, space1)
{
    float4x4 u_transform;
    float4 u_color;
};

struct Input
{
    float4 rect : TEXCOORD0;
    float2 position : TEXCOORD1;
    float rotation : TEXCOORD2;
    float2 uv_origin : TEXCOORD3;
    float4 uv_steps : TEXCOORD4;
    float4 color : TEXCOORD5;
    uint vertex_id : SV_VertexID;
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input)
{
    // two triangles: corners 0 1 2, 0 2 3 (top-left, top-right, bottom-right, bottom-left)
    uint corners[6] = { 0, 1, 2, 0, 2, 3 };
    uint corner = corners[input.vertex_id % 6];
    float fx = (corner == 1 || corner == 2) ? 1.0 : 0.0;
    float fy = (corner >= 2) ? 1.0 : 0.0;
    float2 local = float2(lerp(input.rect.x, input.rect.z, fx), lerp(input.rect.y, input.rect.w, fy));
    float s = sin(input.rotation);
    float c = cos(input.rotation);
    float2 p = float2(local.x * c - local.y * s, local.x * s + local.y * c) + input.position;

    Output output;
    output.position = mul(u_transform, float4(p, 0.0, 1.0));
    output.uv = input.uv_origin + input.uv_steps.xy * fx + input.uv_steps.zw * fy;
    output.color = input.color * u_color;
    return output;
}
