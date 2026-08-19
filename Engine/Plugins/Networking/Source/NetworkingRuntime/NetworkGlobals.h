#pragma once

#include "Networking/NetworkTypes.h"
#include "Containers/Vector.h"

// Nothing outside this plugin names any of these: the engine reaches netcode only through
// INetworkRuntime, so none of it is exported.
namespace Lumina
{
    class INetworkTransport;

    // Active networking transport, or null before Network::Initialize / after Network::Shutdown.
    extern INetworkTransport* GNetwork;

    namespace Network
    {
        // Lifecycle, called by the engine. Initialize installs the ENet allocator hook and
        // constructs the backend; Shutdown tears it down.
        void Initialize();
        void Shutdown();

        // Pump the transport once and stash the events produced this frame. Engine-driven.
        void Update();

        // Events serviced during the most recent Update(); valid until the next Update(). Poll this.
        const TVector<FNetworkEvent>& GetFrameEvents();

        // Constructs a fresh backend transport (ENet today). Caller owns it. Used for per-world
        // networking (a listen-server world and a client world each own one), distinct from GNetwork.
        INetworkTransport* CreateTransport();
    }
}
