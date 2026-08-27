#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Platform
{
    enum class ESocketResult : uint8
    {
        Ok,

        // Nothing was ready, which on a non-blocking socket is the ordinary idle case rather than a fault.
        WouldBlock,

        // The peer hung up cleanly, so the connection is finished but nothing went wrong.
        Closed,

        Failed,
    };

    // A single accepted stream. Reads and writes are non-blocking and report WouldBlock instead of stalling.
    class RUNTIME_API ISocketConnection
    {
    public:

        virtual ~ISocketConnection() = default;

        // Returns the byte count, which is zero for every result other than Ok.
        virtual int32 Receive(uint8* Buffer, int32 Capacity, ESocketResult& OutResult) = 0;

        virtual int32 Send(const uint8* Bytes, int32 Count, ESocketResult& OutResult) = 0;

        virtual void Close() = 0;

        NODISCARD virtual bool IsOpen() const = 0;

        // The peer address in dotted host and port form, for logging a connection.
        NODISCARD virtual FString GetPeerDescription() const = 0;
    };

    using FSocketConnectionPtr = TUniquePtr<ISocketConnection>;

    class RUNTIME_API ISocketListener
    {
    public:

        virtual ~ISocketListener() = default;

        // Waits up to the timeout for a peer. Null means nobody arrived, which is not an error.
        NODISCARD virtual FSocketConnectionPtr Accept(int32 TimeoutMilliseconds) = 0;

        // The port actually bound, which is what to report when the caller asked for an ephemeral one.
        NODISCARD virtual uint16 GetBoundPort() const = 0;

        virtual void Close() = 0;
    };

    using FSocketListenerPtr = TUniquePtr<ISocketListener>;

    struct FSocketListenParams
    {
        // Loopback only, because a control channel for the editor has no business reachable off the machine.
        bool bLoopbackOnly = true;

        // Zero asks the operating system for a free port, which GetBoundPort then reports back.
        uint16 Port = 0;

        int32 Backlog = 8;
    };

    // Null when the platform has no sockets, or the port could not be bound.
    RUNTIME_API FSocketListenerPtr CreateSocketListener(const FSocketListenParams& Params);

    // Null when nothing answered inside the timeout. Host is a dotted address rather than a name.
    RUNTIME_API FSocketConnectionPtr ConnectSocket(FStringView Host, uint16 Port, int32 TimeoutMilliseconds);

    RUNTIME_API bool IsSocketSupported();
}
