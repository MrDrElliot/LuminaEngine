#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractTerrainInternal
    {
        using FTerrainExtract       = FDefaultSceneRenderer::FFrameData::FTerrainExtract;
        using FGrassSpeciesExtract  = FDefaultSceneRenderer::FFrameData::FGrassSpeciesExtract;
        using EHeightChange         = FDefaultSceneRenderer::FFrameData::EHeightChange;

        // Reuses the live element so its heap buffers outlive the frame instead of being freed and remade.
        template <typename T>
        T& ReuseAt(TVector<T>& Container, int32 Index)
        {
            return Index < Container.Num() ? Container[Index] : Container.EmplaceGetRef();
        }

        // A Resolution edit resamples the store, onto a new data file when the old one lives on disk or the map is large.
        void EnsureTerrainStore(STerrainComponent& Terrain)
        {
            FTerrainStore* Store = Terrain.EnsureStore();
            if (Terrain.Resolution < 2 || Terrain.Resolution == Store->GetResolution())
            {
                return;
            }
            constexpr int32 kLargestInMemoryResample = 8193;
            const bool bOnDisk = Store->HasFile() || Terrain.Resolution > kLargestInMemoryResample;
            const FString DataFile = bOnDisk ? FTerrainStoreRegistry::MakeDataFile() : FString();
            const FString DiskPath = bOnDisk ? FTerrainStoreRegistry::ResolveDiskPath(DataFile) : FString();
            TSharedPtr<FTerrainStore> Resampled = FTerrainStore::Resample(*Store, Terrain.Resolution, DiskPath);
            if (Resampled == nullptr)
            {
                LOG_ERROR("Terrain could not resample to {} samples, so it keeps {}.", Terrain.Resolution, Store->GetResolution());
                Terrain.Resolution = Store->GetResolution();
                return;
            }
            Terrain.SetStore(Resampled, DataFile);
        }

        // Reads the material rather than the component, since the GrassOutput node is where species are authored.
        void PrepareGrassExtract(const SGrassComponent* Grass, const STerrainComponent& Terrain, FTerrainExtract& Out)
        {
            Out.Grass.Reset();
            Out.GrassMaxInstances = 0;

            if (Grass == nullptr || !Grass->bEnabled)
            {
                return;
            }

            CMaterialInterface* MaterialInterface = Terrain.Material.Get();
            CMaterial* Material = MaterialInterface != nullptr ? MaterialInterface->GetMaterial() : nullptr;
            if (Material == nullptr || Material->GrassOutputs.IsEmpty())
            {
                return;
            }

            Out.GrassMaxInstances = Grass->MaxInstancesPerSpecies;

            for (const SGrassOutput& Output : Material->GrassOutputs)
            {
                CGrassType* Type = Output.GrassType.Get();
                if (Type == nullptr || Type->Mesh == nullptr)
                {
                    continue;
                }

                // A layer the terrain does not have would sample a slice past the array's end.
                if (Output.LayerIndex >= (uint32)Terrain.Layers.Num())
                {
                    continue;
                }

                const float Density = Type->Density * Output.DensityScale;
                if (Density <= 0.0f)
                {
                    continue;
                }

                FGrassSpeciesExtract& Species = Out.Grass.EmplaceGetRef();
                Species.Type          = Type;
                for (int32 Earlier = 0; Earlier < Out.Grass.Num() - 1; ++Earlier)
                {
                    if (Out.Grass[Earlier].Type == Type && Out.Grass[Earlier].LayerIndex == Output.LayerIndex)
                    {
                        ++Species.Occurrence;
                    }
                }
                Species.Mesh          = Type->Mesh.Get();
                // On the main thread, since resolving can grow the cache's maps.
                Species.ResolveHandle = FMeshResolveCache::Get().Resolve(Species.Mesh, {});
                Species.LayerIndex    = Output.LayerIndex;

                // Density is instances per square meter and a world unit is a meter, so the spacing is its inverse root.
                Species.CellSize = Math::Max(0.01f, 1.0f / Math::Sqrt(Density));

                Species.MinWeight     = Type->MinWeight;
                Species.ScaleMin      = Type->ScaleMin;
                Species.ScaleMax      = Type->ScaleMax;
                Species.ZOffset       = Type->ZOffset;
                Species.AlignToNormal = Type->AlignToNormal;
                Species.MaxSlopeCos   = Math::Cos(Type->MaxSlopeDegrees * (3.14159265358979f / 180.0f));
                Species.CullDistance  = Type->CullDistance;
                Species.Seed          = Type->Seed;
                Species.bReceiveShadow = Type->bReceiveShadow;
                Species.bContactShadows = Type->bContactShadows;
                Species.FullDensityDistance = Type->FullDensityDistance;
                Species.TerrainNormalDistance = Type->TerrainNormalDistance;
                Species.RootDarkening = Type->RootDarkening;
                Species.bRandomYaw    = Type->bRandomYaw;
                const CRuntimeVirtualTexture* Mask = Type->DensityMask.Get();
                Species.MaskTexture   = Mask != nullptr && Mask->GetResourceID() >= 0 ? (uint32)Mask->GetResourceID() : Constants::kIndexNoneU32;
                Species.MaskChannel   = (uint32)Math::Clamp(Type->MaskChannel, 0, 3);
                Species.MaskStrength  = Math::Max(Type->MaskStrength, 0.0f);
            }
        }

        void PrepareTerrainMaterial(STerrainComponent& Terrain, FTerrainExtract& Out)
        {
            // Cleared up front because the element is reused across frames and these are written conditionally.
            Out.VisBufferShader = {};
            Out.DeferredShader  = {};
            Out.MaterialIndex   = 0u;

            const auto ResolveStages = [&Out](const CMaterialInterface* Material)
            {
                if (Material == nullptr || !Material->IsUsableInDomain(EMaterialType::Terrain))
                {
                    return false;
                }
                Out.VisBufferShader = Material->GetShader(ActiveMeshletStage(EMaterialShaderStage::VisBufferMesh));
                Out.DeferredShader  = Material->GetShader(EMaterialShaderStage::Deferred);
                Out.VirtualTextureShader = Material->GetShader(EMaterialShaderStage::VirtualTextureCapture);
                return Out.VisBufferShader != nullptr && Out.DeferredShader != nullptr;
            };

            // Anything that is not a ready terrain material, wrong domain included, falls back to the default.
            CMaterialInterface* TerrainMaterial = Terrain.Material.Get();
            if (!ResolveStages(TerrainMaterial))
            {
                TerrainMaterial = CMaterial::GetDefaultTerrainMaterial();
            }
            if (ResolveStages(TerrainMaterial))
            {
                Out.MaterialIndex        = (uint32)Math::Max(TerrainMaterial->GetMaterialIndex(), 0);

                // Demand only, like particles, since terrain cannot simply disappear while its textures load.
                TerrainMaterial->RequestTexturesResolved();
            }
        }

        // Pages over whatever the heights or weights changed by upload again, and the world distance field learns the height rect.
        void MarkTerrainChanges(STerrainComponent& Terrain, ECS::FEntity Entity, bool bStructural, FTerrainPageStreamer& Streamer, FTerrainExtract& Out)
        {
            const FTerrainCPUState& CPU = Terrain.CPUState;
            const int32 Last = Terrain.Resolution - 1;

            Out.HeightChange = EHeightChange::None;
            if (CPU.bFullHeightmapDirty || bStructural)
            {
                Out.HeightChange = EHeightChange::Full;
            }
            else if (CPU.HeightDirtyMax.x >= CPU.HeightDirtyMin.x)
            {
                Out.HeightChange  = EHeightChange::Rect;
                Out.HeightRectMin = Math::Clamp(CPU.HeightDirtyMin, FIntVector2(0), FIntVector2(Last));
                Out.HeightRectMax = Math::Clamp(CPU.HeightDirtyMax, FIntVector2(0), FIntVector2(Last));
            }

            if (Out.HeightChange == EHeightChange::Full || CPU.bFullWeightsDirty)
            {
                Streamer.MarkAllStale(Entity);
                return;
            }
            if (Out.HeightChange == EHeightChange::Rect)
            {
                Streamer.MarkStale(Entity, Out.HeightRectMin, Out.HeightRectMax);
            }
            if (CPU.WeightDirtyLayerMask != 0u && CPU.WeightDirtyMax.x >= CPU.WeightDirtyMin.x)
            {
                Streamer.MarkStale(Entity, CPU.WeightDirtyMin, CPU.WeightDirtyMax);
            }
        }

        void PrepareTerrainExtract(STerrainComponent& Terrain, const FTerrainPageView& View, FTerrainPageStreamer& Streamer, FTerrainExtract& Out)
        {
            EnsureTerrainStore(Terrain);

            FTerrainCPUState& CPU = Terrain.CPUState;

            const int32 Res = Terrain.Resolution;

            // Render passes read this snapshot, not the component.
            Out.Resolution      = Res;
            Out.TileWorldSize   = Terrain.TileWorldSize;
            Out.MaxHeight       = Terrain.MaxHeight;
            Out.LayerCount      = Terrain.Layers.Num();
            Out.bCastShadow     = Terrain.bCastShadow;
            Out.bReceiveShadow  = Terrain.bReceiveShadow;

            PrepareTerrainMaterial(Terrain, Out);

            Out.HeightChange      = EHeightChange::None;
            Out.bStructuralChange = false;
            Out.Store = Terrain.Store;

            if (Res < 2)
            {
                return;
            }

            const bool bStructural = CPU.PreparedResolution != Res || CPU.PreparedLayerCount != Out.LayerCount;
            Out.bStructuralChange = bStructural;

            // An undo or a script write changes the store without marking a rect, so it counts as a whole new map.
            const uint64 EditSerial = Terrain.Store->GetEditSerial();
            const bool bMarked = CPU.bFullHeightmapDirty || CPU.bFullWeightsDirty || CPU.HeightDirtyMax.x >= CPU.HeightDirtyMin.x || CPU.WeightDirtyLayerMask != 0u;
            if (EditSerial != CPU.SeenEditSerial && !bMarked)
            {
                Terrain.MarkHeightmapReplaced();
                CPU.bFullWeightsDirty = true;
            }
            CPU.SeenEditSerial = EditSerial;

            Streamer.Prepare(Out.Entity, Res, Terrain.MaxHeight, Terrain.TileWorldSize);
            MarkTerrainChanges(Terrain, Out.Entity, bStructural, Streamer, Out);

            const FVector3 WorldOrigin = FVector3(Out.WorldMatrix[3]);
            FTerrainPageLayout Layout;
            Layout.OriginXZ      = FVector2(WorldOrigin.x - Terrain.TileWorldSize * 0.5f, WorldOrigin.z - Terrain.TileWorldSize * 0.5f);
            Layout.WorldY        = WorldOrigin.y;
            Layout.SampleSpacing = Terrain.TileWorldSize / float(Res - 1);
            Layout.MaxHeight     = Terrain.MaxHeight;
            Layout.Resolution    = Res;
            Streamer.Request(Out.Entity, Terrain.Store, Layout, View);

            // This frame captured the dirty state, so it is consumed here.
            CPU.bFullHeightmapDirty = false;
            CPU.bFullWeightsDirty   = false;
            CPU.HeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.HeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.WeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyLayerMask = 0u;
            CPU.PreparedResolution      = Res;
            CPU.PreparedLayerCount      = Out.LayerCount;
        }
    }

    void FDefaultSceneRenderer::ScheduleTerrainExtract(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        using namespace SceneRendererExtractTerrainInternal;

        LUMINA_PROFILE_SECTION("Prepare Terrain Extract");

        auto Transforms     = Registry.GetStorage<STransformComponent>();
        auto GrassStorage   = Registry.GetStorage<SGrassComponent>();
        auto TerrainAllView = Registry.View<ECS::Read<STerrainComponent>>();
        auto TerrainView    = Registry.View<ECS::Write<STerrainComponent>, ECS::Without<SDisabledTag>>();

        RefreshLiveEntities(TerrainAllView.GetDenseEntities(), Frame.Extracts.LiveTerrainEntities, TerrainDenseSnapshot);

        int32 TerrainCount = 0;
        TerrainView.ForEach([&](ECS::FEntity Entity, const STerrainComponent& Terrain)
        {
            FTerrainExtract& Item = ReuseAt(Frame.Extracts.TerrainExtracts, TerrainCount++);
            Item.Entity = Entity;
            PrepareGrassExtract(GrassStorage.TryGet(Entity), Terrain, Item);
        });
        Frame.Extracts.TerrainExtracts.SetNum(TerrainCount);

        if (TerrainCount == 0)
        {
            // The last terrain going still has to give back its pages.
            TerrainPageStreamer.Prune(Frame.Extracts.LiveTerrainEntities);
            return;
        }

        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, &Frame, TerrainView, Transforms]
        {
            ExtractTerrain(Frame, TerrainView, Transforms);
        });
    }

    void FDefaultSceneRenderer::ExtractTerrain(FFrameData& Frame, const auto& TerrainView, const auto& Transforms)
    {
        using namespace SceneRendererExtractTerrainInternal;

        LUMINA_PROFILE_SECTION("Extract Terrain");

        // Pages follow the cull camera, so a frozen cull keeps streaming for the view it was frozen at.
        const FCullData& Cull = Frame.SceneGlobalData.CullData;
        FTerrainPageView View;
        View.Camera        = FVector3(Cull.CullCameraPosition);
        View.PixelsPerUnit = Math::Abs(Cull.CullCameraProjection[1][1]) * 0.5f * float(Frame.SceneGlobalData.ScreenSize.y);
        View.bOrthographic = Cull.CullCameraProjection[3][3] != 0.0f;

        TerrainPageStreamer.BeginFrame();
        TerrainPageStreamer.Prune(Frame.Extracts.LiveTerrainEntities);
        for (FTerrainExtract& Item : Frame.Extracts.TerrainExtracts)
        {
            Item.WorldMatrix = Transforms.Get(Item.Entity).GetWorldMatrix();
            PrepareTerrainExtract(TerrainView.template Get<STerrainComponent>(Item.Entity), View, TerrainPageStreamer, Item);
        }
        TerrainPageStreamer.Commit();
    }
}
