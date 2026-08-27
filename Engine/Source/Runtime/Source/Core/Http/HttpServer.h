#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Http/HttpMessage.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"
#include "Platform/Socket/PlatformSocket.h"

namespace Lumina::Http
{
    // Runs on the server thread, so anything touching the world has to hop to the game thread itself.
    using FRequestHandler = TFunction<FResponse(const FRequest&)>;

    struct FServerParams
    {
        // Zero asks for an ephemeral port, which GetBoundPort then reports back.
        uint16 Port = 0;

        bool bLoopbackOnly = true;

        FParseLimits Limits;
    };

    // A small HTTP 1.1 server that multiplexes its connections on one thread.
    class RUNTIME_API FServer
    {
    public:

        FServer() = default;
        ~FServer();

        FServer(const FServer&) = delete;
        FServer& operator=(const FServer&) = delete;

        bool Start(const FServerParams& Params, FRequestHandler Handler);

        // Blocks until the server thread has stopped. Idempotent.
        void Stop();

        NODISCARD bool IsRunning() const { return bRunning.load(std::memory_order_acquire); }

        NODISCARD uint16 GetBoundPort() const { return BoundPort; }

    private:

        struct FClient
        {
            Platform::FSocketConnectionPtr Connection;
            FString Pending;

            // Set once a reply asked to close, so the remainder drains before the socket goes away.
            bool bClosing = false;
        };

        void ServeLoop();

        // False when the connection should be dropped.
        bool ServiceClient(FClient& Client);

        bool SendAll(Platform::ISocketConnection& Connection, FStringView Text);

        Platform::FSocketListenerPtr Listener;

        FRequestHandler Handler;
        FParseLimits    Limits;

        TVector<FClient> Clients;

        FThread Worker;

        TAtomic<bool> bStopRequested { false };
        TAtomic<bool> bRunning       { false };

        uint16 BoundPort = 0;
    };
}
