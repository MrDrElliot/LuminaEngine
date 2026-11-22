#include "DotNetExport.h"
#include "ExportSignature.h"
#include "Memory/Construct.h"
#include "World/ECS/Registry.h"
#include "Containers/Vector.h"
#include "Memory/Memory.h"
#include "World/World.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Systems/SystemContext.h"

// One boundary crossing per CHUNK rather than per entity, rebinding a reused wrapper handle.

namespace Lumina::DotNetViewInternal
{
    // Snapshotting rather than holding a live iterator is what makes the view mutation-safe.
    struct FViewState
    {
        // The entities ViewBegin will offer, walked back to front like a native view.
        TVector<ECS::FEntity>      Entities;
        size_t                     Remaining = 0;
        const ECS::FSparseSet*     Driver = nullptr;
        int32                      DriverInclude = 0;
        TVector<ECS::FSparseSet*>  IncludeStorages;
        TVector<ECS::FSparseSet*>  ExcludeStorages;
        TVector<ECS::FSparseSet*>  RequireStorages;

        // The tracked includes a query writes in place, whose rows hand out their change-tick slots so a write handle stamps only what it touches.
        uint32                     StampMask = 0;
        uint32                     StampTick = 0;
        uint32                     TickSink = 0;

        // Set when the snapshot is the driver's dense array as it stood, so an unmoved driver finds each row at its snapshot slot.
        bool                       bSnapshotIsDriver = false;
    };

    // Mirrors RowProbe in View.cs, one pool's lookup, payload and stamp layout for a walk that finds its rows in C#.
    struct FManagedRowProbe
    {
        const ECS::FEntity* const* Pages;
        uint64 PageCount;
        uint8* const* PayloadPages;
        uint8* Packed;
        uint32* TickBase;
        const uint32* EpochSlot;
        uint32 ElementSize;
        uint32 PageShift;
        uint32 PageMask;
        uint32 TickStride;
        uint32 bPaged;
        uint32 Epoch;
    };

    static_assert(sizeof(FManagedRowProbe) == 72, "RowProbe in View.cs mirrors this layout");

    // Written in place, since a packed pool reads as one page whose pointer is the probe's own Packed field.
    void WriteManagedProbe(FManagedRowProbe& Out, const ECS::FSparseSet& Pool, uint32* TickBase, uint32 TickStride)
    {
        constexpr uint32 PackedAsOnePageShift = 31;
        const ECS::FRowProbe Row = Pool.MakeRowProbe();
        const bool bOnePage = !Row.bPaged || Row.ElementSize == 0;
        Out = FManagedRowProbe{ Row.Membership.Pages, Row.Membership.PageCount, nullptr, Row.ElementSize != 0 ? Row.Packed : nullptr, TickBase, Pool.GetLayoutEpoch(),
                                Row.ElementSize, bOnePage ? PackedAsOnePageShift : Row.PageShift, bOnePage ? (1u << PackedAsOnePageShift) - 1u : Row.PageMask, TickStride,
                                Row.bPaged ? 1u : 0u, *Pool.GetLayoutEpoch() };
        Out.PayloadPages = bOnePage ? &Out.Packed : Row.PayloadPages;
    }

    NODISCARD uint32 AllIncludes(int NInc)
    {
        return NInc >= 32 ? ~0u : (1u << NInc) - 1u;
    }

    NODISCARD ECS::FSparseSet* FindStorage(ECS::FRegistry& Registry, const void* Ops)
    {
        return Ops != nullptr ? Registry.FindStorage(static_cast<uint32>(static_cast<const FComponentOps*>(Ops)->TypeId)) : nullptr;
    }

