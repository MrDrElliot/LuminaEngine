#pragma once
#include "EditorTool.h"
#include "Assets/AssetRegistry/AssetData.h"
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
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"

namespace Lumina
{
    class CFactory;
    class CImporter;
    class FPropertyTable;
    struct FImportRequest;

    class CObjectRedirector;
    struct FAssetData;
}


namespace Lumina
{
    class FContentBrowserEditorTool : public FEditorTool
    {
    public:

        struct FPendingOSDrop
        {
            FFixedString Path;
            ImVec2 MousePos;
        };

        struct FPendingRename
        {
            FFixedString OldName;
            FFixedString NewName;
        };

        struct FPendingDestroy
        {
            FFixedString PendingDestroy;
        };

        struct FContentBrowserListViewItemData
        {
            FFixedString Path;
        };

        // Picks which icon a tile draws. Resolved once at construction so the draw loop never
        // re-parses the extension (which allocates) or re-classifies the file every frame.
        enum class EIconKind : uint8
        {
            Directory,
            Asset,
            CSharpScript,
            Markup,     // .rml (UI document)
            Stylesheet, // .rcss (UI stylesheet)
            Audio,      // .wav
            Generic,
        };

        class FContentBrowserTileViewItem : public FTileViewItem
        {
        public:

            FContentBrowserTileViewItem(FTileViewItem* InParent, const VFS::FFileInfo& InInfo, bool bInProtected)
                : FTileViewItem(InParent)
                , bProtected(bInProtected)
                , FileInfo(InInfo)
                , IconKind(ClassifyIcon(InInfo))
            {
            }

            void SetDragDropPayloadData() const override
            {
                if (FileInfo.IsLAsset())
                {
                    FStringView Path(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                    if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
                    {
                        DragDrop::SetAssetPayload(*Data);
                        return;
                    }
                }
                DragDrop::SetFilePayload(FStringView(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size()));
            }

            void DrawTooltip() const override;
            
            NODISCARD bool HasContextMenu() override { return true; }

            NODISCARD FStringView GetName() const override
            {
                return VFS::FileName(FileInfo.PathSource, true);
            }
            
            NODISCARD const VFS::FFileInfo& GetFileInfo() const { return FileInfo; }
            NODISCARD FStringView GetPathSource() const { return FileInfo.PathSource; }
            NODISCARD FStringView GetVirtualPath() const { return FileInfo.VirtualPath; }
            NODISCARD bool IsAsset() const { return FileInfo.IsLAsset(); }
            NODISCARD bool IsDirectory() const { return FileInfo.IsDirectory(); }
            NODISCARD FString GetExtension() const { return FileInfo.GetExt(); }
            NODISCARD bool IsProtected() const { return bProtected; }
            NODISCARD EIconKind GetIconKind() const { return IconKind; }

            NODISCARD FStringView GetTypeLabel() const override
            {
                const char* Label = TypeLabel.IsNone() ? nullptr : TypeLabel.c_str();
                return Label != nullptr ? FStringView(Label) : FStringView();
            }

            /** Uppercased asset class / extension shown under the name, and the key the type filter and
             *  the search box both match on. Resolved once during the rebuild; the registry lookup and the
             *  string work behind it must never run per-frame in the draw loop.
             *
             *  Interned rather than stored inline: these items come from a block allocator, and an
             *  FFixedString member is a 255-char inline buffer that pushed the item past a whole block. */
            void SetTypeLabel(const FFixedString& InLabel)
            {
                TypeLabel = InLabel.empty() ? NAME_None : FName(InLabel.c_str());
            }

        private:

            static EIconKind ClassifyIcon(const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory()) { return EIconKind::Directory; }
                if (Info.IsLAsset())    { return EIconKind::Asset; }

                const FString Ext = Info.GetExt();
                if (Ext == ".rml")  { return EIconKind::Markup; }
                if (Ext == ".rcss") { return EIconKind::Stylesheet; }
                if (Ext == ".wav")  { return EIconKind::Audio; }
                if (Ext == ".cs")   { return EIconKind::CSharpScript; }
                return EIconKind::Generic;
            }

