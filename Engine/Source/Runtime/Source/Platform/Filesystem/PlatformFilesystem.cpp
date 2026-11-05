#include "RuntimePCH.h"
#include "PlatformFilesystem.h"

#include "Log/Log.h"

namespace Lumina::Filesystem
{
    namespace
    {
        constexpr int64 kSpliceChunkBytes = 1ll << 20;

        // A size the destination cannot count is refused rather than truncated, which would leave the read overrunning it.
        bool FitsDestination(int64 Size, int64 MaxCount, FStringView Path)
        {
            if (Size <= MaxCount)
            {
                return true;
            }
            LOG_WARN("'{}' is {} bytes, more than one buffer of this kind can hold ({}).", Path, Size, MaxCount);
            return false;
        }

        template <typename TBytes>
        bool ReadAllBytes(TBytes& OutData, FStringView Path)
        {
            OutData.Reset();

            FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
            if (!Handle.IsValid())
            {
                return false;
            }

            const int64 Size = Handle.Size();
            if (Size <= 0)
            {
                return true;
            }
            if (!FitsDestination(Size, static_cast<int64>(TBytes::MaxElements), Path))
            {
                return false;
            }

            OutData.SetNum(static_cast<typename TBytes::SizeType>(Size));

            if (Handle.Read(OutData.GetData(), Size) != Size)
            {
                OutData.Reset();
                return false;
            }

            return true;
        }

        int32 FindRootEnd(FStringView Path)
        {
            if (Path.Len() >= 2 && Path[1] == ':')
            {
                return 2;
            }

            if (Path.Len() >= 2 && Path[0] == '/' && Path[1] == '/')
            {
                const int32 Server = Path.Find('/', 2);
                if (Server == Constants::kIndexNone)
                {
                    return Path.Len();
                }

                const int32 Share = Path.Find('/', Server + 1);
                return Share == Constants::kIndexNone ? Path.Len() : Share;
            }

            return 0;
        }

        int32 FindLastSeparator(FStringView Path)
        {
            return Path.FindLastOf("/\\");
        }

        FString MakeTempSibling(FStringView Path)
        {
            FString Result(Path.GetData(), Path.Len());
            Result.Append(".tmp");
            return Result;
        }

        // Read-only marks a file someone means to keep as it is, so it is never replaced until made writable.
        bool RefuseReadOnlyTarget(FStringView Path)
        {
            if (!Stat(Path).IsReadOnly())
            {
                return false;
            }
            LOG_ERROR("{0} is read-only, so it was not replaced. Make it writable to save over it.", Path);
            return true;
        }
    }

    const char* ToString(EResult Result)
    {
        switch (Result)
        {
        case EResult::Success:       return "Success";
        case EResult::NotFound:      return "NotFound";
        case EResult::AccessDenied:  return "AccessDenied";
        case EResult::AlreadyExists: return "AlreadyExists";
        case EResult::NotEmpty:      return "NotEmpty";
        case EResult::InvalidPath:   return "InvalidPath";
        case EResult::IoError:       return "IoError";
        case EResult::OutOfSpace:    return "OutOfSpace";
        case EResult::Unsupported:   return "Unsupported";
        case EResult::Unknown:       return "Unknown";
        }

        return "Unknown";
    }

    bool MakeDirectoryTree(FStringView Path)
    {
        if (Path.IsEmpty())
        {
            return false;
        }

        if (IsDirectory(Path))
        {
            return true;
        }

        FString Buffer(Path.GetData(), Path.Len());

        for (char& Character : Buffer)
        {
            if (Character == '\\')
            {
                Character = '/';
            }
        }

        while (Buffer.Len() > 1 && Buffer.Last() == '/')
        {
            Buffer.LeftChopInline(1);
        }

        const int32 RootEnd = FindRootEnd(FStringView(Buffer.GetData(), Buffer.Len()));

        for (int32 Index = RootEnd + 1; Index <= Buffer.Len(); ++Index)
        {
            if (Index != Buffer.Len() && Buffer[Index] != '/')
            {
                continue;
            }

            const FStringView Prefix(Buffer.GetData(), Index);
            if (Prefix.Len() <= RootEnd || IsDirectory(Prefix))
            {
                continue;
            }

            if (!MakeDirectory(Prefix))
            {
                return false;
            }
        }

        return true;
    }

    bool MakeParentDirectoryTree(FStringView Path)
    {
        const int32 Separator = FindLastSeparator(Path);
        if (Separator == Constants::kIndexNone || Separator == 0)
        {
            return true;
        }

        return MakeDirectoryTree(Path.Mid(0, Separator));
    }

