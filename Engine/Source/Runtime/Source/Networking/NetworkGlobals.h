#pragma once

#include "ModuleAPI.h"
#include "NetworkTypes.h"
#include "Containers/Array.h"

namespace Lumina
{
    class INetworkTransport;

    // Active networking transport, or null before Network::Initialize / after Network::Shutdown.
    RUNTIME_API extern INetworkTransport* GNetwork;

    namespace Network
    {
        // Lifecycle, called by the engine. Initialize installs the ENet allocator hook and
        // constructs the backend; Shutdown tears it down.
        void Initialize();
        void Shutdown();

        // Pump the transport once and stash the events produced this frame. Engine-driven.
        void Update();

        // Events serviced during the most recent Update(); valid until the next Update(). Poll this.
        RUNTIME_API const TVector<FNetworkEvent>& GetFrameEvents();

        // Constructs a fresh backend transport (ENet today). Caller owns it. Used for per-world
        // networking (a listen-server world and a client world each own one), distinct from GNetwork.
        RUNTIME_API INetworkTransport* CreateTransport();
    }
}
