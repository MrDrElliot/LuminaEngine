#pragma once

#include "Networking/INetworkTransport.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // ENet-backed transport. All ENet state lives behind FImpl in the .cpp so no vendored
    // networking header leaks past this translation unit.
    class FENetTransport final : public INetworkTransport
    {
    public:

        FENetTransport();
        ~FENetTransport() override;

        ENetworkBackend GetBackend() const override { return ENetworkBackend::ENet; }

        bool StartServer(const FListenParams& Params) override;
        FConnectionHandle ConnectToServer(const FConnectParams& Params) override;
        bool IsServer() const override;

        void Disconnect(FConnectionHandle Connection, uint32 Reason, bool bForce) override;

        bool Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override;
        void Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override;

        void Service(TVector<FNetworkEvent>& OutEvents) override;

        EConnectionState GetConnectionState(FConnectionHandle Connection) const override;

        uint32 GetReliableBacklogBytes(FConnectionHandle Connection) const override;

        FNetworkStats GetStats() const override;
        void          GetConnectionStats(TVector<FConnectionStats>& OutStats) const override;

    private:

        struct FImpl;
        TUniquePtr<FImpl> Impl;
    };
}
