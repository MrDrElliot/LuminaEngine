#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractTerrainInternal
    {
        using FTerrainExtract       = FDefaultSceneRenderer::FFrameData::FTerrainExtract;
        using FGrassSpeciesExtract  = FDefaultSceneRenderer::FFrameData::FGrassSpeciesExtract;

        // Reuses the live element so its heap buffers outlive the frame instead of being freed and remade.
        template <typename T>
        T& ReuseAt(TVector<T>& Container, int32 Index)
        {
            return Index < Container.Num() ? Container[Index] : Container.EmplaceGetRef();
        }

        // Bilinear resample of a square row-major grid from OldRes^2 to NewRes^2.
        template <typename T>
        void ResampleGrid(const T* Src, int32 OldRes, T* Dst, int32 NewRes)
        {
            for (int32 Y = 0; Y < NewRes; ++Y)
            {
                const float Fy = (NewRes > 1) ? float(Y) / float(NewRes - 1) * float(OldRes - 1) : 0.0f;
                const int32 Y0 = int(Fy);
                const int32 Y1 = Math::Min(Y0 + 1, OldRes - 1);
                const float Ty = Fy - float(Y0);
                for (int32 X = 0; X < NewRes; ++X)
                {
                    const float Fx = (NewRes > 1) ? float(X) / float(NewRes - 1) * float(OldRes - 1) : 0.0f;
                    const int32 X0 = int(Fx);
                    const int32 X1 = Math::Min(X0 + 1, OldRes - 1);
                    const float Tx = Fx - float(X0);
                    const float V00 = float(Src[size_t(Y0) * OldRes + X0]);
                    const float V10 = float(Src[size_t(Y0) * OldRes + X1]);
                    const float V01 = float(Src[size_t(Y1) * OldRes + X0]);
                    const float V11 = float(Src[size_t(Y1) * OldRes + X1]);
                    const float V   = Math::Mix(Math::Mix(V00, V10, Tx), Math::Mix(V01, V11, Tx), Ty);
                    if constexpr (std::is_integral_v<T>)
                    {
                        Dst[size_t(Y) * NewRes + X] = T(Math::Clamp(V + 0.5f, 0.0f, 255.0f));
                    }
                    else
                    {
                        Dst[size_t(Y) * NewRes + X] = T(V);
                    }
                }
            }
        }

        void EnsureTerrainCpuBuffers(STerrainComponent& Terrain)
        {
            const int32 NewRes        = Terrain.Resolution;
            const int32 NeededHeights = NewRes * NewRes;
            if (Terrain.Heightmap.Num() != NeededHeights)
            {
                const int32 OldCount = Terrain.Heightmap.Num();
                const int32  OldRes   = (int32)std::llround(std::sqrt((double)OldCount));
                const bool   bResample = !Terrain.Heightmap.IsEmpty() && NewRes >= 2 && OldRes >= 2
                                       && size_t(OldRes) * size_t(OldRes) == static_cast<size_t>(OldCount);
                if (bResample)
                {
                    TVector<float> Resampled(NeededHeights);
                    ResampleGrid(Terrain.Heightmap.GetData(), OldRes, Resampled.GetData(), NewRes);
                    Terrain.Heightmap = Move(Resampled);

                    const int32 LayerCount = Terrain.Layers.Num();
                    if (LayerCount > 0 && static_cast<size_t>(Terrain.LayerWeights.Num()) == size_t(LayerCount) * OldCount)
                    {
                        TVector<uint8> NewWeights(size_t(LayerCount) * NeededHeights);
                        for (int32 L = 0; L < LayerCount; ++L)
                        {
                            ResampleGrid(Terrain.LayerWeights.GetData() + size_t(L) * OldCount, OldRes,
                                         NewWeights.GetData() + size_t(L) * NeededHeights, NewRes);
                        }
                        Terrain.LayerWeights = Move(NewWeights);
                        Terrain.CPUState.bFullWeightsDirty = true;
                    }
                }
                else
                {
                    Terrain.Heightmap.Init(0.0f, NeededHeights);
                }
                Terrain.CPUState.bFullHeightmapDirty = true;
            }
            const int32 NeededWeights = Terrain.Layers.Num() * NeededHeights;
            if (Terrain.LayerWeights.Num() != NeededWeights)
            {
                Terrain.LayerWeights.SetNum(NeededWeights, 0u);
                Terrain.CPUState.bFullWeightsDirty = true;
            }
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
                const CGrassType* Type = Output.GrassType.Get();
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
                Species.TypeName      = Type->GetName();
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
                Species.bRandomYaw    = Type->bRandomYaw;
            }
        }

        void PrepareTerrainMaterial(STerrainComponent& Terrain, FTerrainExtract& Out)
        {
            // Cleared up front because the element is reused across frames and these are written conditionally.
            Out.Shaders       = FRenderMaterialShaders{};
            Out.MaterialIndex = 0u;
            Out.bMasked       = false;

            // Anything that is not a ready terrain material, wrong domain included, falls back to the default.
            CMaterialInterface* TerrainMaterial = Terrain.Material.Get();
            FShaderH TerrainVS;
            FShaderH TerrainPS;
            if (TerrainMaterial == nullptr || !TerrainMaterial->ResolveDomainShaders(EMaterialType::Terrain, TerrainVS, TerrainPS))
            {
                TerrainMaterial = CMaterial::GetDefaultTerrainMaterial();
            }
            if (TerrainMaterial != nullptr && TerrainMaterial->ResolveDomainShaders(EMaterialType::Terrain, TerrainVS, TerrainPS))
            {
                Out.Shaders.VertexShader = TerrainVS;
                Out.Shaders.PixelShader  = TerrainPS;
                Out.MaterialIndex        = (uint32)Math::Max(TerrainMaterial->GetMaterialIndex(), 0);
                Out.bMasked              = TerrainMaterial->GetBlendMode() == EBlendMode::Masked;

                // Demand only, like particles, since terrain cannot simply disappear while its textures load.
                TerrainMaterial->RequestTexturesResolved();
            }
        }

        void PrepareTerrainUploads(STerrainComponent& Terrain, FTerrainExtract& Out, bool bStructural, bool& bOutRectHeight, FIntVector2& OutRectMin, FIntVector2& OutRectMax)
        {
            FTerrainCPUState& CPU = Terrain.CPUState;

            const int32 Res         = Terrain.Resolution;
            const int32 LayerCount  = Math::Max(Terrain.Layers.Num(), 1);
            const int32 SlicePixels = Res * Res;

            const bool bFullHeight = CPU.bFullHeightmapDirty || bStructural;
            const bool bRectHeight = !bFullHeight && (CPU.HeightDirtyMax.x >= CPU.HeightDirtyMin.x);
            bOutRectHeight = bRectHeight;

            OutRectMin = FIntVector2(0);
            OutRectMax = FIntVector2(Res - 1);
            if (bFullHeight && Terrain.Heightmap.Num() == SlicePixels)
            {
                Out.HeightUpload = 1;
                Out.HeightRectMin = FIntVector2(0);
                Out.HeightRectMax = FIntVector2(Res - 1);
                Out.HeightBytes.Reset();
                Out.HeightBytes.Append(Terrain.Heightmap.begin(), Terrain.Heightmap.end());
            }
            else if (bRectHeight && Terrain.Heightmap.Num() == SlicePixels)
            {
                OutRectMin = Math::Clamp(CPU.HeightDirtyMin, FIntVector2(0), FIntVector2(Res - 1));
                OutRectMax = Math::Clamp(CPU.HeightDirtyMax, FIntVector2(0), FIntVector2(Res - 1));
                const int32 RegionW = OutRectMax.x - OutRectMin.x + 1;
                const int32 RegionH = OutRectMax.y - OutRectMin.y + 1;
                Out.HeightUpload  = 2;
                Out.HeightRectMin = OutRectMin;
                Out.HeightRectMax = OutRectMax;
                Out.HeightBytes.SetNum(size_t(RegionW) * size_t(RegionH));
                for (int32 Row = 0; Row < RegionH; ++Row)
                {
                    const float* Src = Terrain.Heightmap.GetData() + size_t(OutRectMin.y + Row) * Res + OutRectMin.x;
                    Memory::Memcpy(Out.HeightBytes.GetData() + size_t(Row) * RegionW, Src, size_t(RegionW) * sizeof(float));
                }
            }

            // Weights upload as whole slices, matching the GPU upload granularity.
            if (Terrain.LayerWeights.IsEmpty())
            {
                return;
            }

            if (CPU.bFullWeightsDirty || bStructural)
            {
                uint32 Mask = 0u;
                for (int32 L = 0; L < LayerCount && (size_t(L + 1) * SlicePixels) <= static_cast<size_t>(Terrain.LayerWeights.Num()); ++L)
                {
                    Mask |= (1u << L);
                }
                if (Mask != 0u)
                {
                    Out.WeightUpload    = 1;
                    Out.WeightSliceMask = Mask;
                    // Only the present slices, back to back in ascending slice order.
                    for (int32 L = 0; L < LayerCount; ++L)
                    {
                        if ((Mask & (1u << L)) == 0u)
                        {
                            continue;
                        }
                        const uint8* Slice = Terrain.LayerWeights.GetData() + size_t(L) * SlicePixels;
                        Out.WeightBytes.Append(Slice, SlicePixels);
                    }
                }
            }
            else if (CPU.WeightDirtyLayerMask != 0u)
            {
                uint32 Mask = 0u;
                for (int32 L = 0; L < LayerCount; ++L)
                {
                    if ((CPU.WeightDirtyLayerMask & (1u << L)) == 0u || (size_t(L + 1) * SlicePixels) > static_cast<size_t>(Terrain.LayerWeights.Num()))
                    {
                        continue;
                    }
                    Mask |= (1u << L);
                    const uint8* Slice = Terrain.LayerWeights.GetData() + size_t(L) * SlicePixels;
                    Out.WeightBytes.Append(Slice, SlicePixels);
                }
                if (Mask != 0u)
                {
                    Out.WeightUpload    = 2;
                    Out.WeightSliceMask = Mask;
                }
            }
        }

        void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix, FTerrainExtract& Out)
        {
            EnsureTerrainCpuBuffers(Terrain);

            FTerrainCPUState& CPU = Terrain.CPUState;

            const int32 Res = Terrain.Resolution;

            // Render passes read this snapshot, not the component.
            Out.Resolution      = Res;
            Out.ChunkResolution = Terrain.ChunkResolution;
            Out.TileWorldSize   = Terrain.TileWorldSize;
            Out.MaxHeight       = Terrain.MaxHeight;
            Out.LayerCount      = Terrain.Layers.Num();
            Out.bCastShadow     = Terrain.bCastShadow;
            Out.bReceiveShadow  = Terrain.bReceiveShadow;

            PrepareTerrainMaterial(Terrain, Out);

            Out.HeightUpload      = 0;
            Out.WeightUpload      = 0;
            Out.WeightSliceMask   = 0u;
            Out.bGeometryRebuilt  = false;
            Out.bStructuralChange = false;
            Out.HeightBytes.Reset();
            Out.WeightBytes.Reset();
            Out.Chunks.Reset();
            Out.Meshlets.Reset();

            if (Res < 2 || Terrain.ChunkResolution < 2)
            {
                return;
            }

            const bool bStructural = CPU.PreparedResolution      != Res
                                  || CPU.PreparedChunkResolution != Terrain.ChunkResolution
                                  || CPU.PreparedLayerCount      != Out.LayerCount;
            Out.bStructuralChange = bStructural;

            bool        bRectHeight = false;
            FIntVector2 RectMin;
            FIntVector2 RectMax;
            PrepareTerrainUploads(Terrain, Out, bStructural, bRectHeight, RectMin, RectMax);

            const FVector3 WorldOrigin = FVector3(WorldMatrix[3]);
            const bool     bMoved      = !CPU.Chunks.IsEmpty() && WorldOrigin != CPU.PreparedOrigin && std::isfinite(WorldOrigin.x + WorldOrigin.y + WorldOrigin.z);
            if (CPU.bChunksDirty || bStructural || bMoved)
            {
                const bool bFullRebuild = bStructural || CPU.Chunks.IsEmpty() || (CPU.bChunksDirty && !bRectHeight);
                if (bFullRebuild)
                {
                    TerrainMeshletBuilder::Build(Terrain, WorldOrigin);
                }
                else
                {
                    if (bMoved)
                    {
                        TerrainMeshletBuilder::Translate(Terrain, WorldOrigin - CPU.PreparedOrigin);
                    }
                    if (CPU.bChunksDirty)
                    {
                        TerrainMeshletBuilder::UpdateRegion(Terrain, WorldOrigin, RectMin, RectMax);
                    }
                }

                if (!CPU.Chunks.IsEmpty() && !CPU.Meshlets.IsEmpty())
                {
                    Out.bGeometryRebuilt = true;
                    Out.Chunks.Reset();
                    Out.Chunks.Append(CPU.Chunks.begin(), CPU.Chunks.end());
                    Out.Meshlets.Reset();
                    Out.Meshlets.Append(CPU.Meshlets.begin(), CPU.Meshlets.end());
                }
            }

            // This frame captured the dirty state, so it is consumed here.
            CPU.bFullHeightmapDirty = false;
            CPU.bFullWeightsDirty   = false;
            CPU.bChunksDirty        = false;
            CPU.HeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.HeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.WeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyLayerMask = 0u;
            CPU.PreparedResolution      = Res;
            CPU.PreparedChunkResolution = Terrain.ChunkResolution;
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

        for (FTerrainExtract& Item : Frame.Extracts.TerrainExtracts)
        {
            Item.WorldMatrix = Transforms.Get(Item.Entity).GetWorldMatrixCached();
            PrepareTerrainExtract(TerrainView.template Get<STerrainComponent>(Item.Entity), Item.WorldMatrix, Item);
        }
    }
}
