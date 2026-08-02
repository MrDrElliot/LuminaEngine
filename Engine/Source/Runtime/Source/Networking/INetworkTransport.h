#pragma once

#include "ModuleAPI.h"
#include "NetworkTypes.h"

namespace Lumina
{
    // Backend-agnostic networking transport.
    class RUNTIME_API INetworkTransport
    {
    public:

        virtual ~INetworkTransport() = default;

        virtual ENetworkBackend GetBackend() const = 0;

        // Bind + listen. Returns false on failure (port in use, socket error).
        virtual bool StartServer(const FListenParams& Params) = 0;

        // Begin connecting to a server. Returns the handle for the (still-connecting) link, or an
        // invalid handle on immediate failure. A Connected/Disconnected event follows from Service().
        virtual FConnectionHandle ConnectToServer(const FConnectParams& Params) = 0;

        virtual bool IsServer() const = 0;

        // Schedule a disconnect. bForce drops the peer immediately without the disconnect handshake.
        virtual void Disconnect(FConnectionHandle Connection, uint32 Reason = 0, bool bForce = false) = 0;

        // Queue a packet to one connection. Returns false if the handle is unknown.
        virtual bool Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) = 0;

        // Queue a packet to every connected peer.
        virtual void Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) = 0;

        // Pump the backend and append any events produced this call to OutEvents. Non-blocking.
        // Called once per frame by Network::Update.
        virtual void Service(TVector<FNetworkEvent>& OutEvents) = 0;

        virtual EConnectionState GetConnectionState(FConnectionHandle Connection) const = 0;

        // Bytes of reliable data sent to this connection but not yet acknowledged (in flight). Used as a
        // backpressure signal for property replication. Default 0 for backends that don't expose it.
        virtual uint32 GetReliableBacklogBytes(FConnectionHandle Connection) const { return 0; }

        //~ Debug/telemetry (optional; default empty for backends that don't track it).
        virtual FNetworkStats GetStats() const { return {}; }
        virtual void          GetConnectionStats(TVector<FConnectionStats>& OutStats) const {}
    };
}
