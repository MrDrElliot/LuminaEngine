#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "MemoryFileSystem.h"


#include "FileInfo.h"
#include "FileSystem.h"
#include "Paths/Paths.h"
#include "VfsPathUtils.h"

namespace Lumina::VFS
{
    namespace
    {
        int64 NowAsNanos()
        {
            return PlatformTime::UtcNanoseconds();
        }

    }

    FMemoryFileSystem::FMemoryFileSystem(const FFixedString& InAliasPath)
        : AliasPath(Paths::Normalize(InAliasPath))
    {
    }

    FFixedString FMemoryFileSystem::NormalizeKey(FStringView Path) const
    {
        FFixedString Key(Path.GetData(), Path.Len());
        Paths::Normalize(Key);

        // So "/A/" and "/A" map to the same key.
        Key.SetLen(Paths::TrimTrailingSeparators(Key).Len());

        return Key;
    }

    void FMemoryFileSystem::EnsureDirectoryChain(FStringView VirtualPath)
    {
        FStringView Parent = ParentOf(VirtualPath);
        while (!Parent.IsEmpty() && Parent != "/")
        {
            FFixedString Key(Parent.GetData(), Parent.Len());

            auto It = Entries.Find(Key);
            if (It == nullptr)
            {
                FEntry Entry;
                Entry.bIsDirectory = true;
                Entry.LastModifyTime = NowAsNanos();
                Entries.TryAdd(Move(Key), Move(Entry));
            }
            else if (!It->bIsDirectory)
            {
                // A file already exists here, so refuse rather than silently corrupt the chain.
                break;
            }

            Parent = ParentOf(Parent);
        }
    }

    void FMemoryFileSystem::AddFile(FStringView VirtualPath, TVector<uint8>&& Data)
    {
        FFixedString Key = NormalizeKey(VirtualPath);

        FEntry Entry;
        Entry.Data = Move(Data);
        Entry.LastModifyTime = NowAsNanos();
        Entry.bIsDirectory = false;

        EnsureDirectoryChain(Key);
        Entries[Key] = Move(Entry);
    }

    void FMemoryFileSystem::AddFile(FStringView VirtualPath, TSpan<const uint8> Data)
    {
        TVector<uint8> Copy(Data.begin(), Data.end());
        AddFile(VirtualPath, Move(Copy));
    }

    void FMemoryFileSystem::AddDirectory(FStringView VirtualPath)
    {
        FFixedString Key = NormalizeKey(VirtualPath);

        EnsureDirectoryChain(Key);

        auto It = Entries.Find(Key);
        if (It == nullptr)
        {
            FEntry Entry;
            Entry.bIsDirectory = true;
            Entry.LastModifyTime = NowAsNanos();
            Entries.TryAdd(Move(Key), Move(Entry));
        }
    }

    size_t FMemoryFileSystem::GetNumFiles() const
    {
        size_t Count = 0;
        for (const auto& [Key, Entry] : Entries)
        {
            if (!Entry.bIsDirectory)
            {
                ++Count;
            }
        }
        return Count;
    }

    void FMemoryFileSystem::Clear()
    {
        Entries.Reset();
    }

    bool FMemoryFileSystem::ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        FFixedString Key = NormalizeKey(Path);

        auto It = Entries.Find(Key);
        if (It == nullptr || It->bIsDirectory)
        {
            return false;
        }

