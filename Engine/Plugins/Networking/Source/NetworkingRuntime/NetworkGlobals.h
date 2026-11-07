#pragma once

// Nothing outside this plugin names any of these: the engine reaches netcode only through
// INetworkRuntime, so none of it is exported.
namespace Lumina
{
    class INetworkTransport;

    namespace Network
    {
        // Sets up ENet with the engine allocator; Shutdown tears it down.
        void Initialize();
        void Shutdown();

        // Caller owns the result; each networked world and each load bot owns its own transport.
        INetworkTransport* CreateTransport();
    }
}
