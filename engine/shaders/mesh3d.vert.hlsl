// mesh3d.vert.hlsl - static (not skinned, not morphed) 3D meshes, always instanced.
#include "mesh3d_varyings.hlsli"

Mesh3dVaryings main(Mesh3dInput input)
{
    return mesh3d_finish(input, input.position, input.normal, input.tangent.xyz);
}
