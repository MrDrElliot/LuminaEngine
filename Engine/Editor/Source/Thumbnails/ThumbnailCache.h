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
    // One file per asset GUID, keyed by ContentHash, so generating a thumbnail never rewrites the .lasset.

    // Absolute path of the cache file for GUID (empty if the engine install dir is unknown).
    FString CachePath(const FGuid& GUID);

    // Leaves OutThumbnail untouched and returns false when the file is missing, stale or corrupt.
    bool Load(const FGuid& GUID, uint64 ExpectedContentHash, FPackageThumbnail& OutThumbnail);

    // No-op for an invalid GUID or an empty image.
    bool Save(const FGuid& GUID, uint64 ContentHash, FPackageThumbnail& Thumbnail);

    // Remove GUID's cache file if present (used when an asset is deleted). Safe if absent.
    void Delete(const FGuid& GUID);
}
