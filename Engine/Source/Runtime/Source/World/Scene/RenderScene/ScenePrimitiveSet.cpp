#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "SceneCullMath.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Containers/ConcurrentQueue.h"
#include "Memory/Memcpy.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include <immintrin.h>
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Containers/Algorithm.h"

namespace Lumina
{
    template <typename TArray>
    static void ReserveGeometric(TArray& Array, SIZE_T Target)
    {
        const SIZE_T Capacity = Array.capacity();
        if (Target <= Capacity)
        {
            return;
        }
        Array.reserve(Math::Max(Target, Capacity + Capacity / 2u));
    }

    // Writes a whole cache line without reading it in first, which a crowd of movers would otherwise pay on every primitive.
    static FORCEINLINE void StreamMatrix(FMatrix4& Dest, const FMatrix4& Value)
    {
        const float* Source = &Value[0][0];
        float* Target = &Dest[0][0];
        _mm256_stream_ps(Target, _mm256_loadu_ps(Source));
        _mm256_stream_ps(Target + 8, _mm256_loadu_ps(Source + 8));
    }

    // A full write-combined line for the GPU, which the CPU never reads back.
    static FORCEINLINE void StreamRecord(FTransformStreamRecord& Dest, const FTransformStreamRecord& Value)
    {
        const float* Source = reinterpret_cast<const float*>(&Value);
        float* Target = reinterpret_cast<float*>(&Dest);
        _mm256_stream_ps(Target, _mm256_loadu_ps(Source));
        _mm256_stream_ps(Target + 8, _mm256_loadu_ps(Source + 8));
    }

    static FORCEINLINE FTransformStreamRecord MakeStreamRecord(const FMatrix4& Matrix, const FVector3& LocalCenter, float Radius)
    {
        FTransformStreamRecord Out;
        Out.Transform    = PackTransform3x4(Matrix);
        Out.SphereBounds = SceneCull::TransformSphere(Matrix, LocalCenter, Radius);
        return Out;
    }

    static constexpr uint8  kMovedSources     = (uint8)((1u << (uint32)EPrimitiveSource::Foliage) - 1u);
    // Below this many movers the CPU copies cost less than keeping a stream and catching its stoppers up.
    static constexpr uint32 kMinStreamedMoves = 1024;

    static FORCEINLINE uint64 PackDirty(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        return (uint64)(Entity).Value | ((uint64)Source << 32) | ((uint64)Flags  << 40);
    }

    struct FRenderDirtyTracker::FImpl
    {
        using FQueue = TConcurrentQueue<uint64>;
        FQueue Queue;
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
        TUniquePtr<FRenderDirtyTracker>* Holder = Registry.Ctx().Find<TUniquePtr<FRenderDirtyTracker>>();
        return Holder ? Holder->get() : nullptr;
    }

    FRenderDirtyTracker& FRenderDirtyTracker::Ensure(ECS::FRegistry& Registry)
    {
        if (TUniquePtr<FRenderDirtyTracker>* Holder = Registry.Ctx().Find<TUniquePtr<FRenderDirtyTracker>>())
        {
            return *Holder->get();
        }
        return *Registry.Ctx().Emplace<TUniquePtr<FRenderDirtyTracker>>(MakeUnique<FRenderDirtyTracker>());
    }

    void FRenderDirtyTracker::Mark(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        if (Entity == ECS::NullEntity || Flags == EPrimitiveDirty::None)
        {
            return;
        }
        Impl->Queue.Enqueue(PackDirty(Entity, Source, Flags));
        bAnyDirty.store(true, std::memory_order_release);
    }

    void FRenderDirtyTracker::MarkAllSources(ECS::FEntity Entity, EPrimitiveDirty Flags)
    {
        Mark(Entity, EPrimitiveSource::AnySource, Flags);
    }

    bool FRenderDirtyTracker::Drain(TVector<FEntry>& Out)
    {
        if (!bAnyDirty.load(std::memory_order_acquire))
        {
            return false;
        }

        const SIZE_T Before = Out.size();

        uint64 Batch[256];
        std::size_t Count;
        while ((Count = Impl->Queue.DequeueBulk(Batch, 256)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                const uint64 Packed = Batch[i];
                FEntry& Entry  = Out.emplace_back();
                Entry.Entity = (ECS::FEntity)(uint32)(Packed & 0xFFFFFFFFull);
                Entry.Source = (EPrimitiveSource)((Packed >> 32) & 0xFFull);
                Entry.Flags  = (EPrimitiveDirty)((Packed >> 40) & 0xFFull);
            }
        }

        bAnyDirty.store(false, std::memory_order_release);
        return Out.size() != Before;
    }

    // Linear scan over a list that stays tiny (one entry per material instance of one master).
    static void NoteDeferredMaterial(FSceneBatchRegistry::FBatch& Batch, const FResolvedSurface& Surface)
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

