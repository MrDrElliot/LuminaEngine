#pragma once
#include "ModuleAPI.h"
#include "TextAssetTypes.h"
#include "Containers/String.h"
#include "GUID/GUID.h"

namespace Lumina
{
    // Stable identity for loose text assets (.luau/.rml/.rcss) lives in a tiny sidecar under a hidden
    // ".lmeta" directory at the owning content root, mirroring the file's relative subpath:
    //   /Game/UI/Foo.luau  ->  /Game/.lmeta/UI/Foo.luau.lmeta
    // Mirroring the full subpath (not just the name) keeps same-named files in different folders distinct.
    // The sidecar is the durable source of truth (committed, never shipped); the registry cache + cooked
    // AssetRegistry.bin are rebuildable from it.
    namespace TextAssetSidecar
    {
        // Hidden meta directory name placed at each content root.
        inline constexpr FStringView kMetaDirName  = ".lmeta";
        inline constexpr FStringView kSidecarExt   = ".lmeta";

        // Derive the sidecar virtual path for a content file. Empty if no content root matches.
        RUNTIME_API FFixedString PathFor(FStringView ContentVirtualPath);

        // True iff Path lies inside a ".lmeta" directory (used to skip sidecars in walks/watchers).
        RUNTIME_API bool IsSidecarPath(FStringView Path);

        // Read GUID (+ optional kind) from an existing sidecar. False if missing/corrupt.
        RUNTIME_API bool Read(FStringView ContentVirtualPath, FGuid& OutGuid, ETextAssetKind* OutKind = nullptr);

        // Write/overwrite the sidecar atomically (creates the .lmeta tree).
        RUNTIME_API bool Write(FStringView ContentVirtualPath, const FGuid& Guid, ETextAssetKind Kind);

        // Return the existing GUID, or mint a fresh one and persist a sidecar. Invalid only if the
        // path has no resolvable content root.
        RUNTIME_API FGuid ReadOrMint(FStringView ContentVirtualPath, ETextAssetKind Kind);

        // Relocate a sidecar old->new content path, preserving the GUID. Best-effort.
        RUNTIME_API bool Move(FStringView OldContentPath, FStringView NewContentPath);

        // Delete the sidecar for a content file. Best-effort (no-op if absent).
        RUNTIME_API bool Delete(FStringView ContentVirtualPath);
    }
}
