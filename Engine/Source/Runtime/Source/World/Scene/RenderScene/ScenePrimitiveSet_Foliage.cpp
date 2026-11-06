#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "ScenePrimitiveSetInternal.h"

namespace Lumina
{
    FInstanceCullEntry FScenePrimitiveSet::MakeFoliageCull(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type, const FVector4& WorldSphere, bool bHidden)
    {
        const EInstanceFlags Flags = ScenePrimitiveSetInternal::ComposeInstanceFlags(Type.BaseFlags, Binding, Type.bCastShadow, Type.bCastFarShadow,
                                                                                     Type.Surfaces != nullptr && !bHidden,
                                                                                     Type.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot);
        FInstanceCullEntry Cull = {};
        // The bake already produced the world sphere; there is no local sphere to transform.
        Cull.SphereBounds     = WorldSphere;
        Cull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
        Cull.SurfaceDescIndex = Binding.SurfaceDescIndex;
        Cull.MaxDrawDistance  = Type.MaxDrawDistance;
        Cull.LODAndThinning   = PackLODAndThinning(-1, Type.ThinningStartDistance);
        return Cull;
    }

    FInstanceProxy FScenePrimitiveSet::MakeFoliageProxy(const FFoliageTypeResolve& Type, uint32 SurfaceIndex)
    {
        if (Type.Surfaces == nullptr || SurfaceIndex >= (uint32)Type.Surfaces->Num())
        {
            return FInstanceProxy{};
        }
        FInstanceProxy Proxy = (*Type.Surfaces)[SurfaceIndex].Proxy;
        Proxy.ThinningStartDistance = Type.ThinningStartDistance;
        return Proxy;
    }

    FInstanceStatic FScenePrimitiveSet::MakeFoliageStatic(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type, uint32 EntityID, uint32 MeshletVisibilityBase)
    {
        FInstanceStatic Static = {};
        Static.MeshletHeaderSlot     = Type.MeshletHeaderSlot;
        Static.MaterialIndex         = Binding.MaterialIndex;
        Static.EntityID              = EntityID;
        Static.SkinnedIndex          = kNoSkinnedIndex;
        Static.MeshletVisibilityBase = MeshletVisibilityBase;
        return Static;
    }

    void FScenePrimitiveSet::ReleaseFoliageInstance(FFoliageInstanceRef& Ref, TVector<FUIntVector3>* DeferredBatchReleases)
    {
        ReleaseBindingSpan(Ref.BindingBase, Ref.SurfaceCount, DeferredBatchReleases);
        Ref.BindingBase  = 0;
        Ref.SurfaceCount = 0;
        Ref.TypeRow      = 0xFFFFu;
    }

    void FScenePrimitiveSet::RemoveFoliageEntity(ECS::FEntity Entity)
    {
        FFoliageEntityState* State = FoliageByEntity.Find(Entity);
        if (State == nullptr)
        {
            return;
        }
        FoliageInstanceCount -= (uint32)State->Instances.Num();
        PendingFoliageReleases.Add(Move(*State));
        FoliageByEntity.Remove(Entity);
    }

    // In place, so the slots keep the transform and sphere they hold; the new batch is referenced before the old one is let go.
    void FScenePrimitiveSet::PatchBindingSpan(uint32 Base, const FBindingMemo& Memo, bool bSkinned, TVector<FUIntVector3>& DeferredBatchReleases)
    {
        for (int32 s = 0; s < Memo.Protos.Num(); ++s)
        {
            FSurfaceBinding&       Binding = Bindings[Base + s];
            const FSurfaceBinding& Proto   = Memo.Protos[s];
            if (Binding.BatchIndex != Proto.BatchIndex || Binding.bSkinned != bSkinned)
            {
                Batches.AddBatchRef(Proto.BatchIndex, bSkinned);
                ScenePrimitiveSetInternal::DeferBatchRelease(DeferredBatchReleases, Binding.BatchIndex, Binding.bSkinned);
            }
            const uint32 Slot         = Binding.InstanceSlot;
            const uint32 SkinnedIndex = Binding.SkinnedIndex;
            Binding              = Proto;
            Binding.InstanceSlot = Slot;
            Binding.bSkinned     = bSkinned;
            Binding.SkinnedIndex = SkinnedIndex;
        }
    }

