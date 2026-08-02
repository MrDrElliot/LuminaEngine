#include "RuntimePCH.h"
#include "FileSink.h"

#include <cstdio>

namespace Lumina
{
    namespace
    {
        // Index 0 is the live file.
        FString MakeRotatedPath(const FString& BasePath, uint32 Index)
        {
            if (Index == 0)
            {
                return BasePath;
            }

            const size_t Dot = BasePath.find_last_of('.');
            const FString Stem      = Dot == FString::npos ? BasePath : BasePath.substr(0, Dot);
            const FString Extension = Dot == FString::npos ? FString(".log") : BasePath.substr(Dot);

            FString Result = Stem;
            Result += '.';
            std::format_to(std::back_inserter(Result), "{}", Index);
            Result += Extension;
            return Result;
        }
    }

    FFileSink::FFileSink(const FString& InBasePath, uint64 InMaxFileBytes, uint32 InMaxFiles)
        : BasePath(InBasePath)
        , MaxFileBytes(InMaxFileBytes)
        , MaxFiles(InMaxFiles)
        , Batch(64 * 1024)
    {
        // Up front, so each run gets its own file.
        Rotate();
    }

    FFileSink::~FFileSink()
    {
        if (Handle != nullptr)
        {
            std::fclose(Handle);
            Handle = nullptr;
        }
    }

    void FFileSink::Rotate()
    {
        if (Handle != nullptr)
        {
            std::fclose(Handle);
            Handle = nullptr;
        }

        // Walk down so each rename lands on a free slot.
        for (uint32 Index = MaxFiles; Index > 0; --Index)
        {
            const FString Source = MakeRotatedPath(BasePath, Index - 1);
            const FString Target = MakeRotatedPath(BasePath, Index);

            if (Index == MaxFiles)
            {
                std::remove(Target.c_str());
            }
            std::rename(Source.c_str(), Target.c_str());
        }

        Handle = std::fopen(BasePath.c_str(), "wb");
        if (Handle != nullptr)
        {
            std::setvbuf(Handle, nullptr, _IOFBF, 64 * 1024);
        }

        WrittenBytes = 0;
    }

    void FFileSink::Write(const Logging::FLogRecord& Record)
    {
        if (Handle == nullptr)
        {
            return;
        }

        const Logging::FLevelDescriptor& Descriptor = Logging::GetLevelDescriptor(Record.Level);

        Batch.AppendChar('[');
        Logging::AppendDateTime(Batch, *Record.Timestamp);
        Batch.AppendLiteral("] [");
        Batch.Append(Descriptor.DisplayName.data(), static_cast<uint32>(Descriptor.DisplayName.size()));
        Batch.AppendLiteral("] [");
        Batch.AppendUInt(Record.ThreadId);
        Batch.AppendLiteral("] ");
        Batch.Append(Record.Message);
        Batch.AppendChar('\n');
    }

    void FFileSink::Flush()
    {
        if (Handle == nullptr || Batch.IsEmpty())
        {
            return;
        }

        std::fwrite(Batch.Data(), 1, Batch.Size(), Handle);

        // To the OS, not the disk: survives a process crash without paying for a sync.
        std::fflush(Handle);

        WrittenBytes += Batch.Size();
        Batch.Clear();

        if (WrittenBytes >= MaxFileBytes)
        {
            Rotate();
        }
    }
}
