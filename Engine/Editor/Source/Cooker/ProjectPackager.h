#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    struct FPackageBuildResult
    {
        bool    bSuccess        = false;
        FString OutputDirectory;
        FString PakPath;
        FString ErrorMessage;
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
    class FProjectPackager
    {
    public:

        static FPackageBuildResult Package(const FPackageBuildOptions& Options, const TFunction<void(FStringView)>& LogFunc = {});

        /** Build + binary-copy stages only; expects a .pak to already exist at <OutputDirectory>/<ProjectName>.pak.
         *  Safe from a worker thread (touches no engine state); LogFunc may be invoked from that thread. */
        static FPackageBuildResult BuildAndCopyOnly(
            const FPackageBuildOptions& Options,
            FStringView ProjectName,
            FStringView PakPath,
            const TFunction<void(FStringView)>& LogFunc = {});

        // Mirrors loose (non-.lasset) files from /Game and /Scripts under <OutDir>/{Game,Scripts}/.
        // Main-thread only (walks VFS).
        static size_t ExtractLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc = {});
    };
}
