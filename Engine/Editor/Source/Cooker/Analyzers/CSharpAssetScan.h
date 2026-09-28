#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class FAssetRegistry;

    // Finds asset paths and content folders named in C# string literals, since scripts load by path and nothing imports those assets.
    class FCSharpAssetScan
    {
    public:
        struct FResult
        {
            // Literals that resolved to a registered asset.
            TVector<FString> AssetPaths;

            // Literals naming a folder that holds assets, which scripts join with a name at runtime.
            TVector<FString> FolderPaths;

            size_t FilesScanned  = 0;
            size_t RawCandidates = 0;
        };

        static FResult ScanRoots(const TVector<FString>& VirtualRoots, const FAssetRegistry& Registry,
                                 const TFunction<void(FStringView)>& LogFunc = {});

        // Every string literal starting with a slash, with comments skipped and an interpolated literal cut at its first hole.
        static TVector<FString> ExtractCandidates(FStringView Contents);
    };
}
