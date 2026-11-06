#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "ScenePrimitiveSetInternal.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Containers/Algorithm.h"
#include "World/World.h"

namespace Lumina
{
    namespace ScenePrimitiveSetInternal
    {
        template <typename TComponent>
        void ReadCommonMeshState(FScenePrimitive& Prim, FPrimitiveCullData& Cull, const TComponent& Component)
        {
            Prim.CustomData         = Component.CustomPrimitiveData.Data.Packed;
            Prim.BoundsScale        = Component.BoundsScale;
            Prim.ForcedLODIndex     = Component.ForcedLODIndex;
            Prim.bCastShadow        = Component.bCastShadow;
            Prim.bCastFarShadow     = Component.bCastFarShadow;
            Prim.RenderLayers       = Component.RenderLayers.Bits;
            Prim.CustomDepthStencil = PackCustomDepthStencil(Component.bRenderCustomDepth, Component.CustomDepthStencilValue);

            Cull.MaxDrawDistance = Component.MaxDrawDistance;
            Cull.bCastShadow     = Component.bCastShadow ? 1u : 0u;
        }
    }

    //~ Primitive sync

    void FScenePrimitiveSet::SyncPrimitives(ECS::FRegistry& Registry, const FSyncPools& Pools, TSpan<const FRenderDirtyTracker::FEntry> Requests)
    {
        if (Requests.IsEmpty())
        {
            return;
        }
        SyncStats.SyncEntityCalls += (uint32)Requests.Num();

        ClassifyPrimitiveSyncs(Registry, Pools, Requests);

        for (const FRenderDirtyTracker::FEntry& Move : PrimitiveMoveScratch)
        {
            ApplyEntityTransform(Pools, Move.Entity, (uint8)(1u << (uint32)Move.Source), nullptr);
        }

        const uint32 Count = (uint32)PrimitiveSyncWork.Num();
        if (Count == 0u)
        {
            return;
        }

        Jobs::ParallelForOrSerial(Count, ScenePrimitiveSetInternal::kParallelSyncThreshold, [this, &Pools](const Jobs::FParallelRange& Range)
        {
            for (uint32 Work = Range.Start; Work < Range.End; ++Work)
            {
                ReadPrimitiveState(Pools, Work);
            }
        }, 64);

        for (uint32 Work = 0; Work < Count; ++Work)
        {
            BindPrimitiveState(Work);
        }

        RefreshSyncedPrimitives(Pools);
    }

    // Removals and adds settle the primitive table first, since a swap-remove moves the indices the later phases work on.
    void FScenePrimitiveSet::ClassifyPrimitiveSyncs(ECS::FRegistry& Registry, const FSyncPools& Pools, TSpan<const FRenderDirtyTracker::FEntry> Requests)
    {
        PrimitiveSyncWork.Reset();
        PrimitiveMoveScratch.Reset();

        for (int32 r = 0; r < Requests.Num(); ++r)
        {
            const FRenderDirtyTracker::FEntry& Request = Requests[r];
            const ECS::FEntity Entity = Request.Entity;
            const uint32       Source = (uint32)Request.Source;
            DEBUG_ASSERT(Source < kLinkedSources);

            InvalidateStreamCache(Entity);
            KillMoveRecord(Entity);

            const uint32 Index = FindLinked(Entity, Request.Source);
            if (Request.Flags == EPrimitiveDirty::Transform)
            {
                if (Index != Constants::kIndexNoneU32)
                {
                    PrimitiveMoveScratch.Add(Request);
                }
                continue;
            }

            const bool bShouldExist = Registry.IsValid(Entity) && !Pools.Disabled->Contains(Entity)
                                   && Pools.Sources[Source]->Contains(Entity) && Pools.Transform->Contains(Entity);
            if (!bShouldExist)
            {
                if (Index != Constants::kIndexNoneU32)
                {
                    RemovePrimitive(Index);
                }
                continue;
            }

            if (Index == Constants::kIndexNoneU32)
            {
                AddPrimitive(Entity, Request.Source);
            }
            PrimitiveSyncWork.EmplaceGetRef().Request = (uint32)r;
        }

        for (FPrimitiveSyncWork& Work : PrimitiveSyncWork)
        {
            const FRenderDirtyTracker::FEntry& Request = Requests[(int32)Work.Request];
            Work.Index = FindLinked(Request.Entity, Request.Source);
        }
    }

    // Touches only its own primitive and component, so the parallel half of a sync; anything shared waits for BindPrimitiveState.
    void FScenePrimitiveSet::ReadPrimitiveState(const FSyncPools& Pools, uint32 WorkIndex)
    {
        FPrimitiveSyncWork& Work = PrimitiveSyncWork[WorkIndex];
        FScenePrimitive&    Prim = Primitives[Work.Index];

        Work.OldSurfaces     = Prim.Surfaces;
        Work.OldHandle       = Prim.ResolveHandle;
        Work.OldGeneration   = Prim.ResolveGeneration;
        Work.bHadCustomDepth = Prim.CustomDepthStencil != 0u;
        Work.NewBoneCount    = Prim.BoneCount;
        Work.bContentChanged = false;

        Prim.Surfaces = nullptr;
        Work.bPresent = Prim.Source == EPrimitiveSource::DynamicMesh ? ReadDynamicMeshState(Pools, Work) : ReadMeshState(Pools, Work);
        if (!Work.bPresent)
        {
            return;
        }

        if (const STransformComponent* Transform = Pools.Transform->TryGet(Prim.Entity))
        {
            RebuildWorldBounds(Work.Index, Transform->GetWorldMatrixCached());
        }
    }

