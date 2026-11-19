// mesh3d_common.hlsli - for 3D fragment shaders (mesh3d.frag and custom materials): the
// varyings, the per-view frame/light block at fragment b0 (br_frame3d_uniforms in C) and light
// helpers. Custom materials put their own block at register(b1, space3), textures at t0...

#include "mesh3d_varyings.hlsli"

#define LIGHT3D_MAX 16
#define SHADOW3D_TILES 8
#define LIGHT3D_DIRECTIONAL 0
#define LIGHT3D_POINT 1
#define LIGHT3D_SPOT 2
#define PI 3.14159265

cbuffer Frame : register(b0, space3)
{
    float4 u_camera_position;              // xyz
    float4 u_camera_forward;               // xyz
    float4 u_sky;                          // rgb * intensity
    float4 u_ground;                       // rgb * intensity
    float4 u_counts;                       // x light count, y cascade count, z 1/atlas width, w 1/atlas height
    float4 u_cascade_far;                  // camera distance where each cascade ends
    float4x4 u_shadow_matrix[SHADOW3D_TILES];
    float4 u_shadow_rect[SHADOW3D_TILES];  // atlas uv offset xy, size zw
    float4 u_light_position[LIGHT3D_MAX];  // xyz, w = kind
    float4 u_light_direction[LIGHT3D_MAX]; // xyz = direction the light travels, w = range (0 = infinite)
    float4 u_light_color[LIGHT3D_MAX];     // rgb * intensity, w = shadow tile (-1 = none)
    float4 u_light_params[LIGHT3D_MAX];    // x cos outer, y cos inner, z depth bias, w normal bias
};

// Hemisphere ambient for a normal.
float3 light3d_ambient(float3 n)
{
    return lerp(u_ground.rgb, u_sky.rgb, n.y * 0.5 + 0.5);
}

// Direction to light i (unit) and its radiance at p, without shadows.
float3 light3d_radiance(int i, float3 p, out float3 to_light)
{
    float kind = u_light_position[i].w;
    float3 color = u_light_color[i].rgb;
    if (kind == LIGHT3D_DIRECTIONAL) {
        to_light = -u_light_direction[i].xyz;
        return color;
    }
    float3 d = u_light_position[i].xyz - p;
    float dist = length(d);
    to_light = d / max(dist, 1e-4);
    float range = u_light_direction[i].w;
    float attenuation = 1.0 / max(dist * dist, 1e-4);
    if (range > 0.0) {
        float r = saturate(1.0 - pow(dist / range, 4.0));
        attenuation *= r * r;
    }
    if (kind == LIGHT3D_SPOT) {
        float cd = dot(-to_light, u_light_direction[i].xyz);
        attenuation *= smoothstep(u_light_params[i].x, u_light_params[i].y, cd);
    }
    return color * attenuation;
}
