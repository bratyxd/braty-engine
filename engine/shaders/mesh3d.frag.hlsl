// mesh3d.frag.hlsl - built-in 3D materials: PBR metallic-roughness, Phong, Flat, Toon.
// Textures (fragment, space2): t0 base colour, t1 metallic-roughness, t2 normal, t3 occlusion,
// t4 emissive, t5 shadow atlas (R32F depth). Uniform b0 = frame (mesh3d_common.hlsli),
// b1 = material (br_material3d_uniforms in C). Output: premultiplied linear HDR.
#include "mesh3d_common.hlsli"

[[vk::combinedImageSampler]] Texture2D<float4> u_base_tex : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState u_base_smp : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_mr_tex : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_mr_smp : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_normal_tex : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState u_normal_smp : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_occlusion_tex : register(t3, space2);
[[vk::combinedImageSampler]] SamplerState u_occlusion_smp : register(s3, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_emissive_tex : register(t4, space2);
[[vk::combinedImageSampler]] SamplerState u_emissive_smp : register(s4, space2);
[[vk::combinedImageSampler]] Texture2D<float4> u_shadow_tex : register(t5, space2);
[[vk::combinedImageSampler]] SamplerState u_shadow_smp : register(s5, space2);

cbuffer Material : register(b1, space3)
{
    float4 u_base_color;
    float4 u_emissive;            // rgb * strength
    float4 u_pbr;                 // metallic, roughness, normal scale, occlusion strength
    float4 u_specular;            // rgb, w = shininess
    float4 u_alpha;               // x cutoff, y double sided, z receive shadows
    uint4 u_flags;                // x model, y toon bands, z texture bits, w alpha mode
    float4 u_uv[10];              // per slot: (a, c, tx, uv set), (b, d, ty, 0)
};

#define MODEL_PBR 0
#define MODEL_PHONG 1
#define MODEL_FLAT 2
#define MODEL_TOON 3

float2 slot_uv(int slot, float4 uv)
{
    float4 r0 = u_uv[slot * 2];
    float4 r1 = u_uv[slot * 2 + 1];
    float2 base = r0.w > 0.5 ? uv.zw : uv.xy;
    return float2(dot(r0.xyz, float3(base, 1.0)), dot(r1.xyz, float3(base, 1.0)));
}

bool has_texture(int slot)
{
    return (u_flags.z & (1u << slot)) != 0;
}

float3 srgb_to_linear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// Textures are premultiplied on upload: undo that for colour data.
float4 unpremultiply(float4 t)
{
    return t.a > 0.0 ? float4(t.rgb / t.a, t.a) : t;
}

// One 2x2 bilinear depth comparison around uv (GatherRed: w = (0,0), z = (1,0), x = (0,1), y = (1,1)).
float shadow_bilinear(float2 uv, float depth)
{
    float2 size = 1.0 / u_counts.zw;
    float2 st = uv * size - 0.5;
    float2 f = frac(st);
    float4 g = u_shadow_tex.GatherRed(u_shadow_smp, (floor(st) + 1.0) / size);
    float4 lit = float4(depth <= g.x ? 1.0 : 0.0, depth <= g.y ? 1.0 : 0.0, depth <= g.z ? 1.0 : 0.0, depth <= g.w ? 1.0 : 0.0);
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// 3x3 bilinear PCF in one atlas tile. bias is in depth units, already slope-scaled.
float shadow_tile(int tile, float3 p, float3 n, float bias, float normal_bias)
{
    float4 c = mul(u_shadow_matrix[tile], float4(p + n * normal_bias, 1.0));
    float3 ndc = c.xyz / c.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z > 1.0 || ndc.z < 0.0) {
        return 1.0;
    }
    float4 rect = u_shadow_rect[tile];
    float2 texel = u_counts.zw;
    float2 uv = rect.xy + (ndc.xy * float2(0.5, -0.5) + 0.5) * rect.zw;
    uv = clamp(uv, rect.xy + texel * 2.5, rect.xy + rect.zw - texel * 2.5);
    float depth = ndc.z - bias;
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            lit += shadow_bilinear(uv + float2(x, y) * texel, depth);
        }
    }
    return lit / 9.0;
}

float light_shadow(int i, float3 p, float3 n, float3 l)
{
    int tile = (int)u_light_color[i].w;
    if (tile < 0 || u_alpha.z < 0.5) {
        return 1.0;
    }
    // slope-scaled: surfaces at a grazing angle to the light need more bias
    float n_dot_l = saturate(dot(n, l));
    float slope = min(sqrt(1.0 - n_dot_l * n_dot_l) / max(n_dot_l, 1e-3), 4.0);
    float bias = u_light_params[i].z * (1.0 + slope);
    float normal_bias = u_light_params[i].w;
    if (u_light_position[i].w == LIGHT3D_DIRECTIONAL) {
        float d = dot(p - u_camera_position.xyz, u_camera_forward.xyz);
        int cascades = (int)u_counts.y;
        for (int c = 0; c < cascades; c++) {
            if (d < u_cascade_far[c]) {
                // farther cascades cover more world per texel: scale the normal offset
                return shadow_tile(tile + c, p, n, bias, normal_bias * (1.0 + c));
            }
        }
        return 1.0;
    }
    return shadow_tile(tile, p, n, bias, normal_bias);
}

