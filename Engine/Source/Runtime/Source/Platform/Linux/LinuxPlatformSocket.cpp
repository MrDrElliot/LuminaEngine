#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Containers/String.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Socket/PlatformSocket.h"

namespace Lumina::Platform
{
    namespace
    {
        constexpr int32 GInvalidSocket = -1;

        ESocketResult TranslateErrno()
        {
            switch (errno)
            {
            case EAGAIN:
#if EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EINTR:
                return ESocketResult::WouldBlock;

            case ECONNRESET:
            case ECONNABORTED:
            case EPIPE:
            case ENOTCONN:
                return ESocketResult::Closed;

            default:
                return ESocketResult::Failed;
            }
        }

        bool SetNonBlocking(int32 Handle)
        {
            const int32 Flags = fcntl(Handle, F_GETFL, 0);
            if (Flags == -1)
            {
                return false;
            }

            return fcntl(Handle, F_SETFL, Flags | O_NONBLOCK) == 0;
        }

        class FLinuxSocketConnection final : public ISocketConnection
        {
        public:

            FLinuxSocketConnection(int32 InHandle, const FString& InPeer)
                : Handle(InHandle)
                , Peer(InPeer)
            {}

            ~FLinuxSocketConnection() override { Close(); }

            int32 Receive(uint8* Buffer, int32 Capacity, ESocketResult& OutResult) override;
            int32 Send(const uint8* Bytes, int32 Count, ESocketResult& OutResult) override;

            void Close() override;

            bool IsOpen() const override { return Handle != GInvalidSocket; }
            FString GetPeerDescription() const override { return Peer; }

        private:

            int32   Handle = GInvalidSocket;
            FString Peer;
        };

        int32 FLinuxSocketConnection::Receive(uint8* Buffer, int32 Capacity, ESocketResult& OutResult)
        {
            if (Handle == GInvalidSocket || Buffer == nullptr || Capacity <= 0)
            {
                OutResult = ESocketResult::Failed;
                return 0;
            }

            const ssize_t Read = recv(Handle, Buffer, static_cast<size_t>(Capacity), 0);

            if (Read > 0)
            {
                OutResult = ESocketResult::Ok;
                return static_cast<int32>(Read);
            }

            // A zero length read is how an orderly shutdown arrives, not a failure.
            OutResult = Read == 0 ? ESocketResult::Closed : TranslateErrno();
            return 0;
        }

        int32 FLinuxSocketConnection::Send(const uint8* Bytes, int32 Count, ESocketResult& OutResult)
        {
            if (Handle == GInvalidSocket || Bytes == nullptr || Count <= 0)
            {
                OutResult = ESocketResult::Failed;
                return 0;
            }

            // MSG_NOSIGNAL keeps a write to a hung up peer from killing the process with SIGPIPE.
            const ssize_t Written = send(Handle, Bytes, static_cast<size_t>(Count), MSG_NOSIGNAL);

            if (Written >= 0)
            {
                OutResult = ESocketResult::Ok;
                return static_cast<int32>(Written);
            }

            OutResult = TranslateErrno();
            return 0;
        }

        void FLinuxSocketConnection::Close()
        {
            if (Handle == GInvalidSocket)
            {
                return;
            }

            shutdown(Handle, SHUT_WR);
            close(Handle);

            Handle = GInvalidSocket;
        }

        class FLinuxSocketListener final : public ISocketListener
        {
        public:

            ~FLinuxSocketListener() override { Close(); }

            bool Start(const FSocketListenParams& Params);

            FSocketConnectionPtr Accept(int32 TimeoutMilliseconds) override;

            uint16 GetBoundPort() const override { return BoundPort; }

            void Close() override;

        private:

            int32  Handle    = GInvalidSocket;
            uint16 BoundPort = 0;
        };

