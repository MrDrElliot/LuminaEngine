#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class IEditorToolContext;
}

namespace Lumina::AssetOps
{
    struct FDeleteAssetResult
    {
        bool    bDeleted = false;
        FString Error;
    };

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

    // Every file at or under VirtualPath that is read-only on disk, which move, rename and delete leave alone.
    NODISCARD EDITOR_API TVector<FString> FindReadOnlyFiles(FStringView VirtualPath);

    // A single path segment every platform accepts, with no separator that would turn a rename into a move into a folder.
    NODISCARD EDITOR_API bool IsValidLeafName(FStringView Name, FString* OutReason = nullptr);

    // The prefab variants whose parent is the prefab at AssetPath, comma separated, or empty when it has none.
    NODISCARD EDITOR_API FString FindDependentPrefabVariants(FStringView AssetPath);

    // Rename and move are one operation, since both only change the full path.
    NODISCARD EDITOR_API FPathOpResult MovePath(FStringView OldPath, FStringView NewPath);

    NODISCARD EDITOR_API FPathOpResult CreateFolder(FStringView VirtualPath);

    NODISCARD EDITOR_API bool IsAnyWorldPlayingOrSimulating();

    NODISCARD EDITOR_API bool IsLoadedWorldAsset(FStringView AssetPath);

    // Destroys one .lasset: prefab instances first, then its editor, its package and its file. Not undoable.
    NODISCARD EDITOR_API FDeleteAssetResult DeleteAsset(FStringView AssetPath, IEditorToolContext* Context);
}
