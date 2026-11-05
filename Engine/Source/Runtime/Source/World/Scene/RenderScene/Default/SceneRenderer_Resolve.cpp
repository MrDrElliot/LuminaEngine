#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererResolveInternal
    {
        constexpr uint32 kValidationSliceSize = 256;
        constexpr uint32 kMaxMaterialChainDepth = 16;

        struct FResolveContext
        {
            FMeshResolveCache&              Cache;
            FMeshResolveQueue&              Queue;
            FRenderDirtyTracker&            Tracker;
            TVector<CMaterialInterface*>&   OverrideScratch;
            TVector<uint64>&                KeyScratch;
            uint32                          Epoch;
        };

        template <typename TComponent>
        struct TMeshSourceTraits;

        template <>
        struct TMeshSourceTraits<SStaticMeshComponent>
        {
            static constexpr EMeshResolveSource ResolveSource   = EMeshResolveSource::StaticMesh;
            static constexpr EPrimitiveSource   PrimitiveSource = EPrimitiveSource::StaticMesh;
            static constexpr EInstanceFlags     SeedFlags       = EInstanceFlags::None;

            // A static mesh swapped for one still uploading keeps drawing the old one, which a skeletal mesh cannot since its bones follow the new skeleton.
            static constexpr bool bKeepsPreviousMesh = true;

            static CMesh* GetMesh(const SStaticMeshComponent& Component) { return Component.StaticMesh.Get(); }
        };

        template <>
        struct TMeshSourceTraits<SSkeletalMeshComponent>
        {
            static constexpr EMeshResolveSource ResolveSource   = EMeshResolveSource::SkeletalMesh;
            static constexpr EPrimitiveSource   PrimitiveSource = EPrimitiveSource::SkeletalMesh;

            // Skeletal assets always carry FMeshletSkinnedVertex.
            static constexpr EInstanceFlags     SeedFlags       = EInstanceFlags::Skinned;
            static constexpr bool               bKeepsPreviousMesh = false;

            static CMesh* GetMesh(const SSkeletalMeshComponent& Component) { return Component.SkeletalMesh.Get(); }
        };

        template <typename TResolved>
        FORCEINLINE bool IsResolveCurrent(const TResolved& Resolved, const CMesh* Mesh, const FMeshResolveCache& Cache)
        {
            if (Resolved.CachedMeshKey != (const void*)Mesh || Resolved.CachedEntryState == MESH_RESOLVE_STATE_STALE)
            {
                return false;
            }

            if (Resolved.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE)
            {
                return Resolved.CachedEntryState == MESH_RESOLVE_STATE_NO_MESH;
            }

            return Resolved.CachedEntryState == Cache.GetEntryState(Resolved.ResolveHandle);
        }

        uint32 HashOverrides(const SMeshComponent& Component)
        {
            uint32 Hash = 2166136261u;
            for (const TStrongObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                const uint64 Bits = (uint64)(uintptr_t)Override.Get();
                for (uint32 b = 0; b < 8u; ++b)
                {
                    Hash ^= (uint32)((Bits >> (b * 8u)) & 0xFFull);
                    Hash *= 16777619u;
                }
            }
            return Hash | 1u;
        }

        void WatchHandle(FResolveContext& Context, ECS::FEntity Entity, EMeshResolveSource Source, uint32 Handle)
        {
            if (Handle == INVALID_MESH_RESOLVE_HANDLE)
            {
                Context.Queue.Unwatch(Entity, Source);
                return;
            }

            const uint64 Key = FMeshResolveQueue::HandleKey(Handle);
            Context.Queue.Watch(Entity, Source, TSpan<const uint64>(&Key, 1));
        }

        void AddMaterialChainKeys(const CMaterialInterface* Material, TVector<uint64>& Keys)
        {
            if (!IsValid(Material))
            {
                return;
            }

            if (const CMaterial* Concrete = Material->GetMaterial())
            {
                Keys.AddUnique(FMeshResolveQueue::DependencyKey(Concrete));
            }

            // The whole parent chain, since a mid-chain instance taking a new parent invalidates only itself.
            const CMaterialInterface* Link = Material;
            for (uint32 Depth = 0; IsValid(Link) && Depth < kMaxMaterialChainDepth; ++Depth)
            {
                Keys.AddUnique(FMeshResolveQueue::DependencyKey(Link));
                Link = Link->GetParentMaterial();
            }
        }

        // The overrides count even before the first commit, so a mesh whose materials compile first still hears them finish.
        void CollectDynamicMaterialKeys(const SDynamicMeshComponent& Component, const FDynamicMeshRenderData* Data, TVector<uint64>& Keys)
        {
            Keys.Reset();
            for (const TStrongObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                AddMaterialChainKeys(Override.Get(), Keys);
            }

            if (Data == nullptr)
            {
                return;
            }

            for (const FResolvedSurface& Surface : Data->Surfaces)
            {
                // The substituted material, which is what completes a surface drawing with the default.
                if (Surface.MaterialID != 0)
                {
                    Keys.AddUnique(FMeshResolveQueue::DependencyKey((const void*)(uintptr_t)Surface.MaterialID));
                }
                AddMaterialChainKeys(Surface.SourceMaterial, Keys);
            }
        }

        bool HasStaleSurface(const FDynamicMeshRenderData& Data)
        {
            for (const FResolvedSurface& Surface : Data.Surfaces)
            {
                if (MeshResolve::IsSurfaceStale(Surface))
                {
                    return true;
                }
            }
            return false;
        }

        template <typename TComponent>
        bool ShouldKeepPreviousMesh(const TComponent& Component, const CMesh* Mesh, uint32 Handle, const FMeshResolveCache& Cache)
        {
            const bool bNewReady = Handle != INVALID_MESH_RESOLVE_HANDLE && Cache.GetEntry(Handle).bResolved;
            const uint32 Shown = Component.ResolveHandle;
            const bool bShownReady = Shown != INVALID_MESH_RESOLVE_HANDLE && Cache.IsValidHandle(Shown) && Cache.GetEntry(Shown).bResolved;
            return Mesh != nullptr && !bNewReady && bShownReady && Component.CachedMeshKey != (const void*)Mesh;
        }

        // Copies the mesh-level values into the component so the cull path never reaches the asset, and reports whether they changed.
        template <typename TComponent>
        bool ResolveMeshComponent(FResolveContext& Context, TComponent& Component, CMesh* Mesh, uint32 OverrideHash, uint32& OutWatchedHandle)
        {
            using FTraits = TMeshSourceTraits<TComponent>;

            Context.OverrideScratch.Reset();
            for (const TStrongObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                Context.OverrideScratch.Add(Override.Get());
            }
            Component.CachedMaterialHash = OverrideHash;

            FMeshResolveCache& Cache = Context.Cache;
            const uint32 Handle = Cache.Resolve(Mesh, Context.OverrideScratch);

            // The new entry is the one whose completion matters, whether or not the component adopts it yet.
            OutWatchedHandle = Handle;

            if constexpr (FTraits::bKeepsPreviousMesh)
            {
                if (ShouldKeepPreviousMesh(Component, Mesh, Handle, Cache))
                {
                    Component.bShowingPreviousMesh = true;
                    Component.CachedEntryState     = MESH_RESOLVE_STATE_STALE;
                    return false;
                }
            }
            Component.bShowingPreviousMesh = false;

            Component.ResolveHandle = Handle;
            Component.CachedMeshKey = (const void*)Mesh;

            if (Handle == INVALID_MESH_RESOLVE_HANDLE)
            {
                Component.CachedLocalCenter       = FVector3(0.0f);
                Component.CachedLocalRadius       = 0.0f;
                Component.CachedMeshletHeaderSlot = 0;
                Component.CachedBaseFlags         = EInstanceFlags::None;

                // A null or destroyed mesh is settled, since only assigning another one can change it and that queues the component.
                Component.CachedEntryState = MESH_RESOLVE_STATE_NO_MESH;
                return true;
            }

            const FResolvedMesh& Entry = Cache.GetEntry(Handle);
            Component.CachedLocalCenter       = Entry.LocalCenter;
            Component.CachedLocalRadius       = Entry.LocalRadius;
            Component.CachedMeshletHeaderSlot = Entry.MeshletHeaderSlot;

            EInstanceFlags BaseFlags = FTraits::SeedFlags;
            if (Component.bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
            if (!Component.bReceiveDecals)         { BaseFlags |= EInstanceFlags::NoDecals; }
            if (Component.bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
            Component.CachedBaseFlags = BaseFlags;

            // An unready entry carries its own token too; the asset that completes it stales the entry and wakes its watchers.
            Component.CachedEntryState = Cache.GetEntryState(Handle);
            return true;
        }

        template <typename TComponent>
        uint32 RefreshMeshComponent(FResolveContext& Context, ECS::FEntity Entity, TComponent& Component)
        {
            using FTraits = TMeshSourceTraits<TComponent>;

            Component.ResolveBinding.Bind(Context.Queue, Entity, FTraits::ResolveSource);

            CMesh* Mesh = FTraits::GetMesh(Component);
            const uint32 OverrideHash = HashOverrides(Component);

            uint32 WatchedHandle = Component.ResolveHandle;
            bool bChanged = false;
            if (!IsResolveCurrent(Component, Mesh, Context.Cache) || Component.CachedMaterialHash != OverrideHash)
            {
                bChanged = ResolveMeshComponent(Context, Component, Mesh, OverrideHash, WatchedHandle);
            }

            WatchHandle(Context, Entity, FTraits::ResolveSource, WatchedHandle);

            if (!bChanged)
            {
                return 0u;
            }
            Context.Tracker.Mark(Entity, FTraits::PrimitiveSource, EPrimitiveDirty::Data);
            return 1u;
        }

        void ResolveFoliageType(FResolveContext& Context, SFoliageType& Type)
        {
            CStaticMesh* Mesh = Type.Mesh.Get();

            // Foliage types carry no material overrides.
            Context.OverrideScratch.Reset();
            const uint32 Handle = Context.Cache.Resolve(Mesh, Context.OverrideScratch);
            Type.ResolveHandle = Handle;
            Type.CachedMeshKey = (const void*)Mesh;

            if (Handle == INVALID_MESH_RESOLVE_HANDLE)
            {
                Type.CachedMeshletHeaderSlot = 0;
                Type.CachedBaseFlags         = EInstanceFlags::None;
                Type.CachedEntryState        = MESH_RESOLVE_STATE_NO_MESH;
                return;
            }

            const FResolvedMesh& Entry = Context.Cache.GetEntry(Handle);
            Type.CachedMeshletHeaderSlot = Entry.MeshletHeaderSlot;
            Type.CachedBaseFlags         = EInstanceFlags::Foliage | (Type.bReceiveShadow ? EInstanceFlags::ReceiveShadow : EInstanceFlags::None);
            Type.CachedEntryState        = Context.Cache.GetEntryState(Handle);
        }

        uint32 RefreshFoliage(FResolveContext& Context, ECS::FEntity Entity, SFoliageComponent& Foliage)
        {
            uint32 Refreshed = 0;
            Context.KeyScratch.Reset();
            for (SFoliageType& Type : Foliage.Types)
            {
                if (!IsResolveCurrent(Type, Type.Mesh.Get(), Context.Cache))
                {
                    ResolveFoliageType(Context, Type);
                    ++Refreshed;
                }

                if (Type.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE)
                {
                    Context.KeyScratch.AddUnique(FMeshResolveQueue::HandleKey(Type.ResolveHandle));
                }
            }

            Context.Queue.Watch(Entity, EMeshResolveSource::Foliage, Context.KeyScratch);

            // Every instance of a changed type re-binds, and an incomplete bake retries on the same mark.
            if (Refreshed != 0)
            {
                Context.Tracker.Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
            }
            return Refreshed;
        }

        // Dynamic meshes own no cache entry, so their materials are resolved per component and watched by material.
        uint32 RefreshDynamicMesh(FResolveContext& Context, ECS::FEntity Entity, SDynamicMeshComponent& Component)
        {
            Component.ResolveBinding.Bind(Context.Queue, Entity, EMeshResolveSource::DynamicMesh);

            // Stale is what InvalidateRenderResolve leaves, so a script or editor write to the shared fields reaches the scene.
            bool bChanged = Component.CachedEntryState == MESH_RESOLVE_STATE_STALE;
            Component.CachedEntryState = MESH_RESOLVE_STATE_NO_MESH;

            bChanged |= Component.LoadRenderDataVersion() != Component.ResolvedRenderDataVersion;

            TSharedPtr<FDynamicMeshRenderData> Data = Component.LoadRenderData();
            if (Data)
            {
                const uint32 OverrideHash = HashOverrides(Component);
                if (OverrideHash != Component.CachedMaterialHash
                    || Context.Epoch != Component.CachedResolveEpoch
                    || !Data->bAllMaterialsReady
                    || HasStaleSurface(*Data))
                {
                    // Republishes a copy, so the version below already covers the refresh.
                    Component.RefreshResolvedMaterials();
                    Data = Component.LoadRenderData();
                    Component.CachedMaterialHash = OverrideHash;
                    Component.CachedResolveEpoch = Context.Epoch;
                    bChanged = true;
                }
            }
            Component.ResolvedRenderDataVersion = Component.LoadRenderDataVersion();

            CollectDynamicMaterialKeys(Component, Data.get(), Context.KeyScratch);
            Context.Queue.Watch(Entity, EMeshResolveSource::DynamicMesh, Context.KeyScratch);

            if (!bChanged)
            {
                return 0u;
            }
            Context.Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);
            return 1u;
        }

        template <typename TComponent, typename TRefresh>
        uint32 ResolveSource(FResolveContext& Context, ECS::FRegistry& Registry, EMeshResolveSource Source,
                             TVector<ECS::FEntity>& DirtyScratch, TRefresh&& Refresh)
        {
            Context.Queue.TakeDirty(Source, DirtyScratch);
            const bool bSweep = Context.Queue.ConsumeFullSweep(Source);

            const ECS::TComponentStorage<TComponent> Storage = Registry.FindStorage<TComponent>();
            if (!Storage)
            {
                return 0u;
            }

            uint32 Refreshed = 0;
            if (bSweep)
            {
                const ECS::FEntity* Dense = Storage.GetDenseData();
                for (uint32 i = 0, Count = (uint32)Storage.GetDenseSize(); i < Count; ++i)
                {
                    if (!Dense[i].IsTombstone())
                    {
                        Refreshed += Refresh(Context, Dense[i], Storage.GetAtDense(i));
                    }
                }
                return Refreshed;
            }

            for (ECS::FEntity Entity : DirtyScratch)
            {
                if (TComponent* Component = Storage.TryGet(Entity))
                {
                    Refreshed += Refresh(Context, Entity, *Component);
                }
            }
            return Refreshed;
        }

        // Workers only read the table, so every species mesh the terrain extract will look up is resolved here first.
        void PreResolveGrassMeshes(FResolveContext& Context, ECS::FRegistry& Registry)
        {
            const ECS::TComponentStorage<STerrainComponent> Terrains = Registry.FindStorage<STerrainComponent>();
            if (!Terrains)
            {
                return;
            }

            const ECS::FEntity* Dense = Terrains.GetDenseData();
            for (uint32 i = 0, Count = (uint32)Terrains.GetDenseSize(); i < Count; ++i)
            {
                const SGrassComponent* Grass = Dense[i].IsTombstone() ? nullptr : Registry.TryGet<ECS::Read<SGrassComponent>>(Dense[i]);
                if (Grass == nullptr || !Grass->bEnabled)
                {
                    continue;
                }

                CMaterialInterface* MaterialInterface = Terrains.GetAtDense(i).Material.Get();
                const CMaterial* Material = MaterialInterface != nullptr ? MaterialInterface->GetMaterial() : nullptr;
                if (Material == nullptr)
                {
                    continue;
                }

                for (const SGrassOutput& Output : Material->GrassOutputs)
                {
                    const CGrassType* Type = Output.GrassType.Get();
                    if (Type != nullptr && Type->Mesh != nullptr)
                    {
                        Context.OverrideScratch.Reset();
                        (void)Context.Cache.Resolve(Type->Mesh.Get(), Context.OverrideScratch);
                    }
                }
            }
        }

        // A component still stale after the pass changed through a path that never queued it.
        template <typename TComponent>
        bool IsMissedMeshResolve(const FMeshResolveQueue& Queue, const FMeshResolveCache& Cache, ECS::FEntity Entity, const TComponent& Component)
        {
            using FTraits = TMeshSourceTraits<TComponent>;

            // Waiting on the new mesh's upload is stale on purpose, and a queued one is next frame's work.
            if (Component.bShowingPreviousMesh || Component.ResolveBinding.IsQueued())
            {
                return false;
            }

            if (!IsResolveCurrent(Component, FTraits::GetMesh(Component), Cache) || Component.CachedMaterialHash != HashOverrides(Component))
            {
                return true;
            }

            return Component.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE
                && !Queue.IsWatching(Entity, FTraits::ResolveSource, FMeshResolveQueue::HandleKey(Component.ResolveHandle));
        }

        bool IsMissedDynamicResolve(const SDynamicMeshComponent& Component, const FDynamicMeshRenderData* Data)
        {
            if (Component.ResolveBinding.IsQueued())
            {
                return false;
            }

            return Component.LoadRenderDataVersion() != Component.ResolvedRenderDataVersion
                || (Data != nullptr && Component.CachedMaterialHash != HashOverrides(Component));
        }
    }

    void FDefaultSceneRenderer::ResolveDynamicMeshMaterials(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        using namespace SceneRendererResolveInternal;

        LUMINA_PROFILE_SCOPE();

        FMeshResolveQueue& Queue = FMeshResolveQueue::Ensure(Registry);
        Queue.BeginFrame();

        FResolveContext Context{ FMeshResolveCache::Get(), Queue, Tracker, ResolveOverrideScratch, ResolveKeyScratch, FMeshResolveCache::GetEpoch() };

        const uint32 Refreshed = ResolveSource<SDynamicMeshComponent>(Context, Registry, EMeshResolveSource::DynamicMesh, ResolveDirtyScratch,
            [](FResolveContext& InContext, ECS::FEntity Entity, SDynamicMeshComponent& Component)
            {
                return RefreshDynamicMesh(InContext, Entity, Component);
            });

        LUMINA_PROFILE_VALUE("Resolve/DynamicRefreshed", Refreshed);
    }

    void FDefaultSceneRenderer::ResolveDirtyMeshComponents()
    {
        using namespace SceneRendererResolveInternal;

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        FMeshResolveQueue& Queue = FMeshResolveQueue::Ensure(Registry);
        Queue.BeginFrame();

        FMeshResolveCache& Cache = FMeshResolveCache::Get();
        FResolveContext Context{ Cache, Queue, FRenderDirtyTracker::Ensure(Registry), ResolveOverrideScratch, ResolveKeyScratch, FMeshResolveCache::GetEpoch() };

        const uint32 TableGenerationBefore = Cache.GetTableGeneration();

        PreResolveGrassMeshes(Context, Registry);

        uint32 Refreshed = ResolveSource<SStaticMeshComponent>(Context, Registry, EMeshResolveSource::StaticMesh, ResolveDirtyScratch,
            [](FResolveContext& InContext, ECS::FEntity Entity, SStaticMeshComponent& Component)
            {
                return RefreshMeshComponent(InContext, Entity, Component);
            });

        Refreshed += ResolveSource<SSkeletalMeshComponent>(Context, Registry, EMeshResolveSource::SkeletalMesh, ResolveDirtyScratch,
            [](FResolveContext& InContext, ECS::FEntity Entity, SSkeletalMeshComponent& Component)
            {
                return RefreshMeshComponent(InContext, Entity, Component);
            });

        Refreshed += ResolveSource<SFoliageComponent>(Context, Registry, EMeshResolveSource::Foliage, ResolveDirtyScratch,
            [](FResolveContext& InContext, ECS::FEntity Entity, SFoliageComponent& Foliage)
            {
                return RefreshFoliage(InContext, Entity, Foliage);
            });

        const uint32 EntriesRebuilt = Cache.GetTableGeneration() - TableGenerationBefore;
        if (EntriesRebuilt != 0)
        {
            ScenePrimitives.NotifyResolveTableChanged();
        }

        ValidateNoStaleResolves(Registry);

        LUMINA_PROFILE_VALUE("Resolve/ComponentsRefreshed", Refreshed);
        LUMINA_PROFILE_VALUE("Resolve/EntriesRebuilt",      EntriesRebuilt);
    }

    void FDefaultSceneRenderer::SettleResolveWork(int32 MaxIterations)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);

        for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
        {
            ResolveDynamicMeshMaterials(Registry, Tracker);
            ResolveDirtyMeshComponents();
            if (!FMeshResolveQueue::Ensure(Registry).HasPendingWork())
            {
                return;
            }
        }
    }

    // A slice a frame, one source after another, so a big world never spikes; editor-only.
    void FDefaultSceneRenderer::ValidateNoStaleResolves(ECS::FRegistry& Registry)
    {
#if USING(WITH_EDITOR)
        using namespace SceneRendererResolveInternal;

        FMeshResolveQueue* Queue = FMeshResolveQueue::Find(Registry);
        if (Queue == nullptr)
        {
            return;
        }

        const FMeshResolveCache& Cache = FMeshResolveCache::Get();

        // Heals what it finds, so a missed path costs a few frames rather than a mesh that never updates.
        auto ValidateSlice = [&]<typename TComponent>(EMeshResolveSource Source, auto&& IsMissed) -> bool
        {
            const ECS::TComponentStorage<TComponent> Storage = Registry.FindStorage<TComponent>();
            const uint32 Count = Storage ? (uint32)Storage.GetDenseSize() : 0u;
            const uint32 SliceEnd = Math::Min(Count, StaleValidationCursor + kValidationSliceSize);
            for (uint32 i = StaleValidationCursor; i < SliceEnd; ++i)
            {
                const ECS::FEntity Entity = Storage.GetDenseData()[i];
                if (Entity.IsTombstone())
                {
                    continue;
                }

                TComponent& Component = Storage.GetAtDense(i);
                if (IsMissed(Entity, Component))
                {
                    Component.ResolveBinding.Bind(*Queue, Entity, Source);
                    Component.InvalidateRenderResolve();
                }
            }

            StaleValidationCursor = SliceEnd;
            return SliceEnd >= Count;
        };

        auto CountMiss = [this](bool bMissed)
        {
            StaleValidationMissed += bMissed ? 1u : 0u;
            return bMissed;
        };

        bool bSourceDone = true;
        switch ((EMeshResolveSource)StaleValidationSource)
        {
        case EMeshResolveSource::StaticMesh:
            bSourceDone = ValidateSlice.operator()<SStaticMeshComponent>(EMeshResolveSource::StaticMesh,
                [&](ECS::FEntity Entity, const SStaticMeshComponent& Component) { return CountMiss(IsMissedMeshResolve(*Queue, Cache, Entity, Component)); });
            break;

        case EMeshResolveSource::SkeletalMesh:
            bSourceDone = ValidateSlice.operator()<SSkeletalMeshComponent>(EMeshResolveSource::SkeletalMesh,
                [&](ECS::FEntity Entity, const SSkeletalMeshComponent& Component) { return CountMiss(IsMissedMeshResolve(*Queue, Cache, Entity, Component)); });
            break;

        case EMeshResolveSource::DynamicMesh:
            // Material wakes applied after this frame's dynamic pass are queued for the next one, so they are not misses.
            if (Queue->HasDirty(EMeshResolveSource::DynamicMesh))
            {
                return;
            }

            bSourceDone = ValidateSlice.operator()<SDynamicMeshComponent>(EMeshResolveSource::DynamicMesh,
                [&](ECS::FEntity, const SDynamicMeshComponent& Component)
                {
                    const TSharedPtr<FDynamicMeshRenderData> Data = Component.LoadRenderData();

                    uint32 Stale = 0;
                    if (Data)
                    {
                        for (const FResolvedSurface& Surface : Data->Surfaces)
                        {
                            Stale += MeshResolve::IsSurfaceStale(Surface) ? 1u : 0u;
                        }
                    }
                    StaleValidationSurfaces += Stale;
                    StaleValidationEntities += (Stale > 0) ? 1u : 0u;

                    return CountMiss(IsMissedDynamicResolve(Component, Data.get())) || Stale > 0;
                });
            break;

        default:
            break;
        }

        if (!bSourceDone)
        {
            return;
        }

        StaleValidationCursor = 0;
        ++StaleValidationSource;
        if (StaleValidationSource < (uint8)EMeshResolveSource::Foliage)
        {
            return;
        }
        StaleValidationSource = 0;

        if (StaleValidationSurfaces > 0)
        {
            LOG_WARN("MeshResolve: {} surface(s) across {} dynamic mesh entities were still resolved against superseded "
                     "shaders after a resolve pass. Something recompiled without invalidating its material.",
                     StaleValidationSurfaces, StaleValidationEntities);
        }
        if (StaleValidationMissed > 0)
        {
            LOG_WARN("MeshResolve: {} mesh component(s) changed without reaching the resolve queue and were queued by the "
                     "validator. Call MarkRenderStateDirty after a raw field write, or Patch the component.",
                     StaleValidationMissed);
        }
        StaleValidationSurfaces = 0;
        StaleValidationEntities = 0;
        StaleValidationMissed   = 0;
#else
        (void)Registry;
#endif
    }
}
