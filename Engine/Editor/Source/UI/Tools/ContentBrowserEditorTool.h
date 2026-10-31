#pragma once

#include "EditorTool.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/LuminaCommonTypes.h"
#include "FileSystem/FileSystem.h"
#include "Memory/SmartPtr.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "Tools/Actions/DeferredActions.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/Widgets/TileViewWidget.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

namespace Lumina::ContentBrowserEditorToolInternal
{
    class FContentBrowserTileViewItem;
}

namespace Lumina
{
    class CImporter;
    class FPropertyTable;
    struct FAssetData;
    struct FImportRequest;

    class FContentBrowserEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FContentBrowserEditorTool)

        explicit FContentBrowserEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Content Browser", nullptr)
        {
        }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void Update(const FUpdateContext& UpdateContext) override;
        void EndFrame() override;
        bool OnEvent(FEvent& Event) override;
        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FOLDER_MULTIPLE; }
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

        void RefreshContentBrowser();

        // Reapplies the search and type filter to the folder already read, without walking it again.
        void RefilterContentBrowser();

        void BrowseToAsset(FStringView VirtualPath);

        // Shows Folder with Search typed into the search box, the way a user browsing there would.
        void BrowseToFolder(FStringView Folder, FStringView Search);

        NODISCARD FStringView GetBrowsedFolder() const { return FStringView(SelectedPath.c_str(), SelectedPath.size()); }

        // Selects a just-created asset and opens its inline rename as soon as its tile appears.
        void QueueRenameAfterCreate(FStringView VirtualPath);

        void HandleContentBrowserDragDrop(FStringView DropPath, FStringView PayloadPath);

        // The single highlighted asset, or null for an empty selection, a multi-selection, a folder or a loose file.
        NODISCARD const FAssetData* GetSelectedAsset() const;

        // The one selected file's virtual path, or empty when a folder, several items or nothing is selected.
        NODISCARD FFixedString GetSelectedFilePath() const;

    private:

        using FTileItem = ContentBrowserEditorToolInternal::FContentBrowserTileViewItem;

        // An import or reimport settings dialog, torn down however it closes.
        struct FImportDialogState;

        // One per content root, Game and each enabled plugin mount, so a change resolves to the right root.
        struct FContentWatcher
        {
            TUniquePtr<FDirectoryWatcher>   Watcher;
            FFixedString                    VirtualPrefix;
            size_t                          WatchRootLen = 0;
        };

        struct FListingEntry
        {
            VFS::FFileInfo  Info;
            FName           TypeLabel;
            FName           AssetClass;

            // The asset's class is no longer registered, so it cannot be loaded or opened, only deleted.
            bool            bMissingType = false;
        };

        void OnProjectLoaded();

        // Every delete goes through here: play guard, variant guard, referencer fixup, then the destroy queue.
        void RequestDeletion(TVector<FFixedString> Paths, int32 ProtectedCount);
        void OpenDeletionWarningPopup(const FTileItem* Item);
        bool DestroyAssetAtPath(FStringView AssetPath);

        // Names the prefab variants that would be orphaned, empty when the set is safe to delete.
        FFixedString FindBlockingPrefabVariants(const TVector<FFixedString>& Paths) const;

        // Retargets everything pointing at this asset onto another one, leaving the asset itself in place.
        void OpenReplaceReferencesModal(const FTileItem* Item);

        // True when the user confirmed.
        bool DrawImportWindow(CImporter* Importer, const FImportRequest& Request, int32 RemainingCount, bool& bShouldClose, bool& bOutApplyToAll);

        // Imports become extensionless packages, so a free destination is checked in the package namespace.
        FFixedString MakeUniqueImportDestination(FStringView SourcePath);

        // Takes ownership of Importer, runs the build stage off-thread, then destroys it.
        void StartImport(CImporter* Importer, const FImportRequest& Request);
        void ProcessNextImport();
        void TryImport(const FFixedString& Path);
        void TryImport(const TVector<FFixedString>& Paths);

        // Swaps a source file's contents onto an existing asset, keeping its object, GUID and path, rather than minting a second asset.
        void DrawReimportAssetMenuItem(const FTileItem* ContentItem, bool bIsProtected);

        // Takes a GUID rather than a pointer, since the asset can be destroyed before the async parse returns.
        void StartReimport(const FGuid& AssetGUID, CClass* ImporterClass, const FFixedString& SourceFile);

        // The off-thread half of a reimport, which takes ownership of Importer.
        void FinishReimport(const FGuid& AssetGUID, CImporter* Importer, const FFixedString& SourceFile);

        void ExportAssets(const TVector<FGuid>& Assets);

        void DrawDirectoryBrowser(bool bIsFocused, ImVec2 Size);

        // Walks the tree down to PendingDirectoryReveal, expanding lazily built nodes, once the tree is rebuilt for the frame.
        void RevealPendingDirectory();

        void DrawContentBrowser(bool bIsFocused, ImVec2 Size);
        void DrawContentDirectoryContextMenu();
        void DrawAssetContextMenu(FTileItem* ContentItem);

        // Only the operations that mean the same thing for a mixed set of folders and files.
        void DrawMultiSelectionContextMenu(const TVector<FTileViewItem*>& Items);

        // One confirmation for the whole set, with protected entries left out rather than aborting it.
        void DeleteSelectedItems(const TVector<FTileViewItem*>& Items);
        void BulkRenameItems(const TVector<FTileViewItem*>& Items);

        // Disabled for packages holding sub-objects a property copy would alias.
        void DrawDuplicateAssetMenuItem(const FTileItem* ContentItem, bool bIsProtected);

        // Prefab only, minting a child prefab that inherits from this one.
        void DrawCreateVariantMenuItem(const FTileItem* ContentItem);

        // Every navigation goes through NavigateTo, so back and forward stay honest.
        void NavigateTo(FStringView Path);
        void NavigateBack();
        void NavigateForward();
        NODISCARD bool CanNavigateBack() const { return !NavBackStack.empty(); }
        NODISCARD bool CanNavigateForward() const { return !NavForwardStack.empty(); }

        // Rebuilds FilterState from the asset registry, keeping the user's choices.
        void RefreshFilterClasses();

        // "CStaticMesh" becomes "STATICMESH" and a loose file its extension; empty for a directory.
        NODISCARD static FFixedString MakeTypeLabel(const VFS::FFileInfo& FileInfo);

        FDeferredActionRegistry     ActionRegistry;
        TVector<FContentWatcher>    Watchers;
        TAtomic<bool>               bDiskListingChanged{false};

        FTreeListView               DirectoryListView;
        FTreeListViewContext        DirectoryContext;
        FTileViewWidget             ContentBrowserTileView;
        FTileViewContext            ContentBrowserTileViewContext;
        float                       ContentBrowserTileSize = 84.0f;

        FFixedString                SelectedPath;
        TVector<FFixedString>       NavBackStack;
        TVector<FFixedString>       NavForwardStack;
        FFixedString                PendingBrowseToPath;
        FFixedString                PendingDirectoryReveal;

        // Survives rebuilds until the tile exists, since a factory can finish creating the asset asynchronously.
        FFixedString                PendingRenamePath;

        // Rebuilt from the classes present in the browsed content, so a type with no factory still gets an entry.
        THashMap<FName, bool>       FilterState;
        FFixedString                SearchText;

        // The browsed folder as last read from disk, which the tiles refer into, so it only changes once every tile is gone.
        TVector<FListingEntry>      Listing;
        FFixedString                ListingFolder;
        bool                        bListingStale = true;

        TUniquePtr<FPropertyTable>  ImportSettingsTable;

        // Files still waiting on an options window, drained one at a time by ProcessNextImport.
        TVector<FFixedString>       PendingImports;
        bool                        bImportWindowOpen = false;
        bool                        bApplyImportSettingsToAll = false;
        FString                     LastExportDirectory;
    };
}
