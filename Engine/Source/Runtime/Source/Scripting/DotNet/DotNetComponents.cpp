#include "RuntimePCH.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/Component.h"

namespace
{
    Lumina::ECS::FRegistry* LmRegistryFromWorld(uint64 World)
    {
        Lumina::CWorld* W = reinterpret_cast<Lumina::CWorld*>(World);
        return W ? &W->GetEntityRegistry() : nullptr;
    }
}

// Resolves a reflected component type name to its op-table token (the C# side caches it per type).
LUMINA_DOTNET_EXPORT(const void*, FindComponentOps)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return Lumina::FindComponentOps(Lumina::FStringView(Name, static_cast<size_t>(Len)));
}

namespace
{
    // Inline down to the sparse probe, since this is the path every C# get takes.
    FORCEINLINE const Lumina::ECS::FSparseSet* LmFindPool(uint64 World, const void* Ops)
    {
        Lumina::CWorld* W = reinterpret_cast<Lumina::CWorld*>(World);
        const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
        return (W && O) ? W->GetEntityRegistry().FindStorage(O->GetTypeID()) : nullptr;
    }
}

LUMINA_DOTNET_EXPORT(void*, GetComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    const Lumina::ECS::FSparseSet* Pool = LmFindPool(World, Ops);
    return Pool ? Pool->GetRaw(static_cast<Lumina::ECS::FEntity>(Entity)) : nullptr;
}

// Also hands back the pool's layout epoch, so the view can revalidate itself without crossing again.
LUMINA_DOTNET_EXPORT(void*, GetComponentTracked)(uint64 World, uint32 Entity, const void* Ops, const uint32** OutEpoch)
{
    *OutEpoch = nullptr;
    const Lumina::ECS::FSparseSet* Pool = LmFindPool(World, Ops);
    void* Component = Pool ? Pool->GetRaw(static_cast<Lumina::ECS::FEntity>(Entity)) : nullptr;
    if (Component != nullptr)
    {
        *OutEpoch = Pool->GetLayoutEpoch();
    }
    return Component;
}

// A reflected function hands back a bare component pointer, so this finds its owner for the view to track.
LUMINA_DOTNET_EXPORT(int, TrackComponentPointer)(const void* Payload, const void* Ops, uint64* OutWorld, uint32* OutEntity, const uint32** OutEpoch)
{
    *OutWorld = 0;
    *OutEntity = 0;
    *OutEpoch = nullptr;
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (Payload == nullptr || O == nullptr || Lumina::GWorldManager == nullptr)
    {
        return 0;
    }

    int Found = 0;
    Lumina::GWorldManager->ForEachWorld([&](Lumina::CWorld& World)
    {
        if (Found != 0)
        {
            return;
        }

        const Lumina::ECS::FSparseSet* Pool = World.GetEntityRegistry().FindStorage(static_cast<uint32>(O->TypeId));
        const Lumina::ECS::FEntity Owner = Pool != nullptr ? Pool->FindPayloadOwner(Payload) : Lumina::ECS::NullEntity;
        if (!Owner.IsNull())
        {
            *OutWorld = reinterpret_cast<uint64>(&World);
            *OutEntity = Owner.GetPacked();
            *OutEpoch = Pool->GetLayoutEpoch();
            Found = 1;
        }
    });
    return Found;
}

LUMINA_DOTNET_EXPORT(int, HasComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    const Lumina::ECS::FSparseSet* Pool = LmFindPool(World, Ops);
    return Pool && Pool->Contains(static_cast<Lumina::ECS::FEntity>(Entity)) ? 1 : 0;
}

// Get-or-emplace a default component, returning the live pointer to configure in place.
LUMINA_DOTNET_EXPORT(void*, EmplaceComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::ECS::FRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    return (R && O) ? O->Emplace(*R, static_cast<Lumina::ECS::FEntity>(Entity)) : nullptr;
}