    bool FScenePrimitiveSet::ReadMeshState(const FSyncPools& Pools, FPrimitiveSyncWork& Work)
    {
        FScenePrimitive&    Prim = Primitives[Work.Index];
        FPrimitiveCullData& Cull = CullData[Work.Index];

        const SMeshComponent* Base = nullptr;
        const void*           LiveMesh = nullptr;

        if (Prim.Source == EPrimitiveSource::StaticMesh)
        {
            const SStaticMeshComponent* Component = Pools.StaticMesh->TryGet(Prim.Entity);
            if (Component == nullptr)
            {
                return false;
            }
            ScenePrimitiveSetInternal::ReadCommonMeshState(Prim, Cull, *Component);
            Base     = Component;
            LiveMesh = (const void*)Component->StaticMesh.Get();
        }
        else
        {
            const SSkeletalMeshComponent* Component = Pools.SkeletalMesh->TryGet(Prim.Entity);
            if (Component == nullptr)
            {
                return false;
            }
            ScenePrimitiveSetInternal::ReadCommonMeshState(Prim, Cull, *Component);
            Base     = Component;
            LiveMesh = (const void*)Component->SkeletalMesh.Get();

            // Read here rather than at bind, since the slice is keyed to the SKELETON.
            const CSkeletalMesh*     SkelMesh = Component->SkeletalMesh.Get();
            const FSkeletonResource* SkelRes  = (SkelMesh != nullptr && SkelMesh->Skeleton.IsValid())
                                              ? SkelMesh->Skeleton->GetSkeletonResource()
                                              : nullptr;
            Work.NewBoneCount      = SkelRes != nullptr ? (uint32)SkelRes->GetNumBones() : 0u;
            Prim.RequiredBoneCount = SkelMesh != nullptr ? SkelMesh->GetMeshResource().RequiredBoneCount : 0u;

            if (Work.NewBoneCount != 0u && Prim.RequiredBoneCount > Work.NewBoneCount)
            {
                LOG_ERROR("Skinning: mesh '{}' references {} bones but its skeleton provides {}. Vertices "
                          "weighted to joints >= {} read past the bone slice and will be wildly displaced. "
                          "Mesh and skeleton are out of sync, reimport both.",
                          SkelMesh->GetName(), Prim.RequiredBoneCount, Work.NewBoneCount, Work.NewBoneCount);
            }
        }

        Prim.LocalCenter       = Base->CachedLocalCenter;
        Prim.LocalRadius       = Base->CachedLocalRadius;
        Prim.MeshletHeaderSlot = Base->CachedMeshletHeaderSlot;
        Prim.BaseFlags         = Base->CachedBaseFlags;
        Prim.ResolveHandle     = Base->ResolveHandle;

        if (Prim.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE || (Base->CachedMeshKey != LiveMesh && !Base->bShowingPreviousMesh))
        {
            if (LiveMesh != nullptr)
            {
                Base->QueueRenderResolve();
            }
            return true;
        }

        FMeshResolveCache& Cache = *Pools.ResolveCache;
        if (!Cache.IsValidHandle(Prim.ResolveHandle))
        {
            Base->QueueRenderResolve();
            return true;
        }

        const FResolvedMesh& Entry = Cache.GetEntry(Prim.ResolveHandle);
        if (Entry.bResolved)
        {
            Prim.Surfaces          = &Entry.Surfaces;
            Prim.ResolveGeneration = Entry.Generation;
        }
        return true;
    }

