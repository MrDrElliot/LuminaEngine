#pragma once

#include "LogLevel.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"


namespace Lumina::Logging
{
    // Split once per second by the backend and shared by every sink.
    struct FLogTimestamp
    {
        char    Date[11];   // "YYYY-MM-DD"
        char    Clock[9];   // "HH:MM:SS"
        uint16  Millis;
    };

    // Message points into the queue's storage and is only valid for the Write() call.
    struct FLogRecord
    {
        FStringView             Message;
        const FLogTimestamp*    Timestamp;
        ELogLevel               Level;
        uint32                  ThreadId;
    };

    // Only ever touched by one thread at a time, so implementations need no locking.
    class ILogSink
    {
    public:

        virtual ~ILogSink() = default;

        virtual void Write(const FLogRecord& Record) = 0;

        // Called once per drained batch; buffering sinks push to the OS here.
        virtual void Flush() {}
    };

    RUNTIME_API void AddSink(TUniquePtr<ILogSink> Sink);
}
