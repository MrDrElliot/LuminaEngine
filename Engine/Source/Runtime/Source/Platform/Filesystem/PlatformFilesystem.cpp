#include "RuntimePCH.h"
#include "PlatformFilesystem.h"

#include "Log/Log.h"

namespace Lumina::Filesystem
{
    namespace
    {
        constexpr uint64 kSpliceChunkBytes = 1ull << 20;

        size_t FindRootEnd(FStringView Path)
        {
            if (Path.size() >= 2 && Path[1] == ':')
            {
                return 2;
            }

            if (Path.size() >= 2 && Path[0] == '/' && Path[1] == '/')
            {
                const size_t Server = Path.find('/', 2);
                if (Server == FStringView::npos)
                {
                    return Path.size();
                }

                const size_t Share = Path.find('/', Server + 1);
                return Share == FStringView::npos ? Path.size() : Share;
            }

            return 0;
        }

        size_t FindLastSeparator(FStringView Path)
        {
            return Path.find_last_of("/\\");
        }

        FString MakeTempSibling(FStringView Path)
        {
            FString Result(Path.data(), Path.size());
            Result.append(".tmp");
            return Result;
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
        if (Path.empty())
        {
            return false;
        }

        if (IsDirectory(Path))
        {
            return true;
        }

        FString Buffer(Path.data(), Path.size());

        for (char& Character : Buffer)
        {
            if (Character == '\\')
            {
                Character = '/';
            }
        }

        while (Buffer.size() > 1 && Buffer.back() == '/')
        {
            Buffer.pop_back();
        }

        const size_t RootEnd = FindRootEnd(FStringView(Buffer.data(), Buffer.size()));

        for (size_t Index = RootEnd + 1; Index <= Buffer.size(); ++Index)
        {
            if (Index != Buffer.size() && Buffer[Index] != '/')
            {
                continue;
            }

            const FStringView Prefix(Buffer.data(), Index);
            if (Prefix.size() <= RootEnd || IsDirectory(Prefix))
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
        const size_t Separator = FindLastSeparator(Path);
        if (Separator == FStringView::npos || Separator == 0)
        {
            return true;
        }

        return MakeDirectoryTree(Path.substr(0, Separator));
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
                Directories.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
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

        const size_t SourceLength = From.size();
        FString Target(To.data(), To.size());
        while (Target.size() > 1 && (Target.back() == '/' || Target.back() == '\\'))
        {
            Target.pop_back();
        }

        bool bSuccess = true;
        uint32 Copied = 0;

        IterateDirectoryRecursive(From, [&](const FDirectoryEntry& Entry)
        {
            const FStringView Relative = Entry.FullPath.substr(SourceLength);

            FString Destination = Target;
            Destination.append(Relative.data(), Relative.size());

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
        OutData.clear();

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
        if (!Handle.IsValid())
        {
            return false;
        }

        const uint64 Size = Handle.Size();
        if (Size == 0)
        {
            return true;
        }

        OutData.resize(static_cast<size_t>(Size));

        if (Handle.Read(OutData.data(), Size) != Size)
        {
            OutData.clear();
            return false;
        }

        return true;
    }

    bool ReadFile(FString& OutText, FStringView Path)
    {
        OutText.clear();

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
        if (!Handle.IsValid())
        {
            return false;
        }

        const uint64 Size = Handle.Size();
        if (Size == 0)
        {
            return true;
        }

        OutText.resize(static_cast<size_t>(Size));

        if (Handle.Read(OutText.data(), Size) != Size)
        {
            OutText.clear();
            return false;
        }

        return true;
    }

    bool ReadFileRange(TVector<uint8>& OutData, FStringView Path, uint64 Offset, uint64 Size)
    {
        OutData.clear();

        if (Size == 0)
        {
            return true;
        }

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Random);
        if (!Handle.IsValid())
        {
            return false;
        }

        const uint64 FileLength = Handle.Size();
        if (Offset >= FileLength)
        {
            return true;
        }

        const uint64 Available = FileLength - Offset;
        const uint64 ToRead    = Size < Available ? Size : Available;

        OutData.resize(static_cast<size_t>(ToRead));

        const uint64 Got = Handle.ReadAt(OutData.data(), ToRead, Offset);
        if (Got != ToRead)
        {
            OutData.clear();
            return false;
        }

        return true;
    }

    bool WriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        MakeParentDirectoryTree(Path);

        FFileHandle Handle = Open(Path, EAccess::Write, ECreateMode::CreateAlways, EShare::Read, EHint::Sequential);
        if (!Handle.IsValid())
        {
            return false;
        }

        if (Data.empty())
        {
            return true;
        }

        return Handle.Write(Data.data(), Data.size()) == Data.size();
    }

    bool AppendFile(FStringView Path, TSpan<const uint8> Data)
    {
        MakeParentDirectoryTree(Path);

        FFileHandle Handle = Open(Path, EAccess::Write, ECreateMode::OpenAlways, EShare::Read, EHint::Sequential);
        if (!Handle.IsValid() || !Handle.Seek(0, ESeek::End))
        {
            return false;
        }

        if (Data.empty())
        {
            return true;
        }

        return Handle.Write(Data.data(), Data.size()) == Data.size();
    }

    bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        if (Path.empty())
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

            if (!Data.empty() && Handle.Write(Data.data(), Data.size()) != Data.size())
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
                                FStringView SrcPath, uint64 SrcOffset, uint64 SrcSize,
                                TSpan<const uint8> Suffix)
    {
        if (Path.empty() || SrcPath.empty())
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

            if (Failure == nullptr && !Prefix.empty() && Target.Write(Prefix.data(), Prefix.size()) != Prefix.size())
            {
                Failure = "write failed on the prefix";
            }

            TVector<uint8> Chunk;
            Chunk.resize(static_cast<size_t>(kSpliceChunkBytes));

            uint64 Remaining = SrcSize;
            uint64 Cursor    = SrcOffset;

            while (Failure == nullptr && Remaining > 0)
            {
                const uint64 Want = Remaining < kSpliceChunkBytes ? Remaining : kSpliceChunkBytes;

                if (Source.ReadAt(Chunk.data(), Want, Cursor) != Want)
                {
                    Failure = "source ended early; the bulk region is shorter than its trailer claims";
                    break;
                }

                if (Target.Write(Chunk.data(), Want) != Want)
                {
                    Failure = "write failed while splicing";
                    break;
                }

                Cursor    += Want;
                Remaining -= Want;
            }

            if (Failure == nullptr && !Suffix.empty() && Target.Write(Suffix.data(), Suffix.size()) != Suffix.size())
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
