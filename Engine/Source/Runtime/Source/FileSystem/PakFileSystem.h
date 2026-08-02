#pragma once
#include "IFileSystem.h"
#include "Pak/PakArchive.h"

namespace Lumina::VFS
{
    // Read-only IFileSystem over a shared FPakArchive (multiple aliases can mount one .pak).
    // Mutators are no-ops returning false; IsReadOnly() returns true.
    class RUNTIME_API FPakFileSystem : public IFileSystem
    {
    public:

        FPakFileSystem(const FFixedString& InAliasPath, TSharedPtr<FPakArchive> InArchive);

        bool ReadFile(TVector<uint8>& Result, FStringView Path) override;
        bool ReadFile(FString& OutString, FStringView Path) override;

        // Read-only, these always return false.
        bool WriteFile(FStringView Path, FStringView Data) override            { return false; }
        bool WriteFile(FStringView Path, TSpan<const uint8> Data) override     { return false; }
        bool CreateDir(FStringView Path) override                              { return false; }
        bool Remove(FStringView Path) override                                 { return false; }
        bool RemoveAll(FStringView Path) override                              { return false; }
        bool Rename(FStringView Old, FStringView New) override                 { return false; }

        bool Exists(FStringView Path) const override;
        bool IsDirectory(FStringView Path) const override;
        bool IsEmpty(FStringView Path) const override;
        size_t Size(FStringView Path) const override;

        void PlatformOpen(FStringView Path) const override                     {}

        void DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const override;
        void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const override;

        FStringView GetAliasPath() const override { return AliasPath; }
        FStringView GetBasePath() const override  { return AliasPath; }

        bool IsReadOnly() const override { return true; }

        const FPakArchive* GetArchive() const { return Archive.get(); }

    private:

        FFixedString                AliasPath;
        TSharedPtr<FPakArchive>     Archive;
    };
}
