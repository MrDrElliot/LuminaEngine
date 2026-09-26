#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Assets/AssetTypes/Textures/Texture.h"

#include "MCPAssetTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SAssetInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        /** Content path, which is what the editor shows in the content browser. */
        PROPERTY()
        FString Path;

        /** Asset class, so a field wanting a CStaticMesh can be matched against it. */
        PROPERTY()
        FString AssetClass;

        /** Pass this to any component field that takes an asset. */
        PROPERTY()
        FString Guid;
    };

    REFLECT()
    struct MCPEDITOR_API SSearchAssetsParams
    {
        GENERATED_BODY()

        /** Only assets whose name or path contains this. Empty matches every asset. */
        PROPERTY()
        FString Contains;

        /** Only assets whose class contains this, such as StaticMesh. Empty matches every class. */
        PROPERTY()
        FString AssetClass;

        /** How many to return at most, so a large project cannot flood the reply. */
        PROPERTY()
        int32 Limit = 50;

        /** How many matches to skip first, so a list longer than Limit can be paged. */
        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSearchAssetsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAssetInfo> Results;

        /** How many matched before the limit was applied. */
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SFolderInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        /** How many assets sit directly in this folder, not counting its subfolders. */
        PROPERTY()
        int32 AssetCount = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListFoldersParams
    {
        GENERATED_BODY()

        /** Folder to list beneath, such as /Game/Content. Empty lists from every mounted root. */
        PROPERTY()
        FString Under;

        /** How deep to descend below Under. 1 lists only its immediate children. */
        PROPERTY()
        int32 Depth = 2;
    };

    REFLECT()
    struct MCPEDITOR_API SListFoldersResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SFolderInfo> Folders;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateFolderParams
    {
        GENERATED_BODY()

        /** Full path of the folder to create, such as /Game/Content/Environment/Trees. */
        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SPathOpResult
    {
        GENERATED_BODY()

        /** Where the thing ended up, which is the path every later call should use. */
        PROPERTY()
        FString Path;

        /** Asset identities relocated, above one only when a folder moved with assets inside it. */
        PROPERTY()
        int32 Relocated = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SRenameAssetParams
    {
        GENERATED_BODY()

        /** Path of the asset or folder to rename, from assets.search or assets.list_folders. */
        PROPERTY()
        FString Path;

        /** New name only, with no path and no extension. Use assets.move to change folder. */
        PROPERTY()
        FString NewName;
    };

    REFLECT()
    struct MCPEDITOR_API SMoveAssetParams
    {
        GENERATED_BODY()

        /** Path of the asset or folder to move. */
        PROPERTY()
        FString Path;

        /** Folder to move it into. It must already exist, so create it first if needed. */
        PROPERTY()
        FString DestinationFolder;
    };

    REFLECT()
    struct MCPEDITOR_API SSaveAssetParams
    {
        GENERATED_BODY()

        /** GUID of the asset to write to disk, from assets.search or material.create. */
        PROPERTY()
        FString Asset;
    };

    REFLECT()
    struct MCPEDITOR_API SSaveAssetResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SImportAssetsParams
    {
        GENERATED_BODY()

        /** OS path of a source file, or a directory whose importable files are all imported. */
        PROPERTY()
        FString Source;

        /** Content folder to import into, such as /Game/Content/UI. Created if missing. */
        PROPERTY()
        FString DestinationFolder;

        /** Whether a directory Source also imports its subdirectories, mirrored under DestinationFolder. */
        PROPERTY()
        bool Recursive = true;

        /** Mip policy for imported textures. UI skips the mip chain, since the UI pass never minifies. */
        PROPERTY()
        ETextureGroup TextureGroup = ETextureGroup::World;
    };

    REFLECT()
    struct MCPEDITOR_API SImportAssetsResult
    {
        GENERATED_BODY()

        /** Content paths of the assets this call created. */
        PROPERTY()
        TVector<FString> Imported;

        /** Files left alone because an asset with that name already existed, so a rerun resumes. */
        PROPERTY()
        int32 Skipped = 0;

        /** Source files that failed, each with the importer's reason. */
        PROPERTY()
        TVector<FString> Failed;
    };

    namespace MCP
    {
        void RegisterAssetTools(FStringView Owner);
    }
}
