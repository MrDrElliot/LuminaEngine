#pragma once

#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"


#include "Renderer/ShaderHandle.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RenderLayers.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Lumina.h"

namespace Lumina
{
    class CWorld;
    struct SFoliageComponent;
    struct FFoliageBakedInstance;
    struct FDynamicMeshRenderData;

    enum class EPrimitiveSource : uint8
    {
        StaticMesh,
        DynamicMesh,
        SkeletalMesh,
        Foliage,
        Num,

        AnySource = 0xFF,
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
    ENUM_CLASS_FLAGS(EPrimitiveDirty);

    class RUNTIME_API FRenderDirtyTracker
    {
    public:

        struct FEntry
        {
            ECS::FEntity        Entity;
            EPrimitiveSource    Source;
            EPrimitiveDirty     Flags;
        };

        // Creates the tracker in the registry context on first call. Game thread.
        static FRenderDirtyTracker& Ensure(ECS::FRegistry& Registry);
        static FRenderDirtyTracker* Find(ECS::FRegistry& Registry);

        FRenderDirtyTracker();
        ~FRenderDirtyTracker();
        LE_NO_COPYMOVE(FRenderDirtyTracker);

        // Any thread.
        void Mark(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags);

        void MarkAllSources(ECS::FEntity Entity, EPrimitiveDirty Flags);

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
            uint32              MaterialIndex;
            FShaderH DeferredShader;
        };

        struct FBatch
        {
            FDrawBatchKey                   Key = {};
            // Copied out of the resolve at bind time so the frame path never re-reads the shared entry.
            FShaderH             PixelShader = {};
            FShaderH             VertexShader = {};
            FShaderH             MeshShaderShadow = {};
            FShaderH             MeshShaderBase = {};
            FShaderH             VisBufferMeshShader = {};
            FShaderH             VisBufferMeshShaderMasked = {};
            FShaderH             MaskedVisBufferPixelShader = {};
            FShaderH             MeshShaderShadowMasked = {};
            FShaderH             ShadowMaskedPixelShader = {};
            // No material identity here on purpose: a batch is a PIPELINE, and one pipeline serves every
            // material that compiles to it. The material is carried per instance
            // (FGPUInstance::MaterialIndex) and per deferred slot (DeferredMaterials below).

            uint32                          RefCount = 0;

            // Bound instances, not visible ones. The SPEC_SKINNED variant is a property of everything the
            // GPU cull can emit into this batch, and the CPU gather sees only what it culled to.
            uint32                          SkinnedRefCount = 0;
            uint32                          StaticRefCount = 0;

            TVector<FDeferredMaterialSlot>  DeferredMaterials;
        };

        uint32 FindOrAddBatch(const FResolvedSurface& Surface);

        void AddBatchRef(uint32 BatchIndex, bool bSkinned);
        void AddBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count);

