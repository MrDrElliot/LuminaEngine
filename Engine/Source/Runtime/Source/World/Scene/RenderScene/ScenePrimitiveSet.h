#pragma once
#include "Containers/Array.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    class CWorld;
    struct SFoliageComponent;
    struct FDynamicMeshRenderData;

    enum class EPrimitiveSource : uint8
    {
        StaticMesh,
        DynamicMesh,
        SkeletalMesh,
        Foliage,
        Num,

        Any = 0xFF,
    };

    enum class EPrimitiveDirty : uint32
    {
        None       = 0,
        // World matrix moved: refresh Transform + the world cull sphere. Nothing else.
        Transform  = 1u << 0,
        Data       = 1u << 1,
        // SDisabledTag added or removed: membership only.
        Visibility = 1u << 2,
        // Component or entity added or destroyed: insert or remove the primitive outright.
        Membership = 1u << 3,

        All        = Transform | Data | Visibility | Membership,
    };
    ENUM_CLASS_FLAGS(EPrimitiveDirty)

    class RUNTIME_API FRenderDirtyTracker
    {
    public:

        struct FEntry
        {
            entt::entity        Entity;
            EPrimitiveSource    Source;
            EPrimitiveDirty     Flags;
        };

        // Creates the tracker in the registry context on first call. Game thread.
        static FRenderDirtyTracker& Ensure(FEntityRegistry& Registry);
        static FRenderDirtyTracker* Find(FEntityRegistry& Registry);

        FRenderDirtyTracker();
        ~FRenderDirtyTracker();
        LE_NO_COPYMOVE(FRenderDirtyTracker);

        // Any thread.
        void Mark(entt::entity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags);

        void MarkAllSources(entt::entity Entity, EPrimitiveDirty Flags);

        // One relaxed load on a frame where nothing changed.
        bool HasPending() const { return bAnyDirty.load(std::memory_order_acquire) || bFullRescan; }

        // Game thread. Appends to Out; returns true if anything was drained.
        bool Drain(TVector<FEntry>& Out);

        void RequestFullRescan()
        {
            bFullRescan = true;
            bAnyDirty.store(true, std::memory_order_release);
        }
        bool ConsumeFullRescan()
        {
            const bool bWas = bFullRescan;
            bFullRescan = false;
            return bWas;
        }

    private:

        struct FImpl;
        FImpl*              Impl = nullptr;
        std::atomic<bool>   bAnyDirty{ true };
        bool                bFullRescan = true;
    };

    class FSceneBatchRegistry
    {
    public:

        struct FDeferredMaterialSlot
        {
            uint16              MaterialIndex;
            const FShaderEntry* DeferredShader;
        };

        struct FBatch
        {
            FDrawBatchKey                   Key = {};
            // Copied out of the resolve at bind time so the frame path never re-reads the shared entry.
            const FShaderEntry*             PixelShader = nullptr;
            const FShaderEntry*             VertexShader = nullptr;
            const FShaderEntry*             MeshShaderShadow = nullptr;
            const FShaderEntry*             MeshShaderBase = nullptr;
            const FShaderEntry*             VisBufferMeshShader = nullptr;
            const FShaderEntry*             VisBufferMeshShaderMasked = nullptr;
            const FShaderEntry*             MaskedVisBufferPixelShader = nullptr;
            const FShaderEntry*             DeferredShader = nullptr;
            uint16                          MaterialIdx = 0;
            bool                            bMaterialCastsShadows = true;

            uint32                          RefCount = 0;

            TVector<FDeferredMaterialSlot>  DeferredMaterials;
        };

        uint32 FindOrAddBatch(const FResolvedSurface& Surface);

        void AddBatchRef(uint32 BatchIndex);

        void ReleaseBatchRef(uint32 BatchIndex);

        uint32          Num() const { return (uint32)Batches.size(); }
        FBatch&         Get(uint32 Index) { return Batches[Index]; }
        const FBatch&   Get(uint32 Index) const { return Batches[Index]; }

        uint64          GetLayoutGeneration() const { return LayoutGeneration; }

        void Reset();

    private:

        TVector<FBatch>                     Batches;
        THashMap<uint64, TVector<uint32>>   BatchesByHash;
        uint64                              LayoutGeneration = 1;
    };

    /** Where one surface of a primitive draws: resolved once at bind, read every frame. */
    struct FSurfaceBinding
    {
        uint32          BatchIndex;
        uint32          InstanceSlot;
        uint32          SurfaceDescIndex;   // interned LOD table
        uint16          MaterialIndex;
        EInstanceFlags  MaterialFlags;
        bool            bMaterialCastsShadows;
    };

    struct FScenePrimitive
    {
        FMatrix4                            Transform = FMatrix4(1.0f);
        uint64                              MeshletHeaderAddress = 0;

        const TVector<FResolvedSurface>*    Surfaces = nullptr;

        uint32                              BindingBase = 0;    // span into FScenePrimitiveSet::Bindings
        uint32                              SurfaceCount = 0;

        entt::entity                        Entity = entt::null;
        uint32                              EntityID = 0;
        uint32                              CustomData = 0;

        TSharedPtr<FDynamicMeshRenderData>  DynamicRenderData;

        uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
        uint32                              ResolveGeneration = 0;

        FVector3                            LocalCenter = FVector3(0.0f);
        float                               LocalRadius = 0.0f;
        float                               BoundsScale = 1.0f;

        int32                               ForcedLODIndex = -1;
        EInstanceFlags                      BaseFlags = EInstanceFlags::None;
        EPrimitiveSource                    Source = EPrimitiveSource::StaticMesh;
        bool                                bCastShadow = true;
    };

    /** Cull-hot per-primitive data, kept in its own array so the reject path streams it densely. */
    struct FPrimitiveCullData
    {
        float   MaxDrawDistance = 0.0f;
        uint32  bCastShadow = 0u;
    };

    class FScenePrimitiveSet
    {
    public:

        FScenePrimitiveSet() = default;
        LE_NO_COPYMOVE(FScenePrimitiveSet);

        // Drains the dirty channel and applies it. O(changed), except on a requested full rescan.
        void Sync(CWorld& World);

        // Drops every primitive and re-arms a full rescan. World teardown / scene rebuild.
        void Reset(FEntityRegistry* Registry);

        uint32                              Num() const { return (uint32)Primitives.size(); }
        const FScenePrimitive*              GetPrimitives() const { return Primitives.data(); }
        const FVector4*                     GetBounds() const { return Bounds.data(); }
        const FPrimitiveCullData*           GetCullData() const { return CullData.data(); }
        const FSurfaceBinding*              GetBindings() const { return Bindings.data(); }

        FSceneBatchRegistry&                GetBatches() { return Batches; }
        const FSceneBatchRegistry&          GetBatches() const { return Batches; }

        uint64                              GetStructureGeneration() const { return StructureGeneration; }

        uint32                              GetSkinnedPrimitiveCount() const { return SkinnedCount; }

        EPrimitiveSource                    GetSource(uint32 Index) const
        {
            return (EPrimitiveSource)((Keys[Index] >> kKeySourceShift) & 0xFFull);
        }

        const FInstanceCullEntry*   GetRetainedCullEntries() const { return RetainedCullEntries.data(); }
        const FTransform3x4*        GetRetainedTransforms() const { return RetainedTransforms.data(); }
        const FInstanceStatic*      GetRetainedStatic() const     { return RetainedStatic.data(); }
        uint32                      GetRetainedSlotCount() const  { return (uint32)RetainedCullEntries.size(); }

        const FSurfaceDescGPU*      GetSurfaceDescs() const     { return SurfaceDescs.data(); }
        uint32                      GetSurfaceDescCount() const { return (uint32)SurfaceDescs.size(); }
        bool                        AreSurfaceDescsDirty() const { return bSurfaceDescsDirty; }
        void                        ClearSurfaceDescsDirty()     { bSurfaceDescsDirty = false; }

        uint32                      GetMaxSurfaceDescMeshlets() const { return MaxSurfaceDescMeshlets; }

        // Exact upper bound on the cull blocks ONE view can produce from the retained set. Sizing the block
        // list from this instead of a lagged readback is what makes block overflow impossible: the readback
        // could not see a spike until kFramesInFlight frames after it happened, and until then the builder
        // appended against a capacity that no longer described the scene.
        uint32                      GetWorstCaseBlocksPerView() const { return WorstCaseBlocksPerView; }

        const TVector<uint32>&      GetDirtyInstanceSlots() const { return DirtyInstanceSlots; }
        const TVector<uint32>&      GetDirtyStaticSlots() const   { return DirtyStaticSlots; }
        void                        ClearDirtyInstanceSlots()     { DirtyInstanceSlots.clear(); DirtyStaticSlots.clear(); }
        // True when the slot array itself was reallocated, so the whole thing has to be re-sent.
        bool                        NeedsFullInstanceUpload() const { return bFullInstanceUpload; }
        void                        ClearFullInstanceUpload()       { bFullInstanceUpload = false; }

        void                                NotifyResolveTableChanged() { bResolveTableChanged = true; }

    private:

        static constexpr uint32 kKeySourceShift   = 32;
        static constexpr uint32 kKeySubIndexShift = 40;

        static uint64 MakeKey(entt::entity Entity, EPrimitiveSource Source, uint32 SubIndex = 0)
        {
            return (uint64)entt::to_integral(Entity)
                 | ((uint64)Source << kKeySourceShift)
                 | ((uint64)SubIndex << kKeySubIndexShift);
        }

        static constexpr uint64 kSyncTargetMask = (1ull << kKeySubIndexShift) - 1ull;

        uint32  FindPrimitive(uint64 Key) const;
        uint32  AddPrimitive(uint64 Key);
        void    RemovePrimitive(uint64 Key);
        void    RemoveEntity(FEntityRegistry& Registry, entt::entity Entity, EPrimitiveSource Source);

        struct FSyncPools;
        struct FBindingMemo;

        bool    RefreshPrimitiveData(const FSyncPools& Pools, uint32 Index);

        // Binds one primitive's surfaces to the batch registry. Only called when its resolve changed.
        void    BindSurfaces(uint32 Index);
        void    ReleaseBindings(uint32 Index);

        const FBindingMemo* EnsureBindingMemo(uint32 ResolveHandle, uint32 Generation,
                                              const TVector<FResolvedSurface>& Surfaces);

        bool    BindingsMatchMemo(uint32 Index, const FBindingMemo& Memo) const;

        void    RefreshInstances(uint32 Index, TVector<uint32>* DirtySink = nullptr);

        void    RefreshInstanceTransform(uint32 Index, TVector<uint32>* DirtySink = nullptr);

        uint32  AllocateInstanceSlot();
        void    FreeInstanceSlot(uint32 Slot);
        void    MarkInstanceDirty(uint32 Slot);
        void    MarkStaticDirty(uint32 Slot);
        // Interns a surface's LOD table; identical tables collapse to one entry.
        uint32  InternSurfaceDesc(const FResolvedSurface& Surface);

        void    SyncEntity(FEntityRegistry& Registry, const FSyncPools& Pools, entt::entity Entity,
                           EPrimitiveSource Source, EPrimitiveDirty Flags);
        void    SyncFoliage(FEntityRegistry& Registry, const FSyncPools& Pools, entt::entity Entity,
                            EPrimitiveDirty Flags);
        void    FullRescan(FEntityRegistry& Registry);
        void    PollUnhookedSources(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker);

        void    RebuildWorldBounds(uint32 Index);

        TVector<FScenePrimitive>            Primitives;
        TVector<FVector4>                   Bounds;
        TVector<FPrimitiveCullData>         CullData;
        TVector<uint64>                     Keys;           // parallel: index -> key, for swap-remove fixup

        THashMap<uint64, uint32>            IndexByKey;

        TVector<uint64>                     ResolveKeys;

        static uint64 PackResolveKey(uint32 Handle, uint32 Generation)
        {
            return (uint64)Handle | ((uint64)Generation << 32);
        }

        // Scratch: resolve handle -> that entry's current Generation, filled lazily during the sweep.
        TVector<uint32>                     GenSnapshot;

        TVector<FSurfaceBinding>            Bindings;
        uint32                              DeadBindings = 0;
        void                                CompactBindings();

        FSceneBatchRegistry                 Batches;

        struct FBindingMemo
        {
            uint32                      Generation = ~0u;   // FResolvedMesh::Generation it was built at
            TVector<FSurfaceBinding>    Protos;             // InstanceSlot unused
        };
        TVector<FBindingMemo>               BindingMemoByHandle;

        TVector<uint64>                     RetryScratch;

        static constexpr uint32 kLinkedSources = (uint32)EPrimitiveSource::Foliage;

        struct FPrimitiveLink
        {
            uint32 Entity = ~0u;
            uint32 Index[kLinkedSources] = { ~0u, ~0u, ~0u };
        };
        static_assert(sizeof(FPrimitiveLink) == 16, "Four links per cache line is the point of this table.");
        static_assert(kLinkedSources == 3, "FPrimitiveLink::Index initializer must match kLinkedSources.");

        TVector<FPrimitiveLink>             LinksByEntityIndex;

        // ~0u when this entity owns no primitive for that source. Source must not be Foliage.
        uint32  FindLinked(entt::entity Entity, EPrimitiveSource Source) const;
        // Bitmask of sources this entity owns a primitive for; 0 when it is not renderable at all.
        uint8   GetSourceMask(entt::entity Entity) const;
        void    SetLink(entt::entity Entity, EPrimitiveSource Source, uint32 Index);
        void    ClearLink(entt::entity Entity, EPrimitiveSource Source);

        THashMap<entt::entity, uint32>      FoliageInstanceCount;

        struct FFoliageTypeResolve
        {
            const TVector<FResolvedSurface>*    Surfaces = nullptr;
            uint32                              Generation = 0;
            uint64                              MeshletHeaderAddress = 0;
            uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
            float                               MaxDrawDistance = 0.0f;
            EInstanceFlags                      BaseFlags = EInstanceFlags::None;
            bool                                bCastShadow = true;
        };
        TVector<FFoliageTypeResolve>        FoliageTypeScratch;

        TVector<FRenderDirtyTracker::FEntry> DrainScratch;
        TVector<CMaterialInterface*>        OverrideScratch;

        struct FCoalescedEntity
        {
            uint32          Entity = ~0u;
            EPrimitiveDirty Flags[(uint32)EPrimitiveSource::Num] = {};
        };

        struct FCoalesceSlot
        {
            uint32 Stamp = 0;
            uint32 Index = ~0u;
        };

        TVector<FCoalescedEntity>   CoalescedScratch;
        TVector<FCoalesceSlot>      CoalesceByEntityIndex;
        uint32                      CoalesceStamp = 0;

        // Folds DrainScratch into CoalescedScratch.
        void CoalesceDrain();

        // Grows the append-target arrays once, from the coalesced record set, before the apply pass runs.
        void ReserveForDrain();

        void PartitionDrain();
        void ApplyStructuralRecords(FEntityRegistry& Registry, const FSyncPools& Pools);
        void ApplyTransformRecords(const FSyncPools& Pools);

        void ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex, TVector<uint32>* OutDirty);

        // Indices into CoalescedScratch, filled by PartitionDrain.
        TVector<uint32>             TransformRecords;
        TVector<uint32>             StructuralRecords;

        TVector<TVector<uint32>>    ParallelDirtySlots;

        // Folds the per-worker lists back into DirtyInstanceSlots, or gives up and re-sends everything.
        void MergeParallelDirtySlots();

        TVector<FInstanceCullEntry>         RetainedCullEntries;
        TVector<FTransform3x4>              RetainedTransforms;
        TVector<FInstanceStatic>            RetainedStatic;

        TVector<uint32>                     InstanceFreeSlots;
        TVector<uint32>                     DirtyInstanceSlots;   // cull entry + transform
        TVector<uint32>                     DirtyStaticSlots;     // static payload
        bool                                bFullInstanceUpload = true;

        TVector<FSurfaceDescGPU>            SurfaceDescs;
        THashMap<uint64, TVector<uint32>>   SurfaceDescByHash;
        bool                                bSurfaceDescsDirty = true;
        // See GetMaxSurfaceDescMeshlets. Only ever grows while the table does, and is reset with it.
        uint32                              MaxSurfaceDescMeshlets = 0;

        void                                RefreshWorstCaseBlocks();
        uint32                              WorstCaseBlocksPerView = 0;
        // Recompute is keyed on the structure generation, so a moving scene never pays for it.
        uint64                              WorstCaseBlocksGeneration = 0;

        uint64                              StructureGeneration = 1;
        uint32                              SkinnedCount = 0;
        bool                                bResolveTableChanged = false;

        struct FSyncStats
        {
            uint32 DrainEntries         = 0;
            uint32 CoalescedEntities    = 0;
            uint32 SyncEntityCalls      = 0;
            uint32 SyncFoliageCalls     = 0;
            uint32 BindCalls            = 0;
            uint32 BindsSkipped         = 0;
            uint32 RefreshInstanceCalls = 0;
            uint32 TransformRecords     = 0;
            uint32 StructuralRecords    = 0;
        };
        FSyncStats  SyncStats;
        void        PublishSyncStats() const;
    };
}