    // Leaves IncludeStorages empty when an include or a required pool is missing or empty, which reads as an empty view.
    NODISCARD FViewState* BeginView(ECS::FRegistry& Registry, const void* const* IncludeOps, int NInc, uint32 WriteMask, const void* const* ExcludeOps, int NExc,
                                    const void* const* RequireOps, int NReq)
    {
        FViewState* State = Memory::ConstructAt(static_cast<FViewState*>(Memory::Malloc(sizeof(FViewState), alignof(FViewState))));
        State->IncludeStorages.Reserve(NInc);

        for (int i = 0; i < NInc; ++i)
        {
            ECS::FSparseSet* Storage = FindStorage(Registry, IncludeOps[i]);
            if (Storage == nullptr)
            {
                State->IncludeStorages.Reset();
                State->StampMask = 0;
                return State;
            }
            State->IncludeStorages.Add(Storage);
            if ((WriteMask & (1u << i)) != 0 && Storage->IsTrackingChanges())
            {
                Storage->MarkPointersEscaped();
                State->StampTick = ECS::FSparseSet::GetChangeTick();
                Storage->NoteStampTick(State->StampTick);
                State->StampMask |= 1u << i;
            }

            // The cheapest snapshot drives, by dense slots as FRuntimeView picks it, and an empty pool outranks every size.
            const auto ScanCost = [](const ECS::FSparseSet* Set) { return Set->Num() == 0 ? 0 : Set->GetDenseSize(); };
            if (State->Driver == nullptr || ScanCost(Storage) < ScanCost(State->Driver))
            {
                State->Driver = Storage;
                State->DriverInclude = i;
            }
        }

        for (int i = 0; i < NExc; ++i)
        {
            if (ECS::FSparseSet* Storage = ExcludeOps != nullptr ? FindStorage(Registry, ExcludeOps[i]) : nullptr)
            {
                State->ExcludeStorages.Add(Storage);
            }
        }

        for (int i = 0; i < NReq; ++i)
        {
            ECS::FSparseSet* Storage = FindStorage(Registry, RequireOps[i]);
            if (Storage == nullptr || Storage->Num() == 0)
            {
                State->IncludeStorages.Reset();
                State->StampMask = 0;
                return State;
            }
            State->RequireStorages.Add(Storage);
        }
        return State;
    }

    // Matching waits for the chunk walk, which looks every row up anyway, so the snapshot is one copy.
    void SnapshotDriver(FViewState& State)
    {
        const ECS::FEntity* Dense = State.Driver->GetDenseData();
        State.Entities.Reset();
        State.Entities.Append(Dense, Dense + State.Driver->GetDenseSize());
        State.Remaining = State.Entities.Num();
        State.bSnapshotIsDriver = true;
    }

    // TQuery's filtered walk: the first filter's pool by its ticks, then every other filter, with the chunk walk matching the includes after.
    void SnapshotFiltered(ECS::FRegistry& Registry, FViewState& State, const void* const* FilterOps, int NFilter, uint32 AddedMask, uint32 Since)
    {
        State.Entities.Reset();
        State.Remaining = 0;

        TInlineVector<ECS::FSparseSet*, 8> Pools;
        Pools.Reserve(NFilter);
        for (int f = 0; f < NFilter; ++f)
        {
            ECS::FSparseSet* Pool = FindStorage(Registry, FilterOps[f]);
            if (Pool != nullptr && !Pool->IsTrackingChanges())
            {
                Pool->TrackChanges();
            }
            Pools.Add(Pool);
        }

        const ECS::FSparseSet* First = Pools[0];
        if (First == nullptr)
        {
            return;
        }

        const uint32* Ticks = (AddedMask & 1u) != 0 ? First->GetAddedTickData() : First->GetChangeTickData();
        const ECS::FEntity* Dense = First->GetDenseData();
        const int32 Size = static_cast<int32>(First->GetDenseSize());
        for (int32 Index = 0; Index < Size; ++Index)
        {
            const ECS::FEntity Entity = Dense[Index];
            if (Entity.IsTombstone() || (Ticks != nullptr && !ECS::FSparseSet::IsNewerTick(Ticks[Index], Since)))
            {
                continue;
            }

            bool bPasses = true;
            for (int f = 1; f < NFilter && bPasses; ++f)
            {
                const ECS::FSparseSet* Pool = Pools[f];
                bPasses = Pool != nullptr && ((AddedMask & (1u << f)) != 0 ? Pool->HasAddedSince(Entity, Since) : Pool->HasChangedSince(Entity, Since));
            }
            if (bPasses)
            {
                State.Entities.Add(Entity);
            }
        }
        State.Remaining = State.Entities.Num();
    }

