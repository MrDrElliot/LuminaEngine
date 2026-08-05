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

    // Which renderable component a primitive came from. Drives what the sync pass re-reads when the
    // primitive goes dirty, and which emit path the per-frame build uses (skinned takes the bone path).
    enum class EPrimitiveSource : uint8
    {
        StaticMesh,
        DynamicMesh,
        SkeletalMesh,
        Foliage,
        Num,

        // Marker for changes that aren't attributable to one component type (a transform moved, an
        // entity was disabled). The sync pass expands it and drops the sources the entity lacks, so
        // one queue entry covers the entity instead of one per source.
        Any = 0xFF,
    };

    // Render-relevant change classes. One bit per category so the sync pass does the minimum work for
    // what actually changed instead of rebuilding the primitive.
    enum class EPrimitiveDirty : uint32
    {
        None       = 0,
        // World matrix moved: refresh Transform + the world cull sphere. Nothing else.
        Transform  = 1u << 0,
        // Mesh / materials / render flags / LOD override changed: re-read the component and re-bind
        // its surfaces to the batch registry.
        Data       = 1u << 1,
        // SDisabledTag added or removed: membership only.
        Visibility = 1u << 2,
        // Component or entity added or destroyed: insert or remove the primitive outright.
        Membership = 1u << 3,

        All        = Transform | Data | Visibility | Membership,
    };
    ENUM_CLASS_FLAGS(EPrimitiveDirty)

    /**
     * Per-registry change channel the render scene consumes.
     *
     * Every mutation that can change what the renderer draws lands here as (entity, source, flags).
     * FScenePrimitiveSet::Sync drains it, so the steady-state cost of scene maintenance is proportional
     * to what changed, never to how many entities exist. The queue is lock-free because entt hooks and
     * the transform resolve's ParallelFor both publish into it.
     *
     * This is a routing channel, not a second source of truth: the authoritative dirty state still lives
     * where it always did (STransformComponent::bWorldDirty, FMeshResolveCache's epoch). This only carries
     * "who" so the render scene doesn't have to scan to find out.
     */
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

        // Marks the entity dirty for every source, for changes that aren't attributable to one component
        // type (transform moved, disabled toggled). The sync pass ignores sources the entity doesn't have.
        void MarkAllSources(entt::entity Entity, EPrimitiveDirty Flags);

        // One relaxed load on a frame where nothing changed.
        bool HasPending() const { return bAnyDirty.load(std::memory_order_acquire) || bFullRescan; }

        // Game thread. Appends to Out; returns true if anything was drained.
        bool Drain(TVector<FEntry>& Out);

        // Forces the next sync to walk every pool. Used on world load / primitive-set reset, where the
        // per-entity hooks either never fired or fired before the scene existed.
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

    /**
     * Batch + draw identity, resolved once per distinct (mesh, material assignment) instead of once per
     * entity per frame.
     *
     * Both keys are pure functions of FResolvedSurface: the batch key is the material (id + blend state),
     * the draw key is the geometry range. Neither depends on the entity, the camera or the LOD. So the
     * hash work the old gather paid per surface per frame per entity collapses to one lookup per surface
     * of each interned resolve entry, done at bind time.
     *
     * Slots are never recycled. Batches and draws number in the tens-to-hundreds even for large scenes,
     * and the per-frame compaction drops the empty ones, so there is nothing to reclaim.
     */
    class FSceneBatchRegistry
    {
    public:

        // A batch keys on the MASTER material, so instances of one master collapse into a single draw
        // and only one of their GPU material slots survives on FMeshDrawCommand. The deferred pass needs
        // all of them, so each batch keeps the distinct (slot, shader) pairs that bound into it.
        struct FDeferredMaterialSlot
        {
            uint16              MaterialIndex;
            const FShaderEntry* DeferredShader;
        };

        struct FBatch
        {
            FDrawBatchKey                   Key = {};
            // Copied out of the resolve at bind time so the frame path never re-reads the shared entry.
            const FShaderEntry*             VertexShader = nullptr;
            const FShaderEntry*             PixelShader = nullptr;
            const FShaderEntry*             MeshShader = nullptr;
            const FShaderEntry*             VisBufferMeshShader = nullptr;
            const FShaderEntry*             VisBufferVertexShader = nullptr;
            const FShaderEntry*             MaskedVisBufferPixelShader = nullptr;
            const FShaderEntry*             MaskedVisBufferPixelShaderPrim = nullptr;
            const FShaderEntry*             DeferredShader = nullptr;
            uint16                          MaterialIdx = 0;
            bool                            bMaterialCastsShadows = true;

            uint32                          RefCount = 0;

            // Distinct GPU material slots that share this batch's master. One entry per material
            // instance, so this stays in the low single digits per batch.
            TVector<FDeferredMaterialSlot>  DeferredMaterials;
        };

        // Returns the global batch index for this surface's material, creating it on first sight.
        // Also folds the surface's deferred material slot into the batch's list.
        uint32 FindOrAddBatch(const FResolvedSurface& Surface);

        // Takes a reference on a batch. There is no per-batch draw dimension: a draw is a PSO bucket, so
        // one batch is one draw. Nothing in the meshlet pipeline reads a draw's geometry range --
        // SeedIndirectArgs writes VertexCount = MESHLET_VERTICES_PER_DRAW for every draw and the
        // vertex/mesh shaders resolve geometry per meshlet through FGPUInstance::MeshletHeader. Keying
        // draws on geometry produced one slot per distinct mesh (per chunk in a streaming world) and that
        // slot space is the dimension of the merge counters and the NumViews*NumDraws indirect arg arrays.
        void AddBatchRef(uint32 BatchIndex);

        // Drops one reference taken by FindOrAddBatch / AddBatchRef. Batch slots are never recycled --
        // they number in the tens, and stable indices are what let DrawID be the batch index directly.
        void ReleaseBatchRef(uint32 BatchIndex);

        uint32          Num() const { return (uint32)Batches.size(); }
        FBatch&         Get(uint32 Index) { return Batches[Index]; }
        const FBatch&   Get(uint32 Index) const { return Batches[Index]; }

        // Bumped whenever a batch or a draw is added. The per-frame draw-argument layout is only
        // rederived when this moves (or when the visible set changes which of them are non-empty).
        uint64          GetLayoutGeneration() const { return LayoutGeneration; }

        void Reset();

    private:

        TVector<FBatch>                     Batches;
        // Hash -> candidate indices. A vector per bucket so a hash collision between distinct keys can
        // never merge two materials into one batch.
        THashMap<uint64, TVector<uint32>>   BatchesByHash;
        uint64                              LayoutGeneration = 1;
    };

    /** Where one surface of a primitive draws: resolved once at bind, read every frame. */
    struct FSurfaceBinding
    {
        uint32          BatchIndex;
        // Stable slot in the retained instance array, held for as long as this binding lives. This is
        // what makes the scene retained: the GPU cull reads the slot, and the CPU only writes it when
        // this primitive changes.
        uint32          InstanceSlot;
        uint32          SurfaceDescIndex;   // interned LOD table
        uint16          MaterialIndex;
        EInstanceFlags  MaterialFlags;
        bool            bMaterialCastsShadows;
    };

    /**
     * The camera-independent half of one renderable thing, held across frames.
     *
     * A primitive is one mesh component, or one baked foliage instance. Everything here is derived once
     * when the primitive is inserted or its dirty state fires -- never per frame. The per-frame build
     * reads it and adds only what the camera decides (cull result, LOD pick).
     */
    struct FScenePrimitive
    {
        FMatrix4                            Transform = FMatrix4(1.0f);
        uint64                              MeshletHeaderAddress = 0;

        // Points into the interned FMeshResolveCache entry (or, for dynamic meshes, into the component's
        // own render data). Stable while ResolveGeneration matches; the sync pass re-binds otherwise.
        const TVector<FResolvedSurface>*    Surfaces = nullptr;

        uint32                              BindingBase = 0;    // span into FScenePrimitiveSet::Bindings
        uint32                              SurfaceCount = 0;

        entt::entity                        Entity = entt::null;
        uint32                              EntityID = 0;
        uint32                              CustomData = 0;

        // Dynamic meshes only. Surfaces points inside this block, and Commit() can swap the component's
        // pointer from a worker; holding a ref keeps what the gather is reading alive until the sync
        // pass swings the primitive over to the new one.
        TSharedPtr<FDynamicMeshRenderData>  DynamicRenderData;

        uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
        uint32                              ResolveGeneration = 0;

        // Mesh-local sphere the world bounds were built from; kept so a transform-only change can
        // rebuild the world sphere without touching the component or the mesh.
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

    /**
     * The render scene's persistent view of what exists.
     *
     * Membership and every camera-independent property are maintained incrementally from the dirty
     * channel. The arrays are dense (removal is a swap-with-last plus one map fixup), so the per-frame
     * cull walks them with no holes, no ECS traversal and no component dereference.
     *
     * Invariants:
     *  - Primitives, Bounds and CullData are parallel arrays of the same length. Index them together.
     *  - Bindings[Primitive.BindingBase .. +SurfaceCount) is that primitive's per-surface batch/draw
     *    identity, and is valid exactly while Primitive.Surfaces is non-null.
     *  - A primitive with Surfaces == nullptr is present but not drawable (mesh not resolved yet); it is
     *    kept so the retry costs a flag check instead of a rediscovery.
     *  - StructureGeneration changes iff something the renderer would draw differently changed. The
     *    frame path uses it to decide whether last frame's compile can be reused verbatim.
     */
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

        // Bumped by any change that can alter this frame's draw output. Compared against the last
        // compiled value to decide whether the scene half of the frame can be skipped entirely.
        uint64                              GetStructureGeneration() const { return StructureGeneration; }

        // Skinned primitives are the one source whose render data changes with nothing to observe it by:
        // the animation system writes BoneTransforms directly, bumping no generation. A scene containing
        // any of them can never reuse a previous frame's compile.
        uint32                              GetSkinnedPrimitiveCount() const { return SkinnedCount; }

        //~ Retained GPU-facing scene ---------------------------------------------------------------
        //
        // One stable FGPUInstance slot per primitive surface, written only when that primitive changes.
        // CullInstances.slang reads these plus the parallel cull data, resolves LOD, and appends the
        // survivors into the per-frame instance buffer -- so nothing here is rebuilt per frame and the
        // camera never touches the CPU.

        const FInstanceCullEntry*   GetRetainedCullEntries() const { return RetainedCullEntries.data(); }
        const FTransform3x4*        GetRetainedTransforms() const { return RetainedTransforms.data(); }
        const FInstanceStatic*      GetRetainedStatic() const     { return RetainedStatic.data(); }
        uint32                      GetRetainedSlotCount() const  { return (uint32)RetainedCullEntries.size(); }

        const FSurfaceDescGPU*      GetSurfaceDescs() const     { return SurfaceDescs.data(); }
        uint32                      GetSurfaceDescCount() const { return (uint32)SurfaceDescs.size(); }
        bool                        AreSurfaceDescsDirty() const { return bSurfaceDescsDirty; }
        void                        ClearSurfaceDescsDirty()     { bSurfaceDescsDirty = false; }

        // Slots written since the last upload. Empty on a frame where nothing changed, which is what
        // makes the upload proportional to the change rather than to the scene.
        //
        // TWO lists, because the arrays they cover change at completely different rates. A move touches
        // the cull entry and the transform; the static payload only moves on a re-bind. Keeping them
        // apart is what stops a mover-heavy frame from re-sending a third buffer that did not change --
        // and each upload run is a transient RHI allocation, so the count of them matters on its own.
        const TVector<uint32>&      GetDirtyInstanceSlots() const { return DirtyInstanceSlots; }
        const TVector<uint32>&      GetDirtyStaticSlots() const   { return DirtyStaticSlots; }
        void                        ClearDirtyInstanceSlots()     { DirtyInstanceSlots.clear(); DirtyStaticSlots.clear(); }
        // True when the slot array itself was reallocated, so the whole thing has to be re-sent.
        bool                        NeedsFullInstanceUpload() const { return bFullInstanceUpload; }
        void                        ClearFullInstanceUpload()       { bFullInstanceUpload = false; }

        // A shared FMeshResolveCache entry was rebuilt. Only the components that were individually stale
        // get marked by the resolve pass, but an entry is shared by every instance of that mesh, so the
        // next sync sweeps for primitives whose cached surface generation drifted. Rare (asset load,
        // material recompile, mesh reimport) and gated on this flag, so steady state never pays it.
        void                                NotifyResolveTableChanged() { bResolveTableChanged = true; }

    private:

        // Packed identity: entity | source | foliage sub-index.
        // Bit layout of a primitive key: entity in the low word, then source, then the sub-index that
        // separates one foliage instance from the next. Named because kSyncTargetMask below is derived
        // from them -- a literal there would be a second, silently divergent copy of this layout.
        static constexpr uint32 kKeySourceShift   = 32;
        static constexpr uint32 kKeySubIndexShift = 40;

        static uint64 MakeKey(entt::entity Entity, EPrimitiveSource Source, uint32 SubIndex = 0)
        {
            return (uint64)entt::to_integral(Entity)
                 | ((uint64)Source << kKeySourceShift)
                 | ((uint64)SubIndex << kKeySubIndexShift);
        }

        /**
         * The (entity, source) half of a primitive key -- everything below the sub-index.
         *
         * A re-sync targets an ENTITY, not one of its primitives: SyncFoliage rebuilds every instance its
         * entity owns. Masking a key down to this is what lets a sweep over stale primitives collapse to
         * one call per entity.
         */
        static constexpr uint64 kSyncTargetMask = (1ull << kKeySubIndexShift) - 1ull;

        uint32  FindPrimitive(uint64 Key) const;
        uint32  AddPrimitive(uint64 Key);
        void    RemovePrimitive(uint64 Key);
        void    RemoveEntity(FEntityRegistry& Registry, entt::entity Entity, EPrimitiveSource Source);

        /**
         * Component pools resolved ONCE per sync, defined in the .cpp so this header stays free of the
         * component headers.
         *
         * entt reaches every `all_of<T>` / `any_of<T>` / `storage<T>()` through a type-id hash map, so the
         * per-entity form paid a HASH LOOKUP PER COMPONENT TYPE on top of each sparse probe. SyncEntity did
         * four of those (disabled tag, the source component, the transform pool, then the transform pool
         * AGAIN to read it) for every entity it touched -- and the transform channel touches every moving
         * entity in the world once a frame. Hoisting the pools turns all of that into plain sparse probes.
         *
         * Everything the sync pass reaches for goes through here, including the pools RefreshPrimitiveData
         * and SyncFoliage used to look up themselves. Registry.storage<T>() can CREATE a pool, so the
         * constructor must stay the only caller of it in this translation unit.
         */
        struct FSyncPools;
        // Defined further down, next to the table it indexes; declared here so the binding helpers
        // below can take one by pointer.
        struct FBindingMemo;

        // Re-reads the owning component and rebuilds everything camera-independent. Returns false when
        // the mesh is not resolvable yet, in which case the primitive stays parked as undrawable.
        bool    RefreshPrimitiveData(const FSyncPools& Pools, uint32 Index);

        // Binds one primitive's surfaces to the batch registry. Only called when its resolve changed.
        void    BindSurfaces(uint32 Index);
        void    ReleaseBindings(uint32 Index);

        /**
         * The per-interned-mesh binding template for (Handle, Generation), rebuilt only when stale.
         * Returns nullptr for a primitive with no resolve handle (dynamic meshes), which must bind
         * per-surface instead. Hoisted out of BindSurfaces so the match check below can consult it
         * WITHOUT committing to a rebind.
         */
        const FBindingMemo* EnsureBindingMemo(uint32 ResolveHandle, uint32 Generation,
                                              const TVector<FResolvedSurface>& Surfaces);

        /**
         * True when the bindings this primitive already holds are what a fresh bind would produce --
         * everything except InstanceSlot, which is precisely the field a rebind would needlessly churn.
         *
         * A resolve generation moving means the cache entry was REBUILT, not that anything a binding
         * derives from actually differs; a mesh finishing its GPU upload, or a material recompiling,
         * re-interns to the identical batch index and LOD table. Treating "regenerated" as "different"
         * is what put the whole rebind path (see BindSurfaces) behind an event that usually changes
         * nothing -- and for foliage that path is once per baked instance.
         */
        bool    BindingsMatchMemo(uint32 Index, const FBindingMemo& Memo) const;

        /**
         * Writes one primitive's surfaces into their retained instance slots and records them as dirty.
         *
         * DirtySink is what makes this callable from a worker: pass a list the caller owns and nothing
         * shared is touched. nullptr takes the serial path -- the shared DirtyInstanceSlots plus the
         * full-upload threshold, which cannot be evaluated concurrently because it clears the list.
         */
        void    RefreshInstances(uint32 Index, TVector<uint32>* DirtySink = nullptr);

        /**
         * The move-only half of RefreshInstances: world bounds and the transform, nothing else.
         *
         * A move cannot change the batch, the material, the surface desc, the entity id or the meshlet
         * header, so restamping the static payload for one was pure write traffic -- and it dirtied a
         * third buffer that had not changed.
         */
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

        /**
         * FOLIAGE PRIMITIVES ONLY. Everything else is indexed by LinksByEntityIndex.
         *
         * eastl::hash_map is node-based, so carrying the other three sources here cost a heap ALLOCATION
         * per add and a heap FREE per remove -- for a key the flat link table already answers, maintained
         * in the same statements. Foliage is the only source that needs it: one entity owns one primitive
         * per baked instance, keyed with a third sub-index field that one-slot-per-source cannot express.
         *
         * FindPrimitive routes by source, so callers do not care which index holds their key.
         */
        THashMap<uint64, uint32>            IndexByKey;

        /**
         * Parallel: index -> (ResolveGeneration << 32 | ResolveHandle), mirroring the same two fields on
         * FScenePrimitive.
         *
         * The resolve sweep reads nothing else about a primitive, and reading it off FScenePrimitive meant
         * striding a 160-byte element to pull eight bytes -- touching every cache line in the array to use
         * 5% of it. Mirrored here the sweep streams a dense uint64 array that stays resident.
         *
         * Maintained in lockstep with Keys, in the same statements, and cross-checked against the
         * authoritative fields under DEBUG_ASSERT in the sweep. Drift would otherwise be silent: the sweep
         * would just re-resolve the wrong primitive forever.
         */
        TVector<uint64>                     ResolveKeys;

        static uint64 PackResolveKey(uint32 Handle, uint32 Generation)
        {
            return (uint64)Handle | ((uint64)Generation << 32);
        }

        // Scratch: resolve handle -> that entry's current Generation, filled lazily during the sweep.
        TVector<uint32>                     GenSnapshot;

        // Bump-allocated per-surface binding spans. Reclaimed by compaction when the dead share grows
        // past a threshold, which only happens after a lot of mesh swaps.
        TVector<FSurfaceBinding>            Bindings;
        uint32                              DeadBindings = 0;
        void                                CompactBindings();

        FSceneBatchRegistry                 Batches;

        /**
         * Per resolve handle: the binding every primitive of that mesh gets, minus its instance slot.
         *
         * Everything on FSurfaceBinding except InstanceSlot is a pure function of the FResolvedSurface it
         * came from, and FMeshResolveCache interns one entry per (mesh, material assignment). So a thousand
         * instances of one rock used to ask FindOrAddBatch and InternSurfaceDesc the same question a
         * thousand times -- each a hash plus a chained-bucket probe, and InternSurfaceDesc rebuilds and
         * rehashes an 80-byte LOD table first. Resolving once per (handle, generation) makes that cost
         * scale with distinct meshes rather than with instances.
         *
         * This lives HERE and not on FMeshResolveCache because BatchIndex and SurfaceDescIndex are indices
         * into THIS scene's Batches and SurfaceDescs, while the resolve cache is a process-global singleton
         * shared by every world.
         *
         * INVARIANT: cleared with Batches.Reset(), always, in both places that call it (FullRescan and
         * Reset). A batch reset renumbers every index, so a surviving memo would hand out indices into an
         * empty registry. Those two functions clear SurfaceDescs as well, so one clear covers both. The
         * same pairing is what makes it sound to skip FindOrAddBatch's deferred-material fold on a hit.
         *
         * Handles are indices that are never recycled at runtime (FMeshResolveCache::Flush runs only from
         * the destructor), so (handle, generation) is a stable key. If Flush ever gains a runtime caller,
         * this has to clear there too.
         */
        struct FBindingMemo
        {
            uint32                      Generation = ~0u;   // FResolvedMesh::Generation it was built at
            TVector<FSurfaceBinding>    Protos;             // InstanceSlot unused
        };
        TVector<FBindingMemo>               BindingMemoByHandle;

        TVector<uint64>                     RetryScratch;

        /**
         * Entity index -> the primitive it owns per source. Replaces BOTH a `SourceMaskByEntity` hash probe
         * and a `FindPrimitive` hash probe on the hottest path in the whole sync.
         *
         * The transform channel marks every moving entity in the world, once per frame. For each one the
         * old path hashed the entity to get its source mask, then hashed a (entity, source) key per set bit
         * to find the primitive -- two-plus hash lookups, each a bucket probe into a heap-allocated node,
         * for every mover. Flat-indexing by `entt::to_entity` turns that into ONE load from a contiguous
         * array, and 16-byte entries put four entities on a cache line.
         *
         * Bounded, not unbounded: `entt_traits<uint32>::entity_mask` is 20 bits, so this can never exceed
         * ~1M entries (16 MB) however many entities are created and recycled.
         *
         * FOLIAGE IS DELIBERATELY ABSENT (hence kLinkedSources stopping at Foliage): one foliage entity
         * owns one primitive per baked instance, keyed with a third sub-index field, so it does not fit
         * "one primitive per (entity, source)" and keeps using IndexByKey. It never needed to be here --
         * a foliage instance bakes its own world transform rather than following the owning entity's, so
         * it is not part of the transform routing this table exists to serve.
         */
        static constexpr uint32 kLinkedSources = (uint32)EPrimitiveSource::Foliage;

        struct FPrimitiveLink
        {
            // Full entt::to_integral of the owning entity, version included. Entity INDICES are recycled,
            // so without this guard a new entity would inherit the dead one's primitives; comparing the
            // whole integral makes a recycled slot read as empty. ~0u is never a live entity.
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

        // Foliage entity -> how many baked instances it currently owns, so a rebake removes exactly
        // the primitives it created without scanning.
        THashMap<entt::entity, uint32>      FoliageInstanceCount;

        // Everything a baked foliage instance takes from its type, flattened once per type per rebake so
        // the per-instance loop is pure array reads. See SyncFoliage.
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

        /**
         * One record per distinct entity in a drain, with flags accumulated per source.
         *
         * A bulk spawn queues three to four entries for the SAME entity in one frame: on_construct and
         * on_update both mark Data|Membership (World.cpp), the resolve pass marks Data once the mesh
         * resolves, and the transform channel marks Transform. Applying each separately re-read the
         * component and restamped every instance slot roughly four times per new primitive. Foliage was
         * worse: it is marked once per stale SFoliageType, and every SyncFoliage call is O(baked
         * instances of that entity), so eight types meant eight full rebake sweeps.
         *
         * Folding is sound because SyncEntity and SyncFoliage are state RECONCILERS, not event appliers:
         * both re-derive whether the primitive should exist from the live registry rather than from the
         * flags they were handed. So OR-ing a frame's flags and applying once lands on the same state as
         * applying them in order.
         */
        struct FCoalescedEntity
        {
            // Full entt::to_integral, version included -- entity INDICES are recycled, so the version is
            // what stops a destroyed entity's record from absorbing its successor's. Same guard, and same
            // reason, as FPrimitiveLink::Entity.
            uint32          Entity = ~0u;
            EPrimitiveDirty Flags[(uint32)EPrimitiveSource::Num] = {};
        };

        // Entity index -> record, flat-indexed like LinksByEntityIndex. Stamped so a new drain invalidates
        // every slot without walking the table.
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

        /**
         * Splits the coalesced records by whether applying them can mutate shared structure.
         *
         * Transform is the only flag that can neither create nor destroy a primitive. A record carrying
         * nothing else therefore writes ONLY things addressed by the primitive index its entity already
         * owns -- Primitives[i].Transform, Bounds[i], and that primitive's own instance slots. One entity
         * owns at most one primitive per source and one record covers one entity, so no two records can
         * target the same index and the whole set runs wide with no synchronization.
         *
         * Everything else stays serial and must run FIRST. Adds append, removals swap-with-last, and both
         * renumber indices the transform pass is about to resolve through the link table.
         */
        void PartitionDrain();
        void ApplyStructuralRecords(FEntityRegistry& Registry, const FSyncPools& Pools);
        void ApplyTransformRecords(const FSyncPools& Pools);

        // Body of one transform-only record. Must touch nothing except this entity's own primitives.
        // OutDirty is nullptr on the serial fallback; see RefreshInstances.
        void ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex, TVector<uint32>* OutDirty);

        // Indices into CoalescedScratch, filled by PartitionDrain.
        TVector<uint32>             TransformRecords;
        TVector<uint32>             StructuralRecords;

        /**
         * One dirty-slot list per task-thread slot.
         *
         * Range.Thread is unique among concurrently running chunks and stable within one, so indexing by
         * it needs no lock. The lists are separate heap allocations, so the per-push size bumps land on
         * different cache lines -- the headers here are adjacent, but a worker only touches its own and
         * the merge below reads them all serially.
         */
        TVector<TVector<uint32>>    ParallelDirtySlots;

        // Folds the per-worker lists back into DirtyInstanceSlots, or gives up and re-sends everything.
        void MergeParallelDirtySlots();

        /**
         * Retained GPU scene, split by ACCESS RATE rather than held as one struct.
         *
         * All three are parallel and slot-addressed; freed slots keep their storage and are handed back
         * out by AllocateInstanceSlot, so slot ids stay stable for as long as a binding holds one.
         *
         *  - CullEntries (32 B) is streamed in full by CullInstances every frame. It holds exactly what a
         *    REJECT needs, which is why it is worth its own array: as one 144-byte struct the cull pulled
         *    roughly five times the bandwidth it consumed.
         *  - Transforms is read only by survivors, and rewritten every time a primitive moves.
         *  - Static is read only by survivors, and rewritten only when a primitive is re-bound.
         */
        TVector<FInstanceCullEntry>         RetainedCullEntries;
        // 3x4, not 4x4: an instance transform is always affine, so the last row is (0,0,0,1) and storing
        // it is 16 wasted bytes per slot on an array a moving crowd rewrites every frame. See
        // FTransform3x4 for the convention and how it was verified.
        TVector<FTransform3x4>              RetainedTransforms;
        TVector<FInstanceStatic>            RetainedStatic;

        TVector<uint32>                     InstanceFreeSlots;
        TVector<uint32>                     DirtyInstanceSlots;   // cull entry + transform
        TVector<uint32>                     DirtyStaticSlots;     // static payload
        bool                                bFullInstanceUpload = true;

        // Interned per distinct LOD table, keyed by a hash of its contents: every instance of one rock
        // shares an entry, so this is sized by distinct surfaces rather than by instances.
        TVector<FSurfaceDescGPU>            SurfaceDescs;
        THashMap<uint64, TVector<uint32>>   SurfaceDescByHash;
        bool                                bSurfaceDescsDirty = true;

        uint64                              StructureGeneration = 1;
        uint32                              SkinnedCount = 0;
        bool                                bResolveTableChanged = false;

        /**
         * Per-sync aggregates, plotted once at the end of Sync.
         *
         * Counters rather than per-entity profiler zones: TRACY_CALLSTACK is defined engine-wide
         * (LuminaFeatures.BuildRules.cs), so every LUMINA_PROFILE_SCOPE carries a tracy_malloc plus an
         * RtlWalkFrameChain stack unwind while the profiler is attached. At one zone per dirty entity that
         * cost more than the pass it was measuring. These are one increment each, and the plots say more:
         * DrainEntries vs CoalescedEntities is the redundancy in the change channel, and BindCalls staying
         * flat across frames is how you tell the resolve table has settled.
         */
        struct FSyncStats
        {
            uint32 DrainEntries         = 0;
            uint32 CoalescedEntities    = 0;
            uint32 SyncEntityCalls      = 0;
            uint32 SyncFoliageCalls     = 0;
            uint32 BindCalls            = 0;
            // Rebinds a generation bump asked for and BindingsMatchMemo proved unnecessary. On a foliage
            // scene this should dwarf BindCalls whenever an asset finishes streaming; the two trading
            // places means something is genuinely re-interning and the memo is not the problem.
            uint32 BindsSkipped         = 0;
            uint32 RefreshInstanceCalls = 0;
            // The split PartitionDrain produced. Only the transform half runs wide, so these two say
            // directly how much of a slow Sync/Apply is even parallelizable.
            uint32 TransformRecords     = 0;
            uint32 StructuralRecords    = 0;
        };
        FSyncStats  SyncStats;
        void        PublishSyncStats() const;
    };
}
