#include "RuntimePCH.h"
#include "MemoryFileSystem.h"

#include <chrono>

#include "FileInfo.h"
#include "Paths/Paths.h"
#include "VfsPathUtils.h"

namespace Lumina::VFS
{
    namespace
    {
        int64 NowAsNanos()
        {
            auto Now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(Now.time_since_epoch()).count();
        }

    }

    FMemoryFileSystem::FMemoryFileSystem(const FFixedString& InAliasPath)
        : AliasPath(Paths::Normalize(InAliasPath))
    {
    }

    FFixedString FMemoryFileSystem::NormalizeKey(FStringView Path) const
    {
        FFixedString Key(Path.data(), Path.size());
        Paths::Normalize(Key);

        // Strip a single trailing slash so "/A/" and "/A" map to the same key.
        if (Key.size() > 1 && Key.back() == '/')
        {
            Key.pop_back();
        }

        return Key;
    }

    void FMemoryFileSystem::EnsureDirectoryChain(FStringView VirtualPath)
    {
        FStringView Parent = ParentOf(VirtualPath);
        while (!Parent.empty() && Parent != "/")
        {
            FFixedString Key(Parent.data(), Parent.size());

            auto It = Entries.find(Key);
            if (It == Entries.end())
            {
                FEntry Entry;
                Entry.bIsDirectory = true;
                Entry.LastModifyTime = NowAsNanos();
                Entries.emplace(Move(Key), Move(Entry));
            }
            else if (!It->second.bIsDirectory)
            {
                // A file already exists at this path, refuse to convert it; bail out
                // rather than silently corrupting the chain.
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

        auto It = Entries.find(Key);
        if (It == Entries.end())
        {
            FEntry Entry;
            Entry.bIsDirectory = true;
            Entry.LastModifyTime = NowAsNanos();
            Entries.emplace(Move(Key), Move(Entry));
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
        Entries.clear();
    }

    bool FMemoryFileSystem::ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        FFixedString Key = NormalizeKey(Path);

        auto It = Entries.find(Key);
        if (It == Entries.end() || It->second.bIsDirectory)
        {
            return false;
        }

        Result = It->second.Data;
        return true;
    }

    bool FMemoryFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        FFixedString Key = NormalizeKey(Path);

        auto It = Entries.find(Key);
        if (It == Entries.end() || It->second.bIsDirectory)
        {
            return false;
        }

        const TVector<uint8>& Data = It->second.Data;
        OutString.assign(reinterpret_cast<const char*>(Data.data()), Data.size());
        return true;
    }

    bool FMemoryFileSystem::WriteFile(FStringView Path, FStringView Data)
    {
        if (bReadOnly)
        {
            return false;
        }

        TVector<uint8> Bytes(Data.begin(), Data.end());
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
        return Entries.find(Key) != Entries.end();
    }

    bool FMemoryFileSystem::IsDirectory(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.find(Key);
        return It != Entries.end() && It->second.bIsDirectory;
    }

    bool FMemoryFileSystem::IsEmpty(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.find(Key);
        if (It == Entries.end())
        {
            return true;
        }

        if (!It->second.bIsDirectory)
        {
            return It->second.Data.empty();
        }

        FStringView Prefix(Key.data(), Key.size());
        for (const auto& [OtherKey, Entry] : Entries)
        {
            if (OtherKey == Key)
            {
                continue;
            }

            FStringView OtherView(OtherKey.data(), OtherKey.size());
            if (OtherView.size() > Prefix.size() &&
                OtherView.starts_with(Prefix) &&
                OtherView[Prefix.size()] == '/')
            {
                return false;
            }
        }
        return true;
    }

    size_t FMemoryFileSystem::Size(FStringView Path) const
    {
        FFixedString Key = NormalizeKey(Path);
        auto It = Entries.find(Key);
        if (It == Entries.end() || It->second.bIsDirectory)
        {
            return 0;
        }
        return It->second.Data.size();
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
        return Entries.erase(Key) > 0;
    }

    bool FMemoryFileSystem::RemoveAll(FStringView Path)
    {
        if (bReadOnly)
        {
            return false;
        }

        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.data(), Key.size());

        bool bRemovedAny = false;
        for (auto It = Entries.begin(); It != Entries.end(); )
        {
            FStringView OtherView(It->first.data(), It->first.size());
            const bool bMatches =
                OtherView == Prefix ||
                (OtherView.size() > Prefix.size() &&
                 OtherView.starts_with(Prefix) &&
                 OtherView[Prefix.size()] == '/');

            if (bMatches)
            {
                It = Entries.erase(It);
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

        auto It = Entries.find(OldKey);
        if (It == Entries.end())
        {
            return false;
        }

        FEntry Entry = Move(It->second);
        Entries.erase(It);

        EnsureDirectoryChain(NewKey);
        Entries[NewKey] = Move(Entry);
        return true;
    }

    void FMemoryFileSystem::PlatformOpen(FStringView /*Path*/) const
    {
        // No-op: in-memory entries have no shell representation.
    }

    void FMemoryFileSystem::EmitFileInfo(const FFixedString& Key, const FEntry& Entry, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        FStringView KeyView(Key.data(), Key.size());

        FFileInfo Info
        {
            .Name           = FString(FileNameOf(KeyView).data(), FileNameOf(KeyView).size()),
            .VirtualPath    = Key,
            .PathSource     = Key,
            .LastModifyTime = Entry.LastModifyTime,
            .Flags          = FlagsForPath(KeyView, Entry.bIsDirectory, false),
        };

        Callback(Info);
    }

    void FMemoryFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.data(), Key.size());

        for (const auto& [OtherKey, Entry] : Entries)
        {
            FStringView OtherView(OtherKey.data(), OtherKey.size());
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

    void FMemoryFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        FFixedString Key = NormalizeKey(Path);
        FStringView Prefix(Key.data(), Key.size());

        for (const auto& [OtherKey, Entry] : Entries)
        {
            FStringView OtherView(OtherKey.data(), OtherKey.size());
            if (OtherView == Prefix)
            {
                continue;
            }

            if (OtherView.size() > Prefix.size() &&
                OtherView.starts_with(Prefix) &&
                OtherView[Prefix.size()] == '/')
            {
                EmitFileInfo(OtherKey, Entry, Callback);
            }
        }
    }
}
