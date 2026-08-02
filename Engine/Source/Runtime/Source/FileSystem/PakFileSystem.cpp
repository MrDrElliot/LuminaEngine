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
        if (Data.empty() && !Archive->HasEntry(Path))
        {
            return false;
        }
        Result.assign(Data.begin(), Data.end());
        return true;
    }

    bool FPakFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        if (!Archive)
        {
            return false;
        }
        TSpan<const uint8> Data = Archive->ReadEntry(Path);
        if (Data.empty() && !Archive->HasEntry(Path))
        {
            return false;
        }
        OutString.assign(reinterpret_cast<const char*>(Data.data()), Data.size());
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
        // A "directory" exists if any entry is under it. The archive has no
        // explicit directory records, so we infer from the entry list.
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

    size_t FPakFileSystem::Size(FStringView Path) const
    {
        return Archive ? Archive->EntrySize(Path) : 0;
    }

    void FPakFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        if (!Archive)
        {
            return;
        }

        // Track which immediate-child *directories* we've already emitted so
        // a directory containing many files only shows up once.
        THashSet<FFixedString> EmittedDirs;

        Archive->ForEachEntryUnder(Path, [&](FStringView EntryPath, size_t Bytes)
        {
            // Only emit immediate children of `Path`, entries deeper than
            // one level become a synthetic directory entry instead.
            FStringView Suffix = EntryPath.substr(Path.size() + 1); // skip leading "/"
            const size_t NextSlash = Suffix.find('/');

            if (NextSlash == FStringView::npos)
            {
                // Direct file child.
                FFileInfo Info
                {
                    .Name           = FString(FileNameOf(EntryPath).data(), FileNameOf(EntryPath).size()),
                    .VirtualPath    = FFixedString(EntryPath.data(), EntryPath.size()),
                    .PathSource     = FFixedString(EntryPath.data(), EntryPath.size()),
                    .LastModifyTime = 0,
                    .Flags          = FlagsForPath(EntryPath, false, true),
                };
                Callback(Info);
            }
            else
            {
                // Synthetic directory.
                FStringView DirRel = Suffix.substr(0, NextSlash);
                FFixedString DirFullPath(Path.data(), Path.size());
                DirFullPath.append("/");
                DirFullPath.append(DirRel.data(), DirRel.size());

                if (EmittedDirs.insert(DirFullPath).second)
                {
                    FFileInfo Info
                    {
                        .Name           = FString(DirRel.data(), DirRel.size()),
                        .VirtualPath    = DirFullPath,
                        .PathSource     = DirFullPath,
                        .LastModifyTime = 0,
                        .Flags          = FlagsForPath(FStringView(DirFullPath.data(), DirFullPath.size()), true, true),
                    };
                    Callback(Info);
                }
            }
        });
    }

    void FPakFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        if (!Archive)
        {
            return;
        }
        Archive->ForEachEntryUnder(Path, [&](FStringView EntryPath, size_t)
        {
            FFileInfo Info
            {
                .Name           = FString(FileNameOf(EntryPath).data(), FileNameOf(EntryPath).size()),
                .VirtualPath    = FFixedString(EntryPath.data(), EntryPath.size()),
                .PathSource     = FFixedString(EntryPath.data(), EntryPath.size()),
                .LastModifyTime = 0,
                .Flags          = FlagsForPath(EntryPath, false, true),
            };
            Callback(Info);
        });
    }
}
