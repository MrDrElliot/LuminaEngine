#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Containers/ConcurrentQueue.h"
#include "Memory/Memcpy.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{

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
        if (Surface.DeferredShader == nullptr || Surface.MaterialIdx == (uint16)-1
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
        Batch.MomentPixelShader               = Surface.MomentPixelShader;
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

    void FSceneBatchRegistry::ReleaseBatchRef(uint32 BatchIndex, bool bSkinned)
    {
        if (BatchIndex >= (uint32)Batches.size())
        {
            return;
        }

        FBatch& Batch = Batches[BatchIndex];
        if (Batch.RefCount > 0)
        {
            --Batch.RefCount;
        }

        uint32& Kind = bSkinned ? Batch.SkinnedRefCount : Batch.StaticRefCount;
        if (Kind > 0)
        {
            --Kind;
        }

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
            return ~0u;
        }

        const FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        return Link.Entity == (Entity).Value ? Link.Index[(uint32)Source] : ~0u;
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
            Mask |= (Link.Index[s] != ~0u) ? (uint8)(1u << s) : (uint8)0u;
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

        Link.Index[(uint32)Source] = ~0u;

        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            if (Link.Index[s] != ~0u)
            {
                return;
            }
        }
        Link.Entity = ~0u;
    }

    uint32 FScenePrimitiveSet::AddPrimitive(uint64 Key)
    {
        const uint32 Index = (uint32)Primitives.size();
        Primitives.emplace_back();
        Bounds.emplace_back(FVector4(0.0f));
        CullData.emplace_back();
        Keys.push_back(Key);
        ResolveKeys.push_back(PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0));
        NextByHandle.push_back(~0u);
        PrevByHandle.push_back(~0u);

        const ECS::FEntity     Entity = (ECS::FEntity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        SetLink(Entity, Source, Index);
        if (Source == EPrimitiveSource::SkeletalMesh)
        {
            ++SkinnedCount;
            ++BonelessSkinnedCount;
            ++SkeletalSetGeneration;
        }

        ++StructureGeneration;
        return Index;
    }

    void FScenePrimitiveSet::RemovePrimitive(uint64 Key)
    {
        const uint32 Index = FindPrimitive(Key);
        if (Index == ~0u)
        {
            return;
        }

        const uint32 Last = (uint32)Primitives.size() - 1u;

        if (Primitives[Index].Source == EPrimitiveSource::SkeletalMesh && Primitives[Index].BoneCount == 0u
            && BonelessSkinnedCount > 0u)
        {
            --BonelessSkinnedCount;
        }

        ReleaseBindings(Index);
        ReleaseBoneSlice(Primitives[Index]);
        Primitives[Index].BoneCount = 0u;
        UnlinkHandle(Index, Primitives[Index].ResolveHandle);

        // The tail primitive moves into this slot, so the skeletal index list shifts only when either is skeletal.
        if (Primitives[Index].Source == EPrimitiveSource::SkeletalMesh
            || Primitives[Last].Source == EPrimitiveSource::SkeletalMesh)
        {
            ++SkeletalSetGeneration;
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
            if (MovedPrev != ~0u)
            {
                NextByHandle[MovedPrev] = Index;
            }
            else if (Primitives[Index].ResolveHandle < (uint32)HandleListHead.size()
                     && HandleListHead[Primitives[Index].ResolveHandle] == Last)
            {
                HandleListHead[Primitives[Index].ResolveHandle] = Index;
            }
            if (MovedNext != ~0u)
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
            for (FFoliageInstanceRef& Ref : It->second.Instances)
            {
                ReleaseFoliageInstance(Ref);
            }
            FoliageByEntity.erase(It);
            ++StructureGeneration;
            return;
        }

        RemovePrimitive(MakeKey(Entity, Source));
    }

    void FScenePrimitiveSet::RebuildWorldBounds(uint32 Index)
    {
        const FScenePrimitive& Prim = Primitives[Index];
        const FMatrix4& M = Prim.Transform;

        const FVector3& C = Prim.LocalCenter;
        const FVector3  Center = FVector3(M[0]) * C.x
                               + FVector3(M[1]) * C.y
                               + FVector3(M[2]) * C.z
                               + FVector3(M[3]);

        const float ScaleSq = Math::Max(Math::Max(
            Math::Dot(FVector3(M[0]), FVector3(M[0])),
            Math::Dot(FVector3(M[1]), FVector3(M[1]))),
            Math::Dot(FVector3(M[2]), FVector3(M[2])));

        const float CullScale = Math::Max(Prim.BoundsScale, 1.0f);
        const float Radius    = Prim.LocalRadius * Math::Sqrt(ScaleSq) * CullScale;

        Bounds[Index] = FVector4(Center, Radius);
    }

    void FScenePrimitiveSet::CompactBindings()
    {
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
        const SIZE_T OldCapacity = RetainedCullEntries.capacity();

        RetainedCullEntries.emplace_back();
        RetainedTransforms.emplace_back();
        RetainedStatic.emplace_back();

        if (RetainedCullEntries.capacity() != OldCapacity)
        {
            bFullInstanceUpload = true;
        }
        return Slot;
    }

    void FScenePrimitiveSet::FreeInstanceSlot(uint32 Slot)
    {
        if (Slot >= (uint32)RetainedCullEntries.size())
        {
            return;
        }

        RetainedCullEntries[Slot].DrawIDAndFlags &= ~((uint32)EInstanceFlags::Active << 16);
        MarkInstanceDirty(Slot);
        InstanceFreeSlots.push_back(Slot);
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
            HandleListHead.resize(Handle + 1u, ~0u);
        }

        const uint32 Head = HandleListHead[Handle];
        NextByHandle[Index] = Head;
        PrevByHandle[Index] = ~0u;
        if (Head != ~0u)
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
        if (Prev != ~0u)
        {
            NextByHandle[Prev] = Next;
        }
        else if (HandleListHead[Handle] == Index)
        {
            HandleListHead[Handle] = Next;
        }
        if (Next != ~0u)
        {
            PrevByHandle[Next] = Prev;
        }
        NextByHandle[Index] = ~0u;
        PrevByHandle[Index] = ~0u;
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
        if (SlotCount > 1024 && DirtyInstanceSlots.size() * 4 >= SlotCount)
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            DirtyStaticSlots.clear();
            return;
        }

        DirtyInstanceSlots.push_back(Slot);
    }

    void FScenePrimitiveSet::MarkStaticDirty(uint32 Slot)
    {
        if (bFullInstanceUpload)
        {
            return;
        }

        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && DirtyStaticSlots.size() * 4 >= SlotCount)
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            DirtyStaticSlots.clear();
            return;
        }

        DirtyStaticSlots.push_back(Slot);
    }

    uint32 FScenePrimitiveSet::InternSurfaceDesc(const FResolvedSurface& Surface)
    {
        FSurfaceDescGPU Desc = {};
        Desc.NumLODs = Surface.NumLODs;
        for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
        {
            Desc.LODMeshletOffset[i]     = Surface.LODMeshletOffset[i];
            Desc.LODMeshletCount[i]      = Surface.LODMeshletCount[i];
            Desc.LODScreenThresholdSq[i] = Surface.LODScreenThresholdSq[i];
            
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
            Hash::HashCombine(Hash, Desc.LODScreenThresholdSq[i]);
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
        const FVector4&           Sphere = Bounds[Index];

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Prim.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.size())
            {
                continue;
            }

            EInstanceFlags Flags = Prim.BaseFlags | Binding.MaterialFlags;
            if (Prim.bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
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
            NewCull.ForcedLODIndex   = Prim.ForcedLODIndex;

            const FTransform3x4 NewTransform = PackTransform3x4(Prim.Transform);

            FInstanceStatic NewStatic = {};
            NewStatic.MeshletHeaderSlot = Prim.MeshletHeaderSlot;
            NewStatic.CustomData        = Prim.CustomData;
            NewStatic.MaterialIndex     = Binding.MaterialIndex;
            NewStatic.EntityID          = Prim.EntityID;

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
        if (DirtySink == nullptr)
        {
            ++SyncStats.RefreshInstanceCalls;
        }

        const FScenePrimitive& Prim   = Primitives[Index];
        const FVector4&        Sphere = Bounds[Index];

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const uint32 Slot = Bindings[Prim.BindingBase + s].InstanceSlot;
            if (Slot >= (uint32)RetainedCullEntries.size())
            {
                continue;
            }

            RetainedCullEntries[Slot].SphereBounds = Sphere;
            RetainedTransforms[Slot]               = PackTransform3x4(Prim.Transform);

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

    void FScenePrimitiveSet::ReleaseBindingSpan(uint32 Base, uint32 Count)
    {
        for (uint32 s = 0; s < Count; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Base + s];
            Batches.ReleaseBatchRef(Binding.BatchIndex, Binding.bSkinned);
            FreeInstanceSlot(Binding.InstanceSlot);
        }
        DeadBindings += Count;
    }

    uint32 FScenePrimitiveSet::AppendBindingSpan(const FBindingMemo& Memo, bool bSkinned)
    {
        const uint32 Base = (uint32)Bindings.size();
        for (const FSurfaceBinding& Proto : Memo.Protos)
        {
            FSurfaceBinding& Binding = Bindings.emplace_back(Proto);
            Binding.InstanceSlot = AllocateInstanceSlot();
            Binding.bSkinned     = bSkinned;

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
                Proto.InstanceSlot          = ~0u;
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
        if (Surface.DeferredShader == nullptr || Surface.MaterialIdx == (uint16)-1
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
            AppendBindingSpan(*Memo, bSkinned);
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
            }
        }

        if (DeadBindings > 1024 && DeadBindings * 4 > (uint32)Bindings.size())
        {
            CompactBindings();
        }
    }

    struct FScenePrimitiveSet::FSyncPools
    {
        ECS::TComponentStorage<STransformComponent>          Transform    = {};
        ECS::TComponentStorage<FRenderTransform>       RenderXform  = {};
        ECS::TComponentStorage<SStaticMeshComponent>   StaticMesh   = {};
        ECS::TComponentStorage<SSkeletalMeshComponent> SkeletalMesh = {};
        ECS::TComponentStorage<SDynamicMeshComponent>        DynamicMesh  = {};
        ECS::TComponentStorage<SFoliageComponent>            Foliage      = {};
        const ECS::FSparseSet*                             Disabled     = nullptr;
        const ECS::FSparseSet*                             Sources[kLinkedSources] = {};

        FMeshResolveCache*                                  ResolveCache = &FMeshResolveCache::Get();

        explicit FSyncPools(ECS::FRegistry& Registry)
            : Transform(Registry.GetStorage<STransformComponent>())
            , RenderXform(Registry.GetStorage<FRenderTransform>())
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

    static FORCEINLINE FMatrix4 ReadRenderMatrix(ECS::TComponentStorage<FRenderTransform> RenderStorage,
                                                 ECS::FEntity Entity, const STransformComponent& Transform)
    {
        return RenderStorage->Contains(Entity) ? RenderStorage->Get(Entity).Matrix
                                               : Transform.GetWorldMatrixCached();
    }

    namespace
    {
        template <typename TComponent>
        void ReadCommonMeshState(FScenePrimitive& Prim, FPrimitiveCullData& Cull, const TComponent& Component)
        {
            Prim.CustomData      = Component.CustomPrimitiveData.Data.Packed;
            Prim.BoundsScale     = Component.BoundsScale;
            Prim.ForcedLODIndex  = Component.ForcedLODIndex;
            Prim.bCastShadow     = Component.bCastShadow;

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

                if (Prim.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE || Base->CachedMeshKey != LiveMesh)
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
        RefreshInstances(Index);
        ++StructureGeneration;

        return bResolved;
    }

    void FScenePrimitiveSet::SyncEntity(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity,
                                        EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        ++SyncStats.SyncEntityCalls;

        const uint64 Key = MakeKey(Entity, Source);
        uint32 Index = FindLinked(Entity, Source);
        
        if (Index == ~0u && Flags == EPrimitiveDirty::Transform)
        {
            return;
        }

        if (Index != ~0u && Flags == EPrimitiveDirty::Transform)
        {
            if (!Pools.Transform->Contains(Entity))
            {
                return;
            }

            Primitives[Index].Transform = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->Get(Entity));
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
            if (Index != ~0u)
            {
                RemovePrimitive(Key);
            }
            return;
        }

        const bool bNew = (Index == ~0u);
        if (bNew)
        {
            Index = AddPrimitive(Key);
            Primitives[Index].Entity = Entity;
            Primitives[Index].Source = Source;
            Flags |= EPrimitiveDirty::Data | EPrimitiveDirty::Transform;
        }

        if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Transform))
        {
            Primitives[Index].Transform = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->Get(Entity));
        }

        RefreshPrimitiveData(Pools, Index);
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
                                                    const FFoliageBakedInstance& Instance, uint32 EntityID)
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

            EInstanceFlags Flags = Type.BaseFlags | Binding.MaterialFlags;
            if (Type.bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
            }
            if (Type.Surfaces != nullptr)
            {
                Flags |= EInstanceFlags::Active;
            }
            if (Type.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot)
            {
                Flags |= EInstanceFlags::HasGeometry;
            }

            FInstanceCullEntry NewCull = {};
            // The bake already produced the world sphere; there is no local sphere to transform.
            NewCull.SphereBounds     = Instance.SphereBounds;
            NewCull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
            NewCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
            NewCull.MaxDrawDistance  = Type.MaxDrawDistance;
            NewCull.ForcedLODIndex   = -1;

            const FTransform3x4 NewTransform = PackTransform3x4(Instance.Transform);

            FInstanceStatic NewStatic = {};
            NewStatic.MeshletHeaderSlot = Type.MeshletHeaderSlot;
            NewStatic.MaterialIndex     = Binding.MaterialIndex;
            NewStatic.EntityID          = EntityID;

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

        const bool bBakeChanged = State.SyncedBakeSerial != Foliage->BakeSerial
                               || NewCount != (uint32)State.Instances.size();
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

        for (uint32 i = 0; i < NewCount; ++i)
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
                    Ref.BindingBase  = AppendBindingSpan(*Memo, bSkinned);
                    Ref.SurfaceCount = (uint16)WantCount;
                }
                Ref.TypeRow = TypeRow;
            }
            else
            {
                ++SyncStats.BindsSkipped;
            }

            RefreshFoliageInstance(Ref, Type, Instance, EntityID);
        }

        if (DeadBindings > 1024 && DeadBindings * 4 > (uint32)Bindings.size())
        {
            CompactBindings();
        }

        ++StructureGeneration;
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
                if (Generation == ~0u)
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
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        BonelessSkinnedCount = 0;
        RetainedCullEntries.clear();
        RetainedTransforms.clear();
        RetainedStatic.clear();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        DirtyStaticSlots.clear();
        bFullInstanceUpload = true;
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
    void FScenePrimitiveSet::CoalesceDrain()
    {
        LUMINA_PROFILE_SECTION("Sync/Coalesce");

        // Stamping beats clearing, since the table is sized by the entity index space.
        ++CoalesceStamp;
        CoalescedScratch.clear();
        CoalescedScratch.reserve(DrainScratch.size());

        for (const FRenderDirtyTracker::FEntry& Entry : DrainScratch)
        {
            const uint32 Slot = (uint32)(Entry.Entity).GetIndex();
            if (Slot >= (uint32)CoalesceByEntityIndex.size())
            {
                CoalesceByEntityIndex.resize(Slot + 1u);
            }

            FCoalesceSlot& Mapped = CoalesceByEntityIndex[Slot];

            uint32 RecordIndex = ~0u;
            if (Mapped.Stamp == CoalesceStamp
                && Mapped.Index < (uint32)CoalescedScratch.size()
                && CoalescedScratch[Mapped.Index].Entity == (Entry.Entity).Value)
            {
                RecordIndex = Mapped.Index;
            }

            if (RecordIndex == ~0u)
            {
                RecordIndex = (uint32)CoalescedScratch.size();
                CoalescedScratch.emplace_back().Entity = (Entry.Entity).Value;
                Mapped.Stamp = CoalesceStamp;
                Mapped.Index = RecordIndex;
            }

            FCoalescedEntity& Record = CoalescedScratch[RecordIndex];

            if (Entry.Source == EPrimitiveSource::AnySource)
            {
                Record.Flags[(uint32)EPrimitiveSource::StaticMesh]   |= Entry.Flags;
                Record.Flags[(uint32)EPrimitiveSource::DynamicMesh]  |= Entry.Flags;
                Record.Flags[(uint32)EPrimitiveSource::SkeletalMesh] |= Entry.Flags;

                if (Entry.Flags != EPrimitiveDirty::Transform)
                {
                    Record.Flags[(uint32)EPrimitiveSource::Foliage] |= Entry.Flags;
                }
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

        SyncStats.CoalescedEntities = (uint32)CoalescedScratch.size();
    }

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
                    && FindLinked(Entity, (EPrimitiveSource)s) == ~0u)
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
        const SIZE_T OldSlotCapacity   = RetainedCullEntries.capacity();
        ReserveGeometric(RetainedCullEntries, InstanceTarget);
        ReserveGeometric(RetainedTransforms, InstanceTarget);
        ReserveGeometric(RetainedStatic, InstanceTarget);

        if (RetainedCullEntries.capacity() != OldSlotCapacity)
        {
            bFullInstanceUpload = true;
        }
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

        if (DeadBindings > 1024 && DeadBindings * 4 > (uint32)Bindings.size())
        {
            CompactBindings();
        }
    }

    void FScenePrimitiveSet::ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex,
                                                  TVector<uint32>* OutDirty)
    {
        const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];
        const ECS::FEntity      Entity = (ECS::FEntity)Record.Entity;

        const uint8 Mask = GetSourceMask(Entity);
        if (Mask == 0)
        {
            return;
        }

        if (!Pools.Transform->Contains(Entity))
        {
            return;
        }

        const FMatrix4 Matrix = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->Get(Entity));

        for (uint32 s = 0; s < kLinkedSources; ++s)
        {
            if ((Mask & (1u << s)) == 0 || Record.Flags[s] == EPrimitiveDirty::None)
            {
                continue;
            }

            const uint32 Index = FindLinked(Entity, (EPrimitiveSource)s);
            if (Index == ~0u)
            {
                continue;
            }

            Primitives[Index].Transform = Matrix;
            RebuildWorldBounds(Index);
            RefreshInstanceTransform(Index, OutDirty);
        }
    }

    void FScenePrimitiveSet::MergeParallelDirtySlots()
    {
        SIZE_T Total = 0;
        for (const TVector<uint32>& Bucket : ParallelDirtySlots)
        {
            Total += Bucket.size();
        }

        if (Total == 0 || bFullInstanceUpload)
        {
            return;
        }

        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && (DirtyInstanceSlots.size() + Total) * 4 >= SlotCount)
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            DirtyStaticSlots.clear();
            return;
        }

        ReserveGeometric(DirtyInstanceSlots, DirtyInstanceSlots.size() + Total);
        for (const TVector<uint32>& Bucket : ParallelDirtySlots)
        {
            DirtyInstanceSlots.insert(DirtyInstanceSlots.end(), Bucket.begin(), Bucket.end());
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
            return;
        }

        // Counted in one go rather than per record; see RefreshInstances.
        SyncStats.RefreshInstanceCalls += Count;

        if ((uint32)ParallelDirtySlots.size() < NumSlots)
        {
            ParallelDirtySlots.resize(NumSlots);
        }
        for (TVector<uint32>& Bucket : ParallelDirtySlots)
        {
            Bucket.clear();   // keeps capacity, so this settles into zero allocations per frame
        }

        Task::ParallelFor(Count, [this, &Pools](const Task::FParallelRange& Range)
        {
            TVector<uint32>& Bucket = ParallelDirtySlots[Range.Thread];
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                ApplyTransformRecord(Pools, TransformRecords[i], &Bucket);
            }
        }, 64);

        MergeParallelDirtySlots();
    }

    void FScenePrimitiveSet::Sync(CWorld& World)
    {
        ECS::FRegistry&     Registry = ECS::GetWorldRegistry(World);
        FRenderDirtyTracker& Tracker  = FRenderDirtyTracker::Ensure(Registry);

        SyncStats = FSyncStats{};

        {
            LUMINA_PROFILE_SECTION("Sync/Poll");
            PollUnhookedSources(Registry, Tracker);
        }

        if (!Tracker.HasPending() && !bResolveTableChanged)
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
            FullRescan(Registry);
            PublishSyncStats();
            return;
        }

        {
            LUMINA_PROFILE_SECTION("Sync/Drain");
            DrainScratch.clear();
            Tracker.Drain(DrainScratch);
        }
        SyncStats.DrainEntries = (uint32)DrainScratch.size();

        CoalesceDrain();
        ReserveForDrain();

        // Resolved once for the whole drain, not once per entity. See FSyncPools.
        const FSyncPools Pools(Registry);

        {
            LUMINA_PROFILE_SECTION("Sync/Apply");

            PartitionDrain();

            ApplyStructuralRecords(Registry, Pools);

            ApplyTransformRecords(Pools);
        }

        if (bResolveTableChanged)
        {
            LUMINA_PROFILE_SECTION("Resolve Mesh Cache");
            bResolveTableChanged = false;

            FMeshResolveCache& Cache = FMeshResolveCache::Get();
            RetryScratch.clear();

            GenSnapshot.assign(Cache.NumEntries(), ~0u);

            // Only handles rebuilt since the last sweep are walked, through their primitive lists.
            const uint32 NumHandles = Math::Min(Cache.NumEntries(), (uint32)HandleListHead.size());
            for (uint32 Handle = 0; Handle < NumHandles; ++Handle)
            {
                if (Cache.GetResolvedAtGeneration(Handle) <= LastSweptTableGeneration)
                {
                    continue;
                }

                const uint32 Generation = Cache.GetEntry(Handle).Generation;
                for (uint32 i = HandleListHead[Handle]; i != ~0u; i = NextByHandle[i])
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
    }

    // Every active slot contributes its surface's LARGEST LOD, a true bound on one view's appends.
    void FScenePrimitiveSet::Reset(ECS::FRegistry* Registry)
    {
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
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        BonelessSkinnedCount = 0;
        RetainedCullEntries.clear();
        RetainedTransforms.clear();
        RetainedStatic.clear();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        DirtyStaticSlots.clear();
        bFullInstanceUpload = true;
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