    // Every pool's layout is read once per chunk into locals, so the output writes cannot force a reload per row.
    template<int N>
    int GatherChunk(FViewState& State, uint32* RESTRICT OutEntities, void** RESTRICT OutPtrs, uint32** RESTRICT OutTicks, int MaxCount)
    {
        ECS::FRowProbe Probes[N];
        uint32* TickBases[N];
        uint32 TickStrides[N];
        for (int k = 0; k < N; ++k)
        {
            ECS::FSparseSet* Pool = State.IncludeStorages[k];
            Probes[k] = Pool->MakeRowProbe();
            const bool bStamps = (State.StampMask & (1u << k)) != 0;
            uint32* Tracked = bStamps ? Pool->GetChangeTickSlot(0, State.TickSink) : nullptr;
            TickBases[k] = Tracked;
            TickStrides[k] = bStamps && Pool->IsTrackingChanges() ? 1u : 0u;
        }

        constexpr int32 MaxMembershipProbes = 4;
        ECS::FMembershipProbe Excludes[MaxMembershipProbes];
        ECS::FMembershipProbe Requires[MaxMembershipProbes];
        const int32 NumExcludes = Math::Min(State.ExcludeStorages.Num(), MaxMembershipProbes);
        const int32 NumRequires = Math::Min(State.RequireStorages.Num(), MaxMembershipProbes);
        for (int32 e = 0; e < NumExcludes; ++e)
        {
            Excludes[e] = State.ExcludeStorages[e]->MakeProbe();
        }
        for (int32 r = 0; r < NumRequires; ++r)
        {
            Requires[r] = State.RequireStorages[r]->MakeProbe();
        }

        const int32 DriverInclude = State.DriverInclude;
        const ECS::FEntity* DriverDense = State.Driver->GetDenseData();
        const size_t DriverSize = State.Driver->GetDenseSize();
        const ECS::FEntity* Snapshot = State.Entities.GetData();

        size_t Remaining = State.Remaining;
        int Count = 0;
        while (Count < MaxCount && Remaining > 0)
        {
            const size_t Slot = --Remaining;
            const ECS::FEntity Entity = Snapshot[Slot];
            if (Entity.IsTombstone())
            {
                continue;
            }

            uint32 Dense[N];
            bool bMatches = true;
            for (int k = 0; k < N; ++k)
            {
                const bool bDriverSlot = k == DriverInclude && Slot < DriverSize && DriverDense[Slot] == Entity;
                Dense[k] = bDriverSlot ? static_cast<uint32>(Slot) : Probes[k].FindLiveDense(Entity);
                bMatches &= Dense[k] != ECS::FRowProbe::InvalidDense;
            }
            for (int32 e = 0; e < NumExcludes && bMatches; ++e)
            {
                bMatches = !Excludes[e].Contains(Entity);
            }
            for (int32 r = 0; r < NumRequires && bMatches; ++r)
            {
                bMatches = Requires[r].Contains(Entity);
            }
            if (!bMatches)
            {
                continue;
            }

            void** Row = OutPtrs + static_cast<size_t>(Count) * N;
            for (int k = 0; k < N; ++k)
            {
                Row[k] = Probes[k].PayloadAt(Dense[k]);
            }
            if (OutTicks != nullptr)
            {
                uint32** TickRow = OutTicks + static_cast<size_t>(Count) * N;
                for (int k = 0; k < N; ++k)
                {
                    TickRow[k] = TickBases[k] != nullptr ? TickBases[k] + static_cast<size_t>(Dense[k]) * TickStrides[k] : nullptr;
                }
            }
            OutEntities[Count] = static_cast<uint32>(Entity.Value);
            ++Count;
        }

        State.Remaining = Remaining;
        return Count;
    }
}

