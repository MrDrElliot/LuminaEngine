#pragma once

#include "Log/LogFormat.h"
#include "Log/LogSink.h"


namespace Lumina
{
    // Buffers a whole batch and emits it with one write.
    class FStdoutSink : public Logging::ILogSink
    {
    public:

        FStdoutSink();

        void Write(const Logging::FLogRecord& Record) override;
        void Flush() override;

    private:

        Logging::FLogBuffer Batch;
        bool                bUseColor = false;
    };
}
