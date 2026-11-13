#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"
#include "Assets/AssetTypes/Textures/Texture.h"

#include "MCPAssetTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SAssetRefParams
    {
        GENERATED_BODY()

        // GUID or content path of the asset, from assets.search.
        PROPERTY()
        FString Asset;
    };

    REFLECT()
    struct MCPEDITOR_API SAssetPathParams
    {
        GENERATED_BODY()

        // Content path, such as /Game/Content/Environment/Trees.
        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SPathOpResult
    {
        GENERATED_BODY()

        // Where it ended up; use this path in later calls.
        PROPERTY()
        FString Path;

        // Asset identities relocated, above one only when a folder moved with assets inside it.
        PROPERTY()
        int32 Relocated = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSearchAssetsParams
    {
        GENERATED_BODY()

        // Substring of the name or path. Empty matches every asset.
        PROPERTY()
        FString Contains;

        // Substring of the class, such as StaticMesh. Empty matches every class.
        PROPERTY()
        FString AssetClass;

        PROPERTY()
        int32 Limit = 50;

        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSearchAssetsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAssetInfo> Results;

        // How many matched before Limit was applied.
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SFolderInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        // Assets directly in this folder, not counting subfolders.
        PROPERTY()
        int32 AssetCount = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListFoldersParams
    {
        GENERATED_BODY()

        // Folder to list beneath. Empty means /Game/Content.
        PROPERTY()
        FString Under;

        // Levels below Under; 1 lists only its immediate children.
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
    struct MCPEDITOR_API SMoveAssetParams
    {
        GENERATED_BODY()

        // Asset or folder to move or rename.
        PROPERTY()
        FString Path;

        // Existing folder to move it into. Empty keeps its folder.
        PROPERTY()
        FString DestinationFolder;

        // New name with no path or extension. Empty keeps its name.
        PROPERTY()
        FString NewName;
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

        // OS path of a source file, or a directory whose importable files are all imported.
        PROPERTY()
        FString Source;

        // Content folder to import into, such as /Game/Content/UI. Created if missing.
        PROPERTY()
        FString DestinationFolder;

        // A directory Source also imports subdirectories, mirrored under DestinationFolder.
        PROPERTY()
        bool Recursive = true;

        // Mip policy for imported textures. UI skips the mip chain.
        PROPERTY()
        ETextureGroup TextureGroup = ETextureGroup::World;

        // JSON object of importer properties, such as {"bImportAsPrefab": true}.
        PROPERTY()
        FString Settings;

        // Reimport existing assets in place instead of skipping the file.
        PROPERTY()
        bool ReplaceExisting = false;
    };

    REFLECT()
    struct MCPEDITOR_API SImportAssetsResult
    {
        GENERATED_BODY()

        // Content paths of the assets this call created.
        PROPERTY()
        TVector<FString> Imported;

        // Files left alone because the asset already existed.
        PROPERTY()
        int32 Skipped = 0;

        // Source files that failed, each with the importer's reason.
        PROPERTY()
        TVector<FString> Failed;
    };

    REFLECT()
    struct MCPEDITOR_API SAssetClassInfo
    {
        GENERATED_BODY()

        // Pass this to assets.create as ClassName.
        PROPERTY()
        FString Name;

        // Content browser grouping, such as Gameplay or Materials.
        PROPERTY()
        FString Category;

        // True when the content browser would show a creation dialog, which assets.create skips.
        PROPERTY()
        bool bHasCreationDialogue = false;
    };

    REFLECT()
    struct MCPEDITOR_API SListAssetClassesParams
    {
        GENERATED_BODY()

        // Substring of the class name or category. Empty lists every class.
        PROPERTY()
        FString Contains;
    };

    REFLECT()
    struct MCPEDITOR_API SListAssetClassesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAssetClassInfo> Classes;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateAssetParams
    {
        GENERATED_BODY()

        // Asset class name, from assets.list_classes.
        PROPERTY()
        FString ClassName;

        // Name without an extension.
        PROPERTY()
        FString Name;

        // Existing folder under /Game/Content.
        PROPERTY()
        FString Folder;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeAssetResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        FString Path;

        PROPERTY()
        FString ClassName;

        PROPERTY()
        FString Guid;

        // Every reflected field as JSON, in the shape assets.set_property takes.
        PROPERTY()
        FString Properties;

        // Field paths that cannot be written.
        PROPERTY()
        TVector<FString> UnwritableFields;

        // Assets that reference this one, which assets.delete would leave dangling.
        PROPERTY()
        TVector<SAssetInfo> Referencers;
    };

    REFLECT()
    struct MCPEDITOR_API SSetAssetPropertyParams
    {
        GENERATED_BODY()

        // GUID or content path of the asset.
        PROPERTY()
        FString Asset;

        // Field path, such as Settings.Radius or Layers[2].Name.
        PROPERTY()
        FString Path;

        // New value as JSON, such as 12.5, "Torch" or {"X":1,"Y":0,"Z":0}.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SAssetPropertyResult
    {
        GENERATED_BODY()

        // Value before the change, as JSON.
        PROPERTY()
        FString Previous;

        // Value read back after the change, as JSON.
        PROPERTY()
        FString Current;

        // True when an open editor recorded the edit, so editor.history can undo it.
        PROPERTY()
        bool bUndoable = false;
    };

    REFLECT()
    struct MCPEDITOR_API SDeleteAssetParams
    {
        GENERATED_BODY()

        // GUID or content path of the asset.
        PROPERTY()
        FString Asset;

        // Delete even when other assets reference it, leaving those references dangling.
        PROPERTY()
        bool bForce = false;
    };

    REFLECT()
    struct MCPEDITOR_API SDeleteAssetResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        // False with Referencers listed means bForce was needed.
        PROPERTY()
        bool bDeleted = false;

        PROPERTY()
        TVector<SAssetInfo> Referencers;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterAssetTools(FStringView Owner);
    }
}