    bool FScenePrimitiveSet::ReadDynamicMeshState(const FSyncPools& Pools, FPrimitiveSyncWork& Work)
    {
        FScenePrimitive&    Prim = Primitives[Work.Index];
        FPrimitiveCullData& Cull = CullData[Work.Index];

        SDynamicMeshComponent* Component = Pools.DynamicMesh->TryGet(Prim.Entity);
        if (Component == nullptr)
        {
            return false;
        }
        ScenePrimitiveSetInternal::ReadCommonMeshState(Prim, Cull, *Component);

        Component->SyncedRenderDataVersion = Component->LoadRenderDataVersion();

        Work.OldDynamicData    = std::move(Prim.DynamicRenderData);
        Prim.DynamicRenderData = Component->LoadRenderData();

        const FDynamicMeshRenderData* Data = Prim.DynamicRenderData.get();
        if (Data == nullptr || Data->MeshletHeaderSlot == 0 || Data->Surfaces.IsEmpty())
        {
            Work.NewDynamicData = std::move(Prim.DynamicRenderData);
            return true;
        }

        Prim.LocalCenter       = Data->LocalCenter;
        Prim.LocalRadius       = Data->LocalRadius;
        Prim.MeshletHeaderSlot = Data->MeshletHeaderSlot;
        Prim.ResolveHandle     = INVALID_MESH_RESOLVE_HANDLE;

        EInstanceFlags BaseFlags = EInstanceFlags::None;
        if (Component->bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
        if (!Component->bReceiveDecals)         { BaseFlags |= EInstanceFlags::NoDecals; }
        if (Component->bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
        Prim.BaseFlags = BaseFlags;

        Prim.Surfaces          = &Data->Surfaces;
        Prim.ResolveGeneration = 0;

        // The identity test is blind here; see BindingsMatchSurfaces.
        Work.bContentChanged = !BindingsMatchSurfaces(Work.Index, Data->Surfaces);
        return true;
    }

    void FScenePrimitiveSet::BindPrimitiveState(uint32 WorkIndex)
    {
        FPrimitiveSyncWork& Work = PrimitiveSyncWork[WorkIndex];
        Work.OldDynamicData.reset();
        Work.NewDynamicData.reset();
        if (!Work.bPresent)
        {
            return;
        }

        const uint32     Index = Work.Index;
        FScenePrimitive& Prim  = Primitives[Index];

        SetBoneCount(Index, Work.NewBoneCount);
        if ((Prim.CustomDepthStencil != 0u) != Work.bHadCustomDepth)
        {
            CustomDepthCount += Work.bHadCustomDepth ? ~0u : 1u;
        }

        const bool bSurfacesChanged = Prim.Surfaces != Work.OldSurfaces
                                   || Prim.ResolveHandle != Work.OldHandle
                                   || Prim.ResolveGeneration != Work.OldGeneration
                                   || Work.bContentChanged;
        if (bSurfacesChanged)
        {
            BindSurfaces(Index);
        }

        ScenePrimitiveSetInternal::WarnOnGeometryKindMismatch(Prim.MeshletHeaderSlot, Prim.BaseFlags, Prim.Entity.Value, Prim.Source);

        // One of the two places a primitive's resolve identity settles; mirror it for the sweep.
        ResolveKeys[Index] = PackResolveKey(Prim.ResolveHandle, Prim.ResolveGeneration);
        if (Prim.ResolveHandle != Work.OldHandle)
        {
            UnlinkHandle(Index, Work.OldHandle);
            LinkHandle(Index, Prim.ResolveHandle);
        }

        TrackForeground(Index);
    }

    void FScenePrimitiveSet::RefreshSyncedPrimitives(const FSyncPools& Pools)
    {
        const uint32 Count = (uint32)PrimitiveSyncWork.Num();
        const auto RefreshOne = [this, &Pools](const FPrimitiveSyncWork& Work, FDirtySlotBucket* Sink)
        {
            if (!Work.bPresent)
            {
                return;
            }
            if (const STransformComponent* Transform = Pools.Transform->TryGet(Primitives[Work.Index].Entity))
            {
                RefreshInstances(Work.Index, Transform->GetWorldMatrixCached(), Sink);
            }
        };

        if (Count < ScenePrimitiveSetInternal::kParallelSyncThreshold)
        {
            for (const FPrimitiveSyncWork& Work : PrimitiveSyncWork)
            {
                RefreshOne(Work, nullptr);
            }
            return;
        }

        ResetParallelBuckets();
        Jobs::ParallelFor(Count, [this, &RefreshOne](const Jobs::FParallelRange& Range)
        {
            FDirtySlotBucket& Bucket = BucketFor(Range.Thread);
            for (uint32 Work = Range.Start; Work < Range.End; ++Work)
            {
                RefreshOne(PrimitiveSyncWork[Work], &Bucket);
            }
        }, 64);
        SyncStats.RefreshInstanceCalls += Count;
        MergeParallelDirtySlots();
    }

    //~ Sync

    void FScenePrimitiveSet::ClearScene()
    {
        Batches.Reset();
        // Always paired with Batches.Reset(), which renumbers every batch index the memo cached.
        BindingMemoByHandle.Reset();

        Primitives.Reset();
        Bounds.Reset();
        CullData.Reset();
        ResolveKeys.Reset();
        HandleListHead.Reset();
        NextByHandle.Reset();
        PrevByHandle.Reset();
        LastSweptTableGeneration = 0;
        BonelessSkinnedCount = 0;
        CustomDepthCount = 0;
        SkeletalIndices.Reset();
        // Every slice died with the primitives, so the arena restarts rather than leaking live bases.
        BoneSlices.Reset();
        BoneSliceSweepCursor = 0;

        Bindings.Reset();
        DeadBindings = 0;
        BindingFreeRanges.Reset();
        FreeRangeBindings = 0;

        RetainedCullEntries.Reset();
        RetainedTransforms.Reset();
        RetainedStatic.Reset();
        InstanceFreeSlots.Reset();
        DirtyInstanceSlots.Reset();
        DeactivatedSlots.Reset();
        DirtyStaticSlots.Reset();
        bFullInstanceUpload = true;
        bFullStaticUpload   = true;
        SkinnedIndexFreeList.Reset();
        SkinnedIndexExtent = 0;
        SurfaceDescs.Reset();
        SurfaceDescByHash.Reset();
        bSurfaceDescsDirty = true;
        MaxSurfaceDescMeshlets = 0;
        SlotMeshletVisibility.Reset();
        MeshletVisibilityFreeLists.Reset();
        MeshletVisibilityExtent = 1u;
        LargestLODMeshletTotal  = 0u;

        // The blocks lived in the retained arrays just dropped, so a species re-acquires one rather than scattering over other slots.
        GrassSpecies.Reset();
        GrassFreeBlocks.Reset();
        GrassSlotCount = 0;

        ResetEntityRecords();
        ForegroundKeys.Reset();
        PendingForegroundRefresh = EForegroundRefresh::None;

        FoliageByEntity.Clear();
        FoliageInstanceCount = 0;
        PendingFoliageReleases.Reset();

        ResetStreamState();
        bResolveTableChanged = false;
    }

    FRenderDirtyTracker& FScenePrimitiveSet::AttachDirtyConsumer(ECS::FRegistry& Registry)
    {
        BoundRegistry = &Registry;
        FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);
        if (DirtyConsumer == Constants::kIndexNoneU32)
        {
            DirtyConsumer = Tracker.AddConsumer();
        }
        return Tracker;
    }

    void FScenePrimitiveSet::Reset(ECS::FRegistry* Registry)
    {
        if (Registry == nullptr)
        {
            ClearScene();
            return;
        }

        DisarmMoveStream(*Registry);
        FRenderDirtyTracker& Tracker = AttachDirtyConsumer(*Registry);
        ClearScene();
        Tracker.RequestFullRescan(DirtyConsumer);
    }

    void FScenePrimitiveSet::Shutdown(ECS::FRegistry& Registry)
    {
        BoundRegistry = &Registry;
        DisarmMoveStream(Registry);
        ClearScene();

        if (FRenderDirtyTracker* Tracker = FRenderDirtyTracker::Find(Registry))
        {
            Tracker->RemoveConsumer(DirtyConsumer);
        }
        DirtyConsumer = Constants::kIndexNoneU32;
        MovedConsumer = Constants::kIndexNoneU32;
    }

    void FScenePrimitiveSet::FullRescan(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        ClearScene();

        const FSyncPools Pools(Registry);

        // Level load rebuilds from nothing, so unlike the incremental path the counts here are exact.
        const int32 MeshCount = IntCastChecked<int32>(Pools.StaticMesh->GetDenseSize() + Pools.SkeletalMesh->GetDenseSize() + Pools.DynamicMesh->GetDenseSize());
        Primitives.Reserve(MeshCount);
        Bounds.Reserve(MeshCount);
        CullData.Reserve(MeshCount);
        ResolveKeys.Reserve(MeshCount);
        NextByHandle.Reserve(MeshCount);
        PrevByHandle.Reserve(MeshCount);
        Bindings.Reserve(MeshCount);
        RetainedCullEntries.Reserve(MeshCount);
        RetainedTransforms.Reserve(MeshCount);
        RetainedStatic.Reserve(MeshCount);

        PrimitiveSyncScratch.Reset();
        PrimitiveSyncScratch.Reserve(MeshCount);
        for (ECS::FEntity Entity : Registry.View<SStaticMeshComponent>())
        {
            PrimitiveSyncScratch.Add({ Entity, EPrimitiveSource::StaticMesh, EPrimitiveDirty::All });
        }
        for (ECS::FEntity Entity : Registry.View<SDynamicMeshComponent>())
        {
            PrimitiveSyncScratch.Add({ Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::All });
        }
        for (ECS::FEntity Entity : Registry.View<SSkeletalMeshComponent>())
        {
            PrimitiveSyncScratch.Add({ Entity, EPrimitiveSource::SkeletalMesh, EPrimitiveDirty::All });
        }
        SyncPrimitives(Registry, Pools, TSpan<const FRenderDirtyTracker::FEntry>(PrimitiveSyncScratch.GetData(), PrimitiveSyncScratch.Num()));

        for (ECS::FEntity Entity : Registry.View<SFoliageComponent>())
        {
            SyncFoliage(Registry, Pools, Entity);
        }
        FlushFoliageReleases();
    }

    void FScenePrimitiveSet::PollUnhookedSources(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        PollSkeletalBoneRanges(Registry, Tracker);
    }

    // A skeleton finishing its load marks nothing dirty, so a primitive that synced mid-load stays boneless.
    void FScenePrimitiveSet::PollSkeletalBoneRanges(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        if (BonelessSkinnedCount == 0u)
        {
            return;
        }

        const ECS::TComponentStorage<SSkeletalMeshComponent> Storage = Registry.GetStorage<SSkeletalMeshComponent>();

        for (uint32 Index : SkeletalIndices)
        {
            const FScenePrimitive& Prim = Primitives[Index];

            // A live slice can only need resizing when the mesh changes, which marks the primitive itself.
            if (Prim.BoneCount != 0u)
            {
                continue;
            }

            const SSkeletalMeshComponent* Component = Storage->TryGet(Prim.Entity);
            if (Component == nullptr)
            {
                continue;
            }

            const CSkeletalMesh* SkelMesh = Component->SkeletalMesh.Get();
            const FSkeletonResource* SkelRes = (SkelMesh != nullptr && SkelMesh->Skeleton.IsValid())
                                             ? SkelMesh->Skeleton->GetSkeletonResource()
                                             : nullptr;

            if (SkelRes != nullptr && SkelRes->GetNumBones() > 0)
            {
                Tracker.Mark(Prim.Entity, EPrimitiveSource::SkeletalMesh, EPrimitiveDirty::Data);
            }
        }
    }

    FScenePrimitiveSet::FCoalescedEntity& FScenePrimitiveSet::CoalescedRecordFor(ECS::FEntity Entity)
    {
        FEntityRecord& Mapped = EntityRecords.FindOrAdd(Entity);
        if (Mapped.CoalesceStamp == CoalesceStamp
            && Mapped.CoalesceIndex < (uint32)CoalescedScratch.Num()
            && CoalescedScratch[Mapped.CoalesceIndex].Entity == Entity.Value)
        {
            return CoalescedScratch[Mapped.CoalesceIndex];
        }

        Mapped.CoalesceStamp = CoalesceStamp;
        Mapped.CoalesceIndex = (uint32)CoalescedScratch.Num();
        FCoalescedEntity& Record = CoalescedScratch.EmplaceGetRef();
        Record.Entity = Entity.Value;
        return Record;
    }

    bool FScenePrimitiveSet::HasCoalescedRecord(ECS::FEntity Entity) const
    {
        const FEntityRecord* Mapped = EntityRecords.Find(Entity);
        return Mapped != nullptr
            && Mapped->CoalesceStamp == CoalesceStamp
            && Mapped->CoalesceIndex < (uint32)CoalescedScratch.Num()
            && CoalescedScratch[Mapped->CoalesceIndex].Entity == Entity.Value;
    }

    // A frame's entries are folded per entity before any of them are applied, so an entity touched many times syncs once.
    void FScenePrimitiveSet::CoalesceDrain(TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Coalesce");

        // Stamping beats clearing, since the records span the entity index space.
        if (++CoalesceStamp == 0u)
        {
            EntityRecords.ForEach([](FEntityRecord& Record) { Record.CoalesceStamp = 0u; });
            CoalesceStamp = 1u;
        }
        CoalescedScratch.Reset();
        CoalescedScratch.Reserve(DrainScratch.Num() + Moved.Num());

        // A move dirties every linked source but never foliage, which bakes its own world transform.
        const auto FoldAllSources = [](FCoalescedEntity& Record, EPrimitiveDirty Flags)
        {
            Record.Flags[(uint32)EPrimitiveSource::StaticMesh]   |= Flags;
            Record.Flags[(uint32)EPrimitiveSource::DynamicMesh]  |= Flags;
            Record.Flags[(uint32)EPrimitiveSource::SkeletalMesh] |= Flags;
            if (Flags != EPrimitiveDirty::Transform)
            {
                Record.Flags[(uint32)EPrimitiveSource::Foliage] |= Flags;
            }
        };

        for (const FRenderDirtyTracker::FEntry& Entry : DrainScratch)
        {
            FCoalescedEntity& Record = CoalescedRecordFor(Entry.Entity);
            if (Entry.Source == EPrimitiveSource::AnySource)
            {
                FoldAllSources(Record, Entry.Flags);
            }
            else
            {
                DEBUG_ASSERT((uint32)Entry.Source < (uint32)EPrimitiveSource::Num);
                if ((uint32)Entry.Source < (uint32)EPrimitiveSource::Num)
                {
                    Record.Flags[(uint32)Entry.Source] |= Entry.Flags;
                }
            }
        }

        // Only a mover that also has a tracker record folds into it; every other mover is applied directly later.
        if (!CoalescedScratch.IsEmpty() && !Moved.IsEmpty())
        {
            const uint32 NumThreads = Math::Max(Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 1u, 1u);
            if ((uint32)ParallelOverlaps.Num() < NumThreads)
            {
                ParallelOverlaps.SetNum(NumThreads);
            }
            for (TVector<ECS::FEntity>& Bucket : ParallelOverlaps)
            {
                Bucket.Reset();
            }

            Jobs::ParallelFor((uint32)Moved.Num(), [this, Moved](const Jobs::FParallelRange& Range)
            {
                TVector<ECS::FEntity>& Bucket = ParallelOverlaps[Range.Thread];
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    if (Moved[i] != ECS::NullEntity && HasCoalescedRecord(Moved[i]))
                    {
                        Bucket.Add(Moved[i]);
                    }
                }
            }, 2048);

            for (const TVector<ECS::FEntity>& Bucket : ParallelOverlaps)
            {
                for (ECS::FEntity Entity : Bucket)
                {
                    FoldAllSources(CoalescedRecordFor(Entity), EPrimitiveDirty::Transform);
                }
            }
        }

        SyncStats.CoalescedEntities = (uint32)CoalescedScratch.Num();
    }