    void FScenePrimitiveSet::SyncFoliage(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity)
    {
        ++SyncStats.SyncFoliageCalls;

        SFoliageComponent* Foliage = Registry.IsValid(Entity) ? Pools.Foliage->TryGet(Entity) : nullptr;
        if (Foliage == nullptr || Pools.Disabled->Contains(Entity))
        {
            RemoveFoliageEntity(Entity);
            return;
        }

        FFoliageEntityState* Existing = FoliageByEntity.Find(Entity);
        const bool bFirstSync = Existing == nullptr;
        FFoliageEntityState& State = bFirstSync ? FoliageByEntity.FindOrAdd(Entity) : *Existing;
        const uint32 PreviousCount = (uint32)State.Instances.Num();

        // Only an edit, an unfinished bake or a reshaped type table changes what the bake holds, so a resolve bump alone patches in place.
        const bool bTypeLayoutChanged = ResolveFoliageTypes(State, *Foliage, Pools, Entity);
        const bool bContentChanged    = bFirstSync || bTypeLayoutChanged || State.SyncedVersion != Foliage->InstancesVersion || Foliage->bBakeIncomplete;

        const TVector<FFoliageBakedInstance>* Baked = bContentChanged ? &BakeFoliage(State, *Foliage) : nullptr;

        if (Baked != nullptr && PreviousCount == 0u && (uint32)Baked->Num() >= ScenePrimitiveSetInternal::kFreshFoliageParallelThreshold)
        {
            BindFreshFoliage(State, *Foliage, *Baked, Entity.Value);
        }
        else
        {
            if (!RebindFoliageInstances(State, Baked, bContentChanged))
            {
                Baked = &BakeFoliage(State, *Foliage);
                RebindFoliageInstances(State, Baked, false);
            }
            RefreshFoliageInstances(State, *Foliage, Baked, Entity.Value);
        }
        FlushDeferredBatchReleases(DeferredBatchReleaseScratch);

        // Every slot now holds its transform and bounds, so a second copy per instance is hundreds of megabytes in a large field.
        if (Baked != nullptr)
        {
            State.SyncedVersion = Foliage->BakedVersion;
            Foliage->ReleaseRenderCache();
        }

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }
    }

    const TVector<FFoliageBakedInstance>& FScenePrimitiveSet::BakeFoliage(FFoliageEntityState& State, SFoliageComponent& Foliage)
    {
        Foliage.EnsureRenderCache();

        const uint32 NewCount = (uint32)Foliage.BakedInstances.Num();
        const uint32 OldCount = (uint32)State.Instances.Num();
        for (uint32 i = NewCount; i < OldCount; ++i)
        {
            ReleaseFoliageInstance(State.Instances[i], &DeferredBatchReleaseScratch);
        }
        State.Instances.SetNum(NewCount);
        FoliageInstanceCount = FoliageInstanceCount - OldCount + NewCount;
        return Foliage.BakedInstances;
    }

    bool FScenePrimitiveSet::ResolveFoliageTypes(FFoliageEntityState& State, const SFoliageComponent& Foliage, const FSyncPools& Pools, ECS::FEntity Entity)
    {
        const uint32 TypeCount = (uint32)Foliage.Types.Num();
        FoliageTypeScratch.Reset();
        FoliageTypeScratch.SetNum(TypeCount);

        const bool bTypeLayoutChanged = (uint32)State.TypeResolveKeys.Num() != TypeCount;
        if (bTypeLayoutChanged)
        {
            State.TypeResolveKeys.Init(PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0), TypeCount);
        }

        FMeshResolveCache& Cache = *Pools.ResolveCache;
        for (uint32 t = 0; t < TypeCount; ++t)
        {
            const SFoliageType&  Type = Foliage.Types[t];
            FFoliageTypeResolve& Out  = FoliageTypeScratch[t];

            Out.MeshletHeaderSlot     = Type.CachedMeshletHeaderSlot;
            Out.BaseFlags             = Type.CachedBaseFlags;
            Out.ResolveHandle         = Type.ResolveHandle;
            Out.bCastShadow           = Type.bCastShadow;
            Out.bCastFarShadow        = Type.bCastFarShadow;
            Out.ThinningStartDistance = Type.ThinningStartDistance;
            Out.MaxDrawDistance       = Type.CullDistance;

            if (Type.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE
                && Cache.IsValidHandle(Type.ResolveHandle)
                && Type.CachedMeshKey == (const void*)Type.Mesh.Get())
            {
                const FResolvedMesh& Entry = Cache.GetEntry(Type.ResolveHandle);
                if (Entry.bResolved)
                {
                    Out.Surfaces   = &Entry.Surfaces;
                    Out.Generation = Entry.Generation;
                }
            }

            // Compared per type by the sweep, which is what the per-instance table could not express.
            const uint64 NewKey = PackResolveKey(Out.ResolveHandle, Out.Generation);
            Out.bChanged = bTypeLayoutChanged || State.TypeResolveKeys[t] != NewKey;
            State.TypeResolveKeys[t] = NewKey;

            // Per type rather than per instance; every instance of a type carries the same pair.
            ScenePrimitiveSetInternal::WarnOnGeometryKindMismatch(Out.MeshletHeaderSlot, Out.BaseFlags, Entity.Value, EPrimitiveSource::Foliage);
        }

        // Every instance of a type binds identical protos, so a field of foliage costs a handful of memo lookups.
        for (const FFoliageTypeResolve& Type : FoliageTypeScratch)
        {
            if (Type.Surfaces != nullptr && !Type.Surfaces->IsEmpty())
            {
                EnsureBindingMemo(Type.ResolveHandle, Type.Generation, *Type.Surfaces);
            }
        }

        // Addresses are taken once every memo exists, since EnsureBindingMemo grows the table a pointer would point into.
        for (FFoliageTypeResolve& Type : FoliageTypeScratch)
        {
            if (Type.Surfaces != nullptr && !Type.Surfaces->IsEmpty()
                && Type.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE
                && Type.ResolveHandle < (uint32)BindingMemoByHandle.Num())
            {
                Type.Memo = &BindingMemoByHandle[Type.ResolveHandle];
            }
        }

        return bTypeLayoutChanged;
    }

    bool FScenePrimitiveSet::RebindFoliageInstances(FFoliageEntityState& State, const TVector<FFoliageBakedInstance>* Baked, bool bContentChanged)
    {
        FoliageRefreshScratch.Reset();

        const int32 TypeCount = FoliageTypeScratch.Num();
        for (uint32 i = 0, Count = (uint32)State.Instances.Num(); i < Count; ++i)
        {
            FFoliageInstanceRef& Ref = State.Instances[i];

            // Without a bake the content is unchanged, so every instance keeps the type it was bound to.
            const int32  TypeIndex  = Baked != nullptr ? (*Baked)[i].TypeIndex : (Ref.TypeRow == 0xFFFFu ? -1 : (int32)Ref.TypeRow);
            const bool   bValidType = TypeIndex >= 0 && TypeIndex < TypeCount;
            const uint16 TypeRow    = bValidType ? (uint16)TypeIndex : 0xFFFFu;

            if (!bContentChanged && Ref.TypeRow == TypeRow && (!bValidType || !FoliageTypeScratch[TypeRow].bChanged))
            {
                ++SyncStats.BindsSkipped;
                continue;
            }

            const FFoliageTypeResolve* Type = bValidType ? &FoliageTypeScratch[TypeRow] : nullptr;
            const FBindingMemo*        Memo = Type != nullptr ? Type->Memo : nullptr;
            const uint32          WantCount = Memo != nullptr ? (uint32)Memo->Protos.Num() : 0u;
            const bool             bSkinned = Type != nullptr && EnumHasAnyFlags(Type->BaseFlags, EInstanceFlags::Skinned);

            if (Ref.TypeRow == TypeRow && Ref.SurfaceCount == WantCount)
            {
                if (SpanMatchesMemo(Ref.BindingBase, WantCount, Memo, bSkinned))
                {
                    ++SyncStats.BindsSkipped;
                }
                else
                {
                    ++SyncStats.BindCalls;
                    PatchBindingSpan(Ref.BindingBase, *Memo, bSkinned, DeferredBatchReleaseScratch);
                }
            }
            else
            {
                // New slots start empty, and only the bake holds what to fill them with.
                if (WantCount != 0u && Baked == nullptr)
                {
                    return false;
                }
                ++SyncStats.BindCalls;
                const uint32 NewBase = WantCount != 0u ? AppendBindingSpan(*Memo, bSkinned, false) : 0u;
                ReleaseFoliageInstance(Ref, &DeferredBatchReleaseScratch);
                Ref.BindingBase  = NewBase;
                Ref.SurfaceCount = (uint16)WantCount;
                Ref.TypeRow      = TypeRow;
            }

            FoliageRefreshScratch.Add(i);
        }
        return true;
    }

    // Serial, because each span starts where the previous one ended. Offsets are relative until the span is placed.
    uint32 FScenePrimitiveSet::LayoutFreshFoliageSpans(FFoliageEntityState& State, const TVector<FFoliageBakedInstance>& Baked)
    {
        const uint32 TypeCount = (uint32)FoliageTypeScratch.Num();
        const uint32 NewCount  = (uint32)Baked.Num();

        uint32 TotalBindings = 0;
        for (uint32 i = 0; i < NewCount; ++i)
        {
            const int32 TypeIndex = Baked[i].TypeIndex;
            const bool  bValidType = TypeIndex >= 0 && (uint32)TypeIndex < TypeCount;
            FFoliageTypeResolve* Type = bValidType ? &FoliageTypeScratch[TypeIndex] : nullptr;
            const uint32 WantCount = (Type != nullptr && Type->Memo != nullptr) ? (uint32)Type->Memo->Protos.Num() : 0u;

            FFoliageInstanceRef& Ref = State.Instances[i];
            Ref.TypeRow      = bValidType ? (uint16)TypeIndex : 0xFFFFu;
            Ref.SurfaceCount = (uint16)WantCount;
            Ref.BindingBase  = TotalBindings;
            TotalBindings   += WantCount;
            if (WantCount != 0u)
            {
                ++Type->FreshInstances;
            }
        }

        for (const FFoliageTypeResolve& Type : FoliageTypeScratch)
        {
            if (Type.FreshInstances == 0u)
            {
                continue;
            }
            const bool bSkinned = EnumHasAnyFlags(Type.BaseFlags, EInstanceFlags::Skinned);
            for (const FSurfaceBinding& Proto : Type.Memo->Protos)
            {
                Batches.AddBatchRefs(Proto.BatchIndex, bSkinned, Type.FreshInstances);
            }
        }
        return TotalBindings;
    }

    // From the free list, since a streamed chunk's foliage frees its slots one at a time and a fresh block never reused them.
    void FScenePrimitiveSet::AllocateFreshFoliageSlots(uint32 Count)
    {
        FoliageSlotScratch.SetNum(Count);
        for (uint32& Slot : FoliageSlotScratch)
        {
            // A fresh slot is marked by the allocator, and a reused one is not.
            const bool bReused = !InstanceFreeSlots.IsEmpty();
            Slot = AllocateInstanceSlot();
            if (bReused)
            {
                MarkInstanceDirty(Slot);
                MarkStaticDirty(Slot);
            }
        }
    }

    void FScenePrimitiveSet::BindFreshFoliage(FFoliageEntityState& State, const SFoliageComponent& Foliage, const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 NewCount      = (uint32)Baked.Num();
        const uint32 TotalBindings = LayoutFreshFoliageSpans(State, Baked);

        // A removed chunk's foliage left a span about this size, so streaming reuses it rather than growing toward a compaction.
        const uint32 BindingBase = TakeBindingRange(TotalBindings);
        AllocateFreshFoliageSlots(TotalBindings);

        Jobs::ParallelFor(NewCount, [&](const Jobs::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                FFoliageInstanceRef& Ref = State.Instances[i];
                if (Ref.SurfaceCount == 0u)
                {
                    Ref.BindingBase = 0u;
                    continue;
                }
                const uint32 Offset = Ref.BindingBase;
                Ref.BindingBase     = BindingBase + Offset;

                const FFoliageTypeResolve& Type = FoliageTypeScratch[Ref.TypeRow];
                const bool  bSkinned  = EnumHasAnyFlags(Type.BaseFlags, EInstanceFlags::Skinned);
                const bool  bHidden   = Foliage.IsInstanceHidden(i);
                const FTransform3x4 Packed = PackTransform3x4(Baked[i].Transform);

                for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
                {
                    FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
                    Binding              = Type.Memo->Protos[s];
                    Binding.InstanceSlot = FoliageSlotScratch[Offset + s];
                    Binding.bSkinned     = bSkinned;

                    RetainedCullEntries[Binding.InstanceSlot] = MakeFoliageCull(Binding, Type, Baked[i].SphereBounds, bHidden);
                    RetainedTransforms[Binding.InstanceSlot]  = Packed;
                    RetainedStatic[Binding.InstanceSlot]      = MakeFoliageStatic(Binding, Type, EntityID, kNoMeshletVisibility);
                    RetainedProxies[Binding.InstanceSlot]     = MakeFoliageProxy(Type, s);
                }
            }
        }, 4096);

        SyncStats.BindCalls            += NewCount;
        SyncStats.RefreshInstanceCalls += NewCount;
    }

    // Without a bake the slots keep their own sphere and transform. A parallel caller gets the dirty slots back as a mask, low half instance data and high half static.
    uint32 FScenePrimitiveSet::RefreshFoliageInstance(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                                      const TVector<FFoliageBakedInstance>* Baked, uint32 EntityID, uint32 Index, bool bMarkDirect)
    {
        using namespace ScenePrimitiveSetInternal;

        static constexpr FFoliageTypeResolve Unresolved{};
        const FFoliageInstanceRef& Ref  = State.Instances[Index];
        const FFoliageTypeResolve& Type = Ref.TypeRow < (uint32)FoliageTypeScratch.Num() ? FoliageTypeScratch[Ref.TypeRow] : Unresolved;
        const bool          bHidden = Foliage.IsInstanceHidden(Index);
        const FTransform3x4 Packed  = Baked != nullptr ? PackTransform3x4((*Baked)[Index].Transform) : FTransform3x4{};
        const uint32        SlotCount = (uint32)RetainedCullEntries.Num();

        uint32 Mask = 0u;
        for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= SlotCount)
            {
                continue;
            }

            FInstanceCullEntry& Cull = RetainedCullEntries[Slot];
            const FVector4 Sphere = Baked != nullptr ? (*Baked)[Index].SphereBounds : Cull.SphereBounds;
            bool bInstanceChanged = StoreIfChanged(Cull, MakeFoliageCull(Binding, Type, Sphere, bHidden));
            if (Baked != nullptr)
            {
                bInstanceChanged |= StoreIfChanged(RetainedTransforms[Slot], Packed);
            }
            const bool bProxyChanged  = StoreIfChanged(RetainedProxies[Slot], MakeFoliageProxy(Type, s));
            const bool bStaticChanged = StoreIfChanged(RetainedStatic[Slot], MakeFoliageStatic(Binding, Type, EntityID, RetainedStatic[Slot].MeshletVisibilityBase)) || bProxyChanged;

            if (!bMarkDirect)
            {
                Mask |= bInstanceChanged ? (1u << s) : 0u;
                Mask |= bStaticChanged ? (1u << (s + 16)) : 0u;
                continue;
            }
            if (bInstanceChanged)
            {
                MarkInstanceDirty(Slot);
            }
            if (bStaticChanged)
            {
                MarkStaticDirty(Slot);
            }
        }
        return Mask;
    }

    void FScenePrimitiveSet::RefreshFoliageInstances(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                                     const TVector<FFoliageBakedInstance>* Baked, uint32 EntityID)
    {
        const uint32 Count = (uint32)FoliageRefreshScratch.Num();
        if (Count == 0u)
        {
            return;
        }
        SyncStats.RefreshInstanceCalls += Count;

        if (Count < ScenePrimitiveSetInternal::kFreshFoliageParallelThreshold)
        {
            for (uint32 Index : FoliageRefreshScratch)
            {
                RefreshFoliageInstance(State, Foliage, Baked, EntityID, Index, true);
            }
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // A mask holds 16 surfaces, so an instance with more is refreshed in the serial pass instead.
        constexpr uint32 kMaxMaskedSurfaces = 16;
        FoliageRefreshMasks.Init(0u, Count);
        Jobs::ParallelFor(Count, [&](const Jobs::FParallelRange& Range)
        {
            for (uint32 r = Range.Start; r < Range.End; ++r)
            {
                const uint32 Index = FoliageRefreshScratch[r];
                if (State.Instances[Index].SurfaceCount <= kMaxMaskedSurfaces)
                {
                    FoliageRefreshMasks[r] = RefreshFoliageInstance(State, Foliage, Baked, EntityID, Index, false);
                }
            }
        }, 4096);

        for (uint32 r = 0; r < Count; ++r)
        {
            const uint32 Index = FoliageRefreshScratch[r];
            const FFoliageInstanceRef& Ref = State.Instances[Index];
            if (Ref.SurfaceCount > kMaxMaskedSurfaces)
            {
                RefreshFoliageInstance(State, Foliage, Baked, EntityID, Index, true);
                continue;
            }

            const uint32 Mask = FoliageRefreshMasks[r];
            for (uint32 s = 0; Mask != 0u && s < Ref.SurfaceCount; ++s)
            {
                const uint32 Slot = Bindings[Ref.BindingBase + s].InstanceSlot;
                if (Mask & (1u << s))
                {
                    MarkInstanceDirty(Slot);
                }
                if (Mask & (1u << (s + 16)))
                {
                    MarkStaticDirty(Slot);
                }
            }
        }
    }

    void FScenePrimitiveSet::ApplyFoliageInstanceEdits(const FSyncPools& Pools, FRenderDirtyTracker& Tracker)
    {
        InstanceEditScratch.Reset();
        if (!Tracker.DrainInstanceEdits(DirtyConsumer, InstanceEditScratch))
        {
            return;
        }

        // Edits arrive in runs per entity, so the lookups are paid once a run. An entity not bound yet reads the flags at its first bind.
        ECS::FEntity               Entity  = ECS::NullEntity;
        const FFoliageEntityState* State   = nullptr;
        const SFoliageComponent*   Foliage = nullptr;
        for (const FRenderDirtyTracker::FInstanceEdit& Edit : InstanceEditScratch)
        {
            if (Edit.Entity != Entity)
            {
                Entity  = Edit.Entity;
                State   = FoliageByEntity.Find(Entity);
                Foliage = State != nullptr ? Pools.Foliage->TryGet(Entity) : nullptr;
            }
            if (Foliage != nullptr)
            {
                ApplyFoliageInstanceVisibility(*State, *Foliage, Edit.Instance);
            }
        }
    }

    void FScenePrimitiveSet::ApplyFoliageInstanceVisibility(const FFoliageEntityState& State, const SFoliageComponent& Foliage, uint32 Instance)
    {
        if (Instance >= (uint32)State.Instances.Num())
        {
            return;
        }
        const FFoliageInstanceRef& Ref = State.Instances[Instance];
        const bool bHidden = Foliage.IsInstanceHidden(Instance);
        for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
        {
            const uint32 Slot = Bindings[Ref.BindingBase + s].InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.Num())
            {
                continue;
            }
            uint32& DrawIDAndFlags = RetainedCullEntries[Slot].DrawIDAndFlags;
            const uint32 Updated = bHidden ? (DrawIDAndFlags & ~ScenePrimitiveSetInternal::kActiveBit) : (DrawIDAndFlags | ScenePrimitiveSetInternal::kActiveBit);
            if (Updated != DrawIDAndFlags)
            {
                DrawIDAndFlags = Updated;
                MarkInstanceDirty(Slot);
            }
        }
    }

    // Foliage checks one key per TYPE rather than one per instance, so a field of it adds a handful of comparisons to the sweep.
    void FScenePrimitiveSet::SweepFoliageResolves(FMeshResolveCache& Cache)
    {
        for (uint32 Dense = 0; Dense < FoliageByEntity.Num(); ++Dense)
        {
            for (const uint64 Packed : FoliageByEntity.GetValueAt(Dense).TypeResolveKeys)
            {
                const uint32 Handle = (uint32)(Packed & 0xFFFFFFFFull);
                if (Handle >= (uint32)GenSnapshot.Num())
                {
                    continue;
                }

                uint32 Generation = GenSnapshot[Handle];
                if (Generation == Constants::kIndexNoneU32)
                {
                    Generation = Cache.GetEntry(Handle).Generation;
                    GenSnapshot[Handle] = Generation;
                }

                if (Generation != (uint32)(Packed >> 32))
                {
                    RetryScratch.Add(MakeKey(FoliageByEntity.GetKeyAt(Dense), EPrimitiveSource::Foliage));
                    break;
                }
            }
        }
    }

    void FScenePrimitiveSet::FlushFoliageReleases()
    {
        if (PendingFoliageReleases.IsEmpty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Sync/Release Foliage");

        GatherFoliageReleaseSlots();
        PendingFoliageReleases.Reset();
        FreeReleasedFoliageSlots();
    }

    // Splits every pending entity into blocks and counts each block's bindings on workers, returning how many blocks there are.
    uint32 FScenePrimitiveSet::PlanFoliageReleaseBlocks()
    {
        // Blocks across every pending entity, since one chunk's foliage is too small to split on its own.
        constexpr uint32 kInstancesPerBlock = 4096;
        uint32 NumBlocks = 0;
        for (const FFoliageEntityState& State : PendingFoliageReleases)
        {
            NumBlocks += RenderUtils::GetGroupCount((uint32)State.Instances.Num(), kInstancesPerBlock);
        }
        if ((uint32)FoliageReleaseBlocks.Num() < NumBlocks)
        {
            FoliageReleaseBlocks.SetNum(NumBlocks);
        }
        {
            uint32 Next = 0;
            for (int32 StateIndex = 0; StateIndex < PendingFoliageReleases.Num(); ++StateIndex)
            {
                const uint32 NumInstances = (uint32)PendingFoliageReleases[StateIndex].Instances.Num();
                for (uint32 Begin = 0; Begin < NumInstances; Begin += kInstancesPerBlock)
                {
                    FFoliageReleaseBlock& Block = FoliageReleaseBlocks[Next++];
                    Block.State = StateIndex;
                    Block.Begin = Begin;
                    Block.End   = Math::Min(NumInstances, Begin + kInstancesPerBlock);
                }
            }
        }

        // Counted first, so each block knows where its slots land before any are written.
        Jobs::ParallelFor(NumBlocks, [this](const Jobs::FParallelRange& Range)
        {
            for (uint32 b = Range.Start; b < Range.End; ++b)
            {
                FFoliageReleaseBlock& Block = FoliageReleaseBlocks[b];
                const TVector<FFoliageInstanceRef>& Instances = PendingFoliageReleases[Block.State].Instances;
                Block.NumBindings = 0u;
                Block.bContiguous = true;
                for (uint32 i = Block.Begin; i < Block.End; ++i)
                {
                    const FFoliageInstanceRef& Ref = Instances[i];
                    if (Ref.SurfaceCount == 0u)
                    {
                        continue;
                    }
                    if (Block.NumBindings == 0u)
                    {
                        Block.FirstBase = Ref.BindingBase;
                    }
                    else if (Ref.BindingBase != Block.EndBase)
                    {
                        Block.bContiguous = false;
                    }
                    Block.EndBase      = Ref.BindingBase + Ref.SurfaceCount;
                    Block.NumBindings += Ref.SurfaceCount;
                }
            }
        }, 4);

        return NumBlocks;
    }

    // Collects every released binding's slot into FoliageSlotScratch and drops the batch references, both on workers by block.
    void FScenePrimitiveSet::GatherFoliageReleaseSlots()
    {
        const uint32 NumBlocks = PlanFoliageReleaseBlocks();

        // An entity whose bindings form one span leaves it whole for the next fresh bind.
        uint32 Total = 0;
        for (uint32 b = 0; b < NumBlocks;)
        {
            const uint32 StateIndex = FoliageReleaseBlocks[b].State;
            uint32 StateBindings = 0;
            uint32 SpanBase      = 0;
            uint32 SpanEnd       = 0;
            bool   bContiguous   = true;
            for (; b < NumBlocks && FoliageReleaseBlocks[b].State == StateIndex; ++b)
            {
                FFoliageReleaseBlock& Block = FoliageReleaseBlocks[b];
                Block.SlotOffset = Total;
                if (Block.NumBindings == 0u)
                {
                    continue;
                }
                if (StateBindings == 0u)
                {
                    SpanBase = Block.FirstBase;
                }
                else if (Block.FirstBase != SpanEnd)
                {
                    bContiguous = false;
                }
                bContiguous    = bContiguous && Block.bContiguous;
                SpanEnd        = Block.EndBase;
                StateBindings += Block.NumBindings;
                Total         += Block.NumBindings;
            }
            if (bContiguous && StateBindings != 0u)
            {
                BindingFreeRanges.Emplace(SpanBase, StateBindings);
                FreeRangeBindings += StateBindings;
            }
        }

        FoliageSlotScratch.SetNum(Total);
        Jobs::ParallelFor(NumBlocks, [this](const Jobs::FParallelRange& Range)
        {
            for (uint32 b = Range.Start; b < Range.End; ++b)
            {
                FFoliageReleaseBlock& Block = FoliageReleaseBlocks[b];
                const TVector<FFoliageInstanceRef>& Instances = PendingFoliageReleases[Block.State].Instances;
                Block.Runs.Reset();
                uint32 Write = Block.SlotOffset;
                for (uint32 i = Block.Begin; i < Block.End; ++i)
                {
                    const FFoliageInstanceRef& Ref = Instances[i];
                    for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
                    {
                        const FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
                        ScenePrimitiveSetInternal::DeferBatchRelease(Block.Runs, Binding.BatchIndex, Binding.bSkinned);
                        FoliageSlotScratch[Write++] = Binding.InstanceSlot;
                    }
                }
            }
        }, 4);

        for (uint32 b = 0; b < NumBlocks; ++b)
        {
            FlushDeferredBatchReleases(FoliageReleaseBlocks[b].Runs);
        }

        DeadBindings += Total;
    }

    void FScenePrimitiveSet::FreeReleasedFoliageSlots()
    {
        // A slot past the retained arrays was never live, so it must not reach the free list.
        const uint32 SlotLimit = (uint32)RetainedCullEntries.Num();
        FoliageSlotScratch.RemoveAll([SlotLimit](uint32 Slot) { return Slot >= SlotLimit; });

        const uint32 Count = (uint32)FoliageSlotScratch.Num();
        LUMINA_PROFILE_VALUE("Sync/ReleasedFoliageSlots", Count);
        if (Count == 0u)
        {
            return;
        }

        ResetParallelBuckets();

        // The slots scatter across the retained arrays, so the cache misses spread over workers instead of one thread.
        Jobs::ParallelFor(Count, [this](const Jobs::FParallelRange& Range)
        {
            // Triples of base, word count and meshlets, so the serial pass after only appends to the free lists.
            TVector<uint32>& FreedRanges = BucketFor(Range.Thread).Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const uint32 Slot = FoliageSlotScratch[i];
                RetainedCullEntries[Slot].DrawIDAndFlags &= ~ScenePrimitiveSetInternal::kActiveBit;
                RetainedStatic[Slot].MeshletVisibilityBase = kNoMeshletVisibility;
                if (Slot < (uint32)SlotMeshletVisibility.Num())
                {
                    FUIntVector3& Visibility = SlotMeshletVisibility[Slot];
                    if (Visibility.y != 0u)
                    {
                        FreedRanges.Add(Visibility.x);
                        FreedRanges.Add(Visibility.y);
                        FreedRanges.Add(Visibility.z);
                    }
                    Visibility = FUIntVector3(kNoMeshletVisibility, 0u, 0u);
                }
            }
        }, 16384);

        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            for (int32 i = 0; i + 2 < Bucket.Slots.Num(); i += 3)
            {
                FreeMeshletVisibilityRange(Bucket.Slots[i], Bucket.Slots[i + 1]);
                LargestLODMeshletTotal -= Bucket.Slots[i + 2];
            }
            Bucket.Slots.Reset();
        }

        InstanceFreeSlots.Append(FoliageSlotScratch);

        // Only the active bit changed, which the GPU clears from the slot list instead of a re-send of each entry.
        if (!bFullInstanceUpload)
        {
            DeactivatedSlots.Append(FoliageSlotScratch);
        }
    }
}
