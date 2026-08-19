
// enet pulls <winsock2.h>; include it before anything that could drag in <windows.h>/<winsock.h>.
#include <enet/enet.h>
#include "ENetTransport.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"

namespace Lumina
{
    // Network condition simulation (PIE/testing). Global; every per-world transport reads them, so one
    // setting degrades the whole local session. Driven from the editor's Play settings popup.
    static TConsoleVar<int32> CVarSimLatencyMs("Net.Sim.LatencyMs", 0,
        "Simulated one-way send latency in milliseconds (0 = off). Applied to outgoing packets.");
    static TConsoleVar<float> CVarSimPacketLossPct("Net.Sim.PacketLossPct", 0.0f,
        "Simulated outgoing packet loss, 0..100%. Only unreliable packets are dropped (reliable ones retransmit).");

    // ENet's built-in adaptive range coder: compresses every outgoing datagram, decompresses incoming, host
    // -wide and transparent. Both peers must enable it (they do -- same engine). Read once at host creation.
    static TConsoleVar<bool> CVarCompression("Net.Compression", true,
        "Compress packets with ENet's range coder (good ratio on game traffic, low overhead). Both peers must match.");

    static void EnablePacketCompression(ENetHost* Host)
    {
        if (Host == nullptr || !CVarCompression.GetValue())
        {
            return;
        }
        if (enet_host_compress_with_range_coder(Host) != 0)
        {
            LOG_WARN("Network: failed to install ENet range-coder compression");
        }
    }

    // Cheap xorshift for drop decisions; deterministic seed is fine for a debug tool.
    static float SimRand01()
    {
        static uint32 State = 0x9E3779B9u;
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return static_cast<float>(State & 0xFFFFFFu) / static_cast<float>(0x1000000);
    }

    static enet_uint32 ToPacketFlags(ESendMode Mode)
    {
        switch (Mode)
        {
        case ESendMode::Reliable:            return ENET_PACKET_FLAG_RELIABLE;
        case ESendMode::Unreliable:          return ENET_PACKET_FLAG_UNSEQUENCED;
        case ESendMode::UnreliableSequenced: return 0; // default sequenced, unreliable
        }
        return ENET_PACKET_FLAG_RELIABLE;
    }

    static EConnectionState ToConnectionState(ENetPeerState State)
    {
        switch (State)
        {
        case ENET_PEER_STATE_DISCONNECTED: return EConnectionState::Disconnected;
        case ENET_PEER_STATE_CONNECTED:    return EConnectionState::Connected;
        default:                           return EConnectionState::Connecting;
        }
    }

    struct FENetTransport::FImpl
    {
        ENetHost*                      Host       = nullptr;
        bool                           bIsServer  = false;
        uint32                         NextHandle = 1;
        THashMap<uint32, ENetPeer*>    Peers;

        uint32 AllocHandle()
        {
            uint32 Id = NextHandle++;
            if (NextHandle == 0) // wrapped; 0 is reserved for invalid
            {
                NextHandle = 1;
            }
            return Id;
        }

        ENetPeer* FindPeer(FConnectionHandle Handle) const
        {
            auto It = Peers.find(Handle.Value);
            return It != Peers.end() ? It->second : nullptr;
        }

        // Outgoing packet held for latency simulation. Peer == 0 means broadcast. Resolved at flush time
        // (the peer may have gone away while the packet sat in the queue).
        struct FDelayedPacket
        {
            uint32         Peer = 0;
            uint8          Channel = 0;
            ESendMode      Mode = ESendMode::Reliable;
            uint32         ReleaseTime = 0; // enet_time_get() ms
            TVector<uint8> Data;
        };
        TVector<FDelayedPacket> Delayed;

        void SendImmediate(uint32 PeerHandle, uint8 Channel, ESendMode Mode, const void* Data, SIZE_T Size)
        {
            ENetPacket* Packet = enet_packet_create(Data, Size, ToPacketFlags(Mode));
            if (Packet == nullptr)
            {
                return;
            }
            if (PeerHandle == 0)
            {
                enet_host_broadcast(Host, Channel, Packet);
                return;
            }
            ENetPeer* Peer = FindPeer(FConnectionHandle{ PeerHandle });
            if (Peer == nullptr || enet_peer_send(Peer, Channel, Packet) < 0)
            {
                enet_packet_destroy(Packet); // no peer / send failed: ENet did not take ownership
            }
        }

        // Apply simulated loss/latency. Returns true if the packet was consumed here (dropped or queued);
        // false means the caller should send it immediately.
        bool ApplySim(uint32 PeerHandle, uint8 Channel, ESendMode Mode, const void* Data, SIZE_T Size)
        {
            const int32 LatencyMs = CVarSimLatencyMs.GetValue();
            const float LossPct   = CVarSimPacketLossPct.GetValue();
            if (LatencyMs <= 0 && LossPct <= 0.0f)
            {
                return false; // simulation off -> fast path
            }

            // Drop only unreliable traffic; a real dropped reliable packet is retransmitted by ENet, so
            // for reliable the only observable effect of loss is the added latency below.
            if (LossPct > 0.0f && Mode != ESendMode::Reliable && SimRand01() * 100.0f < LossPct)
            {
                return true; // dropped
            }

            if (LatencyMs > 0)
            {
                FDelayedPacket& P = Delayed.emplace_back();
                P.Peer        = PeerHandle;
                P.Channel     = Channel;
                P.Mode        = Mode;
                P.ReleaseTime = enet_time_get() + static_cast<uint32>(LatencyMs);
                P.Data.assign(static_cast<const uint8*>(Data), static_cast<const uint8*>(Data) + Size);
                return true; // queued
            }
            return false; // loss didn't drop it and no latency -> send now
        }

