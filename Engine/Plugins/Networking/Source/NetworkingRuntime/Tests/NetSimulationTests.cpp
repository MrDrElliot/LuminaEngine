#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Net/NetReplication.h"
#include "Net/NetSimulation.h"
#include "Net/NetWorldState.h"
#include "Networking/INetworkTransport.h"
#include "Scripting/ScriptableTest.h"
#include "Simulation/Simulation.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    // Endpoint 0 is the host. The host addresses a client by its endpoint, and a client addresses the host as connection 1.
    struct FLoopNetwork
    {
        TVector<TVector<FNetworkEvent>> Inbox;
    };

    class FLoopbackTransport final : public INetworkTransport
    {
    public:

        FLoopbackTransport(FLoopNetwork& InNetwork, uint32 InEndpoint) : Network(InNetwork), Endpoint(InEndpoint) {}

        ENetworkBackend GetBackend() const override { return ENetworkBackend::ENet; }
        bool StartServer(const FListenParams&) override { return true; }
        FConnectionHandle ConnectToServer(const FConnectParams&) override { return FConnectionHandle{ 1 }; }
        bool IsServer() const override { return Endpoint == 0; }
        void Disconnect(FConnectionHandle, uint32, bool) override {}
        EConnectionState GetConnectionState(FConnectionHandle) const override { return EConnectionState::Connected; }

        bool Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode) override
        {
            const uint32 Target = Endpoint == 0 ? Connection.Value : 0u;
            FNetworkEvent& Event = Network.Inbox[Target].EmplaceGetRef();
            Event.Type       = ENetworkEventType::Data;
            Event.Connection = FConnectionHandle{ Endpoint == 0 ? 1u : Endpoint };
            Event.Channel    = Channel;
            Event.Data.Append(static_cast<const uint8*>(Data), (int32)Size);
            return true;
        }

        void Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override
        {
            for (uint32 Client = 1; Client < (uint32)Network.Inbox.Num(); ++Client)
            {
                Send(FConnectionHandle{ Client }, Data, Size, Channel, Mode);
            }
        }

        void Service(TVector<FNetworkEvent>& OutEvents) override
        {
            OutEvents.Append(Network.Inbox[Endpoint]);
            Network.Inbox[Endpoint].Reset();
        }

    private:

        FLoopNetwork& Network;
        uint32        Endpoint;
    };

    struct FEndpoint
    {
        CWorld*                  World = nullptr;
        TStrongObjectPtr<CWorld> Owner;
        FWorldContext            Context;
        FNetWorldState           State;
        CSimTestSystem*          System = nullptr;
        bool                     bServer = false;
    };

    SSimTestInput ScriptedInput(uint32 Player, int64 Tick)
    {
        SSimTestInput Input;
        Input.MoveX = (int32)((Tick / (5 + Player)) % 3) - 1;
        Input.bFire = Tick % (11 + Player) == 0;
        return Input;
    }
}

// A listen-server host and two clients run one session through the real simulation messages, with every client message relayed by the host.
TEST(NetSimulation, HostRelaysASessionToEveryClient)
{
    RegisterPendingTypes();
    FLoopNetwork Network;
    Network.Inbox.SetNum(3);

    TVector<TUniquePtr<FEndpoint>> Endpoints;
    CEntitySystemTest::bAllowCreation = true;
    for (uint32 Index = 0; Index < 3; ++Index)
    {
        FEndpoint& Endpoint = *Endpoints.EmplaceGetRef(MakeUnique<FEndpoint>());
        Endpoint.bServer = Index == 0;
        Endpoint.World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        Endpoint.Owner = Endpoint.World;
        Endpoint.Context.NetMode = Endpoint.bServer ? ENetMode::ListenServer : ENetMode::Client;
        FWorldTestAccess::SetOwningContext(*Endpoint.World, &Endpoint.Context);
        FWorldTestAccess::KeepOnlySystems(*Endpoint.World, { FName("CSimTestSystem") });
        for (const TStrongObjectPtr<CEntitySystem>& System : Endpoint.World->Systems)
        {
            if (CSimTestSystem* Found = Cast<CSimTestSystem>(System.Get()))
            {
                Endpoint.System = Found;
            }
        }
        FWorldTestAccess::EnableSimulation(*Endpoint.World);
        Endpoint.State.Transport.reset(new FLoopbackTransport(Network, Index));
        Endpoint.State.ServerConnection = FConnectionHandle{ 1 };
    }
    CEntitySystemTest::bAllowCreation = false;
    Endpoints[0]->State.ReadyClientIds = { 1u, 2u };

    ASSERT_TRUE(NetSimulation::StartSession(Endpoints[0]->World, Endpoints[0]->State, SSimTestInput::StaticStruct(), 4242));

    for (int32 Frame = 0; Frame < 600; ++Frame)
    {
        for (TUniquePtr<FEndpoint>& Endpoint : Endpoints)
        {
            TVector<FNetworkEvent> Events;
            Endpoint->State.Transport->Service(Events);
            for (const FNetworkEvent& Event : Events)
            {
                Net::ForEachFramedMessage(Event.Data.GetData(), (SIZE_T)Event.Data.Num(), [&](const uint8* Msg, SIZE_T Size)
                {
                    NetSimulation::HandleMessage(Endpoint->World, Endpoint->State, Endpoint->bServer, Event.Connection.Value, static_cast<ENetMessage>(Msg[0]), Msg, Size);
                });
            }

            FSimulation* Sim = Endpoint->World->GetSimulation();
            if (Sim->IsRunning())
            {
                Sim->SubmitLocalInput(ScriptedInput(Sim->GetLocalPlayer(), (int64)Sim->GetCurrentTick() + 2));
                Sim->Advance(1.0 / 60.0);
            }
            NetSimulation::Pump(Endpoint->World, Endpoint->State, Endpoint->bServer);
        }
    }

    for (uint32 Index = 0; Index < 3; ++Index)
    {
        FSimulation* Sim = Endpoints[Index]->World->GetSimulation();
        ASSERT_TRUE(Sim->IsRunning()) << "endpoint " << Index;
        EXPECT_EQ(Sim->GetNumPlayers(), 3u);
        EXPECT_EQ(Sim->GetLocalPlayer(), Index) << "the host plays slot 0 and clients follow in join order";
        EXPECT_FALSE(Sim->HasDesynced()) << "endpoint " << Index << " desynced at tick " << Sim->GetDesyncTick();
        EXPECT_GT(Sim->GetVerifiedChecksums(), 100u) << "endpoint " << Index;
        EXPECT_GT(Sim->GetConfirmedTick(), 500);
        Sim->Stop();
    }

    for (TUniquePtr<FEndpoint>& Endpoint : Endpoints)
    {
        EntitySystems::DestroyAll(SSystemContext(), Endpoint->World->Systems);
        FWorldTestAccess::SetOwningContext(*Endpoint->World, nullptr);
    }
}