        void ReleaseBatchRef(uint32 BatchIndex, bool bSkinned);
        void ReleaseBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count);

        uint32          Num() const { return (uint32)Batches.size(); }
        FBatch&         Get(uint32 Index) { return Batches[Index]; }
        const FBatch&   Get(uint32 Index) const { return Batches[Index]; }

        uint64          GetLayoutGeneration() const { return LayoutGeneration; }
        // Moves only when a slot is freed, which is the one event that can make a cached batch index lie.
        uint64          GetRecycleGeneration() const { return RecycleGeneration; }

        bool            IsLive(uint32 Index) const { return Batches[Index].RefCount != 0u; }

        void Reset();

    private:

        void RetireBatch(uint32 BatchIndex);

        TVector<FBatch>                     Batches;
        THashMap<uint64, TVector<uint32>>   BatchesByHash;
        TVector<uint32>                     FreeBatches;
        uint64                              LayoutGeneration = 1;
        uint64                              RecycleGeneration = 1;
    };

    /** Where one surface of a primitive draws: resolved once at bind, read every frame. */
    struct FSurfaceBinding
    {
        uint32          BatchIndex;
        uint32          InstanceSlot;
        uint32          SurfaceDescIndex;   // interned LOD table
        uint32          MaterialIndex;
        EInstanceFlags  MaterialFlags;
        bool            bMaterialCastsShadows;
        // Held so the release decrements the same counter the bind incremented, whatever the primitive
        // has become since. Not part of the memo proto; the primitive owns it, not the resolve.
        bool            bSkinned = false;

        // Mesh-local world size of one UV tile; 0 = unknown. Read by the texture-streaming gather to turn
        // a distance into a required texture resolution. See FGeometrySurface::TexelFactor.
        //
        // Initialized, unlike its neighbors, because this struct is also built field-by-field as a memo
        // proto -- a field missed there reads as garbage rather than zero, and a junk float here becomes a
        // junk mip request (or a NaN that poisons the max downstream).
        float           TexelFactor = 0.0f;

        // Into the skinned frame data, held by a skinned primitive's binding and kNoSkinnedIndex otherwise.
        uint32          SkinnedIndex = kNoSkinnedIndex;
    };

    // Mirror of FTransformStreamRecord in RetainedTransformStream.slang, one moved instance's packed transform and world sphere in one line.
    struct alignas(64) FTransformStreamRecord
    {
        FTransform3x4 Transform;
        FVector4      SphereBounds;
    };
    static_assert(sizeof(FTransformStreamRecord) == 64, "FTransformStreamRecord must match RetainedTransformStream.slang");

    // A slot the GPU scatter skips, written for a mover that took the CPU path instead.
    constexpr uint32 kNoStreamSlot = 0xFFFFFFFFu;

    // Where moved instances go this frame, write-combined memory the GPU scatters into the retained buffers, never read back.
    struct FTransformStreamTarget
    {
        FTransformStreamRecord* Records  = nullptr;
        uint32*                 Slots    = nullptr;
        uint32                  Capacity = 0;
    };

    // Cache-line aligned so the matrix fills exactly one line, which a moved transform streams out without reading it first.
    struct CACHE_ALIGN FScenePrimitive
    {
        // Everything a moved transform rewrites or reads sits together, since a crowd of movers streams this from memory every frame.
        FMatrix4                            Transform = FMatrix4(1.0f);
        FVector3                            LocalCenter = FVector3(0.0f);
        float                               LocalRadius = 0.0f;
        float                               BoundsScale = 1.0f;
        uint32                              BindingBase = 0;    // span into FScenePrimitiveSet::Bindings
        uint32                              SurfaceCount = 0;
        uint32                              RenderLayers = RenderLayers::kDefaultMask;

        uint32                              MeshletHeaderSlot = 0;

        const TVector<FResolvedSurface>*    Surfaces = nullptr;

        ECS::FEntity                        Entity = ECS::NullEntity;
        uint32                              EntityID = 0;
        uint32                              CustomData = 0;

        TSharedPtr<FDynamicMeshRenderData>  DynamicRenderData;

        uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
        uint32                              ResolveGeneration = 0;

        int32                               ForcedLODIndex = -1;
        // PackCustomDepthStencil, zero when it draws no custom depth.
        uint32                              CustomDepthStencil = 0;
        EInstanceFlags                      BaseFlags = EInstanceFlags::None;
        EPrimitiveSource                    Source = EPrimitiveSource::StaticMesh;
        bool                                bCastShadow = true;
        bool                                bCastFarShadow = false;

        // Cached skeleton bone count; 0 means unresolved, which holds the instance inactive.
        uint32                              BoneCount = 0;
        // Highest bone the mesh's weights reference, cached so the per-frame emit never reads the asset.
        uint32                              RequiredBoneCount = 0;

        // Bone-arena slice held while this primitive keeps being gathered; stable across frames.
        uint32                              BoneSliceBase = kNoBoneSlice;
        uint32                              BoneSliceCount = 0;
        uint32                              BoneSliceFrame = 0;

        // What the GPU arena currently holds for this primitive. Either half differing forces a re-upload.
        uint32                              UploadedSliceBase = kNoBoneSlice;
        uint32                              UploadedPoseSerial = 0;
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
        ~FScenePrimitiveSet() = default;
        LE_NO_COPYMOVE(FScenePrimitiveSet);

        // Drains the dirty channel and applies it. O(changed), except on a requested full rescan.
        // Moved is the frame's drained transform list, folded in directly rather than queued through the tracker.
        void Sync(CWorld& World, TSpan<const ECS::FEntity> Moved = {});

        // Drops every primitive and re-arms a full rescan. World teardown / scene rebuild.
        void Reset(ECS::FRegistry* Registry);

        // Remaps Foreground-layer instances for this camera, or draws them in place when it has no foreground. Call before Sync.
        void SetForegroundView(const FViewVolume& View, const FViewLayerSettings& Layers);

        // Moved instances that fit the stream write their transform there instead of into the CPU copies. Call before Sync, or pass an empty target.
        void SetTransformStream(const FTransformStreamTarget& Target) { StreamTarget = Target; }

        // Records the last Sync wrote, one per moved entity, with kNoStreamSlot for any that took the CPU path.
        uint32 GetTransformStreamCount() const { return StreamCount; }

        // Records the movers themselves wrote into the last Sync's stream, as they moved.
        uint32 GetMoveTimeRecords() const { return MoveTimeRecords; }

        // Flat movers write their own records into Target until the next Sync folds them in, so arm it only between frames, with a slice the GPU is done with.
        void ArmMoveStream(ECS::FRegistry& Registry, const FTransformStreamTarget& Target, uint32 MovedConsumer);

        // Takes the writer back off the registry, for a renderer going away while armed.
        void DisarmMoveStream(ECS::FRegistry& Registry);

        bool IsMoveStreamArmed() const { return bMoveStreamArmed; }

        // Blocks of slots holding an instance streamed this frame, sorted, whose CPU spheres trail the GPU.
        const TVector<uint32>& GetStreamedBlocks() const { return StreamedBlocks; }

        uint32                              Num() const { return (uint32)Primitives.size(); }
        const FScenePrimitive*              GetPrimitives() const { return Primitives.data(); }
        // The gather stamps per-primitive upload state through this; entries are disjoint per worker.
        FScenePrimitive*                    GetMutablePrimitives() { return Primitives.data(); }
        const FVector4*                     GetBounds() const { return Bounds.data(); }
        const FPrimitiveCullData*           GetCullData() const { return CullData.data(); }
        const FSurfaceBinding*              GetBindings() const { return Bindings.data(); }

        FSceneBatchRegistry&                GetBatches() { return Batches; }
        const FSceneBatchRegistry&          GetBatches() const { return Batches; }

        uint64                              GetStructureGeneration() const { return StructureGeneration; }

        uint32                              GetSkinnedPrimitiveCount() const { return SkinnedCount; }

        // Primitives drawn into custom depth, so the renderer adds the view and the pass only while one exists.
        uint32                              GetCustomDepthPrimitiveCount() const { return CustomDepthCount; }

        // Dense skeletal primitive indices, ascending; rebuilt only when the set changes shape.
        const TVector<uint32>&              GetSkeletalIndices();

        /** Bones the arena must hold. Only grows within a level, so a buffer sized from it never shrinks
         *  under live data and a base handed out last frame is still valid this frame. */

        EPrimitiveSource                    GetSource(uint32 Index) const
        {
            return (EPrimitiveSource)((Keys[Index] >> kKeySourceShift) & 0xFFull);
        }

        const FInstanceCullEntry*   GetRetainedCullEntries() const { return RetainedCullEntries.data(); }
        const FTransform3x4*        GetRetainedTransforms() const { return RetainedTransforms.data(); }
        const FInstanceStatic*      GetRetainedStatic() const     { return RetainedStatic.data(); }
        uint32                      GetRetainedSlotCount() const  { return (uint32)RetainedCullEntries.size(); }
        uint32                      GetSkinnedIndexExtent() const { return SkinnedIndexExtent; }

        /** What a GPU scatter needs to write retained instances for one grass species. */
        struct FGrassSpeciesBinding
        {
            uint32 InstanceSlotBase   = 0;
            uint32 Capacity           = 0;
            uint32 DrawIDAndFlags     = 0;
            uint32 SurfaceDescIndex   = 0;
            uint32 MeshletHeaderSlot  = 0;
            uint32 MaterialIndex      = 0;
            float  MeshBoundsRadius   = 0.0f;
            bool   bValid             = false;

            // The batch the block holds a reference on and the resolve it was bound from.
            uint32 BatchIndex         = 0;
            uint32 ResolvedGeneration = 0;
        };

        /**
         * Reserves a contiguous run of retained slots the GPU owns, plus the batch and surface-desc
         * indices every blade in it will carry. The CPU never writes the run again: the scatter fills it
         * each frame and leaves unused slots inactive, which is exactly the "free slots carry bActive = 0"
         * contract the instance cull already relies on.
         *
         * Idempotent per (mesh, capacity); a second call for the same species returns the same block.
         */
        FGrassSpeciesBinding AcquireGrassSpecies(CStaticMesh* Mesh, uint32 ResolveHandle, uint32 Capacity);

        // Retained slot ranges (base, count) the grass scatter fills on the GPU, which the CPU arrays only hold as zeros.
        void GetGPUWrittenSlotRanges(TVector<FUIntVector2>& Out) const
        {
            Out.clear();
            for (const auto& [Mesh, Binding] : GrassSpecies)
            {
                Out.push_back(FUIntVector2(Binding.InstanceSlotBase, Binding.Capacity));
            }
        }

        // Points a species' block at its surface's current batch, material and surface desc.
        void BindGrassSurface(FGrassSpeciesBinding& Binding, const FResolvedMesh& Resolved);

        // Touches only Primitives[Index], so this half is parallel-safe; kNoBoneSlice means it needs one.
        uint32                      TouchBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber);
        // Recycled by EXACT size and mutates the free lists, so this half must stay serial.
        uint32                      AcquireBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber);
        // Rolling sweep, a slab of the table per call, so the cost never scales with scene size at once.
        void                        ReleaseStaleBoneSlices(uint32 FrameNumber, uint32 GraceFrames);
        uint32                      GetBoneSliceExtent() const { return BoneSliceExtent; }

        // The skeletal primitive Entity renders through, ~0u when it has none.
        uint32                      FindSkeletalPrimitive(ECS::FEntity Entity) const { return FindLinked(Entity, EPrimitiveSource::SkeletalMesh); }

        const FSurfaceDescGPU*      GetSurfaceDescs() const     { return SurfaceDescs.data(); }
        uint32                      GetSurfaceDescCount() const { return (uint32)SurfaceDescs.size(); }
        bool                        AreSurfaceDescsDirty() const { return bSurfaceDescsDirty; }
        void                        ClearSurfaceDescsDirty()     { bSurfaceDescsDirty = false; }

        uint32                      GetMaxSurfaceDescMeshlets() const { return MaxSurfaceDescMeshlets; }

        const TVector<uint32>&      GetDirtyInstanceSlots() const { return DirtyInstanceSlots; }
        const TVector<uint32>&      GetDirtyStaticSlots() const   { return DirtyStaticSlots; }
        const TVector<uint32>&      GetDeactivatedSlots() const   { return DeactivatedSlots; }
        void                        ClearDirtyInstanceSlots()     { DirtyInstanceSlots.clear(); DirtyStaticSlots.clear(); DeactivatedSlots.clear(); }
        // True after a reset or when most slots changed at once, so the whole array is re-sent.
        bool                        NeedsFullInstanceUpload() const { return bFullInstanceUpload; }
        // The partial paths send only the dirty slots, so re-sending all of them pays once nearly every slot changed.
        static constexpr bool       ShouldResendAllInstances(SIZE_T DirtySlots, SIZE_T SlotCount) { return DirtySlots * 8 >= SlotCount * 7; }
        bool                        NeedsFullStaticUpload() const   { return bFullStaticUpload; }
        void                        ClearFullInstanceUpload()       { bFullInstanceUpload = false; bFullStaticUpload = false; }

        // Sizes every changed slot's meshlet visibility from its surface's largest LOD. Serial, before the dirty lists are consumed.
        void                        AssignMeshletVisibility();
        uint32                      GetMeshletVisibilityExtent() const { return MeshletVisibilityExtent; }

        void                                NotifyResolveTableChanged() { bResolveTableChanged = true; }

    private:

        static constexpr uint32 kKeySourceShift = 32;

        static uint64 MakeKey(ECS::FEntity Entity, EPrimitiveSource Source)
        {
            return (uint64)(Entity).Value | ((uint64)Source << kKeySourceShift);
        }

        uint32  FindPrimitive(uint64 Key) const;
        uint32  AddPrimitive(uint64 Key);
        void    RemovePrimitive(uint64 Key);
        void    RemoveEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, EPrimitiveSource Source);

        struct FSyncPools;
        struct FBindingMemo;

        bool    RefreshPrimitiveData(const FSyncPools& Pools, uint32 Index);

        // Binds one primitive's surfaces to the batch registry. Only called when its resolve changed.
        void    BindSurfaces(uint32 Index);
        void    ReleaseBindings(uint32 Index);
        // The half of the two above that only needs the span, so foliage instances share it.
        // Foliage passes false for bAssignSkinnedIndex, since only a skinned primitive is gathered for frame data.
        uint32  AppendBindingSpan(const FBindingMemo& Memo, bool bSkinned, bool bAssignSkinnedIndex);
        void    ReleaseBindingSpan(uint32 Base, uint32 Count);

        const FBindingMemo* EnsureBindingMemo(uint32 ResolveHandle, uint32 Generation,
                                              const TVector<FResolvedSurface>& Surfaces);

        bool    BindingsMatchMemo(uint32 Index, const FBindingMemo& Memo) const;

        // Content compare for sources whose resolve identity cannot move -- dynamic meshes. See the .cpp.
        bool    BindingsMatchSurfaces(uint32 Index, const TVector<FResolvedSurface>& Surfaces) const;

        void    RefreshInstances(uint32 Index, TVector<uint32>* DirtySink = nullptr);

        void    RefreshInstanceTransform(uint32 Index, TVector<uint32>* DirtySink = nullptr);
        // Takes the matrix just written, since reading back a line that was streamed past the cache costs a trip to memory.
        void    RefreshInstanceTransform(uint32 Index, const FMatrix4& Transform, TVector<uint32>* DirtySink);

        bool        IsRemappedForeground(const FScenePrimitive& Prim) const { return bForegroundActive && (Prim.RenderLayers & RenderLayers::kForegroundMask) != 0u; }
        FMatrix4    InstanceTransformOf(const FScenePrimitive& Prim) const;
        FVector4    InstanceSphereOf(uint32 Index) const;
        void        TrackForeground(uint32 Index);

        uint32  AllocateInstanceSlot();

        TVector<uint32>                     SkinnedIndexFreeList;
        uint32                              SkinnedIndexExtent = 0;
        uint32  AllocateSkinnedIndex();
        void    FreeSkinnedIndex(uint32 Index);
        void    FreeInstanceSlot(uint32 Slot);
        void    MarkInstanceDirty(uint32 Slot);
        void    MarkStaticDirty(uint32 Slot);

        // Caches the skeleton's bone count; the arena slice is assigned by the render gather.
        void    SetBoneCount(uint32 Index, uint32 Count);
        // Interns a surface's LOD table; identical tables collapse to one entry.
        uint32  InternSurfaceDesc(const FResolvedSurface& Surface);

        /** Contiguous run at the end of the retained arrays, bypassing the free list so the block is dense. */
        uint32  AllocateInstanceSlotBlock(uint32 Count);

        // Keyed by mesh, so two terrains sharing a species share its block rather than each reserving one.
        THashMap<const CStaticMesh*, FGrassSpeciesBinding> GrassSpecies;

        void    SyncEntity(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity,
                           EPrimitiveSource Source, EPrimitiveDirty Flags);
        void    SyncFoliage(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity,
                            EPrimitiveDirty Flags);
        void    FullRescan(ECS::FRegistry& Registry);
        void    PollUnhookedSources(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);
        void    PollSkeletalBoneRanges(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);

        void    RebuildWorldBounds(uint32 Index);
        void    RebuildWorldBounds(uint32 Index, const FMatrix4& Transform);

        TVector<FScenePrimitive>            Primitives;
        TVector<FVector4>                   Bounds;
        TVector<FPrimitiveCullData>         CullData;
        TVector<uint64>                     Keys;           // parallel: index -> key, for swap-remove fixup

        TVector<uint64>                     ResolveKeys;

        // Intrusive list of primitives per resolve handle, so a rebuilt entry re-syncs only its own users.
        TVector<uint32>                     HandleListHead;
        TVector<uint32>                     NextByHandle;
        TVector<uint32>                     PrevByHandle;
        uint32                              LastSweptTableGeneration = 0;
        void    LinkHandle(uint32 Index, uint32 Handle);
        void    UnlinkHandle(uint32 Index, uint32 Handle);

        // Skeletal primitives still waiting on a skeleton; gates the per-frame bone poll.
        uint32                              BonelessSkinnedCount = 0;

        static uint64 PackResolveKey(uint32 Handle, uint32 Generation)
        {
            return (uint64)Handle | ((uint64)Generation << 32);
        }

        // Scratch: resolve handle -> that entry's current Generation, filled lazily during the sweep.
        TVector<uint32>                     GenSnapshot;

        TVector<FSurfaceBinding>            Bindings;
        uint32                              DeadBindings = 0;
        void                                CompactBindings();

        // Dead spans a removed foliage entity left whole, as (base, count), handed to the next fresh bind that fits.
        TVector<FUIntVector2>               BindingFreeRanges;
        uint32                              FreeRangeBindings = 0;
        uint32                              TakeBindingRange(uint32 Count);

        // Dead space waiting in a free range is reused rather than wasted, unless it outgrows what is live.
        bool                                ShouldCompactBindings() const;

        FSceneBatchRegistry                 Batches;

        struct FBindingMemo
        {
            uint32                      Generation = Constants::kIndexNoneU32;   // FResolvedMesh::Generation it was built at
            uint64                      RecycleGeneration = 0;
            TVector<FSurfaceBinding>    Protos;             // InstanceSlot unused
        };
        TVector<FBindingMemo>               BindingMemoByHandle;

        TVector<uint64>                     RetryScratch;

        static constexpr uint32 kLinkedSources = (uint32)EPrimitiveSource::Foliage;

        struct FPrimitiveLink
        {
            uint32 Entity = Constants::kIndexNoneU32;
            uint32 Index[kLinkedSources] = { Constants::kIndexNoneU32, Constants::kIndexNoneU32, Constants::kIndexNoneU32 };
        };
        static_assert(sizeof(FPrimitiveLink) == 16, "Four links per cache line is the point of this table.");
        static_assert(kLinkedSources == 3, "FPrimitiveLink::Index initializer must match kLinkedSources.");

        TVector<FPrimitiveLink>             LinksByEntityIndex;

        // ~0u when this entity owns no primitive for that source. Source must not be Foliage.
        uint32  FindLinked(ECS::FEntity Entity, EPrimitiveSource Source) const;
        // Bitmask of sources this entity owns a primitive for; 0 when it is not renderable at all.
        uint8   GetSourceMask(ECS::FEntity Entity) const;
        void    SetLink(ECS::FEntity Entity, EPrimitiveSource Source, uint32 Index);
        void    ClearLink(ECS::FEntity Entity, EPrimitiveSource Source);

        struct FFoliageTypeResolve
        {
            const TVector<FResolvedSurface>*    Surfaces = nullptr;
            uint32                              Generation = 0;
            uint32                              MeshletHeaderSlot = 0;
            uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
            float                               MaxDrawDistance = 0.0f;
            EInstanceFlags                      BaseFlags = EInstanceFlags::None;
            bool                                bCastShadow = true;
            bool                                bCastFarShadow = false;
            float                               ThinningStartDistance = 0.0f;
        };
        TVector<FFoliageTypeResolve>        FoliageTypeScratch;
        TVector<bool>                       FoliageTypeChangedScratch;

        /**
         * A painted instance is NOT a primitive. Everything a FScenePrimitive would carry for one is
         * either shared by every instance of its foliage type (surfaces, meshlet slot, flags, cull
         * distance, resolve identity) or already sitting in the component's bake (world transform, world
         * sphere). What is genuinely per instance is the span of instance slots it owns, so that is all
         * this holds -- 8 bytes against the ~250 a primitive plus its parallel arrays and hash entry cost.
         *
         * The consequence that matters beyond the bytes: a field of foliage no longer lengthens any of the
         * whole-table sweeps, that is the resolve-staleness walk, the bone-slice sweep, and the retry list.
         */
        struct FFoliageInstanceRef
        {
            uint32 BindingBase  = 0;
            uint16 SurfaceCount = 0;
            uint16 TypeRow      = 0xFFFFu;
        };
        static_assert(sizeof(FFoliageInstanceRef) == 8, "One instance per 8 bytes is the point of this table.");

        struct FFoliageEntityState
        {
            TVector<FFoliageInstanceRef>    Instances;
            uint32                          SyncedBakeSerial = 0;
            // Resolve identity per TYPE, which is what the staleness sweep compares instead of per instance.
            TVector<uint64>                 TypeResolveKeys;
        };
        THashMap<ECS::FEntity, FFoliageEntityState> FoliageByEntity;

        void    ReleaseFoliageInstance(FFoliageInstanceRef& Ref);
        // Removed foliage of one sync, released together so a streaming burst spreads over workers.
        TVector<FFoliageEntityState>        PendingFoliageReleases;
        void    FlushFoliageReleases();
        void    RefreshFoliageInstance(const FFoliageInstanceRef& Ref, const FFoliageTypeResolve& Type,
                                       const FFoliageBakedInstance& Instance, uint32 EntityID, bool bHidden);
        // Flips the active bit of instances a game hid or showed since the last frame, leaving the bake alone.
        void    ApplyFoliageVisibilityEdits(ECS::FRegistry& Registry);
        // First sync of an entity, so every instance binds and its slots can be filled in parallel.
        void    BindFreshFoliage(FFoliageEntityState& State, const SFoliageComponent& Foliage, const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID);
        static void BuildFoliageEntries(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type,
                                        const FFoliageBakedInstance& Instance, uint32 EntityID, bool bHidden,
                                        FInstanceCullEntry& OutCull, FTransform3x4& OutTransform, FInstanceStatic& OutStatic);
        bool    FoliageBindingsMatchMemo(const FFoliageInstanceRef& Ref, const FBindingMemo* Memo) const;
        // Appends a retry target per foliage entity whose type resolve moved. Pairs with the primitive sweep.
        void    SweepFoliageResolves(FMeshResolveCache& Cache);

        // Memo per type for one SyncFoliage call; parallel to FoliageTypeScratch.
        TVector<const FBindingMemo*>        FoliageMemoScratch;

        // Instance slot per new binding in one BindFreshFoliage call, so the parallel fill reads what was handed out.
        TVector<uint32>                     FoliageSlotScratch;

        // One per block of instances in FlushFoliageReleases, so the gather runs on workers.
        struct FFoliageReleaseBlock
        {
            uint32                  State       = 0;
            uint32                  Begin       = 0;
            uint32                  End         = 0;
            uint32                  NumBindings = 0;
            uint32                  FirstBase   = 0;
            uint32                  EndBase     = 0;
            uint32                  SlotOffset  = 0;
            bool                    bContiguous = true;
            TVector<FUIntVector3>   Runs;
        };
        TVector<FFoliageReleaseBlock>       FoliageReleaseBlocks;

        // Instances one SyncFoliage call refreshes, and the dirty surfaces each produced when that ran in parallel.
        TVector<uint32>                     FoliageRefreshScratch;
        TVector<uint32>                     FoliageRefreshMasks;

        void    RefreshFoliageInstances(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                        const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID,
                                        const FFoliageTypeResolve& UnresolvedType);

        TVector<FRenderDirtyTracker::FEntry> DrainScratch;
        TVector<CMaterialInterface*>        OverrideScratch;

        struct FCoalescedEntity
        {
            uint32          Entity = Constants::kIndexNoneU32;
            EPrimitiveDirty Flags[(uint32)EPrimitiveSource::Num] = {};
        };

        struct FCoalesceSlot
        {
            uint32 Stamp = 0;
            uint32 Index = Constants::kIndexNoneU32;
        };

        TVector<FCoalescedEntity>   CoalescedScratch;
        TVector<FCoalesceSlot>      CoalesceByEntityIndex;
        uint32                      CoalesceStamp = 0;

        // Folds DrainScratch into CoalescedScratch.
        void CoalesceDrain(TSpan<const ECS::FEntity> Moved);

        // Grows the append-target arrays once, from the coalesced record set, before the apply pass runs.
        void ReserveForDrain();

        void PartitionDrain();
        void ApplyStructuralRecords(ECS::FRegistry& Registry, const FSyncPools& Pools);
        void ApplyTransformRecords(const FSyncPools& Pools);

        void ApplyTransformRecord(const FSyncPools& Pools, uint32 RecordIndex, TVector<uint32>* OutDirty);

        // One entity's new matrix onto every linked source whose bit is set in SourceBits.
        void ApplyEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint8 SourceBits, TVector<uint32>* OutDirty);

        // True when this frame's tracker entries gave the entity a coalesced record of its own.
        bool HasCoalescedRecord(ECS::FEntity Entity) const;

        // Movers with no record skip coalescing entirely and are applied in parallel after the structural pass.
        void ApplyMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved);
        // Every moved entity through the CPU copies, which upload as dirty slots.
        void ApplyMovedLegacy(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved);
        // Opens a claim pass and returns its stamp, so a mover listed twice in it is applied once.
        uint32 BeginMovedClaims();

        // Per entity index, the stamp of the pass that claimed it, so a mover listed twice is applied once.
        TVector<uint32>             MovedClaimByEntityIndex;
        uint32                      MovedClaimStamp = 0;
        TVector<TVector<ECS::FEntity>> ParallelOverlaps;

        // Indices into CoalescedScratch, filled by PartitionDrain.
        TVector<uint32>             TransformRecords;
        TVector<uint32>             StructuralRecords;

        // One cache line per worker, since neighboring vectors bounced the line on every push.
        struct CACHE_ALIGN FDirtySlotBucket
        {
            TVector<uint32>       Slots;
            TVector<ECS::FEntity> Streamed;
            TVector<uint32>       StreamedBlocks;
            TVector<ECS::FEntity> Stopped;
        };
        TVector<FDirtySlotBucket>   ParallelDirtySlots;
        void EnsureParallelBuckets();

        // Writes the record for a mover the stream can take, and reports false for one that needs the CPU path.
        bool StreamEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint32 Record, FDirtySlotBucket& Bucket);

        // One job thread's share of the move-time stream, so a mover allocates records without touching another's line.
        static constexpr uint32 kMoveStreamChunk = 256;
        struct CACHE_ALIGN FMoveStreamLane
        {
            uint32                 Next = 0;
            uint32                 End = 0;
            // Movers that streamed last frame too, which lets the catch-up skip its scan when none stopped.
            uint32                 Continuing = 0;
            TVector<ECS::FEntity>  Streamed;
            TVector<uint32>        Blocks;
            // Moved again after their record was written, so the seal rewrites them from the final pose.
            TVector<ECS::FEntity>  Rewrites;
        };

        static bool WriteFlatMove(void* Context, ECS::FEntity Entity, const FTransform& World);
        bool WriteMove(ECS::FEntity Entity, const FTransform& World);
        // Disarms, closes every lane and returns how many records the movers used, zero when they have to be replayed.
        uint32 SealMoveStream(ECS::FRegistry& Registry);
        // A structural sync can move the entity's slot, so a record written for it this frame must not land.
        void KillMoveRecord(ECS::FEntity Entity);
        void OpenStreamFrame();

        TVector<FMoveStreamLane>    MoveLanes;
        ECS::Utils::FFlatMoveWriter MoveWriter;
        std::atomic<uint32>         MoveStreamNext{ 0 };
        uint32                      MoveTimeRecords = 0;
        bool                        bMoveStreamArmed = false;
        bool                        bStreamFrameOpen = false;
        // A foreground toggle after the movers wrote changes which of them may stream at all.
        bool                        bDiscardMoveStream = false;
        TVector<ECS::FEntity>       MoveReplay;
        uint64                      ContinuingStreams = 0;
        // False after a frame whose stamps no longer match its lists one to one, so the next catch-up has to scan.
        bool                        bStreamListsExact = false;
        void StreamMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved);
        // A streamed mover that stopped still holds CPU copies from before it streamed, so it takes the CPU path once.
        void CatchUpStoppedStreams(const FSyncPools& Pools, uint32 MovedStamp, bool bNoneStopped);

        // What the stream needs of an entity, kept apart so a mover reads one dense entry instead of its primitive and binding.
        enum class EStreamKind : uint32
        {
            Skip,
            CPUPath,
            Stream,
        };
        struct FStreamCacheEntry
        {
            uint32      Generation = 0;
            uint32      Entity = 0;
            uint32      Slot = 0;
            EStreamKind Kind = EStreamKind::Skip;
            FVector3    LocalCenter = FVector3(0.0f);
            float       Radius = 0.0f;
        };
        static_assert(sizeof(FStreamCacheEntry) == 32, "Two cache entries per line.");
        FStreamCacheEntry BuildStreamCacheEntry(ECS::FEntity Entity) const;
        FStreamCacheEntry LookupStreamCacheEntry(ECS::FEntity Entity);
        // A structural sync of the entity, which may move its slot or bounds.
        void InvalidateStreamCache(ECS::FEntity Entity);
        // Everything at once, for a rescan, a reset or a foreground toggle.
        void InvalidateAllStreamCache();
        TVector<FStreamCacheEntry>  StreamCache;
        uint32                      StreamCacheGeneration = 1;

        FTransformStreamTarget      StreamTarget;
        uint32                      StreamCount = 0;
        // Bumped every frame the stream opens, and per entity index the frame that last streamed it and its record.
        struct FMoveRecordStamp
        {
            uint32 Frame  = 0;
            uint32 Record = kNoStreamSlot;
        };
        // Set on a stamp's record once its rewrite is queued, so a mover moved many times queues one.
        static constexpr uint32 kMoveRewriteQueued = 0x80000000u;
        uint32                      StreamFrame = 0;
        TVector<FMoveRecordStamp>   MoveRecords;
        // A frame's streamed movers kept as the lists their threads built, which are swapped in rather than copied into one.
        struct FStreamedLists
        {
            TVector<TVector<ECS::FEntity>> Lists;
            uint32                         Used = 0;
            uint64                         Count = 0;

            bool IsEmpty() const { return Used == 0; }

            // Takes List's entities and hands it back an emptied vector that keeps its capacity.
            void Take(TVector<ECS::FEntity>& List)
            {
                if (List.empty())
                {
                    return;
                }
                if (Used == (uint32)Lists.size())
                {
                    Lists.emplace_back();
                }
                Count += List.size();
                Lists[Used].clear();
                Lists[Used].swap(List);
                ++Used;
            }

            void Clear()
            {
                for (uint32 Index = 0; Index < Used; ++Index)
                {
                    Lists[Index].clear();
                }
                Used  = 0;
                Count = 0;
            }
        };
        FStreamedLists              StreamedThisFrame;
        FStreamedLists              StreamedLastFrame;
        TVector<ECS::FEntity>       StoppedScratch;
        TVector<uint32>             StreamedBlocks;

        // Folds the per-worker lists back into DirtyInstanceSlots, or gives up and re-sends everything.
        void MergeParallelDirtySlots();

        FMatrix4                            ForegroundTransform = FMatrix4(1.0f);
        float                               ForegroundRadiusScale = 1.0f;
        bool                                bForegroundActive = false;
        // Few enough that a camera move refreshes every one of them.
        TVector<uint64>                     ForegroundKeys;

        TVector<FInstanceCullEntry>         RetainedCullEntries;
        TVector<FTransform3x4>              RetainedTransforms;
        TVector<FInstanceStatic>            RetainedStatic;

        TVector<uint32>                     InstanceFreeSlots;
        TVector<uint32>                     DirtyInstanceSlots;   // cull entry + transform
        TVector<uint32>                     DeactivatedSlots;     // freed, so only the active bit changes

        // One past the highest bone ever handed out. The arena's size, and it only grows.
        TVector<uint32>                     DirtyStaticSlots;     // static payload
        bool                                bFullInstanceUpload = true;
        // Tracked apart from the transforms, so a moving crowd does not re-send payload that never changed.
        bool                                bFullStaticUpload = true;

        TVector<FSurfaceDescGPU>            SurfaceDescs;
        THashMap<uint64, TVector<uint32>>   SurfaceDescByHash;
        bool                                bSurfaceDescsDirty = true;
        // See GetMaxSurfaceDescMeshlets. Only ever grows while the table does, and is reset with it.
        uint32                              MaxSurfaceDescMeshlets = 0;

        void                                ReleaseBoneSlice(FScenePrimitive& Prim);

        void                                UpdateMeshletVisibility(uint32 Slot);
        // Visibility words the slot's largest LOD needs, zero for an inactive slot.
        uint32                              MeshletVisibilityWordsOf(uint32 Slot) const;
        void                                ReleaseMeshletVisibility(uint32 Slot);
        void                                ResetMeshletVisibility();

        // Per retained slot, x the first word and y the word count.
        TVector<FUIntVector2>               SlotMeshletVisibility;
        // Indexed by word count, since a streaming burst frees a range per foliage slot and a hash per free showed.
        TVector<TVector<uint32>>            MeshletVisibilityFreeLists;
        void                                FreeMeshletVisibilityRange(uint32 Base, uint32 Words);
        uint32                              MeshletVisibilityExtent = 1;
        TVector<uint32>                     MeshletVisibilityScratch;

        TVector<uint32>                     SkeletalIndices;
        // Adds and removes only; StructureGeneration also moves on a transform, which would thrash this.
        uint32                              SkeletalSetGeneration = 0;
        uint32                              SkeletalIndicesGeneration = Constants::kIndexNoneU32;
        THashMap<uint32, TVector<uint32>>   BoneSliceFreeLists;
        uint32                              BoneSliceExtent = 0;
        uint32                              BoneSliceSweepCursor = 0;


        uint64                              StructureGeneration = 1;
        uint32                              SkinnedCount = 0;
        uint32                              CustomDepthCount = 0;
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
