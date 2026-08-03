#include "LuminaNetworkRuntime.h"

#include "NetworkGlobals.h"
#include "Components/NetworkComponent.h"
#include "Net/NetReplication.h"
#include "World/World.h"
#include "World/WorldContext.h"

namespace Lumina
{
    FLuminaNetworkRuntime& FLuminaNetworkRuntime::Get()
    {
        static FLuminaNetworkRuntime Instance;
        return Instance;
    }

    void FLuminaNetworkRuntime::Initialize()
    {
        Network::Initialize();
    }

    void FLuminaNetworkRuntime::Shutdown()
    {
        Network::Shutdown();
    }

    void FLuminaNetworkRuntime::Update()
    {
        Network::Update();
    }

    void FLuminaNetworkRuntime::OnWorldEntitiesLoaded(CWorld* World)
    {
        if (World == nullptr || World->GetNetMode() != ENetMode::Client)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Collected before destroying: destroying while iterating the view it came from is what the
        // original two-pass shape was avoiding.
        TVector<entt::entity> ServerOnly;
        for (entt::entity Entity : Registry.view<SNetworkComponent>())
        {
            if (!Registry.get<SNetworkComponent>(Entity).bNetLoadOnClient)
            {
                ServerOnly.push_back(Entity);
            }
        }

        for (entt::entity Entity : ServerOnly)
        {
            Registry.destroy(Entity);
        }
    }

    void FLuminaNetworkRuntime::OnEntityAttachmentChanged(CWorld* World, entt::entity Entity)
    {
        if (World == nullptr)
        {
            return;
        }

        const ENetMode Mode = World->GetNetMode();
        if (Mode != ENetMode::ListenServer && Mode != ENetMode::DedicatedServer)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (Registry.all_of<SNetworkComponent>(Entity))
        {
            Registry.emplace_or_replace<FNetDirty>(Entity);
        }
    }
}
