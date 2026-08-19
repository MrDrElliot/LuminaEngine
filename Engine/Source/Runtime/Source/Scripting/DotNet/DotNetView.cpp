#include "DotNetExport.h"
#include "Containers/Vector.h"
#include "Memory/Memory.h"
#include "World/World.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Systems/SystemContext.h"

// The C# View<...> path: native does the entt::runtime_view iteration (type-erased, no templates cross
// the boundary) and GATHERS results into parallel chunk buffers the C# side consumes. One boundary
// crossing per CHUNK (ViewNextChunk), not per entity. Per element, C# rebinds a REUSED wrapper Handle to
// the live component pointer this gathers; field access then uses the existing property interop.

using namespace Lumina;

namespace
{
    FEntityRegistry* LmViewRegistryFromWorld(uint64 World)
    {
        CWorld* W = DotNet::AsWorld(World);
        return W ? &ECS::GetWorldRegistry(*W) : nullptr;
    }

    // Per-call view state, heap-allocated by ViewBegin and freed by ViewEnd. Holds the matched-entity
    // SNAPSHOT (not a live iterator), a cursor into it, and the include + exclude storages so each chunk
    // re-validates and re-resolves per-entity component pointers. Snapshotting (rather than holding a live
    // runtime_view iterator across the C#-side chunk loop) is what makes the view structural-mutation safe:
    // the Each/foreach body can Emplace/Remove via SystemContext.Registry without UB.
    struct FViewState
    {
        TVector<entt::entity>               Entities;
        size_t                              Cursor = 0;
        TVector<entt::basic_sparse_set<>*>  IncludeStorages;
        TVector<entt::basic_sparse_set<>*>  ExcludeStorages;
    };
}

// Builds an entt::runtime_view over the include component storages, excluding the exclude storages, and
// SNAPSHOTS the matching entity ids into the returned opaque state (null if any include storage is missing
// -> empty view). IncludeOps / ExcludeOps are arrays of const FComponentOps* tokens (resolved C#-side via
// FindComponentOps). The runtime_view is consumed here and not retained; only the snapshot + storages live on.
LUMINA_DOTNET_EXPORT(void*, ViewBegin)(uint64 World, const void* const* IncludeOps, int NInc, const void* const* ExcludeOps, int NExc)
{
    FEntityRegistry* Registry = LmViewRegistryFromWorld(World);
    if (Registry == nullptr || IncludeOps == nullptr || NInc <= 0)
    {
        return nullptr;
    }

    FViewState* State = new (Memory::Malloc(sizeof(FViewState), alignof(FViewState))) FViewState();
    State->IncludeStorages.reserve(NInc);

    entt::runtime_view View;
    for (int i = 0; i < NInc; ++i)
    {
        const FComponentOps* Ops = static_cast<const FComponentOps*>(IncludeOps[i]);
        entt::basic_sparse_set<>* Storage = Ops ? Registry->storage(static_cast<entt::id_type>(Ops->TypeId)) : nullptr;
        if (Storage == nullptr)
        {
            // A never-emplaced (or unknown) include type -> the view is empty (no snapshot).
            State->IncludeStorages.clear();
            return State;
        }
        State->IncludeStorages.push_back(Storage);
        View.iterate(*Storage);
    }

    for (int i = 0; i < NExc; ++i)
    {
        const FComponentOps* Ops = ExcludeOps ? static_cast<const FComponentOps*>(ExcludeOps[i]) : nullptr;
        if (Ops != nullptr)
        {
            if (entt::basic_sparse_set<>* Storage = Registry->storage(static_cast<entt::id_type>(Ops->TypeId)))
            {
                State->ExcludeStorages.push_back(Storage);
                View.exclude(*Storage);
            }
        }
    }

    // Snapshot up front. Additions to the view during iteration are intentionally not visited (defined),
    // and removals are handled by the per-chunk re-validation below.
    for (const entt::entity Entity : View)
    {
        State->Entities.push_back(Entity);
    }
    return State;
}

// Fills up to MaxCount entities (from the snapshot cursor) into OutEntities and, for each emitted entity i
// and include k, OutPtrs[i*NInclude + k] = the live component pointer. Each snapshot entry is re-validated
// against the live storages (skipped if it lost an include or gained an exclude since ViewBegin) and its
// pointers are resolved fresh, so a storage realloc between chunks is harmless. Returns the count emitted
// (0 when the snapshot is exhausted). One crossing per chunk.
LUMINA_DOTNET_EXPORT(int, ViewNextChunk)(void* StatePtr, uint32* OutEntities, void** OutPtrs, int MaxCount, int NInclude)
{
    FViewState* State = static_cast<FViewState*>(StatePtr);
    if (State == nullptr || OutEntities == nullptr || OutPtrs == nullptr || MaxCount <= 0)
    {
        return 0;
    }

    const int N = (int)State->IncludeStorages.size();
    const int K = NInclude < N ? NInclude : N;
    const size_t Total = State->Entities.size();

    int Count = 0;
    while (Count < MaxCount && State->Cursor < Total)
    {
        const entt::entity Entity = State->Entities[State->Cursor++];

        bool bMatches = true;
        for (int k = 0; k < N && bMatches; ++k)
        {
            if (!State->IncludeStorages[k]->contains(Entity)) { bMatches = false; }
        }
        for (size_t e = 0; e < State->ExcludeStorages.size() && bMatches; ++e)
        {
            if (State->ExcludeStorages[e]->contains(Entity)) { bMatches = false; }
        }
        if (!bMatches)
        {
            continue; // removed from an include / added to an exclude since the snapshot -> skip
        }

        OutEntities[Count] = static_cast<uint32>(entt::to_integral(Entity));
        void** Row = OutPtrs + (size_t)Count * (size_t)NInclude;
        for (int k = 0; k < K; ++k)
        {
            Row[k] = State->IncludeStorages[k]->value(Entity);
        }
        ++Count;
    }

    return Count;
}

// Frees the per-call view state allocated by ViewBegin.
LUMINA_DOTNET_EXPORT(void, ViewEnd)(void* StatePtr)
{
    FViewState* State = static_cast<FViewState*>(StatePtr);
    if (State != nullptr)
    {
        State->~FViewState();
        Memory::Free(State);
    }
}

// The C# system reaches its registry through the system context: returns the CWorld* (as uint64) the
// FSystemContext is bound to, so SystemContext.Registry can build an EntityRegistry / View over it.
LUMINA_DOTNET_EXPORT(uint64, SystemContext_GetWorld)(const FSystemContext* Ctx)
{
    if (Ctx == nullptr)
    {
        return 0;
    }
    CWorld* W = Ctx->GetRegistry().ctx().get<CWorld*>();
    return reinterpret_cast<uint64>(W);
}
