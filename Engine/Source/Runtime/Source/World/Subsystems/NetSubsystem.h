#pragma once

#include "Containers/Vector.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Networking/NetworkEvents.h"
#include "World/ECS/Entity.h"
#include "World/ECS/EventQueue.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "NetSubsystem.generated.h"

namespace Lumina
{
    // Networked entities, ownership and connections, and the session events C++ and C# subscribe to.
    REFLECT()
    class RUNTIME_API CNetSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        //~ Session events, broadcast at the start of the frame after the session changed, outside any system and even while paused.

        // Host. A client finished joining, with its connection id.
        PROPERTY(Hidden)
        TScriptDelegate<uint32> OnClientJoined;

        // Host. A client left, told before anything it owned changes hands.
        PROPERTY(Hidden)
        TScriptDelegate<uint32> OnClientLeft;

        // Client. This peer finished joining its host.
        PROPERTY(Hidden)
        FScriptDelegate OnJoinedHost;

        // Client. The session ended, or never came up, for the reason given.
        PROPERTY(Hidden)
        TScriptDelegate<ENetLeaveReason> OnLeftHost;

        // Called by the world at frame start, before the pause check, so a join while paused is still told.
        void DispatchSessionEvents();

        // True on the host, a dedicated server and a standalone world, the peers whose word is final.
        FUNCTION()
        bool IsHost();

        // True when this peer controls Entity, which a standalone world and an unnetworked entity always do.
        FUNCTION()
        bool IsOwner(ECS::FEntity Entity);

        // Another peer controls Entity, so this one only mirrors it.
        FUNCTION()
        bool IsProxy(ECS::FEntity Entity);

        // The connection the RPC or Sync write being applied came from, or this peer's own when none is.
        FUNCTION()
        uint32 GetRpcCaller();

        // This peer's own connection, which is 0 on a host and in a standalone world.
        FUNCTION()
        uint32 GetLocalConnection();

        // The remote connections that finished joining, which only a host can see.
        FUNCTION()
        void GetConnections(TVector<uint32>& OutConnections);

        // Needs an SNetworkComponent on the entity.
        FUNCTION()
        bool IsEntityNetworked(ECS::FEntity Entity);

        FUNCTION()
        uint32 GetOwner(ECS::FEntity Entity);

        // Host only, a client asking is refused.
        FUNCTION()
        bool SetOwner(ECS::FEntity Entity, uint32 ConnectionId);

        // Resends the entity's replicated component fields.
        FUNCTION()
        void MarkEntityDirty(ECS::FEntity Entity);

        // Host only. Stops replicating the entity and its children, and every client keeps simulating its own copy instead of despawning it.
        FUNCTION()
        void TearOff(ECS::FEntity Entity);

        // Raised where Target says as an event of T, which a system reads with a TEventReader<T> and finds Sender filled in. Connection names the client for Target Client.
        template<typename T>
        bool Send(ENetMessageTarget Target, const T& Message, uint32 Connection = 0)
        {
            static_assert(std::is_base_of_v<SNetMessage, T>, "A net message derives from SNetMessage.");
            return SendMessageOfType(Target, Connection, ECS::GetComponentTypeID<T>(), &Message);
        }

        bool SendMessageOfType(ENetMessageTarget Target, uint32 Connection, ECS::FComponentTypeID TypeID, const void* Message);

        // The C# side of Send, naming its event by queue id.
        FUNCTION(ScriptInternal)
        bool SendScriptMessage(uint32 Target, uint32 Connection, uint32 TypeId, void* Message);

        // Where a C# message keeps its Sender and the entity ids it carries, as byte offsets.
        FUNCTION(ScriptInternal)
        static void RegisterScriptMessage(uint32 TypeId, uint32 SenderOffset, void* EntityOffsets, uint32 Count);

        FUNCTION()
        uint32 EntityToNetId(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity NetIdToEntity(uint32 NetId);

        // A host is always joined, and a client is once its handshake finished.
        FUNCTION()
        bool IsJoined();

        // The entity a connection owns, preferring one with a character controller.
        FUNCTION()
        ECS::FEntity GetOwnedPawn(uint32 ConnectionId);

        // Client, StandIn shows a spawn before the host makes it. Send the key with the request, 0 means a host or standalone world, which spawns for real.
        FUNCTION()
        uint32 PredictSpawn(ECS::FEntity StandIn, bool bKeepStandIn = true);

        // Host, call the frame Entity spawns, with the connection and key the request carried.
        FUNCTION()
        void ConfirmPredictedSpawn(ECS::FEntity Entity, uint32 ConnectionId, uint32 Key);

    private:

        ECS::TEventReader<SNetSessionEvent> SessionEvents;
    };
}
