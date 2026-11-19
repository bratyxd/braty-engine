// lines3d.frag.hlsl - outputs the vertex colour premultiplied (HDR values above 1 bloom).

struct Output
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

float4 main(Output input) : SV_Target0
{
    return float4(input.color.rgb * input.color.a, input.color.a);
}
