#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    // Opaque transport-agnostic connection id. 0 == invalid. The backend maps it to its own peer type.
    struct FConnectionHandle
    {
        uint32 Value = 0;

        constexpr bool IsValid() const { return Value != 0; }

        constexpr bool operator==(const FConnectionHandle& Other) const { return Value == Other.Value; }
        constexpr bool operator!=(const FConnectionHandle& Other) const { return Value != Other.Value; }

        static constexpr FConnectionHandle Invalid() { return FConnectionHandle{}; }
    };

    enum class ENetworkBackend : uint8
    {
        ENet,
        Steam,
    };

    enum class ESendMode : uint8
    {
        Reliable,            // Guaranteed, ordered.
        Unreliable,          // No guarantee, unsequenced.
        UnreliableSequenced, // No guarantee, but late packets are dropped (sequenced).
    };

    // Who a script RPC runs on. Mirrors LuminaSharp.Networking.ERpcTarget.
    enum class ERpcTarget : uint8
    {
        Broadcast = 0, // the host and every client
        Host      = 1, // the host only
        Owner     = 2, // the entity's owning connection only
    };

    // Mirrors LuminaSharp.Networking.ERpcFlags.
    enum ERpcFlags : uint8
    {
        RpcFlag_None       = 0,
        RpcFlag_Unreliable = 1 << 0,
    };

    enum class EConnectionState : uint8
    {
        Disconnected,
        Connecting,
        Connected,
    };

    enum class ENetworkEventType : uint8
    {
        Connected,
        Disconnected,
        Data,
    };

    struct FNetworkAddress
    {
        FString Host;       // Hostname or dotted IPv4; resolved by the backend.
        uint16  Port = 0;
    };

    struct FListenParams
    {
        uint16 Port              = 0;
        uint32 MaxConnections    = 32;
        uint8  ChannelCount      = 2;
        uint32 IncomingBandwidth = 0; // bytes/sec, 0 == unlimited
        uint32 OutgoingBandwidth = 0; // bytes/sec, 0 == unlimited
    };

    struct FConnectParams
    {
        FNetworkAddress Address;
        uint8           ChannelCount = 2;
        uint32          TimeoutMs    = 5000;
    };

    // One serviced network event. Data is populated only for ENetworkEventType::Data.
    struct FNetworkEvent
    {
        ENetworkEventType Type       = ENetworkEventType::Data;
        FConnectionHandle Connection;
        uint8             Channel    = 0;
        TVector<uint8>    Data;
    };

    // Host-level transport counters (cumulative since start), for the network debug tool.
    struct FNetworkStats
    {
        uint64 TotalSentBytes       = 0;
        uint64 TotalReceivedBytes   = 0;
        uint64 TotalSentPackets     = 0;
        uint64 TotalReceivedPackets = 0;
        uint32 IncomingBandwidth    = 0; // bytes/sec, 0 == unlimited
        uint32 OutgoingBandwidth    = 0;
    };

    // Per-connection (peer) stats.
    struct FConnectionStats
    {
        uint32           ConnectionId      = 0;
        EConnectionState State             = EConnectionState::Disconnected;
        uint32           RoundTripTimeMs   = 0;
        float            PacketLoss        = 0.0f; // 0..1
        uint64           SentBytes         = 0;
        uint64           ReceivedBytes     = 0;
        uint32           PacketsSent       = 0;
        uint32           PacketsLost       = 0;
    };
}
