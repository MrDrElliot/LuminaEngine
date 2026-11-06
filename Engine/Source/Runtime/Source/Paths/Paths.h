#pragma once

#include "Core/Templates/LuminaTemplate.h"
#include "Containers/Name.h"
#include "Log/Log.h"

namespace Lumina::Paths
{
    RUNTIME_API void InitializePaths();
    
    
    RUNTIME_API FString GetEngineDirectory();

    RUNTIME_API FString GetExtension(const FString& InPath);

    RUNTIME_API bool IsDirectory(const FString& Path);

    RUNTIME_API bool Exists(FStringView Filename);

    RUNTIME_API bool CreateDirectories(FStringView Path);

    RUNTIME_API void ReplaceFilename(FString& Path, const FString& NewFilename);

    RUNTIME_API const FString& GetEngineResourceDirectory();
    RUNTIME_API const FString& GetEngineFontDirectory();
    RUNTIME_API const FString& GetEngineContentDirectory();
    RUNTIME_API const FString& GetEngineConfigDirectory();
    RUNTIME_API const FString& GetEngineShadersDirectory();
    RUNTIME_API const FString& GetEngineInstallDirectory();

    // True for a prebuilt engine from LuminaBuild Package, which ships without sources or a toolchain.
    RUNTIME_API bool IsInstalledBuild();

    // The engine root's LuminaBuild script for this host, or empty for an installed build, which has no toolchain.
    RUNTIME_API FString GetBuildToolScript();

    // Written beside Engine/Resources by the packager, so its presence is what marks an installed build.
    inline constexpr const char* InstalledBuildMarker = "Engine/InstalledBuild.json";

    // A packaged game's folder for everything but its exe, <exe folder>/<exe name>_Data, or the exe's own folder when there is none.
    RUNTIME_API const FString& GetGameDataDirectory();

    RUNTIME_API void Normalize(FString& Path);
    RUNTIME_API void Normalize(FFixedString& Path);
    RUNTIME_API FFixedString Normalize(FStringView Path);

    // Builds <prefix><Name>-<Config><ext>, such as libFoo-Development.so.
    RUNTIME_API FFixedString MakeModuleFileName(FStringView ModuleName);

    // The engine's game application, which a package ships and a standalone launch runs.
    RUNTIME_API FFixedString MakeGameApplicationName();

    // The dedicated server build of the engine application, which packaging stages as a separate executable.
    RUNTIME_API FFixedString MakeServerApplicationName();

    // Module file name as a Game target builds it, which is not what an editor process is named.
    RUNTIME_API FFixedString MakeGameModuleFileName(FStringView ModuleName);

    // Drops trailing separators but keeps a bare root whole.
    RUNTIME_API FStringView TrimTrailingSeparators(FStringView Path);

    // Case and separator insensitive, and a trailing separator does not count.
    RUNTIME_API bool PathsEqual(FStringView A, FStringView B);

    RUNTIME_API FString Parent(FStringView Path, bool bRemoveTrailingSlash = true);

    // The per-user writable root, LOCALAPPDATA on Windows and the XDG config directory elsewhere.
    RUNTIME_API FString GetUserDataDirectory();

    template<typename T>
    concept ValidStringType = requires(T s)
    {
        typename T::value_type;
    } && (
        requires(T s) { { *s } -> std::convertible_to<const typename T::value_type*>; } ||
        requires(T s) { { s.GetData() } -> std::convertible_to<const typename T::value_type*>; }
    );
    
    template <typename... Paths>
    NODISCARD FFixedString Combine(Paths&&... InPaths)
    {
        FFixedString Result;

        auto AppendPath = [&Result, bFirst = true](FStringView Path) mutable
        {
            if (Path.IsEmpty())
            {
                return;
            }
    
            if (!bFirst && !Result.IsEmpty() && Result.Last() != '/')
            {
                Result += '/';
            }
    
            if (!bFirst && !Path.IsEmpty() && Path[0] == '/')
            {
                Path = Path.Mid(1);
            }
        
            Result.Append(Path.GetData(), Path.Len());
            bFirst = false;
        };

        (AppendPath(Forward<Paths>(InPaths)), ...);

        return Result;
    }
    

    template<ValidStringType StringType>
    NODISCARD StringType DirName(const StringType& String)
    {
        int32 LastSlash = String.FindLastOf("/\\");
        if (LastSlash != Constants::kIndexNone)
        {
            return String.Mid(0, LastSlash);
        }
        return String;
    }
}
