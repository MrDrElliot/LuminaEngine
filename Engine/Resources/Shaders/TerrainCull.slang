// Two-tier frustum-only terrain cull: reject whole chunks, then refine per-meshlet for silhouettes.
// No Hi-Z here (the inter-phase pyramid lacks full BasePass depth and caused chunk-granularity pop-out).
// Dispatch: one workgroup per chunk, one thread per meshlet; thread 0 runs the chunk test (groupshared).

#include "Includes/Culling.slang"
#include "Includes/TerrainCommon.slang"

// Sized so the default ChunkResolution (64) -> 81 meshlets/chunk fits one wave; larger chunks stride.
#define LOCAL_SIZE_X 128

// The four cull buffers are addressed by device pointer (chunks/meshlets read,
// visible-list/indirect-args written). Pointers first so each 8-byte address stays 8-aligned.
struct FTerrainCullPushConstants
{
    FTerrainChunkInfo*      Chunks;
    FTerrainMeshletInfo*    Meshlets;
    FTerrainVisibleMeshlet* VisibleMeshlets;
    FDrawIndirectArguments* TerrainIndirect;
    uint ChunkCount;
    uint MeshletCount;
    uint _Pad0;
    uint _Pad1;
};

groupshared bool gChunkVisible;

// Smallest sphere enclosing the AABB. Conservative for plane-vs-sphere tests.
void AABBToSphere(float3 BoundsMin, float3 BoundsMax, out float3 Center, out float Radius)
{
    Center = (BoundsMin + BoundsMax) * 0.5;
    Radius = length(BoundsMax - Center);
}

// Frustum-vs-sphere on the AABB's enclosing sphere: conservative (errs toward "keep"). Reuses the
// meshlet cull's helper so frustum-convention quirks (plane orientation, normalization) are shared.
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
    if (ChunkIndex >= Pass().ChunkCount)
        return;

    // Thread 0 owns the chunk-level test. Broadcast through groupshared so
    // every meshlet thread can short-circuit on a coarse rejection.
    if (LocalIdx == 0)
    {
        FTerrainChunkInfo Chunk = Pass().Chunks[ChunkIndex];
        gChunkVisible = ChunkVisible(Scene().CullData.Frustum, Chunk.BoundsMin, Chunk.BoundsMax);
    }
    GroupMemoryBarrierWithGroupSync();

    if (!gChunkVisible)
        return;

    FTerrainChunkInfo Chunk = Pass().Chunks[ChunkIndex];

    // Stride the meshlet test by LOCAL_SIZE_X so chunks larger than the workgroup still cover
    // every meshlet with one thread per pass.
    for (uint Local = LocalIdx; Local < Chunk.MeshletCount; Local += LOCAL_SIZE_X)
    {
        const uint MeshletIndex = Chunk.MeshletOffset + Local;
        if (MeshletIndex >= Pass().MeshletCount)
            break;

        FTerrainMeshletInfo Meshlet = Pass().Meshlets[MeshletIndex];

        // Per-meshlet frustum refinement; tighter bounds catch silhouettes the chunk sphere misses.
        if (!ChunkVisible(Scene().CullData.Frustum, Meshlet.BoundsMin, Meshlet.BoundsMax))
            continue;

        // Append: survivors race for a unique slot in the shared indirect-arg via atomic add.
        uint OldCount;
        InterlockedAdd(Pass().TerrainIndirect[0].InstanceCount, 1, OldCount);

        const uint Slot = Pass().TerrainIndirect[0].FirstInstance + OldCount;

        // Bounded even though it is currently safe by construction: the args are reseeded every frame with
        // FirstInstance = 0 / InstanceCount = 0, and one append per (chunk, meshlet) pair cannot exceed
        // MeshletCount, which is what sized VisibleMeshlets. That is three separate unwritten invariants
        // holding up an unbounded atomic used directly as a write index -- the exact shape that took out the
        // meshlet defer list. MeshletCount is already in the push block, so the guard is free.
        // continue, not return: this is a grid-stride loop, so the thread still owns further meshlets.
        if (Slot >= Pass().MeshletCount)
        {
            continue;
        }

        FTerrainVisibleMeshlet Out;
        Out.ChunkIndex   = ChunkIndex;
        Out.MeshletIndex = MeshletIndex;
        Pass().VisibleMeshlets[Slot] = Out;
    }
}
