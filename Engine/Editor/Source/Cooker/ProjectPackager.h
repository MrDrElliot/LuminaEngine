#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class FThreadStop;

    struct FPackageBuildResult
    {
        bool    bSuccess        = false;
        FString OutputDirectory;
        FString PakPath;
        FString ErrorMessage;
    };

    enum class EPackagePlatform : uint8
    {
        Windows64,
        Linux64,
    };

    // How one target platform names the binaries the packager stages.
    struct EDITOR_API FPackagePlatformRules
    {
        FStringView Name;
        FStringView ExecutableExtension;
        FStringView LibraryPrefix;
        FStringView LibraryExtension;

        // The .NET runtime folder DotNetHost::RuntimeRid resolves on this platform, under External/DotNet/runtime.
        FStringView DotNetRuntimeRid;

        // Linux has no delay-loading, so third-party libraries stay beside the executable on its $ORIGIN rpath.
        bool        bThirdPartyToDataPlugins = true;

        static FPackagePlatformRules For(EPackagePlatform Platform);

        FFixedString BinariesDirectory(FStringView Root) const;
        FString      ExecutableFileName(FStringView Stem) const;
        FString      LibraryFileName(FStringView Stem) const;
        bool         IsExecutable(FStringView FileName) const;
        bool         IsLibrary(FStringView FileName) const;

        // The file name without the library prefix or the extension, "Runtime-Development" for libRuntime-Development.so.
        FStringView  ModuleStem(FStringView FileName) const;

        // A binary the build leaves beside the game's that belongs to another configuration, target or the editor alone.
        bool         IsSkippedBinary(FStringView FileName, FStringView BinaryTail) const;
    };

    struct FPackageBuildOptions
    {
        // Default: <ProjectDir>/Build/<ProjectName>/
        FString OutputDirectory;

        // True builds the project's Game target with LuminaBuildTool before copying; false stops
        // after cook.
        bool    bBuildExecutable = true;

        // Mirror loose /Game scripts next to the exe instead of in the PAK.
        bool    bExtractScriptsAsLooseFiles = false;

        // Build configuration ("Shipping" recommended; "Development" for debugging).
        FString BuildConfiguration = "Shipping";

        // Builds the Server target into <Project>Server, a headless dedicated server with its own data and pak.
        bool    bServer = false;

        EPackagePlatform Platform = EPackagePlatform::Windows64;

        // Project root passed to the build tool as -Project, and the second place binaries are
        // collected from. Carried in the options rather than read from the engine, because the
        // build and copy stages run on a worker thread and must not touch engine state.
        FString ProjectDirectory;

        // Embedded under /Extras/ in the PAK.
        TVector<FString> ExtraFiles;
        TVector<FString> ExtraDirectories;
    };

    /** Cook + (optional) build + binary copy for shipping a project.
     *
     *  The build runs through LuminaBuildTool, the same tool the IDE and the command line use, so a
     *  packaged game is produced by the path everything else is produced by. It needs no Visual
     *  Studio installation to locate and no solution file to have been generated first. */
    class EDITOR_API FProjectPackager
    {
    public:

        static FPackageBuildResult Package(const FPackageBuildOptions& Options, const TFunction<void(FStringView)>& LogFunc = {});

        // The executable and data folder name, which a server package suffixes so it never mixes with the game's.
        static FString GetPackageName(FStringView ProjectName, bool bServer);

        // Worker-safe build and copy of an already cooked pak, where raising Stop kills the build tool and fails the build.
        static FPackageBuildResult BuildAndCopyOnly(
            const FPackageBuildOptions& Options,
            FStringView ProjectName,
            FStringView PakPath,
            const TFunction<void(FStringView)>& LogFunc = {},
            const FThreadStop* Stop = nullptr);

        // Mirrors loose (non-.lasset) files from /Game and /Scripts under <OutDir>/{Game,Scripts}/.
        // Main-thread only (walks VFS).
        static size_t ExtractLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc = {});

        // Everything a package holds besides its exe, <OutputDirectory>/<ProjectName>_Data, which the runtime finds from the exe's own name.
        static FString GetDataDirectory(FStringView OutputDirectory, FStringView ProjectName);

        static FString GetPakPath(FStringView OutputDirectory, FStringView ProjectName);
    };
}
