#include "Includes/DecalCommon.slang"

// Back faces are rasterized (the pipeline culls front faces), so the box still covers the screen with the camera inside it.

struct VSOutput
{
    nointerpolation uint DecalIndex : DECAL_INDEX;
    nointerpolation float DistanceFade : DECAL_FADE;
    float4 Position : SV_Position;
};

// The decal index is the draw's FirstInstance, since a per-draw push value lays out differently across stages.
[shader("vertex")]
VSOutput VertexMain(uint VertexID : SV_VulkanVertexID, uint DecalIndex : SV_VulkanInstanceID)
{
    FGPUDecal Decal = Pass().Decals.Get(DecalIndex);

    VSOutput Output;
    Output.DecalIndex   = DecalIndex;
    Output.DistanceFade = DecalDistanceFade(Decal, GetCameraPosition());

    // A decal faded out by distance collapses to a point, so it costs no pixels at all.
    if (Output.DistanceFade <= 0.0)
    {
        Output.Position = float4(0.0, 0.0, 0.0, 1.0);
        return Output;
    }

    float3 LocalPos = kDecalCubeVerts[VertexID];
    float4 WorldPos = mul(Decal.DecalToWorld, float4(LocalPos, 1.0));
    Output.Position = WorldToClip(WorldPos.xyz);
    return Output;
}
