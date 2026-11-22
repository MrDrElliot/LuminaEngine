#pragma once
#include "ModuleAPI.h"
#include "TextAssetTypes.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "GUID/GUID.h"

namespace Lumina
{
    // Loose text asset identity lives in a sidecar at /Game/.lmeta/UI/Foo.rml.lmeta for /Game/UI/Foo.rml, and the registry cache rebuilds from it.
    namespace TextAssetSidecar
    {
        // Hidden meta directory name placed at each content root.
        inline constexpr FStringView kMetaDirName  = ".lmeta";
        inline constexpr FStringView kSidecarExt   = ".lmeta";

        // Rebuilding this list queries FPluginManager, so callers resolving many paths should build it once and reuse it.
        using FContentRootList = TVector<FFixedString>;
        RUNTIME_API FContentRootList GatherContentRoots();

        // Derive the sidecar virtual path for a content file. Empty if no content root matches.
        RUNTIME_API FFixedString PathFor(FStringView ContentVirtualPath);

        // True iff Path lies inside a ".lmeta" directory (used to skip sidecars in walks/watchers).
        RUNTIME_API bool IsSidecarPath(FStringView Path);

        // Read GUID (+ optional kind) from an existing sidecar. False if missing/corrupt.
        RUNTIME_API bool Read(FStringView ContentVirtualPath, FGuid& OutGuid, ETextAssetKind* OutKind = nullptr);

        // Write/overwrite the sidecar atomically (creates the .lmeta tree).
        RUNTIME_API bool Write(FStringView ContentVirtualPath, const FGuid& Guid, ETextAssetKind Kind);

        // Return the existing GUID, or mint a fresh one and persist a sidecar. Invalid only if the path has no resolvable content root.
        RUNTIME_API FGuid ReadOrMint(FStringView ContentVirtualPath, ETextAssetKind Kind);
        RUNTIME_API FGuid ReadOrMint(FStringView ContentVirtualPath, const FContentRootList& Roots, ETextAssetKind Kind);

        // Relocate a sidecar old->new content path, preserving the GUID. Best-effort.
        RUNTIME_API bool Move(FStringView OldContentPath, FStringView NewContentPath);

        // Delete the sidecar for a content file. Best-effort (no-op if absent).
        RUNTIME_API bool Delete(FStringView ContentVirtualPath);
    }
}
