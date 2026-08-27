#pragma once

#include "Containers/String.h"
#include "Core/Http/HttpMessage.h"
#include "Core/Http/HttpServer.h"
#include "Core/JsonRpc/JsonRpc.h"

namespace Lumina::MCP
{
    struct FServerSettings
    {
        uint16 Port = 8787;

        // Every Model Context Protocol message arrives by POST to this path.
        FString Endpoint = "/mcp";
    };

    // Carries Model Context Protocol traffic over loopback HTTP and hands it to a JSON-RPC dispatcher.
    class FServer
    {
    public:

        FServer() = default;
        ~FServer();

        FServer(const FServer&) = delete;
        FServer& operator=(const FServer&) = delete;

        bool Start(const FServerSettings& InSettings);
        void Stop();

        NODISCARD bool IsRunning() const { return Transport.IsRunning(); }
        NODISCARD uint16 GetBoundPort() const { return Transport.GetBoundPort(); }

        // Where a plugin or subsystem registers the methods it wants to answer.
        NODISCARD JsonRpc::FDispatcher& GetDispatcher() { return Dispatcher; }

    private:

        void RegisterProtocolMethods();

        Http::FResponse Route(const Http::FRequest& Request);

        Http::FServer        Transport;
        JsonRpc::FDispatcher Dispatcher;

        FServerSettings Settings;
    };
}