            bool            bProtected = false;
            FName           TypeLabel;
            VFS::FFileInfo  FileInfo;
            EIconKind       IconKind = EIconKind::Generic;
        };

        LUMINA_SINGLETON_EDITOR_TOOL(FContentBrowserEditorTool)

        FContentBrowserEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Content Browser", nullptr)
        {
        }
        
        bool OnEvent(FEvent& Event) override;
        
        void RefreshContentBrowser();

        /** Navigates to the folder holding VirtualPath, then selects and scrolls to its tile. */
        void BrowseToAsset(FStringView VirtualPath);

        // Selects the asset and opens its inline rename as soon as its tile appears. Called right after
        // creating one, so a new asset lands ready to be named instead of keeping the factory default.
        void QueueRenameAfterCreate(FStringView VirtualPath);
        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FOLDER_MULTIPLE; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override { }

        void Update(const FUpdateContext& UpdateContext) override;
        void EndFrame() override;

        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

        void HandleContentBrowserDragDrop(FStringView DropPath, FStringView PayloadPath);

        // Registry entry for the single highlighted asset, or null when the selection is empty, is a
        // multi-selection, or is a folder / loose file. Backs the property widgets' "use selected".
        NODISCARD const FAssetData* GetSelectedAsset() const;

    private:

        void OpenDeletionWarningPopup(const FContentBrowserTileViewItem* Item);

        // Single funnel for every delete: play guard, variant guard, referencer fixup, then the destroy queue.
        void RequestDeletion(TVector<FFixedString> Paths, int32 ProtectedCount);

        // Names the prefab variants that would be orphaned, empty when the set is safe to delete.
        FFixedString FindBlockingPrefabVariants(const TVector<FFixedString>& Paths) const;

        // Retargets everything pointing at this asset onto another one, leaving the asset itself in place.
        void OpenReplaceReferencesModal(const FContentBrowserTileViewItem* Item);

        void OnProjectLoaded();

        // Shared frame around an importer's reflected settings: source header, scrolling property table,
        // footer buttons. Returns true when the user confirmed.
        bool DrawImportWindow(CImporter* Importer, const FImportRequest& Request, int32 RemainingCount,
                              bool& bShouldClose, bool& bOutApplyToAll);

        // Free package path for importing SourcePath into the current folder, or empty if none could be
        // found. Uniquifying on the source filename is not enough: imports become packages, whose names
        // carry no extension, so the check has to run in the package namespace.
        FFixedString MakeUniqueImportDestination(FStringView SourcePath);

        // Destinations handed out for imports that have not finished yet. Every import runs on its own
        // task, so none of them have created their package by the time the batch is queued; without this
        // two sources in one batch happily claim the same name and the second create fails.
        THashSet<FFixedString> ReservedImportPaths;

        // Takes ownership of Importer: runs the build stage off-thread, then destroys it.
        void StartImport(CImporter* Importer, const FImportRequest& Request);
        void ProcessNextImport();

        void TryImport(const FFixedString& Path);
        void TryImport(const TVector<FFixedString>& Paths);

        // "Reimport From File...": picks a source file and swaps its contents onto an asset that already
        // exists, keeping its object, GUID and path. Deliberately NOT "import the same file again", which
        // would mint a second asset called "<Name>_1" and leave every existing reference on the old one.
        void DrawReimportAssetMenuItem(const FContentBrowserTileViewItem* ContentItem, bool bIsProtected);

        // Runs the importer's normal parse + options dialogue against an existing asset, then performs the
        // swap. AssetGUID rather than a pointer: the parse is async, and the asset can be destroyed
        // (project reload, asset deleted) before it comes back.
        void StartReimport(const FGuid& AssetGUID, CClass* ImporterClass, const FFixedString& SourceFile);

        // Off-thread half of a reimport: the swap itself, then save + registry notify. Takes ownership of
        // Importer.
        void FinishReimport(const FGuid& AssetGUID, CImporter* Importer, const FFixedString& SourceFile);

        // Property table over the importer being configured; rebuilt when the dialogue opens.
        TUniquePtr<FPropertyTable> ImportSettingsTable;

