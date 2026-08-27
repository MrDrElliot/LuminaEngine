#include "LuminaNetworkRuntime.h"
#include "World/ECS/Registry.h"

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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Collected first, since destroying while iterating the view it came from is the hazard.
        TVector<ECS::FEntity> ServerOnly;
        for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
        {
            if (!Registry.Get<SNetworkComponent>(Entity).bNetLoadOnClient)
            {
                ServerOnly.push_back(Entity);
            }
        }

        for (ECS::FEntity Entity : ServerOnly)
        {
            Registry.Destroy(Entity);
        }
    }

    void FLuminaNetworkRuntime::OnEntityAttachmentChanged(CWorld* World, ECS::FEntity Entity)
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (Registry.HasAll<SNetworkComponent>(Entity))
        {
            Registry.EmplaceOrReplace<FNetDirty>(Entity);
        }
    }
}