LUMINA_DOTNET_EXPORT(int, RemoveComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::ECS::FRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R == nullptr || O == nullptr)
    {
        return 0;
    }

    // A parallel script removes at the sync point, and reports success since the removal is now owed.
    if (Lumina::ECS::FCommandBus::ShouldDefer())
    {
        reinterpret_cast<Lumina::CWorld*>(World)->GetCommandBus().Enqueue([O, Target = static_cast<Lumina::ECS::FEntity>(Entity)](Lumina::ECS::FRegistry& Registry)
        {
            O->Remove(Registry, Target);
        });
        return 1;
    }
    return O->Remove(*R, static_cast<Lumina::ECS::FEntity>(Entity));
}

// Allocates the listener, whose address is the disconnect key, and returns it as an opaque handle.
LUMINA_DOTNET_EXPORT(void*, RegistryConnect)(uint64 World, const void* Ops, int32 Kind)
{
    Lumina::ECS::FRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R == nullptr || O == nullptr)
    {
        return nullptr;
    }

    // Teardown has already released this world's listeners, so one made now would outlive the world untracked.
    if (reinterpret_cast<Lumina::CWorld*>(World)->IsTearingDown())
    {
        return nullptr;
    }

    auto* Listener = new Lumina::FManagedSignalListener();
    O->ConnectSignal(*R, static_cast<Lumina::EComponentSignal>(Kind), Listener);

    // Tracked per world, since a torn down world takes its registry and nothing else would free the listener.
    Lumina::DotNet::TrackSignalListener(reinterpret_cast<Lumina::CWorld*>(World), Listener);
    return Listener;
}

LUMINA_DOTNET_EXPORT(void*, RegistryGetSignalDelegate)(void* Handle)
{
    auto* Listener = static_cast<Lumina::FManagedSignalListener*>(Handle);
    return Listener != nullptr ? static_cast<Lumina::FScriptDelegateBase*>(&Listener->Signal) : nullptr;
}

// Disconnect a listener returned by Connect and free it. Safe with a null handle / torn-down world.
LUMINA_DOTNET_EXPORT(void, RegistryDisconnect)(uint64 World, const void* Ops, int32 Kind, void* Handle)
{
    if (Handle == nullptr)
    {
        return;
    }

    // World teardown frees every listener it tracked before scripts detach, so a dispose from OnDetach lands here after it.
    if (!Lumina::DotNet::ForgetSignalListener(reinterpret_cast<Lumina::CWorld*>(World), Handle))
    {
        return;
    }

    Lumina::ECS::FRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R != nullptr && O != nullptr)
    {
        O->DisconnectSignal(*R, static_cast<Lumina::EComponentSignal>(Kind), static_cast<Lumina::FManagedSignalListener*>(Handle));
    }

    delete static_cast<Lumina::FManagedSignalListener*>(Handle);
}

// Fire on_update<T> for an entity's component (the manual "signal" pulse). No-op for tags or if absent.
LUMINA_DOTNET_EXPORT(void, RegistryPatch)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::ECS::FRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R != nullptr && O != nullptr)
    {
        O->Patch(*R, static_cast<Lumina::ECS::FEntity>(Entity));
    }
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(FindComponentOps),
    LUMINA_DOTNET_SIG(GetComponent),
    LUMINA_DOTNET_SIG(GetComponentTracked),
    LUMINA_DOTNET_SIG(TrackComponentPointer),
    LUMINA_DOTNET_SIG(HasComponent),
    LUMINA_DOTNET_SIG(EmplaceComponent),
    LUMINA_DOTNET_SIG(RemoveComponent),
    LUMINA_DOTNET_SIG(RegistryConnect),
    LUMINA_DOTNET_SIG(RegistryGetSignalDelegate),
    LUMINA_DOTNET_SIG(RegistryDisconnect),
    LUMINA_DOTNET_SIG(RegistryPatch)
);
