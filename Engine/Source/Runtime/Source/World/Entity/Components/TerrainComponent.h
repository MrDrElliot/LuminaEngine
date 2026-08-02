#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderResource.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "TerrainComponent.generated.h"

namespace Lumina
{
    /** One weighted texture layer painted across the terrain surface. */
    REFLECT()
    struct RUNTIME_API STerrainLayer
    {
        GENERATED_BODY()

        /** World-space tiling applied to the layer's albedo/normal sampling. */
        PROPERTY(Editable, Category = "Terrain Layer", ClampMin = 0.01f, NoDrag, Delta = 0.1f)
        float UVScale = 1.0f;

        /** Optional human-readable label shown in the paint tool. */
        PROPERTY(Editable, Category = "Terrain Layer")
        FString Name;
    };

    /**
     * Game-thread-owned CPU mirror + dirty tracking. Mutated only on the game
     * thread (sculpt/paint, resolution edits, Extract prep). The render thread
     * never touches this directly; Extract snapshots the dirty bytes into the
     * per-frame FTerrainExtract so the renderer reads a private copy.
     */
    struct FTerrainCPUState
    {
        bool    bFullHeightmapDirty = true;
        bool    bFullWeightsDirty   = true;

        /** Chunk + meshlet metadata need a rebuild before the next cull. */
        bool    bChunksDirty = true;

        /** Inclusive dirty rects awaiting upload. */
        FIntVector2  HeightDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  HeightDirtyMax = FIntVector2(INT32_MIN);

        FIntVector2  WeightDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  WeightDirtyMax = FIntVector2(INT32_MIN);
        uint32      WeightDirtyLayerMask = 0u;

        /** Dimensions last handed to the renderer; a mismatch is a structural change. */
        int32   PreparedResolution      = 0;
        int32   PreparedChunkResolution = 0;
        int32   PreparedLayerCount      = -1;

        /** Bumped on every height edit. Foliage compares against this to know when to re-project. */
        uint32  HeightmapVersion = 0;

        /** Sample-space rect of height changes not yet consumed by the foliage follow pass (union of edits). */
        FIntVector2  FoliageDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  FoliageDirtyMax = FIntVector2(INT32_MIN);

        /** CPU mirror of the per-chunk/meshlet metadata; partial edits re-bound in place. */
        TVector<FTerrainChunkInfo>      Chunks;
        TVector<FTerrainMeshletInfo>    Meshlets;
    };

    /** Heightmap-driven terrain. */
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API STerrainComponent
    {
        GENERATED_BODY()

        STerrainComponent() = default;

        STerrainComponent(const STerrainComponent& Other)
            : Heightmap(Other.Heightmap)
            , LayerWeights(Other.LayerWeights)
            , Layers(Other.Layers)
            , Material(Other.Material)
            , Resolution(Other.Resolution)
            , ChunkResolution(Other.ChunkResolution)
            , TileWorldSize(Other.TileWorldSize)
            , MaxHeight(Other.MaxHeight)
            , bCastShadow(Other.bCastShadow)
            , bReceiveShadow(Other.bReceiveShadow)
        {
        }

        STerrainComponent& operator=(const STerrainComponent& Other)
        {
            if (this != &Other)
            {
                Heightmap       = Other.Heightmap;
                LayerWeights    = Other.LayerWeights;
                Layers          = Other.Layers;
                Material        = Other.Material;
                Resolution      = Other.Resolution;
                ChunkResolution = Other.ChunkResolution;
                TileWorldSize   = Other.TileWorldSize;
                MaxHeight       = Other.MaxHeight;
                bCastShadow     = Other.bCastShadow;
                bReceiveShadow  = Other.bReceiveShadow;
                // Transient CPU mirror never carries across a copy; the assigned-to terrain rebuilds from
                // its fresh CPU data (render scene allocates GPU state keyed by the new entity).
                CPUState        = FTerrainCPUState{};
            }
            return *this;
        }

        STerrainComponent(STerrainComponent&&) noexcept            = default;
        STerrainComponent& operator=(STerrainComponent&&) noexcept = default;

        /** Row-major; values normalized [0, 1] scaled by MaxHeight on GPU. */
        PROPERTY(Category = "Terrain|Data")
        TVector<float> Heightmap;

        /** Per-layer weights, row-major uint8 [0, 255]. */
        PROPERTY(Category = "Terrain|Data")
        TVector<uint8> LayerWeights;

        /** Paintable layers; layer 0 is base. */
        PROPERTY(Editable, Category = "Terrain|Layers")
        TVector<STerrainLayer> Layers;

        /** Null = engine DefaultTerrainMaterial at draw time. */
        PROPERTY(Editable, Category = "Terrain|Material")
        TObjectPtr<CMaterialInterface> Material;

        /** Must be (pow2 + 1) for clean chunking (513, 1025). */
        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 33, NoDrag, Delta = 32)
        int32 Resolution = 513;

        /** Cull/draw dispatch unit. */
        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 16, NoDrag, Delta = 16)
        int32 ChunkResolution = 64;

        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 1.0f, NoDrag, Delta = 64.0f)
        float TileWorldSize = 4096.0f;

        /** World displacement at heightmap sample == 1.0. */
        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 0.0f, NoDrag, Delta = 8.0f)
        float MaxHeight = 256.0f;

        PROPERTY(Editable, Category = "Terrain|Shadows")
        bool bCastShadow = true;

        PROPERTY(Editable, Category = "Terrain|Shadows")
        bool bReceiveShadow = true;

        /**
         * Game-thread-owned CPU mirror + dirty tracking; transient. The matching GPU
         * resources are render-owned in FForwardRenderScene::TerrainGPUStates, keyed by entity.
         */
        FTerrainCPUState CPUState;

        void MarkHeightDirty(const FIntVector2& Min, const FIntVector2& Max)
        {
            CPUState.HeightDirtyMin = Math::Min(CPUState.HeightDirtyMin, Min);
            CPUState.HeightDirtyMax = Math::Max(CPUState.HeightDirtyMax, Max);
            // Heights drive chunk/meshlet bounds; cull must test against fresh geometry.
            CPUState.bChunksDirty = true;

            // Foliage follow: accumulate the changed region + bump the version it watches.
            CPUState.FoliageDirtyMin = Math::Min(CPUState.FoliageDirtyMin, Min);
            CPUState.FoliageDirtyMax = Math::Max(CPUState.FoliageDirtyMax, Max);
            ++CPUState.HeightmapVersion;
        }

        // Whole-heightmap replacement (import / resize / load): flags a full upload + a full foliage re-project.
        void MarkHeightmapReplaced()
        {
            CPUState.bFullHeightmapDirty = true;
            CPUState.bChunksDirty        = true;
            CPUState.FoliageDirtyMin     = FIntVector2(0, 0);
            CPUState.FoliageDirtyMax     = FIntVector2(Resolution - 1, Resolution - 1);
            ++CPUState.HeightmapVersion;
        }

        void MarkWeightsDirty(uint32 LayerIndex, const FIntVector2& Min, const FIntVector2& Max)
        {
            CPUState.WeightDirtyMin = Math::Min(CPUState.WeightDirtyMin, Min);
            CPUState.WeightDirtyMax = Math::Max(CPUState.WeightDirtyMax, Max);
            CPUState.WeightDirtyLayerMask |= (1u << LayerIndex);
        }
    };
}