float d_ggx(float n_dot_h, float a)
{
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float g_smith(float n_dot_v, float n_dot_l, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gv = n_dot_v / (n_dot_v * (1.0 - k) + k);
    float gl = n_dot_l / (n_dot_l * (1.0 - k) + k);
    return gv * gl;
}

float3 f_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

float4 main(Mesh3dVaryings input) : SV_Target0
{
    float4 base = u_base_color * input.color;
    if (has_texture(0)) {
        float4 t = unpremultiply(u_base_tex.Sample(u_base_smp, slot_uv(0, input.uv)));
        base *= float4(srgb_to_linear(t.rgb), t.a);
    }
    if (u_flags.w == 1 && base.a < u_alpha.x) {
        discard;
    }
    float alpha = u_flags.w == 2 ? base.a : 1.0;

    float3 emissive = u_emissive.rgb;
    if (has_texture(4)) {
        emissive *= srgb_to_linear(unpremultiply(u_emissive_tex.Sample(u_emissive_smp, slot_uv(4, input.uv))).rgb);
    }
    if (u_flags.x == MODEL_FLAT) {
        float3 flat = base.rgb + emissive;
        return float4(flat * alpha, alpha);
    }

    float3 p = input.world_position;
    float3 v = normalize(u_camera_position.xyz - p);
    float3 n = normalize(input.normal);
    if (u_alpha.y > 0.5 && dot(n, v) < 0.0) {
        n = -n; // double sided: light the back face
    }
    if (has_texture(2)) {
        float3 t = input.tangent.xyz - n * dot(n, input.tangent.xyz);
        if (dot(t, t) > 1e-8) {
            t = normalize(t);
            float3 b = cross(n, t) * (input.tangent.w < 0.0 ? -1.0 : 1.0);
            float3 m = u_normal_tex.Sample(u_normal_smp, slot_uv(2, input.uv)).xyz * 2.0 - 1.0;
            m.xy *= u_pbr.z;
            n = normalize(t * m.x + b * m.y + n * m.z);
        }
    }
    float occlusion = 1.0;
    if (has_texture(3)) {
        occlusion = lerp(1.0, u_occlusion_tex.Sample(u_occlusion_smp, slot_uv(3, input.uv)).r, u_pbr.w);
    }
    float metallic = u_pbr.x;
    float roughness = u_pbr.y;
    if (has_texture(1)) {
        float4 mr = u_mr_tex.Sample(u_mr_smp, slot_uv(1, input.uv));
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    float n_dot_v = max(dot(n, v), 1e-4);
    float3 ambient = light3d_ambient(n);
    float3 color = float3(0.0, 0.0, 0.0);
    int count = (int)u_counts.x;

    if (u_flags.x == MODEL_PBR) {
        float3 f0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
        float3 diffuse_color = base.rgb * (1.0 - metallic);
        float a = roughness * roughness;
        for (int i = 0; i < count; i++) {
            float3 l;
            float3 radiance = light3d_radiance(i, p, l);
            float n_dot_l = dot(n, l);
            if (n_dot_l <= 0.0) {
                continue;
            }
            float3 h = normalize(l + v);
            float3 f = f_schlick(max(dot(h, v), 0.0), f0);
            float3 spec = d_ggx(max(dot(n, h), 0.0), a) * g_smith(n_dot_v, n_dot_l, roughness) * f /
                          max(4.0 * n_dot_v * n_dot_l, 1e-4);
            // diffuse without 1/PI and specular times PI: a white light of intensity 1 lights a
            // white diffuse surface to 1 (glTF-viewer style units)
            color += ((1.0 - f) * diffuse_color + spec * PI) * radiance * n_dot_l * light_shadow(i, p, n, l);
        }
        float3 r = reflect(-v, n);
        float gloss = 1.0 - roughness;
        float3 f_ambient = f0 + (max(float3(gloss, gloss, gloss), f0) - f0) * pow(1.0 - n_dot_v, 5.0);
        float3 env = lerp(light3d_ambient(r), ambient, roughness);
        color += (ambient * diffuse_color * (1.0 - f_ambient) + env * f_ambient) * occlusion;
    } else if (u_flags.x == MODEL_PHONG) {
        for (int i = 0; i < count; i++) {
            float3 l;
            float3 radiance = light3d_radiance(i, p, l);
            float n_dot_l = dot(n, l);
            if (n_dot_l <= 0.0) {
                continue;
            }
            float3 h = normalize(l + v);
            float spec = pow(max(dot(n, h), 0.0), max(u_specular.w, 1.0));
            color += (base.rgb * n_dot_l + u_specular.rgb * spec) * radiance * light_shadow(i, p, n, l);
        }
        color += ambient * base.rgb * occlusion;
    } else {
        // toon: banded diffuse, hard specular, rim light
        float bands = max((float)u_flags.y, 1.0);
        for (int i = 0; i < count; i++) {
            float3 l;
            float3 radiance = light3d_radiance(i, p, l);
            float lit = saturate(dot(n, l)) * light_shadow(i, p, n, l);
            float banded = ceil(lit * bands) / bands;
            float3 h = normalize(l + v);
            float spec = step(0.5, pow(max(dot(n, h), 0.0), max(u_specular.w, 1.0))) * step(0.001, lit);
            color += (base.rgb * banded + u_specular.rgb * spec) * radiance;
        }
        float rim = step(0.6, 1.0 - n_dot_v);
        color += ambient * base.rgb * occlusion + rim * ambient * 0.5;
    }
    color += emissive;
    return float4(color * alpha, alpha);
}
