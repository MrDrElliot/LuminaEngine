#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Memory/MemoryConcurrentQueue.h"
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
        return (uint64)entt::to_integral(Entity)
             | ((uint64)Source << 32)
             | ((uint64)Flags  << 40);
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

    uint32 FScenePrimitiveSet::FindPrimitive(uint64 Key) const
    {
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
        IndexByKey.emplace(Key, Index);

        // Foliage is deliberately absent from the link table: it is only read to route transform marks,
        // and a foliage instance bakes its own world transform rather than following the owning entity's.
        // It also owns many primitives per entity, which the one-slot-per-source table cannot express.
        const entt::entity     Entity = (entt::entity)(uint32)(Key & 0xFFFFFFFFull);
        const EPrimitiveSource Source = (EPrimitiveSource)((Key >> 32) & 0xFFull);
        if (Source != EPrimitiveSource::Foliage)
        {
            SetLink(Entity, Source, Index);
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
        auto It = IndexByKey.find(Key);
        if (It == IndexByKey.end())
        {
            return;
        }

        const uint32 Index = It->second;
        const uint32 Last  = (uint32)Primitives.size() - 1u;

        // Hand this primitive's draw slots back before the entry is overwritten.
        ReleaseBindings(Index);

        // Swap-with-last keeps the arrays dense so the per-frame cull never walks holes. The moved
        // entry's map slot is the only fixup; its own key comes from the parallel Keys array.
        if (Index != Last)
        {
            Primitives[Index] = Primitives[Last];
            Bounds[Index]     = Bounds[Last];
            CullData[Index]   = CullData[Last];
            Keys[Index]       = Keys[Last];
            IndexByKey[Keys[Index]] = Index;

            // The moved entry's link has to follow it. Its own key carries the (entity, source) pair, so
            // this needs no lookup -- and skipping it would leave that entity pointing at the slot this
            // primitive just vacated, which is the classic swap-remove aliasing bug.
            const entt::entity     MovedEntity = (entt::entity)(uint32)(Keys[Index] & 0xFFFFFFFFull);
            const EPrimitiveSource MovedSource = (EPrimitiveSource)((Keys[Index] >> 32) & 0xFFull);
            if (MovedSource != EPrimitiveSource::Foliage)
            {
                SetLink(MovedEntity, MovedSource, Index);
            }
        }

        Primitives.pop_back();
        Bounds.pop_back();
        CullData.pop_back();
        Keys.pop_back();
        IndexByKey.erase(It);

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

        const uint32 Slot = (uint32)RetainedInstances.size();
        const SIZE_T OldCapacity = RetainedInstances.capacity();

        RetainedInstances.emplace_back();
        RetainedCullData.emplace_back();

        // A reallocation moves every slot, so the device copy has to be re-sent wholesale rather than
        // patched. Growth is amortized, so this is rare after the scene settles.
        if (RetainedInstances.capacity() != OldCapacity)
        {
            bFullInstanceUpload = true;
        }
        return Slot;
    }

    void FScenePrimitiveSet::FreeInstanceSlot(uint32 Slot)
    {
        if (Slot >= (uint32)RetainedCullData.size())
        {
            return;
        }

        // bActive is what the cull reads; the instance beside it is left stale on purpose, because
        // clearing 144 bytes to say "ignore me" would be pure waste.
        RetainedCullData[Slot].bActive = 0u;
        MarkInstanceDirty(Slot);
        InstanceFreeSlots.push_back(Slot);
    }

    void FScenePrimitiveSet::MarkInstanceDirty(uint32 Slot)
    {
        // Duplicates are fine: the upload coalesces, and de-duplicating here would cost a set lookup on
        // the hot path to save a repeated 144-byte copy.
        DirtyInstanceSlots.push_back(Slot);
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
        return NewIndex;
    }

    // Writes everything the GPU cull reads about one primitive into its retained slots. This is the only
    // place instance data is produced -- there is no per-frame equivalent.
    void FScenePrimitiveSet::RefreshInstances(uint32 Index)
    {
        const FScenePrimitive&    Prim = Primitives[Index];
        const FPrimitiveCullData& Cull = CullData[Index];
        const FVector4&           Sphere = Bounds[Index];

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[Prim.BindingBase + s];
            const uint32 Slot = Binding.InstanceSlot;
            if (Slot >= (uint32)RetainedInstances.size())
            {
                continue;
            }

            EInstanceFlags Flags = Prim.BaseFlags | Binding.MaterialFlags;
            if (Prim.bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
            }

            FGPUInstance& Out = RetainedInstances[Slot];
            Out.Transform            = Prim.Transform;
            Out.SphereBounds         = Sphere;
            Out.MeshletHeaderAddress = Prim.MeshletHeaderAddress;
            Out.DrawIDAndFlags       = PackDrawIDAndFlags(Binding.BatchIndex, Flags);
            Out.CustomData           = Prim.CustomData;
            Out.MaterialIndex        = Binding.MaterialIndex;
            Out.EntityID             = Prim.EntityID;
            Out.BoneOffset           = 0u;
            // LOD-dependent fields are resolved by CullInstances; seeding them keeps the retained copy
            // readable in a capture rather than full of garbage.
            Out.SurfaceMeshletOffset = 0u;
            Out.SurfaceMeshletCount  = 0u;
            Out.ShadowMeshletOffset  = 0u;
            Out.ShadowMeshletCount   = 0u;
            Out.SkinnedVertexBase    = 0u;
            Out.ShadowSkinnedVertexBase = 0u;

            FInstanceCullData& OutCull = RetainedCullData[Slot];
            OutCull.SurfaceDescIndex = Binding.SurfaceDescIndex;
            OutCull.MaxDrawDistance  = Cull.MaxDrawDistance;
            OutCull.ForcedLODIndex   = Prim.ForcedLODIndex;
            // Skeletal primitives are CPU-fed every frame (bones + pre-skin bases have no retained
            // form); letting the GPU cull append this slot too would draw a second copy whose
            // SkinnedVertexBase of 0 reads someone else's pre-skin slice.
            OutCull.bActive          = (Prim.Surfaces != nullptr && Prim.Source != EPrimitiveSource::SkeletalMesh) ? 1u : 0u;

            MarkInstanceDirty(Slot);
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

    void FScenePrimitiveSet::BindSurfaces(uint32 Index)
    {
        ReleaseBindings(Index);

        FScenePrimitive& Prim = Primitives[Index];

        if (Prim.Surfaces == nullptr || Prim.Surfaces->empty())
        {
            Prim.BindingBase  = 0;
            Prim.SurfaceCount = 0;
            return;
        }

        const TVector<FResolvedSurface>& Surfaces = *Prim.Surfaces;

        Prim.BindingBase  = (uint32)Bindings.size();
        Prim.SurfaceCount = (uint32)Surfaces.size();

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
    static const FMatrix4& ReadRenderMatrix(FEntityRegistry& Registry, entt::entity Entity, const STransformComponent& Transform)
    {
        const auto& RenderStorage = Registry.storage<FRenderTransform>();
        return RenderStorage.contains(Entity) ? RenderStorage.get(Entity).Matrix : Transform.CachedMatrix;
    }

    void FScenePrimitiveSet::RefreshTransform(FEntityRegistry& Registry, uint32 Index)
    {
        FScenePrimitive& Prim = Primitives[Index];

        // Foliage bakes its world transform into the instance; it has no transform component of its own,
        // and a change to the owning entity's transform re-bakes (and so re-adds) every instance.
        if (Prim.Source == EPrimitiveSource::Foliage)
        {
            return;
        }

        auto& TransformStorage = Registry.storage<STransformComponent>();
        if (!TransformStorage.contains(Prim.Entity))
        {
            return;
        }

        Prim.Transform = ReadRenderMatrix(Registry, Prim.Entity, TransformStorage.get(Prim.Entity));
        RebuildWorldBounds(Index);
        ++StructureGeneration;
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

    bool FScenePrimitiveSet::RefreshPrimitiveData(FEntityRegistry& Registry, uint32 Index)
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
                    const SStaticMeshComponent* C = Registry.try_get<SStaticMeshComponent>(Prim.Entity);
                    if (C == nullptr) { return false; }
                    ReadCommonMeshState(Prim, Cull, *C);
                    Base     = C;
                    LiveMesh = (const void*)C->StaticMesh.Get();
                }
                else
                {
                    const SSkeletalMeshComponent* C = Registry.try_get<SSkeletalMeshComponent>(Prim.Entity);
                    if (C == nullptr) { return false; }
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

                FMeshResolveCache& Cache = FMeshResolveCache::Get();
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
                SDynamicMeshComponent* C = Registry.try_get<SDynamicMeshComponent>(Prim.Entity);
                if (C == nullptr) { return false; }
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

        RebuildWorldBounds(Index);
        RefreshInstances(Index);
        ++StructureGeneration;

        return bResolved;
    }

    // See the forward declaration in the header for why these are hoisted. Sources is indexed by
    // EPrimitiveSource, so it deliberately stops short of Foliage -- SyncEntity never handles that source.
    struct FScenePrimitiveSet::FSyncPools
    {
        entt::storage_type_t<STransformComponent>* Transform = nullptr;
        const entt::sparse_set*                    Disabled  = nullptr;
        const entt::sparse_set*                    Sources[kLinkedSources] = {};

        explicit FSyncPools(FEntityRegistry& Registry)
            : Transform(&Registry.storage<STransformComponent>())
            , Disabled(&Registry.storage<SDisabledTag>())
        {
            Sources[(uint32)EPrimitiveSource::StaticMesh]   = &Registry.storage<SStaticMeshComponent>();
            Sources[(uint32)EPrimitiveSource::DynamicMesh]  = &Registry.storage<SDynamicMeshComponent>();
            Sources[(uint32)EPrimitiveSource::SkeletalMesh] = &Registry.storage<SSkeletalMeshComponent>();
        }
    };

    void FScenePrimitiveSet::SyncEntity(FEntityRegistry& Registry, const FSyncPools& Pools, entt::entity Entity,
                                        EPrimitiveSource Source, EPrimitiveDirty Flags)
    {
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

            Primitives[Index].Transform = ReadRenderMatrix(Registry, Entity, Pools.Transform->get(Entity));
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
            Primitives[Index].Transform = ReadRenderMatrix(Registry, Entity, Pools.Transform->get(Entity));
        }

        if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Data | EPrimitiveDirty::Membership | EPrimitiveDirty::Visibility))
        {
            RefreshPrimitiveData(Registry, Index);
        }
        else if (EnumHasAnyFlags(Flags, EPrimitiveDirty::Transform))
        {
            // Transform-only: rebuild the world sphere and restamp this primitive's instance slots.
            RebuildWorldBounds(Index);
            RefreshInstances(Index);
            ++StructureGeneration;
        }

    }

    void FScenePrimitiveSet::SyncFoliage(FEntityRegistry& Registry, entt::entity Entity, EPrimitiveDirty Flags)
    {
        (void)Flags;

        SFoliageComponent* Foliage = Registry.valid(Entity) ? Registry.try_get<SFoliageComponent>(Entity) : nullptr;
        const bool bEnabled = Foliage != nullptr && !Registry.any_of<SDisabledTag>(Entity);

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

            if (Foliage->IsValidType(Instance.TypeIndex))
            {
                const SFoliageType& Type = Foliage->Types[Instance.TypeIndex];

                Prim.MeshletHeaderAddress = Type.CachedMeshletHeaderAddress;
                Prim.BaseFlags            = Type.CachedBaseFlags;
                Prim.ResolveHandle        = Type.ResolveHandle;
                Prim.bCastShadow          = Type.bCastShadow;
                Prim.ForcedLODIndex       = -1;
                Cull.MaxDrawDistance      = Type.CullDistance;
                Cull.bCastShadow          = Type.bCastShadow ? 1u : 0u;

                FMeshResolveCache& Cache = FMeshResolveCache::Get();
                if (Type.ResolveHandle != INVALID_MESH_RESOLVE_HANDLE
                    && Cache.IsValidHandle(Type.ResolveHandle)
                    && Type.CachedMeshKey == (const void*)Type.Mesh.Get())
                {
                    const FResolvedMesh& Entry = Cache.GetEntry(Type.ResolveHandle);
                    if (Entry.bResolved)
                    {
                        NewSurfaces   = &Entry.Surfaces;
                        NewGeneration = Entry.Generation;
                    }
                }
            }

            const bool bSurfacesChanged = (NewSurfaces != Prim.Surfaces) || (NewGeneration != Prim.ResolveGeneration);
            Prim.Surfaces          = NewSurfaces;
            Prim.ResolveGeneration = NewGeneration;
            if (bSurfacesChanged)
            {
                BindSurfaces(Index);
            }

            Prim.Transform = Instance.Transform;
            Prim.EntityID  = entt::to_integral(Entity);
            Prim.CustomData = 0u;

            // The bake already produced the world sphere; there is no local sphere to transform.
            Bounds[Index] = Instance.SphereBounds;
            RefreshInstances(Index);
        }

        // No retry list for foliage: a type whose mesh is still loading leaves its CachedEpoch unstamped,
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
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        IndexByKey.clear();
        Bindings.clear();
        DeadBindings = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        RetainedInstances.clear();
        RetainedCullData.clear();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        bFullInstanceUpload = true;
        SurfaceDescs.clear();
        SurfaceDescByHash.clear();
        bSurfaceDescsDirty = true;
        FoliageInstanceCount.clear();

        const FSyncPools Pools(Registry);

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
            SyncFoliage(Registry, Entity, EPrimitiveDirty::All);
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
            // The component's own bake versioning is the change signal; bBakeIncomplete re-arms a bake
            // that hit a not-yet-loaded mesh.
            if (Foliage.BakedVersion != Foliage.InstancesVersion || Foliage.bBakeIncomplete)
            {
                Tracker.Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
            }
        }

        // Two adjacent uint32s on a densely iterated component: this whole loop is one sequential pass with
        // no dependent loads. It used to hash the entity into the primitive table, index Primitives[] by the
        // result and chase the component's TSharedPtr to the heap -- three effectively random accesses per
        // dynamic mesh, EVERY frame, before Sync's "nothing changed" early-out could even be reached. With a
        // few thousand of them (voxel chunks, procedural bodies) that poll was the frame's dominant cost and
        // it did no useful work on the frames it dominated.
        //
        // Commit() and ClearMesh() are the only writers of RenderData and both bump the version, so this is
        // exactly as sensitive as the pointer compare it replaces -- and strictly more so, because a
        // reallocated FDynamicMeshRenderData can land on the freed address of its predecessor and read as
        // unchanged. A never-synced component (version 1 vs synced 0) is caught here too, which is what
        // makes the removed Index == ~0u probe unnecessary.
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

    void FScenePrimitiveSet::Sync(CWorld& World)
    {
        FEntityRegistry&     Registry = ECS::GetWorldRegistry(World);
        FRenderDirtyTracker& Tracker  = FRenderDirtyTracker::Ensure(Registry);

        PollUnhookedSources(Registry, Tracker);

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
            return;
        }

        DrainScratch.clear();
        Tracker.Drain(DrainScratch);

        // Resolved once for the whole drain, not once per entity. See FSyncPools.
        const FSyncPools Pools(Registry);

        for (const FRenderDirtyTracker::FEntry& Entry : DrainScratch)
        {
            if (Entry.Source == EPrimitiveSource::Any)
            {
                if (Entry.Flags == EPrimitiveDirty::Transform)
                {
                    // One flat-array load. This is the single hottest line in the pass: every moving
                    // entity in the world lands here once per frame, and most of them are not renderable
                    // at all, so the common answer is "0, skip" and it must be cheap to reach.
                    const uint8 Mask = GetSourceMask(Entry.Entity);
                    if (Mask == 0u)
                    {
                        continue;
                    }
                    // Foliage bakes its own world transform per instance and does not follow the owning
                    // entity's, so it is intentionally not in this list.
                    if (Mask & (1u << (uint32)EPrimitiveSource::StaticMesh))   { SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::StaticMesh,   Entry.Flags); }
                    if (Mask & (1u << (uint32)EPrimitiveSource::DynamicMesh))  { SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::DynamicMesh,  Entry.Flags); }
                    if (Mask & (1u << (uint32)EPrimitiveSource::SkeletalMesh)) { SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::SkeletalMesh, Entry.Flags); }
                    continue;
                }

                SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::StaticMesh,   Entry.Flags);
                SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::DynamicMesh,  Entry.Flags);
                SyncEntity(Registry, Pools, Entry.Entity, EPrimitiveSource::SkeletalMesh, Entry.Flags);
                SyncFoliage(Registry, Entry.Entity, Entry.Flags);
            }
            else if (Entry.Source == EPrimitiveSource::Foliage)
            {
                SyncFoliage(Registry, Entry.Entity, Entry.Flags);
            }
            else
            {
                SyncEntity(Registry, Pools, Entry.Entity, Entry.Source, Entry.Flags);
            }
        }
        
        if (bResolveTableChanged)
        {
            bResolveTableChanged = false;

            FMeshResolveCache& Cache = FMeshResolveCache::Get();
            RetryScratch.clear();

            for (uint32 i = 0, N = (uint32)Primitives.size(); i < N; ++i)
            {
                const FScenePrimitive& Prim = Primitives[i];
                if (Prim.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE || !Cache.IsValidHandle(Prim.ResolveHandle))
                {
                    continue;
                }
                if (Cache.GetEntry(Prim.ResolveHandle).Generation != Prim.ResolveGeneration)
                {
                    RetryScratch.push_back(Keys[i]);
                }
            }

            // Collected first: the refresh below can swap-remove and reorder Primitives.
            for (uint64 Key : RetryScratch)
            {
                const uint32 Index = FindPrimitive(Key);
                if (Index == ~0u)
                {
                    continue;
                }
                const entt::entity     Entity = Primitives[Index].Entity;
                const EPrimitiveSource Source = Primitives[Index].Source;
                if (Source == EPrimitiveSource::Foliage)
                {
                    SyncFoliage(Registry, Entity, EPrimitiveDirty::Data);
                }
                else
                {
                    SyncEntity(Registry, Pools, Entity, Source, EPrimitiveDirty::Data);
                }
            }
        }

    }

    void FScenePrimitiveSet::Reset(FEntityRegistry* Registry)
    {
        Primitives.clear();
        Bounds.clear();
        CullData.clear();
        Keys.clear();
        IndexByKey.clear();
        Bindings.clear();
        DeadBindings = 0;
        LinksByEntityIndex.clear();
        SkinnedCount = 0;
        RetainedInstances.clear();
        RetainedCullData.clear();
        InstanceFreeSlots.clear();
        DirtyInstanceSlots.clear();
        bFullInstanceUpload = true;
        SurfaceDescs.clear();
        SurfaceDescByHash.clear();
        bSurfaceDescsDirty = true;
        FoliageInstanceCount.clear();
        Batches.Reset();
        bResolveTableChanged = false;
        ++StructureGeneration;

        if (Registry != nullptr)
        {
            FRenderDirtyTracker::Ensure(*Registry).RequestFullRescan();
        }
    }
}