// The runtime view is consumed here, and only the snapshot and storages live on. Every include is handed out writable and, like a plain T walk, never stamped.
LUMINA_DOTNET_EXPORT(void*, ViewBegin)(uint64 World, const void* const* IncludeOps, int NInc, const void* const* ExcludeOps, int NExc)
{
    Lumina::CWorld* W = Lumina::DotNet::AsWorld(World);
    if (W == nullptr || IncludeOps == nullptr || NInc <= 0)
    {
        return nullptr;
    }

    Lumina::DotNetViewInternal::FViewState* State = Lumina::DotNetViewInternal::BeginView(W->GetEntityRegistry(), IncludeOps, NInc, Lumina::DotNetViewInternal::AllIncludes(NInc), ExcludeOps, NExc, nullptr, 0);
    if (!State->IncludeStorages.IsEmpty())
    {
        Lumina::DotNetViewInternal::SnapshotDriver(*State);
    }
    return State;
}

// A C# query field's view, whose Write includes hand out tick slots stamped with OutStampTick, and which with filters holds just the rows written or added after Since.
LUMINA_DOTNET_EXPORT(void*, QueryViewBegin)(uint64 World, const void* const* IncludeOps, int NInc, uint32 WriteMask, const void* const* ExcludeOps, int NExc,
                                            const void* const* RequireOps, int NReq, const void* const* FilterOps, int NFilter, uint32 AddedMask, uint32 Since, uint32* OutStampTick)
{
    Lumina::CWorld* W = Lumina::DotNet::AsWorld(World);
    if (W == nullptr || IncludeOps == nullptr || NInc <= 0)
    {
        return nullptr;
    }

    Lumina::ECS::FRegistry& Registry = W->GetEntityRegistry();
    Lumina::DotNetViewInternal::FViewState* State = Lumina::DotNetViewInternal::BeginView(Registry, IncludeOps, NInc, WriteMask, ExcludeOps, NExc, RequireOps, NReq);
    *OutStampTick = State->StampTick;
    if (State->IncludeStorages.IsEmpty())
    {
        return State;
    }
    if (FilterOps != nullptr && NFilter > 0)
    {
        Lumina::DotNetViewInternal::SnapshotFiltered(Registry, *State, FilterOps, NFilter, AddedMask, Since);
    }
    else
    {
        Lumina::DotNetViewInternal::SnapshotDriver(*State);
    }
    return State;
}

// What a filtered query's CompleteRun does: the tick its next run reads changes after.
LUMINA_DOTNET_EXPORT(uint32, AdvanceChangeTick)()
{
    return Lumina::ECS::FSparseSet::AdvanceChangeTick();
}

// Pointers resolve fresh per chunk, so a storage realloc between chunks is harmless. OutTicks, when given, gets each row's change-tick slot per include, null where nothing stamps.
LUMINA_DOTNET_EXPORT(int, ViewNextChunk)(void* StatePtr, uint32* OutEntities, void** OutPtrs, uint32** OutTicks, int MaxCount, int NInclude)
{
    using namespace Lumina::DotNetViewInternal;
    FViewState* State = static_cast<FViewState*>(StatePtr);
    if (State == nullptr || OutEntities == nullptr || OutPtrs == nullptr || MaxCount <= 0 || State->Driver == nullptr || NInclude != State->IncludeStorages.Num())
    {
        return 0;
    }

    switch (NInclude)
    {
    case 1: return GatherChunk<1>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 2: return GatherChunk<2>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 3: return GatherChunk<3>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 4: return GatherChunk<4>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 5: return GatherChunk<5>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 6: return GatherChunk<6>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 7: return GatherChunk<7>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    case 8: return GatherChunk<8>(*State, OutEntities, OutPtrs, OutTicks, MaxCount);
    default: return 0;
    }
}

// The entities a walk that finds its own rows visits, back to front, valid until ViewEnd. OutDriverInclude is the include whose dense slots they are, or -1.
LUMINA_DOTNET_EXPORT(int, ViewSnapshot)(void* StatePtr, const uint32** OutEntities, int* OutDriverInclude)
{
    const Lumina::DotNetViewInternal::FViewState* State = static_cast<const Lumina::DotNetViewInternal::FViewState*>(StatePtr);
    const bool bLive = State != nullptr && State->Driver != nullptr;
    *OutEntities = bLive ? reinterpret_cast<const uint32*>(State->Entities.GetData()) : nullptr;
    *OutDriverInclude = bLive && State->bSnapshotIsDriver ? State->DriverInclude : -1;
    return bLive ? static_cast<int>(State->Remaining) : 0;
}