        // Files still waiting on an options window. Drained one at a time by ProcessNextImport.
        TVector<FFixedString> PendingImports;
        bool bImportWindowOpen = false;
        bool bApplyImportSettingsToAll = false;
        
        void DrawDirectoryBrowser(bool bIsFocused, ImVec2 Size);

        // Walks the left tree down to PendingDirectoryReveal, expanding lazily-built nodes on the way,
        // then selects and scrolls to it. Runs after the tree has been rebuilt for the frame.
        void RevealPendingDirectory();

        void DrawContentBrowser(bool bIsFocused, ImVec2 Size);
        
        void DrawAssetContextMenu(FContentBrowserTileViewItem* ContentItem);

        // Menu shown when a marquee (or Ctrl-click / Ctrl+A) has gathered more than one tile. Deliberately
        // sparse: only the operations that mean the same thing applied to a mixed bag of folders and files.
        void DrawMultiSelectionContextMenu(const TVector<FTileViewItem*>& Items);

        // One confirmation for the whole set, protected entries filtered out rather than aborting the batch.
        void DeleteSelectedItems(const TVector<FTileViewItem*>& Items);

        // Duplicate entry. Disabled for packages holding sub-objects a property copy would alias.
        void DrawDuplicateAssetMenuItem(const FContentBrowserTileViewItem* ContentItem, bool bIsProtected);

        // Prefab-only: mints a child prefab that inherits from this one. Nothing for other asset types.
        void DrawCreateVariantMenuItem(const FContentBrowserTileViewItem* ContentItem);
        
        void DrawContentDirectoryContextMenu();
        
        float                       ContentBrowserTileSize = 84.0f;

        FDeferredActionRegistry     ActionRegistry;

        // One watcher per content root (Game + each enabled plugin mount), each carrying its
        // own virtual-path prefix so reload/content broadcasts resolve to the right root.
        struct FContentWatcher
        {
            TUniquePtr<FDirectoryWatcher>   Watcher;
            FFixedString                    VirtualPrefix;
            size_t                          WatchRootLen = 0;
        };
        TVector<FContentWatcher>    Watchers;
        
        FTreeListView               DirectoryListView;
        FTreeListViewContext        DirectoryContext;

        FTileViewWidget             ContentBrowserTileView;
        FTileViewContext            ContentBrowserTileViewContext;

        FFixedString                SelectedPath;

        // Browsed-folder history. Every navigation goes through NavigateTo so back/forward stay honest;
        // assigning SelectedPath directly moves the browser without recording it.
        void NavigateTo(FStringView Path);
        void NavigateBack();
        void NavigateForward();

        NODISCARD bool CanNavigateBack() const { return !NavBackStack.empty(); }
        NODISCARD bool CanNavigateForward() const { return !NavForwardStack.empty(); }

        TVector<FFixedString>       NavBackStack;
        TVector<FFixedString>       NavForwardStack;

        // One-shot browse-to targets, consumed by the next tile rebuild / directory tree draw.
        FFixedString                PendingBrowseToPath;

        // Asset just created and waiting to be dropped into inline rename. Survives rebuilds until the
        // tile exists, since factory creation can complete asynchronously.
        FFixedString                PendingRenamePath;
        FFixedString                PendingDirectoryReveal;

        // Asset-class -> shown. Rebuilt from the classes actually present in the browsed content, so a
        // type with no factory (meshes, skeletons, animations, prefabs) still gets an entry; user choices
        // survive the rebuild.
        THashMap<FName, bool>       FilterState;

        // Substring match over the tile name, case-insensitive. Empty shows everything.
        FFixedString                SearchText;

        // Rebuilds FilterState from the asset registry, preserving existing choices.
        void RefreshFilterClasses();

        NODISCARD bool PassesFilters(const VFS::FFileInfo& FileInfo, FStringView TypeLabel) const;

        // "CStaticMesh" -> "STATICMESH"; a loose file -> its extension. Empty for directories.
        NODISCARD static FFixedString MakeTypeLabel(const VFS::FFileInfo& FileInfo);
    };
}
