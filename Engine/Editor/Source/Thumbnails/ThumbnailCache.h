#pragma once
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct FPackageThumbnail;
}

namespace Lumina::ThumbnailCache
{
    // On-disk sidecar cache for generated thumbnails, one file per asset GUID under
    // <EngineInstall>/Intermediates/ThumbnailCache. Fully decoupled from the .lasset: generating a
    // thumbnail never rewrites the asset (no cook churn, no mtime bump). Validity is keyed by the
    // asset's ContentHash, so a re-imported/edited asset's stale entry is ignored and regenerated.

    // Absolute path of the cache file for GUID (empty if the engine install dir is unknown).
    FString CachePath(const FGuid& GUID);

    // Read GUID's cached thumbnail into OutThumbnail iff the file exists and its stored ContentHash
    // matches ExpectedContentHash. Returns false (leaving OutThumbnail untouched) on miss/stale/corrupt.
    bool Load(const FGuid& GUID, uint64 ExpectedContentHash, FPackageThumbnail& OutThumbnail);

    // Write Thumbnail's RGBA image to GUID's cache file, stamped with ContentHash. No-op for an
    // invalid GUID or an empty image.
    bool Save(const FGuid& GUID, uint64 ContentHash, FPackageThumbnail& Thumbnail);

    // Remove GUID's cache file if present (used when an asset is deleted). Safe if absent.
    void Delete(const FGuid& GUID);
}
