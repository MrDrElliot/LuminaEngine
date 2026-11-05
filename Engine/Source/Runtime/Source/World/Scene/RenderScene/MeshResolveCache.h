#pragma once

#include "Core/Threading/Thread.h"
#include "Renderer/ShaderHandle.h"
#include <atomic>
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/AABB.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/MeshData.h"
#include "Renderer/RHIFwd.h"
#include "World/ECS/Entity.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    class CMesh;
    class CMaterialInterface;

    namespace ECS
    {
        class FRegistry;
    }

    // Shader entries are FShaderLibrary-owned and process-immortal, so holding them is safe.
    struct FResolvedSurface
    {
        FShaderH PixelShader = {};
        FShaderH VertexShader = {};
        FShaderH MeshShaderShadow = {};
        FShaderH MeshShaderBase = {};
        FShaderH VisBufferMeshShader = {};
        FShaderH VisBufferMeshShaderMasked = {};
        FShaderH MaskedVisBufferPixelShader = {};
        FShaderH MeshShaderShadowMasked = {};
        FShaderH ShadowMaskedPixelShader = {};
        FShaderH DeferredShader = {};

        FDrawBatchKey   BatchKey    = {};

        EInstanceFlags  MaterialFlags = EInstanceFlags::None;
        uint64          MaterialID    = 0;
        uint32          MaterialIdx   = 0;
        bool            bMaterialCastsShadows = true;

        uint32  NumLODs                             = 1;
        uint32  LODMeshletOffset[MAX_MESH_LODS]     = {};
        uint32  LODMeshletCount[MAX_MESH_LODS]      = {};
        // Mesh-local error per LOD, already scaled by the mesh's own LOD distance scale.
        float   LODError[MAX_MESH_LODS]             = {};

        // Mesh-local world size of one UV tile for the texture streamer; 0 = unknown. See FGeometrySurface::TexelFactor.
        float   TexelFactor                         = 0.0f;

        // What this resolve was taken from, since a recompile mints new shader entries and leaves the handles above on superseded code.
        CMaterialInterface* SourceMaterial        = nullptr;
        uint32              SourceShaderRevision  = 0;
        // A destroyed object's address can be handed to a new one, which IsValid cannot tell apart.
        FGuid               SourceMaterialGuid    = {};
    };

    namespace MeshResolve
    {
        // Records what Surface was resolved from; IsSurfaceStale reads the same key back.
        RUNTIME_API void StampSurfaceSource(FResolvedSurface& Surface, CMaterialInterface* RawMaterial);

        // True once the source material has recompiled or been replaced at the same address. No resolve.
        RUNTIME_API bool IsSurfaceStale(const FResolvedSurface& Surface);

        RUNTIME_API bool ResolveSurfaceMaterial(FResolvedSurface& Out, CMaterialInterface* RawMaterial);
    }

    // Interned per (mesh, material assignment), so every instance of a mesh shares one entry.
    struct FResolvedMesh
    {
        TVector<FResolvedSurface>   Surfaces;

        FVector3                    LocalCenter;
        float                       LocalRadius = 0.0f;

        uint32                      MeshletHeaderSlot = 0;

        const void*                 MeshKey = nullptr;
        FGuid                       MeshGuid;
        TVector<const void*>        OverrideKey;

        uint32                      Generation = 0;

        TVector<const void*>        Dependencies;

        bool                        bNeedsResolve = true;

        // False while a slot's material is still compiling and its surface draws with the default material.
        bool                        bAllMaterialsReady = false;
        bool                        bResolved = false;
    };

    enum class EMeshResolveSource : uint8
    {
        StaticMesh,
        SkeletalMesh,
        DynamicMesh,
        Foliage,
        Num,
    };

    // Per registry: the entities whose resolve may be stale, and which entities read each resolve entry or material.
    class RUNTIME_API FMeshResolveQueue
    {
    public:

        // Game thread. Creates the queue in the registry context and hooks the mesh component signals.
        static FMeshResolveQueue& Ensure(ECS::FRegistry& Registry);
        static FMeshResolveQueue* Find(ECS::FRegistry& Registry);

        // Game thread. A foliage component has no binding of its own, so its writers name the entity.
        static void MarkFoliageDirty(ECS::FRegistry& Registry, ECS::FEntity Entity);

        static constexpr uint64 HandleKey(uint32 Handle) { return (uint64)Handle | (1ull << 63); }
        static uint64 DependencyKey(const void* Asset) { return (uint64)(uintptr_t)Asset; }

        // Any thread. Components name their queue by slot, so the binding fits in the bytes a pointer would take.
        static FMeshResolveQueue* FromSlot(uint8 InSlot) { return Slots[InSlot].load(std::memory_order_acquire); }
        uint8 GetSlot() const { return Slot; }

        FMeshResolveQueue();
        ~FMeshResolveQueue();
        LE_NO_COPYMOVE(FMeshResolveQueue);

        // Any thread.
        void Enqueue(ECS::FEntity Entity, EMeshResolveSource Source);
        void RequestFullSweep();

        // Game thread. Applies the cache's invalidations, latches sweep requests and sorts the queued entities by source.
        void BeginFrame();

        bool ConsumeFullSweep(EMeshResolveSource Source);

        // Swaps the source's dirty entities into Out, so both lists keep their capacity.
        void TakeDirty(EMeshResolveSource Source, TVector<ECS::FEntity>& Out);

        bool HasDirty(EMeshResolveSource Source) const;
        bool HasPendingWork() const;

        // Game thread. Keys are unique; an empty list stops watching.
        void Watch(ECS::FEntity Entity, EMeshResolveSource Source, TSpan<const uint64> Keys);
        void Unwatch(ECS::FEntity Entity, EMeshResolveSource Source);
        bool IsWatching(ECS::FEntity Entity, EMeshResolveSource Source, uint64 Key) const;

        // Game thread. Queues every entity watching Key.
        void Wake(uint64 Key);

    private:

        static constexpr uint32 kMaxQueues = 255;

        struct FImpl;
        FImpl* Impl = nullptr;

        // 0 means no slot was free, which leaves components unbound and makes every frame a sweep instead.
        uint8 Slot = 0;

        static std::atomic<FMeshResolveQueue*> Slots[kMaxQueues + 1];
    };

    // Ties a mesh component to the queue of the registry holding it. A copy lives somewhere else, so it starts unbound.
    struct FMeshResolveBinding
    {
        FMeshResolveBinding() = default;
        FMeshResolveBinding(const FMeshResolveBinding&) {}
        FMeshResolveBinding(FMeshResolveBinding&&) = default;
        FMeshResolveBinding& operator = (const FMeshResolveBinding&) { return *this; }
        FMeshResolveBinding& operator = (FMeshResolveBinding&&) { return *this; }

        void Bind(const FMeshResolveQueue& Queue, ECS::FEntity InEntity, EMeshResolveSource InSource)
        {
            Entity    = InEntity;
            QueueSlot = Queue.GetSlot();
            Source    = InSource;
            ClearQueued();
        }

        // Any thread. Queues the entity once until the resolve pass reads the component.
        void Enqueue()
        {
            if (QueueSlot == 0u || std::atomic_ref<uint8>(bQueued).exchange(1u, std::memory_order_acq_rel) != 0u)
            {
                return;
            }
            if (FMeshResolveQueue* Queue = FMeshResolveQueue::FromSlot(QueueSlot))
            {
                Queue->Enqueue(Entity, Source);
            }
        }

        // A read-modify-write, so a write that found the flag still set is visible to the pass that clears it.
        void ClearQueued() { (void)std::atomic_ref<uint8>(bQueued).exchange(0u, std::memory_order_acq_rel); }

        bool IsQueued() const { return std::atomic_ref<uint8>(const_cast<uint8&>(bQueued)).load(std::memory_order_acquire) != 0u; }
        bool IsBound() const { return QueueSlot != 0u; }

        ECS::FEntity        Entity = ECS::NullEntity;
        uint8               QueueSlot = 0;
        EMeshResolveSource  Source = EMeshResolveSource::StaticMesh;
        uint8               bQueued = 0;
    };
    static_assert(sizeof(FMeshResolveBinding) == 8, "the binding rides in every mesh component; keep it one word.");

    // Resolves the material/geometry data the mesh gather needs, once per change instead of per frame.
    class RUNTIME_API FMeshResolveCache
    {
    public:

        static FMeshResolveCache& Get();

        FMeshResolveCache();
        ~FMeshResolveCache();
        LE_NO_COPYMOVE(FMeshResolveCache);

        // False before the singleton is built and after it is destroyed at exit, when a late queue must not reach it.
        static FORCEINLINE bool IsAlive() { return bAlive.load(std::memory_order_acquire); }

        // Game thread only; mutates the table, so nothing may read it concurrently.
        uint32 Resolve(CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        FORCEINLINE const FResolvedMesh& GetEntry(uint32 Handle) const { return *Entries[Handle]; }
        FORCEINLINE bool IsValidHandle(uint32 Handle) const { return Handle < (uint32)Entries.Num(); }
        // Handles are dense indices, so this doubles as the size of any per-handle side table.
        FORCEINLINE uint32 NumEntries() const { return (uint32)Entries.Num(); }

        FORCEINLINE uint32 GetEntryState(uint32 Handle) const
        {
            return Handle < (uint32)EntryStates.Num() ? EntryStates[Handle] : MESH_RESOLVE_STATE_STALE;
        }

        FORCEINLINE uint32 GetTableGeneration() const { return TableGeneration; }

        // TableGeneration at this entry's last rebuild that came out resolved; 0 if never.
        FORCEINLINE uint32 GetResolvedAtGeneration(uint32 Handle) const
        {
            return Handle < (uint32)ResolvedAtGeneration.Num() ? ResolvedAtGeneration[Handle] : 0u;
        }

        // Stamp each component compares against; a mismatch means "re-resolve me".
        static FORCEINLINE uint32 GetEpoch() { return Epoch.load(std::memory_order_acquire); }

        // Any thread. Everything changed: every entry goes stale and every registry sweeps all of its components.
        static void BumpEpoch();

        // Any thread. Stales the entries built from Asset and wakes the components that read them.
        static void InvalidateDependency(const void* Asset);

        // Any thread. Every registry re-checks all of its components against the entries as they stand.
        static void RequestFullSweep();
        static FORCEINLINE uint32 GetFullSweepGeneration() { return FullSweepGeneration.load(std::memory_order_acquire); }



        // Bumped by every asset level change, which is what a retry waiting on an asset needs to hear.
        static FORCEINLINE uint32 GetPendingGeneration() { return PendingGeneration.load(std::memory_order_acquire); }

        // Game thread. Stales the invalidated entries and wakes their readers in every registry.
        void ApplyPendingInvalidations();

        // Game thread; the queue constructor and destructor call these.
        void RegisterQueue(FMeshResolveQueue* Queue);
        void UnregisterQueue(FMeshResolveQueue* Queue);

        // Only safe between frames; every registry sweeps, since the handles its components hold are gone.
        void Flush();

    private:

        static uint64 HashKey(const void* Mesh, const TVector<CMaterialInterface*>& Overrides);
        uint32 FindEntry(uint64 KeyHash, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides) const;
        void ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        // Rebuilds one entry in place and republishes its dependency registration and staleness token.
        void RebuildEntry(uint32 Handle, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);
        void RegisterDependencies(uint32 Handle, const FResolvedMesh& Entry);
        void UnregisterDependencies(uint32 Handle, const FResolvedMesh& Entry);
        void MarkEntryStale(uint32 Handle);
        void WakeQueues(uint64 Key);

        // Handles are indices; entries are boxed so growth never moves one a frame holds a reference to.
        TVector<FResolvedMesh*>             Entries;
        THashMap<uint64, TVector<uint32>>   HandlesByHash;

        // Parallel to Entries. See GetEntryState.
        TVector<uint32>                     EntryStates;
        TVector<uint32>                     ResolvedAtGeneration;

        THashMap<const void*, TVector<uint32>> HandlesByDependency;

        uint32                      TableGeneration = 1;
        uint32                      AppliedEpoch = 0;

        TVector<FMeshResolveQueue*> Queues;

        // Swapped with PendingInvalidations, so draining them never allocates.
        TVector<const void*>        InvalidationScratch;

        static std::atomic<uint32>  Epoch;
        static std::atomic<uint32>  PendingGeneration;
        static std::atomic<uint32>  FullSweepGeneration;
        static std::atomic<bool>    bHasPendingInvalidations;
        static std::atomic<bool>    bAlive;

        // Guards PendingInvalidations only. Contended only by asset loads, which are rare next to frames.
        static FMutex               PendingMutex;
        static TVector<const void*> PendingInvalidations;
    };
}
