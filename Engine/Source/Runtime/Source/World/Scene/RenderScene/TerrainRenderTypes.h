#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RenderResource.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

#ifndef VERIFY_SSBO_ALIGNMENT
#define VERIFY_SSBO_ALIGNMENT(Type) \
    static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned");
#endif

namespace Lumina
{
    /** Maximum number of paintable layers the renderer blends in the terrain pixel shader. */
    constexpr uint32 GTerrainMaxLayers = 4;

    // 7x7 quads = 49 quads, 8x8 verts (64), 98 tris -- fits MESHLET_MAX_VERTICES/TRIANGLES.
    constexpr int32 GTerrainMeshletQuads        = 7;
    constexpr int32 GTerrainMeshletVertsPerSide = GTerrainMeshletQuads + 1;
    constexpr int32 GTerrainMeshletMaxQuads     = GTerrainMeshletQuads * GTerrainMeshletQuads;
    constexpr int32 GTerrainMeshletMaxTris      = GTerrainMeshletMaxQuads * 2;
    constexpr int32 GTerrainMeshletMaxVerts     = GTerrainMeshletMaxTris * 3;

    /** Mirror of FTerrainRenderParams in TerrainBase{Vertex,Pixel}Pass.slang. */
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

    /** Mirror of FTerrainNormalParams in TerrainNormalCompute.slang. */
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

    /** Per-chunk metadata: tight world AABBs + meshlet range. Cull pass tests against frustum/HiZ. */
    struct alignas(16) FTerrainChunkInfo
    {
        FVector3   BoundsMin       = FVector3(0.0f);
        float       _Pad0           = 0.0f;
        FVector3   BoundsMax       = FVector3(0.0f);
        float       _Pad1           = 0.0f;

        // QuadOriginXY = ChunkCoord * QuadsPerChunk.
        FIntVector2  ChunkCoord      = FIntVector2(0);
        FIntVector2  QuadOrigin      = FIntVector2(0);

        // Range into per-terrain meshlet array; MeshletCount stored for future partial chunks.
        uint32      MeshletOffset   = 0u;
        uint32      MeshletCount    = 0u;
        uint32      _Pad2           = 0u;
        uint32      _Pad3           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainChunkInfo);

    /** Per-meshlet sub-region of its parent chunk's quad grid; cull pass appends survivors. */
    struct alignas(16) FTerrainMeshletInfo
    {
        FVector3   BoundsMin       = FVector3(0.0f);
        float       _Pad0           = 0.0f;
        FVector3   BoundsMax       = FVector3(0.0f);
        float       _Pad1           = 0.0f;

        FIntVector2  ChunkLocalQuadOrigin = FIntVector2(0);
        // Quads covered per axis; smaller for partial meshlets at chunk far edge.
        FIntVector2  QuadExtent      = FIntVector2(0);

        uint32      ChunkIndex      = 0u;
        uint32      _Pad2           = 0u;
        uint32      _Pad3           = 0u;
        uint32      _Pad4           = 0u;
    };
    VERIFY_SSBO_ALIGNMENT(FTerrainMeshletInfo);

    /** Survivor slot from terrain cull; consumed by VS via SV_InstanceID. */
    struct alignas(8) FTerrainVisibleMeshlet
    {
        uint32  ChunkIndex   = 0u;
        uint32  MeshletIndex = 0u;
    };
    static_assert(sizeof(FTerrainVisibleMeshlet) == 8);

    // Render-thread-owned GPU resources for one terrain. Lives in TerrainGPUStates keyed by entity, NOT
    // on the component, so the render thread never dereferences a component the game thread may have freed.
    struct FTerrainGPUState
    {
        /** R32_FLOAT mirror of CPU Heightmap. */
        FSceneImage     HeightmapTexture;

        /** RGBA8 normal; derived on GPU. */
        FSceneImage     NormalTexture;

        /** R8_UNORM array, one slice per layer. */
        FSceneImage     LayerWeightTexture;

        FSceneBuffer    ChunkInfoBuffer;
        FSceneBuffer    MeshletInfoBuffer;
        FSceneBuffer    VisibleMeshletBuffer;

        /** Single FDrawIndirectArguments slot; cull atomic-increments InstanceCount. */
        FSceneBuffer    IndirectDrawBuffer;

        uint32  AllocatedResolution = 0;
        uint32  AllocatedLayerCount = 0;
        uint32  AllocatedChunkCount = 0;
        uint32  AllocatedMeshletCount = 0;
    };

    /** Shared TerrainBaseVertexPass/PixelPass push block (no descriptor set 2): render params + cull
        buffers by device address, terrain textures by bindless index. Pointers first (8-byte aligned). */
    struct FTerrainPushConstants
    {
        uint64  ParamsAddr        = 0;   // ConstBufferPointer<FTerrainRenderParams>
        uint64  ChunksAddr        = 0;   // VS
        uint64  MeshletsAddr      = 0;   // VS
        uint64  VisibleAddr       = 0;   // VS
        uint32  HeightmapIndex    = 0;   // VS bindless 2D
        uint32  NormalIndex       = 0;   // VS bindless 2D
        uint32  LayerWeightsIndex = 0;   // PS bindless 2D-array
        uint32  _Pad0             = 0;
    };

    /** TerrainCull.slang push constants; frustum lives in scene globals. The four cull buffers are
        addressed by device pointer (no descriptor set); pointers first to keep 8-byte alignment. */
    struct FTerrainCullPushConstants
    {
        uint64  ChunksAddr          = 0;
        uint64  MeshletsAddr        = 0;
        uint64  VisibleMeshletsAddr = 0;
        uint64  TerrainIndirectAddr = 0;
        uint32  ChunkCount      = 0u;
        uint32  MeshletCount    = 0u;
        uint32  _Pad0           = 0u;
        uint32  _Pad1           = 0u;
    };
}
