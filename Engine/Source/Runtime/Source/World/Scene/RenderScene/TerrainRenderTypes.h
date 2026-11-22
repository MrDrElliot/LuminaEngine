#pragma once

#include "Core/Math/Math.h"
#include "Containers/StaticArray.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RenderResource.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

#ifndef VERIFY_SSBO_ALIGNMENT
#define VERIFY_SSBO_ALIGNMENT(Type) \
    static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned")
#endif

namespace Lumina
{
    // Paintable layers the terrain pixel shader blends.
    constexpr uint32 GTerrainMaxLayers = 4;

    // The shaders size their mesh outputs from the same constants, in SharedConstants.h.
    constexpr int32 GTerrainMeshletQuads        = (int32)TERRAIN_MESHLET_QUADS;
    constexpr int32 GTerrainMeshletVertsPerSide = (int32)TERRAIN_MESHLET_VERTS_PER_SIDE;
    constexpr int32 GTerrainMeshletMaxQuads     = GTerrainMeshletQuads * GTerrainMeshletQuads;
    static_assert(GTerrainMeshletVertsPerSide == GTerrainMeshletQuads + 1 && GTerrainMeshletMaxQuads * 2 == (int32)TERRAIN_MESHLET_MAX_TRIANGLES,
                  "The terrain meshlet constants in SharedConstants.h disagree.");

    // The vertex path's grid, drawn indexed so each shared vertex runs the vertex shader about once instead of six times.
    struct FTerrainMeshletIndices
    {
        uint16 Indices[GTerrainMeshletMaxQuads * 6] = {};
    };

    constexpr FTerrainMeshletIndices MakeTerrainMeshletIndices()
    {
        FTerrainMeshletIndices Out;
        int32 Cursor = 0;
        for (int32 Y = 0; Y < GTerrainMeshletQuads; ++Y)
        {
            for (int32 X = 0; X < GTerrainMeshletQuads; ++X)
            {
                const uint16 V00 = (uint16)(Y * GTerrainMeshletVertsPerSide + X);
                const uint16 V10 = (uint16)(V00 + 1);
                const uint16 V01 = (uint16)(V00 + GTerrainMeshletVertsPerSide);
                const uint16 V11 = (uint16)(V01 + 1);
                const uint16 Quad[6] = { V00, V10, V11, V00, V11, V01 };
                for (uint16 Index : Quad)
                {
                    Out.Indices[Cursor++] = Index;
                }
            }
        }
        return Out;
    }

    constexpr FTerrainMeshletIndices GTerrainMeshletIndices = MakeTerrainMeshletIndices();

    // Rounded up, since a grid that is not a whole number of chunks still needs its last quads drawn.
    inline int32 GetTerrainChunksPerSide(int32 Resolution, int32 QuadsPerChunk)
    {
        return Math::Max(1, (Resolution - 1 + QuadsPerChunk - 1) / QuadsPerChunk);
    }

    // Each level samples every other vertex of the one before, up to quads 32 samples wide. Mirrors TerrainCommon.slang.
    constexpr uint32 GTerrainLODLevels = 6;

    // A chunk on the far edge of the heightmap holds fewer quads than the rest.
    inline FIntVector2 GetTerrainChunkExtent(const FIntVector2& QuadOrigin, int32 QuadsPerChunk, int32 Resolution)
    {
        return FIntVector2(Math::Min(QuadsPerChunk, Resolution - 1 - QuadOrigin.x), Math::Min(QuadsPerChunk, Resolution - 1 - QuadOrigin.y));
    }

    // The last coarse quad is clamped to the chunk edge, so a level always ends where the chunk does.
    inline int32 GetTerrainLevelQuads(int32 FineQuads, uint32 Level)
    {
        return (FineQuads + (1 << Level) - 1) >> Level;
    }

