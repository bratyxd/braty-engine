// filter_common.hlsli - what every filter pass shares (see engine/include/braty/br_filter.h).
// Include it from a filter fragment shader, then declare your cbuffer at b0 starting with the
// four globals (br_filter_globals in C) followed by your own fields:
//
//   cbuffer Filter : register(b0, space3)
//   {
//       float4 u_input_size; // input width, height, 1/width, 1/height (pixels)
//       float4 u_content;    // uv rect of the content in the input: x0, y0, x1, y1
//       float4 u_area;       // world rect of the filter area: x, y, width, height
//       float4 u_params;     // elapsed seconds, pixels per world unit, 0, 0
//       float4 my_value;     // your uniforms (br_filter.uniforms) start here
//   };

[[vk::combinedImageSampler]] Texture2D<float4> u_input : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_input_sampler : register(s0, space2);

// Output of filter.vert.hlsl; a fragment shader's input must be exactly this.
struct FilterVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;    // input texture uv
    float2 local : TEXCOORD1; // 0..1 across the filter area
};
