#pragma once

#include "Assets/AssetRegistry/CookRoot.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class FAssetRegistry;

    // Scans .rml and .rcss for asset references, folded into the cook graph as implicit Soft roots so UI-only assets still reach the PAK.
    class FRmlUiAssetScan
    {
    public:
        struct FResult
        {
            // Deduplicated virtual paths to assets that resolved through FAssetRegistry.
            TVector<FString> AssetPaths;

            // Per-file counts for diagnostics.
            size_t FilesScanned    = 0;
            size_t RawCandidates   = 0;  // before registry-resolve filter
            size_t ResolvedRefs    = 0;
        };

        // Scans every .rml and .rcss under the given VFS roots, with LogFunc getting one line per ref.
        static FResult ScanRoots(
            const TVector<FString>& VirtualRoots,
            const FAssetRegistry& Registry,
            const TFunction<void(FStringView)>& LogFunc = {});

        // Scans one in-memory buffer for raw candidate paths without a registry filter, exposed for testing.
        static TVector<FString> ExtractCandidates(FStringView Contents);
    };
}
