#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "ScenePrimitiveSetInternal.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Algorithm.h"
#include "Containers/ConcurrentQueue.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina
{
    namespace ScenePrimitiveSetInternal
    {
        FORCEINLINE uint64 PackDirty(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags)
        {
            return (uint64)Entity.Value | ((uint64)Source << 32) | ((uint64)Flags << 40);
        }

        // Linear scan over a list that stays tiny (one entry per material instance of one master).
        void NoteDeferredMaterial(FSceneBatchRegistry::FBatch& Batch, const FResolvedSurface& Surface)
        {
            if (Surface.DeferredShader == nullptr || Surface.MaterialIdx == Constants::kIndexNoneU32
                || Surface.BatchKey.bTranslucent != 0u)
            {
                return;
            }

            for (FSceneBatchRegistry::FDeferredMaterialSlot& Existing : Batch.DeferredMaterials)
            {
                if (Existing.MaterialIndex != Surface.MaterialIdx)
                {
                    continue;
                }
                Existing.DeferredShader = Surface.DeferredShader;
                return;
            }

            Batch.DeferredMaterials.Emplace(Surface.MaterialIdx, Surface.DeferredShader);
        }

        // Whether the batch still records the shader this surface resolved to for its material slot.
        bool DeferredMaterialMatches(const FSceneBatchRegistry::FBatch& Batch, const FResolvedSurface& Surface)
        {
            if (Surface.DeferredShader == nullptr || Surface.MaterialIdx == Constants::kIndexNoneU32
                || Surface.BatchKey.bTranslucent != 0u)
            {
                return true;
            }

            for (const FSceneBatchRegistry::FDeferredMaterialSlot& Existing : Batch.DeferredMaterials)
            {
                if (Existing.MaterialIndex == Surface.MaterialIdx)
                {
                    return Existing.DeferredShader == Surface.DeferredShader;
                }
            }

            return false;
        }

        // Batch and surface desc are minted by the caller, so the slot-independent rest of a binding comes from the surface alone.
        FSurfaceBinding MakeSurfaceProto(const FResolvedSurface& Surface, uint32 BatchIndex, uint32 SurfaceDescIndex)
        {
            FSurfaceBinding Proto;
            Proto.BatchIndex            = BatchIndex;
            Proto.InstanceSlot          = Constants::kIndexNoneU32;
            Proto.SurfaceDescIndex      = SurfaceDescIndex;
            Proto.MaterialIndex         = Surface.MaterialIdx;
            Proto.MaterialFlags         = Surface.MaterialFlags;
            Proto.bMaterialCastsShadows = Surface.bMaterialCastsShadows;
            Proto.TexelFactor           = Surface.TexelFactor;
            return Proto;
        }

        FORCEINLINE bool ProtoMatches(const FSurfaceBinding& Have, const FSurfaceBinding& Want)
        {
            return Have.BatchIndex            == Want.BatchIndex
                && Have.SurfaceDescIndex      == Want.SurfaceDescIndex
                && Have.MaterialIndex         == Want.MaterialIndex
                && Have.MaterialFlags         == Want.MaterialFlags
                && Have.bMaterialCastsShadows == Want.bMaterialCastsShadows;
        }

        void ReleasePrimitiveBoneSlice(FBoneSliceArena& Arena, FScenePrimitive& Prim)
        {
            if (Prim.BoneSliceBase != kNoBoneSlice)
            {
                Arena.Release(Prim.BoneSliceBase, Prim.BoneSliceCount);
            }
            Prim.BoneSliceBase      = kNoBoneSlice;
            Prim.BoneSliceCount     = 0u;
            Prim.UploadedSliceBase  = kNoBoneSlice;
            Prim.UploadedPoseSerial = 0u;
        }
    }

    //~ Dirty tracker

    struct FRenderDirtyTracker::FImpl
    {
        using FQueue = TConcurrentQueue<uint64>;

        struct FConsumer
        {
            FQueue              Entries;
            FQueue              InstanceEdits;
            std::atomic<bool>   bAnyDirty{ false };
            std::atomic<bool>   bFullRescan{ true };
        };

        // Never freed once made, so a mark racing a consumer's removal still writes into live memory.
        TUniquePtr<FConsumer> Consumers[kMaxConsumers];

        static void Discard(FQueue& Queue)
        {
            uint64 Batch[256];
            while (Queue.DequeueBulk(Batch, 256) != 0)
            {
            }
        }
    };

    FRenderDirtyTracker::FRenderDirtyTracker()
        : Impl(Memory::New<FImpl>())
    {}

    FRenderDirtyTracker::~FRenderDirtyTracker()
    {
        Memory::Delete(Impl);
    }

    FRenderDirtyTracker* FRenderDirtyTracker::Find(ECS::FRegistry& Registry)
    {
        return Registry.Ctx().Find<FRenderDirtyTracker>();
    }

    FRenderDirtyTracker& FRenderDirtyTracker::Ensure(ECS::FRegistry& Registry)
    {
        return Registry.Ctx().GetOrEmplace<FRenderDirtyTracker>();
    }

    uint32 FRenderDirtyTracker::AddConsumer()
    {
        for (uint32 Index = 0; Index < kMaxConsumers; ++Index)
        {
            if ((ConsumerMask.load(std::memory_order_relaxed) & (1u << Index)) != 0)
            {
                continue;
            }
            if (!Impl->Consumers[Index])
            {
                Impl->Consumers[Index] = MakeUnique<FImpl::FConsumer>();
            }
            FImpl::FConsumer& Consumer = *Impl->Consumers[Index];
            FImpl::Discard(Consumer.Entries);
            FImpl::Discard(Consumer.InstanceEdits);
            Consumer.bAnyDirty.store(false, std::memory_order_relaxed);
            Consumer.bFullRescan.store(true, std::memory_order_relaxed);
            ConsumerMask.fetch_or(1u << Index, std::memory_order_release);
            return Index;
        }
        LOG_ERROR("RenderDirtyTracker: all {} consumers of this registry are taken, so the next render scene rescans every frame.", kMaxConsumers);
        return Constants::kIndexNoneU32;
    }

    void FRenderDirtyTracker::RemoveConsumer(uint32 Consumer)
    {
        if (Consumer >= kMaxConsumers || !Impl->Consumers[Consumer])
        {
            return;
        }
        ConsumerMask.fetch_and(~(1u << Consumer), std::memory_order_acq_rel);
        FImpl::Discard(Impl->Consumers[Consumer]->Entries);
        FImpl::Discard(Impl->Consumers[Consumer]->InstanceEdits);
    }

    void FRenderDirtyTracker::Mark(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags) const
    {
        if (Entity == ECS::NullEntity || Flags == EPrimitiveDirty::None)
        {
            return;
        }
        const uint64 Packed = ScenePrimitiveSetInternal::PackDirty(Entity, Source, Flags);
        for (uint32 Bits = ConsumerMask.load(std::memory_order_acquire); Bits != 0; Bits &= Bits - 1u)
        {
            FImpl::FConsumer& Consumer = *Impl->Consumers[Math::CountTrailingZeros64(Bits)];
            Consumer.Entries.Enqueue(Packed);
            Consumer.bAnyDirty.store(true, std::memory_order_release);
        }
    }

    void FRenderDirtyTracker::MarkAllSources(ECS::FEntity Entity, EPrimitiveDirty Flags) const
    {
        Mark(Entity, EPrimitiveSource::AnySource, Flags);
    }

    void FRenderDirtyTracker::MarkInstanceVisibility(ECS::FEntity Entity, uint32 Instance) const
    {
        if (Entity == ECS::NullEntity)
        {
            return;
        }
        const uint64 Packed = (uint64)Entity.Value | ((uint64)Instance << 32);
        for (uint32 Bits = ConsumerMask.load(std::memory_order_acquire); Bits != 0; Bits &= Bits - 1u)
        {
            FImpl::FConsumer& Consumer = *Impl->Consumers[Math::CountTrailingZeros64(Bits)];
            Consumer.InstanceEdits.Enqueue(Packed);
            Consumer.bAnyDirty.store(true, std::memory_order_release);
        }
    }

    bool FRenderDirtyTracker::HasPending(uint32 Consumer) const
    {
        if (Consumer >= kMaxConsumers || !Impl->Consumers[Consumer])
        {
            return true;
        }
        const FImpl::FConsumer& State = *Impl->Consumers[Consumer];
        return State.bAnyDirty.load(std::memory_order_acquire) || State.bFullRescan.load(std::memory_order_relaxed);
    }

    bool FRenderDirtyTracker::Drain(uint32 Consumer, TVector<FEntry>& Out)
    {
        if (Consumer >= kMaxConsumers || !Impl->Consumers[Consumer])
        {
            return false;
        }
        FImpl::FConsumer& State = *Impl->Consumers[Consumer];

        // Cleared before the queue is read, acquiring the mark that raised it, so a mark landing during the drain raises it again for next frame.
        (void)State.bAnyDirty.exchange(false, std::memory_order_acq_rel);

        const int32 Before = Out.Num();
        uint64 Batch[256];
        std::size_t Count;
        while ((Count = State.Entries.DequeueBulk(Batch, 256)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                const uint64 Packed = Batch[i];
                FEntry& Entry = Out.EmplaceGetRef();
                Entry.Entity = (ECS::FEntity)(uint32)(Packed & 0xFFFFFFFFull);
                Entry.Source = (EPrimitiveSource)((Packed >> 32) & 0xFFull);
                Entry.Flags  = (EPrimitiveDirty)((Packed >> 40) & 0xFFull);
            }
        }
        return Out.Num() != Before;
    }

    bool FRenderDirtyTracker::DrainInstanceEdits(uint32 Consumer, TVector<FInstanceEdit>& Out)
    {
        if (Consumer >= kMaxConsumers || !Impl->Consumers[Consumer])
        {
            return false;
        }

        const int32 Before = Out.Num();
        uint64 Batch[256];
        std::size_t Count;
        while ((Count = Impl->Consumers[Consumer]->InstanceEdits.DequeueBulk(Batch, 256)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                Out.Emplace((ECS::FEntity)(uint32)(Batch[i] & 0xFFFFFFFFull), (uint32)(Batch[i] >> 32));
            }
        }
        return Out.Num() != Before;
    }

    void FRenderDirtyTracker::RequestFullRescan()
    {
        for (uint32 Bits = ConsumerMask.load(std::memory_order_acquire); Bits != 0; Bits &= Bits - 1u)
        {
            RequestFullRescan((uint32)Math::CountTrailingZeros64(Bits));
        }
    }

    void FRenderDirtyTracker::RequestFullRescan(uint32 Consumer)
    {
        if (Consumer < kMaxConsumers && Impl->Consumers[Consumer])
        {
            Impl->Consumers[Consumer]->bFullRescan.store(true, std::memory_order_relaxed);
        }
    }

    bool FRenderDirtyTracker::ConsumeFullRescan(uint32 Consumer)
    {
        if (Consumer >= kMaxConsumers || !Impl->Consumers[Consumer])
        {
            return true;
        }
        return Impl->Consumers[Consumer]->bFullRescan.exchange(false, std::memory_order_relaxed);
    }

    //~ Batch registry

    uint32 FSceneBatchRegistry::FindOrAddBatch(const FResolvedSurface& Surface)
    {
        const uint64 KeyHash = GetTypeHash(Surface.BatchKey);

        TVector<uint32>& Bucket = BatchesByHash.FindOrAdd(KeyHash);
        for (uint32 Index : Bucket)
        {
            if (Batches[Index].Key == Surface.BatchKey)
            {
                ScenePrimitiveSetInternal::NoteDeferredMaterial(Batches[Index], Surface);
                return Index;
            }
        }

        uint32 NewIndex;
        if (!FreeBatches.IsEmpty())
        {
            NewIndex = FreeBatches.Last();
            FreeBatches.Pop();
            Batches[NewIndex] = FBatch{};
        }
        else
        {
            NewIndex = (uint32)Batches.Num();
            if (NewIndex == 0x10000u)
            {
                LOG_ERROR("SceneBatchRegistry: batch count reached {}, the limit PackDrawIDAndFlags can "
                          "address. Draw ids past this wrap.", 0x10000u);
            }
            Batches.Emplace();
        }

        FBatch& Batch = Batches[NewIndex];
        Batch.Key                             = Surface.BatchKey;
        Batch.VertexShader                    = Surface.VertexShader;
        Batch.MeshShaderShadow                = Surface.MeshShaderShadow;
        Batch.MeshShaderBase                  = Surface.MeshShaderBase;
        Batch.PixelShader                     = Surface.PixelShader;
        Batch.VisBufferMeshShader             = Surface.VisBufferMeshShader;
        Batch.VisBufferMeshShaderMasked       = Surface.VisBufferMeshShaderMasked;
        Batch.MaskedVisBufferPixelShader      = Surface.MaskedVisBufferPixelShader;
        Batch.MeshShaderShadowMasked          = Surface.MeshShaderShadowMasked;
        Batch.ShadowMaskedPixelShader         = Surface.ShadowMaskedPixelShader;
        ScenePrimitiveSetInternal::NoteDeferredMaterial(Batch, Surface);

        Bucket.Add(NewIndex);
        return NewIndex;
    }

    void FSceneBatchRegistry::AddBatchRef(uint32 BatchIndex, bool bSkinned)
    {
        AddBatchRefs(BatchIndex, bSkinned, 1u);
    }

    void FSceneBatchRegistry::AddBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count)
    {
        FBatch& Batch = Batches[BatchIndex];
        Batch.RefCount += Count;
        (bSkinned ? Batch.SkinnedRefCount : Batch.StaticRefCount) += Count;
    }

    void FSceneBatchRegistry::ReleaseBatchRef(uint32 BatchIndex, bool bSkinned)
    {
        ReleaseBatchRefs(BatchIndex, bSkinned, 1u);
    }

    void FSceneBatchRegistry::ReleaseBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count)
    {
        if (BatchIndex >= (uint32)Batches.Num() || Count == 0u)
        {
            return;
        }

        FBatch& Batch = Batches[BatchIndex];
        Batch.RefCount -= Math::Min(Batch.RefCount, Count);

        uint32& Kind = bSkinned ? Batch.SkinnedRefCount : Batch.StaticRefCount;
        Kind -= Math::Min(Kind, Count);

        if (Batch.RefCount == 0u)
        {
            RetireBatch(BatchIndex);
        }
    }

    // The slot index stays reserved for reuse; the GPU bucket arrays are sized by slot count, not by live batches.
    void FSceneBatchRegistry::RetireBatch(uint32 BatchIndex)
    {
        FBatch& Batch = Batches[BatchIndex];

        if (TVector<uint32>* Bucket = BatchesByHash.Find(GetTypeHash(Batch.Key)))
        {
            for (int32 i = 0; i < Bucket->Num(); ++i)
            {
                if ((*Bucket)[i] == BatchIndex)
                {
                    Bucket->RemoveAtSwap(i);
                    break;
                }
            }
        }

        Batch = FBatch{};
        FreeBatches.Add(BatchIndex);
        ++RecycleGeneration;
    }

    void FSceneBatchRegistry::Reset()
    {
        Batches.Reset();
        BatchesByHash.Reset();
        FreeBatches.Reset();
        ++RecycleGeneration;
    }

    //~ Bone slice arena

    uint32 FBoneSliceArena::Acquire(uint32 Count)
    {
        if (Count < (uint32)FreeLists.Num() && !FreeLists[Count].IsEmpty())
        {
            const uint32 Base = FreeLists[Count].Last();
            FreeLists[Count].Pop();
            return Base;
        }
        const uint32 Base = Extent;
        Extent += Count;
        return Base;
    }

    void FBoneSliceArena::Release(uint32 Base, uint32 Count)
    {
        if (Count == 0u)
        {
            return;
        }
        if (Count >= (uint32)FreeLists.Num())
        {
            FreeLists.SetNum(Count + 1u);
        }
        FreeLists[Count].Add(Base);
    }

    void FBoneSliceArena::Reset()
    {
        FreeLists.Reset();
        Extent = 0;
    }

    //~ Per entity records

    FScenePrimitiveSet::FEntityRecord& FScenePrimitiveSet::FEntityRecordTable::FindOrAdd(ECS::FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        const uint32 Page  = Index >> kPageShift;
        if (Page >= (uint32)Pages.Num())
        {
            Pages.SetNum(Page + 1u);
        }
        if (Pages[Page].IsEmpty())
        {
            Pages[Page].SetNum(1u << kPageShift);
        }
        return Pages[Page][Index & kPageMask];
    }

    uint32 FScenePrimitiveSet::FindLinked(ECS::FEntity Entity, EPrimitiveSource Source) const
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const FEntityRecord* Record = EntityRecords.Find(Entity);
        return Record != nullptr && Record->Entity == Entity.Value ? Record->Links[(uint32)Source] : Constants::kIndexNoneU32;
    }

    uint8 FScenePrimitiveSet::GetSourceMask(ECS::FEntity Entity) const
    {
        const FEntityRecord* Record = EntityRecords.Find(Entity);
        if (Record == nullptr || Record->Entity != Entity.Value)
        {
            return 0u;
        }

        uint8 Mask = 0u;
        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            Mask |= (Record->Links[s] != Constants::kIndexNoneU32) ? (uint8)(1u << s) : (uint8)0u;
        }
        return Mask;
    }

    void FScenePrimitiveSet::SetLink(ECS::FEntity Entity, EPrimitiveSource Source, uint32 Index)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        FEntityRecord& Record = EntityRecords.FindOrAdd(Entity);
        if (Record.Entity != Entity.Value)
        {
            Record.Entity           = Entity.Value;
            Record.StreamGeneration = 0u;
            for (uint32& Link : Record.Links)
            {
                Link = Constants::kIndexNoneU32;
            }
            SetMovedInterest(Entity, true);
        }
        Record.Links[(uint32)Source] = Index;
    }

    void FScenePrimitiveSet::ClearLink(ECS::FEntity Entity, EPrimitiveSource Source)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        FEntityRecord* Record = EntityRecords.Find(Entity);
        if (Record == nullptr || Record->Entity != Entity.Value)
        {
            return;
        }

        Record->Links[(uint32)Source] = Constants::kIndexNoneU32;
        for (uint32 Link : Record->Links)
        {
            if (Link != Constants::kIndexNoneU32)
            {
                return;
            }
        }
        Record->Entity           = Constants::kIndexNoneU32;
        Record->StreamGeneration = 0u;
        SetMovedInterest(Entity, false);
    }

    void FScenePrimitiveSet::SetMovedInterest(ECS::FEntity Entity, bool bInterested)
    {
        if (BoundRegistry != nullptr && MovedConsumer != Constants::kIndexNoneU32)
        {
            ECS::Utils::SetMovedTransformInterest(*BoundRegistry, MovedConsumer, Entity, bInterested);
        }
    }

    void FScenePrimitiveSet::ResetEntityRecords()
    {
        EntityRecords.ForEach([this](const FEntityRecord& Record)
        {
            if (Record.Entity != Constants::kIndexNoneU32)
            {
                SetMovedInterest((ECS::FEntity)Record.Entity, false);
            }
        });
        EntityRecords.Reset();
    }

    void FScenePrimitiveSet::SetMovedTransformConsumer(ECS::FRegistry& Registry, uint32 Consumer)
    {
        BoundRegistry = &Registry;
        MovedConsumer = Consumer;
        EntityRecords.ForEach([this](const FEntityRecord& Record)
        {
            if (Record.Entity != Constants::kIndexNoneU32)
            {
                SetMovedInterest((ECS::FEntity)Record.Entity, true);
            }
        });
    }

    //~ Primitives

    uint32 FScenePrimitiveSet::AddPrimitive(ECS::FEntity Entity, EPrimitiveSource Source)
    {
        const uint32 Index = (uint32)Primitives.Num();
        FScenePrimitive& Prim = Primitives.EmplaceGetRef();
        Prim.Entity = Entity;
        Prim.Source = Source;
        Bounds.Emplace(FVector4(0.0f));
        CullData.Emplace();
        ResolveKeys.Add(PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0));
        NextByHandle.Add(Constants::kIndexNoneU32);
        PrevByHandle.Add(Constants::kIndexNoneU32);

        SetLink(Entity, Source, Index);
        if (Source == EPrimitiveSource::SkeletalMesh)
        {
            ++BonelessSkinnedCount;
            Prim.SkeletalSlot = (uint32)SkeletalIndices.Num();
            SkeletalIndices.Add(Index);
        }
        return Index;
    }

    void FScenePrimitiveSet::RemovePrimitive(uint32 Index)
    {
        const uint32 Last = (uint32)Primitives.Num() - 1u;

        FScenePrimitive& Prim = Primitives[Index];
        if (Prim.Source == EPrimitiveSource::SkeletalMesh && Prim.BoneCount == 0u && BonelessSkinnedCount > 0u)
        {
            --BonelessSkinnedCount;
        }
        if (Prim.CustomDepthStencil != 0u)
        {
            --CustomDepthCount;
        }

        UntrackForeground(Index);
        ReleaseBindings(Index);
        ScenePrimitiveSetInternal::ReleasePrimitiveBoneSlice(BoneSlices, Prim);
        Prim.BoneCount = 0u;
        UnlinkHandle(Index, Prim.ResolveHandle);

        const ECS::FEntity     Entity = Prim.Entity;
        const EPrimitiveSource Source = Prim.Source;

        if (Prim.SkeletalSlot != Constants::kIndexNoneU32)
        {
            const uint32 Moved = SkeletalIndices.Last();
            SkeletalIndices[Prim.SkeletalSlot] = Moved;
            Primitives[Moved].SkeletalSlot     = Prim.SkeletalSlot;
            SkeletalIndices.Pop();
        }

        if (Index != Last)
        {
            if (Primitives[Last].SkeletalSlot != Constants::kIndexNoneU32)
            {
                SkeletalIndices[Primitives[Last].SkeletalSlot] = Index;
            }

            Primitives[Index]  = Move(Primitives[Last]);
            Bounds[Index]      = Bounds[Last];
            CullData[Index]    = CullData[Last];
            ResolveKeys[Index] = ResolveKeys[Last];

            // The moved primitive keeps its handle list membership under its new index.
            const uint32 MovedNext = NextByHandle[Last];
            const uint32 MovedPrev = PrevByHandle[Last];
            NextByHandle[Index] = MovedNext;
            PrevByHandle[Index] = MovedPrev;
            if (MovedPrev != Constants::kIndexNoneU32)
            {
                NextByHandle[MovedPrev] = Index;
            }
            else if (Primitives[Index].ResolveHandle < (uint32)HandleListHead.Num()
                     && HandleListHead[Primitives[Index].ResolveHandle] == Last)
            {
                HandleListHead[Primitives[Index].ResolveHandle] = Index;
            }
            if (MovedNext != Constants::kIndexNoneU32)
            {
                PrevByHandle[MovedNext] = Index;
            }

            SetLink(Primitives[Index].Entity, Primitives[Index].Source, Index);
        }

        Primitives.Pop();
        Bounds.Pop();
        CullData.Pop();
        ResolveKeys.Pop();
        NextByHandle.Pop();
        PrevByHandle.Pop();

        ClearLink(Entity, Source);
    }

    FVector4 FScenePrimitiveSet::RebuildWorldBounds(uint32 Index, const FMatrix4& Transform)
    {
        FScenePrimitive& Prim = Primitives[Index];
        const float CullScale = Math::Max(Prim.BoundsScale, 1.0f);
        if (Prim.Source == EPrimitiveSource::SkeletalMesh)
        {
            Prim.WorldMaxScale = ScenePrimitiveSetInternal::TransformMaxScale(Transform);
        }
        Bounds[Index] = SceneCull::TransformSphere(Transform, Prim.LocalCenter, Prim.LocalRadius * CullScale);
        return Bounds[Index];
    }

    void FScenePrimitiveSet::SetBoneCount(uint32 Index, uint32 Count)
    {
        FScenePrimitive& Prim = Primitives[Index];
        if (Prim.Source == EPrimitiveSource::SkeletalMesh && (Prim.BoneCount == 0u) != (Count == 0u))
        {
            BonelessSkinnedCount += (Count == 0u) ? 1u : ~0u;
        }
        Prim.BoneCount = Count;
    }

    void FScenePrimitiveSet::LinkHandle(uint32 Index, uint32 Handle)
    {
        if (Handle == INVALID_MESH_RESOLVE_HANDLE)
        {
            return;
        }
        if (Handle >= (uint32)HandleListHead.Num())
        {
            HandleListHead.SetNum(Handle + 1u, Constants::kIndexNoneU32);
        }

        const uint32 Head = HandleListHead[Handle];
        NextByHandle[Index] = Head;
        PrevByHandle[Index] = Constants::kIndexNoneU32;
        if (Head != Constants::kIndexNoneU32)
        {
            PrevByHandle[Head] = Index;
        }
        HandleListHead[Handle] = Index;
    }

    void FScenePrimitiveSet::UnlinkHandle(uint32 Index, uint32 Handle)
    {
        if (Handle == INVALID_MESH_RESOLVE_HANDLE || Handle >= (uint32)HandleListHead.Num())
        {
            return;
        }

        const uint32 Next = NextByHandle[Index];
        const uint32 Prev = PrevByHandle[Index];
        if (Prev != Constants::kIndexNoneU32)
        {
            NextByHandle[Prev] = Next;
        }
        else if (HandleListHead[Handle] == Index)
        {
            HandleListHead[Handle] = Next;
        }
        if (Next != Constants::kIndexNoneU32)
        {
            PrevByHandle[Next] = Prev;
        }
        NextByHandle[Index] = Constants::kIndexNoneU32;
        PrevByHandle[Index] = Constants::kIndexNoneU32;
    }

    //~ Bone slices

    uint32 FScenePrimitiveSet::TouchBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber)
    {
        FScenePrimitive& Prim = Primitives[Index];
        Prim.BoneSliceFrame = FrameNumber;

        return (Prim.BoneSliceBase != kNoBoneSlice && Prim.BoneSliceCount == Count)
             ? Prim.BoneSliceBase
             : kNoBoneSlice;
    }

    uint32 FScenePrimitiveSet::AcquireBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber)
    {
        FScenePrimitive& Prim = Primitives[Index];
        Prim.BoneSliceFrame = FrameNumber;

        if (Prim.BoneSliceBase != kNoBoneSlice && Prim.BoneSliceCount == Count)
        {
            return Prim.BoneSliceBase;
        }

        ScenePrimitiveSetInternal::ReleasePrimitiveBoneSlice(BoneSlices, Prim);
        if (Count == 0u)
        {
            return kNoBoneSlice;
        }

        Prim.BoneSliceBase  = BoneSlices.Acquire(Count);
        Prim.BoneSliceCount = Count;
        return Prim.BoneSliceBase;
    }

    void FScenePrimitiveSet::ReleaseStaleBoneSlices(uint32 FrameNumber, uint32 GraceFrames)
    {
        const uint32 Num = (uint32)SkeletalIndices.Num();
        if (Num == 0u)
        {
            BoneSliceSweepCursor = 0u;
            return;
        }

        const uint32 Budget = Math::Min(Math::Max(Num / 32u, 64u), Num);
        for (uint32 n = 0; n < Budget; ++n)
        {
            const uint32 i = BoneSliceSweepCursor % Num;
            BoneSliceSweepCursor = i + 1u;

            FScenePrimitive& Prim = Primitives[SkeletalIndices[i]];
            if (Prim.BoneSliceBase != kNoBoneSlice && (FrameNumber - Prim.BoneSliceFrame) > GraceFrames)
            {
                ScenePrimitiveSetInternal::ReleasePrimitiveBoneSlice(BoneSlices, Prim);
            }
        }
    }

    //~ Bindings

    void FScenePrimitiveSet::CompactBindings()
    {
        // Pending releases still point into the old layout, so they go before anything moves.
        FlushFoliageReleases();

        TVector<FSurfaceBinding> Packed;
        Packed.Reserve(Bindings.Num() - DeadBindings);

        const auto Repack = [this, &Packed](uint32& BindingBase, uint32 SurfaceCount)
        {
            if (SurfaceCount == 0u)
            {
                BindingBase = 0u;
                return;
            }
            const uint32 NewBase = (uint32)Packed.Num();
            Packed.Append(Bindings.begin() + BindingBase, Bindings.begin() + BindingBase + SurfaceCount);
            BindingBase = NewBase;
        };

        for (FScenePrimitive& Prim : Primitives)
        {
            Repack(Prim.BindingBase, Prim.SurfaceCount);
        }

        // Foliage instances hold spans in the same array without owning a primitive, so they rebase here too.
        for (uint32 Dense = 0; Dense < FoliageByEntity.Num(); ++Dense)
        {
            for (FFoliageInstanceRef& Ref : FoliageByEntity.GetValueAt(Dense).Instances)
            {
                Repack(Ref.BindingBase, Ref.SurfaceCount);
            }
        }

        swap(Bindings, Packed);
        DeadBindings = 0;
        BindingFreeRanges.Reset();
        FreeRangeBindings = 0;
    }

    bool FScenePrimitiveSet::ShouldCompactBindings() const
    {
        const uint32 Wasted = DeadBindings - Math::Min(DeadBindings, FreeRangeBindings);
        return (Wasted > 1024 && Wasted * 4 > (uint32)Bindings.Num())
            || FreeRangeBindings > (uint32)Bindings.Num() / 2u;
    }

    uint32 FScenePrimitiveSet::TakeBindingRange(uint32 Count)
    {
        for (int32 i = 0; i < BindingFreeRanges.Num() && Count != 0u; ++i)
        {
            FUIntVector2& Range = BindingFreeRanges[i];
            if (Range.y < Count)
            {
                continue;
            }

            const uint32 Base = Range.x;
            Range.x += Count;
            Range.y -= Count;
            if (Range.y == 0u)
            {
                BindingFreeRanges.RemoveAtSwap(i);
            }
            DeadBindings      -= Math::Min(DeadBindings, Count);
            FreeRangeBindings -= Math::Min(FreeRangeBindings, Count);
            return Base;
        }

        const uint32 Base = (uint32)Bindings.Num();
        Bindings.SetNum(Base + Count);
        return Base;
    }

    void FScenePrimitiveSet::ReleaseBindingSpan(uint32 Base, uint32 Count, TVector<FUIntVector3>* DeferredBatchReleases)
    {
        for (uint32 s = 0; s < Count; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Base + s];
            if (DeferredBatchReleases != nullptr)
            {
                ScenePrimitiveSetInternal::DeferBatchRelease(*DeferredBatchReleases, Binding.BatchIndex, Binding.bSkinned);
            }
            else
            {
                Batches.ReleaseBatchRef(Binding.BatchIndex, Binding.bSkinned);
            }
            FreeInstanceSlot(Binding.InstanceSlot);
            FreeSkinnedIndex(Binding.SkinnedIndex);
        }
        DeadBindings += Count;
    }

    void FScenePrimitiveSet::FlushDeferredBatchReleases(TVector<FUIntVector3>& DeferredBatchReleases)
    {
        for (const FUIntVector3& Run : DeferredBatchReleases)
        {
            Batches.ReleaseBatchRefs(Run.x, Run.y != 0u, Run.z);
        }
        DeferredBatchReleases.Reset();
    }

    uint32 FScenePrimitiveSet::AppendBindingSpan(const FBindingMemo& Memo, bool bSkinned, bool bAssignSkinnedIndex)
    {
        const uint32 Base = (uint32)Bindings.Num();
        for (const FSurfaceBinding& Proto : Memo.Protos)
        {
            FSurfaceBinding& Binding = Bindings.EmplaceGetRef(Proto);
            Binding.InstanceSlot = AllocateInstanceSlot();
            Binding.bSkinned     = bSkinned;
            Binding.SkinnedIndex = (bSkinned && bAssignSkinnedIndex) ? AllocateSkinnedIndex() : kNoSkinnedIndex;

            Batches.AddBatchRef(Binding.BatchIndex, bSkinned);
        }
        return Base;
    }

    void FScenePrimitiveSet::ReleaseBindings(uint32 Index)
    {
        FScenePrimitive& Prim = Primitives[Index];
        ReleaseBindingSpan(Prim.BindingBase, Prim.SurfaceCount);
        Prim.SurfaceCount = 0;
        Prim.BindingBase  = 0;
    }

    const FScenePrimitiveSet::FBindingMemo* FScenePrimitiveSet::EnsureBindingMemo(
        uint32 ResolveHandle, uint32 Generation, const TVector<FResolvedSurface>& Surfaces)
    {
        // Dynamic meshes carry no resolve handle; the caller binds them per-surface instead.
        if (ResolveHandle == INVALID_MESH_RESOLVE_HANDLE)
        {
            return nullptr;
        }

        if ((uint32)BindingMemoByHandle.Num() <= ResolveHandle)
        {
            BindingMemoByHandle.SetNum(ResolveHandle + 1u);
        }

        FBindingMemo& Entry = BindingMemoByHandle[ResolveHandle];

        if (Entry.Generation != Generation
            || Entry.RecycleGeneration != Batches.GetRecycleGeneration()
            || Entry.Protos.Num() != Surfaces.Num())
        {
            Entry.Protos.Reset();
            Entry.Protos.Reserve(Surfaces.Num());

            for (const FResolvedSurface& Surface : Surfaces)
            {
                Entry.Protos.Add(ScenePrimitiveSetInternal::MakeSurfaceProto(Surface, Batches.FindOrAddBatch(Surface), InternSurfaceDesc(Surface)));
            }

            Entry.Generation        = Generation;
            Entry.RecycleGeneration = Batches.GetRecycleGeneration();
        }

        return &Entry;
    }

    bool FScenePrimitiveSet::SpanMatchesMemo(uint32 Base, uint32 Count, const FBindingMemo* Memo, bool bSkinned) const
    {
        if (Memo == nullptr)
        {
            return Count == 0u;
        }
        if (Count != (uint32)Memo->Protos.Num())
        {
            return false;
        }
        for (uint32 s = 0; s < Count; ++s)
        {
            const FSurfaceBinding& Have = Bindings[Base + s];
            if (!ScenePrimitiveSetInternal::ProtoMatches(Have, Memo->Protos[s]) || Have.bSkinned != bSkinned)
            {
                return false;
            }
        }
        return true;
    }

    // Dynamic meshes re-resolve in place, so bindings must be compared against the surfaces.
    bool FScenePrimitiveSet::BindingsMatchSurfaces(uint32 Index, const TVector<FResolvedSurface>& Surfaces) const
    {
        const FScenePrimitive& Prim = Primitives[Index];

        if (Prim.SurfaceCount != (uint32)Surfaces.Num())
        {
            return false;
        }

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding&  Have = Bindings[Prim.BindingBase + s];
            const FResolvedSurface& Want = Surfaces[s];

            if (Have.BatchIndex >= Batches.Num())
            {
                return false;
            }

            // Compares the batch's KEY, since FindOrAddBatch would mint one on the miss.
            if (!(Batches.Get(Have.BatchIndex).Key == Want.BatchKey)
             || Have.MaterialIndex         != Want.MaterialIdx
             || Have.MaterialFlags         != Want.MaterialFlags
             || Have.bMaterialCastsShadows != Want.bMaterialCastsShadows
             || !ScenePrimitiveSetInternal::DeferredMaterialMatches(Batches.Get(Have.BatchIndex), Want))
            {
                return false;
            }
        }

        return true;
    }

    void FScenePrimitiveSet::BindSurfaces(uint32 Index)
    {
        ++SyncStats.BindCalls;

        ReleaseBindings(Index);

        FScenePrimitive& Prim = Primitives[Index];
        if (Prim.Surfaces == nullptr || Prim.Surfaces->IsEmpty())
        {
            return;
        }

        const TVector<FResolvedSurface>& Surfaces = *Prim.Surfaces;
        const bool bSkinned = EnumHasAnyFlags(Prim.BaseFlags, EInstanceFlags::Skinned);

        Prim.SurfaceCount = (uint32)Surfaces.Num();
        if (const FBindingMemo* Memo = EnsureBindingMemo(Prim.ResolveHandle, Prim.ResolveGeneration, Surfaces))
        {
            Prim.BindingBase = AppendBindingSpan(*Memo, bSkinned, true);
        }
        else
        {
            Prim.BindingBase = (uint32)Bindings.Num();
            for (const FResolvedSurface& Surface : Surfaces)
            {
                const uint32 BatchIndex = Batches.FindOrAddBatch(Surface);
                Batches.AddBatchRef(BatchIndex, bSkinned);

                FSurfaceBinding& Binding = Bindings.EmplaceGetRef(ScenePrimitiveSetInternal::MakeSurfaceProto(Surface, BatchIndex, InternSurfaceDesc(Surface)));
                Binding.InstanceSlot = AllocateInstanceSlot();
                Binding.bSkinned     = bSkinned;
                Binding.SkinnedIndex = bSkinned ? AllocateSkinnedIndex() : kNoSkinnedIndex;
            }
        }

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }
    }

    //~ Retained instance slots

    uint32 FScenePrimitiveSet::AllocateInstanceSlot()
    {
        if (!InstanceFreeSlots.IsEmpty())
        {
            const uint32 Slot = InstanceFreeSlots.Last();
            InstanceFreeSlots.Pop();
            return Slot;
        }

        const uint32 Slot = (uint32)RetainedCullEntries.Num();

        RetainedCullEntries.Emplace();
        RetainedTransforms.Emplace();
        RetainedStatic.Emplace();
        RetainedProxies.Emplace();

        // The device range past the old count may hold a slot from before a reset, so a new slot always uploads.
        MarkInstanceDirty(Slot);
        MarkStaticDirty(Slot);
        return Slot;
    }

    uint32 FScenePrimitiveSet::AllocateInstanceSlotBlock(uint32 Count)
    {
        // Deliberately not the free list: the block must be contiguous so the GPU can index it from one base.
        const uint32 Base = (uint32)RetainedCullEntries.Num();

        RetainedCullEntries.SetNum(Base + Count);
        RetainedTransforms.SetNum(Base + Count);
        RetainedStatic.SetNum(Base + Count);
        RetainedProxies.SetNum(Base + Count);

        // The scatter relies on its unused slots reading inactive, so the block starts from uploaded zeros.
        for (uint32 Slot = Base; Slot < Base + Count; ++Slot)
        {
            MarkInstanceDirty(Slot);
            MarkStaticDirty(Slot);
        }
        return Base;
    }

    uint32 FScenePrimitiveSet::AllocateSkinnedIndex()
    {
        if (!SkinnedIndexFreeList.IsEmpty())
        {
            const uint32 Index = SkinnedIndexFreeList.Last();
            SkinnedIndexFreeList.Pop();
            return Index;
        }
        return SkinnedIndexExtent++;
    }

    void FScenePrimitiveSet::FreeSkinnedIndex(uint32 Index)
    {
        if (Index != kNoSkinnedIndex)
        {
            SkinnedIndexFreeList.Add(Index);
        }
    }

    void FScenePrimitiveSet::FreeInstanceSlot(uint32 Slot)
    {
        if (Slot >= (uint32)RetainedCullEntries.Num())
        {
            return;
        }

        DeactivateInstanceSlot(Slot);
        InstanceFreeSlots.Add(Slot);
    }

    void FScenePrimitiveSet::DeactivateInstanceSlot(uint32 Slot)
    {
        RetainedCullEntries[Slot].DrawIDAndFlags &= ~ScenePrimitiveSetInternal::kActiveBit;
        ReleaseMeshletVisibility(Slot);
        MarkInstanceDirty(Slot);
    }

    void FScenePrimitiveSet::MarkInstanceDirty(uint32 Slot)
    {
        // Already committed to re-sending the whole buffer; tracking individual slots is pure waste.
        if (bFullInstanceUpload)
        {
            return;
        }

        const int32 SlotCount = RetainedCullEntries.Num();
        if (SlotCount > 1024 && ShouldResendAllInstances(DirtyInstanceSlots.Num(), SlotCount))
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.Reset();
            return;
        }

        DirtyInstanceSlots.Add(Slot);
    }

    void FScenePrimitiveSet::MarkStaticDirty(uint32 Slot)
    {
        if (bFullStaticUpload)
        {
            return;
        }

        const int32 SlotCount = RetainedCullEntries.Num();
        if (SlotCount > 1024 && DirtyStaticSlots.Num() * 4 >= SlotCount)
        {
            bFullStaticUpload = true;
            DirtyStaticSlots.Reset();
            return;
        }

        DirtyStaticSlots.Add(Slot);
    }

    void FScenePrimitiveSet::ReleaseMeshletVisibility(uint32 Slot)
    {
        if (Slot < (uint32)SlotMeshletVisibility.Num())
        {
            FUIntVector3& Range = SlotMeshletVisibility[Slot];
            FreeMeshletVisibilityRange(Range.x, Range.y);
            LargestLODMeshletTotal -= Range.z;
            Range = FUIntVector3(kNoMeshletVisibility, 0u, 0u);
        }

        // A reused slot carries its payload's base forward, so a freed range must not survive in it.
        if (Slot < (uint32)RetainedStatic.Num())
        {
            RetainedStatic[Slot].MeshletVisibilityBase = kNoMeshletVisibility;
        }
    }

    uint32 FScenePrimitiveSet::LargestLODMeshletsOf(uint32 Slot) const
    {
        const FInstanceCullEntry& Cull = RetainedCullEntries[Slot];
        return (Cull.DrawIDAndFlags & ScenePrimitiveSetInternal::kActiveBit) != 0u ? LargestLODMeshletsOfSurface(Cull.SurfaceDescIndex) : 0u;
    }

    uint32 FScenePrimitiveSet::LargestLODMeshletsOfSurface(uint32 SurfaceDescIndex) const
    {
        if (SurfaceDescIndex >= (uint32)SurfaceDescs.Num())
        {
            return 0u;
        }

        const FSurfaceDescGPU& Desc = SurfaceDescs[SurfaceDescIndex];
        const bool bClusterLOD = (Desc.Flags & SURFACE_DESC_CLUSTER_LOD) != 0u;
        uint32 LargestLOD = 0u;
        for (uint32 LOD = 0; LOD < Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS); ++LOD)
        {
            // A cluster range can span every level at once, so its words cover the whole hierarchy.
            LargestLOD = bClusterLOD ? LargestLOD + Desc.LODMeshletCount[LOD] : Math::Max(LargestLOD, Desc.LODMeshletCount[LOD]);
        }
        return LargestLOD;
    }

    void FScenePrimitiveSet::UpdateMeshletVisibility(uint32 Slot)
    {
        const uint32 Meshlets = LargestLODMeshletsOf(Slot);
        const uint32 Words    = RenderUtils::GetGroupCount(Meshlets, MESHLET_CULL_GROUP_SIZE);

        FUIntVector3& Range = SlotMeshletVisibility[Slot];
        if (Range.y != Words)
        {
            ReleaseMeshletVisibility(Slot);
            if (Words != 0u)
            {
                if (Words < (uint32)MeshletVisibilityFreeLists.Num() && !MeshletVisibilityFreeLists[Words].IsEmpty())
                {
                    Range.x = MeshletVisibilityFreeLists[Words].Last();
                    MeshletVisibilityFreeLists[Words].Pop();
                }
                else
                {
                    Range.x = MeshletVisibilityExtent;
                    MeshletVisibilityExtent += Words;
                }
                Range.y = Words;
            }
        }
        LargestLODMeshletTotal = LargestLODMeshletTotal - Range.z + Meshlets;
        Range.z = Meshlets;

        if (RetainedStatic[Slot].MeshletVisibilityBase != Range.x)
        {
            RetainedStatic[Slot].MeshletVisibilityBase = Range.x;
            MarkStaticDirty(Slot);
        }
    }

    void FScenePrimitiveSet::AssignMeshletVisibility()
    {
        const uint32 SlotCount = (uint32)RetainedCullEntries.Num();
        if (static_cast<uint32>(SlotMeshletVisibility.Num()) < SlotCount)
        {
            SlotMeshletVisibility.SetNum(SlotCount, FUIntVector3(kNoMeshletVisibility, 0u, 0u));
        }

        if (bFullInstanceUpload || bFullStaticUpload)
        {
            // Nearly every slot already holds the range its LODs need, so the check runs wide and only the stale ones allocate, in slot order.
            ResetParallelBuckets();

            const auto IsStale = [this](uint32 Slot)
            {
                const FUIntVector3& Range = SlotMeshletVisibility[Slot];
                return Range.z != LargestLODMeshletsOf(Slot) || RetainedStatic[Slot].MeshletVisibilityBase != Range.x;
            };
            constexpr uint32 kMeshletVisibilityScanGrain = 4096;
            Jobs::ParallelForOrSerial(SlotCount, kMeshletVisibilityScanGrain, [&](const Jobs::FParallelRange& Range)
            {
                TVector<uint32>& Stale = BucketFor(Range.Thread).Slots;
                for (uint32 Slot = Range.Start; Slot < Range.End; ++Slot)
                {
                    if (IsStale(Slot))
                    {
                        Stale.Add(Slot);
                    }
                }
            });

            MeshletVisibilityScratch.Reset();
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                MeshletVisibilityScratch.Append(Bucket.Slots);
                Bucket.Slots.Reset();
            }
            Algo::Sort(MeshletVisibilityScratch);
            for (uint32 Slot : MeshletVisibilityScratch)
            {
                UpdateMeshletVisibility(Slot);
            }
            return;
        }

        // Copied first, since a base that moves marks its slot static-dirty and grows the list being walked.
        MeshletVisibilityScratch.Reset();
        MeshletVisibilityScratch.Append(DirtyInstanceSlots);
        MeshletVisibilityScratch.Append(DirtyStaticSlots);
        for (uint32 Slot : MeshletVisibilityScratch)
        {
            if (Slot < SlotCount)
            {
                UpdateMeshletVisibility(Slot);
            }
        }
    }

    void FScenePrimitiveSet::FreeMeshletVisibilityRange(uint32 Base, uint32 Words)
    {
        if (Words == 0u)
        {
            return;
        }
        if (Words >= (uint32)MeshletVisibilityFreeLists.Num())
        {
            MeshletVisibilityFreeLists.SetNum(Words + 1u);
        }
        MeshletVisibilityFreeLists[Words].Add(Base);
    }

    uint32 FScenePrimitiveSet::InternSurfaceDesc(const FResolvedSurface& Surface)
    {
        FSurfaceDescGPU Desc = {};
        Desc.NumLODs = Surface.NumLODs;
        Desc.Flags   = Surface.LODFlags;
        Desc.LODErrorMultiplier = Surface.LODErrorMultiplier;
        for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
        {
            Desc.LODMeshletOffset[i] = Surface.LODMeshletOffset[i];
            Desc.LODMeshletCount[i]  = Surface.LODMeshletCount[i];
            Desc.LODError[i]         = Surface.LODError[i];
            Desc.LODParentError[i]   = Surface.LODParentError[i];

            const uint32 RangeEnd = Desc.LODMeshletOffset[i] + Desc.LODMeshletCount[i];
            if (RangeEnd > MAX_MESHLETS_PER_SURFACE_LOD)
            {
                const uint32 Addressable = Desc.LODMeshletOffset[i] < MAX_MESHLETS_PER_SURFACE_LOD
                                         ? MAX_MESHLETS_PER_SURFACE_LOD - Desc.LODMeshletOffset[i]
                                         : 0u;
                LOG_ERROR("ScenePrimitiveSet: surface LOD {} spans meshlets [{}, {}), past the {} the draw "
                          "list can index. Clamping to {}; the mesh needs splitting or a coarser meshlet build.",
                          i, Desc.LODMeshletOffset[i], RangeEnd, MAX_MESHLETS_PER_SURFACE_LOD, Addressable);
                Desc.LODMeshletCount[i] = Addressable;
            }
        }

        uint64 Hash = 0;
        Hash::HashCombine(Hash, Desc.NumLODs);
        Hash::HashCombine(Hash, Desc.Flags);
        Hash::HashCombine(Hash, Desc.LODErrorMultiplier);
        for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
        {
            Hash::HashCombine(Hash, Desc.LODMeshletOffset[i]);
            Hash::HashCombine(Hash, Desc.LODMeshletCount[i]);
            Hash::HashCombine(Hash, Desc.LODError[i]);
            Hash::HashCombine(Hash, Desc.LODParentError[i]);
        }

        TVector<uint32>& Bucket = SurfaceDescByHash.FindOrAdd(Hash);
        for (uint32 Index : Bucket)
        {
            if (Memory::Memcmp(&SurfaceDescs[Index], &Desc, sizeof(FSurfaceDescGPU)) == 0)
            {
                return Index;
            }
        }

        const uint32 NewIndex = (uint32)SurfaceDescs.Num();
        SurfaceDescs.Add(Desc);
        Bucket.Add(NewIndex);
        bSurfaceDescsDirty = true;

        const uint32 DescLODs = Math::Min<uint32>(Desc.NumLODs, MAX_MESH_LODS);
        for (uint32 i = 0; i < DescLODs; ++i)
        {
            MaxSurfaceDescMeshlets = Math::Max(MaxSurfaceDescMeshlets, Desc.LODMeshletCount[i]);
        }

        return NewIndex;
    }

    //~ Grass

    FScenePrimitiveSet::FGrassSpeciesBinding FScenePrimitiveSet::AcquireGrassSpecies(const FGrassSpeciesKey& Key, CStaticMesh* Mesh, uint32 ResolveHandle, uint32 Capacity)
    {
        if (Mesh == nullptr || Capacity == 0u || !FMeshResolveCache::Get().IsValidHandle(ResolveHandle))
        {
            return {};
        }

        // One surface only, since a multi-material grass mesh would need a block per surface.
        const FResolvedMesh& Resolved = FMeshResolveCache::Get().GetEntry(ResolveHandle);
        if (Resolved.Surfaces.IsEmpty())
        {
            return {};
        }

        if (FGrassSpeciesBinding* Existing = GrassSpecies.Find(Key))
        {
            Existing->SeenSerial = GrassSerial;
            if (Existing->Mesh == Mesh && Existing->Capacity >= Capacity)
            {
                // The first resolve may have been the default material standing in for one still compiling or loading.
                if (Existing->ResolvedGeneration != Resolved.Generation)
                {
                    BindGrassSurface(*Existing, Resolved);
                }
                return *Existing;
            }

            // Outgrown or remeshed, so the old block goes back inactive rather than drawing its last blades forever.
            ReleaseGrassBlock(*Existing);
        }

        FGrassSpeciesBinding Result;
        Result.Mesh              = Mesh;
        Result.InstanceSlotBase  = TakeGrassBlock(Capacity);
        Result.SeenSerial        = GrassSerial;
        Result.Capacity          = Capacity;
        Result.MeshletHeaderSlot = Mesh->GetMeshResource().MeshBuffers.MeshletHeaderSlot;
        // Enclosing-sphere radius of the bind-pose AABB, which is what the cull's sphere test wants.
        const FVector3 Extent = Mesh->GetAABB().GetSize() * 0.5f;
        Result.MeshBoundsRadius  = Math::Sqrt(Extent.x * Extent.x + Extent.y * Extent.y + Extent.z * Extent.z);
        Result.bValid            = true;
        BindGrassSurface(Result, Resolved);

        // The block starts inactive so a frame that dispatches no scatter draws nothing rather than garbage.
        for (uint32 i = 0; i < Capacity; ++i)
        {
            RetainedCullEntries[Result.InstanceSlotBase + i] = {};
        }

        GrassSpecies.Add(Key, Result);
        GrassSlotCount += Capacity;
        return Result;
    }

    // Freed blocks are kept whole for the next species, since single slots from the free list could never form one again.
    void FScenePrimitiveSet::ReleaseGrassBlock(const FGrassSpeciesBinding& Binding)
    {
        Batches.ReleaseBatchRef(Binding.BatchIndex, false);
        for (uint32 Slot = Binding.InstanceSlotBase; Slot < Binding.InstanceSlotBase + Binding.Capacity; ++Slot)
        {
            DeactivateInstanceSlot(Slot);
        }
        AddGrassFreeBlock(Binding.InstanceSlotBase, Binding.Capacity);
        GrassSlotCount -= Binding.Capacity;
    }

    // Sorted by base and merged with its neighbors, so repeated release and reacquire cannot fragment the retained arrays.
    void FScenePrimitiveSet::AddGrassFreeBlock(uint32 Base, uint32 Count)
    {
        int32 Insert = 0;
        while (Insert < GrassFreeBlocks.Num() && GrassFreeBlocks[Insert].x < Base)
        {
            ++Insert;
        }
        GrassFreeBlocks.Insert(FUIntVector2(Base, Count), Insert);

        if (Insert + 1 < GrassFreeBlocks.Num() && GrassFreeBlocks[Insert].x + GrassFreeBlocks[Insert].y == GrassFreeBlocks[Insert + 1].x)
        {
            GrassFreeBlocks[Insert].y += GrassFreeBlocks[Insert + 1].y;
            GrassFreeBlocks.RemoveAt(Insert + 1);
        }
        if (Insert > 0 && GrassFreeBlocks[Insert - 1].x + GrassFreeBlocks[Insert - 1].y == GrassFreeBlocks[Insert].x)
        {
            GrassFreeBlocks[Insert - 1].y += GrassFreeBlocks[Insert].y;
            GrassFreeBlocks.RemoveAt(Insert);
        }
    }

    FScenePrimitiveSet::FMeshletDemand FScenePrimitiveSet::GetGrassMeshletDemand() const
    {
        FMeshletDemand Demand;
        for (const auto& [Key, Binding] : GrassSpecies)
        {
            const uint32 Meshlets = LargestLODMeshletsOfSurface(Binding.SurfaceDescIndex);
            Demand.Words    += (uint64)Binding.Capacity * RenderUtils::GetGroupCount(Meshlets, MESHLET_CULL_GROUP_SIZE);
            Demand.Meshlets += (uint64)Binding.Capacity * Meshlets;
        }
        return Demand;
    }

    void FScenePrimitiveSet::KeepGrassSpecies(ECS::FEntity Terrain)
    {
        for (auto& [Key, Binding] : GrassSpecies)
        {
            if (Key.Terrain == Terrain)
            {
                Binding.SeenSerial = GrassSerial;
            }
        }
    }

    uint32 FScenePrimitiveSet::TakeGrassBlock(uint32 Capacity)
    {
        for (int32 Index = 0; Index < GrassFreeBlocks.Num(); ++Index)
        {
            FUIntVector2& Block = GrassFreeBlocks[Index];
            if (Block.y < Capacity)
            {
                continue;
            }
            const uint32 Base = Block.x;
            Block.x += Capacity;
            Block.y -= Capacity;
            if (Block.y == 0u)
            {
                GrassFreeBlocks.RemoveAt(Index);
            }
            for (uint32 Slot = Base; Slot < Base + Capacity; ++Slot)
            {
                MarkInstanceDirty(Slot);
                MarkStaticDirty(Slot);
            }
            return Base;
        }
        return AllocateInstanceSlotBlock(Capacity);
    }

    void FScenePrimitiveSet::ReleaseUnseenGrassSpecies()
    {
        for (auto It = GrassSpecies.begin(); It != GrassSpecies.end();)
        {
            if (It->Value.SeenSerial != GrassSerial)
            {
                ReleaseGrassBlock(It->Value);
                It = GrassSpecies.Remove(It);
            }
            else
            {
                ++It;
            }
        }
        ++GrassSerial;
    }

    void FScenePrimitiveSet::BindGrassSurface(FGrassSpeciesBinding& Binding, const FResolvedMesh& Resolved)
    {
        const FResolvedSurface& Surface = Resolved.Surfaces[0];

        const uint32 BatchIndex = Batches.FindOrAddBatch(Surface);
        Batches.AddBatchRef(BatchIndex, false);
        if (Binding.ResolvedGeneration != 0u)
        {
            Batches.ReleaseBatchRef(Binding.BatchIndex, false);
        }

        // Grass is never skinned and always has geometry once its mesh resolved; Active is set per blade by the scatter.
        const EInstanceFlags Flags = EInstanceFlags::HasGeometry | Surface.MaterialFlags;

        Binding.BatchIndex         = BatchIndex;
        Binding.DrawIDAndFlags     = PackDrawIDAndFlags(BatchIndex, Flags);
        Binding.SurfaceDescIndex   = InternSurfaceDesc(Surface);
        Binding.MaterialIndex      = Surface.MaterialIdx;
        Binding.ResolvedGeneration = Resolved.Generation;
    }

    //~ Instance refresh

    void FScenePrimitiveSet::RefreshInstances(uint32 Index, const FMatrix4& Transform, FDirtySlotBucket* Sink)
    {
        using namespace ScenePrimitiveSetInternal;

        if (Sink == nullptr)
        {
            ++SyncStats.RefreshInstanceCalls;
        }

        const FScenePrimitive&    Prim = Primitives[Index];
        const FPrimitiveCullData& Cull = CullData[Index];
        const FVector4            Sphere = InstanceSphereOf(Prim, Bounds[Index]);
        // Its shadow would fall from where it is drawn rather than where it is.
        const bool                bRemapped    = IsRemappedForeground(Prim);
        const bool                bActive      = Prim.Surfaces != nullptr && !(Prim.Source == EPrimitiveSource::SkeletalMesh && Prim.BoneCount == 0u);
        const bool                bHasGeometry = Prim.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot;
        const FTransform3x4       NewTransform = PackTransform3x4(InstanceTransformOf(Prim, Transform));
        const uint32              LODAndThinning = PackLODAndThinning(Prim.ForcedLODIndex, 0.0f, Prim.RenderLayers, Prim.CustomDepthStencil);

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Prim.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.Num())
            {
                continue;
            }

            EInstanceFlags Flags = ComposeInstanceFlags(Prim.BaseFlags, Binding, Prim.bCastShadow && !bRemapped, Prim.bCastFarShadow, bActive, bHasGeometry);
            if (bRemapped)
            {
                Flags &= ~EInstanceFlags::HasDistanceField;
            }

            FInstanceCullEntry NewCull = {};
            NewCull.SphereBounds     = Sphere;
            NewCull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
            NewCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
            NewCull.MaxDrawDistance  = Cull.MaxDrawDistance;
            NewCull.LODAndThinning   = LODAndThinning;

            FInstanceStatic NewStatic = {};
            NewStatic.MeshletHeaderSlot     = Prim.MeshletHeaderSlot;
            NewStatic.CustomData            = Prim.CustomData;
            NewStatic.MaterialIndex         = Binding.MaterialIndex;
            NewStatic.EntityID              = Prim.Entity.Value;
            NewStatic.SkinnedIndex          = Binding.SkinnedIndex;
            // Owned by AssignMeshletVisibility, which a rebuilt payload must not undo.
            NewStatic.MeshletVisibilityBase = RetainedStatic[Slot].MeshletVisibilityBase;
            NewStatic.RenderLayers          = Prim.RenderLayers;

            // A resolve bump onto identical values must not dirty the slot, or every instance re-uploads.
            const bool bCullChanged      = StoreIfChanged(RetainedCullEntries[Slot], NewCull);
            const bool bTransformChanged = StoreIfChanged(RetainedTransforms[Slot], NewTransform);
            if (bCullChanged || bTransformChanged)
            {
                if (Sink != nullptr)
                {
                    Sink->Slots.Add(Slot);
                }
                else
                {
                    MarkInstanceDirty(Slot);
                }
            }
            // The proxy rides the static upload, so a changed splat dirties the slot the same way.
            const FInstanceProxy NewProxy = Prim.Surfaces != nullptr && s < (uint32)Prim.Surfaces->Num() ? (*Prim.Surfaces)[s].Proxy : FInstanceProxy{};
            const bool bProxyChanged = StoreIfChanged(RetainedProxies[Slot], NewProxy);
            if (StoreIfChanged(RetainedStatic[Slot], NewStatic) || bProxyChanged)
            {
                if (Sink != nullptr)
                {
                    Sink->StaticSlots.Add(Slot);
                }
                else
                {
                    MarkStaticDirty(Slot);
                }
            }
        }
    }

    void FScenePrimitiveSet::RefreshInstanceTransform(uint32 Index, const FMatrix4& Transform, const FVector4& WorldSphere, TVector<uint32>* DirtySink)
    {
        using namespace ScenePrimitiveSetInternal;

        if (DirtySink == nullptr)
        {
            ++SyncStats.RefreshInstanceCalls;
        }

        const FScenePrimitive& Prim   = Primitives[Index];
        const FVector4         Sphere = InstanceSphereOf(Prim, WorldSphere);
        const FTransform3x4    Packed = PackTransform3x4(InstanceTransformOf(Prim, Transform));

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const uint32 Slot = Bindings[Prim.BindingBase + s].InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.Num())
            {
                continue;
            }

            const bool bBoundsChanged    = StoreIfChanged(RetainedCullEntries[Slot].SphereBounds, Sphere);
            const bool bTransformChanged = StoreIfChanged(RetainedTransforms[Slot], Packed);
            if (!bBoundsChanged && !bTransformChanged)
            {
                continue;
            }

            if (DirtySink != nullptr)
            {
                DirtySink->Add(Slot);
            }
            else
            {
                MarkInstanceDirty(Slot);
            }
        }
    }

    //~ Foreground

    FMatrix4 FScenePrimitiveSet::InstanceTransformOf(const FScenePrimitive& Prim, const FMatrix4& Transform) const
    {
        return IsRemappedForeground(Prim) ? ForegroundTransform * Transform : Transform;
    }

    FVector4 FScenePrimitiveSet::InstanceSphereOf(const FScenePrimitive& Prim, const FVector4& WorldSphere) const
    {
        if (!IsRemappedForeground(Prim))
        {
            return WorldSphere;
        }
        return FVector4(FVector3(ForegroundTransform * FVector4(FVector3(WorldSphere), 1.0f)), WorldSphere.w * ForegroundRadiusScale);
    }

    void FScenePrimitiveSet::TrackForeground(uint32 Index)
    {
        FScenePrimitive& Prim = Primitives[Index];
        if (!Prim.bForegroundTracked && (Prim.RenderLayers & RenderLayers::kForegroundMask) != 0u)
        {
            Prim.bForegroundTracked = true;
            ForegroundKeys.Add(MakeKey(Prim.Entity, Prim.Source));
        }
    }

    void FScenePrimitiveSet::UntrackForeground(uint32 Index)
    {
        FScenePrimitive& Prim = Primitives[Index];
        if (Prim.bForegroundTracked)
        {
            Prim.bForegroundTracked = false;
            ForegroundKeys.RemoveSingleSwap(MakeKey(Prim.Entity, Prim.Source));
        }
    }

    void FScenePrimitiveSet::SetForegroundView(const FViewVolume& View, const FViewLayerSettings& Layers)
    {
        const bool bActive = Layers.IsForegroundActive() && !View.IsOrthographic();

        // Scaled about the eye, toward it by the depth scale and across by the field of view ratio, so every point keeps its pixel ray.
        FMatrix4 Transform(1.0f);
        float    RadiusScale = 1.0f;
        if (bActive)
        {
            const float Widen = Math::Tan(Math::Radians(View.GetFOV()) * 0.5f) / Math::Tan(Math::Radians(Layers.ForegroundFOV) * 0.5f);
            const float Depth = Layers.ForegroundDepthScale;
            Transform   = View.GetInverseViewMatrix() * Math::Scale(FMatrix4(1.0f), FVector3(Widen * Depth, Widen * Depth, Depth)) * View.GetViewMatrix();
            RadiusScale = Math::Max(Widen, 1.0f) * Depth;
        }

        const bool bToggled = bActive != bForegroundActive;
        if (!bToggled && Transform == ForegroundTransform)
        {
            return;
        }
        // Whether a foreground primitive needs the remap decides if it may stream at all.
        if (bToggled)
        {
            InvalidateAllStreamCache();
            bDiscardMoveStream = bMoveStreamArmed;
        }

        bForegroundActive     = bActive;
        ForegroundTransform   = Transform;
        ForegroundRadiusScale = RadiusScale;

        if (bToggled || PendingForegroundRefresh == EForegroundRefresh::None)
        {
            PendingForegroundRefresh = bToggled ? EForegroundRefresh::Full : EForegroundRefresh::Transforms;
        }
    }

    void FScenePrimitiveSet::ApplyForegroundRefresh(const FSyncPools& Pools)
    {
        if (PendingForegroundRefresh == EForegroundRefresh::None)
        {
            return;
        }
        const bool bFull = PendingForegroundRefresh == EForegroundRefresh::Full;
        PendingForegroundRefresh = EForegroundRefresh::None;

        for (int32 i = 0; i < ForegroundKeys.Num();)
        {
            const uint64 Key   = ForegroundKeys[i];
            const uint32 Index = FindLinked(KeyEntity(Key), KeySource(Key));
            if (Index == Constants::kIndexNoneU32 || (Primitives[Index].RenderLayers & RenderLayers::kForegroundMask) == 0u)
            {
                if (Index != Constants::kIndexNoneU32)
                {
                    Primitives[Index].bForegroundTracked = false;
                }
                ForegroundKeys.RemoveAtSwap(i);
                continue;
            }
            ++i;

            const STransformComponent* Transform = Pools.Transform->TryGet(Primitives[Index].Entity);
            if (Transform == nullptr)
            {
                continue;
            }
            const FMatrix4 Matrix = Transform->GetWorldMatrixCached();
            const FVector4 Sphere = RebuildWorldBounds(Index, Matrix);
            if (bFull)
            {
                RefreshInstances(Index, Matrix);
            }
            else
            {
                RefreshInstanceTransform(Index, Matrix, Sphere, nullptr);
            }
        }
    }
}