// Every include's probe, then every exclude's, then every With pool's, fetched again once any of their epochs moves.
LUMINA_DOTNET_EXPORT(int, ViewRowProbes)(void* StatePtr, void* OutProbes, int MaxProbes, int* OutExcludes, int* OutRequires, const uint32** OutGeneration, uint32* OutSeenGeneration)
{
    using namespace Lumina::DotNetViewInternal;
    FViewState* State = static_cast<FViewState*>(StatePtr);
    FManagedRowProbe* Out = static_cast<FManagedRowProbe*>(OutProbes);
    *OutExcludes = 0;
    *OutRequires = 0;

    // Read before the probes, so a move while they are built still shows as a newer generation.
    std::atomic<Lumina::uint32>* Generation = Lumina::ECS::FComponentTypeRegistry::Get().GetLayoutGeneration();
    *OutGeneration = reinterpret_cast<const Lumina::uint32*>(Generation);
    *OutSeenGeneration = Generation->load(std::memory_order_relaxed);
    if (State == nullptr || State->Driver == nullptr)
    {
        return 0;
    }

    const int Includes = State->IncludeStorages.Num();
    const int Excludes = State->ExcludeStorages.Num();
    const int Requires = State->RequireStorages.Num();
    if (Includes + Excludes + Requires > MaxProbes)
    {
        return 0;
    }

    int Written = 0;
    for (int k = 0; k < Includes; ++k)
    {
        Lumina::ECS::FSparseSet* Pool = State->IncludeStorages[k];
        const bool bStamps = (State->StampMask & (1u << k)) != 0;
        WriteManagedProbe(Out[Written++], *Pool, bStamps ? Pool->GetChangeTickSlot(0, State->TickSink) : nullptr, bStamps && Pool->IsTrackingChanges() ? 1u : 0u);
    }
    for (Lumina::ECS::FSparseSet* Pool : State->ExcludeStorages)
    {
        WriteManagedProbe(Out[Written++], *Pool, nullptr, 0);
    }
    for (Lumina::ECS::FSparseSet* Pool : State->RequireStorages)
    {
        WriteManagedProbe(Out[Written++], *Pool, nullptr, 0);
    }
    *OutExcludes = Excludes;
    *OutRequires = Requires;
    return Written;
}

// Each include's layout epoch, so a handle the view hands out can tell when its pool has moved.
LUMINA_DOTNET_EXPORT(void, ViewIncludeEpochs)(void* StatePtr, const uint32** OutSlots, int NInclude)
{
    const Lumina::DotNetViewInternal::FViewState* State = static_cast<const Lumina::DotNetViewInternal::FViewState*>(StatePtr);
    for (int i = 0; i < NInclude; ++i)
    {
        const bool bKnown = State != nullptr && i < State->IncludeStorages.Num();
        OutSlots[i] = bKnown ? State->IncludeStorages[i]->GetLayoutEpoch() : nullptr;
    }
}

// Frees the per-call view state allocated by ViewBegin.
LUMINA_DOTNET_EXPORT(void, ViewEnd)(void* StatePtr)
{
    Lumina::DotNetViewInternal::FViewState* State = static_cast<Lumina::DotNetViewInternal::FViewState*>(StatePtr);
    if (State != nullptr)
    {
        State->~FViewState();
        Lumina::Memory::Free(State);
    }
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(ViewBegin),
    LUMINA_DOTNET_SIG(QueryViewBegin),
    LUMINA_DOTNET_SIG(AdvanceChangeTick),
    LUMINA_DOTNET_SIG(ViewNextChunk),
    LUMINA_DOTNET_SIG(ViewIncludeEpochs),
    LUMINA_DOTNET_SIG(ViewSnapshot),
    LUMINA_DOTNET_SIG(ViewRowProbes),
    LUMINA_DOTNET_SIG(ViewEnd)
);
