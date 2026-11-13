#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"

namespace Lumina
{
    namespace SceneRendererExtractParticlesInternal
    {
        uint32 ResidentMeshletHeaderSlot(const CStaticMesh* Mesh)
        {
            return (Mesh != nullptr && Mesh->IsGeometryResident()) ? Mesh->GetMeshBuffers().MeshletHeaderSlot : 0u;
        }

        // Rigid, so the axes stay unit length and the entity's scale moves into the extent.
        FParticleShapeGPU MakeParticleShape(const STransformComponent& Transform, EParticleShapeType Shape, const FVector3& Extent)
        {
            const FMatrix4 World = Transform.GetWorldMatrixCached();
            const FVector3 Scale = Transform.GetWorldScaleCached();

            FParticleShapeGPU Out;
            Out.Center = FVector4(FVector3(World[3]), (float)Shape);
            Out.AxisX  = FVector4(Math::Normalize(FVector3(World[0])), 0.0f);
            Out.AxisY  = FVector4(Math::Normalize(FVector3(World[1])), 0.0f);
            Out.AxisZ  = FVector4(Math::Normalize(FVector3(World[2])), 0.0f);
            Out.Extent = Shape == EParticleShapeType::Sphere
                ? FVector4(Extent.x * Math::Max(Math::Max(Scale.x, Scale.y), Scale.z), 0.0f, 0.0f, 0.0f)
                : FVector4(Extent * Scale, 0.0f);
            Out.Params = FVector4(0.0f);
            return Out;
        }

        // A grid copy replaces only that emitter's last one, since a frame with no new copy keeps the old.
        void MergeGridSnapshots(SParticleSystemComponent& Component, TVector<FParticleGridSnapshot>& Grids)
        {
            const CParticleSystem* PS = Component.ParticleSystem.Get();
            for (FParticleGridSnapshot& Snapshot : Grids)
            {
                if (PS != nullptr && Snapshot.EmitterIndex < PS->Emitters.Num() && PS->Emitters[Snapshot.EmitterIndex] != nullptr)
                {
                    Snapshot.ChannelNames = PS->Emitters[Snapshot.EmitterIndex]->GridChannelNames;
                }

                auto Existing = Algo::FindIf(Component.GridSnapshots, [&](const FParticleGridSnapshot& Old) { return Old.EmitterIndex == Snapshot.EmitterIndex; });
                if (Existing != Component.GridSnapshots.end())
                {
                    *Existing = Move(Snapshot);
                }
                else
                {
                    Component.GridSnapshots.Add(Move(Snapshot));
                }
            }
        }

        constexpr uint32 ParticleSystemGrain = 8;
    }