        // Send any delayed packets whose release time has passed. Constant latency keeps the queue ordered
        // by ReleaseTime, so a due prefix is safe to flush and erase.
        void FlushDelayed()
        {
            if (Delayed.empty())
            {
                return;
            }
            const uint32 Now = enet_time_get();
            SIZE_T Count = 0;
            while (Count < Delayed.size() && Delayed[Count].ReleaseTime <= Now)
            {
                FDelayedPacket& P = Delayed[Count];
                SendImmediate(P.Peer, P.Channel, P.Mode, P.Data.data(), static_cast<SIZE_T>(P.Data.size()));
                ++Count;
            }
            if (Count > 0)
            {
                Delayed.erase(Delayed.begin(), Delayed.begin() + Count);
            }
        }
    };

    FENetTransport::FENetTransport()
        : Impl(MakeUnique<FImpl>())
    {
    }

    FENetTransport::~FENetTransport()
    {
        if (Impl->Host)
        {
            // Notify peers before tearing down (e.g. a client whose PIE preview window was closed):
            // disconnect_now queues the packet, flush pushes it out so the other side gets a clean
            // DISCONNECT instead of waiting on a timeout.
            for (const auto& [Id, Peer] : Impl->Peers)
            {
                if (Peer != nullptr)
                {
                    enet_peer_disconnect_now(Peer, 0);
                }
            }
            if (!Impl->Peers.empty())
            {
                enet_host_flush(Impl->Host);
            }

            enet_host_destroy(Impl->Host);
            Impl->Host = nullptr;
        }
    }

    bool FENetTransport::StartServer(const FListenParams& Params)
    {
        if (Impl->Host)
        {
            LOG_ERROR("FENetTransport::StartServer: transport already has an active host");
            return false;
        }

        ENetAddress Address{};
        Address.host = ENET_HOST_ANY;
        Address.port = Params.Port;

        Impl->Host = enet_host_create(&Address, Params.MaxConnections, Params.ChannelCount,
            Params.IncomingBandwidth, Params.OutgoingBandwidth);

        if (Impl->Host == nullptr)
        {
            LOG_ERROR("FENetTransport::StartServer: failed to create host on port {}", Params.Port);
            return false;
        }

        EnablePacketCompression(Impl->Host);

        Impl->bIsServer = true;
        LOG_DISPLAY("Network: ENet server listening on port {} (max {} connections)", Params.Port, Params.MaxConnections);
        return true;
    }

