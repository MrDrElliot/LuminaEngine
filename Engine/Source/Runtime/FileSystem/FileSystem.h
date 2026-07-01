#pragma once

#include "FileInfo.h"
#include "IFileSystem.h"
#include "MemoryFileSystem.h"
#include "NativeFileSystem.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Memory/SmartPtr.h"

namespace Lumina::VFS
{
    namespace Detail
    {
        RUNTIME_API IFileSystem& AddFileSystemImpl(const FFixedString& Alias, TUniquePtr<IFileSystem> System);
    }

    template<typename T, typename... TArgs>
    requires std::derived_from<T, IFileSystem> && std::constructible_from<T, FFixedString, TArgs...>
    T& Mount(const FFixedString& Alias, TArgs&&... Args)
    {
        TUniquePtr<T> Owned = MakeUnique<T>(Alias, Forward<TArgs>(Args)...);
        T& Result = *Owned;
        Detail::AddFileSystemImpl(Alias, TUniquePtr<IFileSystem>(Owned.release()));
        return Result;
    }

    // Remove every mount registered under Alias (e.g. "/Game"). No-op if nothing is mounted there.
    // Use before re-mounting a project-scoped alias so reloads/switches don't stack duplicate mounts
    // (the mount list is append-only and DirectoryIterator visits every entry).
    RUNTIME_API void Unmount(FStringView Alias);

    RUNTIME_API void DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback);
    RUNTIME_API void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback);

    RUNTIME_API bool IsEmpty(FStringView Directory);
    RUNTIME_API FStringView RemoveExtension(FStringView Path);
    RUNTIME_API bool Rename(FStringView Old, FStringView New);
    RUNTIME_API FFixedString MakeUniqueFilePath(FStringView BasePath);
    RUNTIME_API FStringView Extension(FStringView Path);
    RUNTIME_API FStringView FileName(FStringView Path, bool bRemoveExtension = false);
    RUNTIME_API bool Remove(FStringView Path);
    RUNTIME_API size_t Size(FStringView Path);
    RUNTIME_API bool RemoveAll(FStringView Path);
    RUNTIME_API FFixedString ResolvePath(FStringView Path);
    RUNTIME_API bool DoesAliasExists(const FName& Alias);
    RUNTIME_API bool CreateDir(FStringView Path);
    RUNTIME_API bool IsUnderDirectory(FStringView Parent, FStringView Path);
    RUNTIME_API bool IsDirectory(FStringView Path);
    RUNTIME_API bool IsLuminaAsset(FStringView Path);
    RUNTIME_API FStringView Parent(FStringView Path, bool bRemoveTrailingSlash = false);

    RUNTIME_API bool ReadFile(TVector<uint8>& Result, FStringView Path);
    RUNTIME_API bool ReadFile(FString& OutString, FStringView Path);
    RUNTIME_API bool WriteFile(FStringView Path, FStringView Data);
    RUNTIME_API bool WriteFile(FStringView Path, TSpan<const uint8> Data);
    RUNTIME_API bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data);

    RUNTIME_API void PlatformOpen(FStringView Path);

    RUNTIME_API bool Exists(FStringView Path);

    RUNTIME_API bool HasExtension(FStringView Path, FStringView Ext);

    // Best-effort conversion of any editor path into VFS form: '/'-prefixed inputs are normalized as-is,
    // else matched against each native mount's BasePath; no match returns the input verbatim.
    RUNTIME_API FFixedString ResolveToVirtualPath(FStringView InputPath);
}