    void FDefaultSceneRenderer::ScheduleParticleExtract(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SCOPE();
        auto Transforms      = Registry.GetStorage<STransformComponent>();
        auto AttractorView   = Registry.View<ECS::Read<SParticleAttractorComponent>, ECS::Without<SDisabledTag>>();
        auto ColliderView    = Registry.View<ECS::Read<SParticleColliderComponent>, ECS::Without<SDisabledTag>>();
        auto ParticleAllView = Registry.View<ECS::Read<SParticleSystemComponent>>();
        auto ParticleView    = Registry.View<ECS::Write<SParticleSystemComponent>, ECS::Without<SDisabledTag>>();

        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [=, this, &Frame]() -> Jobs::TTask<void>
        {

            co_await Jobs::Fork([this, &Frame, AttractorView, ColliderView, Transforms]
            {
                ExtractParticleFields(Frame, AttractorView, ColliderView, Transforms);
            });
            co_await Jobs::Call([this, &Frame, ParticleAllView, ParticleView, Transforms]
            {
                LUMINA_PROFILE_SECTION("Extract Particles");
                RefreshLiveEntities(ParticleAllView.GetDenseEntities(), Frame.Extracts.LiveParticleEntities, ParticleDenseSnapshot);
                ExtractParticles(Frame, ParticleView, Transforms);
            });
            co_await Jobs::Join();
        });
    }

    void FDefaultSceneRenderer::ExtractParticleFields(FFrameData& Frame, const auto& AttractorView, const auto& ColliderView, const auto& Transforms)
    {
        using namespace SceneRendererExtractParticlesInternal;

        LUMINA_PROFILE_SECTION("Extract Particle Fields");

        Frame.Extracts.ParticleAttractors.Reset();
        AttractorView.ForEach([&](ECS::FEntity Entity, const SParticleAttractorComponent& Attractor)
        {
            if (Attractor.bEnabled && Attractor.Strength != 0.0f)
            {
                FParticleShapeGPU& Shape = Frame.Extracts.ParticleAttractors.EmplaceGetRef(MakeParticleShape(Transforms.Get(Entity), Attractor.Shape, Attractor.Extent));
                Shape.Params = FVector4(Attractor.Strength, Attractor.Attenuation, Attractor.Directionality, 0.0f);
            }
        });

        Frame.Extracts.ParticleColliders.Reset();
        ColliderView.ForEach([&](ECS::FEntity Entity, const SParticleColliderComponent& Collider)
        {
            if (Collider.bEnabled)
            {
                Frame.Extracts.ParticleColliders.Add(MakeParticleShape(Transforms.Get(Entity), Collider.Shape, Collider.Extent));
            }
        });
    }

    void FDefaultSceneRenderer::ExtractParticles(FFrameData& Frame, const auto& ParticleView, const auto& Transforms)
    {
        using namespace SceneRendererExtractParticlesInternal;

        const int32 NumItems = PrepareParticleSystems(ParticleView);
        Frame.Extracts.ParticleExtracts.SetNum(NumItems);

        Jobs::ParallelFor((uint32)ParticleSystemWork.Num(), [&](const Jobs::FParallelRange& Range)
        {
            for (uint32 Index = Range.Start; Index < Range.End; ++Index)
            {
                const FParticleSystemWork& Work = ParticleSystemWork[Index];
                if (Work.Asset >= 0)
                {
                    FillParticleSystemExtracts(Frame, Work, Transforms.Get(Work.Entity).GetWorldMatrixCached());
                }
            }
        }, ParticleSystemGrain);

        WriteBackParticleComponents();
    }

    int32 FDefaultSceneRenderer::PrepareParticleSystems(const auto& ParticleView)
    {
        ParticleSystemWork.Reset();
        ParticleAssetStatics.Reset();
        ParticleAssetLookup.Reset();
        ParticleEmitterStatics.Reset();
        ParticleMaterialStatics.Reset();
        ParticleMaterialLookup.Reset();
        ParticleItemMaterials.Reset();

        int32 NumItems = 0;
        ParticleView.ForEach([&](ECS::FEntity Entity, ECS::TMut<SParticleSystemComponent> ComponentMut)
        {
            SParticleSystemComponent& Component = ComponentMut.BypassChangeDetection();
            FParticleSystemWork& Work = ParticleSystemWork.EmplaceGetRef();
            Work.Entity    = Entity;
            Work.Component = &Component;
            Work.FirstItem = NumItems;
            Work.Asset     = FindOrBuildParticleAssetStatics(Component.ParticleSystem.Get());
            if (Work.Asset < 0)
            {
                return;
            }

            const FParticleAssetStatics& Asset = ParticleAssetStatics[Work.Asset];
            for (int32 Local = 0; Local < Asset.NumEmitters; ++Local)
            {
                const FParticleEmitterStatics& Statics = ParticleEmitterStatics[Asset.FirstEmitter + Local];
                // Through the component, so a script's dynamic instance beats the asset's material.
                ParticleItemMaterials.Add(Statics.bReady ? FindOrResolveParticleMaterial(Component.GetMaterialForEmitter(Statics.EmitterIndex)) : -1);
            }
            NumItems += Asset.NumEmitters;
        });
        return NumItems;
    }

    int32 FDefaultSceneRenderer::FindOrBuildParticleAssetStatics(CParticleSystem* PS)
    {
        using namespace SceneRendererExtractParticlesInternal;

        if (PS == nullptr)
        {
            return -1;
        }
        if (const int32* Found = ParticleAssetLookup.Find(PS))
        {
            return *Found;
        }

        const int32 EmitterCount = PS->Emitters.Num();

        // A sub-emitter names its parent, so the parent learns here which event lists it has to raise.
        TInlineVector<int32, 8>  SourceIndices;
        TInlineVector<uint32, 8> RaisedMasks;
        TInlineVector<float, 8>  RaisedRates;
        SourceIndices.SetNum(EmitterCount, -1);
        RaisedMasks.SetNum(EmitterCount, 0u);
        RaisedRates.SetNum(EmitterCount, 0.0f);
        for (int32 Child = 0; Child < EmitterCount; ++Child)
        {
            const CParticleEmitter* ChildEmitter = PS->Emitters[Child].Get();
            if (ChildEmitter == nullptr || !ChildEmitter->bEnabled || ChildEmitter->SpawnFromEmitter.IsEmpty())
            {
                continue;
            }

            for (int32 Parent = 0; Parent < EmitterCount; ++Parent)
            {
                const CParticleEmitter* ParentEmitter = PS->Emitters[Parent].Get();
                if (Parent != Child && ParentEmitter != nullptr && ParentEmitter->EmitterName == ChildEmitter->SpawnFromEmitter)
                {
                    SourceIndices[Child] = Parent;
                    RaisedMasks[Parent] |= 1u << (uint32)ChildEmitter->SpawnEvent;
                    if (ChildEmitter->SpawnEvent == EParticleEventType::Continuous)
                    {
                        RaisedRates[Parent] = Math::Max(RaisedRates[Parent], ChildEmitter->EventRate);
                    }
                    break;
                }
            }
        }

        FParticleAssetStatics& Asset = ParticleAssetStatics.EmplaceGetRef();
        Asset.FirstEmitter = ParticleEmitterStatics.Num();
        Asset.EmitterCount = EmitterCount;

        for (int32 EmitterIdx = 0; EmitterIdx < EmitterCount; ++EmitterIdx)
        {
            CParticleEmitter* Emitter = PS->Emitters[EmitterIdx].Get();
            if (Emitter == nullptr || !Emitter->bEnabled)
            {
                continue;
            }

            FParticleEmitterStatics& Statics = ParticleEmitterStatics.EmplaceGetRef();
            Statics.Emitter            = Emitter;
            Statics.EmitterIndex       = EmitterIdx;
            Statics.SourceEmitterIndex = SourceIndices[EmitterIdx];
            Statics.RaisedEventMask    = RaisedMasks[EmitterIdx] | (Emitter->bReportCollisions ? 1u << (uint32)EParticleEventType::Collision : 0u);
            Statics.RaisedEventRate    = RaisedRates[EmitterIdx];
            Statics.MeshletHeaderSlot  = Emitter->RenderMode == EParticleRenderMode::Mesh ? ResidentMeshletHeaderSlot(Emitter->Mesh.Get()) : 0u;
            Statics.MeshletCount       = Statics.MeshletHeaderSlot != 0u ? Emitter->Mesh->GetMeshBuffers().MeshletCount : 0u;
            Statics.EmissionMeshSlot   = ResidentMeshletHeaderSlot(Emitter->EmissionMesh.Get());
            Statics.bReady             = Emitter->IsReadyForSimulation();
            Statics.TextureIndex       = 0u;
            Statics.GridTargetSlot     = RHI::kInvalidHeapSlot;
            Statics.GridTargetExtent   = FUIntVector2(0u);
            if (!Statics.bReady)
            {
                continue;
            }

            if (const CTexture* Tex = Emitter->Texture.Get())
            {
                const int32 CacheIdx = Tex->GetResourceID();
                if (CacheIdx > 0)
                {
                    Statics.TextureIndex = (uint32)CacheIdx;
                }
            }

            // Sized from the image itself, since an edited Width or Height waits for a rebuild to reach it.
            const CTextureRenderTarget* Target = Emitter->GridRenderTarget.Get();
            if (Emitter->UsesCustomShader() && Emitter->GridChannelCount > 0u && Target != nullptr && Target->GetTextureResource().NewTexture.IsValid())
            {
                const RHI::FManagedTexture& Image = Target->GetTextureResource().NewTexture;
                Statics.GridTargetSlot   = RHI::Textures::StorageSlot(Image, 0);
                Statics.GridTargetExtent = FUIntVector2(RHI::GetTextureDesc(Image.Texture).Dimension);
            }
        }
        Asset.NumEmitters = ParticleEmitterStatics.Num() - Asset.FirstEmitter;

        const int32 AssetIndex = ParticleAssetStatics.Num() - 1;
        ParticleAssetLookup.Add(PS, AssetIndex);
        return AssetIndex;
    }

    int32 FDefaultSceneRenderer::FindOrResolveParticleMaterial(CMaterialInterface* Candidate)
    {
        if (Candidate == nullptr)
        {
            return -1;
        }
        if (const int32* Found = ParticleMaterialLookup.Find(Candidate))
        {
            return *Found;
        }

        FParticleMaterialStatics& Material = ParticleMaterialStatics.EmplaceGetRef();
        if (CMaterialInterface* SpriteMaterial = ResolveParticleSpriteMaterial(Candidate))
        {
            if (SpriteMaterial->ResolveDomainShaders(EMaterialType::Particle, Material.VertexShader, Material.PixelShader))
            {
                Material.bResolved     = true;
                Material.MaterialIndex = SpriteMaterial->GetMaterialIndex();
                Material.BlendMode     = SpriteMaterial->GetBlendMode();
                Material.bWritesDepth  = SpriteMaterial->WritesDepth();

                // Demand only; an unresolved slot reads the placeholder rather than popping paths.
                SpriteMaterial->RequestTexturesResolved();
            }
        }

        const int32 MaterialIndex = ParticleMaterialStatics.Num() - 1;
        ParticleMaterialLookup.Add(Candidate, MaterialIndex);
        return MaterialIndex;
    }

    void FDefaultSceneRenderer::FillParticleSystemExtracts(FFrameData& Frame, const FParticleSystemWork& Work, const FMatrix4& WorldMatrix)
    {
        const SParticleSystemComponent& Component = *Work.Component;
        const FParticleAssetStatics&    Asset     = ParticleAssetStatics[Work.Asset];
        const bool                      bOnDrawnLayer = Frame.ViewLayers.Draws(Component.RenderLayers);

        for (int32 Local = 0; Local < Asset.NumEmitters; ++Local)
        {
            const int32 ItemIndex = Work.FirstItem + Local;
            FFrameData::FParticleExtract& Item = Frame.Extracts.ParticleExtracts[ItemIndex];

            Item.Entity              = Work.Entity;
            Item.EmitterCount        = Asset.EmitterCount;
            Item.WorldMatrix         = WorldMatrix;
            Item.bOnDrawnLayer       = bOnDrawnLayer;
            FillParticleExtract(Item, ParticleEmitterStatics[Asset.FirstEmitter + Local], Component, ParticleItemMaterials[ItemIndex]);
        }
    }

    // Writes every field, since the element is reused across frames.
    void FDefaultSceneRenderer::FillParticleExtract(FFrameData::FParticleExtract& Item, const FParticleEmitterStatics& Statics, const SParticleSystemComponent& Component, int32 MaterialSlot) const
    {
        const CParticleEmitter& Emitter = *Statics.Emitter;
        const int32 EmitterIdx = Statics.EmitterIndex;

        Item.EmitterIndex        = EmitterIdx;
        Item.EmitterOffset       = Component.EmitterOffset;
        Item.TimeScale           = Component.TimeScale;
        Item.SpawnRateMultiplier = Component.SpawnRateMultiplier;
        Item.bEmit               = Component.bEmit;
        Item.bBurstOnSpawn       = Component.bBurstOnSpawn;
        Item.bForceBurst         = Component.bForceBurst;
        Item.bForceReset         = Component.bForceReset;
        Item.MeshletHeaderSlot   = Statics.MeshletHeaderSlot;
        Item.MeshletCount        = Statics.MeshletCount;
        Item.EmissionMeshSlot    = Statics.EmissionMeshSlot;
        Item.SourceEmitterIndex  = Statics.SourceEmitterIndex;
        Item.bReportCollisions   = Emitter.bReportCollisions;
        Item.RaisedEventMask     = Statics.RaisedEventMask;
        Item.RaisedEventRate     = Statics.RaisedEventRate;

        Item.ScriptEmits.Reset();
        for (const FParticleScriptEmit& Emit : Component.PendingEmits)
        {
            if (Emit.EmitterIndex == EmitterIdx)
            {
                Item.ScriptEmits.Emplace(Emit.PositionSize, Emit.VelocityFlags, Emit.Color);
            }
        }

        Item.bReady               = Statics.bReady;
        Item.bUsesCustomShader    = false;
        Item.CustomComputeShader  = {};
        Item.TextureIndex         = Statics.TextureIndex;
        Item.MaterialVertexShader = {};
        Item.MaterialPixelShader  = {};
        Item.MaterialIndex        = -1;
        Item.AttributeFloatCount  = 1u;
        Item.Resolved             = FResolvedParticleParams{};
        Item.ModuleParamValues.Reset();
        Item.GridChannels         = 0u;
        Item.GridStageIterations.Reset();
        Item.GridTargetSlot       = RHI::kInvalidHeapSlot;
        Item.GridTargetExtent     = FUIntVector2(0u);
        Item.bReadBackGrid        = false;
        Item.ArrayData.Reset();
        Item.ArrayRanges.Reset();
        Item.NeighborResolution   = 0u;
        for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
        {
            Item.RenderAttrSlots[A] = -1;
        }

        if (!Statics.bReady)
        {
            return;
        }

        Item.Resolved          = ResolveParticleParams(*Component.ParticleSystem.Get(), Emitter, Component);
        Item.bUsesCustomShader = Emitter.UsesCustomShader();
        if (Item.bUsesCustomShader)
        {
            Item.CustomComputeShader = Emitter.GetCustomComputeShader();
            Item.ModuleParamValues.Append(Emitter.ModuleParamValues);
            ApplyParticleParamBindings(Emitter, Component, Item.ModuleParamValues);
            Item.AttributeFloatCount = Math::Max(Emitter.AttributeFloatCount, 1u);
            for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
            {
                Item.RenderAttrSlots[A] = Emitter.GetRenderAttributeSlot((ParticleRenderAttribute::Type)A);
            }

            for (const FName& ArrayName : Emitter.ArrayNames)
            {
                const auto Input = Algo::FindIf(Component.ArrayInputs, [&](const FParticleArrayInput& Candidate) { return Candidate.Name == ArrayName; });
                const uint32 Count = Input != Component.ArrayInputs.end() ? (uint32)Input->Values.Num() : 0u;
                Item.ArrayRanges.Emplace((uint32)Item.ArrayData.Num(), Count);
                if (Count > 0u)
                {
                    Item.ArrayData.Append(Input->Values);
                }
            }

            if (Emitter.bUsesNeighborGrid)
            {
                Item.NeighborResolution = (uint32)Math::Clamp(Emitter.NeighborGridResolution, 1, 64);
                Item.NeighborsPerCell   = (uint32)Math::Clamp(Emitter.NeighborsPerCell, 1, 32);
                Item.NeighborCellSize   = Math::Max(Emitter.NeighborCellSize, 0.01f);
            }

            Item.GridChannels = Emitter.GridChannelCount;
            if (Item.GridChannels > 0u)
            {
                Item.GridResolution      = FUIntVector2((uint32)Math::Clamp(Emitter.GridWidth, 1, ParticleGridMaxResolution),
                                                        (uint32)Math::Clamp(Emitter.GridHeight, 1, ParticleGridMaxResolution));
                Item.GridWorldSize       = Emitter.GridWorldSize;
                Item.bGridWraps          = Emitter.bGridWraps;
                Item.GridStageIterations.Append(Emitter.GridStageIterations);
                Item.bReadBackGrid       = Emitter.bReadBackGrid;
                Item.GridTargetSlot      = Statics.GridTargetSlot;
                Item.GridTargetExtent    = Statics.GridTargetExtent;
            }
        }

        if (MaterialSlot >= 0 && ParticleMaterialStatics[MaterialSlot].bResolved)
        {
            const FParticleMaterialStatics& Material = ParticleMaterialStatics[MaterialSlot];
            Item.MaterialVertexShader = Material.VertexShader;
            Item.MaterialPixelShader  = Material.PixelShader;
            Item.MaterialIndex        = Material.MaterialIndex;
            Item.MaterialBlendMode    = Material.BlendMode;
            Item.bMaterialWritesDepth = Material.bWritesDepth;
        }
    }

    // The gameplay writes extract still owes the particle components, kept in one place so a later phase can move them out.
    void FDefaultSceneRenderer::WriteBackParticleComponents()
    {
        using namespace SceneRendererExtractParticlesInternal;

        LUMINA_PROFILE_SECTION("Write Back Particle Components");

        const bool bAnyCollisions = !ParticleCollisionResults.IsEmpty();
        const bool bAnyGrids      = !ParticleGridResults.IsEmpty();

        for (const FParticleSystemWork& Work : ParticleSystemWork)
        {
            SParticleSystemComponent& Component = *Work.Component;

            Component.Collisions.Reset();
            if (bAnyCollisions)
            {
                if (TVector<SParticleCollision>* Hits = ParticleCollisionResults.Find(Work.Entity))
                {
                    Component.Collisions = Move(*Hits);
                }
            }

            if (bAnyGrids)
            {
                if (TVector<FParticleGridSnapshot>* Grids = ParticleGridResults.Find(Work.Entity))
                {
                    MergeGridSnapshots(Component, *Grids);
                }
            }

            Component.bForceBurst = false;
            Component.bForceReset = false;
            if (Component.ParticleSystem.Get() != nullptr)
            {
                Component.PendingEmits.Reset();
            }
        }

        // Whatever no component claimed belongs to one that is gone.
        ParticleCollisionResults.Reset();
        ParticleGridResults.Reset();
    }
}