        Result = It->Data;
        return true;
    }

    bool FMemoryFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        FFixedString Key = NormalizeKey(Path);

        auto It = Entries.Find(Key);
        if (It == nullptr || It->bIsDirectory)
        {
            return false;
        }

        const TVector<uint8>& Data = It->Data;
        OutString = FStringView(reinterpret_cast<const char*>(Data.GetData()), Data.Num());
        return true;
    }

    bool FMemoryFileSystem::WriteFile(FStringView Path, FStringView Data)
    {
        if (bReadOnly)
        {
            return false;
        }

        TVector<uint8> Bytes(Data.GetData(), Data.GetData() + Data.Len());
        AddFile(Path, Move(Bytes));
        return true;
    }

    bool FMemoryFileSystem::WriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        if (bReadOnly)
        {
            return false;
        }

        AddFile(Path, Data);
        return true;
    }

    bool FMemoryFileSystem::Exists(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        return Entries.Contains(Key);
    }

    bool FMemoryFileSystem::IsDirectory(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.Find(Key);
        return It != nullptr && It->bIsDirectory;
    }

    bool FMemoryFileSystem::IsEmpty(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.Find(Key);
        if (It == nullptr)
        {
            return true;
        }

        if (!It->bIsDirectory)
        {
            return It->Data.IsEmpty();
        }

        FStringView Prefix(Key.GetData(), Key.Len());
        for (const auto& [OtherKey, Entry] : Entries)
        {
            if (OtherKey == Key)
            {
                continue;
            }

            FStringView OtherView(OtherKey.GetData(), OtherKey.Len());
            if (IsUnderDirectory(Prefix, OtherView))
            {
                return false;
            }
        }
        return true;
    }

    int64 FMemoryFileSystem::Size(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.Find(Key);
        if (It == nullptr || It->bIsDirectory)
        {
            return 0;
        }
        return It->Data.Num();
    }

    bool FMemoryFileSystem::CreateDir(FStringView Path)
    {
        if (bReadOnly)
        {
            return false;
        }

        AddDirectory(Path);
        return true;
    }

    bool FMemoryFileSystem::Remove(FStringView Path)
    {
        if (bReadOnly)
        {
            return false;
        }

        FFixedString Key = NormalizeKey(Path);
        return Entries.Remove(Key) > 0;
    }

    bool FMemoryFileSystem::RemoveAll(FStringView Path)
    {
        if (bReadOnly)
        {
            return false;
        }

        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.GetData(), Key.Len());

        bool bRemovedAny = false;
        for (auto It = Entries.begin(); It != Entries.end(); )
        {
            FStringView OtherView(It->Key.GetData(), It->Key.Len());
            if (IsUnderDirectory(Prefix, OtherView))
            {
                It = Entries.Remove(It);
                bRemovedAny = true;
            }
            else
            {
                ++It;
            }
        }
        return bRemovedAny;
    }

    bool FMemoryFileSystem::Rename(FStringView Old, FStringView New)
    {
        if (bReadOnly)
        {
            return false;
        }

        FFixedString OldKey = NormalizeKey(Old);
        FFixedString NewKey = NormalizeKey(New);

        if (Entries.Find(OldKey) == nullptr)
        {
            return false;
        }

        FStringView Prefix(OldKey.GetData(), OldKey.Len());

        TVector<FFixedString> KeysToMove;
        for (const auto& Pair : Entries)
        {
            if (IsUnderDirectory(Prefix, FStringView(Pair.Key.GetData(), Pair.Key.Len())))
            {
                KeysToMove.Add(Pair.Key);
            }
        }

        EnsureDirectoryChain(NewKey);

        for (const FFixedString& Key : KeysToMove)
        {
            FEntry Entry = Move(*Entries.Find(Key));
            Entries.Remove(Key);

            FFixedString RemappedKey = NewKey + Key.Mid(OldKey.Len());
            Entries[RemappedKey] = Move(Entry);
        }

        return true;
    }

    void FMemoryFileSystem::PlatformOpen(FStringView /*Path*/) const
    {
        // A no-op, since in-memory entries have no shell representation.
    }

    void FMemoryFileSystem::EmitFileInfo(const FFixedString& Key, const FEntry& Entry, const TFunction<void(FFileInfo&)>& Callback) const
    {
        FStringView KeyView(Key.GetData(), Key.Len());

        FFileInfo Info
        {
            .Name           = FString(FileNameOf(KeyView).GetData(), FileNameOf(KeyView).Len()),
            .VirtualPath    = FString(*Key, Key.Len()),
            .PathSource     = FString(*Key, Key.Len()),
            .LastModifyTime = Entry.LastModifyTime,
            .Size           = Entry.bIsDirectory ? 0 : (uint64)Entry.Data.Num(),
            .Flags          = FlagsForPath(KeyView, Entry.bIsDirectory, false),
        };

        Callback(Info);
    }

    void FMemoryFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const
    {
        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.GetData(), Key.Len());

        for (const auto& [OtherKey, Entry] : Entries)
        {
            FStringView OtherView(OtherKey.GetData(), OtherKey.Len());
            if (OtherView == Prefix)
            {
                continue;
            }

            if (ParentOf(OtherView) == Prefix)
            {
                EmitFileInfo(OtherKey, Entry, Callback);
            }
        }
    }

    void FMemoryFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const
    {
        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.GetData(), Key.Len());

        for (const auto& [OtherKey, Entry] : Entries)
        {
            FStringView OtherView(OtherKey.GetData(), OtherKey.Len());
            if (OtherView == Prefix)
            {
                continue;
            }

            if (IsUnderDirectory(Prefix, OtherView))
            {
                EmitFileInfo(OtherKey, Entry, Callback);
            }
        }
    }
}
