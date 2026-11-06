#include "Containers/StringFormat.h"
#include "RuntimePCH.h"
#include "FileSink.h"

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

            const int32 Dot = BasePath.FindLastOf('.');
            const FString Stem      = Dot == Constants::kIndexNone ? BasePath : BasePath.Mid(0, Dot);
            const FString Extension = Dot == Constants::kIndexNone ? FString(".log") : BasePath.Mid(Dot);

            FString Result = Stem;
            Result += '.';
            AppendFormat(Result, "{}", Index);
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
        OpenCurrent(Filesystem::ECreateMode::CreateAlways);
    }

    void FFileSink::RotateExisting()
    {
        // Walk down so each move lands on a free slot, replacing the oldest at MaxFiles.
        for (uint32 Index = MaxFiles; Index > 0; --Index)
        {
            Filesystem::Move(MakeRotatedPath(BasePath, Index - 1), MakeRotatedPath(BasePath, Index));
        }
    }

    void FFileSink::OpenCurrent(Filesystem::ECreateMode Mode)
    {
        // The Logs folder is ours to make, since opening a file won't create the directory.
        Filesystem::MakeParentDirectoryTree(BasePath);

        Handle = Filesystem::Open(BasePath, Filesystem::EAccess::Write, Mode, Filesystem::EShare::Read, Filesystem::EHint::Sequential);
        if (Handle.IsValid())
        {
            Handle.Seek(0, Filesystem::ESeek::End);
        }
    }

    void FFileSink::Rotate()
    {
        Handle.Close();

        RotateExisting();
        OpenCurrent(Filesystem::ECreateMode::CreateAlways);

        WrittenBytes = 0;
    }

    void FFileSink::Retarget(const FString& NewBasePath)
    {
        if (NewBasePath.IsEmpty() || NewBasePath == BasePath)
        {
            return;
        }

        Flush();
        Handle.Close();

        const FString OldPath = BasePath;
        BasePath = NewBasePath;

        // Whatever sits at the destination is a previous run, so age it out before this one lands.
        Filesystem::MakeParentDirectoryTree(BasePath);
        RotateExisting();

        // Carry the boot lines across. A move can fail across volumes, so fall back to a copy.
        if (!Filesystem::Move(OldPath, BasePath))
        {
            Filesystem::Copy(OldPath, BasePath, true);
            Filesystem::RemoveFile(OldPath);
        }

        // Appends, since the moved file already holds everything written so far.
        OpenCurrent(Filesystem::ECreateMode::OpenAlways);
    }

    void FFileSink::Write(const Logging::FLogRecord& Record)
    {
        if (!Handle.IsValid())
        {
            return;
        }

        const Logging::FLevelDescriptor& Descriptor = Logging::GetLevelDescriptor(Record.Level);

        Batch.AppendChar('[');
        Logging::AppendDateTime(Batch, *Record.Timestamp);
        Batch.AppendLiteral("] [");
        Batch.Append(Descriptor.DisplayName.GetData(), static_cast<uint32>(Descriptor.DisplayName.Len()));
        Batch.AppendLiteral("] [");
        Batch.AppendUInt(Record.ThreadId);
        Batch.AppendLiteral("] ");
        Batch.Append(Record.Message);
        Batch.AppendChar('\n');
    }

    void FFileSink::Flush()
    {
        if (!Handle.IsValid() || Batch.IsEmpty())
        {
            return;
        }

        // Straight to the OS rather than the disk, so it survives a crash without paying for a sync.
        Handle.Write(Batch.Data(), Batch.Size());

        WrittenBytes += Batch.Size();
        Batch.Clear();

        if (WrittenBytes >= MaxFileBytes)
        {
            Rotate();
        }
    }
}
