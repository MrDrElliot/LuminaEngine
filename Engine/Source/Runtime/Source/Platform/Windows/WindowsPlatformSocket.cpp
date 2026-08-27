#include "RuntimePCH.h"
#ifdef _WIN32

#include "Containers/String.h"
#include "Core/Threading/Sync.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Socket/PlatformSocket.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

namespace Lumina::Platform
{
    namespace
    {
        FMutex GStartupMutex;

        bool GStartupAttempted = false;
        bool GStartupSucceeded = false;

        bool EnsureWinsock()
        {
            FScopeLock Lock(GStartupMutex);

            if (GStartupAttempted)
            {
                return GStartupSucceeded;
            }

            GStartupAttempted = true;

            WSADATA Data = {};
            const int32 Result = WSAStartup(MAKEWORD(2, 2), &Data);

            GStartupSucceeded = Result == 0;
            if (!GStartupSucceeded)
            {
                LOG_ERROR("[Socket] WSAStartup failed ({}).", Result);
            }

            return GStartupSucceeded;
        }

        ESocketResult TranslateLastError()
        {
            const int32 Error = WSAGetLastError();

            switch (Error)
            {
            case WSAEWOULDBLOCK:
                return ESocketResult::WouldBlock;

            case WSAECONNRESET:
            case WSAECONNABORTED:
            case WSAENETRESET:
            case WSAESHUTDOWN:
                return ESocketResult::Closed;

            default:
                return ESocketResult::Failed;
            }
        }

        bool SetNonBlocking(SOCKET Handle)
        {
            u_long Mode = 1;
            return ioctlsocket(Handle, FIONBIO, &Mode) == 0;
        }

        class FWindowsSocketConnection final : public ISocketConnection
        {
        public:

            FWindowsSocketConnection(SOCKET InHandle, const FString& InPeer)
                : Handle(InHandle)
                , Peer(InPeer)
            {}

            ~FWindowsSocketConnection() override { Close(); }

            int32 Receive(uint8* Buffer, int32 Capacity, ESocketResult& OutResult) override;
            int32 Send(const uint8* Bytes, int32 Count, ESocketResult& OutResult) override;

            void Close() override;

            bool IsOpen() const override { return Handle != INVALID_SOCKET; }
            FString GetPeerDescription() const override { return Peer; }

        private:

            SOCKET  Handle = INVALID_SOCKET;
            FString Peer;
        };

        int32 FWindowsSocketConnection::Receive(uint8* Buffer, int32 Capacity, ESocketResult& OutResult)
        {
            if (Handle == INVALID_SOCKET || Buffer == nullptr || Capacity <= 0)
            {
                OutResult = ESocketResult::Failed;
                return 0;
            }

            const int32 Read = recv(Handle, reinterpret_cast<char*>(Buffer), Capacity, 0);

            if (Read > 0)
            {
                OutResult = ESocketResult::Ok;
                return Read;
            }

            // Winsock reports an orderly shutdown as a zero length read rather than an error.
            OutResult = Read == 0 ? ESocketResult::Closed : TranslateLastError();
            return 0;
        }

        int32 FWindowsSocketConnection::Send(const uint8* Bytes, int32 Count, ESocketResult& OutResult)
        {
            if (Handle == INVALID_SOCKET || Bytes == nullptr || Count <= 0)
            {
                OutResult = ESocketResult::Failed;
                return 0;
            }

            const int32 Written = send(Handle, reinterpret_cast<const char*>(Bytes), Count, 0);

            if (Written >= 0)
            {
                OutResult = ESocketResult::Ok;
                return Written;
            }

            OutResult = TranslateLastError();
            return 0;
        }

        void FWindowsSocketConnection::Close()
        {
            if (Handle == INVALID_SOCKET)
            {
                return;
            }

            // Half closing first lets whatever is already queued drain instead of being discarded.
            shutdown(Handle, SD_SEND);
            closesocket(Handle);

            Handle = INVALID_SOCKET;
        }

        class FWindowsSocketListener final : public ISocketListener
        {
        public:

            ~FWindowsSocketListener() override { Close(); }

            bool Start(const FSocketListenParams& Params);

            FSocketConnectionPtr Accept(int32 TimeoutMilliseconds) override;

            uint16 GetBoundPort() const override { return BoundPort; }

            void Close() override;

        private:

            SOCKET Handle    = INVALID_SOCKET;
            uint16 BoundPort = 0;
        };

        bool FWindowsSocketListener::Start(const FSocketListenParams& Params)
        {
            if (!EnsureWinsock())
            {
                return false;
            }

            Handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (Handle == INVALID_SOCKET)
            {
                LOG_ERROR("[Socket] Failed to create the listening socket ({}).", WSAGetLastError());
                return false;
            }

            // Windows SO_REUSEADDR lets an unrelated process steal the port, so the exclusive flag is used.
            BOOL Exclusive = TRUE;
            setsockopt(Handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&Exclusive), sizeof(Exclusive));

            sockaddr_in Address = {};
            Address.sin_family = AF_INET;
            Address.sin_port   = htons(Params.Port);
            Address.sin_addr.s_addr = Params.bLoopbackOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

            if (bind(Handle, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) == SOCKET_ERROR)
            {
                LOG_ERROR("[Socket] Failed to bind port {} ({}).", Params.Port, WSAGetLastError());
                Close();
                return false;
            }

