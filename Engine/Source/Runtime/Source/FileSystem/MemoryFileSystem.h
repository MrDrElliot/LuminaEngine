#pragma once
#include "IFileSystem.h"

namespace Lumina::VFS
{
    // In-memory IFileSystem; keys are full virtual paths (with alias prefix).
    // Not synchronized: populate before exposing the mount, or guard mutations with the caller's lock.
    class RUNTIME_API FMemoryFileSystem : public IFileSystem
    {
    public:

        explicit FMemoryFileSystem(const FFixedString& InAliasPath);

        // Population API. The PAK loader populates here, then calls SetReadOnly(true).
        void AddFile(FStringView VirtualPath, TVector<uint8>&& Data);
        void AddFile(FStringView VirtualPath, TSpan<const uint8> Data);
        void AddDirectory(FStringView VirtualPath);

        size_t GetNumFiles() const;
        void Clear();

        void SetReadOnly(bool bInReadOnly) { bReadOnly = bInReadOnly; }
        bool IsReadOnly() const override { return bReadOnly; }

        // IFileSystem
        bool ReadFile(TVector<uint8>& Result, FStringView Path) override;
        bool ReadFile(FString& OutString, FStringView Path) override;

        bool WriteFile(FStringView Path, FStringView Data) override;
        bool WriteFile(FStringView Path, TSpan<const uint8> Data) override;

        bool Exists(FStringView Path) const override;
        bool IsDirectory(FStringView Path) const override;
        bool IsEmpty(FStringView Path) const override;
        size_t Size(FStringView Path) const override;

        bool CreateDir(FStringView Path) override;
        bool Remove(FStringView Path) override;
        bool RemoveAll(FStringView Path) override;
        bool Rename(FStringView Old, FStringView New) override;

        void PlatformOpen(FStringView Path) const override;

        void DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const override;
        void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const override;

        FStringView GetAliasPath() const override { return AliasPath; }
        FStringView GetBasePath() const override { return AliasPath; }

    private:

        struct FEntry
        {
            TVector<uint8>  Data;
            int64           LastModifyTime = 0;
            bool            bIsDirectory = false;
        };

        FFixedString NormalizeKey(FStringView Path) const;
        void EnsureDirectoryChain(FStringView VirtualPath);
        void EmitFileInfo(const FFixedString& Key, const FEntry& Entry, const TFunction<void(const FFileInfo&)>& Callback) const;

        FFixedString                        AliasPath;
        THashMap<FFixedString, FEntry>      Entries;
        bool                                bReadOnly = false;
    };
}
