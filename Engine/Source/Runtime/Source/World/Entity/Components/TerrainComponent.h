#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderResource.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Terrain/TerrainStore.h"
#include "World/Entity/Components/VirtualTextureComponent.h"
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

        /** Inclusive dirty rects awaiting upload. */
        FIntVector2  HeightDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  HeightDirtyMax = FIntVector2(INT32_MIN);

        FIntVector2  WeightDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  WeightDirtyMax = FIntVector2(INT32_MIN);
        uint32      WeightDirtyLayerMask = 0u;

        /** Dimensions last handed to the renderer; a mismatch is a structural change. */
        int32   PreparedResolution      = 0;
        int32   PreparedLayerCount      = -1;

        /** Bumped on every height edit. Foliage compares against this to know when to re-project. */
        uint32  HeightmapVersion = 0;

        // The store's edit serial the renderer last saw, so an edit that marked nothing still reaches it.
        uint64  SeenEditSerial = 0;

        /** Sample-space rect of height changes not yet consumed by the foliage follow pass (union of edits). */
        FIntVector2  FoliageDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  FoliageDirtyMax = FIntVector2(INT32_MIN);

        // Height changes the collision patches have not rebuilt from yet.
        FIntVector2  CollisionDirtyMin = FIntVector2(INT32_MAX);
        FIntVector2  CollisionDirtyMax = FIntVector2(INT32_MIN);


        // A new render scene holds none of this terrain's GPU data, so everything uploads again.
        void MarkRendererLost()
        {
            bFullHeightmapDirty     = true;
            bFullWeightsDirty       = true;
            PreparedResolution      = 0;
            PreparedLayerCount      = -1;
        }
    };

    /** Heightmap-driven terrain. */
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API STerrainComponent
    {
        GENERATED_BODY()

        STerrainComponent() = default;

        ~STerrainComponent() = default;
        STerrainComponent(const STerrainComponent& Other)
            : DataFile(Other.DataFile)
            , Layers(Other.Layers)
            , Material(Other.Material)
            , Resolution(Other.Resolution)
            , TileWorldSize(Other.TileWorldSize)
            , MaxHeight(Other.MaxHeight)
            , bCastShadow(Other.bCastShadow)
            , bReceiveShadow(Other.bReceiveShadow)
            , VirtualTextureOutputs(Other.VirtualTextureOutputs)
            , Store(Other.Store)
        {
        }

        STerrainComponent& operator=(const STerrainComponent& Other)
        {
            if (this != &Other)
            {
                DataFile        = Other.DataFile;
                Store           = Other.Store;
                Layers          = Other.Layers;
                Material        = Other.Material;
                Resolution      = Other.Resolution;
                TileWorldSize   = Other.TileWorldSize;
                MaxHeight       = Other.MaxHeight;
                bCastShadow     = Other.bCastShadow;
                bReceiveShadow  = Other.bReceiveShadow;
                VirtualTextureOutputs = Other.VirtualTextureOutputs;
                // Transient CPU mirror never carries across a copy, and the target terrain rebuilds from its own data.
                CPUState        = FTerrainCPUState{};
            }
            return *this;
        }

        STerrainComponent(STerrainComponent&&) noexcept            = default;
        STerrainComponent& operator=(STerrainComponent&&) noexcept = default;

        // The project path of the tiled file the heights and weights save to, beside the world rather than in it.
        PROPERTY(Hidden, Category = "Terrain|Data")
        FString DataFile;

        // Worlds saved before the data file kept heights here, which EnsureStore moves into a store and clears.
        PROPERTY(Hidden, Category = "Terrain|Data")
        TVector<float> Heightmap;

        PROPERTY(Hidden, Category = "Terrain|Data")
        TVector<uint8> LayerWeights;

        /** Paintable layers; layer 0 is base. */
        PROPERTY(Editable, Category = "Terrain|Layers")
        TVector<STerrainLayer> Layers;

        /** Null = engine DefaultTerrainMaterial at draw time. */
        PROPERTY(Editable, Category = "Terrain|Material")
        TStrongObjectPtr<CMaterialInterface> Material;

        // Power of two plus one (513, 1025) fills the quadtree exactly.
        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 33, NoDrag, Delta = 32)
        int32 Resolution = 513;

        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 1.0f, NoDrag, Delta = 64.0f)
        float TileWorldSize = 4096.0f;

        /** World displacement at heightmap sample == 1.0. */
        PROPERTY(Editable, Category = "Terrain|Layout", ClampMin = 0.0f, NoDrag, Delta = 8.0f)
        float MaxHeight = 256.0f;

        PROPERTY(Editable, Category = "Terrain|Shadows")
        bool bCastShadow = true;

        PROPERTY(Editable, Category = "Terrain|Shadows")
        bool bReceiveShadow = true;

        // Runtime virtual textures this terrain's material fills from above, so grass and other surfaces can sample the ground under them.
        PROPERTY(Editable, Category = "Terrain|Virtual Texture")
        TVector<SVirtualTextureOutput> VirtualTextureOutputs;

        /**
         * Game-thread-owned CPU mirror + dirty tracking; transient. The matching GPU
         * resources are render-owned in FDefaultSceneRenderer::TerrainGPUStates, keyed by entity.
         */
        FTerrainCPUState CPUState;

        // Shared with copies of this component, such as a PIE world's, and found again by DataFile through the engine's registry.
        TSharedPtr<FTerrainStore> Store;

        // The store, opened from DataFile or migrated from the old arrays or started flat, on the thread that owns the component.
        FTerrainStore* EnsureStore();
        FTerrainStore* GetStore() const { return Store.Get(); }
        // Replaces the data, under InDataFile or a fresh data file when that is empty.
        void SetStore(TSharedPtr<FTerrainStore> NewStore, FStringView InDataFile = {});
        // Writes unsaved edits, or the whole store when it has never been written, to DataFile.
        bool SaveStore() const;

        void MarkHeightDirty(const FIntVector2& Min, const FIntVector2& Max)
        {
            CPUState.HeightDirtyMin = Math::Min(CPUState.HeightDirtyMin, Min);
            CPUState.HeightDirtyMax = Math::Max(CPUState.HeightDirtyMax, Max);
            // Foliage follow: accumulate the changed region + bump the version it watches.
            CPUState.FoliageDirtyMin = Math::Min(CPUState.FoliageDirtyMin, Min);
            CPUState.FoliageDirtyMax = Math::Max(CPUState.FoliageDirtyMax, Max);
            CPUState.CollisionDirtyMin = Math::Min(CPUState.CollisionDirtyMin, Min);
            CPUState.CollisionDirtyMax = Math::Max(CPUState.CollisionDirtyMax, Max);
            ++CPUState.HeightmapVersion;
        }

        // Whole-heightmap replacement (import / resize / load): flags a full upload + a full foliage re-project.
        void MarkHeightmapReplaced()
        {
            CPUState.bFullHeightmapDirty = true;
            CPUState.FoliageDirtyMin     = FIntVector2(0, 0);
            CPUState.FoliageDirtyMax     = FIntVector2(Resolution - 1, Resolution - 1);
            CPUState.CollisionDirtyMin   = FIntVector2(0, 0);
            CPUState.CollisionDirtyMax   = FIntVector2(Resolution - 1, Resolution - 1);
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
