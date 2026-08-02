#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina::VFS
{
    struct FFileInfo;

    class RUNTIME_API IFileSystem
    {
    public:

        virtual ~IFileSystem() = default;

        virtual bool ReadFile(TVector<uint8>& Result, FStringView Path) = 0;
        virtual bool ReadFile(FString& OutString, FStringView Path) = 0;

        virtual bool WriteFile(FStringView Path, FStringView Data) = 0;
        virtual bool WriteFile(FStringView Path, TSpan<const uint8> Data) = 0;

        // Crash-safe write (all-or-nothing); default falls back to WriteFile for backends with no atomic primitive.
        virtual bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data) { return WriteFile(Path, Data); }

        virtual bool Exists(FStringView Path) const = 0;
        virtual bool IsDirectory(FStringView Path) const = 0;
        virtual bool IsEmpty(FStringView Path) const = 0;
        virtual size_t Size(FStringView Path) const = 0;

        virtual bool CreateDir(FStringView Path) = 0;
        virtual bool Remove(FStringView Path) = 0;
        virtual bool RemoveAll(FStringView Path) = 0;
        virtual bool Rename(FStringView Old, FStringView New) = 0;

        virtual void PlatformOpen(FStringView Path) const = 0;

        virtual void DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const = 0;
        virtual void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const = 0;

        virtual FStringView GetAliasPath() const = 0;
        virtual FStringView GetBasePath() const = 0;

        virtual bool IsReadOnly() const { return false; }
    };
}
