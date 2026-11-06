#include "RuntimePCH.h"
#include "MeshResolveCache.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Containers/ConcurrentQueue.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Renderer/Vertex.h"
#include "World/ECS/EntityMap.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"

namespace Lumina
{
    namespace MeshResolveCacheInternal
    {
        constexpr uint32 kNoNode      = Constants::kIndexNoneU32;
        constexpr uint32 kNumSources  = (uint32)EMeshResolveSource::Num;
        constexpr uint32 kAllSources  = (1u << kNumSources) - 1u;

        // Past this many queued entities a source sweeps instead, which bounds a queue whose renderer stopped draining it.
        constexpr int32  kMaxDirtyPerSource = 1 << 16;

        FORCEINLINE uint64 PackQueued(ECS::FEntity Entity, EMeshResolveSource Source)
        {
            return (uint64)Entity.GetPacked() | ((uint64)Source << 32);
        }

        // One entity's interest in one key, linked both into the key's list and into the entity's own chain.
        struct FWatchNode
        {
            uint64              Key = 0;
            ECS::FEntity        Entity;
            EMeshResolveSource  Source{};
            uint32              PrevInKey = kNoNode;
            uint32              NextInKey = kNoNode;
            uint32              NextOfOwner = kNoNode;
        };

        struct FWatchOwner
        {
            FWatchOwner()
            {
                for (uint32& Head : Heads)
                {
                    Head = kNoNode;
                }
            }

            bool IsEmpty() const
            {
                for (uint32 Head : Heads)
                {
                    if (Head != kNoNode)
                    {
                        return false;
                    }
                }
                return true;
            }

            uint32 Heads[kNumSources];
        };

        // Binds on every construct and replace, since a replaced value is a fresh copy that starts unbound.
        template <typename TComponent, EMeshResolveSource Source>
        void OnMeshComponentChanged(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FMeshResolveQueue* Queue = FMeshResolveQueue::Find(Registry);
            TComponent* Component = Registry.TryGet<TComponent>(Entity);
            if (Queue == nullptr || Component == nullptr)
            {
                return;
            }
            Component->ResolveBinding.Bind(*Queue, Entity, Source);
            Component->InvalidateRenderResolve();
        }

        template <EMeshResolveSource Source>
        void OnMeshComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            if (FMeshResolveQueue* Queue = FMeshResolveQueue::Find(Registry))
            {
                Queue->Unwatch(Entity, Source);
            }
        }

        void OnFoliageChanged(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FMeshResolveQueue::MarkFoliageDirty(Registry, Entity);
        }

        template <typename TComponent, auto OnChanged, auto OnRemoved>
        void ConnectSignals(ECS::FRegistry& Registry)
        {
            ECS::FComponentSignals& Signals = Registry.GetSignals<TComponent>();

            // Disconnected first, so a queue recreated in the same registry never hears an event twice.
            Signals.OnConstruct.Disconnect<OnChanged>();
            Signals.OnUpdate.Disconnect<OnChanged>();
            Signals.OnDestroy.Disconnect<OnRemoved>();

            Signals.OnConstruct.Connect<OnChanged>();
            Signals.OnUpdate.Connect<OnChanged>();
            Signals.OnDestroy.Connect<OnRemoved>();
        }

