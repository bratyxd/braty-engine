// filter_color_matrix.frag.hlsl - Pixi's 4x5 colour matrix on straight colour (also the alpha filter).
#include "filter_common.hlsli"

cbuffer Filter : register(b0, space3)
{
    float4 u_input_size;
    float4 u_content;
    float4 u_area;
    float4 u_params;
    float4 u_red;    // matrix rows: coefficients of (r, g, b, a)
    float4 u_green;
    float4 u_blue;
    float4 u_alpha;
    float4 u_offset; // fifth column
    float4 u_mix;    // x = 0 original .. 1 result
};

float4 main(FilterVaryings input) : SV_Target0
{
    float4 color = u_input.Sample(u_input_sampler, input.uv);
    float4 straight = float4(0.0, 0.0, 0.0, color.a);
    if (color.a > 0.0) {
        straight.rgb = color.rgb / color.a;
    }
    float4 result = float4(dot(u_red, straight), dot(u_green, straight), dot(u_blue, straight),
                           dot(u_alpha, straight)) + u_offset;
    result = saturate(result);
    result.rgb *= result.a;
    return lerp(color, result, u_mix.x);
}
