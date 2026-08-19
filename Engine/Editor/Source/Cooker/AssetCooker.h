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
        size_t  NumAssets   = 0;
        size_t  NumExtras   = 0; // engine resources, shaders, config, etc. (Main only)
        size_t  Bytes       = 0;
    };

    struct FCookResult
    {
        bool                      bSuccess        = false;
        size_t                    NumAssetsCooked = 0;
        size_t                    NumExtraFiles   = 0;
        size_t                    TotalBytes      = 0;
        TVector<FCookChunkResult> Chunks;          // one entry per written .pak
        FString                   ErrorMessage;
    };

    // Reachability-driven cooker, seeded from FEngine::GetCookRoots(); only .lasset reachable via FAssetData::Dependencies enters a PAK.
    // Chunked output: "Main" uses OutputPakPath verbatim + holds shared content; other chunks land at <stem>-<chunk>.pak.
    class FAssetCooker
    {
    public:
        // LogFunc gets one line per stage/file; {} to suppress.
        static FCookResult Cook(FStringView OutputPakPath, const FCookOptions& Options = {}, const TFunction<void(FStringView)>& LogFunc = {});
    };
}
