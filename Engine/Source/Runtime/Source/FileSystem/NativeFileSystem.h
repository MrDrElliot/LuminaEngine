#pragma once
#include "IFileSystem.h"

namespace Lumina::VFS
{
    class RUNTIME_API FNativeFileSystem : public IFileSystem
    {
    public:
        FNativeFileSystem(const FFixedString& InAliasPath, FStringView InBasePath);

        FPathString ResolveVirtualPath(FStringView Path) const;

        FPathString ResolveToDiskPath(FStringView Path) const override { return ResolveVirtualPath(Path); }

        bool ReadFile(TVector<uint8>& Result, FStringView Path) override;
        bool ReadFile(FString& OutString, FStringView Path) override;
        bool ReadFileRange(TVector<uint8>& Result, FStringView Path, uint64 Offset, uint64 Size) override;

        bool WriteFile(FStringView Path, FStringView Data) override;
        bool WriteFile(FStringView Path, TSpan<const uint8> Data) override;
        bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data) override;
        bool AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                    FStringView SrcPath, uint64 SrcOffset, uint64 SrcSize,
                                    TSpan<const uint8> Suffix) override;

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

        void Iterate(FStringView Path, bool bRecursive, const TFunction<void(const FFileInfo&)>& Callback) const;

        FFixedString AliasPath;
        FFixedString BasePath;
    };
}
