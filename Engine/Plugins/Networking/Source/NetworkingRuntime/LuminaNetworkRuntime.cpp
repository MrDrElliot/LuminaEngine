#include "LuminaNetworkRuntime.h"
#include "World/ECS/Registry.h"

#include "NetworkGlobals.h"
#include "Components/NetworkComponent.h"
#include "Net/NetReplication.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "Net/NetWorldState.h"
#include "Net/NetLoadBots.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "Networking/INetworkTransport.h"

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
        FNetLoadBotSwarm::StartFromCommandLine();
    }

    void FLuminaNetworkRuntime::Shutdown()
    {
        FNetLoadBotSwarm::Stop();
        Network::Shutdown();
    }

    void FLuminaNetworkRuntime::Update()
    {
        Network::Update();
        FNetLoadBotSwarm::Tick();
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

    namespace
    {
        bool IsServerWorld(const CWorld* World)
        {
            const ENetMode Mode = World->GetNetMode();
            return Mode == ENetMode::ListenServer || Mode == ENetMode::DedicatedServer;
        }

        ECS::FRegistry& RegistryOf(const CWorld* World)
        {
            return ECS::GetWorldRegistry(*const_cast<CWorld*>(World));
        }

        FNetWorldState* StateOf(const CWorld* World)
        {
            if (World == nullptr || World->GetNetMode() == ENetMode::Standalone)
            {
                return nullptr;
            }
            return RegistryOf(World).Ctx().Find<FNetWorldState>();
        }

        const SNetworkComponent* NetOf(const CWorld* World, ECS::FEntity Entity)
        {
            ECS::FRegistry& Registry = RegistryOf(World);
            return Registry.IsValid(Entity) ? Registry.TryGet<SNetworkComponent>(Entity) : nullptr;
        }
    }

    int32 FLuminaNetworkRuntime::GetConnectedClientCount(const CWorld* World) const
    {
        const FNetWorldState* State = StateOf(World);
        return State != nullptr ? (int32)State->ReadyClientIds.size() : 0;
    }

    bool FLuminaNetworkRuntime::TakeClientConnection(CWorld* OldWorld, TUniquePtr<INetworkTransport>& OutTransport,
        FConnectionHandle& OutConnection, uint32& OutLocalPeerId)
    {
        FNetWorldState* State = StateOf(OldWorld);
        if (State == nullptr || OldWorld->GetNetMode() != ENetMode::Client || !State->bClientConnected || State->Transport == nullptr)
        {
            return false;
        }
        OutTransport   = Move(State->Transport);
        OutConnection  = State->ServerConnection;
        OutLocalPeerId = State->LocalPeerId;
        State->bClientConnected = false;
        return true;
    }

    bool FLuminaNetworkRuntime::IsJoined(const CWorld* World) const
    {
        const FNetWorldState* State = StateOf(World);
        return State != nullptr && !IsServerWorld(World) && State->bJoinDispatched && State->bClientConnected;
    }

    ECS::FEntity FLuminaNetworkRuntime::FindOwnedPawn(const CWorld* World, uint32 ConnectionId) const
    {
        if (World == nullptr || StateOf(World) == nullptr)
        {
            return ECS::NullEntity;
        }
        ECS::FRegistry& Registry = RegistryOf(World);
        ECS::FEntity Fallback = ECS::NullEntity;
        for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
        {
            if (Registry.Get<SNetworkComponent>(Entity).OwningConnectionId != ConnectionId)
            {
                continue;
            }
            if (Registry.HasAll<SCharacterControllerComponent>(Entity))
            {
                return Entity;
            }
            if (Fallback == ECS::NullEntity)
            {
                Fallback = Entity;
            }
        }
        return Fallback;
    }

    void FLuminaNetworkRuntime::BindEntityIds(const CWorld* World, FNetArchive& Ar) const
    {
        if (FNetWorldState* State = const_cast<FNetWorldState*>(StateOf(World)))
        {
            Net::BindEntityIds(Ar, *State);
        }
    }

    uint32 FLuminaNetworkRuntime::GetLocalConnectionId(const CWorld* World) const
    {
        const FNetWorldState* State = StateOf(World);
        return (State != nullptr && !IsServerWorld(World)) ? State->LocalPeerId : 0u;
    }

    void FLuminaNetworkRuntime::GetConnectionIds(const CWorld* World, TVector<uint32>& OutIds) const
    {
        OutIds.clear();
        if (const FNetWorldState* State = StateOf(World))
        {
            OutIds = State->ReadyClientIds;
        }
    }

    bool FLuminaNetworkRuntime::IsEntityNetworked(const CWorld* World, ECS::FEntity Entity) const
    {
        const SNetworkComponent* Net = World != nullptr ? NetOf(World, Entity) : nullptr;
        // Not keyed on the id, since a freshly spawned entity runs its scripts before the host assigns one.
        return Net != nullptr && Net->bReplicates && Net->bNetLoadOnClient;
    }

    uint32 FLuminaNetworkRuntime::GetEntityOwner(const CWorld* World, ECS::FEntity Entity) const
    {
        const SNetworkComponent* Net = World != nullptr ? NetOf(World, Entity) : nullptr;
        return Net != nullptr ? Net->OwningConnectionId : 0u;
    }

    bool FLuminaNetworkRuntime::SetEntityOwner(CWorld* World, ECS::FEntity Entity, uint32 ConnectionId)
    {
        FNetWorldState* State = StateOf(World);
        if (State == nullptr || !IsServerWorld(World))
        {
            return false;
        }
        ECS::FRegistry& Registry = RegistryOf(World);
        if (!Registry.IsValid(Entity))
        {
            return false;
        }
        SNetworkComponent& Net = Registry.GetOrEmplace<SNetworkComponent>(Entity);
        if (Net.OwningConnectionId != ConnectionId)
        {
            Net.OwningConnectionId = ConnectionId;
            Net.RemoteRole = ConnectionId != 0 ? ENetRole::AutonomousProxy : ENetRole::SimulatedProxy;
            State->OwnershipChanged.push_back(Entity);
        }
        return true;
    }

    void FLuminaNetworkRuntime::MarkEntityDirty(CWorld* World, ECS::FEntity Entity)
    {
        if (World == nullptr || !IsServerWorld(World))
        {
            return;
        }
        ECS::FRegistry& Registry = RegistryOf(World);
        if (Registry.IsValid(Entity) && Registry.HasAll<SNetworkComponent>(Entity) && !Registry.HasAll<FNetDirty>(Entity))
        {
            Registry.Emplace<FNetDirty>(Entity);
        }
    }

    uint32 FLuminaNetworkRuntime::EntityToNetId(const CWorld* World, ECS::FEntity Entity) const
    {
        const SNetworkComponent* Net = World != nullptr ? NetOf(World, Entity) : nullptr;
        return Net != nullptr ? Net->NetGUID.Value : 0u;
    }

    ECS::FEntity FLuminaNetworkRuntime::NetIdToEntity(const CWorld* World, uint32 NetId) const
    {
        const FNetWorldState* State = StateOf(World);
        return (State != nullptr && NetId != 0) ? State->GuidTable.Find(FNetGUID{ NetId }) : ECS::NullEntity;
    }

    bool FLuminaNetworkRuntime::SendScriptRpc(CEntityScript* Script, uint32 RpcId, ERpcTarget Target, uint8 Flags,
        const uint8* Payload, uint32 PayloadSize)
    {
        CWorld* World = Script->GetWorld();
        FNetWorldState* State = StateOf(World);
        if (State == nullptr || State->Transport == nullptr || !Script->IsAttached() || PayloadSize > Net::MaxFramedMessageSize / 2)
        {
            return false;
        }

        ECS::FRegistry& Registry = RegistryOf(World);
        const ECS::FEntity Entity = Script->GetOwningEntity();
        const SNetworkComponent* Net = NetOf(World, Entity);
        const int32 ScriptIndex = Net::FindScriptIndex(Registry, Entity, Script);
        if (Net == nullptr || Net->NetGUID.Value == 0 || ScriptIndex == Constants::kIndexNone)
        {
            return false;
        }

        const bool bReliable = (Flags & static_cast<uint8>(ENetFlags::Unreliable)) == 0;
        auto Queue = [&](uint32 Connection)
        {
            TVector<uint8>& Batch = bReliable ? State->PendingRpcReliable[Connection] : State->PendingRpcUnreliable[Connection];
            Net::AppendScriptRpc(Batch, Net->NetGUID.Value, (uint32)ScriptIndex, RpcId, Target, Flags, State->LocalPeerId, Payload, PayloadSize);
        };

        if (!IsServerWorld(World))
        {
            if (!State->bClientConnected)
            {
                return false;
            }
            Queue(State->ServerConnection.Value);
            return true;
        }

        switch (Target)
        {
        case ERpcTarget::Broadcast:
            for (uint32 Connection : State->ReadyClientIds)
            {
                if (State->HoldsEntity(Connection, Net->NetGUID.Value))
                {
                    Queue(Connection);
                }
            }
            return true;
        case ERpcTarget::Owner:
            if (Net->OwningConnectionId == 0)
            {
                return false;
            }
            Queue(Net->OwningConnectionId);
            return true;
        case ERpcTarget::Host:
            return false;
        }
        return false;
    }
}
