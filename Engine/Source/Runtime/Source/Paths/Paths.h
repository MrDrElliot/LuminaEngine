#pragma once

#include "Core/Templates/LuminaTemplate.h"
#include "Containers/Name.h"
#include "Log/Log.h"

namespace Lumina::Paths
{
    void InitializePaths();
    
    
    RUNTIME_API FString GetEngineDirectory();

    RUNTIME_API FString GetExtension(const FString& InPath);

    RUNTIME_API bool IsDirectory(const FString& Path);

    RUNTIME_API bool Exists(FStringView Filename);

    /** Pulls the "scheme://" prefix from a virtual path. */
    RUNTIME_API FString GetVirtualPathPrefix(const FString& VirtualPath);

    RUNTIME_API bool CreateDirectories(FStringView Path);

    /** Path-based comparison only; no FS query. */
    RUNTIME_API bool IsUnderDirectory(const FString& ParentDirectory, const FString& Directory);

    /** e.g. ("/a/b/c.txt", "/a/") -> "b/c.txt". */
    RUNTIME_API FString MakeRelativeTo(const FString& Path, const FString& BasePath);

    RUNTIME_API void ReplaceFilename(FString& Path, const FString& NewFilename);

    RUNTIME_API const FString& GetEngineResourceDirectory();
    RUNTIME_API const FString& GetEngineFontDirectory();
    RUNTIME_API const FString& GetEngineContentDirectory();
    RUNTIME_API const FString& GetEngineConfigDirectory();
    RUNTIME_API const FString& GetEngineShadersDirectory();
    RUNTIME_API const FString& GetEngineInstallDirectory();

    RUNTIME_API void Normalize(FString& Path);
    RUNTIME_API void Normalize(FFixedString& Path);
    RUNTIME_API FFixedString Normalize(FStringView Path);

    RUNTIME_API bool PathsEqual(FStringView A, FStringView B);

    RUNTIME_API FString Parent(FStringView Path, bool bRemoveTrailingSlash = true);

    template<typename T>
    concept ValidStringType = requires(T s)
    {
        typename T::value_type;
    } && (
        requires(T s) { { s.c_str() } -> std::convertible_to<const T::value_type*>; } ||
        requires(T s) { { s.data() } -> std::convertible_to<const T::value_type*>; }
    );
    
    template <typename... Paths>
    NODISCARD FFixedString Combine(Paths&&... InPaths)
    {
        FFixedString Result;

        auto AppendPath = [&Result, bFirst = true](FStringView Path) mutable
        {
            if (Path.empty())
            {
                return;
            }
    
            if (!bFirst && !Result.empty() && Result.back() != '/')
            {
                Result += '/';
            }
    
            if (!bFirst && !Path.empty() && Path.front() == '/')
            {
                Path = Path.substr(1);
            }
        
            Result.append(Path.data(), Path.length());
            bFirst = false;
        };

        (AppendPath(Forward<Paths>(InPaths)), ...);

        return Result;
    }
    

    template<ValidStringType StringType>
    NODISCARD StringType DirName(const StringType& String)
    {
        size_t LastSlash = String.find_last_of("/\\");
        if (LastSlash != FString::npos)
        {
            return String.substr(0, LastSlash);
        }
        return String;
    }
}
