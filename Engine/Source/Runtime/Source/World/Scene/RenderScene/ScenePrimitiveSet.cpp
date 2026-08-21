#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Memory/MemoryConcurrentQueue.h"
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

    static FORCEINLINE uint64 PackDirty(entt::entity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        return (uint64)entt::to_integral(Entity) | ((uint64)Source << 32) | ((uint64)Flags  << 40);
    }

    struct FRenderDirtyTracker::FImpl
    {
        using FQueue = moodycamel::ConcurrentQueue<uint64, Memory::FTrackedConcurrentQueueTraits>;
        FQueue Queue;
    };

    FRenderDirtyTracker::FRenderDirtyTracker()
        : Impl(Memory::New<FImpl>())
    {}

    FRenderDirtyTracker::~FRenderDirtyTracker()
    {
        Memory::Delete(Impl);
    }

    FRenderDirtyTracker* FRenderDirtyTracker::Find(FEntityRegistry& Registry)
    {
        TUniquePtr<FRenderDirtyTracker>* Holder = Registry.ctx().find<TUniquePtr<FRenderDirtyTracker>>();
        return Holder ? Holder->get() : nullptr;
    }

    FRenderDirtyTracker& FRenderDirtyTracker::Ensure(FEntityRegistry& Registry)
    {
        if (TUniquePtr<FRenderDirtyTracker>* Holder = Registry.ctx().find<TUniquePtr<FRenderDirtyTracker>>())
        {
            return *Holder->get();
        }
        return *Registry.ctx().emplace<TUniquePtr<FRenderDirtyTracker>>(MakeUnique<FRenderDirtyTracker>());
    }

    void FRenderDirtyTracker::Mark(entt::entity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        if (Entity == entt::null || Flags == EPrimitiveDirty::None)
        {
            return;
        }
        Impl->Queue.enqueue(PackDirty(Entity, Source, Flags));
        bAnyDirty.store(true, std::memory_order_release);
    }

    void FRenderDirtyTracker::MarkAllSources(entt::entity Entity, EPrimitiveDirty Flags)
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
        while ((Count = Impl->Queue.try_dequeue_bulk(Batch, 256)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                const uint64 Packed = Batch[i];
                FEntry& Entry  = Out.emplace_back();
                Entry.Entity = (entt::entity)(uint32)(Packed & 0xFFFFFFFFull);
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

        const uint32 NewIndex = (uint32)Batches.size();

        if (NewIndex == 0x10000u)
        {
            LOG_ERROR("SceneBatchRegistry: batch count reached {}, the limit PackDrawIDAndFlags can "
                      "address. Draw ids past this wrap.", 0x10000u);
        }

        FBatch& Batch = Batches.emplace_back();
        Batch.Key                             = Surface.BatchKey;
        Batch.VertexShader                    = Surface.VertexShader;
        Batch.MeshShaderShadow                = Surface.MeshShaderShadow;
        Batch.MeshShaderBase                  = Surface.MeshShaderBase;
        Batch.PixelShader                     = Surface.PixelShader;
        Batch.MomentPixelShader               = Surface.MomentPixelShader;
        Batch.VisBufferMeshShader             = Surface.VisBufferMeshShader;
        Batch.VisBufferMeshShaderMasked       = Surface.VisBufferMeshShaderMasked;
        Batch.MaskedVisBufferPixelShader      = Surface.MaskedVisBufferPixelShader;
        NoteDeferredMaterial(Batch, Surface);

        Bucket.push_back(NewIndex);
        ++LayoutGeneration;
        return NewIndex;
    }

    void FSceneBatchRegistry::AddBatchRef(uint32 BatchIndex)
    {
        ++Batches[BatchIndex].RefCount;
    }

    void FSceneBatchRegistry::ReleaseBatchRef(uint32 BatchIndex)
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
    }

    void FSceneBatchRegistry::Reset()
    {
        Batches.clear();
        BatchesByHash.clear();
        ++LayoutGeneration;
    }

    uint32 FScenePrimitiveSet::FindPrimitive(uint64 Key) const
    {
        const entt::entity     Entity = (entt::entity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);

        if (Source != EPrimitiveSource::Foliage)
        {
            return FindLinked(Entity, Source);
        }

        auto It = IndexByKey.find(Key);
        return It != IndexByKey.end() ? It->second : ~0u;
    }

    //~ Flat entity-index link table. See FPrimitiveLink for why this exists instead of a hash probe.

    uint32 FScenePrimitiveSet::FindLinked(entt::entity Entity, EPrimitiveSource Source) const
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)entt::to_entity(Entity);
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return ~0u;
        }

        const FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        return Link.Entity == entt::to_integral(Entity) ? Link.Index[(uint32)Source] : ~0u;
    }

    uint8 FScenePrimitiveSet::GetSourceMask(entt::entity Entity) const
    {
        const uint32 Slot = (uint32)entt::to_entity(Entity);
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return 0u;
        }

        const FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != entt::to_integral(Entity))
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

    void FScenePrimitiveSet::SetLink(entt::entity Entity, EPrimitiveSource Source, uint32 Index)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)entt::to_entity(Entity);
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            LinksByEntityIndex.resize(Slot + 1u);
        }

        FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != entt::to_integral(Entity))
        {
            Link = FPrimitiveLink{};
            Link.Entity = entt::to_integral(Entity);
        }
        Link.Index[(uint32)Source] = Index;
    }

    void FScenePrimitiveSet::ClearLink(entt::entity Entity, EPrimitiveSource Source)
    {
        DEBUG_ASSERT((uint32)Source < kLinkedSources);

        const uint32 Slot = (uint32)entt::to_entity(Entity);
        if (Slot >= (uint32)LinksByEntityIndex.size())
        {
            return;
        }

        FPrimitiveLink& Link = LinksByEntityIndex[Slot];
        if (Link.Entity != entt::to_integral(Entity))
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

        const entt::entity     Entity = (entt::entity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        if (Source != EPrimitiveSource::Foliage)
        {
            SetLink(Entity, Source, Index);
        }
        else
        {
            IndexByKey.emplace(Key, Index);
        }
        if (Source == EPrimitiveSource::SkeletalMesh)
        {
            ++SkinnedCount;
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

        ReleaseBindings(Index);
        ReleaseBoneSlice(Primitives[Index]);
        Primitives[Index].BoneCount = 0u;

        if (Index != Last)
        {
            Primitives[Index]  = std::move(Primitives[Last]);
            Bounds[Index]      = Bounds[Last];
            CullData[Index]    = CullData[Last];
            Keys[Index]        = Keys[Last];
            ResolveKeys[Index] = ResolveKeys[Last];

            const entt::entity     MovedEntity = (entt::entity)(uint32)(Keys[Index] & 0xFFFFFFFFull);
            const EPrimitiveSource MovedSource = (EPrimitiveSource)((Keys[Index] >> 32) & 0xFFull);
            if (MovedSource != EPrimitiveSource::Foliage)
            {
                SetLink(MovedEntity, MovedSource, Index);
            }
            else
            {
                IndexByKey[Keys[Index]] = Index;
            }
        }

        Primitives.pop_back();
        Bounds.pop_back();
        CullData.pop_back();
        Keys.pop_back();
        ResolveKeys.pop_back();

        const entt::entity     Entity = (entt::entity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        if (Source == EPrimitiveSource::SkeletalMesh && SkinnedCount > 0)
        {
            --SkinnedCount;
        }
        if (Source != EPrimitiveSource::Foliage)
        {
            ClearLink(Entity, Source);
        }
        else
        {
            // By key rather than by a held iterator: the moved entry's fixup above can rehash the map.
            IndexByKey.erase(Key);
        }

        ++StructureGeneration;
    }

    void FScenePrimitiveSet::RemoveEntity(FEntityRegistry& Registry, entt::entity Entity, EPrimitiveSource Source)
    {
        if (Source == EPrimitiveSource::Foliage)
        {
            auto It = FoliageInstanceCount.find(Entity);
            if (It == FoliageInstanceCount.end())
            {
                return;
            }
            const uint32 Count = It->second;
            for (uint32 i = 0; i < Count; ++i)
            {
                RemovePrimitive(MakeKey(Entity, EPrimitiveSource::Foliage, i));
            }
            FoliageInstanceCount.erase(It);
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
        Primitives[Index].BoneCount = Count;
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
            if (std::memcmp(&SurfaceDescs[Index], &Desc, sizeof(FSurfaceDescGPU)) == 0)
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

            // Skeletal primitives are Active now: CullInstances compacts them like everything else, taking
            // their per-frame LOD ranges and pre-skin slices from FSkinnedFrameData. They used to be held
            // inactive here and CPU-fed into the head of the visible buffer instead.
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

            FInstanceCullEntry& OutCull = RetainedCullEntries[Slot];
            OutCull.SphereBounds     = Sphere;
            OutCull.DrawIDAndFlags   = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
            OutCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
            OutCull.MaxDrawDistance  = Cull.MaxDrawDistance;
            OutCull.ForcedLODIndex   = Prim.ForcedLODIndex;

            RetainedTransforms[Slot] = PackTransform3x4(Prim.Transform);

            FInstanceStatic& OutStatic = RetainedStatic[Slot];
            OutStatic.MeshletHeaderSlot    = Prim.MeshletHeaderSlot;
            OutStatic.CustomData              = Prim.CustomData;
            OutStatic.MaterialIndex           = Binding.MaterialIndex;
            OutStatic.EntityID                = Prim.EntityID;
            // Stable for as long as the primitive holds its skeleton, which is what lets it live in a
            // payload that only re-uploads on a re-bind. 0 for anything unskinned; the shaders read it
            // only under EInstanceFlags::Skinned.
            OutStatic.BoneOffset              = 0u;
            // Still per-frame: assigned from a global budget every frame, so it cannot be stable here.
            // Phase 2 moves the claim onto the GPU and drops these two fields from this payload.
            OutStatic.SkinnedVertexBase       = 0u;
            OutStatic.ShadowSkinnedVertexBase = 0u;

            if (DirtySink != nullptr)
            {
                DirtySink->push_back(Slot);
            }
            else
            {
                MarkInstanceDirty(Slot);
            }
            MarkStaticDirty(Slot);
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

    void FScenePrimitiveSet::ReleaseBindings(uint32 Index)
    {
        FScenePrimitive& Prim = Primitives[Index];
        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Prim.BindingBase + s];
            Batches.ReleaseBatchRef(Binding.BatchIndex);
            FreeInstanceSlot(Binding.InstanceSlot);
        }
        DeadBindings += Prim.SurfaceCount;
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

        if (Entry.Generation != Generation || Entry.Protos.size() != Surfaces.size())
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

            Entry.Generation = Generation;
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

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Have = Bindings[Prim.BindingBase + s];
            const FSurfaceBinding& Want = Memo.Protos[s];

            if (Have.BatchIndex            != Want.BatchIndex
             || Have.SurfaceDescIndex      != Want.SurfaceDescIndex
             || Have.MaterialIndex         != Want.MaterialIndex
             || Have.MaterialFlags         != Want.MaterialFlags
             || Have.bMaterialCastsShadows != Want.bMaterialCastsShadows)
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

    // The memo above is keyed on a resolve handle, and dynamic meshes have none. They also re-resolve
    // their materials IN PLACE, into the same FDynamicMeshRenderData::Surfaces vector, so not one of the
    // three identity fields RefreshPrimitiveData compares can move when a material recompiles: the
    // vector's address is unchanged, the handle is always INVALID_MESH_RESOLVE_HANDLE, and the generation
    // is always 0. Their bindings have to be compared against the surfaces themselves.
    //
    // Everything BindSurfaces would produce is compared here except InstanceSlot (the rebind reallocates
    // it) and SurfaceDescIndex: the LOD table half of a surface is written only by Commit, which always
    // publishes a fresh FDynamicMeshRenderData, so a change there already moves the Surfaces pointer.
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

            // Compares the batch's KEY rather than calling FindOrAddBatch: this runs on primitives that
            // have usually not changed, and FindOrAdd would mint a batch on the miss that BindSurfaces is
            // about to mint anyway.
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

        const FBindingMemo* Memo = EnsureBindingMemo(Prim.ResolveHandle, Prim.ResolveGeneration, Surfaces);

        Prim.BindingBase  = (uint32)Bindings.size();
        Prim.SurfaceCount = (uint32)Surfaces.size();

        if (Memo != nullptr)
        {
            for (const FSurfaceBinding& Proto : Memo->Protos)
            {
                FSurfaceBinding& Binding = Bindings.emplace_back(Proto);
                Binding.InstanceSlot = AllocateInstanceSlot();

                Batches.AddBatchRef(Binding.BatchIndex);
            }
        }
        else
        {
            for (const FResolvedSurface& Surface : Surfaces)
            {
                const uint32 BatchIndex = Batches.FindOrAddBatch(Surface);
                Batches.AddBatchRef(BatchIndex);

                FSurfaceBinding& Binding = Bindings.emplace_back();
                Binding.BatchIndex            = BatchIndex;
                Binding.InstanceSlot          = AllocateInstanceSlot();
                Binding.SurfaceDescIndex      = InternSurfaceDesc(Surface);
                Binding.MaterialIndex         = Surface.MaterialIdx;
                Binding.MaterialFlags         = Surface.MaterialFlags;
                Binding.bMaterialCastsShadows = Surface.bMaterialCastsShadows;
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
        entt::storage_type_t<STransformComponent>*          Transform    = nullptr;
        const entt::storage_type_t<FRenderTransform>*       RenderXform  = nullptr;
        const entt::storage_type_t<SStaticMeshComponent>*   StaticMesh   = nullptr;
        const entt::storage_type_t<SSkeletalMeshComponent>* SkeletalMesh = nullptr;
        entt::storage_type_t<SDynamicMeshComponent>*        DynamicMesh  = nullptr;
        entt::storage_type_t<SFoliageComponent>*            Foliage      = nullptr;
        const entt::sparse_set*                             Disabled     = nullptr;
        const entt::sparse_set*                             Sources[kLinkedSources] = {};

        FMeshResolveCache*                                  ResolveCache = &FMeshResolveCache::Get();

        explicit FSyncPools(FEntityRegistry& Registry)
            : Transform(&Registry.storage<STransformComponent>())
            , RenderXform(&Registry.storage<FRenderTransform>())
            , StaticMesh(&Registry.storage<SStaticMeshComponent>())
            , SkeletalMesh(&Registry.storage<SSkeletalMeshComponent>())
            , DynamicMesh(&Registry.storage<SDynamicMeshComponent>())
            , Foliage(&Registry.storage<SFoliageComponent>())
            , Disabled(&Registry.storage<SDisabledTag>())
        {
            Sources[(uint32)EPrimitiveSource::StaticMesh]   = StaticMesh;
            Sources[(uint32)EPrimitiveSource::DynamicMesh]  = DynamicMesh;
            Sources[(uint32)EPrimitiveSource::SkeletalMesh] = SkeletalMesh;
        }
    };

    static FORCEINLINE FMatrix4 ReadRenderMatrix(const entt::storage_type_t<FRenderTransform>* RenderStorage,
                                                 entt::entity Entity, const STransformComponent& Transform)
    {
        return RenderStorage->contains(Entity) ? RenderStorage->get(Entity).Matrix
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

    bool FScenePrimitiveSet::RefreshPrimitiveData(const FSyncPools& Pools, uint32 Index)
    {
        FScenePrimitive&    Prim = Primitives[Index];
        FPrimitiveCullData& Cull = CullData[Index];

        const TVector<FResolvedSurface>* OldSurfaces = Prim.Surfaces;
        const uint32                     OldHandle   = Prim.ResolveHandle;
        const uint32                     OldGen      = Prim.ResolveGeneration;

        Prim.Surfaces = nullptr;
        Prim.EntityID = entt::to_integral(Prim.Entity);

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
                    if (!Pools.StaticMesh->contains(Prim.Entity)) { return false; }
                    const SStaticMeshComponent* C = &Pools.StaticMesh->get(Prim.Entity);
                    ReadCommonMeshState(Prim, Cull, *C);
                    Base     = C;
                    LiveMesh = (const void*)C->StaticMesh.Get();
                }
                else
                {
                    if (!Pools.SkeletalMesh->contains(Prim.Entity)) { return false; }
                    const SSkeletalMeshComponent* C = &Pools.SkeletalMesh->get(Prim.Entity);
                    ReadCommonMeshState(Prim, Cull, *C);
                    Base     = C;
                    LiveMesh = (const void*)C->SkeletalMesh.Get();

                    // Claimed here rather than at bind: the slice is keyed to the SKELETON, so it must
                    // survive a material re-bind and must be re-sized when the mesh itself changes. Serial
                    // by construction -- only the structural sync path reaches this.
                    const CSkeletalMesh*     SkelMesh = C->SkeletalMesh.Get();
                    const FSkeletonResource* SkelRes  = (SkelMesh != nullptr && SkelMesh->Skeleton.IsValid())
                                                      ? SkelMesh->Skeleton->GetSkeletonResource()
                                                      : nullptr;
                    SetBoneCount(Index, SkelRes != nullptr ? (uint32)SkelRes->GetNumBones() : 0u);
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
                if (!Pools.DynamicMesh->contains(Prim.Entity)) { return false; }
                SDynamicMeshComponent* C = &Pools.DynamicMesh->get(Prim.Entity);
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

                // The identity test below is blind here -- see BindingsMatchSurfaces. Without this, a
                // material recompile re-resolved the surfaces (new FShaderH, new batch key) and the
                // primitive kept drawing through its old binding, i.e. last build's shader.
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

        // One of the two places a primitive's resolve identity settles; mirror it for the sweep.
        ResolveKeys[Index] = PackResolveKey(Prim.ResolveHandle, Prim.ResolveGeneration);

        RebuildWorldBounds(Index);
        RefreshInstances(Index);
        ++StructureGeneration;

        return bResolved;
    }

    void FScenePrimitiveSet::SyncEntity(FEntityRegistry& Registry, const FSyncPools& Pools, entt::entity Entity,
                                        EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
        ++SyncStats.SyncEntityCalls;

        const uint64 Key = MakeKey(Entity, Source);
        uint32 Index = FindLinked(Entity, Source);
        
        if (Index == ~0u && Flags == EPrimitiveDirty::Transform)
        {
            return;
        }

        // move on a primitive that already exists. 
        if (Index != ~0u && Flags == EPrimitiveDirty::Transform)
        {
            if (!Pools.Transform->contains(Entity))
            {
                return;
            }

            Primitives[Index].Transform = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->get(Entity));
            RebuildWorldBounds(Index);
            RefreshInstances(Index);
            ++StructureGeneration;
            return;
        }

        // Does the entity still carry this source, and is it enabled?
        bool bShouldExist = Registry.valid(Entity) && !Pools.Disabled->contains(Entity);
        if (bShouldExist)
        {
            const uint32 SourceIdx = (uint32)Source;
            bShouldExist = (SourceIdx < kLinkedSources) && Pools.Sources[SourceIdx]->contains(Entity);
        }
        // A transform component is required to place it.
        if (bShouldExist && !Pools.Transform->contains(Entity))
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
            Primitives[Index].Transform = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->get(Entity));
        }

        if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Data | EPrimitiveDirty::Membership | EPrimitiveDirty::Visibility))
        {
            RefreshPrimitiveData(Pools, Index);
        }
        else if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Transform))
        {
            // Transform-only: rebuild the world sphere and restamp this primitive's instance slots.
            RebuildWorldBounds(Index);
            RefreshInstances(Index);
            ++StructureGeneration;
        }

    }

    void FScenePrimitiveSet::SyncFoliage(FEntityRegistry& Registry, const FSyncPools& Pools, entt::entity Entity,
                                         EPrimitiveDirty Flags)
    {
        (void)Flags;

        // Counted, not scoped -- see SyncEntity.
        ++SyncStats.SyncFoliageCalls;

        SFoliageComponent* Foliage = (Registry.valid(Entity) && Pools.Foliage->contains(Entity))
                                   ? &Pools.Foliage->get(Entity)
                                   : nullptr;
        const bool bEnabled = Foliage != nullptr && !Pools.Disabled->contains(Entity);

        if (!bEnabled)
        {
            RemoveEntity(Registry, Entity, EPrimitiveSource::Foliage);
            return;
        }

        Foliage->EnsureRenderCache();

        const TVector<FFoliageBakedInstance>& Baked = Foliage->BakedInstances;
        const uint32 NewCount = (uint32)Baked.size();

        uint32& OldCount = FoliageInstanceCount[Entity];

        // Shrink: drop the tail. Grow and overlap are handled by the write loop below.
        for (uint32 i = NewCount; i < OldCount; ++i)
        {
            RemovePrimitive(MakeKey(Entity, EPrimitiveSource::Foliage, i));
        }

        const uint32 TypeCount = (uint32)Foliage->Types.size();
        FoliageTypeScratch.clear();
        FoliageTypeScratch.resize(TypeCount);

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
        }

        for (uint32 i = 0; i < NewCount; ++i)
        {
            const FFoliageBakedInstance& Instance = Baked[i];
            const uint64 Key = MakeKey(Entity, EPrimitiveSource::Foliage, i);

            uint32 Index = FindPrimitive(Key);
            if (Index == ~0u)
            {
                Index = AddPrimitive(Key);
                Primitives[Index].Entity = Entity;
                Primitives[Index].Source = EPrimitiveSource::Foliage;
            }

            FScenePrimitive&    Prim = Primitives[Index];
            FPrimitiveCullData& Cull = CullData[Index];

            const TVector<FResolvedSurface>* NewSurfaces = nullptr;
            uint32 NewGeneration = 0;

            // Same range test IsValidType makes; the scratch is sized from Types.
            if (Instance.TypeIndex >= 0 && (uint32)Instance.TypeIndex < TypeCount)
            {
                const FFoliageTypeResolve& Type = FoliageTypeScratch[Instance.TypeIndex];

                Prim.MeshletHeaderSlot = Type.MeshletHeaderSlot;
                Prim.BaseFlags            = Type.BaseFlags;
                Prim.ResolveHandle        = Type.ResolveHandle;
                Prim.bCastShadow          = Type.bCastShadow;
                Prim.ForcedLODIndex       = -1;
                Cull.MaxDrawDistance      = Type.MaxDrawDistance;
                Cull.bCastShadow          = Type.bCastShadow ? 1u : 0u;

                NewSurfaces   = Type.Surfaces;
                NewGeneration = Type.Generation;
            }

            const bool bSurfacesChanged = (NewSurfaces != Prim.Surfaces) || (NewGeneration != Prim.ResolveGeneration);
            Prim.Surfaces          = NewSurfaces;
            Prim.ResolveGeneration = NewGeneration;
            if (bSurfacesChanged)
            {
                const FBindingMemo* Memo = (NewSurfaces != nullptr && !NewSurfaces->empty())
                    ? EnsureBindingMemo(Prim.ResolveHandle, NewGeneration, *NewSurfaces)
                    : nullptr;

                if (Memo != nullptr && BindingsMatchMemo(Index, *Memo))
                {
                    ++SyncStats.BindsSkipped;
                }
                else
                {
                    BindSurfaces(Index);
                }
            }

            // The other place a resolve identity settles; mirror it for the sweep.
            ResolveKeys[Index] = PackResolveKey(Prim.ResolveHandle, Prim.ResolveGeneration);

            Prim.Transform = Instance.Transform;
            Prim.EntityID  = entt::to_integral(Entity);
            Prim.CustomData = 0u;

            // The bake already produced the world sphere; there is no local sphere to transform.
            Bounds[Index] = Instance.SphereBounds;
            RefreshInstances(Index);
        }

        OldCount = NewCount;
        ++StructureGeneration;
    }

    void FScenePrimitiveSet::FullRescan(FEntityRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        Batches.Reset();
        // Paired with Batches.Reset(), always: a reset renumbers every batch index the memo cached.
        BindingMemoByHandle.clear();
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        ResolveKeys.clear();
        IndexByKey.clear();
        Bindings.clear();
        // Every slice died with the primitives, so the arena restarts rather than leaking live bases.
        BoneSliceFreeLists.clear();
        BoneSliceExtent = 0;
        BoneSliceSweepCursor = 0;
        DeadBindings = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
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
        FoliageInstanceCount.clear();

        const FSyncPools Pools(Registry);

        // Level load rebuilds from nothing, so unlike the incremental path the counts here are exact.
        const SIZE_T MeshCount = Pools.StaticMesh->size() + Pools.SkeletalMesh->size() + Pools.DynamicMesh->size();
        Primitives.reserve(MeshCount);
        Bounds.reserve(MeshCount);
        CullData.reserve(MeshCount);
        Keys.reserve(MeshCount);
        ResolveKeys.reserve(MeshCount);
        Bindings.reserve(MeshCount);
        RetainedCullEntries.reserve(MeshCount);
        RetainedTransforms.reserve(MeshCount);
        RetainedStatic.reserve(MeshCount);

        for (entt::entity Entity : Registry.view<SStaticMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::StaticMesh, EPrimitiveDirty::All);
        }
        for (entt::entity Entity : Registry.view<SDynamicMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::All);
        }
        for (entt::entity Entity : Registry.view<SSkeletalMeshComponent>())
        {
            SyncEntity(Registry, Pools, Entity, EPrimitiveSource::SkeletalMesh, EPrimitiveDirty::All);
        }
        for (entt::entity Entity : Registry.view<SFoliageComponent>())
        {
            SyncFoliage(Registry, Pools, Entity, EPrimitiveDirty::All);
        }

        ++StructureGeneration;
    }

    void FScenePrimitiveSet::PollUnhookedSources(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        for (auto&& [Entity, Foliage] : Registry.view<SFoliageComponent>().each())
        {
            if (Foliage.BakedVersion != Foliage.InstancesVersion || Foliage.bBakeIncomplete)
            {
                Tracker.Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
            }
        }
        
        for (auto&& [Entity, Mesh] : Registry.view<SDynamicMeshComponent>().each())
        {
            if (Mesh.SyncedRenderDataVersion != Mesh.LoadRenderDataVersion())
            {
                Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);
            }
        }

        PollSkeletalBoneRanges(Registry, Tracker);
    }

    // A skeleton finishing its load marks nothing dirty, so a primitive that synced mid-load stays boneless.
    void FScenePrimitiveSet::PollSkeletalBoneRanges(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        if (SkinnedCount == 0u)
        {
            return;
        }

        const entt::storage_type_t<SSkeletalMeshComponent>* Storage =
            &Registry.storage<SSkeletalMeshComponent>();

        for (const FScenePrimitive& Prim : Primitives)
        {
            // A live slice can only need resizing when the MESH changes, which marks the primitive itself.
            if (Prim.Source != EPrimitiveSource::SkeletalMesh || Prim.BoneCount != 0u)
            {
                continue;
            }

            if (!Storage->contains(Prim.Entity))
            {
                continue;
            }

            const CSkeletalMesh* SkelMesh = Storage->get(Prim.Entity).SkeletalMesh.Get();
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

        // Stamping beats clearing: the table is sized by the entity index space, not by the drain.
        ++CoalesceStamp;
        CoalescedScratch.clear();
        CoalescedScratch.reserve(DrainScratch.size());

        for (const FRenderDirtyTracker::FEntry& Entry : DrainScratch)
        {
            const uint32 Slot = (uint32)entt::to_entity(Entry.Entity);
            if (Slot >= (uint32)CoalesceByEntityIndex.size())
            {
                CoalesceByEntityIndex.resize(Slot + 1u);
            }

            FCoalesceSlot& Mapped = CoalesceByEntityIndex[Slot];

            uint32 RecordIndex = ~0u;
            if (Mapped.Stamp == CoalesceStamp
                && Mapped.Index < (uint32)CoalescedScratch.size()
                && CoalescedScratch[Mapped.Index].Entity == entt::to_integral(Entry.Entity))
            {
                RecordIndex = Mapped.Index;
            }

            if (RecordIndex == ~0u)
            {
                RecordIndex = (uint32)CoalescedScratch.size();
                CoalescedScratch.emplace_back().Entity = entt::to_integral(Entry.Entity);
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
            const entt::entity Entity = (entt::entity)Record.Entity;
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

    void FScenePrimitiveSet::ApplyStructuralRecords(FEntityRegistry& Registry, const FSyncPools& Pools)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Structural");

        for (uint32 RecordIndex : StructuralRecords)
        {
            const FCoalescedEntity& Record = CoalescedScratch[RecordIndex];
            const entt::entity      Entity = (entt::entity)Record.Entity;

            for (uint32 s = 0; s < kLinkedSources; ++s)
            {
                if (Record.Flags[s] != EPrimitiveDirty::None)
                {
                    SyncEntity(Registry, Pools, Entity, (EPrimitiveSource)s, Record.Flags[s]);
                }
            }

            const EPrimitiveDirty FoliageFlags = Record.Flags[(uint32)EPrimitiveSource::Foliage];
            if (FoliageFlags != EPrimitiveDirty::None
                && (Pools.Foliage->contains(Entity)
                    || (!FoliageInstanceCount.empty() && FoliageInstanceCount.find(Entity) != FoliageInstanceCount.end())))
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
        const entt::entity      Entity = (entt::entity)Record.Entity;

        const uint8 Mask = GetSourceMask(Entity);
        if (Mask == 0)
        {
            return;
        }

        if (!Pools.Transform->contains(Entity))
        {
            return;
        }

        const FMatrix4 Matrix = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->get(Entity));

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
                // nullptr sink: straight onto the shared list, thresholds and all. Nothing to merge.
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
        FEntityRegistry&     Registry = ECS::GetWorldRegistry(World);
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
            // Drain and discard: the rescan supersedes every queued entry.
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

            for (uint32 i = 0, N = (uint32)Primitives.size(); i < N; ++i)
            {
                const uint64 Packed = ResolveKeys[i];
                const uint32 Handle = (uint32)(Packed & 0xFFFFFFFFull);

                if (Handle >= (uint32)GenSnapshot.size())
                {
                    continue;
                }

                DEBUG_ASSERT(Primitives[i].ResolveHandle == Handle);
                DEBUG_ASSERT(Primitives[i].ResolveGeneration == (uint32)(Packed >> 32));

                uint32 Generation = GenSnapshot[Handle];
                if (Generation == ~0u)
                {
                    Generation = Cache.GetEntry(Handle).Generation;
                    GenSnapshot[Handle] = Generation;
                }

                if (Generation != (uint32)(Packed >> 32))
                {
                    RetryScratch.push_back(Keys[i] & kSyncTargetMask);
                }
            }

            Algo::Sort(RetryScratch.begin(), RetryScratch.end());
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
                const entt::entity     Entity = (entt::entity)(uint32)(Target & 0xFFFFFFFFull);
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

    // Every active slot contributes its surface's LARGEST LOD, because a view may pick any of them.
    // Summing that is a true bound on one view's appends, whatever the camera does.
    void FScenePrimitiveSet::Reset(FEntityRegistry* Registry)
    {
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        ResolveKeys.clear();
        IndexByKey.clear();
        Bindings.clear();
        // Every slice died with the primitives, so the arena restarts rather than leaking live bases.
        BoneSliceFreeLists.clear();
        BoneSliceExtent = 0;
        BoneSliceSweepCursor = 0;
        DeadBindings = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
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
        FoliageInstanceCount.clear();
        Batches.Reset();
        // Paired with Batches.Reset(), always: a reset renumbers every batch index the memo cached.
        BindingMemoByHandle.clear();
        bResolveTableChanged = false;
        ++StructureGeneration;

        if (Registry != nullptr)
        {
            FRenderDirtyTracker::Ensure(*Registry).RequestFullRescan();
        }
    }
}
