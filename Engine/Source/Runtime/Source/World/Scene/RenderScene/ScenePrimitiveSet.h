#pragma once

#include "World/ECS/EntityMap.h"
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
#include "World/Scene/RenderScene/RenderDirtyTracker.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Lumina.h"

namespace Lumina
{
    class CWorld;
    class CGrassType;
    struct SFoliageComponent;
    struct FFoliageBakedInstance;
    struct FDynamicMeshRenderData;

    class FSceneBatchRegistry
    {
    public:

        struct FDeferredMaterialSlot
        {
            uint32      MaterialIndex;
            FShaderH    DeferredShader;
        };

        struct FBatch
        {
            FDrawBatchKey                   Key = {};
            // Copied out of the resolve at bind time so the frame path never re-reads the shared entry.
            FShaderH                        PixelShader = {};
            FShaderH                        VertexShader = {};
            FShaderH                        MeshShaderShadow = {};
            FShaderH                        MeshShaderBase = {};
            FShaderH                        VisBufferMeshShader = {};
            FShaderH                        VisBufferMeshShaderMasked = {};
            FShaderH                        MaskedVisBufferPixelShader = {};
            FShaderH                        MeshShaderShadowMasked = {};
            FShaderH                        ShadowMaskedPixelShader = {};

            uint32                          RefCount = 0;

            // Bound instances, not visible ones, since the SPEC_SKINNED variant covers everything the GPU cull can emit into this batch.
            uint32                          SkinnedRefCount = 0;
            uint32                          StaticRefCount = 0;

            // A batch is a pipeline serving every material that compiles to it, so materials live per instance and per deferred slot.
            TVector<FDeferredMaterialSlot>  DeferredMaterials;
        };

        uint32 FindOrAddBatch(const FResolvedSurface& Surface);

        void AddBatchRef(uint32 BatchIndex, bool bSkinned);
        void AddBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count);

        void ReleaseBatchRef(uint32 BatchIndex, bool bSkinned);
        void ReleaseBatchRefs(uint32 BatchIndex, bool bSkinned, uint32 Count);

        uint32          Num() const { return (uint32)Batches.Num(); }
        FBatch&         Get(uint32 Index) { return Batches[Index]; }
        const FBatch&   Get(uint32 Index) const { return Batches[Index]; }

        // Moves only when a slot is freed, which is the one event that can make a cached batch index lie.
        uint64          GetRecycleGeneration() const { return RecycleGeneration; }

        bool            IsLive(uint32 Index) const { return Batches[Index].RefCount != 0u; }

        void Reset();

    private:

        void RetireBatch(uint32 BatchIndex);

