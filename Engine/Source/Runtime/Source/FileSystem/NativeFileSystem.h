#pragma once
#include "IFileSystem.h"

namespace Lumina::VFS
{
    class RUNTIME_API FNativeFileSystem : public IFileSystem
    {
    public:
        FNativeFileSystem(const FFixedString& InAliasPath, FStringView InBasePath);

        FFixedString ResolveVirtualPath(FStringView Path) const;

        bool ReadFile(TVector<uint8>& Result, FStringView Path) override;
        bool ReadFile(FString& OutString, FStringView Path) override;

        bool WriteFile(FStringView Path, FStringView Data) override;
        bool WriteFile(FStringView Path, TSpan<const uint8> Data) override;
        bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data) override;

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
        FStringView GetBasePath() const override { return BasePath; }

    private:

        FFixedString AliasPath;
        FFixedString BasePath;
    };
}