    FConnectionHandle FENetTransport::ConnectToServer(const FConnectParams& Params)
    {
        if (Impl->Host)
        {
            LOG_ERROR("FENetTransport::ConnectToServer: transport already has an active host");
            return FConnectionHandle::Invalid();
        }

        Impl->Host = enet_host_create(nullptr, 1, Params.ChannelCount, 0, 0);
        if (Impl->Host == nullptr)
        {
            LOG_ERROR("FENetTransport::ConnectToServer: failed to create client host");
            return FConnectionHandle::Invalid();
        }
        EnablePacketCompression(Impl->Host);
        Impl->bIsServer = false;

        ENetAddress Address{};
        if (enet_address_set_host(&Address, Params.Address.Host.c_str()) != 0)
        {
            LOG_ERROR("FENetTransport::ConnectToServer: could not resolve host '{}'", Params.Address.Host.c_str());
            enet_host_destroy(Impl->Host);
            Impl->Host = nullptr;
            return FConnectionHandle::Invalid();
        }
        Address.port = Params.Address.Port;

        ENetPeer* Peer = enet_host_connect(Impl->Host, &Address, Params.ChannelCount, 0);
        if (Peer == nullptr)
        {
            LOG_ERROR("FENetTransport::ConnectToServer: no available peers for connection");
            enet_host_destroy(Impl->Host);
            Impl->Host = nullptr;
            return FConnectionHandle::Invalid();
        }

        FConnectionHandle Handle{ Impl->AllocHandle() };
        Peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(Handle.Value));
        Impl->Peers[Handle.Value] = Peer;
        return Handle;
    }

    bool FENetTransport::IsServer() const
    {
        return Impl->bIsServer;
    }

    void FENetTransport::Disconnect(FConnectionHandle Connection, uint32 Reason, bool bForce)
    {
        ENetPeer* Peer = Impl->FindPeer(Connection);
        if (Peer == nullptr)
        {
            return;
        }

        if (bForce)
        {
            enet_peer_disconnect_now(Peer, Reason);
            // disconnect_now emits no event; drop the mapping here.
            Peer->data = nullptr;
            Impl->Peers.erase(Connection.Value);
        }
        else
        {
            enet_peer_disconnect(Peer, Reason); // a DISCONNECT event will finalize the mapping
        }
    }

    bool FENetTransport::Send(FConnectionHandle Connection, const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode)
    {
        if (Impl->Host == nullptr || Impl->FindPeer(Connection) == nullptr)
        {
            return false;
        }

        if (Impl->ApplySim(Connection.Value, Channel, Mode, Data, Size))
        {
            return true; // dropped or queued by the network simulator
        }

        Impl->SendImmediate(Connection.Value, Channel, Mode, Data, Size);
        return true;
    }

    void FENetTransport::Broadcast(const void* Data, SIZE_T Size, uint8 Channel, ESendMode Mode)
    {
        if (Impl->Host == nullptr)
        {
            return;
        }

        if (Impl->ApplySim(0, Channel, Mode, Data, Size))
        {
            return; // dropped or queued by the network simulator
        }

        Impl->SendImmediate(0, Channel, Mode, Data, Size);
    }

    void FENetTransport::Service(TVector<FNetworkEvent>& OutEvents)
    {
        if (Impl->Host == nullptr)
        {
            return;
        }

        Impl->FlushDelayed(); // release any latency-delayed packets that are now due

        ENetEvent Event;
        while (enet_host_service(Impl->Host, &Event, 0) > 0)
        {
            switch (Event.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                {
                    uint32 Id;
                    if (Impl->bIsServer)
                    {
                        Id = Impl->AllocHandle();
                        Event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(Id));
                        Impl->Peers[Id] = Event.peer;
                    }
                    else
                    {
                        // Client: the handle was assigned in ConnectToServer; CONNECT just confirms it.
                        Id = static_cast<uint32>(reinterpret_cast<uintptr_t>(Event.peer->data));
                    }

                    FNetworkEvent& Out = OutEvents.emplace_back();
                    Out.Type       = ENetworkEventType::Connected;
                    Out.Connection = FConnectionHandle{ Id };
                }
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                {
                    const uint32 Id = static_cast<uint32>(reinterpret_cast<uintptr_t>(Event.peer->data));

                    FNetworkEvent& Out = OutEvents.emplace_back();
                    Out.Type       = ENetworkEventType::Data;
                    Out.Connection = FConnectionHandle{ Id };
                    Out.Channel    = Event.channelID;
                    Out.Data.assign(Event.packet->data, Event.packet->data + Event.packet->dataLength);

                    enet_packet_destroy(Event.packet);
                }
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                {
                    const uint32 Id = static_cast<uint32>(reinterpret_cast<uintptr_t>(Event.peer->data));

                    FNetworkEvent& Out = OutEvents.emplace_back();
                    Out.Type       = ENetworkEventType::Disconnected;
                    Out.Connection = FConnectionHandle{ Id };

                    Impl->Peers.erase(Id);
                    Event.peer->data = nullptr;
                }
                break;

            default:
                break;
            }
        }
    }

    EConnectionState FENetTransport::GetConnectionState(FConnectionHandle Connection) const
    {
        ENetPeer* Peer = Impl->FindPeer(Connection);
        if (Peer == nullptr)
        {
            return EConnectionState::Disconnected;
        }
        return ToConnectionState(Peer->state);
    }

    uint32 FENetTransport::GetReliableBacklogBytes(FConnectionHandle Connection) const
    {
        ENetPeer* Peer = Impl->FindPeer(Connection);
        return Peer != nullptr ? static_cast<uint32>(Peer->reliableDataInTransit) : 0;
    }

    FNetworkStats FENetTransport::GetStats() const
    {
        FNetworkStats Stats;
        if (Impl->Host != nullptr)
        {
            Stats.TotalSentBytes       = Impl->Host->totalSentData;
            Stats.TotalReceivedBytes   = Impl->Host->totalReceivedData;
            Stats.TotalSentPackets     = Impl->Host->totalSentPackets;
            Stats.TotalReceivedPackets = Impl->Host->totalReceivedPackets;
            Stats.IncomingBandwidth    = Impl->Host->incomingBandwidth;
            Stats.OutgoingBandwidth    = Impl->Host->outgoingBandwidth;
        }
        return Stats;
    }

    void FENetTransport::GetConnectionStats(TVector<FConnectionStats>& OutStats) const
    {
        OutStats.clear();
        for (const auto& [Id, Peer] : Impl->Peers)
        {
            if (Peer == nullptr)
            {
                continue;
            }
            FConnectionStats& S = OutStats.emplace_back();
            S.ConnectionId    = Id;
            S.State           = ToConnectionState(Peer->state);
            S.RoundTripTimeMs = Peer->roundTripTime;
            S.PacketLoss      = static_cast<float>(Peer->packetLoss) / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE);
            S.SentBytes       = Peer->outgoingDataTotal;
            S.ReceivedBytes   = Peer->incomingDataTotal;
            S.PacketsSent     = Peer->packetsSent;
            S.PacketsLost     = Peer->packetsLost;
        }
    }
}
