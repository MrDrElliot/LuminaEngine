#include "DotNetExport.h"
#include "Containers/Vector.h"
#include "Memory/Memory.h"
#include "World/World.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Systems/SystemContext.h"

// One boundary crossing per CHUNK rather than per entity, rebinding a reused wrapper handle.

using namespace Lumina;

namespace
{
    FEntityRegistry* LmViewRegistryFromWorld(uint64 World)
    {
        CWorld* W = DotNet::AsWorld(World);
        return W ? &ECS::GetWorldRegistry(*W) : nullptr;
    }

    // Snapshotting rather than holding a live iterator is what makes the view mutation-safe.
    struct FViewState
    {
        TVector<entt::entity>               Entities;
        size_t                              Cursor = 0;
        TVector<entt::basic_sparse_set<>*>  IncludeStorages;
        TVector<entt::basic_sparse_set<>*>  ExcludeStorages;
    };
}

// The runtime view is consumed here, and only the snapshot and storages live on.
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

    // Additions during iteration are intentionally not visited, and removals are re-validated per chunk.
    for (const entt::entity Entity : View)
    {
        State->Entities.push_back(Entity);
    }
    return State;
}

// Pointers resolve fresh per chunk, so a storage realloc between chunks is harmless.
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

// Returns the world the context is bound to, so the managed side can build a view over it.
LUMINA_DOTNET_EXPORT(uint64, SystemContext_GetWorld)(const FSystemContext* Ctx)
{
    if (Ctx == nullptr)
    {
        return 0;
    }
    CWorld* W = Ctx->GetRegistry().ctx().get<CWorld*>();
    return reinterpret_cast<uint64>(W);
}