    inline int32 GetTerrainLevelMeshletsSide(int32 FineQuads, uint32 Level)
    {
        return (GetTerrainLevelQuads(FineQuads, Level) + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;
    }

    // Mirrors FTerrainRenderParams in Includes/TerrainTypes.slang.
    struct alignas(16) FTerrainRenderParams
    {
        FVector2   OriginXZ        = FVector2(0.0f);
        float       TileWorldSize   = 4096.0f;
        float       MaxHeight       = 256.0f;

        int32       Resolution      = 0;
        int32       ChunkResolution = 0;
        int32       ChunksPerSide   = 0;
        int32       LayerCount      = 0;

        FVector3   WorldOriginY    = FVector3(0.0f);
        uint32      EntityID        = 0u;

        uint32      MaterialIndex   = 0u;
        int32       MeshletsPerChunkSide = 0;
        int32       MeshletQuadSide      = GTerrainMeshletQuads;
        uint32      _Pad0           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainRenderParams);

    // Mirrors FTerrainNormalParams in TerrainNormalCompute.slang.
    struct alignas(16) FTerrainNormalParams
    {
        int32   Resolution   = 0;
        int32   RegionMinX   = 0;
        int32   RegionMinY   = 0;
        int32   RegionSizeX  = 0;

        int32   RegionSizeY   = 0;
        float   TileWorldSize = 4096.0f;
        float   MaxHeight     = 256.0f;
        int32   _Pad          = 0;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainNormalParams);

    // Tight world bounds and meshlet range of one chunk, which the cull pass tests against the frustum and HiZ.
    struct alignas(16) FTerrainChunkInfo
    {
        FVector3   BoundsMin       = FVector3(0.0f);
        float       _Pad0           = 0.0f;
        FVector3   BoundsMax       = FVector3(0.0f);
        float       _Pad1           = 0.0f;

        // QuadOriginXY = ChunkCoord * QuadsPerChunk.
        FIntVector2  ChunkCoord      = FIntVector2(0);
        FIntVector2  QuadOrigin      = FIntVector2(0);

        // Every level's meshlets, finest first, each level's count following from the chunk's extent.
        uint32      MeshletOffset   = 0u;
        uint32      MeshletCount    = 0u;
        uint32      _Pad2           = 0u;
        uint32      _Pad3           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainChunkInfo);

    // One meshlet's sub-region of its chunk's quad grid; the cull pass appends survivors.
    struct alignas(16) FTerrainMeshletInfo
    {
        FVector3   BoundsMin       = FVector3(0.0f);
        float       _Pad0           = 0.0f;
        FVector3   BoundsMax       = FVector3(0.0f);
        float       _Pad1           = 0.0f;

        // In quads of this meshlet's level, as is the extent.
        FIntVector2  ChunkLocalQuadOrigin = FIntVector2(0);
        // Quads covered per axis; smaller for partial meshlets at chunk far edge.
        FIntVector2  QuadExtent      = FIntVector2(0);

        uint32      ChunkIndex      = 0u;
        // Each of its quads spans 1 << Level heightmap samples.
        uint32      Level           = 0u;
        uint32      _Pad3           = 0u;
        uint32      _Pad4           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainMeshletInfo);

    // Survivor slot from terrain cull, one mesh workgroup or vertex-path instance per entry.
    struct alignas(8) FTerrainVisibleMeshlet
    {
        uint32  ChunkIndex   = 0u;
        uint32  MeshletIndex = 0u;
    };
    static_assert(sizeof(FTerrainVisibleMeshlet) == 8);

    struct FTerrainGPUState
    {
        // R32_FLOAT mirror of the CPU heightmap.
        FSceneImage     HeightmapTexture;

        // RG8 normals derived on the GPU, the packed XZ slope every reader rebuilds Y from.
        FSceneImage     NormalTexture;

        // R8_UNORM array, one slice per layer.
        FSceneImage     LayerWeightTexture;

        RHI::FGPUAllocation    ChunkInfoBuffer;
        RHI::FGPUAllocation    MeshletInfoBuffer;
        RHI::FGPUAllocation    VisibleMeshletBuffer;
        // The level each chunk drew at this view, which the vertex shader reads to meet a coarser neighbor without cracks.
        RHI::FGPUAllocation    ChunkLevelBuffer;

        // One FTerrainDrawArgs, whose counts the cull atomically increments.
        RHI::FGPUAllocation    IndirectDrawBuffer;

        uint32  AllocatedResolution = 0;
        uint32  AllocatedLayerCount = 0;
        uint32  AllocatedChunkCount = 0;
        uint32  AllocatedMeshletCount = 0;
    };

    // Per terrain and species scatter state; instances live in the retained arrays, so only the append cursor is owned here.
    struct FGrassGPUState
    {
        // Last frame's cursor, so the retire pass can deactivate exactly the slots that fell out of use.
        RHI::FGPUAllocation PrevCursorBuffer;

        // Blades the scatter wanted last time, which grows the block toward the budget.
        TArray<RHI::FGPUAllocation, RHI::kFramesInFlight> CursorReadback = {};

        // The block size asked for, grown from the readback toward the terrain's MaxInstancesPerSpecies.
        uint32 WantedCapacity = 0;

        // The retained block this state last scattered into; a different one may still hold another species' blades.
        uint32 BoundSlotBase = Constants::kIndexNoneU32;
        uint32 BoundCapacity = 0;

    };

    // Everything one terrain draws and shades from, published per frame in FSceneRoot::Terrains. Mirrors TerrainTypes.slang.
    struct alignas(16) FTerrainRecord
    {
        FTerrainRenderParams                  Params;
        RHI::TGPUSpan<FTerrainChunkInfo>      Chunks;
        RHI::TGPUSpan<FTerrainMeshletInfo>    Meshlets;
        RHI::TGPUSpan<FTerrainVisibleMeshlet> Visible;
        RHI::TGPUSpan<uint32>                 ChunkLevels;
        uint32  HeightmapIndex    = 0;
        uint32  NormalIndex       = 0;
        uint32  LayerWeightsIndex = 0;
        // Where this terrain's visible meshlets start in the frame's terrain VisID slots.
        uint32  VisibleBase       = 0;
    };
    static_assert(sizeof(FTerrainRecord) == 144, "FTerrainRecord must match TerrainTypes.slang.");

    // The indexed arguments the vertex path draws with, then the mesh task groups the mesh path launches, both counted by the cull.
    struct FTerrainDrawArgs
    {
        RHI::FDrawIndexedIndirectArguments Indexed;
        uint32 MeshGroups[3] = { 0u, 1u, 1u };
    };
    static_assert(sizeof(FTerrainDrawArgs) == 32, "FTerrainDrawArgs must match TerrainCull.slang.");
    constexpr uint32 kTerrainDrawInstanceWord  = offsetof(RHI::FDrawIndexedIndirectArguments, InstanceCount) / sizeof(uint32);
    constexpr uint32 kTerrainDrawMeshGroupWord = offsetof(FTerrainDrawArgs, MeshGroups) / sizeof(uint32);

    // Which terrain record a terrain draw reads, the only per-draw input.
    struct FTerrainDrawPush
    {
        uint32 TerrainIndex = 0;
        uint32 _Pad[3]      = {};
    };

    struct FTerrainCullPushConstants
    {
        RHI::TGPUSpan<FTerrainChunkInfo>      Chunks;
        RHI::TGPUSpan<FTerrainMeshletInfo>    Meshlets;
        RHI::TGPUSpan<FTerrainVisibleMeshlet> VisibleMeshlets;
        RHI::TGPUSpan<uint32>                 TerrainIndirect;
        RHI::TGPUSpan<uint32>                 ChunkLevels;
        int32   QuadsPerChunk = 0;
        int32   Resolution    = 0;
        // World size of one heightmap quad.
        float   SampleSpacing = 1.0f;
        uint32  _Pad0         = 0;
    };
    static_assert(sizeof(FTerrainCullPushConstants) == 96, "FTerrainCullPushConstants must match TerrainCull.slang.");
}
