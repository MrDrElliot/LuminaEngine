#pragma once

#include "Containers/StaticArray.h"
#include "Memory/SmartPtr.h"
#include "Networking/INetworkTransport.h"

namespace Lumina
{
    // Forwards to the real transport and tallies outgoing bytes by message type, for the -netstats breakdown.
    class FNetCountingTransport final : public INetworkTransport
    {
    public:

        static constexpr uint32 TypeCount = 16;

        explicit FNetCountingTransport(TUniquePtr<INetworkTransport> InInner)
            : Inner(Move(InInner))
        {}

        ENetworkBackend GetBackend() const override { return Inner->GetBackend(); }
        bool StartServer(const FListenParams& Params) override { return Inner->StartServer(Params); }
        FConnectionHandle ConnectToServer(const FConnectParams& Params) override { return Inner->ConnectToServer(Params); }
        bool IsServer() const override { return Inner->IsServer(); }
        void Disconnect(FConnectionHandle Connection, uint32 Reason, bool bForce) override { Inner->Disconnect(Connection, Reason, bForce); }
        void Service(TVector<FNetworkEvent>& OutEvents) override { Inner->Service(OutEvents); }
        void Flush() override { Inner->Flush(); }
        EConnectionState GetConnectionState(FConnectionHandle Connection) const override { return Inner->GetConnectionState(Connection); }
        uint32 GetReliableBacklogBytes(FConnectionHandle Connection) const override { return Inner->GetReliableBacklogBytes(Connection); }
        FNetworkStats GetStats() const override { return Inner->GetStats(); }
        void GetConnectionStats(TVector<FConnectionStats>& OutStats) const override { Inner->GetConnectionStats(OutStats); }

        bool Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override
        {
            Tally(static_cast<const uint8*>(Data), Size, 1);
            return Inner->Send(Connection, Data, Size, Channel, Mode);
        }

        void Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode) override
        {
            ConnectionScratch.clear();
            Inner->GetConnectionStats(ConnectionScratch);
            Tally(static_cast<const uint8*>(Data), Size, static_cast<uint64>(ConnectionScratch.size()));
            Inner->Broadcast(Data, Size, Channel, Mode);
        }

        // Bytes per type since the last call, which also starts a new window.
        TArray<uint64, TypeCount> TakeBytesByType()
        {
            TArray<uint64, TypeCount> Result = BytesByType;
            BytesByType.fill(0);
            return Result;
        }

    private:

        // Every payload is a run of framed messages, a 16-bit length then a type byte.
        void Tally(const uint8* Data, SIZE_T Size, uint64 Copies)
        {
            SIZE_T Offset = 0;
            while (Offset + 3 <= Size)
            {
                const SIZE_T Length = static_cast<SIZE_T>(Data[Offset]) | (static_cast<SIZE_T>(Data[Offset + 1]) << 8);
                const uint8 Type = Data[Offset + 2];
                BytesByType[Type < TypeCount ? Type : 0] += (Length + 2) * Copies;
                Offset += Length + 2;
            }
        }

        TUniquePtr<INetworkTransport>  Inner;
        TArray<uint64, TypeCount>      BytesByType{};
        TVector<FConnectionStats>      ConnectionScratch;
    };
}
