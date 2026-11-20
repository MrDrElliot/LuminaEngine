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
    // Lumina::FTerrainDrawArgs as words, the indexed InstanceCount at 1 and the mesh task group count at 5.
    GPUSpan<uint>                   TerrainIndirect;
    GPUSpan<uint>                   ChunkLevels;
    int                             QuadsPerChunk;
    int                             Resolution;
    float                           SampleSpacing;
    uint                            _Pad0;
};

static const uint kTerrainDrawInstanceWord  = 1u;
static const uint kTerrainDrawMeshGroupWord = 5u;

// Widest a terrain quad may look before its chunk drops to a finer level, since the normal map carries the detail between vertices.
static const float kTerrainQuadPixels = 6.0;

groupshared uint gsChunkLevel;

// The coarsest level whose quads still project within kTerrainQuadPixels from the nearest point of the chunk.
uint SelectChunkLevel(FTerrainChunkInfo Chunk)
{
    const float3 Camera   = Scene().CullData.CullCameraPosition.xyz;
    const float  Distance = length(Camera - clamp(Camera, Chunk.BoundsMin, Chunk.BoundsMax));
    const float  PixelsPerUnitAtOne = Scene().CullData.CullCameraProjection[1][1] * 0.5 * float(GetScreenSize().y);
    const float  QuadPixels = Pass().SampleSpacing * PixelsPerUnitAtOne / max(Distance, 1e-3);
    const float  Level = floor(log2(max(kTerrainQuadPixels / max(QuadPixels, 1e-6), 1.0)));
    return min((uint)Level, kTerrainLODLevels - 1u);
}

groupshared bool gsChunkVisible;

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
        gsChunkVisible = FrustumIntersectsAABB(Scene().CullData.Frustum, Chunk.BoundsMin, Chunk.BoundsMax);
        gsChunkLevel   = SelectChunkLevel(Chunk);
        // Written for hidden chunks too, since a visible neighbor's border vertices read it.
        if (Pass().ChunkLevels.Contains(ChunkIndex))
        {
            Pass().ChunkLevels.Set(ChunkIndex, gsChunkLevel);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (!gsChunkVisible)
        return;

    FTerrainChunkInfo Chunk = Pass().Chunks.Get(ChunkIndex);
    const int2 Extent = TerrainChunkExtent(Chunk.QuadOrigin, Pass().QuadsPerChunk, Pass().Resolution);

    uint LevelOffset = 0u;
    for (uint Finer = 0u; Finer < gsChunkLevel; ++Finer)
    {
        const int2 Side = TerrainLevelMeshletsSide(Extent, Finer);
        LevelOffset += (uint)(Side.x * Side.y);
    }
    const int2 LevelSide = TerrainLevelMeshletsSide(Extent, gsChunkLevel);
    const uint LevelCount = min((uint)(LevelSide.x * LevelSide.y), Chunk.MeshletCount - min(LevelOffset, Chunk.MeshletCount));

    for (uint Local = LocalIdx; Local < LevelCount; Local += LOCAL_SIZE_X)
    {
        const uint MeshletIndex = Chunk.MeshletOffset + LevelOffset + Local;
        if (!Pass().Meshlets.Contains(MeshletIndex))
            break;

        FTerrainMeshletInfo Meshlet = Pass().Meshlets.Get(MeshletIndex);

        // Per-meshlet frustum refinement; tighter bounds catch silhouettes the chunk box misses.
        if (!FrustumIntersectsAABB(Scene().CullData.Frustum, Meshlet.BoundsMin, Meshlet.BoundsMax))
            continue;

        // Base 0 because the CPU rewrites FirstInstance to 0, and reading it would contend with the lanes incrementing InstanceCount.
        const uint Slot = AppendReserve(Pass().TerrainIndirect.At(kTerrainDrawInstanceWord),
                                        Pass().VisibleMeshlets.Count, /*Base*/ 0u);
        if (Slot == kAppendFull)
        {
            continue;
        }
        InterlockedAdd(Pass().TerrainIndirect.At(kTerrainDrawMeshGroupWord)[0], 1u);

        FTerrainVisibleMeshlet Out;
        Out.ChunkIndex   = ChunkIndex;
        Out.MeshletIndex = MeshletIndex;
        Pass().VisibleMeshlets.Set(Slot, Out);
    }
}