        Batch.DeferredMaterials.push_back({ Surface.MaterialIdx, Surface.DeferredShader });
    }

    uint32 FSceneBatchRegistry::FindOrAddBatch(const FResolvedSurface& Surface)
    {
        const uint64 KeyHash = GetTypeHash(Surface.BatchKey);

        TVector<uint32>& Bucket = BatchesByHash[KeyHash];
        for (uint32 Index : Bucket)
        {
            if (Batches[Index].Key == Surface.BatchKey)
            {
                NoteDeferredMaterial(Batches[Index], Surface);
                return Index;
            }
        }

        uint32 NewIndex;
        if (!FreeBatches.empty())
        {
            NewIndex = FreeBatches.back();
            FreeBatches.pop_back();
            Batches[NewIndex] = FBatch{};
        }
        else
        {
            NewIndex = (uint32)Batches.size();
            if (NewIndex == 0x10000u)
            {
                LOG_ERROR("SceneBatchRegistry: batch count reached {}, the limit PackDrawIDAndFlags can "
                          "address. Draw ids past this wrap.", 0x10000u);
            }
            Batches.emplace_back();
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
        NoteDeferredMaterial(Batch, Surface);

        Bucket.push_back(NewIndex);
        ++LayoutGeneration;
        return NewIndex;
    }

    void FSceneBatchRegistry::AddBatchRef(uint32 BatchIndex, bool bSkinned)
    {
        FBatch& Batch = Batches[BatchIndex];
        ++Batch.RefCount;
        ++(bSkinned ? Batch.SkinnedRefCount : Batch.StaticRefCount);
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
        if (BatchIndex >= (uint32)Batches.size() || Count == 0u)
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

        auto It = BatchesByHash.find(GetTypeHash(Batch.Key));
        if (It != BatchesByHash.end())
        {
            TVector<uint32>& Bucket = It->second;
            for (SIZE_T i = 0; i < Bucket.size(); ++i)
            {
                if (Bucket[i] == BatchIndex)
                {
                    Bucket[i] = Bucket.back();
                    Bucket.pop_back();
                    break;
                }
            }
        }

        Batch = FBatch{};
        FreeBatches.push_back(BatchIndex);
        ++LayoutGeneration;
        ++RecycleGeneration;
    }

    void FSceneBatchRegistry::Reset()
    {
        Batches.clear();
        BatchesByHash.clear();
        FreeBatches.clear();
        ++LayoutGeneration;
        ++RecycleGeneration;
    }

    uint32 FScenePrimitiveSet::FindPrimitive(uint64 Key) const
    {
        const ECS::FEntity     Entity = (ECS::FEntity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);

        // Foliage owns no primitive; its instances live in FoliageByEntity. See FFoliageInstanceRef.
        return FindLinked(Entity, Source);
    }

    //~ Flat entity-index link table. See FPrimitiveLink for why this exists instead of a hash probe.

    uint32 FScenePrimitiveSet::FindLinked(ECS::FEntity Entity, EPrimitiveSource Source) const
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)(Entity).GetIndex();
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return Constants::kIndexNoneU32;
        }

        const FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        return Link.Entity == (Entity).Value ? Link.Index[(uint32)Source] : Constants::kIndexNoneU32;
    }

    uint8 FScenePrimitiveSet::GetSourceMask(ECS::FEntity Entity) const
    {
        const uint32 Slot = (uint32)(Entity).GetIndex();
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return 0u;
        }

        const FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != (Entity).Value)
        {
            return 0u;
        }

        uint8 Mask = 0u;
        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            Mask |= (Link.Index[s] != Constants::kIndexNoneU32) ? (uint8)(1u << s) : (uint8)0u;
        }
        return Mask;
    }

    void FScenePrimitiveSet::SetLink(ECS::FEntity Entity, EPrimitiveSource Source, uint32 Index)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)(Entity).GetIndex();
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            LinksByEntityIndex.resize(Slot + 1u);
        }

        FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != (Entity).Value)
        {
            Link = FPrimitiveLink{};
            Link.Entity = (Entity).Value;
        }
        Link.Index[(uint32)Source] = Index;
    }

    void FScenePrimitiveSet::ClearLink(ECS::FEntity Entity, EPrimitiveSource Source)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)(Entity).GetIndex();
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return;
        }

        FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != (Entity).Value)
        {
            return;
        }

        Link.Index[(uint32)Source] = Constants::kIndexNoneU32;

        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            if (Link.Index[s] != Constants::kIndexNoneU32)
            {
                return;
            }
        }
        Link.Entity = Constants::kIndexNoneU32;
    }

    uint32 FScenePrimitiveSet::AddPrimitive(uint64 Key)
    {
        const uint32 Index = (uint32)Primitives.size();
        Primitives.emplace_back();
        Bounds.emplace_back(FVector4(0.0f));
        CullData.emplace_back();
        Keys.push_back(Key);
        ResolveKeys.push_back(PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0));
        NextByHandle.push_back(Constants::kIndexNoneU32);
        PrevByHandle.push_back(Constants::kIndexNoneU32);

        const ECS::FEntity     Entity = (ECS::FEntity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        SetLink(Entity, Source, Index);
        if (Source == EPrimitiveSource::SkeletalMesh)
        {
            ++SkinnedCount;
            ++BonelessSkinnedCount;

            // Kept in step here, so streaming other primitives never pays for a walk over all of them.
            if (SkeletalIndicesGeneration == SkeletalSetGeneration)
            {
                SkeletalIndices.push_back(Index);
            }
        }

        ++StructureGeneration;
        return Index;
    }

    void FScenePrimitiveSet::RemovePrimitive(uint64 Key)
    {
        const uint32 Index = FindPrimitive(Key);
        if (Index == Constants::kIndexNoneU32)
        {
            return;
        }

        const uint32 Last = (uint32)Primitives.size() - 1u;

        if (Primitives[Index].Source == EPrimitiveSource::SkeletalMesh && Primitives[Index].BoneCount == 0u
            && BonelessSkinnedCount > 0u)
        {
            --BonelessSkinnedCount;
        }
        if (Primitives[Index].CustomDepthStencil != 0u)
        {
            --CustomDepthCount;
        }

        ReleaseBindings(Index);
        ReleaseBoneSlice(Primitives[Index]);
        Primitives[Index].BoneCount = 0u;
        UnlinkHandle(Index, Primitives[Index].ResolveHandle);

        // The tail primitive moves into this slot, so only the removed entry and the tail's entry change.
        if (SkeletalIndicesGeneration == SkeletalSetGeneration)
        {
            if (Primitives[Index].Source == EPrimitiveSource::SkeletalMesh)
            {
                if (auto It = Algo::Find(SkeletalIndices, Index); It != SkeletalIndices.end())
                {
                    *It = SkeletalIndices.back();
                    SkeletalIndices.pop_back();
                }
            }
            if (Index != Last && Primitives[Last].Source == EPrimitiveSource::SkeletalMesh)
            {
                if (auto It = Algo::Find(SkeletalIndices, Last); It != SkeletalIndices.end())
                {
                    *It = Index;
                }
            }
        }

        if (Index != Last)
        {
            Primitives[Index]  = std::move(Primitives[Last]);
            Bounds[Index]      = Bounds[Last];
            CullData[Index]    = CullData[Last];
            Keys[Index]        = Keys[Last];
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
            else if (Primitives[Index].ResolveHandle < (uint32)HandleListHead.size()
                     && HandleListHead[Primitives[Index].ResolveHandle] == Last)
            {
                HandleListHead[Primitives[Index].ResolveHandle] = Index;
            }
            if (MovedNext != Constants::kIndexNoneU32)
            {
                PrevByHandle[MovedNext] = Index;
            }

            const ECS::FEntity     MovedEntity = (ECS::FEntity)(uint32)(Keys[Index] & 0xFFFFFFFFull);
            const EPrimitiveSource MovedSource = (EPrimitiveSource)((Keys[Index] >> 32) & 0xFFull);
            SetLink(MovedEntity, MovedSource, Index);
        }

        Primitives.pop_back();
        Bounds.pop_back();
        CullData.pop_back();
        Keys.pop_back();
        ResolveKeys.pop_back();
        NextByHandle.pop_back();
        PrevByHandle.pop_back();

        const ECS::FEntity     Entity = (ECS::FEntity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        if (Source == EPrimitiveSource::SkeletalMesh && SkinnedCount > 0)
        {
            --SkinnedCount;
        }
        ClearLink(Entity, Source);

        ++StructureGeneration;
    }

    void FScenePrimitiveSet::RemoveEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, EPrimitiveSource Source)
    {
        if (Source == EPrimitiveSource::Foliage)
        {
            auto It = FoliageByEntity.find(Entity);
            if (It == FoliageByEntity.end())
            {
                return;
            }
            PendingFoliageReleases.push_back(Move(It->second));
            FoliageByEntity.erase(It);
            ++StructureGeneration;
            return;
        }

        RemovePrimitive(MakeKey(Entity, Source));
    }

    void FScenePrimitiveSet::RebuildWorldBounds(uint32 Index)
    {
        RebuildWorldBounds(Index, Primitives[Index].Transform);
    }

    void FScenePrimitiveSet::RebuildWorldBounds(uint32 Index, const FMatrix4& Transform)
    {
        const FScenePrimitive& Prim = Primitives[Index];
        const float CullScale = Math::Max(Prim.BoundsScale, 1.0f);
        Bounds[Index] = SceneCull::TransformSphere(Transform, Prim.LocalCenter, Prim.LocalRadius * CullScale);
    }

    void FScenePrimitiveSet::CompactBindings()
    {
        // Pending releases still point into the old layout, so they go before anything moves.
        FlushFoliageReleases();

        TVector<FSurfaceBinding> Packed;
        Packed.reserve(Bindings.size() - DeadBindings);

        for (FScenePrimitive& Prim : Primitives)
        {
            if (Prim.SurfaceCount == 0)
            {
                Prim.BindingBase = 0;
                continue;
            }
            const uint32 NewBase = (uint32)Packed.size();
            Packed.insert(Packed.end(),
                          Bindings.begin() + Prim.BindingBase,
                          Bindings.begin() + Prim.BindingBase + Prim.SurfaceCount);
            Prim.BindingBase = NewBase;
        }

        // Foliage instances hold spans in the same array without owning a primitive, so they rebase here
        // too. Miss this and a compaction drops every painted instance's bindings on the floor.
        for (auto& [Entity, State] : FoliageByEntity)
        {
            for (FFoliageInstanceRef& Ref : State.Instances)
            {
                if (Ref.SurfaceCount == 0)
                {
                    Ref.BindingBase = 0;
                    continue;
                }
                const uint32 NewBase = (uint32)Packed.size();
                Packed.insert(Packed.end(),
                              Bindings.begin() + Ref.BindingBase,
                              Bindings.begin() + Ref.BindingBase + Ref.SurfaceCount);
                Ref.BindingBase = NewBase;
            }
        }

        Bindings.swap(Packed);
        DeadBindings = 0;
        BindingFreeRanges.clear();
        FreeRangeBindings = 0;
    }

    bool FScenePrimitiveSet::ShouldCompactBindings() const
    {
        const uint32 Wasted = DeadBindings - Math::Min(DeadBindings, FreeRangeBindings);
        return (Wasted > 1024 && Wasted * 4 > (uint32)Bindings.size())
            || FreeRangeBindings > (uint32)Bindings.size() / 2u;
    }

    uint32 FScenePrimitiveSet::TakeBindingRange(uint32 Count)
    {
        for (size_t i = 0; i < BindingFreeRanges.size() && Count != 0u; ++i)
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
                Range = BindingFreeRanges.back();
                BindingFreeRanges.pop_back();
            }
            DeadBindings      -= Math::Min(DeadBindings, Count);
            FreeRangeBindings -= Math::Min(FreeRangeBindings, Count);
            return Base;
        }

        const uint32 Base = (uint32)Bindings.size();
        Bindings.resize(Base + Count);
        return Base;
    }

    uint32 FScenePrimitiveSet::AllocateInstanceSlot()
    {
        if (!InstanceFreeSlots.empty())
        {
            const uint32 Slot = InstanceFreeSlots.back();
            InstanceFreeSlots.pop_back();
            return Slot;
        }

        const uint32 Slot = (uint32)RetainedCullEntries.size();

        RetainedCullEntries.emplace_back();
        RetainedTransforms.emplace_back();
        RetainedStatic.emplace_back();

        // The device range past the old count may hold a slot from before a reset, so a new slot always uploads.
        MarkInstanceDirty(Slot);
        MarkStaticDirty(Slot);
        return Slot;
    }

    uint32 FScenePrimitiveSet::AllocateSkinnedIndex()
    {
        if (!SkinnedIndexFreeList.empty())
        {
            const uint32 Index = SkinnedIndexFreeList.back();
            SkinnedIndexFreeList.pop_back();
            return Index;
        }
        return SkinnedIndexExtent++;
    }

    void FScenePrimitiveSet::FreeSkinnedIndex(uint32 Index)
    {
        if (Index != kNoSkinnedIndex)
        {
            SkinnedIndexFreeList.push_back(Index);
        }
    }

    void FScenePrimitiveSet::FreeInstanceSlot(uint32 Slot)
    {
        if (Slot >= (uint32)RetainedCullEntries.size())
        {
            return;
        }

        RetainedCullEntries[Slot].DrawIDAndFlags &= ~((uint32)EInstanceFlags::Active << 16);
        ReleaseMeshletVisibility(Slot);
        MarkInstanceDirty(Slot);
        InstanceFreeSlots.push_back(Slot);
    }

    void FScenePrimitiveSet::ReleaseMeshletVisibility(uint32 Slot)
    {
        if (Slot < (uint32)SlotMeshletVisibility.size())
        {
            FUIntVector2& Range = SlotMeshletVisibility[Slot];
            FreeMeshletVisibilityRange(Range.x, Range.y);
            Range = FUIntVector2(kNoMeshletVisibility, 0u);
        }

        // A reused slot carries its payload's base forward, so a freed range must not survive in it.
        if (Slot < (uint32)RetainedStatic.size())
        {
            RetainedStatic[Slot].MeshletVisibilityBase = kNoMeshletVisibility;
        }
    }

    uint32 FScenePrimitiveSet::MeshletVisibilityWordsOf(uint32 Slot) const
    {
        const FInstanceCullEntry& Cull = RetainedCullEntries[Slot];
        const bool bActive = ((Cull.DrawIDAndFlags >> 16) & (uint32)EInstanceFlags::Active) != 0u;
        if (!bActive || Cull.SurfaceDescIndex >= (uint32)SurfaceDescs.size())
        {
            return 0u;
        }

        const FSurfaceDescGPU& Desc = SurfaceDescs[Cull.SurfaceDescIndex];
        uint32 LargestLOD = 0u;
        for (uint32 LOD = 0; LOD < Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS); ++LOD)
        {
            LargestLOD = Math::Max(LargestLOD, Desc.LODMeshletCount[LOD]);
        }
        return (LargestLOD + MESHLET_CULL_GROUP_SIZE - 1u) / MESHLET_CULL_GROUP_SIZE;
    }

    void FScenePrimitiveSet::UpdateMeshletVisibility(uint32 Slot)
    {
        const uint32 Words = MeshletVisibilityWordsOf(Slot);

        FUIntVector2& Range = SlotMeshletVisibility[Slot];
        if (Range.y != Words)
        {
            ReleaseMeshletVisibility(Slot);
            if (Words != 0u)
            {
                if (Words < (uint32)MeshletVisibilityFreeLists.size() && !MeshletVisibilityFreeLists[Words].empty())
                {
                    Range.x = MeshletVisibilityFreeLists[Words].back();
                    MeshletVisibilityFreeLists[Words].pop_back();
                }
                else
                {
                    Range.x = MeshletVisibilityExtent;
                    MeshletVisibilityExtent += Words;
                }
                Range.y = Words;
            }
        }

        if (RetainedStatic[Slot].MeshletVisibilityBase != Range.x)
        {
            RetainedStatic[Slot].MeshletVisibilityBase = Range.x;
            MarkStaticDirty(Slot);
        }
    }

    void FScenePrimitiveSet::AssignMeshletVisibility()
    {
        const uint32 SlotCount = (uint32)RetainedCullEntries.size();
        if (SlotMeshletVisibility.size() < SlotCount)
        {
            SlotMeshletVisibility.resize(SlotCount, FUIntVector2(kNoMeshletVisibility, 0u));
        }

        if (bFullInstanceUpload || bFullStaticUpload)
        {
            // Nearly every slot already holds the range its LODs need, so the check runs wide and only the stale ones allocate, in slot order.
            EnsureParallelBuckets();
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                Bucket.Slots.clear();
            }

            const auto IsStale = [this](uint32 Slot)
            {
                const FUIntVector2& Range = SlotMeshletVisibility[Slot];
                return Range.y != MeshletVisibilityWordsOf(Slot) || RetainedStatic[Slot].MeshletVisibilityBase != Range.x;
            };
            constexpr uint32 kMeshletVisibilityScanGrain = 4096;
            Task::ParallelForOrSerial(SlotCount, kMeshletVisibilityScanGrain, [&](const Task::FParallelRange& Range)
            {
                TVector<uint32>& Stale = ParallelDirtySlots[Range.Thread < (uint32)ParallelDirtySlots.size() ? Range.Thread : 0u].Slots;
                for (uint32 Slot = Range.Start; Slot < Range.End; ++Slot)
                {
                    if (IsStale(Slot))
                    {
                        Stale.push_back(Slot);
                    }
                }
            });

            MeshletVisibilityScratch.clear();
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                MeshletVisibilityScratch.insert(MeshletVisibilityScratch.end(), Bucket.Slots.begin(), Bucket.Slots.end());
                Bucket.Slots.clear();
            }
            Algo::Sort(MeshletVisibilityScratch);
            for (uint32 Slot : MeshletVisibilityScratch)
            {
                UpdateMeshletVisibility(Slot);
            }
            return;
        }

        // Copied first, since a base that moves marks its slot static-dirty and grows the list being walked.
        MeshletVisibilityScratch.clear();
        MeshletVisibilityScratch.insert(MeshletVisibilityScratch.end(), DirtyInstanceSlots.begin(), DirtyInstanceSlots.end());
        MeshletVisibilityScratch.insert(MeshletVisibilityScratch.end(), DirtyStaticSlots.begin(), DirtyStaticSlots.end());
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
        if (Words >= (uint32)MeshletVisibilityFreeLists.size())
        {
            MeshletVisibilityFreeLists.resize(Words + 1u);
        }
        MeshletVisibilityFreeLists[Words].push_back(Base);
    }

    void FScenePrimitiveSet::ResetMeshletVisibility()
    {
        SlotMeshletVisibility.clear();
        MeshletVisibilityFreeLists.clear();
        MeshletVisibilityExtent = 1u;
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
        if (Handle >= (uint32)HandleListHead.size())
        {
            HandleListHead.resize(Handle + 1u, Constants::kIndexNoneU32);
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
        if (Handle == INVALID_MESH_RESOLVE_HANDLE || Handle >= (uint32)HandleListHead.size())
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

    const TVector<uint32>& FScenePrimitiveSet::GetSkeletalIndices()
    {
        if (SkeletalIndicesGeneration != SkeletalSetGeneration)
        {
            LUMINA_PROFILE_SCOPE();

            SkeletalIndices.clear();
            SkeletalIndices.reserve(SkinnedCount);

            const uint32 Num = (uint32)Primitives.size();
            for (uint32 i = 0; i < Num; ++i)
            {
                if (Primitives[i].Source == EPrimitiveSource::SkeletalMesh)
                {
                    SkeletalIndices.push_back(i);
                }
            }

            SkeletalIndicesGeneration = SkeletalSetGeneration;
        }
        return SkeletalIndices;
    }

    void FScenePrimitiveSet::ReleaseBoneSlice(FScenePrimitive& Prim)
    {
        if (Prim.BoneSliceBase != kNoBoneSlice && Prim.BoneSliceCount != 0u)
        {
            BoneSliceFreeLists[Prim.BoneSliceCount].push_back(Prim.BoneSliceBase);
        }

        Prim.BoneSliceBase      = kNoBoneSlice;
        Prim.BoneSliceCount     = 0u;
        Prim.UploadedSliceBase  = kNoBoneSlice;
        Prim.UploadedPoseSerial = 0u;
    }

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

        ReleaseBoneSlice(Prim);
        if (Count == 0u)
        {
            return kNoBoneSlice;
        }

        auto It = BoneSliceFreeLists.find(Count);
        if (It != BoneSliceFreeLists.end() && !It->second.empty())
        {
            Prim.BoneSliceBase = It->second.back();
            It->second.pop_back();
        }
        else
        {
            Prim.BoneSliceBase = BoneSliceExtent;
            BoneSliceExtent += Count;
        }

        Prim.BoneSliceCount = Count;
        return Prim.BoneSliceBase;
    }

    void FScenePrimitiveSet::ReleaseStaleBoneSlices(uint32 FrameNumber, uint32 GraceFrames)
    {
        const uint32 Num = (uint32)Primitives.size();
        if (Num == 0u)
        {
            BoneSliceSweepCursor = 0u;
            return;
        }

        const uint32 Budget = Math::Max(Num / 32u, 64u);
        for (uint32 n = 0; n < Budget; ++n)
        {
            const uint32 i = BoneSliceSweepCursor % Num;
            BoneSliceSweepCursor = i + 1u;

            FScenePrimitive& Prim = Primitives[i];
            if (Prim.BoneSliceBase != kNoBoneSlice && (FrameNumber - Prim.BoneSliceFrame) > GraceFrames)
            {
                ReleaseBoneSlice(Prim);
            }
        }
    }

    void FScenePrimitiveSet::MarkInstanceDirty(uint32 Slot)
    {
        // Already committed to re-sending the whole buffer; tracking individual slots is pure waste.
        if (bFullInstanceUpload)
        {
            return;
        }

        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && ShouldResendAllInstances(DirtyInstanceSlots.size(), SlotCount))
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            return;
        }

        DirtyInstanceSlots.push_back(Slot);
    }

    void FScenePrimitiveSet::MarkStaticDirty(uint32 Slot)
    {
        if (bFullStaticUpload)
        {
            return;
        }

        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && DirtyStaticSlots.size() * 4 >= SlotCount)
        {
            bFullStaticUpload = true;
            DirtyStaticSlots.clear();
            return;
        }

        DirtyStaticSlots.push_back(Slot);
    }

    uint32 FScenePrimitiveSet::AllocateInstanceSlotBlock(uint32 Count)
    {
        // Deliberately not the free list: the block must be contiguous so the GPU can index it from one base.
        const uint32 Base = (uint32)RetainedCullEntries.size();

        RetainedCullEntries.resize(Base + Count);
        RetainedTransforms.resize(Base + Count);
        RetainedStatic.resize(Base + Count);

        // The scatter relies on its unused slots reading inactive, so the block starts from uploaded zeros.
        for (uint32 Slot = Base; Slot < Base + Count; ++Slot)
        {
            MarkInstanceDirty(Slot);
            MarkStaticDirty(Slot);
        }
        return Base;
    }

    FScenePrimitiveSet::FGrassSpeciesBinding FScenePrimitiveSet::AcquireGrassSpecies(CStaticMesh* Mesh, uint32 ResolveHandle, uint32 Capacity)
    {
        if (Mesh == nullptr || Capacity == 0u)
        {
            return {};
        }

        // One surface only. A multi-material grass mesh would need a block per surface, and silently
        // drawing just the first would look like a broken mesh rather than an unsupported one.
        const uint32 Handle = ResolveHandle;
        if (!FMeshResolveCache::Get().IsValidHandle(Handle))
        {
            return {};
        }

        const FResolvedMesh& Resolved = FMeshResolveCache::Get().GetEntry(Handle);
        if (Resolved.Surfaces.empty())
        {
            return {};
        }

        if (auto It = GrassSpecies.find(Mesh); It != GrassSpecies.end() && It->second.Capacity >= Capacity)
        {
            // The first resolve may have been the default material standing in for one still compiling or loading.
            if (It->second.ResolvedGeneration != Resolved.Generation)
            {
                BindGrassSurface(It->second, Resolved);
            }
            return It->second;
        }

        FGrassSpeciesBinding Result;
        Result.InstanceSlotBase  = AllocateInstanceSlotBlock(Capacity);
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

        GrassSpecies[Mesh] = Result;
        return Result;
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

        // Grass is never skinned and always has geometry once its mesh resolved. Active is set per blade by
        // the scatter, not here, so an unwritten slot in the block stays free.
        const EInstanceFlags Flags = EInstanceFlags::HasGeometry | Surface.MaterialFlags;

        Binding.BatchIndex         = BatchIndex;
        Binding.DrawIDAndFlags     = PackDrawIDAndFlags(BatchIndex, Flags);
        Binding.SurfaceDescIndex   = InternSurfaceDesc(Surface);
        Binding.MaterialIndex      = Surface.MaterialIdx;
        Binding.ResolvedGeneration = Resolved.Generation;
    }

    uint32 FScenePrimitiveSet::InternSurfaceDesc(const FResolvedSurface& Surface)
    {
        FSurfaceDescGPU Desc = {};
        Desc.NumLODs = Surface.NumLODs;
        for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
        {
            Desc.LODMeshletOffset[i]     = Surface.LODMeshletOffset[i];
            Desc.LODMeshletCount[i]      = Surface.LODMeshletCount[i];
            Desc.LODError[i]         = Surface.LODError[i];

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
        for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
        {
            Hash::HashCombine(Hash, Desc.LODMeshletOffset[i]);
            Hash::HashCombine(Hash, Desc.LODMeshletCount[i]);
            Hash::HashCombine(Hash, Desc.LODError[i]);
        }

        TVector<uint32>& Bucket = SurfaceDescByHash[Hash];
        for (uint32 Index : Bucket)
        {
            if (Memory::Memcmp(&SurfaceDescs[Index], &Desc, sizeof(FSurfaceDescGPU)) == 0)
            {
                return Index;
            }
        }

        const uint32 NewIndex = (uint32)SurfaceDescs.size();
        SurfaceDescs.push_back(Desc);
        Bucket.push_back(NewIndex);
        bSurfaceDescsDirty = true;

        const uint32 DescLODs = Math::Min<uint32>(Desc.NumLODs, MAX_MESH_LODS);
        for (uint32 i = 0; i < DescLODs; ++i)
        {
            MaxSurfaceDescMeshlets = Math::Max(MaxSurfaceDescMeshlets, Desc.LODMeshletCount[i]);
        }

        return NewIndex;
    }

    // Retained slots mirror the device buffer byte for byte, so an equal write needs no upload.
    template <typename T>
    static FORCEINLINE bool StoreIfChanged(T& Slot, const T& Value)
    {
        if (Memory::Memcmp(&Slot, &Value, sizeof(T)) == 0)
        {
            return false;
        }
        Slot = Value;
        return true;
    }

    void FScenePrimitiveSet::RefreshInstances(uint32 Index, TVector<uint32>* DirtySink)
    {
        if (DirtySink == nullptr)
        {
            ++SyncStats.RefreshInstanceCalls;
        }

        const FScenePrimitive&    Prim = Primitives[Index];
        const FPrimitiveCullData& Cull = CullData[Index];
        const FVector4            Sphere = InstanceSphereOf(Index);
        // Its shadow would fall from where it is drawn rather than where it is.
        const bool                bRemapped   = IsRemappedForeground(Prim);
        const bool                bCastShadow = Prim.bCastShadow && !bRemapped;

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Prim.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.size())
            {
                continue;
            }

            EInstanceFlags Flags = Prim.BaseFlags | Binding.MaterialFlags;
            if (bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
                if (Prim.bCastFarShadow)
                {
                    Flags |= EInstanceFlags::CastFarShadow;
                }
            }
            if (bRemapped)
            {
                Flags &= ~EInstanceFlags::HasDistanceField;
            }

            // Skeletal primitives are Active now, so CullInstances compacts them like everything else.
            const bool bAwaitingBones = Prim.Source == EPrimitiveSource::SkeletalMesh
                                     && Prim.BoneCount == 0u;

            if (Prim.Surfaces != nullptr && !bAwaitingBones)
            {
                Flags |= EInstanceFlags::Active;
            }
            if (Prim.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot)
            {
                Flags |= EInstanceFlags::HasGeometry;
            }

            FInstanceCullEntry NewCull = {};
            NewCull.SphereBounds     = Sphere;
            NewCull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
            NewCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
            NewCull.MaxDrawDistance  = Cull.MaxDrawDistance;
            NewCull.LODAndThinning   = PackLODAndThinning(Prim.ForcedLODIndex, 0.0f, Prim.RenderLayers, Prim.CustomDepthStencil);

            const FTransform3x4 NewTransform = PackTransform3x4(InstanceTransformOf(Prim));

            FInstanceStatic NewStatic = {};
            NewStatic.MeshletHeaderSlot = Prim.MeshletHeaderSlot;
            NewStatic.CustomData        = Prim.CustomData;
            NewStatic.MaterialIndex     = Binding.MaterialIndex;
            NewStatic.EntityID          = Prim.EntityID;
            NewStatic.SkinnedIndex      = Binding.SkinnedIndex;
            // Owned by AssignMeshletVisibility, which a rebuilt payload must not undo.
            NewStatic.MeshletVisibilityBase = RetainedStatic[Slot].MeshletVisibilityBase;
            NewStatic.RenderLayers      = Prim.RenderLayers;

            // A resolve bump onto identical values must not dirty the slot, or every instance re-uploads.
            const bool bCullChanged      = StoreIfChanged(RetainedCullEntries[Slot], NewCull);
            const bool bTransformChanged = StoreIfChanged(RetainedTransforms[Slot], NewTransform);
            if (bCullChanged || bTransformChanged)
            {
                if (DirtySink != nullptr)
                {
                    DirtySink->push_back(Slot);
                }
                else
                {
                    MarkInstanceDirty(Slot);
                }
            }
            if (StoreIfChanged(RetainedStatic[Slot], NewStatic))
            {
                MarkStaticDirty(Slot);
            }
        }
    }

    void FScenePrimitiveSet::RefreshInstanceTransform(uint32 Index, TVector<uint32>* DirtySink)
    {
        RefreshInstanceTransform(Index, Primitives[Index].Transform, DirtySink);
    }

    void FScenePrimitiveSet::RefreshInstanceTransform(uint32 Index, const FMatrix4& Transform, TVector<uint32>* DirtySink)
    {
        if (DirtySink == nullptr)
        {
            ++SyncStats.RefreshInstanceCalls;
        }

        const FScenePrimitive& Prim   = Primitives[Index];
        const FVector4         Sphere = InstanceSphereOf(Index);
        const FTransform3x4    Packed = PackTransform3x4(IsRemappedForeground(Prim) ? ForegroundTransform * Transform : Transform);

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const uint32 Slot = Bindings[Prim.BindingBase + s].InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.size())
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
                DirtySink->push_back(Slot);
            }
            else
            {
                MarkInstanceDirty(Slot);
            }
        }
    }

    FMatrix4 FScenePrimitiveSet::InstanceTransformOf(const FScenePrimitive& Prim) const
    {
        return IsRemappedForeground(Prim) ? ForegroundTransform * Prim.Transform : Prim.Transform;
    }

    FVector4 FScenePrimitiveSet::InstanceSphereOf(uint32 Index) const
    {
        const FVector4& Sphere = Bounds[Index];
        if (!IsRemappedForeground(Primitives[Index]))
        {
            return Sphere;
        }
        return FVector4(FVector3(ForegroundTransform * FVector4(FVector3(Sphere), 1.0f)), Sphere.w * ForegroundRadiusScale);
    }

    void FScenePrimitiveSet::TrackForeground(uint32 Index)
    {
        if ((Primitives[Index].RenderLayers & RenderLayers::kForegroundMask) != 0u && !Algo::Contains(ForegroundKeys, Keys[Index]))
        {
            ForegroundKeys.push_back(Keys[Index]);
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

        for (SIZE_T i = 0; i < ForegroundKeys.size();)
        {
            const uint32 Index = FindPrimitive(ForegroundKeys[i]);
            if (Index == Constants::kIndexNoneU32 || (Primitives[Index].RenderLayers & RenderLayers::kForegroundMask) == 0u)
            {
                ForegroundKeys[i] = ForegroundKeys.back();
                ForegroundKeys.pop_back();
                continue;
            }

            // Toggling changes whether it casts shadows, which only the full refresh rewrites.
            if (bToggled)
            {
                RefreshInstances(Index);
            }
            else
            {
                RefreshInstanceTransform(Index);
            }
            ++i;
        }
    }

    void FScenePrimitiveSet::ReleaseBindingSpan(uint32 Base, uint32 Count)
    {
        for (uint32 s = 0; s < Count; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Base + s];
            Batches.ReleaseBatchRef(Binding.BatchIndex, Binding.bSkinned);
            FreeInstanceSlot(Binding.InstanceSlot);
            FreeSkinnedIndex(Binding.SkinnedIndex);
        }
        DeadBindings += Count;
    }

    uint32 FScenePrimitiveSet::AppendBindingSpan(const FBindingMemo& Memo, bool bSkinned, bool bAssignSkinnedIndex)
    {
        const uint32 Base = (uint32)Bindings.size();
        for (const FSurfaceBinding& Proto : Memo.Protos)
        {
            FSurfaceBinding& Binding = Bindings.emplace_back(Proto);
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

        if ((uint32)BindingMemoByHandle.size() <= ResolveHandle)
        {
            BindingMemoByHandle.resize(ResolveHandle + 1u);
        }

        FBindingMemo& Entry = BindingMemoByHandle[ResolveHandle];

        if (Entry.Generation != Generation
            || Entry.RecycleGeneration != Batches.GetRecycleGeneration()
            || Entry.Protos.size() != Surfaces.size())
        {
            Entry.Protos.clear();
            Entry.Protos.reserve(Surfaces.size());

            for (const FResolvedSurface& Surface : Surfaces)
            {
                FSurfaceBinding& Proto = Entry.Protos.emplace_back();

                Proto.BatchIndex            = Batches.FindOrAddBatch(Surface);
                Proto.InstanceSlot          = Constants::kIndexNoneU32;
                Proto.SurfaceDescIndex      = InternSurfaceDesc(Surface);
                Proto.MaterialIndex         = Surface.MaterialIdx;
                Proto.MaterialFlags         = Surface.MaterialFlags;
                Proto.bMaterialCastsShadows = Surface.bMaterialCastsShadows;
                Proto.TexelFactor           = Surface.TexelFactor;
            }

            Entry.Generation        = Generation;
            Entry.RecycleGeneration = Batches.GetRecycleGeneration();
        }

        return &Entry;
    }

    bool FScenePrimitiveSet::BindingsMatchMemo(uint32 Index, const FBindingMemo& Memo) const
    {
        const FScenePrimitive& Prim = Primitives[Index];

        if (Prim.SurfaceCount != (uint32)Memo.Protos.size())
        {
            return false;
        }

        const bool bSkinned = EnumHasAnyFlags(Prim.BaseFlags, EInstanceFlags::Skinned);

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Have = Bindings[Prim.BindingBase + s];
            const FSurfaceBinding& Want = Memo.Protos[s];

            if (Have.BatchIndex            != Want.BatchIndex
             || Have.SurfaceDescIndex      != Want.SurfaceDescIndex
             || Have.MaterialIndex         != Want.MaterialIndex
             || Have.MaterialFlags         != Want.MaterialFlags
             || Have.bMaterialCastsShadows != Want.bMaterialCastsShadows
             || Have.bSkinned              != bSkinned)
            {
                return false;
            }
        }

        return true;
    }

    // Whether the batch still records the shader this surface resolved to for its material slot.
    static bool DeferredMaterialMatches(const FSceneBatchRegistry::FBatch& Batch, const FResolvedSurface& Surface)
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

    // Dynamic meshes re-resolve in place, so bindings must be compared against the surfaces.
    bool FScenePrimitiveSet::BindingsMatchSurfaces(uint32 Index, const TVector<FResolvedSurface>& Surfaces) const
    {
        const FScenePrimitive& Prim = Primitives[Index];

        if (Prim.SurfaceCount != (uint32)Surfaces.size())
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
             || !DeferredMaterialMatches(Batches.Get(Have.BatchIndex), Want))
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

        if (Prim.Surfaces == nullptr || Prim.Surfaces->empty())
        {
            Prim.BindingBase  = 0;
            Prim.SurfaceCount = 0;
            return;
        }

        const TVector<FResolvedSurface>& Surfaces = *Prim.Surfaces;

        const bool bSkinned = EnumHasAnyFlags(Prim.BaseFlags, EInstanceFlags::Skinned);

        const FBindingMemo* Memo = EnsureBindingMemo(Prim.ResolveHandle, Prim.ResolveGeneration, Surfaces);

        Prim.BindingBase  = (uint32)Bindings.size();
        Prim.SurfaceCount = (uint32)Surfaces.size();

        if (Memo != nullptr)
        {
            AppendBindingSpan(*Memo, bSkinned, true);
        }
        else
        {
            for (const FResolvedSurface& Surface : Surfaces)
            {
                const uint32 BatchIndex = Batches.FindOrAddBatch(Surface);
                Batches.AddBatchRef(BatchIndex, bSkinned);

                FSurfaceBinding& Binding = Bindings.emplace_back();
                Binding.BatchIndex            = BatchIndex;
                Binding.InstanceSlot          = AllocateInstanceSlot();
                Binding.SurfaceDescIndex      = InternSurfaceDesc(Surface);
                Binding.MaterialIndex         = Surface.MaterialIdx;
                Binding.MaterialFlags         = Surface.MaterialFlags;
                Binding.bMaterialCastsShadows = Surface.bMaterialCastsShadows;
                Binding.bSkinned              = bSkinned;
                Binding.TexelFactor           = Surface.TexelFactor;
                Binding.SkinnedIndex          = bSkinned ? AllocateSkinnedIndex() : kNoSkinnedIndex;
            }
        }

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }
    }

    struct FScenePrimitiveSet::FSyncPools
    {
        ECS::TComponentStorage<STransformComponent>          Transform    = {};
        ECS::TComponentStorage<SStaticMeshComponent>   StaticMesh   = {};
        ECS::TComponentStorage<SSkeletalMeshComponent> SkeletalMesh = {};
        ECS::TComponentStorage<SDynamicMeshComponent>        DynamicMesh  = {};
        ECS::TComponentStorage<SFoliageComponent>            Foliage      = {};
        const ECS::FSparseSet*                             Disabled     = nullptr;
        const ECS::FSparseSet*                             Sources[kLinkedSources] = {};

        FMeshResolveCache*                                  ResolveCache = &FMeshResolveCache::Get();

        explicit FSyncPools(ECS::FRegistry& Registry)
            : Transform(Registry.GetStorage<STransformComponent>())
            , StaticMesh(Registry.GetStorage<SStaticMeshComponent>())
            , SkeletalMesh(Registry.GetStorage<SSkeletalMeshComponent>())
            , DynamicMesh(Registry.GetStorage<SDynamicMeshComponent>())
            , Foliage(Registry.GetStorage<SFoliageComponent>())
            , Disabled(Registry.GetStorage<SDisabledTag>().GetSet())
        {
            Sources[(uint32)EPrimitiveSource::StaticMesh]   = StaticMesh.GetSet();
            Sources[(uint32)EPrimitiveSource::DynamicMesh]  = DynamicMesh.GetSet();
            Sources[(uint32)EPrimitiveSource::SkeletalMesh] = SkeletalMesh.GetSet();
        }
    };

    namespace
    {
        template <typename TComponent>
        void ReadCommonMeshState(FScenePrimitive& Prim, FPrimitiveCullData& Cull, const TComponent& Component)
        {
            Prim.CustomData      = Component.CustomPrimitiveData.Data.Packed;
            Prim.BoundsScale     = Component.BoundsScale;
            Prim.ForcedLODIndex  = Component.ForcedLODIndex;
            Prim.bCastShadow     = Component.bCastShadow;
            Prim.bCastFarShadow  = Component.bCastFarShadow;
            Prim.RenderLayers    = Component.RenderLayers.Bits;
            Prim.CustomDepthStencil = PackCustomDepthStencil(Component.bRenderCustomDepth, Component.CustomDepthStencilValue);

            Cull.MaxDrawDistance = Component.MaxDrawDistance;
            Cull.bCastShadow     = Component.bCastShadow ? 1u : 0u;
        }
    }

    namespace
    {
        // The raster picks the vertex stride from EInstanceFlags::Skinned and the vertex DATA from the
        // header slot. Disagreement reads 36-byte skinned vertices 28 bytes apart, which draws the mesh
        // as a shattered version of itself rather than failing.
        void WarnOnGeometryKindMismatch(uint32 HeaderSlot, EInstanceFlags Flags, uint32 EntityID,
                                        EPrimitiveSource Source)
        {
#if USING(WITH_EDITOR)
            if (HeaderSlot == MeshletHeaderSlab::kNullSlot)
            {
                return;
            }

            const bool bHeaderSkinned = MeshletHeaderSlab::IsSkinnedSlot(HeaderSlot);
            const bool bFlagSkinned   = EnumHasAnyFlags(Flags, EInstanceFlags::Skinned);
            if (bHeaderSkinned == bFlagSkinned)
            {
                return;
            }

            LOG_ERROR("ScenePrimitiveSet: entity {} source {} names header slot {} ({} geometry) but its "
                      "instance flags say {}. It will rasterize at the wrong vertex stride.",
                      EntityID, (uint32)Source, HeaderSlot,
                      bHeaderSkinned ? "skinned" : "static", bFlagSkinned ? "skinned" : "static");
#else
            (void)HeaderSlot; (void)Flags; (void)EntityID; (void)Source;
#endif
        }
    }

    bool FScenePrimitiveSet::RefreshPrimitiveData(const FSyncPools& Pools, uint32 Index)
    {
        FScenePrimitive&    Prim = Primitives[Index];
        FPrimitiveCullData& Cull = CullData[Index];

        const TVector<FResolvedSurface>* OldSurfaces = Prim.Surfaces;
        const uint32                     OldHandle   = Prim.ResolveHandle;
        const uint32                     OldGen      = Prim.ResolveGeneration;
        const bool                       bHadCustomDepth = Prim.CustomDepthStencil != 0u;

        Prim.Surfaces = nullptr;
        Prim.EntityID = (Prim.Entity).Value;

        bool bResolved = false;
        // Set by sources whose (Surfaces, Handle, Generation) triple cannot report their own change.
        bool bContentChanged = false;

        switch (Prim.Source)
        {
        case EPrimitiveSource::StaticMesh:
        case EPrimitiveSource::SkeletalMesh:
            {
                const SMeshComponent* Base = nullptr;
                const void*           LiveMesh = nullptr;

                if (Prim.Source == EPrimitiveSource::StaticMesh)
                {
                    if (!Pools.StaticMesh->Contains(Prim.Entity)) { return false; }
                    const SStaticMeshComponent* C = &Pools.StaticMesh->Get(Prim.Entity);
                    ReadCommonMeshState(Prim, Cull, *C);
                    Base     = C;
                    LiveMesh = (const void*)C->StaticMesh.Get();
                }
                else
                {
                    if (!Pools.SkeletalMesh->Contains(Prim.Entity)) { return false; }
                    const SSkeletalMeshComponent* C = &Pools.SkeletalMesh->Get(Prim.Entity);
                    ReadCommonMeshState(Prim, Cull, *C);
                    Base     = C;
                    LiveMesh = (const void*)C->SkeletalMesh.Get();

                    // Claimed here rather than at bind, since the slice is keyed to the SKELETON.
                    const CSkeletalMesh*     SkelMesh = C->SkeletalMesh.Get();
                    const FSkeletonResource* SkelRes  = (SkelMesh != nullptr && SkelMesh->Skeleton.IsValid())
                                                      ? SkelMesh->Skeleton->GetSkeletonResource()
                                                      : nullptr;
                    SetBoneCount(Index, SkelRes != nullptr ? (uint32)SkelRes->GetNumBones() : 0u);
                    Prim.RequiredBoneCount = SkelMesh != nullptr ? SkelMesh->GetMeshResource().RequiredBoneCount : 0u;

                    if (Prim.BoneCount != 0u && Prim.RequiredBoneCount > Prim.BoneCount)
                    {
                        LOG_ERROR("Skinning: mesh '{}' references {} bones but its skeleton provides {}. Vertices "
                                  "weighted to joints >= {} read past the bone slice and will be wildly displaced. "
                                  "Mesh and skeleton are out of sync, reimport both.",
                                  SkelMesh->GetName(), Prim.RequiredBoneCount, Prim.BoneCount, Prim.BoneCount);
                    }
                }

                Prim.LocalCenter          = Base->CachedLocalCenter;
                Prim.LocalRadius          = Base->CachedLocalRadius;
                Prim.MeshletHeaderSlot = Base->CachedMeshletHeaderSlot;
                Prim.BaseFlags            = Base->CachedBaseFlags;
                Prim.ResolveHandle        = Base->ResolveHandle;

                if (Prim.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE || (Base->CachedMeshKey != LiveMesh && !Base->bShowingPreviousMesh))
                {
                    if (LiveMesh != nullptr)
                    {
                        FMeshResolveCache::MarkPendingWork();
                    }
                    break;
                }

                FMeshResolveCache& Cache = *Pools.ResolveCache;
                if (!Cache.IsValidHandle(Prim.ResolveHandle))
                {
                    FMeshResolveCache::MarkPendingWork();
                    break;
                }

                const FResolvedMesh& Entry = Cache.GetEntry(Prim.ResolveHandle);
                if (!Entry.bResolved)
                {
                    break;
                }

                Prim.Surfaces          = &Entry.Surfaces;
                Prim.ResolveGeneration = Entry.Generation;
                bResolved              = true;
            }
            break;

        case EPrimitiveSource::DynamicMesh:
            {
                if (!Pools.DynamicMesh->Contains(Prim.Entity)) { return false; }
                SDynamicMeshComponent* C = &Pools.DynamicMesh->Get(Prim.Entity);
                ReadCommonMeshState(Prim, Cull, *C);

                C->SyncedRenderDataVersion = C->LoadRenderDataVersion();

                Prim.DynamicRenderData = C->LoadRenderData();

                const FDynamicMeshRenderData* Data = Prim.DynamicRenderData.get();
                if (Data == nullptr || Data->MeshletHeaderSlot == 0 || Data->Surfaces.empty())
                {
                    Prim.DynamicRenderData.reset();
                    break;
                }

                Prim.LocalCenter          = Data->LocalCenter;
                Prim.LocalRadius          = Data->LocalRadius;
                Prim.MeshletHeaderSlot = Data->MeshletHeaderSlot;
                Prim.ResolveHandle        = INVALID_MESH_RESOLVE_HANDLE;

                EInstanceFlags BaseFlags = EInstanceFlags::None;
                if (C->bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
                if (!C->bReceiveDecals)         { BaseFlags |= EInstanceFlags::NoDecals; }
                if (C->bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
                Prim.BaseFlags = BaseFlags;

                Prim.Surfaces          = &Data->Surfaces;
                Prim.ResolveGeneration = 0;
                bResolved              = true;

                // The identity test is blind here; see BindingsMatchSurfaces.
                bContentChanged = !BindingsMatchSurfaces(Index, Data->Surfaces);
            }
            break;

        default:
            break;
        }

        if ((Prim.CustomDepthStencil != 0u) != bHadCustomDepth)
        {
            CustomDepthCount += bHadCustomDepth ? ~0u : 1u;
        }

        const bool bSurfacesChanged = (Prim.Surfaces != OldSurfaces)
                                   || (Prim.ResolveHandle != OldHandle)
                                   || (Prim.ResolveGeneration != OldGen)
                                   || bContentChanged;
        if (bSurfacesChanged)
        {
            BindSurfaces(Index);
        }

        WarnOnGeometryKindMismatch(Prim.MeshletHeaderSlot, Prim.BaseFlags, Prim.EntityID, Prim.Source);

        // One of the two places a primitive's resolve identity settles; mirror it for the sweep.
        ResolveKeys[Index] = PackResolveKey(Prim.ResolveHandle, Prim.ResolveGeneration);
        if (Prim.ResolveHandle != OldHandle)
        {
            UnlinkHandle(Index, OldHandle);
            LinkHandle(Index, Prim.ResolveHandle);
        }

        RebuildWorldBounds(Index);
        TrackForeground(Index);
        RefreshInstances(Index);
        ++StructureGeneration;

        return bResolved;
    }

    void FScenePrimitiveSet::SyncEntity(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity,
                                        EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        ++SyncStats.SyncEntityCalls;
        InvalidateStreamCache(Entity);
        KillMoveRecord(Entity);

        const uint64 Key = MakeKey(Entity, Source);
        uint32 Index = FindLinked(Entity, Source);
        
        if (Index == Constants::kIndexNoneU32 && Flags == EPrimitiveDirty::Transform)
        {
            return;
        }

        if (Index != Constants::kIndexNoneU32 && Flags == EPrimitiveDirty::Transform)
        {
            if (!Pools.Transform->Contains(Entity))
            {
                return;
            }

            Primitives[Index].Transform = Pools.Transform->Get(Entity).GetWorldMatrixCached();
            RebuildWorldBounds(Index);
            RefreshInstanceTransform(Index);
            ++StructureGeneration;
            return;
        }

        // Does the entity still carry this source, and is it enabled?
        bool bShouldExist = Registry.IsValid(Entity) && !Pools.Disabled->Contains(Entity);
        if (bShouldExist)
        {
            const uint32 SourceIdx = (uint32)Source;
            bShouldExist = (SourceIdx < kLinkedSources) && Pools.Sources[SourceIdx]->Contains(Entity);
        }
        // A transform component is required to place it.
        if (bShouldExist && !Pools.Transform->Contains(Entity))
        {
            bShouldExist = false;
        }

        if (!bShouldExist)
        {
            if (Index != Constants::kIndexNoneU32)
            {
                RemovePrimitive(Key);
            }
            return;
        }

        const bool bNew = (Index == Constants::kIndexNoneU32);
        if (bNew)
        {
            Index = AddPrimitive(Key);
            Primitives[Index].Entity = Entity;
            Primitives[Index].Source = Source;
            Flags |= EPrimitiveDirty::Data | EPrimitiveDirty::Transform;
        }

        if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Transform))
        {
            Primitives[Index].Transform = Pools.Transform->Get(Entity).GetWorldMatrixCached();
        }

        RefreshPrimitiveData(Pools, Index);
    }

    void FScenePrimitiveSet::FlushFoliageReleases()
    {
        if (PendingFoliageReleases.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Sync/Release Foliage");

        // Blocks across every pending entity, since one chunk's foliage is too small to split on its own.
        constexpr uint32 kInstancesPerBlock = 4096;
        uint32 NumBlocks = 0;
        for (const FFoliageEntityState& State : PendingFoliageReleases)
        {
            NumBlocks += ((uint32)State.Instances.size() + kInstancesPerBlock - 1u) / kInstancesPerBlock;
        }
        if ((uint32)FoliageReleaseBlocks.size() < NumBlocks)
        {
            FoliageReleaseBlocks.resize(NumBlocks);
        }
        {
            uint32 Next = 0;
            for (uint32 StateIndex = 0; StateIndex < (uint32)PendingFoliageReleases.size(); ++StateIndex)
            {
                const uint32 NumInstances = (uint32)PendingFoliageReleases[StateIndex].Instances.size();
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
        Task::ParallelFor(NumBlocks, [this](const Task::FParallelRange& Range)
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
                BindingFreeRanges.emplace_back(SpanBase, StateBindings);
                FreeRangeBindings += StateBindings;
            }
        }

        FoliageSlotScratch.resize(Total);
        Task::ParallelFor(NumBlocks, [this](const Task::FParallelRange& Range)
        {
            for (uint32 b = Range.Start; b < Range.End; ++b)
            {
                FFoliageReleaseBlock& Block = FoliageReleaseBlocks[b];
                const TVector<FFoliageInstanceRef>& Instances = PendingFoliageReleases[Block.State].Instances;
                Block.Runs.clear();
                uint32 Write = Block.SlotOffset;
                for (uint32 i = Block.Begin; i < Block.End; ++i)
                {
                    const FFoliageInstanceRef& Ref = Instances[i];
                    for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
                    {
                        const FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
                        if (Block.Runs.empty() || Block.Runs.back().x != Binding.BatchIndex || Block.Runs.back().y != (uint32)Binding.bSkinned)
                        {
                            Block.Runs.emplace_back(Binding.BatchIndex, (uint32)Binding.bSkinned, 0u);
                        }
                        ++Block.Runs.back().z;
                        FoliageSlotScratch[Write++] = Binding.InstanceSlot;
                    }
                }
            }
        }, 4);

        for (uint32 b = 0; b < NumBlocks; ++b)
        {
            for (const FUIntVector3& Run : FoliageReleaseBlocks[b].Runs)
            {
                Batches.ReleaseBatchRefs(Run.x, Run.y != 0u, Run.z);
            }
        }

        DeadBindings += Total;
        PendingFoliageReleases.clear();

        // A slot past the retained arrays was never live, so it must not reach the free list.
        const uint32 SlotLimit = (uint32)RetainedCullEntries.size();
        FoliageSlotScratch.erase(Algo::RemoveIf(FoliageSlotScratch, [SlotLimit](uint32 Slot) { return Slot >= SlotLimit; }),
                                 FoliageSlotScratch.end());

        const uint32 Count = (uint32)FoliageSlotScratch.size();
        LUMINA_PROFILE_VALUE("Sync/ReleasedFoliageSlots", (int64)Count);
        if (Count == 0u)
        {
            return;
        }

        const uint32 NumThreads = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
        if ((uint32)ParallelDirtySlots.size() < NumThreads)
        {
            ParallelDirtySlots.resize(NumThreads);
        }
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Slots.clear();
        }

        // The slots scatter across the retained arrays, so the cache misses spread over workers instead of one thread.
        Task::ParallelFor(Count, [this](const Task::FParallelRange& Range)
        {
            // Pairs of base and word count, so the serial pass after only appends to the free lists.
            TVector<uint32>& FreedRanges = ParallelDirtySlots[Range.Thread].Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const uint32 Slot = FoliageSlotScratch[i];
                RetainedCullEntries[Slot].DrawIDAndFlags &= ~((uint32)EInstanceFlags::Active << 16);
                RetainedStatic[Slot].MeshletVisibilityBase = kNoMeshletVisibility;
                if (Slot < (uint32)SlotMeshletVisibility.size())
                {
                    FUIntVector2& Visibility = SlotMeshletVisibility[Slot];
                    if (Visibility.y != 0u)
                    {
                        FreedRanges.push_back(Visibility.x);
                        FreedRanges.push_back(Visibility.y);
                    }
                    Visibility = FUIntVector2(kNoMeshletVisibility, 0u);
                }
            }
        }, 16384);

        for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            for (size_t i = 0; i + 1 < Bucket.Slots.size(); i += 2)
            {
                FreeMeshletVisibilityRange(Bucket.Slots[i], Bucket.Slots[i + 1]);
            }
        }

        InstanceFreeSlots.insert(InstanceFreeSlots.end(), FoliageSlotScratch.begin(), FoliageSlotScratch.end());

        // Only the active bit changed, which the GPU clears from the slot list instead of a re-send of each entry.
        if (!bFullInstanceUpload)
        {
            DeactivatedSlots.insert(DeactivatedSlots.end(), FoliageSlotScratch.begin(), FoliageSlotScratch.end());
        }
    }

    void FScenePrimitiveSet::ReleaseFoliageInstance(FFoliageInstanceRef& Ref)
    {
        ReleaseBindingSpan(Ref.BindingBase, Ref.SurfaceCount);
        Ref.BindingBase  = 0;
        Ref.SurfaceCount = 0;
        Ref.TypeRow      = 0xFFFFu;
    }

    // The foliage counterpart of RefreshInstances. Everything the primitive path reads off FScenePrimitive
    // comes from the type row or the bake instead, so nothing per instance is stored to read it back.
    void FScenePrimitiveSet::RefreshFoliageInstance(const FFoliageInstanceRef& Ref, const FFoliageTypeResolve& Type,
                                                    const FFoliageBakedInstance& Instance, uint32 EntityID, bool bHidden)
    {
        ++SyncStats.RefreshInstanceCalls;

        for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.size())
            {
                continue;
            }

            FInstanceCullEntry NewCull;
            FTransform3x4      NewTransform;
            FInstanceStatic    NewStatic;
            BuildFoliageEntries(Binding, Type, Instance, EntityID, bHidden, NewCull, NewTransform, NewStatic);
            NewStatic.MeshletVisibilityBase = RetainedStatic[Slot].MeshletVisibilityBase;

            // A rebake or resolve bump rewrites every blade; only the ones that actually moved upload.
            const bool bCullChanged      = StoreIfChanged(RetainedCullEntries[Slot], NewCull);
            const bool bTransformChanged = StoreIfChanged(RetainedTransforms[Slot], NewTransform);
            if (bCullChanged || bTransformChanged)
            {
                MarkInstanceDirty(Slot);
            }
            if (StoreIfChanged(RetainedStatic[Slot], NewStatic))
            {
                MarkStaticDirty(Slot);
            }
        }
    }

    // Below this a field binds faster serially than it takes to fan out.
    static constexpr uint32 FreshFoliageParallelThreshold = 8192;

    void FScenePrimitiveSet::RefreshFoliageInstances(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                                     const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID,
                                                     const FFoliageTypeResolve& UnresolvedType)
    {
        const uint32 Count = (uint32)FoliageRefreshScratch.size();
        auto TypeOf = [&](const FFoliageInstanceRef& Ref) -> const FFoliageTypeResolve&
        {
            return Ref.TypeRow < (uint16)FoliageTypeScratch.size() ? FoliageTypeScratch[Ref.TypeRow] : UnresolvedType;
        };

        if (Count < FreshFoliageParallelThreshold)
        {
            for (uint32 Index : FoliageRefreshScratch)
            {
                const FFoliageInstanceRef& Ref = State.Instances[Index];
                RefreshFoliageInstance(Ref, TypeOf(Ref), Baked[Index], EntityID, Foliage.IsInstanceHidden(Index));
            }
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // Low half marks a surface whose cull or transform changed, high half one whose static data did.
        constexpr uint32 kMaxMaskedSurfaces = 16;
        FoliageRefreshMasks.assign(Count, 0u);

        Task::ParallelFor(Count, [&](const Task::FParallelRange& Range)
        {
            for (uint32 r = Range.Start; r < Range.End; ++r)
            {
                const uint32 Index = FoliageRefreshScratch[r];
                const FFoliageInstanceRef& Ref = State.Instances[Index];
                if (Ref.SurfaceCount > kMaxMaskedSurfaces)
                {
                    continue;
                }

                const FFoliageTypeResolve& Type = TypeOf(Ref);
                const bool bHidden = Foliage.IsInstanceHidden(Index);
                uint32 Mask = 0;
                for (uint32 Surface = 0; Surface < Ref.SurfaceCount; ++Surface)
                {
                    const FSurfaceBinding& Binding = Bindings[Ref.BindingBase + Surface];
                    const uint32 Slot = Binding.InstanceSlot;
                    if (Slot >= (uint32)RetainedCullEntries.size())
                    {
                        continue;
                    }

                    FInstanceCullEntry NewCull;
                    FTransform3x4      NewTransform;
                    FInstanceStatic    NewStatic;
                    BuildFoliageEntries(Binding, Type, Baked[Index], EntityID, bHidden, NewCull, NewTransform, NewStatic);
                    NewStatic.MeshletVisibilityBase = RetainedStatic[Slot].MeshletVisibilityBase;

                    const bool bCullChanged      = StoreIfChanged(RetainedCullEntries[Slot], NewCull);
                    const bool bTransformChanged = StoreIfChanged(RetainedTransforms[Slot], NewTransform);
                    Mask |= (bCullChanged || bTransformChanged) ? (1u << Surface) : 0u;
                    Mask |= StoreIfChanged(RetainedStatic[Slot], NewStatic) ? (1u << (Surface + 16)) : 0u;
                }
                FoliageRefreshMasks[r] = Mask;
            }
        }, 4096);

        SyncStats.RefreshInstanceCalls += Count;
        for (uint32 r = 0; r < Count; ++r)
        {
            const uint32 Index = FoliageRefreshScratch[r];
            const FFoliageInstanceRef& Ref = State.Instances[Index];
            if (Ref.SurfaceCount > kMaxMaskedSurfaces)
            {
                RefreshFoliageInstance(Ref, TypeOf(Ref), Baked[Index], EntityID, Foliage.IsInstanceHidden(Index));
                continue;
            }

            const uint32 Mask = FoliageRefreshMasks[r];
            for (uint32 Surface = 0; Mask != 0u && Surface < Ref.SurfaceCount; ++Surface)
            {
                const uint32 Slot = Bindings[Ref.BindingBase + Surface].InstanceSlot;
                if (Mask & (1u << Surface))
                {
                    MarkInstanceDirty(Slot);
                }
                if (Mask & (1u << (Surface + 16)))
                {
                    MarkStaticDirty(Slot);
                }
            }
        }
    }

    void FScenePrimitiveSet::BuildFoliageEntries(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type,
                                                 const FFoliageBakedInstance& Instance, uint32 EntityID, bool bHidden,
                                                 FInstanceCullEntry& OutCull, FTransform3x4& OutTransform, FInstanceStatic& OutStatic)
    {
        EInstanceFlags Flags = Type.BaseFlags | Binding.MaterialFlags;
        if (Type.bCastShadow && Binding.bMaterialCastsShadows)
        {
            Flags |= EInstanceFlags::CastShadow;
            if (Type.bCastFarShadow)
            {
                Flags |= EInstanceFlags::CastFarShadow;
            }
        }
        if (Type.Surfaces != nullptr && !bHidden)
        {
            Flags |= EInstanceFlags::Active;
        }
        if (Type.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot)
        {
            Flags |= EInstanceFlags::HasGeometry;
        }

        OutCull = {};
        // The bake already produced the world sphere; there is no local sphere to transform.
        OutCull.SphereBounds     = Instance.SphereBounds;
        OutCull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
        OutCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
        OutCull.MaxDrawDistance  = Type.MaxDrawDistance;
        OutCull.LODAndThinning   = PackLODAndThinning(-1, Type.ThinningStartDistance);

        OutTransform = PackTransform3x4(Instance.Transform);

        OutStatic = {};
        OutStatic.MeshletHeaderSlot = Type.MeshletHeaderSlot;
        OutStatic.MaterialIndex     = Binding.MaterialIndex;
        OutStatic.EntityID          = EntityID;
        OutStatic.SkinnedIndex      = kNoSkinnedIndex;
    }

    void FScenePrimitiveSet::BindFreshFoliage(FFoliageEntityState& State, const SFoliageComponent& Foliage, const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 TypeCount   = (uint32)FoliageTypeScratch.size();
        const uint32 NewCount    = (uint32)Baked.size();

        // Serial, because each span starts where the previous one ended. Offsets are relative until the span is placed.
        TVector<uint32> InstancesPerType(TypeCount, 0u);
        uint32 TotalBindings = 0;
        for (uint32 i = 0; i < NewCount; ++i)
        {
            const int32 TypeIndex = Baked[i].TypeIndex;
            const bool  bValidType = TypeIndex >= 0 && (uint32)TypeIndex < TypeCount;
            const FBindingMemo* Memo = bValidType ? FoliageMemoScratch[TypeIndex] : nullptr;
            const uint32 WantCount = (Memo != nullptr) ? (uint32)Memo->Protos.size() : 0u;

            FFoliageInstanceRef& Ref = State.Instances[i];
            Ref.TypeRow      = bValidType ? (uint16)TypeIndex : 0xFFFFu;
            Ref.SurfaceCount = (uint16)WantCount;
            Ref.BindingBase  = TotalBindings;
            TotalBindings   += WantCount;
            if (Memo != nullptr)
            {
                ++InstancesPerType[TypeIndex];
            }
        }

        for (uint32 t = 0; t < TypeCount; ++t)
        {
            if (InstancesPerType[t] == 0u || FoliageMemoScratch[t] == nullptr)
            {
                continue;
            }
            const bool bSkinned = EnumHasAnyFlags(FoliageTypeScratch[t].BaseFlags, EInstanceFlags::Skinned);
            for (const FSurfaceBinding& Proto : FoliageMemoScratch[t]->Protos)
            {
                Batches.AddBatchRefs(Proto.BatchIndex, bSkinned, InstancesPerType[t]);
            }
        }

        // A removed chunk's foliage left a span about this size, so streaming reuses it rather than growing toward a compaction.
        const uint32 BindingBase = TakeBindingRange(TotalBindings);

        // From the free list, since a streamed chunk's foliage frees its slots one at a time and a fresh block never reused them.
        FoliageSlotScratch.resize(TotalBindings);
        for (uint32& Slot : FoliageSlotScratch)
        {
            // A fresh slot is marked by the allocator, and a reused one is not.
            const bool bReused = !InstanceFreeSlots.empty();
            Slot = AllocateInstanceSlot();
            if (bReused)
            {
                MarkInstanceDirty(Slot);
                MarkStaticDirty(Slot);
            }
        }

        Task::ParallelFor(NewCount, [&](const Task::FParallelRange& Range)
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
                const FBindingMemo&        Memo = *FoliageMemoScratch[Ref.TypeRow];
                const bool bSkinned = EnumHasAnyFlags(Type.BaseFlags, EInstanceFlags::Skinned);

                for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
                {
                    FSurfaceBinding& Binding = Bindings[Ref.BindingBase + s];
                    Binding              = Memo.Protos[s];
                    Binding.InstanceSlot = FoliageSlotScratch[Offset + s];
                    Binding.bSkinned     = bSkinned;

                    BuildFoliageEntries(Binding, Type, Baked[i], EntityID, Foliage.IsInstanceHidden(i),
                                        RetainedCullEntries[Binding.InstanceSlot],
                                        RetainedTransforms[Binding.InstanceSlot],
                                        RetainedStatic[Binding.InstanceSlot]);
                }
            }
        }, 4096);

        SyncStats.BindCalls            += NewCount;
        SyncStats.RefreshInstanceCalls += NewCount;
    }

    void FScenePrimitiveSet::SyncFoliage(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity,
                                         EPrimitiveDirty Flags)
    {
        (void)Flags;

        // Counted, not scoped; see SyncEntity.
        ++SyncStats.SyncFoliageCalls;

        SFoliageComponent* Foliage = (Registry.IsValid(Entity) && Pools.Foliage->Contains(Entity))
                                   ? &Pools.Foliage->Get(Entity)
                                   : nullptr;
        const bool bEnabled = Foliage != nullptr && !Pools.Disabled->Contains(Entity);

        if (!bEnabled)
        {
            RemoveEntity(Registry, Entity, EPrimitiveSource::Foliage);
            return;
        }

        Foliage->EnsureRenderCache();
        Foliage->BakeRetryGeneration = FMeshResolveCache::GetPendingGeneration();

        const TVector<FFoliageBakedInstance>& Baked = Foliage->BakedInstances;
        const uint32 NewCount = (uint32)Baked.size();

        FFoliageEntityState& State = FoliageByEntity[Entity];

        const uint32 PreviousCount = (uint32)State.Instances.size();
        const bool bBakeChanged = State.SyncedBakeSerial != Foliage->BakeSerial
                               || NewCount != PreviousCount;
        State.SyncedBakeSerial = Foliage->BakeSerial;

        // Shrink drops the tail; grow and overlap are handled by the write loop below.
        for (uint32 i = NewCount; i < (uint32)State.Instances.size(); ++i)
        {
            ReleaseFoliageInstance(State.Instances[i]);
        }
        State.Instances.resize(NewCount);

        const uint32 TypeCount = (uint32)Foliage->Types.size();
        FoliageTypeScratch.clear();
        FoliageTypeScratch.resize(TypeCount);

        // Whether each type's resolve identity moved since the last sync; unchanged types skip their instances.
        const bool bTypeLayoutChanged = (uint32)State.TypeResolveKeys.size() != TypeCount;
        FoliageTypeChangedScratch.assign(TypeCount, bTypeLayoutChanged);
        if (bTypeLayoutChanged)
        {
            State.TypeResolveKeys.assign(TypeCount, PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0));
        }

        FMeshResolveCache& Cache = *Pools.ResolveCache;
        for (uint32 t = 0; t < TypeCount; ++t)
        {
            const SFoliageType&   Type = Foliage->Types[t];
            FFoliageTypeResolve&  Out  = FoliageTypeScratch[t];

            Out.MeshletHeaderSlot = Type.CachedMeshletHeaderSlot;
            Out.BaseFlags            = Type.CachedBaseFlags;
            Out.ResolveHandle        = Type.ResolveHandle;
            Out.bCastShadow          = Type.bCastShadow;
            Out.bCastFarShadow       = Type.bCastFarShadow;
            Out.ThinningStartDistance = Type.ThinningStartDistance;
            Out.MaxDrawDistance      = Type.CullDistance;

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
            if (State.TypeResolveKeys[t] != NewKey)
            {
                FoliageTypeChangedScratch[t] = true;
                State.TypeResolveKeys[t]     = NewKey;
            }

            // Per type rather than per instance; every instance of a type carries the same pair.
            WarnOnGeometryKindMismatch(Out.MeshletHeaderSlot, Out.BaseFlags, (Entity).Value,
                                       EPrimitiveSource::Foliage);
        }

        // Fetched once per type, not once per instance, since every instance of a type binds identical
        // protos. This is what turns a field of foliage into a handful of memo lookups.
        FoliageMemoScratch.clear();
        FoliageMemoScratch.resize(TypeCount, nullptr);
        for (uint32 t = 0; t < TypeCount; ++t)
        {
            const FFoliageTypeResolve& Type = FoliageTypeScratch[t];
            if (Type.Surfaces != nullptr && !Type.Surfaces->empty())
            {
                EnsureBindingMemo(Type.ResolveHandle, Type.Generation, *Type.Surfaces);
            }
        }

        // Addresses are taken only once every memo above exists, because EnsureBindingMemo grows the memo
        // table by handle and a pointer captured before a later call can be left dangling by it.
        for (uint32 t = 0; t < TypeCount; ++t)
        {
            const FFoliageTypeResolve& Type = FoliageTypeScratch[t];
            if (Type.Surfaces != nullptr && !Type.Surfaces->empty()
                && Type.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE
                && Type.ResolveHandle < (uint32)BindingMemoByHandle.size())
            {
                FoliageMemoScratch[t] = &BindingMemoByHandle[Type.ResolveHandle];
            }
        }

        const uint32 EntityID = (Entity).Value;

        // Stands in for an instance whose TypeIndex is out of range; it binds nothing, so the refresh below
        // writes nothing for it.
        const FFoliageTypeResolve UnresolvedType;

        const bool bFreshBind = PreviousCount == 0u && NewCount >= FreshFoliageParallelThreshold;
        if (bFreshBind)
        {
            BindFreshFoliage(State, *Foliage, Baked, EntityID);
        }

        // Rebinds stay serial since each appends a span, and the refreshes they gather run after them, in parallel for a big field.
        FoliageRefreshScratch.clear();

        for (uint32 i = 0; i < NewCount && !bFreshBind; ++i)
        {
            const FFoliageBakedInstance& Instance = Baked[i];
            FFoliageInstanceRef&         Ref      = State.Instances[i];

            const bool bValidType = Instance.TypeIndex >= 0 && (uint32)Instance.TypeIndex < TypeCount;
            const uint16 TypeRow  = bValidType ? (uint16)Instance.TypeIndex : 0xFFFFu;

            if (!bBakeChanged && Ref.TypeRow == TypeRow && (!bValidType || !FoliageTypeChangedScratch[TypeRow]))
            {
                ++SyncStats.BindsSkipped;
                continue;
            }

            const FFoliageTypeResolve& Type = bValidType ? FoliageTypeScratch[TypeRow] : UnresolvedType;
            const FBindingMemo*        Memo = bValidType ? FoliageMemoScratch[TypeRow] : nullptr;

            const uint32 WantCount = (Memo != nullptr) ? (uint32)Memo->Protos.size() : 0u;

            // A memo is rebuilt in place when its generation moves, so a span that still matches the type
            // and the surface count is still bound to the right batches and needs no rebind.
            if (Ref.TypeRow != TypeRow || Ref.SurfaceCount != WantCount || !FoliageBindingsMatchMemo(Ref, Memo))
            {
                ReleaseFoliageInstance(Ref);
                ++SyncStats.BindCalls;
                if (Memo != nullptr)
                {
                    const bool bSkinned = EnumHasAnyFlags(Type.BaseFlags, EInstanceFlags::Skinned);
                    Ref.BindingBase  = AppendBindingSpan(*Memo, bSkinned, false);
                    Ref.SurfaceCount = (uint16)WantCount;
                }
                Ref.TypeRow = TypeRow;
            }
            else
            {
                ++SyncStats.BindsSkipped;
            }

            FoliageRefreshScratch.push_back(i);
        }

        RefreshFoliageInstances(State, *Foliage, Baked, EntityID, UnresolvedType);

        // Every slot now holds its transform and bounds, so a second copy per instance is hundreds of megabytes in a large field.
        Foliage->ReleaseRenderCache();

        // The binds above already read the hidden flags, so edits queued before them are settled.
        Foliage->PendingVisibilityEdits.clear();

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }

        ++StructureGeneration;
    }

    void FScenePrimitiveSet::ApplyFoliageVisibilityEdits(ECS::FRegistry& Registry)
    {
        for (auto&& [Entity, Foliage] : Registry.View<SFoliageComponent>().Each())
        {
            if (Foliage.PendingVisibilityEdits.empty())
            {
                continue;
            }
            auto Found = FoliageByEntity.find(Entity);
            if (Found == FoliageByEntity.end())
            {
                // Not bound yet, and the first bind reads the hidden flags itself.
                continue;
            }

            const uint32 ActiveBit = (uint32)EInstanceFlags::Active << 16;
            const FFoliageEntityState& State = Found->second;
            for (uint32 Index : Foliage.PendingVisibilityEdits)
            {
                if (Index >= (uint32)State.Instances.size())
                {
                    continue;
                }
                const FFoliageInstanceRef& Ref = State.Instances[Index];
                const bool bHidden = Foliage.IsInstanceHidden(Index);
                for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
                {
                    const uint32 Slot = Bindings[Ref.BindingBase + s].InstanceSlot;
                    if (Slot >= (uint32)RetainedCullEntries.size())
                    {
                        continue;
                    }
                    uint32& DrawIDAndFlags = RetainedCullEntries[Slot].DrawIDAndFlags;
                    const uint32 Updated = bHidden ? (DrawIDAndFlags & ~ActiveBit) : (DrawIDAndFlags | ActiveBit);
                    if (Updated != DrawIDAndFlags)
                    {
                        DrawIDAndFlags = Updated;
                        MarkInstanceDirty(Slot);
                    }
                }
            }
            Foliage.PendingVisibilityEdits.clear();
        }
    }

    bool FScenePrimitiveSet::FoliageBindingsMatchMemo(const FFoliageInstanceRef& Ref, const FBindingMemo* Memo) const
    {
        if (Memo == nullptr)
        {
            return Ref.SurfaceCount == 0u;
        }

        for (uint32 s = 0; s < Ref.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Have = Bindings[Ref.BindingBase + s];
            const FSurfaceBinding& Want = Memo->Protos[s];

            if (Have.BatchIndex       != Want.BatchIndex
             || Have.SurfaceDescIndex != Want.SurfaceDescIndex
             || Have.MaterialIndex    != Want.MaterialIndex
             || Have.MaterialFlags    != Want.MaterialFlags
             || Have.bMaterialCastsShadows != Want.bMaterialCastsShadows)
            {
                return false;
            }
        }

        return true;
    }

    // Foliage checks one key per TYPE rather than one per instance, so a field of it adds a handful of
    // comparisons to the sweep instead of one per painted blade.
    void FScenePrimitiveSet::SweepFoliageResolves(FMeshResolveCache& Cache)
    {
        for (const auto& Pair : FoliageByEntity)
        {
            for (const uint64 Packed : Pair.second.TypeResolveKeys)
            {
                const uint32 Handle = (uint32)(Packed & 0xFFFFFFFFull);
                if (Handle >= (uint32)GenSnapshot.size())
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
                    RetryScratch.push_back(MakeKey(Pair.first, EPrimitiveSource::Foliage));
                    break;
                }
            }
        }
    }

    void FScenePrimitiveSet::FullRescan(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        Batches.Reset();
        // Always paired with Batches.Reset(), which renumbers every batch index the memo cached.
        BindingMemoByHandle.clear();
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        ResolveKeys.clear();
        HandleListHead.clear();
        NextByHandle.clear();
        PrevByHandle.clear();
        LastSweptTableGeneration = 0;
        Bindings.clear();
        // Every slice died with the primitives, so the arena restarts rather than leaking live bases.
        BoneSliceFreeLists.clear();
        BoneSliceExtent = 0;
        BoneSliceSweepCursor = 0;
        ++SkeletalSetGeneration;
        DeadBindings = 0;
        BindingFreeRanges.clear();
        FreeRangeBindings = 0;
        PendingFoliageReleases.clear();
        SkinnedIndexFreeList.clear();
        SkinnedIndexExtent = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        BonelessSkinnedCount = 0;
        CustomDepthCount = 0;
        RetainedCullEntries.clear();
        RetainedTransforms.clear();
        RetainedStatic.clear();
        ResetMeshletVisibility();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        DirtyStaticSlots.clear();
        bFullInstanceUpload = true;
        bFullStaticUpload   = true;
        SurfaceDescs.clear();
        SurfaceDescByHash.clear();
        bSurfaceDescsDirty = true;
        MaxSurfaceDescMeshlets = 0;
        FoliageByEntity.clear();

        const FSyncPools Pools(Registry);

        // Level load rebuilds from nothing, so unlike the incremental path the counts here are exact.
        const SIZE_T MeshCount = Pools.StaticMesh->GetDenseSize() + Pools.SkeletalMesh->GetDenseSize() + Pools.DynamicMesh->GetDenseSize();
        Primitives.reserve(MeshCount);
        Bounds.reserve(MeshCount);
        CullData.reserve(MeshCount);
        Keys.reserve(MeshCount);
        ResolveKeys.reserve(MeshCount);
        Bindings.reserve(MeshCount);
        RetainedCullEntries.reserve(MeshCount);
        RetainedTransforms.reserve(MeshCount);
        RetainedStatic.reserve(MeshCount);

        for (ECS::FEntity Entity : Registry.View<SStaticMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::StaticMesh, EPrimitiveDirty::All);
        }
        for (ECS::FEntity Entity : Registry.View<SDynamicMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::All);
        }
        for (ECS::FEntity Entity : Registry.View<SSkeletalMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::SkeletalMesh, EPrimitiveDirty::All);
        }
        for (ECS::FEntity Entity : Registry.View<SFoliageComponent>())
        {
            SyncFoliage(Registry, Pools, Entity, EPrimitiveDirty::All);
        }
        FlushFoliageReleases();

        ++StructureGeneration;
    }

    void FScenePrimitiveSet::PollUnhookedSources(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        // An incomplete bake retries when a resolve lands, not every frame.
        const uint32 ResolveGeneration = FMeshResolveCache::GetPendingGeneration();
        for (auto&& [Entity, Foliage] : Registry.View<SFoliageComponent>().Each())
        {
            if (Foliage.BakedVersion != Foliage.InstancesVersion
                || (Foliage.bBakeIncomplete && Foliage.BakeRetryGeneration != ResolveGeneration))
            {
                Tracker.Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
            }
        }
        
        for (auto&& [Entity, Mesh] : Registry.View<SDynamicMeshComponent>().Each())
        {
            if (Mesh.SyncedRenderDataVersion != Mesh.LoadRenderDataVersion())
            {
                Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);
            }
        }

        PollSkeletalBoneRanges(Registry, Tracker);
    }

    // A skeleton finishing its load marks nothing dirty, so a primitive that synced mid-load stays boneless.
    void FScenePrimitiveSet::PollSkeletalBoneRanges(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        if (BonelessSkinnedCount == 0u)
        {
            return;
        }

        const ECS::TComponentStorage<SSkeletalMeshComponent> Storage =
            Registry.GetStorage<SSkeletalMeshComponent>();

        // The cached skeletal list, so an unchanged scene costs O(skinned) rather than O(primitives).
        for (uint32 Index : GetSkeletalIndices())
        {
            const FScenePrimitive& Prim = Primitives[Index];

            // A live slice can only need resizing when the mesh changes, which marks the primitive itself.
            if (Prim.BoneCount != 0u)
            {
                continue;
            }

            if (!Storage->Contains(Prim.Entity))
            {
                continue;
            }

            const CSkeletalMesh* SkelMesh = Storage->Get(Prim.Entity).SkeletalMesh.Get();
            const FSkeletonResource* SkelRes = (SkelMesh != nullptr && SkelMesh->Skeleton.IsValid())
                                             ? SkelMesh->Skeleton->GetSkeletonResource()
                                             : nullptr;

            if (SkelRes != nullptr && SkelRes->GetNumBones() > 0)
            {
                Tracker.Mark(Prim.Entity, EPrimitiveSource::SkeletalMesh, EPrimitiveDirty::Data);
            }
        }
    }

    // See FCoalescedEntity for why a frame's entries are folded before any of them are applied.
    void FScenePrimitiveSet::CoalesceDrain(TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Coalesce");

        // Stamping beats clearing, since the table is sized by the entity index space.
        ++CoalesceStamp;
        CoalescedScratch.clear();
        CoalescedScratch.reserve(DrainScratch.size() + Moved.size());

        auto RecordFor = [this](ECS::FEntity Entity) -> FCoalescedEntity&
        {
            const uint32 Slot = (uint32)Entity.GetIndex();
            if (Slot >= (uint32)CoalesceByEntityIndex.size())
            {
                CoalesceByEntityIndex.resize(Slot + 1u);
            }

            FCoalesceSlot& Mapped = CoalesceByEntityIndex[Slot];
            if (Mapped.Stamp == CoalesceStamp
                && Mapped.Index < (uint32)CoalescedScratch.size()
                && CoalescedScratch[Mapped.Index].Entity == Entity.Value)
            {
                return CoalescedScratch[Mapped.Index];
            }

            Mapped.Stamp = CoalesceStamp;
            Mapped.Index = (uint32)CoalescedScratch.size();
            FCoalescedEntity& Record = CoalescedScratch.emplace_back();
            Record.Entity = Entity.Value;
            return Record;
        };

        // A move dirties every linked source but never foliage, which bakes its own world transform.
        auto FoldAllSources = [](FCoalescedEntity& Record, EPrimitiveDirty Flags)
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
            FCoalescedEntity& Record = RecordFor(Entry.Entity);
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
        if (!CoalescedScratch.empty() && !Moved.empty())
        {
            const uint32 NumThreads = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 1u;
            if ((uint32)ParallelOverlaps.size() < Math::Max(NumThreads, 1u))
            {
                ParallelOverlaps.resize(Math::Max(NumThreads, 1u));
            }
            for (TVector<ECS::FEntity>& Bucket : ParallelOverlaps)
            {
                Bucket.clear();
            }

            Task::ParallelFor((uint32)Moved.size(), [this, Moved](const Task::FParallelRange& Range)
            {
                TVector<ECS::FEntity>& Bucket = ParallelOverlaps[Range.Thread];
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    if (Moved[i] != ECS::NullEntity && HasCoalescedRecord(Moved[i]))
                    {
                        Bucket.push_back(Moved[i]);
                    }
                }
            }, 2048);

            for (const TVector<ECS::FEntity>& Bucket : ParallelOverlaps)
            {
                for (ECS::FEntity Entity : Bucket)
                {
                    FoldAllSources(RecordFor(Entity), EPrimitiveDirty::Transform);
                }
            }
        }

        SyncStats.CoalescedEntities = (uint32)CoalescedScratch.size();
    }

    void FScenePrimitiveSet::ReserveForDrain()
    {
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

        const SIZE_T PrimitiveTarget = Primitives.size() + NewPrimitives;
        ReserveGeometric(Primitives, PrimitiveTarget);
        ReserveGeometric(Bounds, PrimitiveTarget);
        ReserveGeometric(CullData, PrimitiveTarget);
        ReserveGeometric(Keys, PrimitiveTarget);
        ReserveGeometric(ResolveKeys, PrimitiveTarget);

        ReserveGeometric(Bindings, Bindings.size() + NewPrimitives);

        const SIZE_T FreeSlots         = InstanceFreeSlots.size();
        const SIZE_T NeededSlots       = NewPrimitives > FreeSlots ? NewPrimitives - FreeSlots : 0;
        const SIZE_T InstanceTarget    = RetainedCullEntries.size() + NeededSlots;
        ReserveGeometric(RetainedCullEntries, InstanceTarget);
        ReserveGeometric(RetainedTransforms, InstanceTarget);
        ReserveGeometric(RetainedStatic, InstanceTarget);
    }

    void FScenePrimitiveSet::PartitionDrain()
    {
        TransformRecords.clear();
        StructuralRecords.clear();
        ReserveGeometric(TransformRecords, CoalescedScratch.size());

        for (uint32 i = 0, N = (uint32)CoalescedScratch.size(); i < N; ++i)
        {
            const FCoalescedEntity& Record = CoalescedScratch[i];

            EPrimitiveDirty All = Record.Flags[(uint32)EPrimitiveSource::Foliage];
            for (uint32 s = 0; s < kLinkedSources; ++s)
            {
                All |= Record.Flags[s];
            }

            if (All == EPrimitiveDirty::None)
            {
                continue;
            }

            if (All == EPrimitiveDirty::Transform)
            {
                TransformRecords.push_back(i);
            }
            else
            {
                StructuralRecords.push_back(i);
            }
        }

        SyncStats.TransformRecords  = (uint32)TransformRecords.size();
        SyncStats.StructuralRecords = (uint32)StructuralRecords.size();
    }

    void FScenePrimitiveSet::ApplyStructuralRecords(ECS::FRegistry& Registry, const FSyncPools& Pools)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Structural");
        LUMINA_PROFILE_VALUE("Sync/StructuralRecords", (int64)StructuralRecords.size());

        for (uint32 RecordIndex : StructuralRecords)
        {
            const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];
            const ECS::FEntity      Entity = (ECS::FEntity)Record.Entity;

            for (uint32 s = 0; s < kLinkedSources; ++s)
            {
                if (Record.Flags[s] != EPrimitiveDirty::None)
                {
                    SyncEntity(Registry, Pools, Entity, (EPrimitiveSource)s, Record.Flags[s]);
                }
            }

            const EPrimitiveDirty FoliageFlags = Record.Flags[(uint32)EPrimitiveSource::Foliage];
            if (FoliageFlags != EPrimitiveDirty::None
                && (Pools.Foliage->Contains(Entity)
                    || (!FoliageByEntity.empty() && FoliageByEntity.find(Entity) != FoliageByEntity.end())))
            {
                SyncFoliage(Registry, Pools, Entity, FoliageFlags);
            }
        }

        FlushFoliageReleases();

        if (ShouldCompactBindings())
        {
            CompactBindings();
        }
    }

    void FScenePrimitiveSet::ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex,
                                                  TVector<uint32>* OutDirty)
    {
        const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];

        uint8 SourceBits = 0;
        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            SourceBits |= Record.Flags[s] != EPrimitiveDirty::None ? (uint8)(1u << s) : (uint8)0u;
        }
        ApplyEntityTransform(Pools, (ECS::FEntity)Record.Entity, SourceBits, OutDirty);
    }

    void FScenePrimitiveSet::ApplyEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint8 SourceBits,
                                                  TVector<uint32>* OutDirty)
    {
        const uint8 Mask = GetSourceMask(Entity) & SourceBits;
        if (Mask == 0)
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
            if ((Mask & (1u << s)) == 0)
            {
                continue;
            }

            const uint32 Index = FindLinked(Entity, (EPrimitiveSource)s);
            if (Index == Constants::kIndexNoneU32)
            {
                continue;
            }

            StreamMatrix(Primitives[Index].Transform, Matrix);
            RebuildWorldBounds(Index, Matrix);
            RefreshInstanceTransform(Index, Matrix, OutDirty);
        }
    }

    static_assert(offsetof(FScenePrimitive, Transform) == 0 && alignof(FScenePrimitive) >= 64 && sizeof(FMatrix4) == 64,
                  "StreamMatrix writes the first cache line of a primitive as two aligned 32-byte halves.");

    bool FScenePrimitiveSet::HasCoalescedRecord(ECS::FEntity Entity) const
    {
        const uint32 Slot = (uint32)Entity.GetIndex();
        if (Slot >= (uint32)CoalesceByEntityIndex.size())
        {
            return false;
        }
        const FCoalesceSlot& Mapped = CoalesceByEntityIndex[Slot];
        return Mapped.Stamp == CoalesceStamp
            && Mapped.Index < (uint32)CoalescedScratch.size()
            && CoalescedScratch[Mapped.Index].Entity == Entity.Value;
    }

    uint32 FScenePrimitiveSet::BeginMovedClaims()
    {
        if (MovedClaimByEntityIndex.empty())
        {
            MovedClaimByEntityIndex.resize((SIZE_T)ECS::FEntity::IndexMask + 1u, 0u);
        }
        // Zero is the never-claimed value, so a wrapped stamp clears the table rather than reusing it.
        if (++MovedClaimStamp == 0u)
        {
            std::fill(MovedClaimByEntityIndex.begin(), MovedClaimByEntityIndex.end(), 0u);
            MovedClaimStamp = 1u;
        }
        return MovedClaimStamp;
    }

    void FScenePrimitiveSet::EnsureParallelBuckets()
    {
        const uint32 NumSlots = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
        if ((uint32)ParallelDirtySlots.size() < Math::Max(NumSlots, 1u))
        {
            ParallelDirtySlots.resize(Math::Max(NumSlots, 1u));
        }
    }

    void FScenePrimitiveSet::ApplyMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Moved");

        // Movers whose move-time records were thrown away go round again with the rest.
        if (!MoveReplay.empty())
        {
            MoveReplay.insert(MoveReplay.end(), Moved.begin(), Moved.end());
            Moved = TSpan<const ECS::FEntity>(MoveReplay.data(), MoveReplay.size());
        }
        const bool bReplayed = !MoveReplay.empty();

        const uint32 Count = (uint32)Moved.size();
        const uint32 NumSlots = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
        const bool bStream = Count >= kMinStreamedMoves && NumSlots > 1 && StreamCount + Count <= StreamTarget.Capacity
                          && StreamTarget.Records != nullptr && StreamTarget.Slots != nullptr;
        if (bStream)
        {
            StreamMovedTransforms(Pools, Moved);
        }
        else if (Count > 0)
        {
            ApplyMovedLegacy(Pools, Moved);
        }
        else
        {
            // The catch-up reads this frame's claims, so a frame the movers covered alone still opens a pass.
            BeginMovedClaims();
        }
        MoveReplay.clear();

        Algo::Sort(StreamedBlocks);
        StreamedBlocks.erase(Algo::Unique(StreamedBlocks), StreamedBlocks.end());

        // The movers count each entity once, but the drained list can repeat one, so only a frame they streamed alone can trust the count.
        const bool bNoneStopped = bStreamListsExact && ContinuingStreams == StreamedLastFrame.Count;
        bStreamListsExact = !bStream && !bReplayed;
        ContinuingStreams = 0;
        CatchUpStoppedStreams(Pools, MovedClaimStamp, bNoneStopped);
    }

    void FScenePrimitiveSet::ApplyMovedLegacy(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        const uint32 Count = (uint32)Moved.size();
        ++StructureGeneration;
        SyncStats.SyncEntityCalls += Count;

        const uint32 Stamp = BeginMovedClaims();
        uint32* Claims = MovedClaimByEntityIndex.data();

        const auto ApplyOne = [this, &Pools, Stamp, Claims](ECS::FEntity Entity, TVector<uint32>* OutDirty)
        {
            if (Entity == ECS::NullEntity || HasCoalescedRecord(Entity))
            {
                return;
            }
            uint32& Claim = Claims[Entity.GetIndex()];
            if (std::atomic_ref<uint32>(Claim).exchange(Stamp, std::memory_order_relaxed) == Stamp)
            {
                return;
            }
            ApplyEntityTransform(Pools, Entity, kMovedSources, OutDirty);
        };

        constexpr uint32 kParallelThreshold = 1024;
        const uint32 NumSlots = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
        if (Count < kParallelThreshold || NumSlots <= 1)
        {
            for (ECS::FEntity Entity : Moved)
            {
                ApplyOne(Entity, nullptr);
            }
            _mm_sfence();
            return;
        }

        SyncStats.RefreshInstanceCalls += Count;

        EnsureParallelBuckets();
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Slots.clear();
        }

        // Streaming stores are not ordered by the join, so each range fences its own.
        Task::ParallelFor(Count, [&ApplyOne, Moved, this](const Task::FParallelRange& Range)
        {
            TVector<uint32>& Bucket = ParallelDirtySlots[Range.Thread].Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                ApplyOne(Moved[i], &Bucket);
            }
            _mm_sfence();
        }, 64);

        MergeParallelDirtySlots();
    }

    bool FScenePrimitiveSet::StreamEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint32 Record, FDirtySlotBucket& Bucket)
    {
        const uint32 EntityIndex = Entity.GetIndex();
        const FStreamCacheEntry Entry = LookupStreamCacheEntry(Entity);
        if (Entry.Kind == EStreamKind::CPUPath)
        {
            return false;
        }
        const STransformComponent* Transform = Entry.Kind == EStreamKind::Stream ? Pools.Transform->TryGet(Entity) : nullptr;
        if (Transform == nullptr)
        {
            StreamTarget.Slots[Record] = kNoStreamSlot;
            return true;
        }

        StreamRecord(StreamTarget.Records[Record], MakeStreamRecord(Transform->GetWorldMatrixCached(), Entry.LocalCenter, Entry.Radius));
        StreamTarget.Slots[Record] = Entry.Slot;
        const uint32 Slot = Entry.Slot;

        MoveRecords[EntityIndex] = { StreamFrame, Record };
        Bucket.Streamed.push_back(Entity);
        const uint32 Block = Slot / kInstanceCullBlockSize;
        if (Bucket.StreamedBlocks.empty() || Bucket.StreamedBlocks.back() != Block)
        {
            Bucket.StreamedBlocks.push_back(Block);
        }
        return true;
    }

    FScenePrimitiveSet::FStreamCacheEntry FScenePrimitiveSet::BuildStreamCacheEntry(ECS::FEntity Entity) const
    {
        FStreamCacheEntry Entry;
        Entry.Generation = StreamCacheGeneration;
        Entry.Entity     = Entity.Value;

        const uint8 Mask = GetSourceMask(Entity) & kMovedSources;
        if (Mask == 0)
        {
            Entry.Kind = EStreamKind::Skip;
            return Entry;
        }

        // One instance slot of a plain mesh in place, since several sources, surfaces, bones or a foreground remap need the CPU path.
        Entry.Kind = EStreamKind::CPUPath;
        if ((Mask & (Mask - 1u)) != 0)
        {
            return Entry;
        }
        const uint32 Index = FindLinked(Entity, (EPrimitiveSource)Math::CountTrailingZeros64(Mask));
        if (Index == Constants::kIndexNoneU32)
        {
            return Entry;
        }
        const FScenePrimitive& Prim = Primitives[Index];
        if (Prim.SurfaceCount != 1u || Prim.Source == EPrimitiveSource::SkeletalMesh || IsRemappedForeground(Prim))
        {
            return Entry;
        }
        const uint32 Slot = Bindings[Prim.BindingBase].InstanceSlot;
        if (Slot >= (uint32)RetainedCullEntries.size())
        {
            return Entry;
        }

        Entry.Kind        = EStreamKind::Stream;
        Entry.Slot        = Slot;
        Entry.LocalCenter = Prim.LocalCenter;
        Entry.Radius      = Prim.LocalRadius * Math::Max(Prim.BoundsScale, 1.0f);
        return Entry;
    }

    FScenePrimitiveSet::FStreamCacheEntry FScenePrimitiveSet::LookupStreamCacheEntry(ECS::FEntity Entity)
    {
        const uint32 EntityIndex = Entity.GetIndex();
        FStreamCacheEntry* Cached = EntityIndex < (uint32)StreamCache.size() ? &StreamCache[EntityIndex] : nullptr;
        if (Cached != nullptr && Cached->Generation == StreamCacheGeneration && Cached->Entity == Entity.Value)
        {
            return *Cached;
        }

        const FStreamCacheEntry Entry = BuildStreamCacheEntry(Entity);
        if (Cached != nullptr)
        {
            *Cached = Entry;
        }
        return Entry;
    }

    void FScenePrimitiveSet::OpenStreamFrame()
    {
        if (MoveRecords.empty())
        {
            MoveRecords.resize((SIZE_T)ECS::FEntity::IndexMask + 1u);
        }
        // Zero is never a frame, so a wrapped counter clears the table rather than matching an old stamp.
        if (++StreamFrame == 0u)
        {
            Algo::Fill(MoveRecords, FMoveRecordStamp{});
            StreamFrame = 1u;
        }
        bStreamFrameOpen = true;
    }

    void FScenePrimitiveSet::ArmMoveStream(ECS::FRegistry& Registry, const FTransformStreamTarget& Target, uint32 MovedConsumer)
    {
        DisarmMoveStream(Registry);
        StreamTarget = Target;
        if (Target.Records == nullptr || Target.Slots == nullptr || Target.Capacity == 0 || !Jobs::IsInitialized())
        {
            return;
        }

        if (MoveLanes.size() < Jobs::GetNumThreadSlots())
        {
            MoveLanes.resize(Jobs::GetNumThreadSlots());
        }
        // Every streamable entity has a link, so the link table bounds the entity indices a mover can write for.
        if (StreamCache.size() < LinksByEntityIndex.size())
        {
            StreamCache.resize(LinksByEntityIndex.size());
        }
        OpenStreamFrame();
        MoveStreamNext.store(0u, std::memory_order_relaxed);

        MoveWriter.Write    = &FScenePrimitiveSet::WriteFlatMove;
        MoveWriter.Context  = this;
        MoveWriter.Consumer = MovedConsumer;
        bMoveStreamArmed = ECS::Utils::SetFlatMoveWriter(Registry, &MoveWriter);
    }

    void FScenePrimitiveSet::DisarmMoveStream(ECS::FRegistry& Registry)
    {
        if (!bMoveStreamArmed)
        {
            return;
        }
        ECS::Utils::ClearFlatMoveWriter(Registry, &MoveWriter);
        bMoveStreamArmed = false;
        for (FMoveStreamLane& Lane : MoveLanes)
        {
            Lane.Next = Lane.End = 0;
            Lane.Streamed.clear();
            Lane.Blocks.clear();
            Lane.Rewrites.clear();
        }
        StreamTarget = {};
    }

    bool FScenePrimitiveSet::WriteFlatMove(void* Context, ECS::FEntity Entity, const FTransform& World)
    {
        return static_cast<FScenePrimitiveSet*>(Context)->WriteMove(Entity, World);
    }

    bool FScenePrimitiveSet::WriteMove(ECS::FEntity Entity, const FTransform& World)
    {
        const uint32 LaneIndex   = Jobs::GetWorkerIndex();
        const uint32 EntityIndex = Entity.GetIndex();
        if (LaneIndex >= (uint32)MoveLanes.size() || EntityIndex >= (uint32)StreamCache.size())
        {
            return false;
        }
        FMoveStreamLane& Lane = MoveLanes[LaneIndex];

        FMoveRecordStamp& Stamp = MoveRecords[EntityIndex];
        if (Stamp.Frame == StreamFrame)
        {
            if ((Stamp.Record & kMoveRewriteQueued) == 0u)
            {
                Stamp.Record |= kMoveRewriteQueued;
                Lane.Rewrites.push_back(Entity);
            }
            return true;
        }

        const FStreamCacheEntry Entry = LookupStreamCacheEntry(Entity);
        if (Entry.Kind != EStreamKind::Stream)
        {
            return Entry.Kind == EStreamKind::Skip;
        }

        if (Lane.Next == Lane.End)
        {
            const uint32 Begin = MoveStreamNext.fetch_add(kMoveStreamChunk, std::memory_order_relaxed);
            if (Begin >= StreamTarget.Capacity)
            {
                return false;
            }
            Lane.Next = Begin;
            Lane.End  = Math::Min(Begin + kMoveStreamChunk, StreamTarget.Capacity);
        }
        // No fence, since the locked operation that ends every job flushes a worker's write-combining buffers.
        const uint32 Record = Lane.Next++;
        StreamRecord(StreamTarget.Records[Record], MakeStreamRecord(World.GetMatrix(), Entry.LocalCenter, Entry.Radius));
        StreamTarget.Slots[Record] = Entry.Slot;

        // Zero is no frame, so a table cleared by a wrapped counter never reads as continuing.
        Lane.Continuing += (Stamp.Frame != 0u && Stamp.Frame + 1u == StreamFrame) ? 1u : 0u;
        Stamp = { StreamFrame, Record };
        Lane.Streamed.push_back(Entity);
        const uint32 Block = Entry.Slot / kInstanceCullBlockSize;
        if (Lane.Blocks.empty() || Lane.Blocks.back() != Block)
        {
            Lane.Blocks.push_back(Block);
        }
        return true;
    }

    uint32 FScenePrimitiveSet::SealMoveStream(ECS::FRegistry& Registry)
    {
        if (!bMoveStreamArmed)
        {
            return 0u;
        }

        LUMINA_PROFILE_SECTION("Sync/Seal Move Stream");
        ECS::Utils::ClearFlatMoveWriter(Registry, &MoveWriter);
        bMoveStreamArmed = false;

        const uint32 Used = Math::Min(MoveStreamNext.load(std::memory_order_relaxed), StreamTarget.Capacity);
        bool bRewrites = false;
        for (FMoveStreamLane& Lane : MoveLanes)
        {
            ContinuingStreams += Lane.Continuing;
            Lane.Continuing = 0;
            // The rest of the lane's last chunk was claimed but never written.
            for (uint32 Record = Lane.Next; Record < Lane.End; ++Record)
            {
                StreamTarget.Slots[Record] = kNoStreamSlot;
            }
            Lane.Next = Lane.End = 0;

            StreamedThisFrame.Take(Lane.Streamed);
            StreamedBlocks.insert(StreamedBlocks.end(), Lane.Blocks.begin(), Lane.Blocks.end());
            Lane.Blocks.clear();
            bRewrites |= !Lane.Rewrites.empty();
        }

        if (bRewrites)
        {
            const FSyncPools Pools(Registry);
            for (FMoveStreamLane& Lane : MoveLanes)
            {
                for (ECS::FEntity Entity : Lane.Rewrites)
                {
                    FMoveRecordStamp& Stamp = MoveRecords[Entity.GetIndex()];
                    Stamp.Record &= ~kMoveRewriteQueued;
                    const STransformComponent* Transform = Pools.Transform->TryGet(Entity);
                    if (Transform != nullptr && Stamp.Record < Used)
                    {
                        const FStreamCacheEntry& Entry = StreamCache[Entity.GetIndex()];
                        StreamRecord(StreamTarget.Records[Stamp.Record], MakeStreamRecord(Transform->GetWorldMatrixCached(), Entry.LocalCenter, Entry.Radius));
                    }
                }
                Lane.Rewrites.clear();
            }
        }
        _mm_sfence();

        if (!bDiscardMoveStream)
        {
            return Used;
        }

        // Records past the frame's count never reach the GPU, so dropping them only needs their movers replayed.
        bDiscardMoveStream = false;
        bStreamListsExact  = false;
        MoveReplay.clear();
        for (uint32 List = 0; List < StreamedThisFrame.Used; ++List)
        {
            MoveReplay.insert(MoveReplay.end(), StreamedThisFrame.Lists[List].begin(), StreamedThisFrame.Lists[List].end());
        }
        for (ECS::FEntity Entity : MoveReplay)
        {
            MoveRecords[Entity.GetIndex()].Record = kNoStreamSlot;
        }
        StreamedThisFrame.Clear();
        StreamedBlocks.clear();
        return 0u;
    }

    void FScenePrimitiveSet::KillMoveRecord(ECS::FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        if (Index >= (uint32)MoveRecords.size() || MoveRecords[Index].Frame != StreamFrame)
        {
            return;
        }
        FMoveRecordStamp& Stamp = MoveRecords[Index];
        const uint32 Record = Stamp.Record & ~kMoveRewriteQueued;
        if (Record < StreamTarget.Capacity && StreamTarget.Slots != nullptr)
        {
            StreamTarget.Slots[Record] = kNoStreamSlot;
        }
        Stamp.Record = kNoStreamSlot;
    }

    void FScenePrimitiveSet::InvalidateStreamCache(ECS::FEntity Entity)
    {
        const uint32 EntityIndex = Entity.GetIndex();
        if (EntityIndex < (uint32)StreamCache.size())
        {
            StreamCache[EntityIndex].Generation = 0u;
        }
    }

    void FScenePrimitiveSet::InvalidateAllStreamCache()
    {
        // Zero marks an empty entry, so a wrapped generation clears the table instead.
        if (++StreamCacheGeneration == 0u)
        {
            std::fill(StreamCache.begin(), StreamCache.end(), FStreamCacheEntry{});
            StreamCacheGeneration = 1u;
        }
    }

    void FScenePrimitiveSet::StreamMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Stream");

        const uint32 Count = (uint32)Moved.size();
        ++StructureGeneration;
        SyncStats.SyncEntityCalls += Count;

        // Every streamable entity has a link, so the link table bounds the entity indices worth caching.
        if (StreamCache.size() < LinksByEntityIndex.size())
        {
            StreamCache.resize(LinksByEntityIndex.size());
        }
        const uint32 Stamp = BeginMovedClaims();
        uint32* Claims = MovedClaimByEntityIndex.data();

        EnsureParallelBuckets();
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Slots.clear();
            Bucket.Streamed.clear();
            Bucket.StreamedBlocks.clear();
        }

        // Record i belongs to Moved[i], so a duplicate mover streams twice harmlessly and only the CPU path claims, since a locked exchange drains write-combining.
        const auto StreamOne = [this, &Pools, Stamp, Claims](ECS::FEntity Entity, uint32 Record, FDirtySlotBucket& Bucket)
        {
            if (Entity != ECS::NullEntity && !HasCoalescedRecord(Entity))
            {
                if (StreamEntityTransform(Pools, Entity, Record, Bucket))
                {
                    return;
                }
                if (std::atomic_ref<uint32>(Claims[Entity.GetIndex()]).exchange(Stamp, std::memory_order_relaxed) != Stamp)
                {
                    ApplyEntityTransform(Pools, Entity, kMovedSources, &Bucket.Slots);
                }
            }
            StreamTarget.Slots[Record] = kNoStreamSlot;
        };

        const uint32 Base = StreamCount;
        Task::ParallelFor(Count, [&StreamOne, Moved, Base, this](const Task::FParallelRange& Range)
        {
            FDirtySlotBucket& Bucket = ParallelDirtySlots[Range.Thread];
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                StreamOne(Moved[i], Base + i, Bucket);
            }
            _mm_sfence();
        }, 64);

        MergeParallelDirtySlots();

        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            StreamedThisFrame.Take(Bucket.Streamed);
            StreamedBlocks.insert(StreamedBlocks.end(), Bucket.StreamedBlocks.begin(), Bucket.StreamedBlocks.end());
        }

        StreamCount = Base + Count;
    }

    void FScenePrimitiveSet::CatchUpStoppedStreams(const FSyncPools& Pools, uint32 MovedStamp, bool bNoneStopped)
    {
        StoppedScratch.clear();
        if (!StreamedLastFrame.IsEmpty() && !bNoneStopped)
        {
            LUMINA_PROFILE_SECTION("Sync/Apply/CatchUp");

            EnsureParallelBuckets();
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                Bucket.Stopped.clear();
            }

            // Streamed again, or moved through the CPU path this frame, means its copies are already handled.
            const FMoveRecordStamp* Streamed = MoveRecords.data();
            const uint32* Claims   = MovedClaimByEntityIndex.data();
            const uint32  Frame    = StreamFrame;
            Task::ParallelForOrSerial(StreamedLastFrame.Used, 1, [&](const Task::FParallelRange& Range)
            {
                TVector<ECS::FEntity>& Stopped = ParallelDirtySlots[Range.Thread < (uint32)ParallelDirtySlots.size() ? Range.Thread : 0u].Stopped;
                for (uint32 List = Range.Start; List < Range.End; ++List)
                {
                    for (const ECS::FEntity Entity : StreamedLastFrame.Lists[List])
                    {
                        const uint32 Index = Entity.GetIndex();
                        if (Streamed[Index].Frame != Frame && Claims[Index] != MovedStamp)
                        {
                            Stopped.push_back(Entity);
                        }
                    }
                }
            });
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                StoppedScratch.insert(StoppedScratch.end(), Bucket.Stopped.begin(), Bucket.Stopped.end());
            }
        }

        std::swap(StreamedLastFrame, StreamedThisFrame);
        StreamedThisFrame.Clear();

        if (!StoppedScratch.empty())
        {
            ApplyMovedLegacy(Pools, TSpan<const ECS::FEntity>(StoppedScratch.data(), StoppedScratch.size()));
        }
    }

    void FScenePrimitiveSet::MergeParallelDirtySlots()
    {
        SIZE_T Total = 0;
        for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Total += Bucket.Slots.size();
        }

        if (Total == 0 || bFullInstanceUpload)
        {
            return;
        }

        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && ShouldResendAllInstances(DirtyInstanceSlots.size() + Total, SlotCount))
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            return;
        }

        ReserveGeometric(DirtyInstanceSlots, DirtyInstanceSlots.size() + Total);
        for (const FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            DirtyInstanceSlots.insert(DirtyInstanceSlots.end(), Bucket.Slots.begin(), Bucket.Slots.end());
        }
    }

    void FScenePrimitiveSet::ApplyTransformRecords(const FSyncPools& Pools)
    {
        const uint32 Count = (uint32)TransformRecords.size();
        if (Count == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Sync/Apply/Transforms");

        SyncStats.SyncEntityCalls += Count;

        ++StructureGeneration;

        constexpr uint32 kParallelThreshold = 1024;

        const uint32 NumSlots = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
        if (Count < kParallelThreshold || NumSlots <= 1)
        {
            for (uint32 RecordIndex : TransformRecords)
            {
                // A nullptr sink goes straight onto the shared list, thresholds and all.
                ApplyTransformRecord(Pools, RecordIndex, nullptr);
            }
            _mm_sfence();
            return;
        }

        // Counted in one go rather than per record; see RefreshInstances.
        SyncStats.RefreshInstanceCalls += Count;

        if ((uint32)ParallelDirtySlots.size() < NumSlots)
        {
            ParallelDirtySlots.resize(NumSlots);
        }
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Slots.clear();   // keeps capacity, so this settles into zero allocations per frame
        }

        Task::ParallelFor(Count, [this, &Pools](const Task::FParallelRange& Range)
        {
            TVector<uint32>& Bucket = ParallelDirtySlots[Range.Thread].Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                ApplyTransformRecord(Pools, TransformRecords[i], &Bucket);
            }
            _mm_sfence();
        }, 64);

        MergeParallelDirtySlots();
    }

    void FScenePrimitiveSet::Sync(CWorld& World, TSpan<const ECS::FEntity> Moved)
    {
        ECS::FRegistry&     Registry = World.GetEntityRegistry();
        FRenderDirtyTracker& Tracker  = FRenderDirtyTracker::Ensure(Registry);

        SyncStats = FSyncStats{};
        StreamedBlocks.clear();
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

        ApplyFoliageVisibilityEdits(Registry);

        if (!Tracker.HasPending() && !bResolveTableChanged && Moved.empty() && StreamedLastFrame.IsEmpty() && StreamedThisFrame.IsEmpty() && MoveReplay.empty())
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        if (Tracker.ConsumeFullRescan())
        {
            // Drain and discard, since the rescan supersedes every queued entry.
            DrainScratch.clear();
            Tracker.Drain(DrainScratch);
            DrainScratch.clear();
            bResolveTableChanged = false;
            // Rebuilt from the ECS, so nothing streamed is behind any more.
            InvalidateAllStreamCache();
            StreamCount = 0;
            ContinuingStreams = 0;
            bStreamListsExact = false;
            MoveReplay.clear();
            StreamedLastFrame.Clear();
            StreamedThisFrame.Clear();
            StreamedBlocks.clear();
            FullRescan(Registry);
            PublishSyncStats();
            return;
        }

        {
            LUMINA_PROFILE_SECTION("Sync/Drain");
            DrainScratch.clear();
            Tracker.Drain(DrainScratch);
        }
        SyncStats.DrainEntries = (uint32)(DrainScratch.size() + Moved.size());

        CoalesceDrain(Moved);
        ReserveForDrain();

        // Resolved once for the whole drain, not once per entity. See FSyncPools.
        const FSyncPools Pools(Registry);

        {
            LUMINA_PROFILE_SECTION("Sync/Apply");

            PartitionDrain();

            ApplyStructuralRecords(Registry, Pools);

            ApplyTransformRecords(Pools);

            ApplyMovedTransforms(Pools, Moved);
        }

        if (bResolveTableChanged)
        {
            LUMINA_PROFILE_SECTION("Resolve Mesh Cache");
            bResolveTableChanged = false;

            FMeshResolveCache& Cache = FMeshResolveCache::Get();
            RetryScratch.clear();

            GenSnapshot.assign(Cache.NumEntries(), Constants::kIndexNoneU32);

            // Only handles rebuilt since the last sweep are walked, through their primitive lists.
            const uint32 NumHandles = Math::Min(Cache.NumEntries(), (uint32)HandleListHead.size());
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
                        RetryScratch.push_back(Keys[i]);
                    }
                }
            }
            LastSweptTableGeneration = Cache.GetTableGeneration();

            SweepFoliageResolves(Cache);

            Algo::Sort(RetryScratch);
            {
                size_t Unique = 0;
                for (size_t i = 0; i < RetryScratch.size(); ++i)
                {
                    if (i == 0 || RetryScratch[i] != RetryScratch[i - 1])
                    {
                        RetryScratch[Unique++] = RetryScratch[i];
                    }
                }
                RetryScratch.resize(Unique);
            }

            for (uint64 Target : RetryScratch)
            {
                const ECS::FEntity     Entity = (ECS::FEntity)(uint32)(Target & 0xFFFFFFFFull);
                const EPrimitiveSource Source = (EPrimitiveSource)((Target >> kKeySourceShift) & 0xFFull);
                if (Source == EPrimitiveSource::Foliage)
                {
                    SyncFoliage(Registry, Pools, Entity, EPrimitiveDirty::Data);
                }
                else
                {
                    SyncEntity(Registry, Pools, Entity, Source, EPrimitiveDirty::Data);
                }
            }
            FlushFoliageReleases();
        }

        PublishSyncStats();
    }

    void FScenePrimitiveSet::PublishSyncStats() const
    {
        LUMINA_PROFILE_VALUE("Sync/DrainEntries",     (int64)SyncStats.DrainEntries);
        LUMINA_PROFILE_VALUE("Sync/Coalesced",        (int64)SyncStats.CoalescedEntities);
        LUMINA_PROFILE_VALUE("Sync/TransformRecords", (int64)SyncStats.TransformRecords);
        LUMINA_PROFILE_VALUE("Sync/StructuralRecords",(int64)SyncStats.StructuralRecords);
        LUMINA_PROFILE_VALUE("Sync/SyncEntity",       (int64)SyncStats.SyncEntityCalls);
        LUMINA_PROFILE_VALUE("Sync/SyncFoliage",      (int64)SyncStats.SyncFoliageCalls);
        LUMINA_PROFILE_VALUE("Sync/BindSurfaces",     (int64)SyncStats.BindCalls);
        LUMINA_PROFILE_VALUE("Sync/BindsSkipped",     (int64)SyncStats.BindsSkipped);
        LUMINA_PROFILE_VALUE("Sync/RefreshInstances", (int64)SyncStats.RefreshInstanceCalls);
        LUMINA_PROFILE_VALUE("Sync/DirtySlots",       (int64)DirtyInstanceSlots.size());
        LUMINA_PROFILE_VALUE("Sync/Primitives",       (int64)Primitives.size());

        int64 FoliageInstances = 0;
        for (const auto& [Entity, State] : FoliageByEntity)
        {
            FoliageInstances += (int64)State.Instances.size();
        }
        int64 GrassSlots = 0;
        for (const auto& [Mesh, Binding] : GrassSpecies)
        {
            GrassSlots += Binding.Capacity;
        }
        LUMINA_PROFILE_VALUE("Sync/FoliageInstances", FoliageInstances);
        LUMINA_PROFILE_VALUE("Sync/GrassSlots",       GrassSlots);
    }

    // Every active slot contributes its surface's LARGEST LOD, a true bound on one view's appends.
    void FScenePrimitiveSet::Reset(ECS::FRegistry* Registry)
    {
        if (Registry != nullptr)
        {
            DisarmMoveStream(*Registry);
        }
        MoveReplay.clear();
        ContinuingStreams = 0;
        bStreamListsExact = false;
        InvalidateAllStreamCache();
        StreamedLastFrame.Clear();
        StreamedThisFrame.Clear();
        StreamedBlocks.clear();
        StreamCount = 0;
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        ResolveKeys.clear();
        HandleListHead.clear();
        NextByHandle.clear();
        PrevByHandle.clear();
        LastSweptTableGeneration = 0;
        Bindings.clear();
        // Every slice died with the primitives, so the arena restarts rather than leaking live bases.
        BoneSliceFreeLists.clear();
        BoneSliceExtent = 0;
        BoneSliceSweepCursor = 0;
        ++SkeletalSetGeneration;
        DeadBindings = 0;
        BindingFreeRanges.clear();
        FreeRangeBindings = 0;
        PendingFoliageReleases.clear();
        SkinnedIndexFreeList.clear();
        SkinnedIndexExtent = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        BonelessSkinnedCount = 0;
        CustomDepthCount = 0;
        RetainedCullEntries.clear();
        RetainedTransforms.clear();
        RetainedStatic.clear();
        ResetMeshletVisibility();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        DirtyStaticSlots.clear();
        bFullInstanceUpload = true;
        bFullStaticUpload   = true;
        SurfaceDescs.clear();
        SurfaceDescByHash.clear();
        bSurfaceDescsDirty = true;
        MaxSurfaceDescMeshlets = 0;
        FoliageByEntity.clear();
        Batches.Reset();
        // Always paired with Batches.Reset(), which renumbers every batch index the memo cached.
        BindingMemoByHandle.clear();
        bResolveTableChanged = false;
        ++StructureGeneration;

        if (Registry != nullptr)
        {
            FRenderDirtyTracker::Ensure(*Registry).RequestFullRescan();
        }
    }
}
