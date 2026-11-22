#pragma once

#include "Networking/INetworkTransport.h"
#include "Memory/SmartPtr.h"
#include "Containers/FunctionRef.h"

namespace Lumina
{
    namespace NetSim
    {
        // Whether simulated loss takes a received datagram, decided from its bytes and Salt so no generator state is needed.
        NETWORKINGRUNTIME_API bool DropsDatagram(const uint8* Data, SIZE_T Size, uint32 Salt, float LossPct);
    }

    // The network simulator's latency queue; times are in ms and a peer of 0 means broadcast.
    class NETWORKINGRUNTIME_API FNetSimQueue
    {
    public:

        struct FPacket
        {
            uint32         Peer = 0;
            uint8          Channel = 0;
            ESendMode      Mode = ESendMode::Reliable;
            uint32         ReleaseTime = 0;
            TVector<uint8> Data;
        };

        // Returns true when the packet was queued here, and false to send it immediately.
        bool Submit(uint32 Peer, uint8 Channel, ESendMode Mode, const void* Data, SIZE_T Size, uint32 NowMs, int32 LatencyMs);

        void Flush(uint32 NowMs, TFunctionRef<void(const FPacket&)> Send);

        bool IsEmpty() const { return Delayed.IsEmpty(); }

    private:

        TVector<FPacket> Delayed;
    };

    // ENet-backed transport, with all ENet state behind FImpl so no vendored header leaks past the .cpp.
    class FENetTransport final : public INetworkTransport
    {
    public:

        FENetTransport();
        ~FENetTransport() override;
        FENetTransport(const FENetTransport&) = delete;
        FENetTransport& operator=(const FENetTransport&) = delete;
        FENetTransport(FENetTransport&&) = delete;
        FENetTransport& operator=(FENetTransport&&) = delete;

        ENetworkBackend GetBackend() const override { return ENetworkBackend::ENet; }

        bool StartServer(const FListenParams& Params) override;
        FConnectionHandle ConnectToServer(const FConnectParams& Params) override;
        bool IsServer() const override;

        void Disconnect(FConnectionHandle Connection, uint32 Reason, bool bForce) override;

        bool Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override;
        void Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override;

        void Service(TVector<FNetworkEvent>& OutEvents) override;
        void Flush() override;

        EConnectionState GetConnectionState(FConnectionHandle Connection) const override;

        uint32 GetReliableBacklogBytes(FConnectionHandle Connection) const override;

        FNetworkStats GetStats() const override;
        void          GetConnectionStats(TVector<FConnectionStats>& OutStats) const override;

    private:

        struct FImpl;
        TUniquePtr<FImpl> Impl;
    };
}