        void AddDependency(FResolvedMesh& Out, const void* Dependency)
        {
            if (Dependency != nullptr)
            {
                Out.Dependencies.AddUnique(Dependency);
            }
        }
    }

    struct FMeshResolveQueue::FImpl
    {
        TConcurrentQueue<uint64>                                    Incoming;
        std::atomic<bool>                                           bAnyIncoming{ false };
        std::atomic<uint32>                                         FullSweepSources{ MeshResolveCacheInternal::kAllSources };
        uint32                                                      SeenSweepGeneration = 0;

        TVector<ECS::FEntity>                                       Dirty[MeshResolveCacheInternal::kNumSources];

        TVector<MeshResolveCacheInternal::FWatchNode>               Nodes;
        uint32                                                      FreeNodes = MeshResolveCacheInternal::kNoNode;
        THashMap<uint64, uint32>                                    HeadByKey;
        ECS::TEntityMap<MeshResolveCacheInternal::FWatchOwner>      Owners;

        uint32 AllocateNode()
        {
            if (FreeNodes != MeshResolveCacheInternal::kNoNode)
            {
                const uint32 Node = FreeNodes;
                FreeNodes = Nodes[Node].NextOfOwner;
                return Node;
            }
            return (uint32)Nodes.Emplace();
        }

        void LinkIntoKey(uint32 Node)
        {
            MeshResolveCacheInternal::FWatchNode& Linked = Nodes[Node];
            Linked.PrevInKey = MeshResolveCacheInternal::kNoNode;
            if (uint32* Head = HeadByKey.Find(Linked.Key))
            {
                Linked.NextInKey = *Head;
                Nodes[*Head].PrevInKey = Node;
                *Head = Node;
                return;
            }
            Linked.NextInKey = MeshResolveCacheInternal::kNoNode;
            HeadByKey.Add(Linked.Key, Node);
        }

        void UnlinkFromKey(uint32 Node)
        {
            const MeshResolveCacheInternal::FWatchNode& Linked = Nodes[Node];
            if (Linked.NextInKey != MeshResolveCacheInternal::kNoNode)
            {
                Nodes[Linked.NextInKey].PrevInKey = Linked.PrevInKey;
            }

            if (Linked.PrevInKey != MeshResolveCacheInternal::kNoNode)
            {
                Nodes[Linked.PrevInKey].NextInKey = Linked.NextInKey;
            }
            else if (Linked.NextInKey != MeshResolveCacheInternal::kNoNode)
            {
                HeadByKey.FindChecked(Linked.Key) = Linked.NextInKey;
            }
            else
            {
                HeadByKey.Remove(Linked.Key);
            }
        }

        void ReleaseChain(uint32& Head)
        {
            for (uint32 Node = Head; Node != MeshResolveCacheInternal::kNoNode;)
            {
                const uint32 Next = Nodes[Node].NextOfOwner;
                UnlinkFromKey(Node);
                Nodes[Node].NextOfOwner = FreeNodes;
                FreeNodes = Node;
                Node = Next;
            }
            Head = MeshResolveCacheInternal::kNoNode;
        }

        bool ChainMatches(uint32 Head, TSpan<const uint64> Keys) const
        {
            uint32 Node = Head;
            for (uint64 Key : Keys)
            {
                if (Node == MeshResolveCacheInternal::kNoNode || Nodes[Node].Key != Key)
                {
                    return false;
                }
                Node = Nodes[Node].NextOfOwner;
            }
            return Node == MeshResolveCacheInternal::kNoNode;
        }

        // Nothing queued matters while a sweep is pending, since the sweep visits every component anyway.
        void AddDirty(uint32 Source, ECS::FEntity Entity)
        {
            const uint32 Bit = 1u << Source;
            if ((FullSweepSources.load(std::memory_order_relaxed) & Bit) != 0u)
            {
                return;
            }

            TVector<ECS::FEntity>& List = Dirty[Source];
            if (List.Num() >= MeshResolveCacheInternal::kMaxDirtyPerSource)
            {
                List.Reset();
                FullSweepSources.fetch_or(Bit, std::memory_order_acq_rel);
                return;
            }
            List.Add(Entity);
        }

        void DrainIncoming()
        {
            if (!bAnyIncoming.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }

            uint64 Batch[256];
            uint32 Count;
            while ((Count = Incoming.DequeueBulk(Batch, 256)) != 0)
            {
                for (uint32 i = 0; i < Count; ++i)
                {
                    const uint32 Source = (uint32)(Batch[i] >> 32);
                    if (Source < MeshResolveCacheInternal::kNumSources)
                    {
                        AddDirty(Source, ECS::FEntity::FromPacked((uint32)Batch[i]));
                    }
                }
            }
        }
    };

    std::atomic<FMeshResolveQueue*> FMeshResolveQueue::Slots[FMeshResolveQueue::kMaxQueues + 1] = {};

    FMeshResolveQueue::FMeshResolveQueue()
        : Impl(Memory::New<FImpl>())
    {
        Impl->SeenSweepGeneration = FMeshResolveCache::GetFullSweepGeneration();

        for (uint32 Candidate = 1; Candidate <= kMaxQueues && Slot == 0u; ++Candidate)
        {
            FMeshResolveQueue* Expected = nullptr;
            if (Slots[Candidate].compare_exchange_strong(Expected, this, std::memory_order_acq_rel))
            {
                Slot = (uint8)Candidate;
            }
        }
        if (Slot == 0u)
        {
            LOG_ERROR("MeshResolve: more than {} registries hold a resolve queue; this one re-checks every mesh component every frame.", kMaxQueues);
        }

        // Get first, since it is what builds the singleton the liveness check reads.
        FMeshResolveCache& Cache = FMeshResolveCache::Get();
        if (FMeshResolveCache::IsAlive())
        {
            Cache.RegisterQueue(this);
        }
    }

    // A registry outliving the cache at exit leaves nothing to unregister from, since the cache's queue list went with it.
    FMeshResolveQueue::~FMeshResolveQueue()
    {
        if (FMeshResolveCache::IsAlive())
        {
            FMeshResolveCache::Get().UnregisterQueue(this);
        }
        if (Slot != 0u)
        {
            Slots[Slot].store(nullptr, std::memory_order_release);
        }
        Memory::Delete(Impl);
    }

    FMeshResolveQueue* FMeshResolveQueue::Find(ECS::FRegistry& Registry)
    {
        TUniquePtr<FMeshResolveQueue>* Holder = Registry.Ctx().Find<TUniquePtr<FMeshResolveQueue>>();
        return Holder ? Holder->get() : nullptr;
    }

    FMeshResolveQueue& FMeshResolveQueue::Ensure(ECS::FRegistry& Registry)
    {
        if (FMeshResolveQueue* Existing = Find(Registry))
        {
            return *Existing;
        }

        FMeshResolveQueue& Queue = *Registry.Ctx().Emplace<TUniquePtr<FMeshResolveQueue>>(MakeUnique<FMeshResolveQueue>());

        using namespace MeshResolveCacheInternal;
        ConnectSignals<SStaticMeshComponent,
            &OnMeshComponentChanged<SStaticMeshComponent, EMeshResolveSource::StaticMesh>,
            &OnMeshComponentRemoved<EMeshResolveSource::StaticMesh>>(Registry);
        ConnectSignals<SSkeletalMeshComponent,
            &OnMeshComponentChanged<SSkeletalMeshComponent, EMeshResolveSource::SkeletalMesh>,
            &OnMeshComponentRemoved<EMeshResolveSource::SkeletalMesh>>(Registry);
        ConnectSignals<SDynamicMeshComponent,
            &OnMeshComponentChanged<SDynamicMeshComponent, EMeshResolveSource::DynamicMesh>,
            &OnMeshComponentRemoved<EMeshResolveSource::DynamicMesh>>(Registry);
        ConnectSignals<SFoliageComponent, &OnFoliageChanged, &OnMeshComponentRemoved<EMeshResolveSource::Foliage>>(Registry);

        return Queue;
    }

    void FMeshResolveQueue::MarkFoliageDirty(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        FMeshResolveQueue* Queue = Find(Registry);
        SFoliageComponent* Foliage = Registry.TryGet<SFoliageComponent>(Entity);
        if (Queue == nullptr || Foliage == nullptr)
        {
            return;
        }

        for (SFoliageType& Type : Foliage->Types)
        {
            Type.CachedEntryState = MESH_RESOLVE_STATE_STALE;
        }
        Queue->Enqueue(Entity, EMeshResolveSource::Foliage);
    }

    void FMeshResolveQueue::Enqueue(ECS::FEntity Entity, EMeshResolveSource Source)
    {
        if (Entity.IsNull())
        {
            return;
        }
        Impl->Incoming.Enqueue(MeshResolveCacheInternal::PackQueued(Entity, Source));
        Impl->bAnyIncoming.store(true, std::memory_order_release);
    }

    void FMeshResolveQueue::RequestFullSweep()
    {
        Impl->FullSweepSources.fetch_or(MeshResolveCacheInternal::kAllSources, std::memory_order_acq_rel);
    }

    void FMeshResolveQueue::BeginFrame()
    {
        // Read before the invalidations apply, so a bump landing in between sweeps next frame instead of never.
        const uint32 SweepGeneration = FMeshResolveCache::GetFullSweepGeneration();

        if (FMeshResolveCache::IsAlive())
        {
            FMeshResolveCache::Get().ApplyPendingInvalidations();
        }

        if (SweepGeneration != Impl->SeenSweepGeneration || Slot == 0u)
        {
            Impl->SeenSweepGeneration = SweepGeneration;
            RequestFullSweep();
        }

        Impl->DrainIncoming();
    }

    bool FMeshResolveQueue::ConsumeFullSweep(EMeshResolveSource Source)
    {
        const uint32 Bit = 1u << (uint32)Source;
        return (Impl->FullSweepSources.fetch_and(~Bit, std::memory_order_acq_rel) & Bit) != 0u;
    }

    void FMeshResolveQueue::TakeDirty(EMeshResolveSource Source, TVector<ECS::FEntity>& Out)
    {
        Out.Reset();
        swap(Out, Impl->Dirty[(uint32)Source]);
    }

    bool FMeshResolveQueue::HasDirty(EMeshResolveSource Source) const
    {
        return !Impl->Dirty[(uint32)Source].IsEmpty();
    }

    bool FMeshResolveQueue::HasPendingWork() const
    {
        if (Impl->bAnyIncoming.load(std::memory_order_acquire) || Impl->FullSweepSources.load(std::memory_order_acquire) != 0u)
        {
            return true;
        }
        for (const TVector<ECS::FEntity>& Dirty : Impl->Dirty)
        {
            if (!Dirty.IsEmpty())
            {
                return true;
            }
        }
        return false;
    }

    void FMeshResolveQueue::Watch(ECS::FEntity Entity, EMeshResolveSource Source, TSpan<const uint64> Keys)
    {
        if (Keys.IsEmpty())
        {
            Unwatch(Entity, Source);
            return;
        }

        FImpl& State = *Impl;
        uint32& Head = State.Owners.FindOrAdd(Entity).Heads[(uint32)Source];
        if (State.ChainMatches(Head, Keys))
        {
            return;
        }

        State.ReleaseChain(Head);

        uint32 Tail = MeshResolveCacheInternal::kNoNode;
        for (uint64 Key : Keys)
        {
            const uint32 Node = State.AllocateNode();
            MeshResolveCacheInternal::FWatchNode& Watched = State.Nodes[Node];
            Watched.Key         = Key;
            Watched.Entity      = Entity;
            Watched.Source      = Source;
            Watched.NextOfOwner = MeshResolveCacheInternal::kNoNode;
            State.LinkIntoKey(Node);

            if (Tail == MeshResolveCacheInternal::kNoNode)
            {
                Head = Node;
            }
            else
            {
                State.Nodes[Tail].NextOfOwner = Node;
            }
            Tail = Node;
        }
    }

    void FMeshResolveQueue::Unwatch(ECS::FEntity Entity, EMeshResolveSource Source)
    {
        FImpl& State = *Impl;
        MeshResolveCacheInternal::FWatchOwner* Owner = State.Owners.Find(Entity);
        if (Owner == nullptr)
        {
            return;
        }

        State.ReleaseChain(Owner->Heads[(uint32)Source]);
        if (Owner->IsEmpty())
        {
            State.Owners.Remove(Entity);
        }
    }

    bool FMeshResolveQueue::IsWatching(ECS::FEntity Entity, EMeshResolveSource Source, uint64 Key) const
    {
        const MeshResolveCacheInternal::FWatchOwner* Owner = Impl->Owners.Find(Entity);
        if (Owner == nullptr)
        {
            return false;
        }

        for (uint32 Node = Owner->Heads[(uint32)Source]; Node != MeshResolveCacheInternal::kNoNode; Node = Impl->Nodes[Node].NextOfOwner)
        {
            if (Impl->Nodes[Node].Key == Key)
            {
                return true;
            }
        }
        return false;
    }

    void FMeshResolveQueue::Wake(uint64 Key)
    {
        FImpl& State = *Impl;
        const uint32* Head = State.HeadByKey.Find(Key);
        if (Head == nullptr)
        {
            return;
        }

        for (uint32 Node = *Head; Node != MeshResolveCacheInternal::kNoNode; Node = State.Nodes[Node].NextInKey)
        {
            const MeshResolveCacheInternal::FWatchNode& Watched = State.Nodes[Node];
            State.AddDirty((uint32)Watched.Source, Watched.Entity);
        }
    }

    std::atomic<uint32> FMeshResolveCache::Epoch{1};
    std::atomic<uint32> FMeshResolveCache::PendingGeneration{1};
    std::atomic<uint32> FMeshResolveCache::FullSweepGeneration{1};
    std::atomic<bool>   FMeshResolveCache::bHasPendingInvalidations{false};
    constinit std::atomic<bool> FMeshResolveCache::bAlive{false};
    FMutex               FMeshResolveCache::PendingMutex;
    TVector<const void*> FMeshResolveCache::PendingInvalidations;

    FMeshResolveCache& FMeshResolveCache::Get()
    {
        static FMeshResolveCache Instance;
        return Instance;
    }

    FMeshResolveCache::FMeshResolveCache()
    {
        bAlive.store(true, std::memory_order_release);
    }

    FMeshResolveCache::~FMeshResolveCache()
    {
        bAlive.store(false, std::memory_order_release);
        Flush();
    }

    void FMeshResolveCache::BumpEpoch()
    {
        Epoch.fetch_add(1, std::memory_order_acq_rel);
        RequestFullSweep();
    }

    void FMeshResolveCache::RequestFullSweep()
    {
        FullSweepGeneration.fetch_add(1, std::memory_order_acq_rel);
        PendingGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void FMeshResolveCache::InvalidateDependency(const void* Asset)
    {
        // Before the cache exists no entry or queue can read the asset, and after it is gone the pending list may be too.
        if (Asset == nullptr || !IsAlive())
        {
            return;
        }

        {
            FScopeLock Lock(PendingMutex);
            PendingInvalidations.Add(Asset);
            bHasPendingInvalidations.store(true, std::memory_order_release);

        }

        PendingGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void FMeshResolveCache::RegisterQueue(FMeshResolveQueue* Queue)
    {
        Queues.AddUnique(Queue);
    }

    void FMeshResolveCache::UnregisterQueue(FMeshResolveQueue* Queue)
    {
        Queues.RemoveSingleSwap(Queue);
    }

    void FMeshResolveCache::WakeQueues(uint64 Key)
    {
        for (FMeshResolveQueue* Queue : Queues)
        {
            Queue->Wake(Key);
        }
    }

    void FMeshResolveCache::MarkEntryStale(uint32 Handle)
    {
        if (Handle >= (uint32)Entries.Num())
        {
            return;
        }
        Entries[Handle]->bNeedsResolve = true;
        EntryStates[Handle]            = MESH_RESOLVE_STATE_STALE;
    }

    void FMeshResolveCache::RegisterDependencies(uint32 Handle, const FResolvedMesh& Entry)
    {
        for (const void* Dependency : Entry.Dependencies)
        {
            HandlesByDependency.FindOrAdd(Dependency).Add(Handle);
        }
    }

    void FMeshResolveCache::UnregisterDependencies(uint32 Handle, const FResolvedMesh& Entry)
    {
        for (const void* Dependency : Entry.Dependencies)
        {
            if (TVector<uint32>* Bucket = HandlesByDependency.Find(Dependency))
            {
                Bucket->RemoveSingleSwap(Handle);
            }
        }
    }

    void FMeshResolveCache::RebuildEntry(uint32 Handle, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        FResolvedMesh& Entry = *Entries[Handle];

        UnregisterDependencies(Handle, Entry);
        ResolveSurfaces(Entry, Mesh, Overrides);
        Entry.bNeedsResolve = false;
        RegisterDependencies(Handle, Entry);

        // Bit 0 marks "not ready at this generation"; the dependency that completes it re-marks the entry stale.
        EntryStates[Handle] = (Entry.Generation << 1) | (Entry.bResolved ? 0u : 1u);
    }

    void FMeshResolveCache::ApplyPendingInvalidations()
    {
        // An epoch stales everything and every registry sweeps, so the per-asset wakes below would only repeat that.
        const uint32 CurrentEpoch = GetEpoch();
        const bool bEpochChanged = CurrentEpoch != AppliedEpoch;
        if (bEpochChanged)
        {
            AppliedEpoch = CurrentEpoch;
            for (uint32 Handle = 0, N = (uint32)Entries.Num(); Handle < N; ++Handle)
            {
                MarkEntryStale(Handle);
            }
        }

        if (!bHasPendingInvalidations.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        InvalidationScratch.Reset();
        {
            FScopeLock Lock(PendingMutex);
            swap(InvalidationScratch, PendingInvalidations);
        }

        if (!bEpochChanged)
        {
            for (const void* Asset : InvalidationScratch)
            {
                if (const TVector<uint32>* Handles = HandlesByDependency.Find(Asset))
                {
                    for (uint32 Handle : *Handles)
                    {
                        MarkEntryStale(Handle);
                        WakeQueues(FMeshResolveQueue::HandleKey(Handle));
                    }
                }
                WakeQueues(FMeshResolveQueue::DependencyKey(Asset));
            }
        }

        InvalidationScratch.Reset();
    }

    uint64 FMeshResolveCache::HashKey(const void* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, (uint64)Mesh);
        for (CMaterialInterface* Override : Overrides)
        {
            Hash::HashCombine(Seed, (uint64)Override);
        }
        return (uint64)Seed;
    }

    void FMeshResolveCache::Flush()
    {
        for (FResolvedMesh* Entry : Entries)
        {
            Memory::Delete(Entry);
        }
        Entries.Reset();
        EntryStates.Reset();
        ResolvedAtGeneration.Reset();
        HandlesByHash.Reset();
        HandlesByDependency.Reset();
        ++TableGeneration;
        RequestFullSweep();
    }

    uint32 FMeshResolveCache::FindEntry(uint64 KeyHash, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides) const
    {
        const TVector<uint32>* Bucket = HandlesByHash.Find(KeyHash);
        if (Bucket == nullptr)
        {
            return INVALID_MESH_RESOLVE_HANDLE;
        }

        for (uint32 Handle : *Bucket)
        {
            const FResolvedMesh& Candidate = *Entries[Handle];
            if (Candidate.MeshKey != Mesh || Candidate.MeshGuid != Mesh->GetGUID()
                || Candidate.OverrideKey.Num() != Overrides.Num())
            {
                continue;
            }

            bool bSame = true;
            for (int32 i = 0; i < Overrides.Num(); ++i)
            {
                if (Candidate.OverrideKey[i] != (const void*)Overrides[i])
                {
                    bSame = false;
                    break;
                }
            }

            if (bSame)
            {
                return Handle;
            }
        }
        return INVALID_MESH_RESOLVE_HANDLE;
    }

    uint32 FMeshResolveCache::Resolve(CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        if (!IsValid(Mesh))
        {
            return INVALID_MESH_RESOLVE_HANDLE;
        }

        const uint64 KeyHash = HashKey(Mesh, Overrides);

        const uint32 Existing = FindEntry(KeyHash, Mesh, Overrides);
        if (Existing != INVALID_MESH_RESOLVE_HANDLE)
        {
            if (Entries[Existing]->bNeedsResolve)
            {
                RebuildEntry(Existing, Mesh, Overrides);

                if (Entries[Existing]->bResolved)
                {
                    ++TableGeneration;
                    ResolvedAtGeneration[Existing] = TableGeneration;
                }
            }
            return Existing;
        }

        const uint32 NewHandle = (uint32)Entries.Num();
        Entries.Add(Memory::New<FResolvedMesh>());
        EntryStates.Add(MESH_RESOLVE_STATE_STALE);
        ResolvedAtGeneration.Add(0u);
        FResolvedMesh& Entry = *Entries.Last();

        Entry.MeshKey  = Mesh;
        Entry.MeshGuid = Mesh->GetGUID();
        Entry.OverrideKey.Reserve(Overrides.Num());
        for (CMaterialInterface* Override : Overrides)
        {
            Entry.OverrideKey.Add((const void*)Override);
        }

        RebuildEntry(NewHandle, Mesh, Overrides);

        HandlesByHash.FindOrAdd(KeyHash).Add(NewHandle);
        return NewHandle;
    }

    namespace MeshResolveCacheInternal
    {
        // Shaders a surface must actually have to be VISIBLE, as opposed to merely compiled.
        bool HasRequiredPassShaders(CMaterialInterface* Material)
        {
            CMaterial* Concrete = IsValid(Material) ? Material->GetMaterial() : nullptr;
            if (Concrete == nullptr)
            {
                return false;
            }

            return Concrete->HasRequiredStages();
        }
    }

    void MeshResolve::StampSurfaceSource(FResolvedSurface& Surface, CMaterialInterface* RawMaterial)
    {
        Surface.SourceMaterial       = RawMaterial;
        Surface.SourceMaterialGuid   = IsValid(RawMaterial) ? RawMaterial->GetGUID() : FGuid();
        Surface.SourceShaderRevision = (IsValid(RawMaterial) && IsValid(RawMaterial->GetMaterial()))
                                     ? RawMaterial->GetMaterial()->GetShaderRevision() : 0u;
    }

    bool MeshResolve::IsSurfaceStale(const FResolvedSurface& Surface)
    {
        if (!IsValid(Surface.SourceMaterial))
        {
            // Nothing was requested (or it died); the assignment hash covers a pointer change.
            return false;
        }

        // A different asset occupies the slot now, so the revision there means nothing.
        if (Surface.SourceMaterial->GetGUID() != Surface.SourceMaterialGuid)
        {
            return true;
        }

        const CMaterial* Concrete = Surface.SourceMaterial->GetMaterial();
        const uint32 Live = IsValid(Concrete) ? Concrete->GetShaderRevision() : 0u;

        return Live != Surface.SourceShaderRevision;
    }

    bool MeshResolve::ResolveSurfaceMaterial(FResolvedSurface& R, CMaterialInterface* RawMaterial)
    {
        bool bReady = true;

        CMaterialInterface* Material = RawMaterial;
        if (IsValid(Material) && !MaterialDomain::IsMeshlet(Material->GetMaterialType()))
        {
            Material = nullptr;
        }

        if (!IsValid(Material) || !IsValid(Material->GetMaterial()) || !Material->IsReadyForRender())
        {
            if (IsValid(RawMaterial) && !RawMaterial->IsReadyForRender())
            {
                bReady = false;
            }

            // A material still compiling keeps the plain default, so only a real failure shows the checker.
            const CMaterial* Root = IsValid(RawMaterial) ? RawMaterial->GetMaterial() : nullptr;
            CMaterial* ErrorMaterial = CMaterial::GetErrorMaterial();
            const bool bShowError = IsValid(Root) && Root->HasCompileFailed() && IsValid(ErrorMaterial) && ErrorMaterial->IsReadyForRender();
            Material = bShowError ? ErrorMaterial : CMaterial::GetDefaultMaterial();
        }

        // Second gate. IsReadyForRender reports that the material finished COMPILING.
        if (!MeshResolveCacheInternal::HasRequiredPassShaders(Material))
        {
            bReady   = false;
            Material = CMaterial::GetDefaultMaterial();
        }

        if (Material != nullptr && !Material->RequestTexturesResolved())
        {
            bReady   = false;
            Material = CMaterial::GetDefaultMaterial();
        }

        CMaterial* ConcreteMaterial = Material->GetMaterial();

        const EBlendMode BlendMode    = Material->GetBlendMode();
        const bool       bMasked      = BlendMode == EBlendMode::Masked;
        const bool       bAdditive    = BlendMode == EBlendMode::Additive;
        const bool       bModulate    = BlendMode == EBlendMode::Modulate;
        // Everything the forward lane draws, whether the moments resolve it or a blend composites it.
        const bool       bTranslucent = Material->IsOITResolved() || Material->IsUnorderedBlend();
        // Translucency forces two-sided, or a translucent surface reads as a hole from behind.
        const bool       bTwoSided    = bTranslucent || Material->IsTwoSided();

        // The default key resolves to the master's own stage, so a switchless material pays nothing.
        const uint64 SwitchKey = Material->GetStaticSwitchKey();
        auto Stage = [ConcreteMaterial, SwitchKey](EMaterialShaderStage InStage)
        {
            return ConcreteMaterial ? ConcreteMaterial->GetStageForKey(ActiveMeshletStage(InStage), SwitchKey) : FShaderH{};
        };

        R.PixelShader                = Stage(EMaterialShaderStage::Pixel);
        R.VertexShader               = Stage(EMaterialShaderStage::Vertex);
        R.MeshShaderShadow           = Stage(EMaterialShaderStage::MeshShadow);
        R.MeshShaderBase             = Stage(EMaterialShaderStage::MeshBase);
        R.VisBufferMeshShader        = Stage(EMaterialShaderStage::VisBufferMesh);
        R.VisBufferMeshShaderMasked  = Stage(EMaterialShaderStage::VisBufferMeshMasked);
        R.MaskedVisBufferPixelShader = Stage(EMaterialShaderStage::MaskedVisBufferPixel);
        R.MeshShaderShadowMasked     = Stage(EMaterialShaderStage::MeshShadowMasked);
        R.ShadowMaskedPixelShader    = Stage(EMaterialShaderStage::ShadowMaskedPixel);
        R.DeferredShader             = Stage(EMaterialShaderStage::Deferred);

        // Recorded from the RAW request, so a not-ready material that later compiles is noticed.
        MeshResolve::StampSurfaceSource(R, RawMaterial);

        R.MaterialID            = (uint64)ConcreteMaterial;
        R.MaterialIdx           = (uint32)Material->GetMaterialIndex();
        R.bMaterialCastsShadows = Material->DoesCastShadows();

        // Casting is what shadow-only draws, so the two together are the only useful combination.
        const bool bShadowOnly = Material->IsShadowOnly() && R.bMaterialCastsShadows;

        EInstanceFlags MaterialFlags = EInstanceFlags::None;
        if (bTranslucent) { MaterialFlags |= EInstanceFlags::Translucent; }
        if (bMasked)      { MaterialFlags |= EInstanceFlags::Masked; }
        if (bTwoSided)    { MaterialFlags |= EInstanceFlags::TwoSided; }
        if (bShadowOnly)  { MaterialFlags |= EInstanceFlags::ShadowOnly; }
        R.MaterialFlags = MaterialFlags;

        // The OIT pass and the unordered blend pass are the only binders of MeshShaderBase / PixelShader.
        const bool bForwardShaded = bTranslucent;

        // Only the unordered lane can honor it; the OIT lane reads depth and never writes.
        const bool bWriteDepth = Material->WritesDepth() && Material->IsUnorderedBlend();

        R.BatchKey = FDrawBatchKey
        {
            // Only the shaders the batch's OWN passes bind, or it splits on something nothing reads.
            .VisBufferMeshShader        = R.VisBufferMeshShader,
            .VisBufferMeshShaderMasked  = bMasked        ? R.VisBufferMeshShaderMasked  : FShaderH{},
            .MaskedVisBufferPixelShader = bMasked        ? R.MaskedVisBufferPixelShader : FShaderH{},
            .MeshShaderBase             = bForwardShaded ? R.MeshShaderBase             : FShaderH{},
            .MeshShaderShadow           = R.MeshShaderShadow,
            .MeshShaderShadowMasked     = bMasked        ? R.MeshShaderShadowMasked     : FShaderH{},
            .ShadowMaskedPixelShader    = bMasked        ? R.ShadowMaskedPixelShader    : FShaderH{},
            .PixelShader                = bForwardShaded  ? R.PixelShader                : FShaderH{},
            .bTranslucent = (bTranslucent ? 1u : 0u),
            .bMasked      = (bMasked      ? 1u : 0u),
            .bAdditive    = (bAdditive    ? 1u : 0u),
            .bModulate    = (bModulate    ? 1u : 0u),
            .bWriteDepth  = (bWriteDepth  ? 1u : 0u),
            .bTwoSided    = (bTwoSided    ? 1u : 0u),
        };

        return bReady;
    }

    double MeshResolve::MeshletRangeArea(const FMeshletData& MD, uint32 Begin, uint32 Count)
    {
        double Area = 0.0;
        for (uint32 m = Begin; m < Begin + Count; ++m)
        {
            const FMeshlet& M = MD.Meshlets[m];
            for (uint32 t = 0; t < M.TriangleCount; ++t)
            {
                const uint32   Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                const FVector3 A = DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[GetStaticVertexIndex(MD, M, Packed & 0xFFu)]);
                const FVector3 B = DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[GetStaticVertexIndex(MD, M, (Packed >> 8) & 0xFFu)]);
                const FVector3 C = DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[GetStaticVertexIndex(MD, M, (Packed >> 16) & 0xFFu)]);
                Area += 0.5 * (double)Math::Length(Math::Cross(B - A, C - A));
            }
        }
        return Area;
    }

    // The coarsest slot's vertices averaged into one splat, since that slot is what the proxy replaces.
    FInstanceProxy MeshResolve::ComputeSurfaceProxy(const FMeshResource& Resource, const FGeometrySurface& Surface)
    {
        FInstanceProxy Proxy = {};
        Proxy.PackedNormal = PackNormal(FVector3(0.0f, 1.0f, 0.0f));
        Proxy.PackedUV     = Math::PackHalf2x16(FVector2(0.5f, 0.5f));
        Proxy.Color        = 0xFFFFFFFFu;

        const FMeshletData& MD = Resource.MeshletData;
        if (Surface.NumLODs == 0u || MD.IsEmpty())
        {
            return Proxy;
        }

        const uint32 Slot  = Math::Min(Surface.NumLODs, (uint32)MAX_MESH_LODS) - 1u;
        const uint32 Begin = Surface.LODMeshletOffset[Slot];
        const uint32 End   = Math::Min(Begin + Surface.LODMeshletCount[Slot], (uint32)MD.Meshlets.Num());

        FVector3 CenterSum(0.0f);
        FVector3 NormalSum(0.0f);
        FVector2 UVSum(0.0f);
        FVector4 ColorSum(0.0f);
        uint32   Count = 0;
        const bool bStatic = !Resource.bSkinnedMesh && MD.VertexAttributes.Num() == MD.VertexPositions.Num();
        for (uint32 m = Begin; m < End; ++m)
        {
            const FMeshlet& M = MD.Meshlets[m];
            if (!bStatic)
            {
                CenterSum += MD.MeshletSpheres[m].Center;
                ++Count;
                continue;
            }
            for (uint32 v = 0; v < M.VertexCount; ++v)
            {
                const uint32 Vertex = GetStaticVertexIndex(MD, M, v);
                CenterSum += DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[Vertex]);
                NormalSum += UnpackNormal(MD.VertexAttributes[Vertex].Normal);
                UVSum     += Math::UnpackHalf2x16(MD.VertexAttributes[Vertex].UV);
                ColorSum  += MD.VertexColors.IsEmpty() ? FVector4(1.0f) : UnpackColor(MD.VertexColors[Vertex]);
                ++Count;
            }
        }
        if (Count == 0u)
        {
            return Proxy;
        }

        const float Inv = 1.0f / (float)Count;
        Proxy.LocalCenter = CenterSum * Inv;
        if (bStatic)
        {
            if (Math::Dot(NormalSum, NormalSum) > 1e-12f)
            {
                Proxy.PackedNormal = PackNormal(Math::Normalize(NormalSum));
            }
            Proxy.PackedUV = Math::PackHalf2x16(UVSum * Inv);
            Proxy.Color    = PackColor(ColorSum * Inv);
        }

        // Grown to the farthest meshlet sphere, so the splat covers everything the slot drew.
        float Radius = 0.0f;
        for (uint32 m = Begin; m < End; ++m)
        {
            const FMeshletSphere& S = MD.MeshletSpheres[m];
            Radius = Math::Max(Radius, Math::Length(S.Center - Proxy.LocalCenter) + S.Radius);
        }

        // A convex surface's mean projected area is a quarter of its area, so the square takes that and a sparse canopy shrinks with it.
        const float MeanProjectedArea = bStatic ? (float)MeshResolve::MeshletRangeArea(MD, Begin, End - Begin) * 0.25f : 4.0f * Radius * Radius;
        Proxy.SplatHalfSize = Math::Min(Math::Sqrt(MeanProjectedArea) * 0.5f, Radius);
        return Proxy;
    }

    void FMeshResolveCache::ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        ++Out.Generation;

        Out.Dependencies.Reset();
        Out.Dependencies.Add((const void*)Mesh);

        if (Mesh->HasAnyFlag(OF_NeedsLoad))
        {
            Out.Surfaces.Reset();
            Out.MeshletHeaderSlot = 0;
            Out.bAllMaterialsReady   = false;
            Out.bResolved            = false;
            return;
        }

        const FMeshResource& Resource = Mesh->GetMeshResource();

        const SAABB LocalBounds = Mesh->GetCullBounds();
        Out.LocalCenter = (LocalBounds.Min + LocalBounds.Max) * 0.5f;
        // Degenerate bounds (geometry not resident) collapse to a zero sphere rather than a NaN radius.
        Out.LocalRadius = (LocalBounds.Max.x < LocalBounds.Min.x)
            ? 0.0f
            : Math::Length(LocalBounds.Max - Out.LocalCenter);

        Out.MeshletHeaderSlot = Mesh->GetMeshBuffers().MeshletHeaderSlot;
        Out.bAllMaterialsReady   = true;

        Out.Surfaces.Reset();
        Out.Surfaces.Reserve(Resource.GeometrySurfaces.Num());

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            FResolvedSurface& R = Out.Surfaces.EmplaceGetRef();

            R.NumLODs     = Surface.NumLODs;
            R.TexelFactor = Surface.TexelFactor;
            R.LODFlags    = Surface.LODFlags;
            R.LODErrorMultiplier = Mesh->LODDistanceScale;
            R.Proxy       = MeshResolve::ComputeSurfaceProxy(Resource, Surface);

            for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
            {
                R.LODMeshletOffset[LOD] = Surface.LODMeshletOffset[LOD];
                R.LODMeshletCount[LOD]  = Surface.LODMeshletCount[LOD];
                R.LODParentError[LOD]   = Surface.LODParentError[LOD];
                R.LODError[LOD]         = Surface.LODError[LOD];
            }

            // -1 means unassigned, which IsValidIndex rejects so the slot falls through to the default.
            const int32 Slot = Surface.MaterialIndex;
            CMaterialInterface* RawMaterial = nullptr;
            if (Overrides.IsValidIndex(Slot))
            {
                RawMaterial = Overrides[Slot];
            }
            if (!RawMaterial)
            {
                RawMaterial = Mesh->GetMaterialAtSlot(Slot);
            }

            if (!MeshResolve::ResolveSurfaceMaterial(R, RawMaterial))
            {
                Out.bAllMaterialsReady = false;
            }
            if (Mesh->HasDistanceField())
            {
                R.MaterialFlags |= EInstanceFlags::HasDistanceField;
            }

            MeshResolveCacheInternal::AddDependency(Out, (const void*)RawMaterial);
            MeshResolveCacheInternal::AddDependency(Out, (const void*)(uintptr_t)R.MaterialID);

            // MaterialID names the SUBSTITUTED material, so a fallback surface is not keyed on its real master.
            if (IsValid(RawMaterial))
            {
                MeshResolveCacheInternal::AddDependency(Out, (const void*)RawMaterial->GetMaterial());
            }
        }

        // Geometry alone gates drawing. A surface whose material is still compiling draws with the default one, and the material's invalidation swaps it back.
        Out.bResolved = Out.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot;
    }
}