    void FScenePrimitiveSet::ReserveForDrain()
    {
        using namespace ScenePrimitiveSetInternal;

        uint32 NewPrimitives = 0;
        for (const FCoalescedEntity& Record : CoalescedScratch)
        {
            const ECS::FEntity Entity = (ECS::FEntity)Record.Entity;
            for (uint32 s = 0; s < kLinkedSources; ++s)
            {
                const EPrimitiveDirty Flags = Record.Flags[s];
                if (Flags != EPrimitiveDirty::None && Flags != EPrimitiveDirty::Transform
                    && FindLinked(Entity, (EPrimitiveSource)s) == Constants::kIndexNoneU32)
                {
                    ++NewPrimitives;
                }
            }
        }

        if (NewPrimitives == 0)
        {
            return;
        }

        const SIZE_T PrimitiveTarget = Primitives.Num() + NewPrimitives;
        ReserveGeometric(Primitives, PrimitiveTarget);
        ReserveGeometric(Bounds, PrimitiveTarget);
        ReserveGeometric(CullData, PrimitiveTarget);
        ReserveGeometric(ResolveKeys, PrimitiveTarget);

        ReserveGeometric(Bindings, Bindings.Num() + NewPrimitives);

        const SIZE_T FreeSlots      = InstanceFreeSlots.Num();
        const SIZE_T NeededSlots    = NewPrimitives > FreeSlots ? NewPrimitives - FreeSlots : 0;
        const SIZE_T InstanceTarget = RetainedCullEntries.Num() + NeededSlots;
        ReserveGeometric(RetainedCullEntries, InstanceTarget);
        ReserveGeometric(RetainedTransforms, InstanceTarget);
        ReserveGeometric(RetainedStatic, InstanceTarget);
    }

