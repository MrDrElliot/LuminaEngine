#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPPackageTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SPackageEmptyParams
    {
        GENERATED_BODY()
    };

    REFLECT()
    struct MCPEDITOR_API SPackageCookRootInfo
    {
        GENERATED_BODY()

        // An asset path, or a folder whose every asset is cooked.
        PROPERTY()
        FString Asset;

        PROPERTY()
        FString Chunk;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageSettingsResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString ProjectName;

        PROPERTY()
        FString ProjectDirectory;

        // Where package.build writes when given no OutputDirectory.
        PROPERTY()
        FString DefaultOutputDirectory;

        PROPERTY()
        FString StartupMap;

        PROPERTY()
        TVector<SPackageCookRootInfo> CookRoots;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageBuildParams
    {
        GENERATED_BODY()

        // Empty writes to <Project>/Build/<ProjectName>.
        PROPERTY()
        FString OutputDirectory;

        // Shipping, Development or Debug.
        PROPERTY()
        FString Configuration = "Development";

        // Off stops after the cook, leaving only the pak files.
        PROPERTY()
        bool bBuildExecutable = true;

        // Ship loose /Game files next to the exe rather than inside the pak.
        PROPERTY()
        bool bExtractScriptsAsLooseFiles = false;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageChunkInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Chunk;

        PROPERTY()
        FString PakPath;

        PROPERTY()
        int32 Assets = 0;

        PROPERTY()
        int64 Bytes = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageStatusParams
    {
        GENERATED_BODY()

        // First log line to return, so a poll can ask only for what it has not seen.
        PROPERTY()
        int32 FromLine = 0;

        // Most log lines to return. The newest are kept when there are more.
        PROPERTY()
        int32 MaxLines = 60;

        // Only lines reporting an error or a warning.
        PROPERTY()
        bool bProblemsOnly = false;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageStatusResult
    {
        GENERATED_BODY()

        // Idle, Cooking, Building, Succeeded or Failed.
        PROPERTY()
        FString Stage;

        PROPERTY()
        bool bFinished = false;

        PROPERTY()
        bool bSuccess = false;

        PROPERTY()
        FString Error;

        PROPERTY()
        FString Configuration;

        PROPERTY()
        FString OutputDirectory;

        // The packaged game, once the build has copied it.
        PROPERTY()
        FString Executable;

        PROPERTY()
        int32 AssetsCooked = 0;

        PROPERTY()
        TVector<SPackageChunkInfo> Chunks;

        PROPERTY()
        float ElapsedSeconds = 0.0f;

        PROPERTY()
        int32 LogLineCount = 0;

        PROPERTY()
        int32 Warnings = 0;

        PROPERTY()
        TVector<FString> Log;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageLaunchParams
    {
        GENERATED_BODY()

        // Empty runs the executable the last package.build produced.
        PROPERTY()
        FString Executable;

        // Command line for the game, such as -benchmark -frames=600 to run a timed pass and quit.
        PROPERTY()
        FString Arguments;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageGameStatusParams
    {
        GENERATED_BODY()

        // Most lines of the game's log to return, newest kept.
        PROPERTY()
        int32 MaxLines = 40;

        // Only lines reporting an error or a warning.
        PROPERTY()
        bool bProblemsOnly = false;
    };

    REFLECT()
    struct MCPEDITOR_API SPackageGameStatusResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bRunning = false;

        PROPERTY()
        int32 ProcessId = 0;

        // Valid once bRunning is false.
        PROPERTY()
        int32 ExitCode = 0;

        PROPERTY()
        float ElapsedSeconds = 0.0f;

        PROPERTY()
        FString Executable;

        PROPERTY()
        FString LogPath;

        PROPERTY()
        int32 Errors = 0;

        PROPERTY()
        int32 Warnings = 0;

        PROPERTY()
        TVector<FString> Log;
    };

    namespace MCP
    {
        void RegisterPackageTools(FStringView Owner);
    }
}
