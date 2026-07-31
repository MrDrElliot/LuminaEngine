#pragma once
#include "EditorTool.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/LuminaCommonTypes.h"
#include "FileSystem/FileSystem.h"
#include "Memory/SmartPtr.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "Tools/Actions/DeferredActions.h"
#include "Tools/UI/ImGui/imfilebrowser.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/Widgets/TileViewWidget.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"

namespace Lumina
{
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
        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FORMAT_LIST_BULLETED_TYPE; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override { }

        void Update(const FUpdateContext& UpdateContext) override;
        void EndFrame() override;

        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

        void HandleContentBrowserDragDrop(FStringView DropPath, FStringView PayloadPath);
        
    private:

        void OpenDeletionWarningPopup(const FContentBrowserTileViewItem* Item, const TFunction<void(EYesNo)>& Callback = TFunction<void(EYesNo)>());
        void OnProjectLoaded();

        void TryImport(const FFixedString& Path);
        
        void DrawDirectoryBrowser(bool bIsFocused, ImVec2 Size);

        // Walks the left tree down to PendingDirectoryReveal, expanding lazily-built nodes on the way,
        // then selects and scrolls to it. Runs after the tree has been rebuilt for the frame.
        void RevealPendingDirectory();

        void DrawContentBrowser(bool bIsFocused, ImVec2 Size);
        
        void DrawAssetContextMenu(FContentBrowserTileViewItem* ContentItem);
        
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

        // One-shot browse-to targets, consumed by the next tile rebuild / directory tree draw.
        FFixedString                PendingBrowseToPath;
        FFixedString                PendingDirectoryReveal;

        THashMap<FName, bool>       FilterState;
    };
}