    bool RemoveTree(FStringView Path)
    {
        if (!Exists(Path))
        {
            return true;
        }

        if (!IsDirectory(Path))
        {
            return RemoveFile(Path);
        }

        TVector<FString> Directories;
        bool bSuccess = true;

        IterateDirectoryRecursive(Path, [&Directories, &bSuccess](const FDirectoryEntry& Entry)
        {
            if (Entry.IsDirectory())
            {
                Directories.Emplace(Entry.FullPath.GetData(), Entry.FullPath.Len());
            }
            else if (!RemoveFile(Entry.FullPath))
            {
                bSuccess = false;
            }
        });

        // Pre-order iteration lists a parent before its children, so unwinding backwards empties leaves first.
        for (auto It = Directories.rbegin(); It != Directories.rend(); ++It)
        {
            if (!DeleteDirectory(*It))
            {
                bSuccess = false;
            }
        }

        return DeleteDirectory(Path) && bSuccess;
    }

    bool CopyTree(FStringView From, FStringView To, uint32* OutFilesCopied)
    {
        if (!IsDirectory(From))
        {
            return false;
        }

        if (!MakeDirectoryTree(To))
        {
            return false;
        }

        const int32 SourceLength = From.Len();
        FString Target(To.GetData(), To.Len());
        while (Target.Len() > 1 && (Target.Last() == '/' || Target.Last() == '\\'))
        {
            Target.LeftChopInline(1);
        }

        bool bSuccess = true;
        uint32 Copied = 0;

        IterateDirectoryRecursive(From, [&](const FDirectoryEntry& Entry)
        {
            const FStringView Relative = Entry.FullPath.Mid(SourceLength);

            FString Destination = Target;
            Destination.Append(Relative.GetData(), Relative.Len());

            if (Entry.IsDirectory())
            {
                bSuccess = MakeDirectoryTree(Destination) && bSuccess;
                return;
            }

            if (Copy(Entry.FullPath, Destination, true))
            {
                ++Copied;
            }
            else
            {
                bSuccess = false;
            }
        });

        if (OutFilesCopied != nullptr)
        {
            *OutFilesCopied = Copied;
        }

        return bSuccess;
    }

    bool ReadFile(TVector<uint8>& OutData, FStringView Path)
    {
        return ReadAllBytes(OutData, Path);
    }

    bool ReadFile(TVector64<uint8>& OutData, FStringView Path)
    {
        return ReadAllBytes(OutData, Path);
    }

    bool ReadFile(FString& OutText, FStringView Path)
    {
        OutText.Reset();

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
        if (!Handle.IsValid())
        {
            return false;
        }

        const int64 Size = Handle.Size();
        if (Size <= 0)
        {
            return true;
        }
        if (!FitsDestination(Size, TNumericLimits<int32>::Max(), Path))
        {
            return false;
        }

        OutText.SetLen(static_cast<int32>(Size));

        if (Handle.Read(OutText.GetData(), Size) != Size)
        {
            OutText.Reset();
            return false;
        }

        return true;
    }

    bool ReadFileRange(TVector<uint8>& OutData, FStringView Path, int64 Offset, int64 Size)
    {
        OutData.Reset();

        if (Size <= 0 || Offset < 0)
        {
            return Size == 0;
        }

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Random);
        if (!Handle.IsValid())
        {
            return false;
        }

        const int64 FileLength = Handle.Size();
        if (Offset >= FileLength)
        {
            return true;
        }

        const int64 Available = FileLength - Offset;
        const int64 ToRead    = Size < Available ? Size : Available;
        if (!FitsDestination(ToRead, TVector<uint8>::MaxElements, Path))
        {
            return false;
        }

        OutData.SetNum(static_cast<int32>(ToRead));

        const int64 Got = Handle.ReadAt(OutData.GetData(), ToRead, Offset);
        if (Got != ToRead)
        {
            OutData.Reset();
            return false;
        }

