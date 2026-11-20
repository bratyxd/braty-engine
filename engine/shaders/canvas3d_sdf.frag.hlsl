// canvas3d_sdf.frag.hlsl - text_sdf.frag for SDF text in the 3D pass (br_canvas3d): distance
// field coverage, then the canvas shading. u_field / u_mode come from the text draw.

#include "canvas3d_common.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_sampler : register(s0, space2);

float4 main(CanvasVaryings input) : SV_Target0
{
    float4 texel = u_texture.Sample(u_sampler, input.uv);
    float value = texel.a;
    if (u_mode.x > 0.5) {
        // pages are premultiplied on upload: MTSDF (a true SDF in alpha) has rgb * a here, so
        // divide it back out (MSDF pages have a = 1 and are unchanged)
        float3 rgb = texel.rgb / max(texel.a, 1.0 / 255.0);
        value = max(min(rgb.r, rgb.g), min(max(rgb.r, rgb.g), rgb.b));
    }
    float dist = (value - u_field.x) * u_field.y + u_field.z;
    float width = max(fwidth(dist), max(u_field.w, 0.0001));
    float alpha = saturate(dist / width + 0.5);
    return canvas_shade(input.color * alpha, input.world);
}
