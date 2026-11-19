// mesh3d_deform.vert.hlsl - skinned and/or morphed 3D meshes.
// Storage buffers (vertex, space0):
//   t0 joints: 4 float4 columns per joint matrix (br_mat4 as stored in C)
//   t1 morphs: per target t, vertex v: [ (t * vertex_count + v) * 2 ] = position delta,
//              [ ... + 1 ] = normal delta
// Uniform b1: morph weights, joint offset/count, morph count, vertex count.
#include "mesh3d_varyings.hlsli"

StructuredBuffer<float4> u_joints : register(t0, space0);
StructuredBuffer<float4> u_morphs : register(t1, space0);

cbuffer Deform : register(b1, space1)
{
    float4 u_morph_weights[2];
    uint4 u_deform; // x = first joint, y = joint count (0 = no skin), z = morph count, w = vertex count
};

float3 joint_apply(uint joint, float4 p)
{
    uint base = (u_deform.x + joint) * 4;
    return (u_joints[base] * p.x + u_joints[base + 1] * p.y + u_joints[base + 2] * p.z + u_joints[base + 3] * p.w).xyz;
}

Mesh3dVaryings main(Mesh3dInput input)
{
    float3 position = input.position;
    float3 normal = input.normal;
    float3 tangent = input.tangent.xyz;

    for (uint t = 0; t < u_deform.z; t++) {
        float w = u_morph_weights[t / 4][t % 4];
        uint index = (t * u_deform.w + input.vertex_id) * 2;
        position += u_morphs[index].xyz * w;
        normal += u_morphs[index + 1].xyz * w;
    }

    if (u_deform.y > 0) {
        float3 sp = float3(0.0, 0.0, 0.0);
        float3 sn = float3(0.0, 0.0, 0.0);
        float3 st = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < 4; i++) {
            float w = input.weights[i];
            if (w > 0.0) {
                uint j = min(input.joints[i], u_deform.y - 1);
                sp += joint_apply(j, float4(position, 1.0)) * w;
                sn += joint_apply(j, float4(normal, 0.0)) * w;
                st += joint_apply(j, float4(tangent, 0.0)) * w;
            }
        }
        position = sp;
        normal = sn;
        tangent = st;
    }
    return mesh3d_finish(input, position, normal, tangent);
}