        return true;
    }

    bool WriteFile(FStringView Path, TSpan64<const uint8> Data)
    {
        MakeParentDirectoryTree(Path);

        FFileHandle Handle = Open(Path, EAccess::Write, ECreateMode::CreateAlways, EShare::Read, EHint::Sequential);
        if (!Handle.IsValid())
        {
            return false;
        }

        if (Data.IsEmpty())
        {
            return true;
        }

        return Handle.Write(Data.GetData(), Data.Num()) == Data.Num();
    }

    bool AppendFile(FStringView Path, TSpan64<const uint8> Data)
    {
        MakeParentDirectoryTree(Path);

        FFileHandle Handle = Open(Path, EAccess::Write, ECreateMode::OpenAlways, EShare::Read, EHint::Sequential);
        if (!Handle.IsValid() || !Handle.Seek(0, ESeek::End))
        {
            return false;
        }

        if (Data.IsEmpty())
        {
            return true;
        }

        return Handle.Write(Data.GetData(), Data.Num()) == Data.Num();
    }

    bool AtomicWriteFile(FStringView Path, TSpan64<const uint8> Data)
    {
        if (Path.IsEmpty())
        {
            return false;
        }

        if (RefuseReadOnlyTarget(Path))
        {
            return false;
        }

        MakeParentDirectoryTree(Path);

        const FString TempPath = MakeTempSibling(Path);
        RemoveFile(TempPath);

        {
            FFileHandle Handle = Open(TempPath, EAccess::Write, ECreateMode::CreateAlways, EShare::None, EHint::Sequential);
            if (!Handle.IsValid())
            {
                LOG_ERROR("AtomicWriteFile: cannot open temporary output {0}: {1}", TempPath, ToString(GetLastResult()));
                return false;
            }

            if (!Data.IsEmpty() && Handle.Write(Data.GetData(), Data.Num()) != Data.Num())
            {
                Handle.Close();
                RemoveFile(TempPath);
                return false;
            }
        }

        if (!Move(TempPath, Path, true))
        {
            LOG_ERROR("AtomicWriteFile: rename of {0} to {1} failed: {2}", TempPath, Path, ToString(GetLastResult()));
            RemoveFile(TempPath);
            return false;
        }

        return true;
    }

    bool AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                FStringView SrcPath, int64 SrcOffset, int64 SrcSize,
                                TSpan<const uint8> Suffix)
    {
        if (Path.IsEmpty() || SrcPath.IsEmpty())
        {
            return false;
        }

        if (RefuseReadOnlyTarget(Path))
        {
            return false;
        }

        // Delete sharing is what lets the rename below replace a source file this splice is still reading.
        FFileHandle Source = Open(SrcPath, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
        if (!Source.IsValid())
        {
            LOG_ERROR("AtomicWriteFileSpliced: cannot open source {0}: {1}", SrcPath, ToString(GetLastResult()));
            return false;
        }

        MakeParentDirectoryTree(Path);

        const FString TempPath = MakeTempSibling(Path);
        RemoveFile(TempPath);

        const char* Failure = nullptr;

        {
            FFileHandle Target = Open(TempPath, EAccess::Write, ECreateMode::CreateAlways, EShare::None, EHint::Sequential);
            if (!Target.IsValid())
            {
                Failure = "could not open the temporary output";
            }

            if (Failure == nullptr && !Prefix.IsEmpty() && Target.Write(Prefix.GetData(), Prefix.Num()) != Prefix.Num())
            {
                Failure = "write failed on the prefix";
            }

            TVector<uint8> Chunk;
            Chunk.SetNum(static_cast<int32>(kSpliceChunkBytes));

            int64 Remaining = SrcSize;
            int64 Cursor    = SrcOffset;

            while (Failure == nullptr && Remaining > 0)
            {
                const int64 Want = Remaining < kSpliceChunkBytes ? Remaining : kSpliceChunkBytes;

                if (Source.ReadAt(Chunk.GetData(), Want, Cursor) != Want)
                {
                    Failure = "source ended early; the bulk region is shorter than its trailer claims";
                    break;
                }

                if (Target.Write(Chunk.GetData(), Want) != Want)
                {
                    Failure = "write failed while splicing";
                    break;
                }

                Cursor    += Want;
                Remaining -= Want;
            }

            if (Failure == nullptr && !Suffix.IsEmpty() && Target.Write(Suffix.GetData(), Suffix.Num()) != Suffix.Num())
            {
                Failure = "write failed on the suffix";
            }
        }

        Source.Close();

        if (Failure != nullptr)
        {
            LOG_ERROR("AtomicWriteFileSpliced: {0}", Failure);
            RemoveFile(TempPath);
            return false;
        }

        if (!Move(TempPath, Path, true))
        {
            LOG_ERROR("AtomicWriteFileSpliced: rename of {0} to {1} failed: {2}", TempPath, Path, ToString(GetLastResult()));
            RemoveFile(TempPath);
            return false;
        }

        return true;
    }
}
