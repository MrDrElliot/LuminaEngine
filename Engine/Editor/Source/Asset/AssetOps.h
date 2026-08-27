#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"

namespace Lumina::AssetOps
{
    struct FPathOpResult
    {
        bool    bSucceeded = false;
        FString Error;

        // More than one only for a folder, which relocates every asset identity beneath it.
        uint32  AssetsRelocated = 0;

        NODISCARD static FPathOpResult Ok(uint32 Relocated = 0)
        {
            FPathOpResult Result;
            Result.bSucceeded     = true;
            Result.AssetsRelocated = Relocated;
            return Result;
        }

        NODISCARD static FPathOpResult Fail(FString InError)
        {
            FPathOpResult Result;
            Result.Error = Move(InError);
            return Result;
        }
    };

    // Mount roots the editor refuses to rename, move or delete.
    NODISCARD EDITOR_API bool IsProtectedRoot(FStringView VirtualPath);

    // A mount root holds Content and Scripts, so an asset placed directly in one lands where nothing scans.
    NODISCARD EDITOR_API bool IsAssetLocation(FStringView VirtualPath);

    // Rename and move are one operation, since both only change the full path.
    NODISCARD EDITOR_API FPathOpResult MovePath(FStringView OldPath, FStringView NewPath);

    NODISCARD EDITOR_API FPathOpResult CreateFolder(FStringView VirtualPath);
}