        TVector<FBatch>                     Batches;
        THashMap<uint64, TVector<uint32>>   BatchesByHash;
        TVector<uint32>                     FreeBatches;
        uint64                              RecycleGeneration = 1;
    };

    // Where one surface of a primitive draws: resolved once at bind, read every frame.
    struct FSurfaceBinding
    {
        uint32          BatchIndex;
        uint32          InstanceSlot;
        uint32          SurfaceDescIndex;
        uint32          MaterialIndex;
        EInstanceFlags  MaterialFlags;
        bool            bMaterialCastsShadows;
        // Held so the release decrements the same counter the bind incremented, whatever the primitive has become since.
        bool            bSkinned = false;

        // Mesh-local world size of one UV tile, 0 when unknown, which the texture-streaming gather turns into a required resolution.
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

    struct FScenePrimitive
    {
        FVector3                            LocalCenter = FVector3(0.0f);
        float                               LocalRadius = 0.0f;
        float                               BoundsScale = 1.0f;
        // Largest world axis scale, kept current for skeletal primitives, whose LOD pick reads it.
        float                               WorldMaxScale = 1.0f;
        uint32                              BindingBase = 0;
        uint32                              SurfaceCount = 0;
        uint32                              RenderLayers = RenderLayers::kDefaultMask;

        uint32                              MeshletHeaderSlot = 0;

        const TVector<FResolvedSurface>*    Surfaces = nullptr;

        ECS::FEntity                        Entity = ECS::NullEntity;
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
        // Whether ForegroundKeys holds this primitive, so tracking it is one test rather than a search.
        bool                                bForegroundTracked = false;

        // Position in the skeletal index list, so a removal swaps it out without a search.
        uint32                              SkeletalSlot = Constants::kIndexNoneU32;

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

    // Cull-hot per-primitive data, kept in its own array so the reject path streams it densely.
    struct FPrimitiveCullData
    {
        float   MaxDrawDistance = 0.0f;
        uint32  bCastShadow = 0u;
    };

    // Bone slices recycled by exact size, with an extent that only grows within a level so a base handed out last frame stays valid.
    class FBoneSliceArena
    {
    public:

        uint32  Acquire(uint32 Count);
        void    Release(uint32 Base, uint32 Count);
        void    Reset();

        uint32  GetExtent() const { return Extent; }

    private:

        // Indexed by bone count.
        TVector<TVector<uint32>>    FreeLists;
        uint32                      Extent = 0;
    };

    class FScenePrimitiveSet
    {
    public:

        FScenePrimitiveSet() = default;
        ~FScenePrimitiveSet() = default;
        LE_NO_COPYMOVE(FScenePrimitiveSet);

        // Applies this set's dirty entries and the frame's moved entities at O(changed), except on a requested full rescan.
        void Sync(CWorld& World, TSpan<const ECS::FEntity> Moved = {});

        // Drops every primitive and re-arms a full rescan, registering with the registry's dirty tracker on first use.
        void Reset(ECS::FRegistry* Registry);

        // Drops every primitive and leaves the registry's dirty tracker, for a renderer going away. Call before removing the moved-transform consumer.
        void Shutdown(ECS::FRegistry& Registry);

        // The filtered moved-transform consumer the renderer drains, which this set points at every entity it draws.
        void SetMovedTransformConsumer(ECS::FRegistry& Registry, uint32 Consumer);

        // Remaps Foreground-layer instances for this camera, or draws them in place when it has no foreground. Call before Sync.
        void SetForegroundView(const FViewVolume& View, const FViewLayerSettings& Layers);

        // Moved instances that fit the stream write their transform there instead of into the CPU copies. Call before Sync, or pass an empty target.
        void SetTransformStream(const FTransformStreamTarget& Target) { StreamTarget = Target; }

        // Records the last Sync wrote, one per moved entity, with kNoStreamSlot for any that took the CPU path.
        uint32 GetTransformStreamCount() const { return StreamCount; }

        // Records the movers themselves wrote into the last Sync's stream, as they moved.
        uint32 GetMoveTimeRecords() const { return MoveTimeRecords; }

        // Flat movers write their own records into Target until the next Sync folds them in, so arm it only between frames, with a slice the GPU is done with.
        void ArmMoveStream(ECS::FRegistry& Registry, const FTransformStreamTarget& Target, uint32 InMovedConsumer);

        // Takes the writer back off the registry, for a renderer going away while armed.
        void DisarmMoveStream(ECS::FRegistry& Registry);

        bool IsMoveStreamArmed() const { return bMoveStreamArmed; }

        // Blocks of slots holding an instance streamed this frame, sorted, whose CPU spheres trail the GPU.
        const TVector<uint32>& GetStreamedBlocks() const { return StreamedBlocks; }

        uint32                              Num() const { return (uint32)Primitives.Num(); }
        const FScenePrimitive*              GetPrimitives() const { return Primitives.GetData(); }
        // The gather stamps per-primitive upload state through this; entries are disjoint per worker.
        FScenePrimitive*                    GetMutablePrimitives() { return Primitives.GetData(); }
        const FVector4*                     GetBounds() const { return Bounds.GetData(); }
        const FPrimitiveCullData*           GetCullData() const { return CullData.GetData(); }
        const FSurfaceBinding*              GetBindings() const { return Bindings.GetData(); }

        FSceneBatchRegistry&                GetBatches() { return Batches; }
        const FSceneBatchRegistry&          GetBatches() const { return Batches; }

        uint32                              GetSkinnedPrimitiveCount() const { return (uint32)SkeletalIndices.Num(); }

        // Primitives drawn into custom depth, so the renderer adds the view and the pass only while one exists.
        uint32                              GetCustomDepthPrimitiveCount() const { return CustomDepthCount; }

        // Dense skeletal primitive indices in no particular order, kept in step with every add and remove.
        const TVector<uint32>&              GetSkeletalIndices() const { return SkeletalIndices; }

        const FInstanceCullEntry*   GetRetainedCullEntries() const { return RetainedCullEntries.GetData(); }
        const FTransform3x4*        GetRetainedTransforms() const { return RetainedTransforms.GetData(); }
        const FInstanceStatic*      GetRetainedStatic() const     { return RetainedStatic.GetData(); }
        const FInstanceProxy*       GetRetainedProxies() const    { return RetainedProxies.GetData(); }
        uint32                      GetRetainedSlotCount() const  { return (uint32)RetainedCullEntries.Num(); }
        uint32                      GetSkinnedIndexExtent() const { return SkinnedIndexExtent; }

        // One grass type on one terrain layer, stable across edits to the rest of the material's grass list.
        struct FGrassSpeciesKey
        {
            ECS::FEntity      Terrain    = ECS::NullEntity;
            const CGrassType* Type       = nullptr;
            uint32            LayerIndex = 0;
            uint32            Occurrence = 0;

            bool operator==(const FGrassSpeciesKey&) const = default;

            friend uint64 GetTypeHash(const FGrassSpeciesKey& Key)
            {
                const uint64 TypeBits = (uint64)(uintptr_t)Key.Type * 0x9E3779B97F4A7C15ull;
                return ECS::GetTypeHash(Key.Terrain) ^ TypeBits ^ ((uint64)Key.LayerIndex << 48) ^ ((uint64)Key.Occurrence << 40);
            }
        };

        // What a GPU scatter needs to write retained instances for one grass species.
        struct FGrassSpeciesBinding
        {
            const CStaticMesh* Mesh   = nullptr;
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
            uint32 SeenSerial         = 0;
        };

        // A contiguous run of retained slots the GPU scatter fills each frame, leaving unused ones inactive. Idempotent per key while the mesh is the same and Capacity fits.
        FGrassSpeciesBinding AcquireGrassSpecies(const FGrassSpeciesKey& Key, CStaticMesh* Mesh, uint32 ResolveHandle, uint32 Capacity);

        // Frees every block no AcquireGrassSpecies asked for since the last call, which covers dead, disabled and grassless terrains alike.
        void ReleaseUnseenGrassSpecies();

        // Keeps a terrain's blocks through a frame it skips, such as one whose textures are still being created.
        void KeepGrassSpecies(ECS::FEntity Terrain);

        struct FMeshletDemand
        {
            uint64 Words    = 0;
            uint64 Meshlets = 0;
        };

        // Every grass blade active at its largest LOD, which the CPU cull entries cannot show since the scatter activates them on the GPU.
        FMeshletDemand GetGrassMeshletDemand() const;
        uint32         GetGrassSlotCount() const { return GrassSlotCount; }

        // Retained slot ranges (base, count) the grass scatter fills on the GPU, which the CPU arrays only hold as zeros.
        void GetGPUWrittenSlotRanges(TVector<FUIntVector2>& Out) const
        {
            Out.Reset();
            for (const auto& [Key, Binding] : GrassSpecies)
            {
                Out.Emplace(Binding.InstanceSlotBase, Binding.Capacity);
            }
        }

        // Points a species' block at its surface's current batch, material and surface desc.
        void BindGrassSurface(FGrassSpeciesBinding& Binding, const FResolvedMesh& Resolved);
        void ReleaseGrassBlock(const FGrassSpeciesBinding& Binding);
        uint32 TakeGrassBlock(uint32 Capacity);
        void   AddGrassFreeBlock(uint32 Base, uint32 Count);

        // Touches only Primitives[Index], so this half is parallel-safe; kNoBoneSlice means it needs one.
        uint32                      TouchBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber);
        // Recycled by EXACT size and mutates the free lists, so this half must stay serial.
        uint32                      AcquireBoneSlice(uint32 Index, uint32 Count, uint32 FrameNumber);
        // Rolling sweep over the skeletal primitives, a slab per call, so the cost never scales with the scene at once.
        void                        ReleaseStaleBoneSlices(uint32 FrameNumber, uint32 GraceFrames);
        uint32                      GetBoneSliceExtent() const { return BoneSlices.GetExtent(); }

        // The skeletal primitive Entity renders through, ~0u when it has none.
        uint32                      FindSkeletalPrimitive(ECS::FEntity Entity) const { return FindLinked(Entity, EPrimitiveSource::SkeletalMesh); }

        const FSurfaceDescGPU*      GetSurfaceDescs() const     { return SurfaceDescs.GetData(); }
        uint32                      GetSurfaceDescCount() const { return (uint32)SurfaceDescs.Num(); }
        bool                        AreSurfaceDescsDirty() const { return bSurfaceDescsDirty; }
        void                        ClearSurfaceDescsDirty()     { bSurfaceDescsDirty = false; }

        uint32                      GetMaxSurfaceDescMeshlets() const { return MaxSurfaceDescMeshlets; }

        const TVector<uint32>&      GetDirtyInstanceSlots() const { return DirtyInstanceSlots; }
        const TVector<uint32>&      GetDirtyStaticSlots() const   { return DirtyStaticSlots; }
        const TVector<uint32>&      GetDeactivatedSlots() const   { return DeactivatedSlots; }
        void                        ClearDirtyInstanceSlots()     { DirtyInstanceSlots.Reset(); DirtyStaticSlots.Reset(); DeactivatedSlots.Reset(); }
        // True after a reset or when most slots changed at once, so the whole array is re-sent.
        bool                        NeedsFullInstanceUpload() const { return bFullInstanceUpload; }
        // The partial paths send only the dirty slots, so re-sending all of them pays once nearly every slot changed.
        static constexpr bool       ShouldResendAllInstances(SIZE_T DirtySlots, SIZE_T SlotCount) { return DirtySlots * 8 >= SlotCount * 7; }
        bool                        NeedsFullStaticUpload() const   { return bFullStaticUpload; }
        void                        ClearFullInstanceUpload()       { bFullInstanceUpload = false; bFullStaticUpload = false; }

        // Sizes every changed slot's meshlet visibility from its surface's largest LOD. Serial, before the dirty lists are consumed.
        void                        AssignMeshletVisibility();
        uint32                      GetMeshletVisibilityExtent() const { return MeshletVisibilityExtent; }
        uint64                      GetLargestLODMeshletTotal() const { return LargestLODMeshletTotal; }

        void                        NotifyResolveTableChanged() { bResolveTableChanged = true; }

    private:

        struct FSyncPools;
        struct FBindingMemo;
        struct FDirtySlotBucket;
        struct FFoliageTypeResolve;
        struct FFoliageEntityState;
        struct FPrimitiveSyncWork;

        static constexpr uint32 kKeySourceShift = 32;
        static constexpr uint32 kLinkedSources  = (uint32)EPrimitiveSource::Foliage;

        static uint64           MakeKey(ECS::FEntity Entity, EPrimitiveSource Source) { return (uint64)Entity.Value | ((uint64)Source << kKeySourceShift); }
        static ECS::FEntity     KeyEntity(uint64 Key) { return (ECS::FEntity)(uint32)(Key & 0xFFFFFFFFull); }
        static EPrimitiveSource KeySource(uint64 Key) { return (EPrimitiveSource)((Key >> kKeySourceShift) & 0xFFull); }

        static uint64 PackResolveKey(uint32 Handle, uint32 Generation) { return (uint64)Handle | ((uint64)Generation << 32); }

        //~ Primitives

        uint32  AddPrimitive(ECS::FEntity Entity, EPrimitiveSource Source);
        void    RemovePrimitive(uint32 Index);
        void    SetBoneCount(uint32 Index, uint32 Count);
        void    LinkHandle(uint32 Index, uint32 Handle);
        void    UnlinkHandle(uint32 Index, uint32 Handle);
        // Writes and returns the world cull sphere, and the max scale of a skeletal primitive.
        FVector4 RebuildWorldBounds(uint32 Index, const FMatrix4& Transform);

        // One batch of structural changes: classified and allocated serially, read and refreshed in parallel when large.
        void    SyncPrimitives(ECS::FRegistry& Registry, const FSyncPools& Pools, TSpan<const FRenderDirtyTracker::FEntry> Requests);
        void    ClassifyPrimitiveSyncs(ECS::FRegistry& Registry, const FSyncPools& Pools, TSpan<const FRenderDirtyTracker::FEntry> Requests);
        void    ReadPrimitiveState(const FSyncPools& Pools, uint32 Work);
        // False when the component is gone, which leaves the primitive unbound until it is removed.
        bool    ReadMeshState(const FSyncPools& Pools, FPrimitiveSyncWork& Work);
        bool    ReadDynamicMeshState(const FSyncPools& Pools, FPrimitiveSyncWork& Work);
        void    BindPrimitiveState(uint32 Work);
        void    RefreshSyncedPrimitives(const FSyncPools& Pools);

        TVector<FScenePrimitive>            Primitives;
        TVector<FVector4>                   Bounds;
        TVector<FPrimitiveCullData>         CullData;
        TVector<uint64>                     ResolveKeys;

        // Intrusive list of primitives per resolve handle, so a rebuilt entry re-syncs only its own users.
        TVector<uint32>                     HandleListHead;
        TVector<uint32>                     NextByHandle;
        TVector<uint32>                     PrevByHandle;
        uint32                              LastSweptTableGeneration = 0;

        // Skeletal primitives still waiting on a skeleton; gates the per-frame bone poll.
        uint32                              BonelessSkinnedCount = 0;
        uint32                              CustomDepthCount = 0;

        TVector<uint32>                     SkeletalIndices;
        uint32                              BoneSliceSweepCursor = 0;
        FBoneSliceArena                     BoneSlices;

        // What one primitive's sync read in parallel, applied serially by the bind half.
        struct FPrimitiveSyncWork
        {
            // Dynamic mesh data the read let go of, which may be its last reference and so is freed on the game thread.
            TSharedPtr<FDynamicMeshRenderData>  OldDynamicData;
            TSharedPtr<FDynamicMeshRenderData>  NewDynamicData;
            // The request it came from, and the primitive it resolved to once every add and remove settled.
            uint32                              Request = 0;
            uint32                              Index = 0;
            uint32                              OldHandle = INVALID_MESH_RESOLVE_HANDLE;
            uint32                              NewBoneCount = 0;
            const TVector<FResolvedSurface>*    OldSurfaces = nullptr;
            uint32                              OldGeneration = 0;
            bool                                bHadCustomDepth = false;
            bool                                bContentChanged = false;
            bool                                bPresent = false;
        };
        TVector<FPrimitiveSyncWork>         PrimitiveSyncWork;
        TVector<FRenderDirtyTracker::FEntry> PrimitiveSyncScratch;
        TVector<FRenderDirtyTracker::FEntry> PrimitiveMoveScratch;

        //~ Bindings

        // Binds one primitive's surfaces to the batch registry. Only called when its resolve changed.
        void    BindSurfaces(uint32 Index);
        void    ReleaseBindings(uint32 Index);
        // Foliage passes false for bAssignSkinnedIndex, since only a skinned primitive is gathered for frame data.
        uint32  AppendBindingSpan(const FBindingMemo& Memo, bool bSkinned, bool bAssignSkinnedIndex);
        // Batch references go to DeferredBatchReleases when given, so a rebind takes its new references before any old batch can retire.
        void    ReleaseBindingSpan(uint32 Base, uint32 Count, TVector<FUIntVector3>* DeferredBatchReleases = nullptr);
        void    FlushDeferredBatchReleases(TVector<FUIntVector3>& DeferredBatchReleases);
        // Rewrites a span of the same shape to the memo's batches, materials and surface descs while it keeps its slots.
        void    PatchBindingSpan(uint32 Base, const FBindingMemo& Memo, bool bSkinned, TVector<FUIntVector3>& DeferredBatchReleases);

        const FBindingMemo* EnsureBindingMemo(uint32 ResolveHandle, uint32 Generation, const TVector<FResolvedSurface>& Surfaces);

        // Content compare for sources whose resolve identity cannot move, which is dynamic meshes.
        bool    BindingsMatchSurfaces(uint32 Index, const TVector<FResolvedSurface>& Surfaces) const;
        bool    SpanMatchesMemo(uint32 Base, uint32 Count, const FBindingMemo* Memo, bool bSkinned) const;

        void    CompactBindings();
        uint32  TakeBindingRange(uint32 Count);
        // Dead space waiting in a free range is reused rather than wasted, unless it outgrows what is live.
        bool    ShouldCompactBindings() const;

        TVector<FSurfaceBinding>            Bindings;
        uint32                              DeadBindings = 0;

        // Dead spans a removed foliage entity left whole, as (base, count), handed to the next fresh bind that fits.
        TVector<FUIntVector2>               BindingFreeRanges;
        uint32                              FreeRangeBindings = 0;

        FSceneBatchRegistry                 Batches;

        struct FBindingMemo
        {
            uint32                      Generation = Constants::kIndexNoneU32;
            uint64                      RecycleGeneration = 0;
            // InstanceSlot unused.
            TVector<FSurfaceBinding>    Protos;
        };
        TVector<FBindingMemo>               BindingMemoByHandle;

        TVector<uint64>                     RetryScratch;
        // Scratch: resolve handle -> that entry's current Generation, filled lazily during the sweep.
        TVector<uint32>                     GenSnapshot;

        //~ Retained instance slots

        uint32  AllocateInstanceSlot();
        // Contiguous run at the end of the retained arrays, bypassing the free list so the block is dense.
        uint32  AllocateInstanceSlotBlock(uint32 Count);
        void    DeactivateInstanceSlot(uint32 Slot);
        void    FreeInstanceSlot(uint32 Slot);
        uint32  AllocateSkinnedIndex();
        void    FreeSkinnedIndex(uint32 Index);
        void    MarkInstanceDirty(uint32 Slot);
        void    MarkStaticDirty(uint32 Slot);
        // Interns a surface's LOD table; identical tables collapse to one entry.
        uint32  InternSurfaceDesc(const FResolvedSurface& Surface);

        // Writes every slot of a primitive from its state and Transform, which is the world matrix before any foreground remap.
        void    RefreshInstances(uint32 Index, const FMatrix4& Transform, FDirtySlotBucket* Sink = nullptr);
        void    RefreshInstanceTransform(uint32 Index, const FMatrix4& Transform, const FVector4& WorldSphere, TVector<uint32>* DirtySink);

        TVector<FInstanceCullEntry>         RetainedCullEntries;
        TVector<FTransform3x4>              RetainedTransforms;
        TVector<FInstanceStatic>            RetainedStatic;

        // Parallel to RetainedStatic and uploaded with it, the splat each slot draws as once too small for meshlets.
        TVector<FInstanceProxy>             RetainedProxies;

        TVector<uint32>                     InstanceFreeSlots;
        // Cull entry and transform.
        TVector<uint32>                     DirtyInstanceSlots;
        // Freed, so only the active bit changes.
        TVector<uint32>                     DeactivatedSlots;
        TVector<uint32>                     DirtyStaticSlots;
        bool                                bFullInstanceUpload = true;
        // Tracked apart from the transforms, so a moving crowd does not re-send payload that never changed.
        bool                                bFullStaticUpload = true;

        TVector<uint32>                     SkinnedIndexFreeList;
        uint32                              SkinnedIndexExtent = 0;

        TVector<FSurfaceDescGPU>            SurfaceDescs;
        THashMap<uint64, TVector<uint32>>   SurfaceDescByHash;
        bool                                bSurfaceDescsDirty = true;
        // Only ever grows while the table does, and is reset with it.
        uint32                              MaxSurfaceDescMeshlets = 0;

        void    UpdateMeshletVisibility(uint32 Slot);
        // Meshlets in the slot's largest LOD, zero for an inactive slot.
        uint32  LargestLODMeshletsOf(uint32 Slot) const;
        uint32  LargestLODMeshletsOfSurface(uint32 SurfaceDescIndex) const;
        void    ReleaseMeshletVisibility(uint32 Slot);
        void    FreeMeshletVisibilityRange(uint32 Base, uint32 Words);

        // Per retained slot, x the first word, y the word count and z the largest LOD's meshlets.
        TVector<FUIntVector3>               SlotMeshletVisibility;
        // Indexed by word count, since a streaming burst frees a range per foliage slot and a hash per free showed.
        TVector<TVector<uint32>>            MeshletVisibilityFreeLists;
        uint32                              MeshletVisibilityExtent = 1;
        uint64                              LargestLODMeshletTotal = 0;
        TVector<uint32>                     MeshletVisibilityScratch;

        //~ Grass

        // One block per (terrain, grass type, layer), with free blocks kept whole and merged for reuse.
        THashMap<FGrassSpeciesKey, FGrassSpeciesBinding> GrassSpecies;
        uint32                              GrassSlotCount = 0;
        uint32                              GrassSerial = 1;
        TVector<FUIntVector2>               GrassFreeBlocks;

        //~ Per entity

        // The hot per-entity fields of every stage in one line, paged by entity index and allocated only where an entity was seen.
        struct alignas(64) FEntityRecord
        {
            // The entity the links and the stream cache belong to.
            uint32      Entity = Constants::kIndexNoneU32;
            uint32      Links[kLinkedSources] = { Constants::kIndexNoneU32, Constants::kIndexNoneU32, Constants::kIndexNoneU32 };
            uint32      CoalesceStamp = 0;
            uint32      CoalesceIndex = Constants::kIndexNoneU32;
            // The stamp of the moved pass that claimed it, so a mover listed twice is applied once.
            uint32      MovedClaim = 0;
            // The stream frame that last wrote a record for it, and that record.
            uint32      MoveFrame = 0;
            uint32      MoveRecord = kNoStreamSlot;
            // What the stream needs of a linked entity, with StreamSlot kNoStreamSlot for one that takes the CPU path.
            uint32      StreamGeneration = 0;
            uint32      StreamSlot = kNoStreamSlot;
            float       StreamRadius = 0.0f;
            FVector3    StreamCenter = FVector3(0.0f);
        };
        static_assert(sizeof(FEntityRecord) == 64, "One entity per cache line, so movers on different threads never share one.");
        static_assert(kLinkedSources == 3, "FEntityRecord::Links initializer must match kLinkedSources.");

        // Grows only on the game thread inside Sync or Reset, so the workers of a sync and the movers of a frame read stable pages.
        class FEntityRecordTable
        {
        public:

            FEntityRecord* Find(ECS::FEntity Entity)
            {
                const uint32 Index = Entity.GetIndex();
                const uint32 Page  = Index >> kPageShift;
                return Page < (uint32)Pages.Num() && !Pages[Page].IsEmpty() ? &Pages[Page][Index & kPageMask] : nullptr;
            }

            const FEntityRecord* Find(ECS::FEntity Entity) const
            {
                return const_cast<FEntityRecordTable*>(this)->Find(Entity);
            }

            FEntityRecord& FindOrAdd(ECS::FEntity Entity);

            template <typename TFunc>
            void ForEach(TFunc&& Func)
            {
                for (TVector<FEntityRecord>& Page : Pages)
                {
                    for (FEntityRecord& Record : Page)
                    {
                        Func(Record);
                    }
                }
            }

            void Reset() { Pages.Reset(); }

        private:

            static constexpr uint32 kPageShift = 10;
            static constexpr uint32 kPageMask  = (1u << kPageShift) - 1u;

            TVector<TVector<FEntityRecord>> Pages;
        };
        FEntityRecordTable                  EntityRecords;

        // ~0u when this entity owns no primitive for that source. Source must not be Foliage.
        uint32  FindLinked(ECS::FEntity Entity, EPrimitiveSource Source) const;
        // Bitmask of sources this entity owns a primitive for; 0 when it is not renderable at all.
        uint8   GetSourceMask(ECS::FEntity Entity) const;
        void    SetLink(ECS::FEntity Entity, EPrimitiveSource Source, uint32 Index);
        void    ClearLink(ECS::FEntity Entity, EPrimitiveSource Source);
        // Clears every link and the moved-transform interest they raised, for a reset or a rescan.
        void    ResetEntityRecords();
        void    SetMovedInterest(ECS::FEntity Entity, bool bInterested);

        // The registry this set draws. Interest only changes inside Sync, Reset and Shutdown on the game thread, while no system writes transforms.
        ECS::FRegistry*                     BoundRegistry = nullptr;
        uint32                              MovedConsumer = Constants::kIndexNoneU32;
        // This set's own queue in the registry's dirty tracker.
        uint32                              DirtyConsumer = Constants::kIndexNoneU32;
        FRenderDirtyTracker&                AttachDirtyConsumer(ECS::FRegistry& Registry);

        //~ Foreground

        bool        IsRemappedForeground(const FScenePrimitive& Prim) const { return bForegroundActive && (Prim.RenderLayers & RenderLayers::kForegroundMask) != 0u; }
        FMatrix4    InstanceTransformOf(const FScenePrimitive& Prim, const FMatrix4& Transform) const;
        FVector4    InstanceSphereOf(const FScenePrimitive& Prim, const FVector4& WorldSphere) const;
        void        TrackForeground(uint32 Index);
        void        UntrackForeground(uint32 Index);
        // Rewrites the foreground primitives SetForegroundView changed, from their current transforms.
        void        ApplyForegroundRefresh(const FSyncPools& Pools);

        enum class EForegroundRefresh : uint8
        {
            None,
            Transforms,
            // Toggling changes whether they cast shadows, which only the full refresh rewrites.
            Full,
        };

        FMatrix4                            ForegroundTransform = FMatrix4(1.0f);
        float                               ForegroundRadiusScale = 1.0f;
        bool                                bForegroundActive = false;
        EForegroundRefresh                  PendingForegroundRefresh = EForegroundRefresh::None;
        // Few enough that a camera move refreshes every one of them.
        TVector<uint64>                     ForegroundKeys;

        //~ Foliage

        // A painted instance is not a primitive: everything else it needs is shared by its type or sits in the slots it owns, so it holds only that span.
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
            // InstancesVersion of the bake last synced, zero before the first.
            uint32                          SyncedVersion = 0;
            // Resolve identity per TYPE, which is what the staleness sweep compares instead of per instance.
            TVector<uint64>                 TypeResolveKeys;
        };

        struct FFoliageTypeResolve
        {
            const TVector<FResolvedSurface>*    Surfaces = nullptr;
            const FBindingMemo*                 Memo = nullptr;
            uint32                              Generation = 0;
            uint32                              MeshletHeaderSlot = 0;
            uint32                              ResolveHandle = INVALID_MESH_RESOLVE_HANDLE;
            float                               MaxDrawDistance = 0.0f;
            EInstanceFlags                      BaseFlags = EInstanceFlags::None;
            bool                                bCastShadow = true;
            bool                                bCastFarShadow = false;
            bool                                bChanged = false;
            float                               ThinningStartDistance = 0.0f;
            // Instances of this type a fresh bind gives a span, counted so its batch references are taken in one add.
            uint32                              FreshInstances = 0;
        };

        static FInstanceCullEntry   MakeFoliageCull(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type, const FVector4& WorldSphere, bool bHidden);
        static FInstanceStatic      MakeFoliageStatic(const FSurfaceBinding& Binding, const FFoliageTypeResolve& Type, uint32 EntityID, uint32 MeshletVisibilityBase);
        static FInstanceProxy       MakeFoliageProxy(const FFoliageTypeResolve& Type, uint32 SurfaceIndex);

        void    SyncFoliage(ECS::FRegistry& Registry, const FSyncPools& Pools, ECS::FEntity Entity);
        // Fills FoliageTypeScratch for this sync and reports whether the type table changed shape.
        bool    ResolveFoliageTypes(FFoliageEntityState& State, const SFoliageComponent& Foliage, const FSyncPools& Pools, ECS::FEntity Entity);
        // Rebuilds the component's bake and sizes the instance table to it, releasing a shrunk tail.
        const TVector<FFoliageBakedInstance>& BakeFoliage(FFoliageEntityState& State, SFoliageComponent& Foliage);
        // Rebinds what changed and gathers what to refresh. False when a span must grow but there is no bake to fill its new slots from.
        bool    RebindFoliageInstances(FFoliageEntityState& State, const TVector<FFoliageBakedInstance>* Baked, bool bContentChanged);
        uint32  LayoutFreshFoliageSpans(FFoliageEntityState& State, const TVector<FFoliageBakedInstance>& Baked);
        void    AllocateFreshFoliageSlots(uint32 Count);
        uint32  RefreshFoliageInstance(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                       const TVector<FFoliageBakedInstance>* Baked, uint32 EntityID, uint32 Index, bool bMarkDirect);
        // First sync of a large entity, so every instance binds and its slots can be filled in parallel.
        void    BindFreshFoliage(FFoliageEntityState& State, const SFoliageComponent& Foliage, const TVector<FFoliageBakedInstance>& Baked, uint32 EntityID);
        // Rewrites the gathered instances' slots, from the bake when there is one and from their own transform and sphere when not.
        void    RefreshFoliageInstances(const FFoliageEntityState& State, const SFoliageComponent& Foliage,
                                        const TVector<FFoliageBakedInstance>* Baked, uint32 EntityID);
        void    ApplyFoliageInstanceVisibility(const FFoliageEntityState& State, const SFoliageComponent& Foliage, uint32 Instance);
        void    ReleaseFoliageInstance(FFoliageInstanceRef& Ref, TVector<FUIntVector3>* DeferredBatchReleases = nullptr);
        // Queues a removed entity's instances for FlushFoliageReleases.
        void    RemoveFoliageEntity(ECS::FEntity Entity);
        void    FlushFoliageReleases();
        uint32  PlanFoliageReleaseBlocks();
        void    GatherFoliageReleaseSlots();
        void    FreeReleasedFoliageSlots();
        // Appends a retry target per foliage entity whose type resolve moved. Pairs with the primitive sweep.
        void    SweepFoliageResolves(FMeshResolveCache& Cache);

        ECS::TEntityMap<FFoliageEntityState> FoliageByEntity;
        uint32                              FoliageInstanceCount = 0;

        TVector<FFoliageTypeResolve>        FoliageTypeScratch;

        // Removed foliage of one sync, released together so a streaming burst spreads over workers.
        TVector<FFoliageEntityState>        PendingFoliageReleases;

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
        // (batch, skinned, count) runs a rebind released, applied once its new references are taken.
        TVector<FUIntVector3>               DeferredBatchReleaseScratch;

        //~ Sync

        void    FullRescan(ECS::FRegistry& Registry);
        // Drops every primitive, binding, slot and cached table, which Reset and FullRescan share.
        void    ClearScene();
        void    PollUnhookedSources(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);
        void    PollSkeletalBoneRanges(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);
        void    SweepStaleResolves(ECS::FRegistry& Registry, const FSyncPools& Pools);

        TVector<FRenderDirtyTracker::FEntry> DrainScratch;
        TVector<FRenderDirtyTracker::FInstanceEdit> InstanceEditScratch;
        // Flips the active bits of the instances hidden or shown since the last sync, at O(edits).
        void    ApplyFoliageInstanceEdits(const FSyncPools& Pools, FRenderDirtyTracker& Tracker);

        struct FCoalescedEntity
        {
            uint32          Entity = Constants::kIndexNoneU32;
            EPrimitiveDirty Flags[(uint32)EPrimitiveSource::Num] = {};
        };

        TVector<FCoalescedEntity>   CoalescedScratch;
        uint32                      CoalesceStamp = 0;

        // Folds DrainScratch into CoalescedScratch.
        void CoalesceDrain(TSpan<const ECS::FEntity> Moved);
        FCoalescedEntity& CoalescedRecordFor(ECS::FEntity Entity);

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

        // Indices into CoalescedScratch, filled by PartitionDrain.
        TVector<uint32>             TransformRecords;
        TVector<uint32>             StructuralRecords;
        TVector<TVector<ECS::FEntity>> ParallelOverlaps;

        // One cache line per worker, since neighboring vectors bounced the line on every push.
        struct CACHE_ALIGN FDirtySlotBucket
        {
            TVector<uint32>       Slots;
            TVector<uint32>       StaticSlots;
            TVector<ECS::FEntity> Streamed;
            TVector<uint32>       StreamedBlocks;
            TVector<ECS::FEntity> Stopped;
        };
        TVector<FDirtySlotBucket>   ParallelDirtySlots;
        // Sized to the task threads, with every bucket's slot lists emptied.
        void ResetParallelBuckets();
        FDirtySlotBucket& BucketFor(uint32 Thread) { return ParallelDirtySlots[Thread < (uint32)ParallelDirtySlots.Num() ? Thread : 0u]; }

        // Folds the per-worker lists back into the dirty lists, or gives up and re-sends everything.
        void MergeParallelDirtySlots();

        //~ Transform stream

        // Movers with no record skip coalescing entirely and are applied in parallel after the structural pass.
        void ApplyMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved);
        // Every moved entity through the CPU copies, which upload as dirty slots.
        void ApplyMovedThroughCPU(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved);
        // Opens a claim pass and returns its stamp, so a mover listed twice in it is applied once.
        uint32 BeginMovedClaims();
        uint32                      MovedClaimStamp = 0;

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

        enum class EStreamKind : uint32
        {
            Skip,
            CPUPath,
            Stream,
        };
        // What the stream needs of one entity this frame, read from its record.
        struct FStreamCacheEntry
        {
            EStreamKind Kind = EStreamKind::Skip;
            uint32      Slot = kNoStreamSlot;
            FVector3    LocalCenter = FVector3(0.0f);
            float       Radius = 0.0f;
        };
        FStreamCacheEntry LookupStreamCacheEntry(ECS::FEntity Entity);
        void BuildStreamCacheEntry(FEntityRecord& Record) const;
        // A structural sync of the entity, which may move its slot or bounds.
        void InvalidateStreamCache(ECS::FEntity Entity);
        // Everything at once, for a rescan, a reset or a foreground toggle.
        void InvalidateAllStreamCache();
        uint32                      StreamCacheGeneration = 1;

        FTransformStreamTarget      StreamTarget;
        uint32                      StreamCount = 0;
        // Set on a record once its rewrite is queued, so a mover moved many times queues one.
        static constexpr uint32 kMoveRewriteQueued = 0x80000000u;
        // Bumped every frame the stream opens.
        uint32                      StreamFrame = 0;
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
                if (List.IsEmpty())
                {
                    return;
                }
                if (Used == (uint32)Lists.Num())
                {
                    Lists.Emplace();
                }
                Count += List.Num();
                Lists[Used].Reset();
                swap(Lists[Used], List);
                ++Used;
            }

            void Clear()
            {
                for (uint32 Index = 0; Index < Used; ++Index)
                {
                    Lists[Index].Reset();
                }
                Used  = 0;
                Count = 0;
            }
        };
        FStreamedLists              StreamedThisFrame;
        FStreamedLists              StreamedLastFrame;
        TVector<ECS::FEntity>       StoppedScratch;
        TVector<uint32>             StreamedBlocks;
        // Drops every streamed list and record count, for a reset or a rescan that rebuilt the CPU copies.
        void ResetStreamState();

        bool                        bResolveTableChanged = false;

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