    void FScenePrimitiveSet::PartitionDrain()
    {
        TransformRecords.Reset();
        StructuralRecords.Reset();
        ScenePrimitiveSetInternal::ReserveGeometric(TransformRecords, CoalescedScratch.Num());

        for (uint32 i = 0, N = (uint32)CoalescedScratch.Num(); i < N; ++i)
        {
            const FCoalescedEntity& Record = CoalescedScratch[i];

            EPrimitiveDirty All = EPrimitiveDirty::None;
            for (const EPrimitiveDirty Flags : Record.Flags)
            {
                All |= Flags;
            }

            if (All == EPrimitiveDirty::Transform)
            {
                TransformRecords.Add(i);
            }
            else if (All != EPrimitiveDirty::None)
            {
                StructuralRecords.Add(i);
            }
        }

        SyncStats.TransformRecords  = (uint32)TransformRecords.Num();
        SyncStats.StructuralRecords = (uint32)StructuralRecords.Num();
    }

    void FScenePrimitiveSet::ApplyStructuralRecords(ECS::FRegistry& Registry, const FSyncPools& Pools)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Structural");
        LUMINA_PROFILE_VALUE("Sync/StructuralRecords", StructuralRecords.Num());

        PrimitiveSyncScratch.Reset();
        for (uint32 RecordIndex : StructuralRecords)
        {
            const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];
            for (uint32 s = 0; s < kLinkedSources; ++s)
            {
                if (Record.Flags[s] != EPrimitiveDirty::None)
                {
                    PrimitiveSyncScratch.Add({ (ECS::FEntity)Record.Entity, (EPrimitiveSource)s, Record.Flags[s] });
                }
            }
        }
        SyncPrimitives(Registry, Pools, TSpan<const FRenderDirtyTracker::FEntry>(PrimitiveSyncScratch.GetData(), PrimitiveSyncScratch.Num()));

        for (uint32 RecordIndex : StructuralRecords)
        {
            const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];
            const ECS::FEntity      Entity = (ECS::FEntity)Record.Entity;

            const EPrimitiveDirty FoliageFlags = Record.Flags[(uint32)EPrimitiveSource::Foliage];
            if (FoliageFlags != EPrimitiveDirty::None && (Pools.Foliage->Contains(Entity) || FoliageByEntity.Contains(Entity)))
            {
                SyncFoliage(Registry, Pools, Entity);
            }
        }

        FlushFoliageReleases();

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }
    }

    void FScenePrimitiveSet::ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex, TVector<uint32>* OutDirty)
    {
        const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];

        uint8 SourceBits = 0;
        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            SourceBits |= Record.Flags[s] != EPrimitiveDirty::None ? (uint8)(1u << s) : (uint8)0u;
        }
        ApplyEntityTransform(Pools, (ECS::FEntity)Record.Entity, SourceBits, OutDirty);
    }

    void FScenePrimitiveSet::ApplyEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint8 SourceBits, TVector<uint32>* OutDirty)
    {
        const FEntityRecord* Record = EntityRecords.Find(Entity);
        if (Record == nullptr || Record->Entity != Entity.Value)
        {
            return;
        }

        const STransformComponent* Transform = Pools.Transform->TryGet(Entity);
        if (Transform == nullptr)
        {
            return;
        }
        const FMatrix4 Matrix = Transform->GetWorldMatrixCached();

        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            const uint32 Index = Record->Links[s];
            if ((SourceBits & (1u << s)) == 0 || Index == Constants::kIndexNoneU32)
            {
                continue;
            }

            const FVector4 Sphere = RebuildWorldBounds(Index, Matrix);
            RefreshInstanceTransform(Index, Matrix, Sphere, OutDirty);
        }
    }

    void FScenePrimitiveSet::ApplyTransformRecords(const FSyncPools& Pools)
    {
        const uint32 Count = (uint32)TransformRecords.Num();
        if (Count == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Sync/Apply/Transforms");

        SyncStats.SyncEntityCalls += Count;

        const uint32 NumSlots = Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 0u;
        if (Count < ScenePrimitiveSetInternal::kParallelMoveThreshold || NumSlots <= 1)
        {
            for (uint32 RecordIndex : TransformRecords)
            {
                ApplyTransformRecord(Pools, RecordIndex, nullptr);
            }
            return;
        }

        // Counted in one go rather than per record; see RefreshInstances.
        SyncStats.RefreshInstanceCalls += Count;

        ResetParallelBuckets();
        Jobs::ParallelFor(Count, [this, &Pools](const Jobs::FParallelRange& Range)
        {
            TVector<uint32>& Bucket = BucketFor(Range.Thread).Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                ApplyTransformRecord(Pools, TransformRecords[i], &Bucket);
            }
        }, 64);

        MergeParallelDirtySlots();
    }

    void FScenePrimitiveSet::ResetParallelBuckets()
    {
        const uint32 NumSlots = Math::Max(Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 0u, 1u);
        if ((uint32)ParallelDirtySlots.Num() < NumSlots)
        {
            ParallelDirtySlots.SetNum(NumSlots);
        }
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Slots.Reset();
            Bucket.StaticSlots.Reset();
        }
    }

    void FScenePrimitiveSet::MergeParallelDirtySlots()
    {
        using namespace ScenePrimitiveSetInternal;

        const int32 SlotCount = RetainedCullEntries.Num();

        SIZE_T Total = 0;
        SIZE_T StaticTotal = 0;
        for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Total       += Bucket.Slots.Num();
            StaticTotal += Bucket.StaticSlots.Num();
        }

        if (Total != 0 && !bFullInstanceUpload)
        {
            if (SlotCount > 1024 && ShouldResendAllInstances(DirtyInstanceSlots.Num() + Total, SlotCount))
            {
                bFullInstanceUpload = true;
                DirtyInstanceSlots.Reset();
            }
            else
            {
                ReserveGeometric(DirtyInstanceSlots, DirtyInstanceSlots.Num() + Total);
                for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
                {
                    DirtyInstanceSlots.Append(Bucket.Slots);
                }
            }
        }

        if (StaticTotal != 0 && !bFullStaticUpload)
        {
            if (SlotCount > 1024 && (DirtyStaticSlots.Num() + StaticTotal) * 4 >= (SIZE_T)SlotCount)
            {
                bFullStaticUpload = true;
                DirtyStaticSlots.Reset();
            }
            else
            {
                ReserveGeometric(DirtyStaticSlots, DirtyStaticSlots.Num() + StaticTotal);
                for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
                {
                    DirtyStaticSlots.Append(Bucket.StaticSlots);
                }
            }
        }
    }

    // Only handles rebuilt since the last sweep are walked, through their primitive lists, and foliage per type.
    void FScenePrimitiveSet::SweepStaleResolves(ECS::FRegistry& Registry, const FSyncPools& Pools)
    {
        LUMINA_PROFILE_SECTION("Resolve Mesh Cache");
        bResolveTableChanged = false;

        FMeshResolveCache& Cache = FMeshResolveCache::Get();
        RetryScratch.Reset();

        GenSnapshot.Init(Constants::kIndexNoneU32, Cache.NumEntries());

        const uint32 NumHandles = Math::Min(Cache.NumEntries(), (uint32)HandleListHead.Num());
        for (uint32 Handle = 0; Handle < NumHandles; ++Handle)
        {
            if (Cache.GetResolvedAtGeneration(Handle) <= LastSweptTableGeneration)
            {
                continue;
            }

            const uint32 Generation = Cache.GetEntry(Handle).Generation;
            for (uint32 i = HandleListHead[Handle]; i != Constants::kIndexNoneU32; i = NextByHandle[i])
            {
                DEBUG_ASSERT(Primitives[i].ResolveHandle == Handle);
                if (Generation != (uint32)(ResolveKeys[i] >> 32))
                {
                    RetryScratch.Add(MakeKey(Primitives[i].Entity, Primitives[i].Source));
                }
            }
        }
        LastSweptTableGeneration = Cache.GetTableGeneration();

        SweepFoliageResolves(Cache);

        Algo::Sort(RetryScratch);
        RetryScratch.SetNum(static_cast<int32>(Algo::Unique(RetryScratch) - RetryScratch.begin()));

        PrimitiveSyncScratch.Reset();
        for (uint64 Target : RetryScratch)
        {
            if (KeySource(Target) != EPrimitiveSource::Foliage)
            {
                PrimitiveSyncScratch.Add({ KeyEntity(Target), KeySource(Target), EPrimitiveDirty::Data });
            }
        }
        SyncPrimitives(Registry, Pools, TSpan<const FRenderDirtyTracker::FEntry>(PrimitiveSyncScratch.GetData(), PrimitiveSyncScratch.Num()));

        for (uint64 Target : RetryScratch)
        {
            if (KeySource(Target) == EPrimitiveSource::Foliage)
            {
                SyncFoliage(Registry, Pools, KeyEntity(Target));
            }
        }
        FlushFoliageReleases();
    }

    void FScenePrimitiveSet::Sync(CWorld& World, TSpan<const ECS::FEntity> Moved)
    {
        ECS::FRegistry&      Registry = World.GetEntityRegistry();
        FRenderDirtyTracker& Tracker  = AttachDirtyConsumer(Registry);

        SyncStats = FSyncStats{};
        StreamedBlocks.Reset();
        StreamCount = SealMoveStream(Registry);
        MoveTimeRecords = StreamCount;
        if (!bStreamFrameOpen)
        {
            OpenStreamFrame();
        }
        bStreamFrameOpen = false;

        {
            LUMINA_PROFILE_SECTION("Sync/Poll");
            PollUnhookedSources(Registry, Tracker);
        }

        if (!Tracker.HasPending(DirtyConsumer) && !bResolveTableChanged && Moved.IsEmpty() && StreamedLastFrame.IsEmpty() && StreamedThisFrame.IsEmpty()
            && MoveReplay.IsEmpty() && PendingForegroundRefresh == EForegroundRefresh::None)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        if (Tracker.ConsumeFullRescan(DirtyConsumer))
        {
            // Drain and discard, since the rescan supersedes every queued entry.
            DrainScratch.Reset();
            Tracker.Drain(DirtyConsumer, DrainScratch);
            DrainScratch.Reset();
            InstanceEditScratch.Reset();
            Tracker.DrainInstanceEdits(DirtyConsumer, InstanceEditScratch);
            InstanceEditScratch.Reset();
            FullRescan(Registry);
            PublishSyncStats();
            return;
        }

        const FSyncPools Pools(Registry);
        ApplyForegroundRefresh(Pools);

        {
            LUMINA_PROFILE_SECTION("Sync/Drain");
            DrainScratch.Reset();
            Tracker.Drain(DirtyConsumer, DrainScratch);
        }
        SyncStats.DrainEntries = (uint32)(DrainScratch.Num() + Moved.Num());

        CoalesceDrain(Moved);
        ReserveForDrain();

        {
            LUMINA_PROFILE_SECTION("Sync/Apply");

            PartitionDrain();
            ApplyStructuralRecords(Registry, Pools);
            ApplyFoliageInstanceEdits(Pools, Tracker);
            ApplyTransformRecords(Pools);
            ApplyMovedTransforms(Pools, Moved);
        }

        if (bResolveTableChanged)
        {
            SweepStaleResolves(Registry, Pools);
        }

        PublishSyncStats();
    }

    void FScenePrimitiveSet::PublishSyncStats() const
    {
        LUMINA_PROFILE_VALUE("Sync/DrainEntries",     SyncStats.DrainEntries);
        LUMINA_PROFILE_VALUE("Sync/Coalesced",        SyncStats.CoalescedEntities);
        LUMINA_PROFILE_VALUE("Sync/TransformRecords", SyncStats.TransformRecords);
        LUMINA_PROFILE_VALUE("Sync/StructuralRecords",SyncStats.StructuralRecords);
        LUMINA_PROFILE_VALUE("Sync/SyncEntity",       SyncStats.SyncEntityCalls);
        LUMINA_PROFILE_VALUE("Sync/SyncFoliage",      SyncStats.SyncFoliageCalls);
        LUMINA_PROFILE_VALUE("Sync/BindSurfaces",     SyncStats.BindCalls);
        LUMINA_PROFILE_VALUE("Sync/BindsSkipped",     SyncStats.BindsSkipped);
        LUMINA_PROFILE_VALUE("Sync/RefreshInstances", SyncStats.RefreshInstanceCalls);
        LUMINA_PROFILE_VALUE("Sync/DirtySlots",       DirtyInstanceSlots.Num());
        LUMINA_PROFILE_VALUE("Sync/Primitives",       Primitives.Num());
        LUMINA_PROFILE_VALUE("Sync/FoliageInstances", FoliageInstanceCount);
        LUMINA_PROFILE_VALUE("Sync/GrassSlots",       GrassSlotCount);
    }
}
