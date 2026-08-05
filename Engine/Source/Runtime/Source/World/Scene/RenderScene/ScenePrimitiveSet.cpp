#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
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
    //-------------------------------------------------------------------------
    // FRenderDirtyTracker
    //-------------------------------------------------------------------------

    // Entity (32) | Source (8) | Flags (8). Packing keeps the queue element trivially copyable and
    // one word wide, which is what makes the bulk dequeue a memcpy.
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
        // One entry, expanded by the sync pass. Transform marks are the highest-volume producer here
        // (one per moving entity per frame), so widening them to one entry per source would multiply
        // the queue traffic for no benefit.
        Mark(Entity, EPrimitiveSource::Any, Flags);
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

    //-------------------------------------------------------------------------
    // FSceneBatchRegistry
    //-------------------------------------------------------------------------

    // Linear scan over a list that stays tiny (one entry per material instance of one master).
    static void NoteDeferredMaterial(FSceneBatchRegistry::FBatch& Batch, const FResolvedSurface& Surface)
    {
        if (Surface.DeferredShader == nullptr || Surface.MaterialIdx == (uint16)-1
            || Surface.BatchKey.bTranslucent != 0u)
        {
            return;
        }

        for (const FSceneBatchRegistry::FDeferredMaterialSlot& Existing : Batch.DeferredMaterials)
        {
            if (Existing.MaterialIndex == Surface.MaterialIdx && Existing.DeferredShader == Surface.DeferredShader)
            {
                return;
            }
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

        // PackDrawIDAndFlags carries the draw id in 16 bits (the other 16 are instance flags), so past
        // this a batch index wraps and instances silently draw against the wrong PSO. Batches number in
        // the tens to hundreds, so reaching this means something is interning them per-instance.
        if (NewIndex == 0x10000u)
        {
            LOG_ERROR("SceneBatchRegistry: batch count reached {}, the limit PackDrawIDAndFlags can "
                      "address. Draw ids past this wrap.", 0x10000u);
        }

        FBatch& Batch = Batches.emplace_back();
        Batch.Key                             = Surface.BatchKey;
        Batch.VertexShader                    = Surface.VertexShader;
        Batch.PixelShader                     = Surface.PixelShader;
        Batch.MeshShader                      = Surface.MeshShader;
        Batch.VisBufferMeshShader             = Surface.VisBufferMeshShader;
        Batch.VisBufferVertexShader           = Surface.VisBufferVertexShader;
        Batch.MaskedVisBufferPixelShader      = Surface.MaskedVisBufferPixelShader;
        Batch.MaskedVisBufferPixelShaderPrim  = Surface.MaskedVisBufferPixelShaderPrim;
        Batch.DeferredShader                  = Surface.DeferredShader;
        Batch.MaterialIdx                     = Surface.MaterialIdx;
        Batch.bMaterialCastsShadows           = Surface.bMaterialCastsShadows;
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

    //-------------------------------------------------------------------------
    // FScenePrimitiveSet
    //-------------------------------------------------------------------------

    /**
     * Index of the primitive owning this key, or ~0u.
     *
     * Non-foliage keys are served from the flat link table, NOT from IndexByKey. One (entity, source) owns
     * at most one primitive, which is exactly what the table already stores and already maintains in the
     * same statements -- so carrying those keys in the hash map bought nothing and cost a node ALLOCATION
     * on every add and a node FREE on every remove, plus the probes. Only foliage genuinely needs the map:
     * one entity owns one primitive per baked instance, keyed with a third sub-index field that the
     * one-slot-per-source table cannot express.
     */
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
            // Slot belonged to a since-destroyed entity whose index got recycled. Its primitives were
            // removed with it, so adopting the slot wholesale is correct -- and required, or a stale
            // sibling index would survive into the new entity.
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

        // Release the slot once nothing is left, so a later recycle of this index starts clean without
        // depending on the guard alone.
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
        // Lockstep with Keys. A fresh primitive has no resolve yet; RefreshPrimitiveData / SyncFoliage
        // restamp this the moment they settle one.
        ResolveKeys.push_back(PackResolveKey(INVALID_MESH_RESOLVE_HANDLE, 0));

        // Exactly one of the two indexes carries this primitive; see FindPrimitive. Foliage is absent from
        // the link table because it owns many primitives per entity, and the other three are absent from
        // the map because the link table already answers the same question without a heap node.
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

        // Hand this primitive's draw slots back before the entry is overwritten.
        ReleaseBindings(Index);

        // Swap-with-last keeps the arrays dense so the per-frame cull never walks holes. The moved entry's
        // index registration is the only fixup; its own key comes from the parallel Keys array.
        if (Index != Last)
        {
            // MOVE, not copy. FScenePrimitive holds a TSharedPtr to the dynamic-mesh render data, so a copy
            // is an atomic increment here plus an atomic decrement when the source is popped below -- per
            // removal, on a path a mass despawn runs thousands of times.
            Primitives[Index]  = eastl::move(Primitives[Last]);
            Bounds[Index]      = Bounds[Last];
            CullData[Index]    = CullData[Last];
            Keys[Index]        = Keys[Last];
            ResolveKeys[Index] = ResolveKeys[Last];

            // The moved entry's index registration has to follow it. Its own key carries the (entity,
            // source) pair, so this needs no lookup -- and skipping it would leave that entity pointing at
            // the slot this primitive just vacated, which is the classic swap-remove aliasing bug.
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

        // Transform the cached local sphere rather than rebuilding a world AABB: fewer ops, and it stays
        // tight under rotation. BoundsScale inflates it for animation/displacement past the asset bounds.
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

    //-------------------------------------------------------------------------
    // Retained GPU scene
    //-------------------------------------------------------------------------

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

        // A reallocation moves every slot, so the device copy has to be re-sent wholesale rather than
        // patched. Growth is amortized, so this is rare after the scene settles. Checked on the cull
        // entries alone: the three grow in lockstep, so one is the whole story.
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

        // The Active flag is what the cull reads; the transform and static payload beside it are left
        // stale on purpose, because writing 96 bytes to say "ignore me" would be pure waste. Only the
        // cull entry is dirtied for the same reason.
        RetainedCullEntries[Slot].DrawIDAndFlags &= ~((uint32)EInstanceFlags::Active << 16);
        MarkInstanceDirty(Slot);
        InstanceFreeSlots.push_back(Slot);
    }

    void FScenePrimitiveSet::MarkInstanceDirty(uint32 Slot)
    {
        // Already committed to re-sending the whole buffer; tracking individual slots is pure waste.
        if (bFullInstanceUpload)
        {
            return;
        }

        // PublishRetainedUpload throws the list away and uploads whole once it passes a quarter of the slot
        // space. Deciding that HERE rather than there stops the accumulation as well as the sort+unique that
        // used to run over a list about to be discarded -- which on a bulk add is every frame. The floor
        // keeps a small scene from flipping to full uploads over a handful of slots, same reasoning as
        // CompactBindings' DeadBindings floor.
        const SIZE_T SlotCount = RetainedCullEntries.size();
        if (SlotCount > 1024 && DirtyInstanceSlots.size() * 4 >= SlotCount)
        {
            bFullInstanceUpload = true;
            DirtyInstanceSlots.clear();
            DirtyStaticSlots.clear();
            return;
        }

        // Duplicates are fine: the upload coalesces, and de-duplicating here would cost a set lookup on
        // the hot path to save a repeated copy.
        DirtyInstanceSlots.push_back(Slot);
    }

    // Same policy as MarkInstanceDirty, on the list that covers the static payload. Separate because the
    // two fill at wildly different rates: a crowd moving fills the instance list every frame and leaves
    // this one empty, and re-sending an unchanged third buffer along with them is exactly what splitting
    // the arrays was for.
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
            
            if (Desc.LODMeshletCount[i] > MAX_MESHLETS_PER_SURFACE_LOD)
            {
                LOG_ERROR("ScenePrimitiveSet: surface LOD {} has {} meshlets, past the {} the draw list can "
                          "index. Clamping; the mesh needs splitting or a coarser meshlet build.",
                          i, Desc.LODMeshletCount[i], MAX_MESHLETS_PER_SURFACE_LOD);
                Desc.LODMeshletCount[i] = MAX_MESHLETS_PER_SURFACE_LOD;
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

        // Full compare on a hash hit; a collision handing back another mesh's LOD table would draw the
        // wrong geometry, which is exactly the class of bug the resolve cache guards against too.
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

        // Folded in HERE, from the clamped copy that just entered the table, rather than by rescanning
        // SurfaceDescs later. The render phase multiplies this by the retained slot count to get the
        // meshlet cull's dispatch ceiling, so it has to come from a value this thread owns.
        const uint32 DescLODs = Math::Min<uint32>(Desc.NumLODs, MAX_MESH_LODS);
        for (uint32 i = 0; i < DescLODs; ++i)
        {
            MaxSurfaceDescMeshlets = Math::Max(MaxSurfaceDescMeshlets, Desc.LODMeshletCount[i]);
        }

        return NewIndex;
    }

    // Writes everything the GPU cull reads about one primitive into its retained slots. This is the only
    // place instance data is produced -- there is no per-frame equivalent.
    void FScenePrimitiveSet::RefreshInstances(uint32 Index, TVector<uint32>* DirtySink)
    {
        // Counted on the serial path only; the parallel transform pass adds its records in one go, because
        // a shared counter incremented from every worker is a data race for a number nobody reads per-item.
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

            // Skeletal primitives are CPU-fed every frame (bones + pre-skin bases have no retained
            // form); letting the GPU cull append this slot too would draw a second copy whose
            // SkinnedVertexBase of 0 reads someone else's pre-skin slice.
            if (Prim.Surfaces != nullptr && Prim.Source != EPrimitiveSource::SkeletalMesh)
            {
                Flags |= EInstanceFlags::Active;
            }
            // Gates the LOD meshlet counts in the cull. A null header with a non-zero count would page
            // fault the meshlet pass, and the cull must know this WITHOUT loading the static payload --
            // it decides on it before an instance has earned that load.
            if (Prim.MeshletHeaderAddress != 0ull)
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
            OutStatic.MeshletHeaderAddress    = Prim.MeshletHeaderAddress;
            OutStatic.CustomData              = Prim.CustomData;
            OutStatic.MaterialIndex           = Binding.MaterialIndex;
            OutStatic.EntityID                = Prim.EntityID;
            OutStatic.BoneOffset              = 0u;
            OutStatic.SkinnedVertexBase       = 0u;
            OutStatic.ShadowSkinnedVertexBase = 0u;

            if (DirtySink != nullptr)
            {
                // Caller-owned list: no shared state, and the full-upload threshold is evaluated once
                // during the merge rather than raced by every worker.
                DirtySink->push_back(Slot);
            }
            else
            {
                MarkInstanceDirty(Slot);
            }
            // Always the shared list: only the serial structural pass ever rewrites the static payload,
            // so there is no parallel writer to keep off it.
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

            // The two fields a move can change, and nothing else. The flags, batch, surface desc, draw
            // distance and LOD override in this entry are all unaffected by it, and the static payload
            // is not touched at all -- so its buffer stays clean and is not re-sent.
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

    // Drops the refs a primitive's current bindings hold, so a re-bind or a removal returns its draw
    // slots to the registry instead of stranding them.
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

        // The surface count is checked alongside the generation purely as insurance: a resolved entry
        // always bumps its generation, so the two cannot legitimately disagree.
        if (Entry.Generation != Generation || Entry.Protos.size() != Surfaces.size())
        {
            Entry.Protos.clear();
            Entry.Protos.reserve(Surfaces.size());

            for (const FResolvedSurface& Surface : Surfaces)
            {
                FSurfaceBinding& Proto = Entry.Protos.emplace_back();

                // FindOrAddBatch also folds this surface's deferred material slot into the batch. That
                // fold is keyed on the same surface and is idempotent, so running it once per interned
                // entry instead of once per instance is equivalent -- provided the memo and the batch
                // registry are only ever invalidated together, which is the stated invariant.
                Proto.BatchIndex            = Batches.FindOrAddBatch(Surface);
                Proto.InstanceSlot          = ~0u;
                Proto.SurfaceDescIndex      = InternSurfaceDesc(Surface);
                Proto.MaterialIndex         = Surface.MaterialIdx;
                Proto.MaterialFlags         = Surface.MaterialFlags;
                Proto.bMaterialCastsShadows = Surface.bMaterialCastsShadows;
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

            // InstanceSlot is deliberately absent: it is the primitive's own identity in the retained
            // buffer, not something the resolve derives, and keeping it is the entire point of matching.
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

        // Resolve the batch and LOD-table identities once per interned mesh rather than once per instance.
        // See BindingMemoByHandle. Usually already built: the caller consulted it to decide whether this
        // rebind was needed at all.
        const FBindingMemo* Memo = EnsureBindingMemo(Prim.ResolveHandle, Prim.ResolveGeneration, Surfaces);

        Prim.BindingBase  = (uint32)Bindings.size();
        Prim.SurfaceCount = (uint32)Surfaces.size();

        if (Memo != nullptr)
        {
            for (const FSurfaceBinding& Proto : Memo->Protos)
            {
                FSurfaceBinding& Binding = Bindings.emplace_back(Proto);
                Binding.InstanceSlot = AllocateInstanceSlot();

                // Per BINDING, never memoized: ReleaseBindings drops one ref for each and the counts have
                // to balance or a batch outlives everything drawing it.
                Batches.AddBatchRef(Binding.BatchIndex);
            }
        }
        else
        {
            // Dynamic meshes carry no resolve handle. Their Surfaces pointer IS their version, so a
            // pointer-keyed memo would dangle the moment Commit republishes, and they number in the tens.
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
            }
        }

        // 1/4 dead is where the wasted cache traffic in the emit loop starts to outweigh the compaction
        // pass; the floor keeps small scenes from compacting on every mesh swap.
        if (DeadBindings > 1024 && DeadBindings * 4 > (uint32)Bindings.size())
        {
            CompactBindings();
        }
    }

    // Physics interpolates simulated bodies to display time and parks the result in FRenderTransform;
    // STransformComponent holds the simulated pose, which is a fixed step behind what should be on screen.
    // Everything without an override renders straight off the resolved world matrix.
    // See the forward declaration in the header for why these are hoisted. Sources is indexed by
    // EPrimitiveSource, so it deliberately stops short of Foliage -- SyncEntity never handles that source.
    struct FScenePrimitiveSet::FSyncPools
    {
        entt::storage_type_t<STransformComponent>*          Transform    = nullptr;
        const entt::storage_type_t<FRenderTransform>*       RenderXform  = nullptr;
        const entt::storage_type_t<SStaticMeshComponent>*   StaticMesh   = nullptr;
        const entt::storage_type_t<SSkeletalMeshComponent>* SkeletalMesh = nullptr;
        // Non-const: RefreshPrimitiveData stamps SyncedRenderDataVersion on the component it reads, and
        // SyncFoliage rebakes through EnsureRenderCache.
        entt::storage_type_t<SDynamicMeshComponent>*        DynamicMesh  = nullptr;
        entt::storage_type_t<SFoliageComponent>*            Foliage      = nullptr;
        const entt::sparse_set*                             Disabled     = nullptr;
        const entt::sparse_set*                             Sources[kLinkedSources] = {};

        // Same reasoning as the pools. FMeshResolveCache::Get() is a function-local static, so every call
        // is a guard-variable load and a branch -- and SyncFoliage used to make one PER BAKED INSTANCE.
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

    //
    // Takes the resolved pool rather than the registry: this runs once per moving entity per frame, and
    // Registry.storage<T>() is a type-id hash lookup. FSyncPools exists to pay that once per sync.
    static FORCEINLINE const FMatrix4& ReadRenderMatrix(const entt::storage_type_t<FRenderTransform>* RenderStorage,
                                                        entt::entity Entity, const STransformComponent& Transform)
    {
        return RenderStorage->contains(Entity) ? RenderStorage->get(Entity).Matrix : Transform.CachedMatrix;
    }

    // Copies the camera-independent render state out of one mesh component. Shared by the three mesh
    // sources; the caller supplies the mesh pointer and the seed flags that differ between them.
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

        switch (Prim.Source)
        {
        case EPrimitiveSource::StaticMesh:
        case EPrimitiveSource::SkeletalMesh:
            {
                // Both asset-backed paths share the interned resolve; the only difference is which field
                // holds the mesh and whether the Skinned seed flag is set (already folded into
                // CachedBaseFlags by ResolveDirtyMeshComponents).
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
                }

                Prim.LocalCenter          = Base->CachedLocalCenter;
                Prim.LocalRadius          = Base->CachedLocalRadius;
                Prim.MeshletHeaderAddress = Base->CachedMeshletHeaderAddress;
                Prim.BaseFlags            = Base->CachedBaseFlags;
                Prim.ResolveHandle        = Base->ResolveHandle;

                // Same self-heal the old gather did per frame per entity: a mesh assigned directly (editor
                // tools, thumbnails) bypasses the setters, so the resolve can disagree with the live field.
                // Re-arming the resolve pass is what retries this -- there is no separate retry list,
                // because MarkPendingWork is already the engine's "come back to this" signal and
                // ResolveDirtyMeshComponents marks every component it revisits.
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
                    // The resolve pass leaves an unresolved entry's component unstamped and re-marks
                    // pending work itself, so it will revisit and re-mark this primitive.
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

                // Stamped here, before the readiness checks below, not on success. This records "the sync
                // has looked at this version", which is what PollUnhookedSources asks. Stamping only on a
                // successful resolve would leave a cleared or empty mesh permanently mismatched, and the
                // poll would re-mark it every frame forever.
                C->SyncedRenderDataVersion = C->LoadRenderDataVersion();

                // Dynamic meshes own their resolve outright: Commit publishes RenderData synchronously, so
                // there is no cache handle, no epoch and no staleness check.
                //
                // Ref-taken ONCE, atomically: Commit() can swap the component's pointer from a worker, so
                // reading it twice could see two different snapshots, and a plain copy would race the swap
                // outright. Holding the ref also keeps the surfaces alive for the gather that reads them.
                Prim.DynamicRenderData = C->LoadRenderData();

                const FDynamicMeshRenderData* Data = Prim.DynamicRenderData.get();
                if (Data == nullptr || Data->MeshletHeaderAddress == 0 || Data->Surfaces.empty())
                {
                    Prim.DynamicRenderData.reset();
                    break;
                }

                Prim.LocalCenter          = Data->LocalCenter;
                Prim.LocalRadius          = Data->LocalRadius;
                Prim.MeshletHeaderAddress = Data->MeshletHeaderAddress;
                Prim.ResolveHandle        = INVALID_MESH_RESOLVE_HANDLE;

                EInstanceFlags BaseFlags = EInstanceFlags::None;
                if (C->bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
                if (C->bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
                Prim.BaseFlags = BaseFlags;

                Prim.Surfaces          = &Data->Surfaces;
                // The component's render data has no generation; the pointer identity is the version,
                // and Commit always publishes a fresh FDynamicMeshRenderData.
                Prim.ResolveGeneration = 0;
                bResolved              = true;
            }
            break;

        default:
            // Foliage never reaches here: SyncEntity rejects that source outright and Sync routes it to
            // SyncFoliage, which fills a foliage primitive wholesale from the bake.
            break;
        }

        // Re-bind only when the surface set actually changed. A flags-only edit (cast shadow, LOD
        // override, bounds scale) leaves the batch/draw identity alone, so it costs nothing here.
        const bool bSurfacesChanged = (Prim.Surfaces != OldSurfaces)
                                   || (Prim.ResolveHandle != OldHandle)
                                   || (Prim.ResolveGeneration != OldGen);
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
        // Counted, not scoped. TRACY_CALLSTACK is defined engine-wide, so every LUMINA_PROFILE_SCOPE is a
        // tracy_malloc plus an RtlWalkFrameChain unwind while the profiler is attached -- microseconds, on a
        // function that runs once per dirty entity per frame. It swamped what it was measuring.
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

        // Transform first: a data refresh rebuilds the world sphere from it. No contains() guard -- reaching
        // here required bShouldExist, which already established the transform pool holds this entity.
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

        // Foliage already caches its per-instance world transform + cull sphere and rebakes only when
        // InstancesVersion moves, so this is where a paint/erase turns into primitive churn.
        Foliage->EnsureRenderCache();

        const TVector<FFoliageBakedInstance>& Baked = Foliage->BakedInstances;
        const uint32 NewCount = (uint32)Baked.size();

        uint32& OldCount = FoliageInstanceCount[Entity];

        // Shrink: drop the tail. Grow and overlap are handled by the write loop below.
        for (uint32 i = NewCount; i < OldCount; ++i)
        {
            RemovePrimitive(MakeKey(Entity, EPrimitiveSource::Foliage, i));
        }

        /**
         * Everything an instance takes from its type, resolved ONCE per type.
         *
         * Types number in the single digits; instances number in the hundreds of thousands. Doing this per
         * instance meant, for every blade of grass: a random index into a large SFoliageType, a
         * FMeshResolveCache::Get() guard check, a TObjectPtr->CMesh handle resolve, and a dependent load
         * into a heap-boxed resolve entry. None of it varies within a type.
         */
        const uint32 TypeCount = (uint32)Foliage->Types.size();
        FoliageTypeScratch.clear();
        FoliageTypeScratch.resize(TypeCount);

        FMeshResolveCache& Cache = *Pools.ResolveCache;
        for (uint32 t = 0; t < TypeCount; ++t)
        {
            const SFoliageType&   Type = Foliage->Types[t];
            FFoliageTypeResolve&  Out  = FoliageTypeScratch[t];

            Out.MeshletHeaderAddress = Type.CachedMeshletHeaderAddress;
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

                Prim.MeshletHeaderAddress = Type.MeshletHeaderAddress;
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
                /**
                 * Almost always a generation bump with identical bindings behind it.
                 *
                 * Resolve entries are heap-boxed and never move, so NewSurfaces compares EQUAL across a
                 * rebuild and only the generation differs -- which is exactly the case where a rebind
                 * reproduces the batch index and interned LOD table it already had. Rebinding anyway cost
                 * a release + realloc of every instance slot this entity owns, and at foliage scale that
                 * is once per blade of grass: N batch-ref round trips, N free-list pops, N dead bindings
                 * appended to Bindings (which then trips CompactBindings and repacks the entire array),
                 * and two dirty marks per slot instead of one.
                 *
                 * The memo makes the alternative cheap: build the per-type template once, then a handful
                 * of integer compares per surface says whether anything actually moved.
                 */
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

        // No retry list for foliage: a type whose mesh is still loading leaves its CachedEntryState stale,
        // so ResolveDirtyMeshComponents revisits it and marks this entity. A type with no mesh at all is
        // permanently undrawable and should NOT retry -- which is exactly what having no retry list gives.
        OldCount = NewCount;
        ++StructureGeneration;
    }

    void FScenePrimitiveSet::FullRescan(FEntityRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        // Everything currently known is re-derived below; anything that no longer exists simply never
        // gets re-added. Cheaper and far less error-prone than diffing. The batch registry goes with it:
        // dropping the primitives without releasing their bindings would strand every refcount.
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
        // Dropped with the table it summarizes; carrying a bound for descs that are gone would leave the
        // cull's dispatch ceiling permanently wider than the scene.
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
        // IndexByKey deliberately NOT reserved from MeshCount: it holds foliage primitives only now, and
        // mesh components say nothing about how many baked instances a level has.
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

    /**
     * Two sources cannot route their invalidation through an entt hook: SFoliageComponent's paint/erase/
     * terrain-follow all go through MarkInstancesChanged(), and SDynamicMeshComponent republishes its
     * geometry from Commit(). Both are plain component methods with no registry access, so there is
     * nothing to fire a signal from.
     *
     * They are polled instead. The cost is O(components), never O(instances): one foliage component owns
     * every blade of grass in a level, one dynamic mesh component owns its whole geometry. Giving those
     * two component types a registry back-pointer just to emit a signal would be a worse trade.
     */
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
            // Acquire-load: Commit may bump this from a worker. On x86 it still compiles to the same
            // plain load, so the dense sequential scan this comment block is about is unchanged.
            if (Mesh.SyncedRenderDataVersion != Mesh.LoadRenderDataVersion())
            {
                Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);
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

            // The entity compare is the authoritative test, not the stamp. It catches an index recycled
            // WITHIN this drain, and it makes a wrapped stamp harmless.
            uint32 RecordIndex = ~0u;
            if (Mapped.Stamp == CoalesceStamp
                && Mapped.Index < (uint32)CoalescedScratch.size()
                && CoalescedScratch[Mapped.Index].Entity == entt::to_integral(Entry.Entity))
            {
                RecordIndex = Mapped.Index;
            }

            if (RecordIndex == ~0u)
            {
                // A recycled index appends a SECOND record rather than overwriting the first. The dead
                // entity's Membership still has to be applied, or its primitive outlives it pointing at
                // an entity that no longer exists.
                RecordIndex = (uint32)CoalescedScratch.size();
                CoalescedScratch.emplace_back().Entity = entt::to_integral(Entry.Entity);
                Mapped.Stamp = CoalesceStamp;
                Mapped.Index = RecordIndex;
            }

            FCoalescedEntity& Record = CoalescedScratch[RecordIndex];

            if (Entry.Source == EPrimitiveSource::Any)
            {
                Record.Flags[(uint32)EPrimitiveSource::StaticMesh]   |= Entry.Flags;
                Record.Flags[(uint32)EPrimitiveSource::DynamicMesh]  |= Entry.Flags;
                Record.Flags[(uint32)EPrimitiveSource::SkeletalMesh] |= Entry.Flags;

                // A foliage instance bakes its own world transform rather than following the owning
                // entity's, so a transform-only mark must never reach it. Letting one through would turn
                // every moving entity in the world into a full rebake sweep, which is O(baked instances).
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

    /**
     * Grows a vector to hold at least Target elements while KEEPING geometric growth.
     *
     * eastl::vector::reserve(n) allocates exactly n. Calling it with (size + what this frame adds) is
     * therefore not a hint, it is a set_capacity: capacity comes back equal to size, so the NEXT frame
     * that adds a primitive reallocates again. A world gaining one mesh per frame -- streaming, a bulk
     * spawn spread over frames, the editor dropping actors in -- reallocated and moved every parallel
     * array in the set EVERY frame, at 144 bytes per FGPUInstance and 160 per FScenePrimitive, and each
     * of those reallocations also forced a full retained-instance re-upload. Overshooting is what makes
     * the amortization real.
     */
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

    /**
     * Grows every array the apply pass appends to, once, before any of it runs.
     *
     * Without this a bulk add walks roughly fourteen doublings of arrays whose elements are 144 bytes
     * (FGPUInstance) and 160 (FScenePrimitive), and each of those reallocations sets bFullInstanceUpload
     * in AllocateInstanceSlot, so the whole retained buffer is re-sent that frame.
     *
     * Over-counting is harmless: Transform alone can never create a primitive, so it is excluded, but a
     * Membership mark for a component that has since been destroyed still counts here and simply reserves
     * a slot nothing uses.
     */
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

        // One binding and one instance slot per new primitive is the floor, not the truth -- a multi-surface
        // mesh takes more. Geometric growth covers the rest; the point is to skip the doublings.
        ReserveGeometric(Bindings, Bindings.size() + NewPrimitives);

        // Only slots that cannot be served from the free list grow the array. Counting the whole add here
        // made a scene that constantly swaps meshes grow its slot space without bound, and every one of
        // those growths is a full retained re-upload.
        const SIZE_T FreeSlots         = InstanceFreeSlots.size();
        const SIZE_T NeededSlots       = NewPrimitives > FreeSlots ? NewPrimitives - FreeSlots : 0;
        const SIZE_T InstanceTarget    = RetainedCullEntries.size() + NeededSlots;
        const SIZE_T OldSlotCapacity   = RetainedCullEntries.capacity();
        ReserveGeometric(RetainedCullEntries, InstanceTarget);
        ReserveGeometric(RetainedTransforms, InstanceTarget);
        ReserveGeometric(RetainedStatic, InstanceTarget);

        // Same conclusion AllocateInstanceSlot draws when it grows: the slot array moved, so the device
        // copy has to be re-sent wholesale rather than patched. Drawn once here instead of per slot.
        if (RetainedCullEntries.capacity() != OldSlotCapacity)
        {
            bFullInstanceUpload = true;
        }
    }

    // See the header for why this split is sound. The short version: Transform is the only flag that can
    // neither create nor destroy a primitive.
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

            // CoalesceDrain never lets a transform-only mark reach the foliage slot, so All == Transform
            // also means this record has no foliage work -- which the parallel body could not do anyway.
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

            // Gated on the entity actually being foliage. An `Any` mark carries no source, so the old path
            // ran the whole foliage reconcile for every entity that was enabled, disabled or respawned.
            // FoliageInstanceCount is the second half of the test: a destroyed component is gone from the
            // pool, and SyncFoliage is what has to run to reclaim the primitives it left behind.
            const EPrimitiveDirty FoliageFlags = Record.Flags[(uint32)EPrimitiveSource::Foliage];
            if (FoliageFlags != EPrimitiveDirty::None
                && (Pools.Foliage->contains(Entity)
                    || (!FoliageInstanceCount.empty() && FoliageInstanceCount.find(Entity) != FoliageInstanceCount.end())))
            {
                SyncFoliage(Registry, Pools, Entity, FoliageFlags);
            }
        }

        // Same threshold BindSurfaces uses, checked once for the whole pass. Without it a mass despawn
        // left the dead share high until the next bind happened to come along -- and until then every
        // RefreshInstances streamed a Bindings array mostly full of holes.
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

        // One flat-array load, and it is the single hottest line in the pass: every moving entity in the
        // world lands here once per frame and most are not renderable at all, so "0, skip" must be cheap.
        const uint8 Mask = GetSourceMask(Entity);
        if (Mask == 0)
        {
            return;
        }

        // Marks are produced concurrently and drained in arbitrary order, so a move can be processed after
        // its entity was destroyed. Reading a pool that no longer contains it is UB.
        if (!Pools.Transform->contains(Entity))
        {
            return;
        }

        const FMatrix4& Matrix = ReadRenderMatrix(Pools.RenderXform, Entity, Pools.Transform->get(Entity));

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

        // Decided BEFORE the merge, on the combined count. Past a quarter of the slot space the whole
        // buffer is re-sent anyway, so copying the per-worker lists into a list about to be discarded is
        // pure waste -- which on a frame where the whole crowd moved is every frame.
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

        // One bump for the batch. The value only has to CHANGE, and a shared counter incremented from
        // every worker would be a data race for no benefit.
        ++StructureGeneration;

        // Below this the dispatch costs more than the work: the body is ~100ns per record against a
        // ParallelFor round trip measured in tens of microseconds.
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

        // Reset ahead of the early-out, so a frame that does nothing reports zeros rather than last
        // frame's numbers.
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

            // Structural FIRST, and serially. It appends and swap-removes, which renumbers the very
            // indices the transform pass is about to resolve through the link table.
            ApplyStructuralRecords(Registry, Pools);

            ApplyTransformRecords(Pools);
        }

        if (bResolveTableChanged)
        {
            LUMINA_PROFILE_SECTION("Resolve Mesh Cache");
            bResolveTableChanged = false;

            FMeshResolveCache& Cache = FMeshResolveCache::Get();
            RetryScratch.clear();

            // Entries are heap-boxed so growth never moves one a frame holds, which makes every GetEntry a
            // dependent load into cold memory. At one per primitive that WAS this sweep's cost, and the
            // sweep re-runs every frame an asset is still streaming in. Snapshot instead: one deref per
            // distinct mesh (tens to hundreds) against a table small enough to stay resident.
            GenSnapshot.assign(Cache.NumEntries(), ~0u);

            for (uint32 i = 0, N = (uint32)Primitives.size(); i < N; ++i)
            {
                const uint64 Packed = ResolveKeys[i];
                const uint32 Handle = (uint32)(Packed & 0xFFFFFFFFull);

                // Covers INVALID_MESH_RESOLVE_HANDLE and anything past the table, so it subsumes the
                // IsValidHandle check this loop used to make.
                if (Handle >= (uint32)GenSnapshot.size())
                {
                    continue;
                }

                // The mirror is maintained by hand in four places. Drift would be silent -- the sweep would
                // re-resolve the wrong primitive forever -- so it is worth catching in development.
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
                    // The (entity, source) half of the key, dropping the sub-index. What gets re-synced is
                    // an ENTITY, and SyncFoliage resyncs every instance its entity owns -- so one entry per
                    // stale primitive meant a foliage entity re-synced once per stale blade, each pass
                    // walking all of them. That is O(instances^2), and a single mesh finishing its upload
                    // is enough to trigger it across the whole painted set.
                    RetryScratch.push_back(Keys[i] & kSyncTargetMask);
                }
            }

            // Sorted-then-compacted rather than probed into a set: the input is one entry per stale
            // primitive (hundreds of thousands under foliage) and the output is one per entity (a
            // handful), so an O(n log n) pass with no allocation beats n set insertions.
            eastl::sort(RetryScratch.begin(), RetryScratch.end());
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

            // Entity and source are unpacked from the key rather than read back off the primitive: the
            // refresh below can swap-remove and reorder Primitives, which is why the sweep collects first
            // in the original -- and this drops the FindPrimitive probe that re-derived them.
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
        // Only the transform half runs wide. If a slow Sync/Apply is mostly Structural, threading is not
        // the lever -- the work there mutates the shared index tables and has to stay serial.
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

    void FScenePrimitiveSet::Reset(FEntityRegistry* Registry)
    {
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        ResolveKeys.clear();
        IndexByKey.clear();
        Bindings.clear();
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
        // Dropped with the table it summarizes; carrying a bound for descs that are gone would leave the
        // cull's dispatch ceiling permanently wider than the scene.
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