        bool FLinuxSocketListener::Start(const FSocketListenParams& Params)
        {
            Handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (Handle == GInvalidSocket)
            {
                LOG_ERROR("[Socket] Failed to create the listening socket ({}).", errno);
                return false;
            }

            // Without this a restart inside the TIME_WAIT window cannot rebind the same port.
            int32 Reuse = 1;
            setsockopt(Handle, SOL_SOCKET, SO_REUSEADDR, &Reuse, sizeof(Reuse));

            sockaddr_in Address = {};
            Address.sin_family = AF_INET;
            Address.sin_port   = htons(Params.Port);
            Address.sin_addr.s_addr = Params.bLoopbackOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

            if (bind(Handle, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0)
            {
                LOG_ERROR("[Socket] Failed to bind port {} ({}).", Params.Port, errno);
                Close();
                return false;
            }

            if (listen(Handle, Params.Backlog) != 0)
            {
                LOG_ERROR("[Socket] Failed to listen ({}).", errno);
                Close();
                return false;
            }

            sockaddr_in Bound = {};
            socklen_t BoundSize = sizeof(Bound);
            if (getsockname(Handle, reinterpret_cast<sockaddr*>(&Bound), &BoundSize) == 0)
            {
                BoundPort = ntohs(Bound.sin_port);
            }

            if (!SetNonBlocking(Handle))
            {
                LOG_ERROR("[Socket] Failed to make the listening socket non-blocking ({}).", errno);
                Close();
                return false;
            }

            return true;
        }

        FSocketConnectionPtr FLinuxSocketListener::Accept(int32 TimeoutMilliseconds)
        {
            if (Handle == GInvalidSocket)
            {
                return FSocketConnectionPtr();
            }

            pollfd Watch = {};
            Watch.fd     = Handle;
            Watch.events = POLLIN;

            const int32 Ready = poll(&Watch, 1, TimeoutMilliseconds);
            if (Ready <= 0)
            {
                return FSocketConnectionPtr();
            }

            sockaddr_in PeerAddress = {};
            socklen_t PeerSize = sizeof(PeerAddress);

            const int32 Accepted = accept(Handle, reinterpret_cast<sockaddr*>(&PeerAddress), &PeerSize);
            if (Accepted == GInvalidSocket)
            {
                return FSocketConnectionPtr();
            }

            if (!SetNonBlocking(Accepted))
            {
                close(Accepted);
                return FSocketConnectionPtr();
            }

            // Request and reply are both small and latency bound, so waiting to coalesce them only hurts.
            int32 NoDelay = 1;
            setsockopt(Accepted, IPPROTO_TCP, TCP_NODELAY, &NoDelay, sizeof(NoDelay));

            char Text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &PeerAddress.sin_addr, Text, sizeof(Text));

            const FString Peer = Lumina::Format("{} port {}", Text, ntohs(PeerAddress.sin_port));

            return FSocketConnectionPtr(Memory::New<FLinuxSocketConnection>(Accepted, Peer));
        }

        void FLinuxSocketListener::Close()
        {
            if (Handle == GInvalidSocket)
            {
                return;
            }

            close(Handle);
            Handle = GInvalidSocket;
        }
    }

    bool IsSocketSupported()
    {
        return true;
    }

    FSocketConnectionPtr ConnectSocket(FStringView Host, uint16 Port, int32 TimeoutMilliseconds)
    {
        const FString HostText(Host.data(), Host.size());

        sockaddr_in Address = {};
        Address.sin_family = AF_INET;
        Address.sin_port   = htons(Port);

        if (inet_pton(AF_INET, HostText.c_str(), &Address.sin_addr) != 1)
        {
            LOG_ERROR("[Socket] '{}' is not a dotted address.", HostText);
            return FSocketConnectionPtr();
        }

        const int32 Handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Handle == GInvalidSocket)
        {
            return FSocketConnectionPtr();
        }

        // Connecting non-blocking is what makes the timeout ours rather than whatever the stack picks.
        if (!SetNonBlocking(Handle))
        {
            close(Handle);
            return FSocketConnectionPtr();
        }

        bool bConnected = connect(Handle, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) == 0;

        if (!bConnected && errno == EINPROGRESS)
        {
            pollfd Watch = {};
            Watch.fd     = Handle;
            Watch.events = POLLOUT;

            if (poll(&Watch, 1, TimeoutMilliseconds) > 0 && (Watch.revents & POLLOUT) != 0)
            {
                int32 PendingError = 0;
                socklen_t ErrorSize = sizeof(PendingError);

                bConnected = getsockopt(Handle, SOL_SOCKET, SO_ERROR, &PendingError, &ErrorSize) == 0
                          && PendingError == 0;
            }
        }

        if (!bConnected)
        {
            close(Handle);
            return FSocketConnectionPtr();
        }

        int32 NoDelay = 1;
        setsockopt(Handle, IPPROTO_TCP, TCP_NODELAY, &NoDelay, sizeof(NoDelay));

        const FString Peer = Lumina::Format("{} port {}", HostText, Port);

        return FSocketConnectionPtr(Memory::New<FLinuxSocketConnection>(Handle, Peer));
    }

    FSocketListenerPtr CreateSocketListener(const FSocketListenParams& Params)
    {
        FLinuxSocketListener* Listener = Memory::New<FLinuxSocketListener>();
        if (!Listener->Start(Params))
        {
            Memory::Delete(Listener);
            return FSocketListenerPtr();
        }

        return FSocketListenerPtr(Listener);
    }
}

#endif
