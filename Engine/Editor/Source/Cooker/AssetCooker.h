#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    struct FCookOptions
    {
        // Ship loose /Game files next to the exe instead of in the PAK (mountable as overlay for tweakability).
        bool bExtractScriptsAsLooseFiles = false;

        // Absolute paths embedded at /Extras/<filename> in the cooked VFS.
        TVector<FString> ExtraFiles;

        // Absolute directories walked recursively; files land at /Extras/<dirname>/<relative>.
        TVector<FString> ExtraDirectories;
    };

    struct FCookChunkResult
    {
        FName   Chunk;        // FName("Main") for the default / shared content chunk.
        FString PakPath;      // Absolute path of the written .pak.
        int32   NumAssets   = 0;
        int32   NumExtras   = 0; // engine resources, shaders, config, etc. (Main only)
        int64   Bytes       = 0;
    };

    struct FCookResult
    {
        bool                      bSuccess        = false;
        int32                     NumAssetsCooked = 0;
        int32                     NumExtraFiles   = 0;
        int64                     TotalBytes      = 0;
        TVector<FCookChunkResult> Chunks;          // one entry per written .pak
        FString                   ErrorMessage;
    };

    // Sidecars, IDE state, build output, project files and source-control markers, none of which a game reads.
    EDITOR_API bool IsDevelopmentOnlyGameFile(FStringView VirtualPath, bool bKeepScriptSources);

    // Where the cook of PakPath keeps a record with the project, so the shipped folder holds only what the game reads.
    EDITOR_API FString GetCookRecordPath(FStringView ProjectDirectory, FStringView PakPath, FStringView Suffix);

    // The project icon re-encoded as PNG, with one warning through LogFunc when it is set but missing or undecodable.
    EDITOR_API bool EncodeProjectIcon(FStringView IconPath, TVector<uint8>& OutPng, const TFunction<void(FStringView)>& LogFunc);

    // The cooked settings point the game here, and the packager stamps the exe from the record beside the pak's contents list.
    inline constexpr FStringView CookedProjectIconPath = "/Engine/ProjectIcon.png";
    inline constexpr FStringView ProjectIconRecordSuffix = ".icon.png";

    // Reachability-driven cooker, seeded from FEngine::GetCookRoots(); only .lasset reachable via FAssetData::Dependencies enters a PAK.
    // Chunked output: "Main" uses OutputPakPath verbatim + holds shared content; other chunks land at <stem>-<chunk>.pak.
    class EDITOR_API FAssetCooker
    {
    public:
        // LogFunc gets one line per stage/file; {} to suppress.
        static FCookResult Cook(FStringView OutputPakPath, const FCookOptions& Options = {}, const TFunction<void(FStringView)>& LogFunc = {});
    };
}
