// No Hi-Z here, since the inter-phase pyramid lacks full BasePass depth and pops whole chunks out.

#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"
#include "Includes/TerrainCommon.slang"

// Covers the 81 meshlets of a default 64-resolution chunk in one pass; larger chunks stride.
#define LOCAL_SIZE_X 128

// Pointers first so each 8-byte address stays 8-aligned.
struct FTerrainCullPushConstants
{
    GPUSpan<FTerrainChunkInfo>      Chunks;
    GPUSpan<FTerrainMeshletInfo>    Meshlets;
    GPUSpan<FTerrainVisibleMeshlet> VisibleMeshlets;
    GPUSpan<FDrawIndirectArguments> TerrainIndirect;
};

groupshared bool gsChunkVisible;

void AABBToSphere(float3 BoundsMin, float3 BoundsMax, out float3 Center, out float Radius)
{
    Center = (BoundsMin + BoundsMax) * 0.5;
    Radius = length(BoundsMax - Center);
}

bool ChunkVisible(FFrustum F, float3 BoundsMin, float3 BoundsMax)
{
    float3 Center;
    float  Radius;
    AABBToSphere(BoundsMin, BoundsMax, Center, Radius);
    return FrustumContainsSphere(F, Center, Radius);
}

FTerrainCullPushConstants* Pass() { return GetPassArgs<FTerrainCullPushConstants>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(
    uint3   GroupID    : SV_GroupID,
    uint    LocalIdx   : SV_GroupIndex)
{
    const uint ChunkIndex = GroupID.x;
    if (!Pass().Chunks.Contains(ChunkIndex))
        return;

    if (LocalIdx == 0)
    {
        FTerrainChunkInfo Chunk = Pass().Chunks.Get(ChunkIndex);
        gsChunkVisible = ChunkVisible(Scene().CullData.Frustum, Chunk.BoundsMin, Chunk.BoundsMax);
    }
    GroupMemoryBarrierWithGroupSync();

    if (!gsChunkVisible)
        return;

    FTerrainChunkInfo Chunk = Pass().Chunks.Get(ChunkIndex);

    for (uint Local = LocalIdx; Local < Chunk.MeshletCount; Local += LOCAL_SIZE_X)
    {
        const uint MeshletIndex = Chunk.MeshletOffset + Local;
        if (!Pass().Meshlets.Contains(MeshletIndex))
            break;

        FTerrainMeshletInfo Meshlet = Pass().Meshlets.Get(MeshletIndex);

        // Per-meshlet frustum refinement; tighter bounds catch silhouettes the chunk sphere misses.
        if (!ChunkVisible(Scene().CullData.Frustum, Meshlet.BoundsMin, Meshlet.BoundsMax))
            continue;

        // Base 0 because the CPU rewrites FirstInstance to 0, and reading it would contend with the lanes incrementing InstanceCount.
        const uint Slot = AppendReserve(&Pass().TerrainIndirect.At(0u).InstanceCount,
                                        Pass().VisibleMeshlets.Count, /*Base*/ 0u);
        if (Slot == kAppendFull)
        {
            continue;
        }

        FTerrainVisibleMeshlet Out;
        Out.ChunkIndex   = ChunkIndex;
        Out.MeshletIndex = MeshletIndex;
        Pass().VisibleMeshlets.Set(Slot, Out);
    }
}
