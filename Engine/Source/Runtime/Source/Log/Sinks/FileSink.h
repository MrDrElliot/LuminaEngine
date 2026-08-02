#pragma once

#include <cstdio>

#include "Log/LogFormat.h"
#include "Log/LogSink.h"


namespace Lumina
{
    class FFileSink : public Logging::ILogSink
    {
    public:

        FFileSink(const FString& InBasePath, uint64 InMaxFileBytes, uint32 InMaxFiles);
        ~FFileSink() override;

        NODISCARD bool IsOpen() const { return Handle != nullptr; }

        void Write(const Logging::FLogRecord& Record) override;
        void Flush() override;

    private:

        // Renames Lumina.log -> Lumina.1.log -> ... and drops the oldest.
        void Rotate();

        FString             BasePath;
        std::FILE*          Handle       = nullptr;
        uint64              WrittenBytes = 0;
        uint64              MaxFileBytes = 0;
        uint32              MaxFiles     = 0;
        Logging::FLogBuffer Batch;
    };
}
