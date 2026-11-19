// canvas3d_varyings.hlsli - vertex -> fragment struct of the br_canvas3d shaders.

struct CanvasVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint texture_slot : TEXCOORD2;
    float3 world : TEXCOORD3;
};
