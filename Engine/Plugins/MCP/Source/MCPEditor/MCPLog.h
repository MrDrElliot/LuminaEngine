#pragma once

#include "Containers/RingBuffer.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "Memory/SmartPtr.h"
#include "Platform/Time/PlatformTime.h"

namespace Lumina::MCP
{
    struct FTransaction
    {
        PlatformTime::FDateTime Time;

        // The session the client was handed by initialize, empty for a client that never ran it.
        FString Session;
        FString Method;
        FString Tool;

        // The call's params, or a tool call's arguments, in one compact line for the list.
        FString Arguments;

        FString Request;
        FString Response;

        // Empty on success, else the protocol error or the failing tool's own text.
        FString Error;

        double DurationMs = 0.0;

        NODISCARD bool IsError() const { return !Error.IsEmpty(); }
    };

    struct FConnection
    {
        FString Session;
        FString Client;

        PlatformTime::FDateTime FirstSeen;
        PlatformTime::FDateTime LastSeen;

        // Orders connections by recency where LastSeen, at millisecond resolution, would tie.
        uint64 LastSeenRevision = 0;

        uint32 Requests = 0;
        uint32 Errors   = 0;
        bool   bOpen    = true;
    };

    // Text beyond MaxBytes is cut, ending in a note of how much was dropped.
    NODISCARD MCPEDITOR_API FString ClipText(FStringView Text, int32 MaxBytes);

    // Every message the server dispatched and every client it saw, bounded so a long session holds a fixed amount.
    class MCPEDITOR_API FLog
    {
    public:

        static constexpr int32 kCapacity = 256;

        // Each payload is clipped to this, so the capacity also bounds the bytes held.
        static constexpr int32 kMaxPayloadBytes = 256 * 1024;

        static constexpr int32 kMaxConnections = 64;

        FLog();

        // Returns the session a successful initialize opened, which the transport hands back to the client.
        FString Record(FStringView Session, FStringView Request, FStringView Response, double DurationMs);

        void RecordDisconnect(FStringView Session);

        void Clear();

        // Bumps on every change, so a reader can skip copying when nothing happened.
        NODISCARD uint64 GetRevision() const { return Revision.load(std::memory_order_acquire); }

        // Entries come out oldest first and are shared, so the copy holds the lock only for pointer copies.
        void Snapshot(TVector<TSharedPtr<const FTransaction>>& OutTransactions, TVector<FConnection>& OutConnections) const;

    private:

        FConnection* FindConnection(FStringView Session);
        FConnection& FindOrAddConnection(FStringView Session, const PlatformTime::FDateTime& Now);

        TRingBuffer<TSharedPtr<const FTransaction>> Transactions;
        TVector<FConnection> Connections;

        mutable FMutex Mutex;
        TAtomic<uint64> Revision { 0 };
    };
}
