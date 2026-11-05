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

        // The tracked includes the caller writes in place, whose rows are stamped as they are handed out.
        uint32                     StampMask = 0;
    };

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
    }

    // TQuery's filtered walk: the first filter's pool by its ticks, then every other filter, with the chunk walk matching the includes after.
    void SnapshotFiltered(ECS::FRegistry& Registry, FViewState& State, const void* const* FilterOps, int NFilter, uint32 AddedMask, uint32 Since)
    {
        State.Entities.Reset();
        State.Remaining = 0;

        for (int f = 0; f < NFilter; ++f)
        {
            ECS::FSparseSet* Pool = FindStorage(Registry, FilterOps[f]);
            if (Pool != nullptr && !Pool->IsTrackingChanges())
            {
                Pool->TrackChanges();
            }
        }

        const ECS::FSparseSet* First = FindStorage(Registry, FilterOps[0]);
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
                const ECS::FSparseSet* Pool = FindStorage(Registry, FilterOps[f]);
                bPasses = Pool != nullptr && ((AddedMask & (1u << f)) != 0 ? Pool->HasAddedSince(Entity, Since) : Pool->HasChangedSince(Entity, Since));
            }
            if (bPasses)
            {
                State.Entities.Add(Entity);
            }
        }
        State.Remaining = State.Entities.Num();
    }
}

// The runtime view is consumed here, and only the snapshot and storages live on. Every include is written in place.
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

// A C# query field's view: only its Write includes stamp, and with filters it holds just the rows written or added after Since.
LUMINA_DOTNET_EXPORT(void*, QueryViewBegin)(uint64 World, const void* const* IncludeOps, int NInc, uint32 WriteMask, const void* const* ExcludeOps, int NExc,
                                            const void* const* RequireOps, int NReq, const void* const* FilterOps, int NFilter, uint32 AddedMask, uint32 Since)
{
    Lumina::CWorld* W = Lumina::DotNet::AsWorld(World);
    if (W == nullptr || IncludeOps == nullptr || NInc <= 0)
    {
        return nullptr;
    }

    Lumina::ECS::FRegistry& Registry = W->GetEntityRegistry();
    Lumina::DotNetViewInternal::FViewState* State = Lumina::DotNetViewInternal::BeginView(Registry, IncludeOps, NInc, WriteMask, ExcludeOps, NExc, RequireOps, NReq);
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

// Pointers resolve fresh per chunk, so a storage realloc between chunks is harmless.
LUMINA_DOTNET_EXPORT(int, ViewNextChunk)(void* StatePtr, uint32* OutEntities, void** OutPtrs, int MaxCount, int NInclude)
{
    Lumina::DotNetViewInternal::FViewState* State = static_cast<Lumina::DotNetViewInternal::FViewState*>(StatePtr);
    if (State == nullptr || OutEntities == nullptr || OutPtrs == nullptr || MaxCount <= 0 || State->Driver == nullptr)
    {
        return 0;
    }

    const int N = State->IncludeStorages.Num();
    const int K = NInclude < N ? NInclude : N;
    Lumina::ECS::FSparseSet* const* Includes = State->IncludeStorages.GetData();
    Lumina::ECS::FSparseSet* const* Excludes = State->ExcludeStorages.GetData();
    const int32 NumExcludes = State->ExcludeStorages.Num();
    Lumina::ECS::FSparseSet* const* Requires = State->RequireStorages.GetData();
    const int32 NumRequires = State->RequireStorages.Num();
    const Lumina::ECS::FEntity* Snapshot = State->Entities.GetData();

    const Lumina::ECS::FSparseSet* Driver = State->Driver;
    const int32 DriverInclude = State->DriverInclude;
    const Lumina::ECS::FEntity* DriverDense = Driver->GetDenseData();
    const size_t DriverSize = Driver->GetDenseSize();

    // An unpaged driver's payload is one block, so a row still in its snapshot slot costs a multiply.
    const bool bDriverBlock = !Driver->IsTagOnly() && !Driver->IsPaged();
    uint8* const DriverBlock = Driver->GetPackedBlock();
    const size_t DriverStride = Driver->GetElementSize();

    // A local, since the row writes could alias State and would pin the cursor in memory.
    size_t Remaining = State->Remaining;
    int Count = 0;
    while (Count < MaxCount && Remaining > 0)
    {
        const size_t Slot = --Remaining;
        const Lumina::ECS::FEntity Entity = Snapshot[Slot];
        if (Entity.IsTombstone())
        {
            continue;
        }

        // Written before the match is known, since a row the loop rejects is simply overwritten by the next.
        void** Row = OutPtrs + (size_t)Count * (size_t)NInclude;

        void* DriverPayload = nullptr;
        if (Slot < DriverSize && DriverDense[Slot] == Entity)
        {
            DriverPayload = bDriverBlock ? DriverBlock + Slot * DriverStride : Driver->GetRawAtDense((uint32)Slot);
        }
        else
        {
            DriverPayload = Driver->GetRaw(Entity);
            if (DriverPayload == nullptr && !(Driver->IsTagOnly() && Driver->Contains(Entity)))
            {
                continue;
            }
        }
        if (DriverInclude < K)
        {
            Row[DriverInclude] = DriverPayload;
        }

        bool bMatches = true;
        for (int k = 0; k < N && bMatches; ++k)
        {
            if (k == DriverInclude)
            {
                continue;
            }
            const Lumina::ECS::FSparseSet* Storage = Includes[k];
            void* Payload = nullptr;
            if (Storage->IsTagOnly())
            {
                bMatches = Storage->Contains(Entity);
            }
            else
            {
                Payload = Storage->GetRaw(Entity);
                bMatches = Payload != nullptr;
            }
            if (k < K)
            {
                Row[k] = Payload;
            }
        }
        for (int32 e = 0; e < NumExcludes && bMatches; ++e)
        {
            bMatches = !Excludes[e]->Contains(Entity);
        }
        for (int32 r = 0; r < NumRequires && bMatches; ++r)
        {
            bMatches = Requires[r]->Contains(Entity);
        }
        if (!bMatches)
        {
            continue;
        }

        if (State->StampMask != 0)
        {
            for (int k = 0; k < N; ++k)
            {
                if ((State->StampMask & (1u << k)) != 0)
                {
                    Includes[k]->StampEntity(Entity);
                }
            }
        }

        OutEntities[Count] = static_cast<uint32>(Entity.Value);
        ++Count;
    }

    State->Remaining = Remaining;
    return Count;
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
    LUMINA_DOTNET_SIG(ViewEnd)
);
