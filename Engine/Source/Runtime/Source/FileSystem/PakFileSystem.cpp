#include "RuntimePCH.h"
#include "PakFileSystem.h"

#include <cstring>

#include "FileInfo.h"
#include "Paths/Paths.h"
#include "VfsPathUtils.h"

namespace Lumina::VFS
{
    FPakFileSystem::FPakFileSystem(const FFixedString& InAliasPath, TSharedPtr<FPakArchive> InArchive)
        : AliasPath(Paths::Normalize(InAliasPath))
        , Archive(Move(InArchive))
    {
    }

    bool FPakFileSystem::ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        if (!Archive)
        {
            return false;
        }
        TSpan<const uint8> Data = Archive->ReadEntry(Path);
        if (Data.IsEmpty() && !Archive->HasEntry(Path))
        {
            return false;
        }
        Result.Reset();
        Result.Append(Data.begin(), Data.end());
        return true;
    }

    // The archive already holds the whole pak, so a ranged read avoids copying the whole entry.
    bool FPakFileSystem::ReadFileRange(TVector<uint8>& Result, FStringView Path, int64 Offset, int64 Size)
    {
        Result.Reset();

        if (!Archive)
        {
            return false;
        }

        TSpan<const uint8> Data = Archive->ReadEntry(Path);
        if (Data.IsEmpty() && !Archive->HasEntry(Path))
        {
            return false;
        }

        Result.Reset();
        if (Offset < 0 || Size <= 0 || Offset >= Data.Num())
        {
            return true;
        }

        const int64 Remaining = Data.Num() - Offset;
        const int64 ToRead    = Size < Remaining ? Size : Remaining;
        Result.Append(Data.GetData() + Offset, static_cast<int32>(ToRead));
        return true;
    }

    bool FPakFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        if (!Archive)
        {
            return false;
        }
        TSpan<const uint8> Data = Archive->ReadEntry(Path);
        if (Data.IsEmpty() && !Archive->HasEntry(Path))
        {
            return false;
        }
        OutString = FStringView(reinterpret_cast<const char*>(Data.GetData()), Data.Num());
        return true;
    }

    bool FPakFileSystem::Exists(FStringView Path) const
    {
        if (!Archive)
        {
            return false;
        }
        if (Archive->HasEntry(Path))
        {
            return true;
        }
        // The archive has no explicit directory records, so a directory is inferred from the entry list.
        bool bFound = false;
        Archive->ForEachEntryUnder(Path, [&](FStringView, size_t) { bFound = true; });
        return bFound;
    }

    bool FPakFileSystem::IsDirectory(FStringView Path) const
    {
        if (!Archive)
        {
            return false;
        }
        // PAKs only contain files. Anything that has children-but-not-itself is a dir.
        if (Archive->HasEntry(Path))
        {
            return false;
        }
        bool bFound = false;
        Archive->ForEachEntryUnder(Path, [&](FStringView, size_t) { bFound = true; });
        return bFound;
    }

    bool FPakFileSystem::IsEmpty(FStringView Path) const
    {
        if (!Archive)
        {
            return true;
        }
        if (Archive->HasEntry(Path))
        {
            return Archive->EntrySize(Path) == 0;
        }
        bool bAny = false;
        Archive->ForEachEntryUnder(Path, [&](FStringView, size_t) { bAny = true; });
        return !bAny;
    }

    int64 FPakFileSystem::Size(FStringView Path) const
    {
        return Archive ? Archive->EntrySize(Path) : 0;
    }

    void FPakFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const
    {
        if (!Archive)
        {
            return;
        }

        // So a directory containing many files only shows up once.
        THashSet<FFixedString> EmittedDirs;

        Archive->ForEachEntryUnder(Path, [&](FStringView EntryPath, size_t Bytes)
        {
            // Anything deeper than one level becomes a synthetic directory entry instead.
            FStringView Suffix = EntryPath.Mid(Path.Len() + 1); // skip leading "/"
            const int32 NextSlash = Suffix.Find('/');

            if (NextSlash == Constants::kIndexNone)
            {
                // Direct file child.
                FFileInfo Info
                {
                    .Name           = FString(FileNameOf(EntryPath).GetData(), FileNameOf(EntryPath).Len()),
                    .VirtualPath    = FString(EntryPath.GetData(), EntryPath.Len()),
                    .PathSource     = FString(EntryPath.GetData(), EntryPath.Len()),
                    .LastModifyTime = 0,
                    .Flags          = FlagsForPath(EntryPath, false, true),
                };
                Callback(Info);
            }
            else
            {
                // Synthetic directory.
                FStringView DirRel = Suffix.Mid(0, NextSlash);
                FFixedString DirFullPath(Path.GetData(), Path.Len());
                DirFullPath.Append("/");
                DirFullPath.Append(DirRel.GetData(), DirRel.Len());

                if (EmittedDirs.TryAdd(DirFullPath))
                {
                    FFileInfo Info
                    {
                        .Name           = FString(DirRel.GetData(), DirRel.Len()),
                        .VirtualPath    = FString(*DirFullPath, DirFullPath.Len()),
                        .PathSource     = FString(*DirFullPath, DirFullPath.Len()),
                        .LastModifyTime = 0,
                        .Flags          = FlagsForPath(FStringView(DirFullPath.GetData(), DirFullPath.Len()), true, true),
                    };
                    Callback(Info);
                }
            }
        });
    }

    void FPakFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const
    {
        if (!Archive)
        {
            return;
        }
        Archive->ForEachEntryUnder(Path, [&](FStringView EntryPath, size_t)
        {
            FFileInfo Info
            {
                .Name           = FString(FileNameOf(EntryPath).GetData(), FileNameOf(EntryPath).Len()),
                .VirtualPath    = FString(EntryPath.GetData(), EntryPath.Len()),
                .PathSource     = FString(EntryPath.GetData(), EntryPath.Len()),
                .LastModifyTime = 0,
                .Flags          = FlagsForPath(EntryPath, false, true),
            };
            Callback(Info);
        });
    }
}
