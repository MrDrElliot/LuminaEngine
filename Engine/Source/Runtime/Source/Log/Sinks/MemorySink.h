#pragma once

#include "Log/Log.h"
#include "Log/LogSink.h"


namespace Lumina
{
    // Feeds the editor console; the ring drops the oldest, so this never grows.
    class FMemorySink : public Logging::ILogSink
    {
    public:

        explicit FMemorySink(Logging::FLogQueue& InOutputMessages)
            : OutputMessages(InOutputMessages)
        {}

        void Write(const Logging::FLogRecord& Record) override;

    private:

        Logging::FLogQueue& OutputMessages;
    };
}
