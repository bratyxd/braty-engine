// canvas3d_common.hlsli - 2D content drawn inside the 3D pass (br_canvas3d): the shading shared
// by canvas3d.frag / canvas3d_sdf.frag. Fragment b0 = the 3D frame/light block (mesh3d_common.hlsli, pushed by the
// view), b1 = the canvas block below.

#include "mesh3d_common.hlsli"
#include "canvas3d_varyings.hlsli"

cbuffer Canvas : register(b1, space3)
{
    float4 u_tint;     // linear rgb, a = straight alpha
    float4 u_params;   // x = lit (1/0), y = alpha cutoff (0 = blended)
    float4 u_normal;   // world normal, turned toward the camera
    float4 u_field;    // SDF draws: text_sdf.frag's u_field
    float4 u_mode;     // SDF draws: text_sdf.frag's u_mode
};

float3 canvas_srgb_to_linear(float3 c)
{
    float3 low = c / 12.92;
    float3 high = pow(abs((c + 0.055) / 1.055), 2.4);
    return lerp(high, low, step(c, 0.04045));
}

// c = premultiplied sRGB colour from the 2D shaders. Returns what the 3D pass expects:
// premultiplied linear (blended), or opaque linear for alpha-tested canvases.
float4 canvas_shade(float4 c, float3 world)
{
    float a = c.a;
    float3 rgb = a > 0.0001 ? c.rgb / a : float3(0.0, 0.0, 0.0);
    rgb = canvas_srgb_to_linear(rgb) * u_tint.rgb;
    a *= u_tint.a;
    if (u_params.x > 0.5) {
        float3 n = u_normal.xyz;
        float3 light = light3d_ambient(n);
        int count = (int)u_counts.x;
        for (int i = 0; i < count; i++) {
            float3 to_light;
            float3 radiance = light3d_radiance(i, world, to_light);
            light += radiance * saturate(dot(n, to_light));
        }
        rgb *= light;
    }
    if (u_params.y > 0.0) {
        if (a < u_params.y) {
            discard;
        }
        return float4(rgb, 1.0);
    }
    return float4(rgb * a, a);
}