            if (listen(Handle, Params.Backlog) == SOCKET_ERROR)
            {
                LOG_ERROR("[Socket] Failed to listen ({}).", WSAGetLastError());
                Close();
                return false;
            }

            sockaddr_in Bound = {};
            int32 BoundSize = sizeof(Bound);
            if (getsockname(Handle, reinterpret_cast<sockaddr*>(&Bound), &BoundSize) == 0)
            {
                BoundPort = ntohs(Bound.sin_port);
            }

            if (!SetNonBlocking(Handle))
            {
                LOG_ERROR("[Socket] Failed to make the listening socket non-blocking ({}).", WSAGetLastError());
                Close();
                return false;
            }

            return true;
        }

        FSocketConnectionPtr FWindowsSocketListener::Accept(int32 TimeoutMilliseconds)
        {
            if (Handle == INVALID_SOCKET)
            {
                return FSocketConnectionPtr();
            }

            fd_set ReadSet;
            FD_ZERO(&ReadSet);
            FD_SET(Handle, &ReadSet);

            timeval Timeout = {};
            Timeout.tv_sec  = TimeoutMilliseconds / 1000;
            Timeout.tv_usec = (TimeoutMilliseconds % 1000) * 1000;

            const int32 Ready = select(0, &ReadSet, nullptr, nullptr, TimeoutMilliseconds < 0 ? nullptr : &Timeout);
            if (Ready <= 0)
            {
                return FSocketConnectionPtr();
            }

            sockaddr_in PeerAddress = {};
            int32 PeerSize = sizeof(PeerAddress);

            const SOCKET Accepted = accept(Handle, reinterpret_cast<sockaddr*>(&PeerAddress), &PeerSize);
            if (Accepted == INVALID_SOCKET)
            {
                return FSocketConnectionPtr();
            }

            if (!SetNonBlocking(Accepted))
            {
                closesocket(Accepted);
                return FSocketConnectionPtr();
            }

            // Request and reply are both small and latency bound, so waiting to coalesce them only hurts.
            BOOL NoDelay = TRUE;
            setsockopt(Accepted, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay));

            char Text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &PeerAddress.sin_addr, Text, sizeof(Text));

            const FString Peer = Lumina::Format("{} port {}", Text, ntohs(PeerAddress.sin_port));

            return FSocketConnectionPtr(Memory::New<FWindowsSocketConnection>(Accepted, Peer));
        }

        void FWindowsSocketListener::Close()
        {
            if (Handle == INVALID_SOCKET)
            {
                return;
            }

            closesocket(Handle);
            Handle = INVALID_SOCKET;
        }
    }

    bool IsSocketSupported()
    {
        return EnsureWinsock();
    }

    FSocketConnectionPtr ConnectSocket(FStringView Host, uint16 Port, int32 TimeoutMilliseconds)
    {
        if (!EnsureWinsock())
        {
            return FSocketConnectionPtr();
        }

        const FString HostText(Host.data(), Host.size());

        sockaddr_in Address = {};
        Address.sin_family = AF_INET;
        Address.sin_port   = htons(Port);

        if (inet_pton(AF_INET, HostText.c_str(), &Address.sin_addr) != 1)
        {
            LOG_ERROR("[Socket] '{}' is not a dotted address.", HostText);
            return FSocketConnectionPtr();
        }

        const SOCKET Handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Handle == INVALID_SOCKET)
        {
            return FSocketConnectionPtr();
        }

        // Connecting non-blocking is what makes the timeout ours rather than whatever the stack picks.
        if (!SetNonBlocking(Handle))
        {
            closesocket(Handle);
            return FSocketConnectionPtr();
        }

        bool bConnected = connect(Handle, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) == 0;

        if (!bConnected && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set WriteSet;
            FD_ZERO(&WriteSet);
            FD_SET(Handle, &WriteSet);

            fd_set ErrorSet;
            FD_ZERO(&ErrorSet);
            FD_SET(Handle, &ErrorSet);

            timeval Timeout = {};
            Timeout.tv_sec  = TimeoutMilliseconds / 1000;
            Timeout.tv_usec = (TimeoutMilliseconds % 1000) * 1000;

            const int32 Ready = select(0, nullptr, &WriteSet, &ErrorSet,
                TimeoutMilliseconds < 0 ? nullptr : &Timeout);

            if (Ready > 0 && FD_ISSET(Handle, &WriteSet))
            {
                int32 PendingError = 0;
                int32 ErrorSize = sizeof(PendingError);

                bConnected = getsockopt(Handle, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&PendingError), &ErrorSize) == 0 && PendingError == 0;
            }
        }

        if (!bConnected)
        {
            closesocket(Handle);
            return FSocketConnectionPtr();
        }

        BOOL NoDelay = TRUE;
        setsockopt(Handle, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay));

        const FString Peer = Lumina::Format("{} port {}", HostText, Port);

        return FSocketConnectionPtr(Memory::New<FWindowsSocketConnection>(Handle, Peer));
    }

    FSocketListenerPtr CreateSocketListener(const FSocketListenParams& Params)
    {
        FWindowsSocketListener* Listener = Memory::New<FWindowsSocketListener>();
        if (!Listener->Start(Params))
        {
            Memory::Delete(Listener);
            return FSocketListenerPtr();
        }

        return FSocketListenerPtr(Listener);
    }
}

#endif
