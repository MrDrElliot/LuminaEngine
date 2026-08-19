#pragma once

#include "Assets/AssetRegistry/CookRoot.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class FAssetRegistry;

    // Scans .rml/.rcss for asset references (src/href/sprite, CSS url(), material: URIs), resolving to registered FAssetData; follows link/@import chains transitively.
    // Folded into the cook graph as implicit Soft cook roots so UI-only assets (no inbound .lasset import) still reach the PAK.
    class FRmlUiAssetScan
    {
    public:
        struct FResult
        {
            // Virtual paths to assets that resolved through FAssetRegistry.
            // Deduplicated; each entry suitable for FCookRoot.Asset.
            TVector<FString> AssetPaths;

            // Per-file counts for diagnostics.
            size_t FilesScanned    = 0;
            size_t RawCandidates   = 0;  // before registry-resolve filter
            size_t ResolvedRefs    = 0;
        };

        // Walk the given VFS roots recursively; scan every .rml / .rcss.
        // LogFunc gets one line per discovered ref (or {} to suppress).
        static FResult ScanRoots(
            const TVector<FString>& VirtualRoots,
            const FAssetRegistry& Registry,
            const TFunction<void(FStringView)>& LogFunc = {});

        // Lower-level: scan a single in-memory buffer; emits raw candidate
        // paths (no registry filter). Exposed for testing.
        static TVector<FString> ExtractCandidates(FStringView Contents);
    };
}
