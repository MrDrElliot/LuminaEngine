#include "RuntimePCH.h"
#include "Core/Http/HttpServer.h"

#include "Log/Log.h"

namespace Lumina::Http
{
    namespace
    {
        constexpr int32 GAcceptTimeoutMs   = 25;
        constexpr int32 GReceiveChunkBytes = 8 * 1024;
        constexpr int32 GIdleSleepMs       = 2;
    }

    FServer::~FServer()
    {
        Stop();
    }

    bool FServer::Start(const FServerParams& Params, FRequestHandler InHandler)
    {
        if (IsRunning())
        {
            LOG_WARN("[Http] The server is already running on port {}.", BoundPort);
            return false;
        }

        if (!InHandler)
        {
            LOG_ERROR("[Http] Refused to start without a request handler.");
            return false;
        }

        Platform::FSocketListenParams ListenParams;
        ListenParams.Port          = Params.Port;
        ListenParams.bLoopbackOnly = Params.bLoopbackOnly;

        Listener = Platform::CreateSocketListener(ListenParams);
        if (!Listener)
        {
            LOG_ERROR("[Http] Failed to listen on port {}.", Params.Port);
            return false;
        }

        Handler   = Move(InHandler);
        Limits    = Params.Limits;
        BoundPort = Listener->GetBoundPort();

        bStopRequested.store(false, std::memory_order_release);
        bRunning.store(true, std::memory_order_release);

        Worker = FThread([this]() { ServeLoop(); });

        LOG_INFO("[Http] Listening on port {}.", BoundPort);
        return true;
    }

    void FServer::Stop()
    {
        if (!IsRunning() && !Worker.Joinable())
        {
            return;
        }

        bStopRequested.store(true, std::memory_order_release);

        if (Worker.Joinable())
        {
            Worker.Join();
        }

        Listener.reset();
        Clients.clear();

        bRunning.store(false, std::memory_order_release);
        BoundPort = 0;
    }

    bool FServer::SendAll(Platform::ISocketConnection& Connection, FStringView Text)
    {
        const uint8* Bytes = reinterpret_cast<const uint8*>(Text.data());

        int32 Sent = 0;
        const int32 Total = static_cast<int32>(Text.size());

        while (Sent < Total)
        {
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            Sent += Connection.Send(Bytes + Sent, Total - Sent, Result);

            if (Result == Platform::ESocketResult::WouldBlock)
            {
                // The kernel buffer filled, so yield rather than spinning on a socket that cannot take more.
                Threading::Sleep(GIdleSleepMs);
                continue;
            }

            if (Result != Platform::ESocketResult::Ok)
            {
                return false;
            }
        }

        return true;
    }

    bool FServer::ServiceClient(FClient& Client)
    {
        if (!Client.Connection || !Client.Connection->IsOpen())
        {
            return false;
        }

        uint8 Chunk[GReceiveChunkBytes];

        for (;;)
        {
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            const int32 Read = Client.Connection->Receive(Chunk, GReceiveChunkBytes, Result);

            if (Result == Platform::ESocketResult::WouldBlock)
            {
                break;
            }

            if (Result != Platform::ESocketResult::Ok || Read <= 0)
            {
                return false;
            }

            Client.Pending.append(reinterpret_cast<const char*>(Chunk), static_cast<size_t>(Read));

            if (static_cast<int32>(Client.Pending.size()) > Limits.MaxHeaderBytes + Limits.MaxBodyBytes)
            {
                return false;
            }
        }

        // One read can carry several pipelined requests, so the buffer is drained until it stops yielding one.
        for (;;)
        {
            FRequest Request;
            const EParseResult Parsed = ParseRequest(Client.Pending, Request, Limits);

            if (Parsed == EParseResult::Incomplete)
            {
                return true;
            }

            if (Parsed == EParseResult::Malformed)
            {
                const FResponse Bad = FResponse::Text(400, "Bad Request", "The request could not be parsed.");
                SendAll(*Client.Connection, FStringView(Bad.Serialize()));
                return false;
            }

            FResponse Response;

            try
            {
                Response = Handler(Request);
            }
            catch (const std::exception& Exception)
            {
                LOG_ERROR("[Http] The handler threw. {}", Exception.what());
                Response = FResponse::Text(500, "Internal Server Error", "The handler threw an exception.");
            }
            catch (...)
            {
                Response = FResponse::Text(500, "Internal Server Error", "The handler threw an exception.");
            }

            Response.bKeepAlive = Response.bKeepAlive && Request.WantsKeepAlive();

            const FString Serialized = Response.Serialize();
            if (!SendAll(*Client.Connection, FStringView(Serialized)))
            {
                return false;
            }

            if (!Response.bKeepAlive)
            {
                return false;
            }
        }
    }

    void FServer::ServeLoop()
    {
        while (!bStopRequested.load(std::memory_order_acquire))
        {
            if (Platform::FSocketConnectionPtr Accepted = Listener->Accept(GAcceptTimeoutMs))
            {
                FClient Client;
                Client.Connection = Move(Accepted);
                Clients.push_back(Move(Client));
            }

            bool bServicedAnything = false;

            for (size_t Index = Clients.size(); Index > 0; --Index)
            {
                FClient& Client = Clients[Index - 1];

                const size_t Before = Client.Pending.size();

                if (!ServiceClient(Client))
                {
                    Clients.erase(Clients.begin() + static_cast<int32>(Index - 1));
                    continue;
                }

                bServicedAnything = bServicedAnything || Client.Pending.size() != Before;
            }

            // Accept already blocked for its timeout, so only an idle spin with clients attached needs a rest.
            if (!Clients.empty() && !bServicedAnything)
            {
                Threading::Sleep(GIdleSleepMs);
            }
        }

        Clients.clear();
    }
}
