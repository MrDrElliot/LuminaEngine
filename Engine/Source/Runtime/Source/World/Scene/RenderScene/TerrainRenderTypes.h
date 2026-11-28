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
    static_assert(GTerrainMeshletQuads == 1 << TERRAIN_MESHLET_QUADS_LOG2, "Shaders shift by the node's quad count.");
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

    // Quadtree nodes all draw GTerrainMeshletQuads quads per side, a node at level L spacing its vertices 1 << L samples apart.
    constexpr int32 GTerrainMaxTreeDepth = 14;

    // How many levels sit below the root, so the leaves (level 0) step one sample per quad.
    inline int32 GetTerrainTreeDepth(int32 Resolution)
    {
        const int32 Leaves = Math::Max(1, (Resolution - 1 + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads);
        int32 Depth = 0;
        while ((1 << Depth) < Leaves && Depth < GTerrainMaxTreeDepth)
        {
            ++Depth;
        }
        return Depth;
    }

    // Pages sit on quadtree nodes three levels above the leaves, so the deepest page depth holds mip 0.
    inline int32 GetTerrainPageDepth(int32 TreeDepth)
    {
        return Math::Max(TreeDepth - 3, 0);
    }

    // Nodes are stored root first, depth by depth, each depth row-major. Mirrors TerrainCommon.slang.
    constexpr uint32 GetTerrainNodeDepthOffset(int32 Depth)
    {
        return ((1u << (2u * (uint32)Depth)) - 1u) / 3u;
    }

    constexpr uint32 GetTerrainNodeCount(int32 TreeDepth)
    {
        return GetTerrainNodeDepthOffset(TreeDepth + 1);
    }

    // Caps one depth's candidate list and the drawn cut, which stay near a thousand at any terrain size.
    constexpr uint32 GTerrainMaxCullNodes = 32768u;

    // Mirrors FTerrainRenderParams in Includes/TerrainTypes.slang.
    struct alignas(16) FTerrainRenderParams
    {
        FVector2   OriginXZ        = FVector2(0.0f);
        float       TileWorldSize   = 4096.0f;
        float       MaxHeight       = 256.0f;

        int32       Resolution      = 0;
        int32       TreeDepth       = 0;
        int32       LayerCount      = 0;
        float       SampleSpacing   = 1.0f;

        FVector3   WorldOriginY    = FVector3(0.0f);
        uint32      EntityID        = 0u;

        uint32      MaterialIndex   = 0u;
        // Pages sit at depths 0 through this, the deepest holding mip 0.
        int32       PageDepth       = 0;
        uint32      Flags           = 0u;
        uint32      _Pad0           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainRenderParams);

    // Mirrors TERRAIN_FLAG_* in TerrainTypes.slang.
    constexpr uint32 GTerrainFlagCastShadow = 1u;

    // One node the cull drew, with the level of the node across each edge (-X, +X, -Y, +Y) in four bits apiece.
    struct alignas(8) FTerrainVisibleNode
    {
        uint32  Node           = 0u;
        uint32  NeighborLevels = 0u;
    };
    static_assert(sizeof(FTerrainVisibleNode) == 8);

    class FTerrainStore;

    struct FTerrainGPUState
    {
        // The store the node bounds came from and its change serial then, so only bounds edited since upload again.
        TSharedPtr<FTerrainStore> BoundsStore;
        uint64                    BoundsSerial = 0;

        // The page slot of each page node, which shaders look a sample's page up in.
        RHI::FGPUAllocation    PageTableBuffer;
        uint32                 PageTableCount = 0;

        // Normalized min and max height under every quadtree node.
        RHI::FGPUAllocation    NodeBoundsBuffer;
        // The cull walks one depth at a time, reading one list and filling the other with the children it splits into.
        RHI::FGPUAllocation    NodeLists[2];
        // Per depth, the indirect group count and the node count the depth above appended.
        RHI::FGPUAllocation    LevelArgsBuffer;
        RHI::FGPUAllocation    VisibleNodeBuffer;

        // One FTerrainDrawArgs, whose counts the cull atomically increments.
        RHI::FGPUAllocation    IndirectDrawBuffer;

        uint32  AllocatedNodeCount  = 0;
        uint32  ListCapacity        = 0;
        uint32  VisibleCapacity     = 0;
        int32   TreeDepth           = 0;

        // Every free path walks this, so a buffer added to the state cannot leak from one of them.
        template<typename TFunc>
        void ForEachBuffer(TFunc&& Func)
        {
            RHI::FGPUAllocation* Buffers[] = { &NodeBoundsBuffer, &NodeLists[0], &NodeLists[1], &LevelArgsBuffer, &VisibleNodeBuffer, &IndirectDrawBuffer,
                                               &PageTableBuffer };
            for (RHI::FGPUAllocation* Buffer : Buffers)
            {
                Func(*Buffer);
            }
        }
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
        RHI::TGPUSpan<FVector2>               NodeBounds;
        RHI::TGPUSpan<FTerrainVisibleNode>    Visible;
        RHI::TGPUSpan<uint32>                 PageTable;
        uint32  HeightAtlasIndex  = 0;
        uint32  NormalAtlasIndex  = 0;
        uint32  WeightAtlasIndex  = 0;
        // Where this terrain's visible nodes start in the frame's terrain VisID slots.
        uint32  VisibleBase       = 0;
    };
    static_assert(sizeof(FTerrainRecord) == 128, "FTerrainRecord must match TerrainTypes.slang.");

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

    // Four words per depth, the indirect dispatch groups then the node count, mirrored in TerrainCull.slang.
    constexpr uint32 kTerrainLevelArgsWords = 4u;

    struct FTerrainCullPushConstants
    {
        RHI::TGPUSpan<uint32> NodesIn;
        RHI::TGPUSpan<uint32> NodesOut;
        RHI::TGPUSpan<uint32> LevelArgs;
        RHI::TGPUSpan<uint32> TerrainIndirect;
        uint32  TerrainIndex = 0;
        uint32  Depth        = 0;
        uint32  _Pad0        = 0;
        uint32  _Pad1        = 0;
    };
    static_assert(sizeof(FTerrainCullPushConstants) == 80, "FTerrainCullPushConstants must match TerrainCull.slang.");
}
