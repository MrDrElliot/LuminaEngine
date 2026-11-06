#pragma once

#include "Log/LogFormat.h"
#include "Log/LogSink.h"
#include "Platform/Filesystem/PlatformFilesystem.h"


namespace Lumina
{
    class RUNTIME_API FFileSink : public Logging::ILogSink
    {
    public:

        FFileSink(const FString& InBasePath, uint64 InMaxFileBytes, uint32 InMaxFiles);
        FFileSink(const FFileSink&) = delete;
        FFileSink& operator=(const FFileSink&) = delete;
        FFileSink(FFileSink&&) = delete;
        FFileSink& operator=(FFileSink&&) = delete;

        NODISCARD bool IsOpen() const { return Handle.IsValid(); }
        NODISCARD const FString& GetBasePath() const { return BasePath; }

        // Moves the live file to NewBasePath, carrying this run's lines with it. Used once the
        // project is known, so a run that started against the engine ends up in the project's Logs.
        void Retarget(const FString& NewBasePath);

        void Write(const Logging::FLogRecord& Record) override;
        void Flush() override;

    private:

        // Renames Lumina.log -> Lumina.1.log -> ... and drops the oldest.
        void Rotate();

        void RotateExisting();
        void OpenCurrent(Filesystem::ECreateMode Mode);

        FString                 BasePath;
        Filesystem::FFileHandle Handle;
        uint64                  WrittenBytes = 0;
        uint64                  MaxFileBytes = 0;
        uint32                  MaxFiles     = 0;
        Logging::FLogBuffer     Batch;
    };
}
