#include "RuntimePCH.h"
#include "FileSink.h"

#include <cstdio>
#include <filesystem>

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
        RotateExisting();
        OpenCurrent("wb");
    }

    FFileSink::~FFileSink()
    {
        if (Handle != nullptr)
        {
            std::fclose(Handle);
            Handle = nullptr;
        }
    }

    void FFileSink::RotateExisting()
    {
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
    }

    void FFileSink::OpenCurrent(const char* Mode)
    {
        // The Logs folder is ours to make; fopen won't create the directory.
        const std::filesystem::path Parent = std::filesystem::path(BasePath.c_str()).parent_path();
        if (!Parent.empty())
        {
            std::error_code Ec;
            std::filesystem::create_directories(Parent, Ec);
        }

        Handle = std::fopen(BasePath.c_str(), Mode);
        if (Handle != nullptr)
        {
            std::setvbuf(Handle, nullptr, _IOFBF, 64 * 1024);
        }
    }

    void FFileSink::Rotate()
    {
        if (Handle != nullptr)
        {
            std::fclose(Handle);
            Handle = nullptr;
        }

        RotateExisting();
        OpenCurrent("wb");

        WrittenBytes = 0;
    }

    void FFileSink::Retarget(const FString& NewBasePath)
    {
        if (NewBasePath.empty() || NewBasePath == BasePath)
        {
            return;
        }

        Flush();

        if (Handle != nullptr)
        {
            std::fclose(Handle);
            Handle = nullptr;
        }

        const FString OldPath = BasePath;
        BasePath = NewBasePath;

        // Whatever sits at the destination is a previous run: age it out before this one lands on it.
        {
            const std::filesystem::path Parent = std::filesystem::path(BasePath.c_str()).parent_path();
            if (!Parent.empty())
            {
                std::error_code Ec;
                std::filesystem::create_directories(Parent, Ec);
            }
        }
        RotateExisting();

        // Carry the boot lines across. rename fails across volumes, so fall back to a copy.
        if (std::rename(OldPath.c_str(), BasePath.c_str()) != 0)
        {
            std::error_code Ec;
            std::filesystem::copy_file(OldPath.c_str(), BasePath.c_str(),
                std::filesystem::copy_options::overwrite_existing, Ec);
            std::remove(OldPath.c_str());
        }

        // Append: the moved file already holds everything written so far.
        OpenCurrent("ab");
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
