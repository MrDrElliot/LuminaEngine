#pragma once

#include "Containers/FunctionRef.h"
#include "Containers/Queue.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include <imgui/imgui.h>
#include "Events/EventProcessor.h"
#include "Tools/EditorToolContext.h"
#include "Tools/EditorToolModal.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompileScheduler.h"
#include "Particles/ParticleCompileScheduler.h"
#include "Tools/Impostor/ImpostorBaker.h"
#include "Play/StandaloneLauncher.h"

struct ImGuiSettingsHandler;



namespace Lumina
{
    class FGamePreviewTool;
    class FCodeWorkspaceTool;
    class FEditorToolModal;
    class FContentBrowserEditorTool;
    class FPrimitiveDrawManager;
    class FConsoleLogEditorTool;
    class FWorldEditorTool;
    class FEditorTool;
    class CWorld;
}


namespace Lumina
{
    // EDITOR_API: exported so editor plugins can create/toggle tools via CreateTool/ToggleTool.
    class EDITOR_API FEditorUI : public IDevelopmentToolUI, public IEditorToolContext, public IObjectReferenceProvider
    {
    public:

        //~ IObjectReferenceProvider, since a tool is not a CObject and its held assets are reachable nowhere else.

        const char* GetReferenceProviderName() const override { return "editor tools"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;


        FEditorUI() = default;
        ~FEditorUI() override = default;
        LE_NO_COPYMOVE(FEditorUI);

        // Begin IEventHandler
        bool OnEvent(FEvent& Event) override;
        //~ End IEventHandler

        // Chords that must work while a game owns the keyboard, so they run before any viewport sees the key.
        bool OnShortcutEvent(FEvent& Event);

        void Initialize(const FUpdateContext& UpdateContext) override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;

        void OnStartFrame(const FUpdateContext& UpdateContext) override;
        void OnEndFrame(const FUpdateContext& UpdateContext) override;

        /**
         * Per-frame interval for one tool's preview world: 0 for the tool being worked in, 1/MaxBackgroundFPS
         * for a visible-but-unfocused one. Six open material editors are six full scene renders a frame; five
         * of them are being glanced at, and glancing does not need 165Hz.
         */
        double GetToolWorldUpdateInterval(const CWorld* ToolWorld, bool bFocused) const;

        void DestroyTool(const FUpdateContext& UpdateContext, FEditorTool* Tool);

        void AddPendingTools();

        void PushModal(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction) override;

        void OpenScriptEditor(FStringView ScriptPath) override;

        void OpenScriptInExternalEditor(FStringView ScriptPath, int32 Line) override;

        void OpenAssetEditor(const FGuid& AssetGUID) override;
        void OpenFileEditor(FStringView VirtualPath) override;
        FEditorTool* FindFileEditor(FStringView VirtualPath) const override;

        // The code workspace, opened and recorded in the session when it is not already up.
        FCodeWorkspaceTool* OpenCodeWorkspace();
        void BrowseToAsset(FStringView VirtualPath) override;
        bool BrowseToFolder(FStringView Folder, FStringView Search);
        FAssetDataPtr GetContentBrowserSelectedAsset() const override;

        // The one file selected in the Content Browser, asset or not, for slots that take loose files.
        NODISCARD FFixedString GetContentBrowserSelectedFilePath() const;
        void OnDestroyAsset(CObject* InAsset) override;
        void OnAssetDeleted(FStringView AssetPath);


        template<typename T, typename... Args>
        requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
        T* CreateTool(Args&&... args);

        // A tool that lives in the footer bar and docks like the console, which is also how a plugin adds one.
        template<typename T, typename... Args>
        requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
        T* CreateDrawerTool(const char* Icon, const char* Label, ImGuiKeyChord Shortcut, Args&&... args);

        // The tool editing this asset, or null when nothing has it open.
        NODISCARD FEditorTool* FindAssetEditor(CObject* Asset) const;

        // One open tab as an outside caller sees it: its window name, what it edits, and whether it can go.
        struct FTabInfo
        {
            FString Name;
            FString Id;
            FString AssetGuid;
            bool    bUnsaved  = false;
            bool    bFocused  = false;
            bool    bClosable = false;
        };

        void ForEachTab(const TFunction<void(const FTabInfo&)>& Functor) const;

        // Resolves a full window name, the id after its ###, or a unique case-insensitive substring.
        NODISCARD FEditorTool* FindTab(FStringView Name, FString& OutError) const;

        // Focus lands on the next frame, since a caller outside the tool cannot be inside an ImGui frame.
        bool FocusTab(FStringView Name, FString& OutError);

        // Closes as the tab's X would; false with a reason for the world editor or an unsaved tab kept.
        bool CloseTab(FStringView Name, bool bDiscardUnsaved, FString& OutError);

        // Undocks the tab and places it at a screen rect, which outside the main window gives it its own OS window.
        bool FloatTab(FStringView Name, const FVector2& ScreenPosition, const FVector2& Size, FString& OutError);

        // Find an active tool by its singleton-style unique type id, or nullptr if not present.
        template<typename T>
        requires std::is_base_of_v<FEditorTool, T>
        T* FindTool() const;

        // Returns true if a tool of type T is currently alive (not pending destroy).
        template<typename T>
        requires std::is_base_of_v<FEditorTool, T>
        bool IsToolActive() const { return FindTool<T>() != nullptr; }

        // Toggle a singleton-style debug tool, creating it with the given args or scheduling the live one for destruction.
        template<typename T, typename... Args>
        requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
        void ToggleTool(Args&&... args);

        // A singleton tool window the menus toggle, and OpenToolPanel opens by the label after its icon.
        struct FToolPanel
        {
            const char*  Label  = nullptr;
            uint32       TypeID = 0;
            FEditorTool* (*Create)(FEditorUI&) = nullptr;
        };

        template<typename T>
        requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, FEditorUI*>
        static constexpr FToolPanel MakeToolPanel(const char* Label)
        {
            return { Label, T::s_toolTypeID, [](FEditorUI& UI) -> FEditorTool* { return UI.CreateTool<T>(&UI); } };
        }

        // A MenuItem whose check state mirrors the panel's existence and whose click toggles it.
        void DrawToolMenuItem(const FToolPanel& Panel);

        bool IsToolPanelOpen(const FToolPanel& Panel) const;

        // Every window the Tools and File menus toggle, named by its label after the icon.
        void ForEachToolPanel(TFunctionRef<void(FStringView, bool)> Visit) const;

        // By exact name or a unique part of one, focusing a window that is already open.
        bool OpenToolPanel(FStringView Name, FString& OutError);

        // Closing a window that is not open succeeds and does nothing.
        bool CloseToolPanel(FStringView Name, FString& OutError);

        void VerifyDirtyPackages();

        // Lists the packages and unsaved file editors with a checkbox each, and saves, discards or cancels the pending exit.
        void OpenUnsavedChangesDialog(TVector<CPackage*> Packages, TVector<FEditorTool*> FileTools);

        // The asset editor of the package, else the tool whose world lives in it.
        FEditorTool* FindToolForPackage(const CPackage* Package) const;

        // Dirty, or an open world whose package stays clean while its external entities wait to be written.
        NODISCARD bool IsPackageUnsaved(const CPackage* Package) const;
        NODISCARD TVector<CPackage*> GatherUnsavedPackages() const;

        static constexpr int32 MaxListedUnsavedItems = 8;

        // A count line, then up to MaxListedUnsavedItems labels, then "+N more" for the rest.
        NODISCARD static TVector<FString> DescribeUnsavedItems(const TVector<FString>& Labels);

        bool SaveToolBeforeClose(FEditorTool* Tool);

        void PromptCloseTools(TVector<FEditorTool*> Tools);

        // Ctrl+P quick-open: a filterable list of every registered asset, Enter to open.
        void OpenAssetSearchModal();

        // Queues an impostor bake for every mesh the open world's forests and painted foliage scatter.
        void BakeFoliageImpostors();

        FImpostorBaker& GetImpostorBaker() { return ImpostorBaker; }

        // File > Save: saves the focused tool, which is what Ctrl+S already does.
        void SaveActiveTool();

        // File > Save All, routed through each open editor tool so type-specific save work still runs.
        void SaveAllDirtyPackages() override;
        void SavePackages(TVector<CPackage*> Packages) override;
        void OpenPropertyMatrix(TVector<FGuid> Assets) override;
        void OpenSettings(CClass* Page, FStringView Search = {}) override;
        bool ConfirmWritable(TVector<FString> VirtualPaths, TMoveOnlyFunction<void()> Then, bool bThenSaves = true) override;

        struct FSaveAllResult
        {
            uint32           Saved = 0;
            TVector<FString> FailedPaths;
        };

        // Save All without the toasts, reporting what it wrote and which packages refused.
        FSaveAllResult SaveAllDirtyPackagesSilently();

        // Exits a few frames from now past the unsaved-changes prompt and any other modal, so an agent's reply goes out first.
        void QuitWithoutPrompt();

        FStandaloneLauncher& GetStandaloneLauncher() { return StandaloneLauncher; }

        // Empty targets the world editor; otherwise the tab name of another scene editor, such as a prefab.
        const FString& GetSceneTargetTab() const { return SceneTargetTab; }
        void SetSceneTargetTab(FString InTab) { SceneTargetTab = std::move(InTab); }

        // Re-entry guard for the dirty-packages prompt, a member so Cancel can re-arm it.
        bool bVerifyingDirtyPackages = false;

        int32 PendingQuitFrames = 0;
        bool  bQuitWithoutPrompt = false;

    private:

        // Shared tail of tool creation, passing nullptr through so registry-driven creation chains without null checks.
        FEditorTool* FinalizeNewTool(FEditorTool* Tool);

        // Registers the built-in asset and file editors with FEditorToolRegistry, which plugins also use.
        void RegisterBuiltinEditorTools();

        FEditorTool* FindToolByTypeID(uint32 TypeID) const;
        
        // Adds Tool's tab to the persisted list, skipping a key already present so replay does not rewrite the file it reads.
        void RecordSessionTab(FEditorTool* Tool, FString Key);

        // Drops Tool's tab from the persisted list and writes it out. No-op for unrecorded tools.
        void ForgetSessionTab(FEditorTool* Tool);

        // Reopens the persisted tabs and prunes dead entries, running on the first update so the asset registry is populated.
        void RestoreSessionTabs();

        void EditorToolLayoutCopy(FEditorTool* SourceTool);

        /** Returns false if the tool wants to close */
        bool SubmitToolMainWindow(const FUpdateContext& UpdateContext, FEditorTool* EditorTool, ImGuiID TopLevelDockspaceID);
        void DrawToolContents(const FUpdateContext& UpdateContext, FEditorTool* Tool);

        // Right-click menu on a tool's dock tab, called between the tool's Begin() and End() where the last item is its tab.
        void DrawToolTabContextMenu(FEditorTool* EditorTool);

        // Closes a tool exactly as its tab's X does, so batch and manual closes cannot drift.
        void RequestCloseTool(FEditorTool* Tool);
        bool CanCloseTool(const FEditorTool* Tool) const;

        FEditorTool* FindToolByWindowName(const char* WindowName) const;


        // Multiplayer PIE spawns and destroys the extra-player Game Preview tools (players 2..N).
        void CreateGameViewportTool();
        void DestroyGameViewportTool();

        void DrawTitleBarMenu(const FUpdateContext& UpdateContext);

        // Packages with unsaved changes, which walks every package so per-frame callers must throttle it.
        uint32 CountDirtyPackages() const;

        // Titlebar stats, composed once per frame so the right-hand region is sized from their measured width.
        struct FTitleBarStat
        {
            const char*      Label   = nullptr;
            const char*      Tooltip = nullptr;
            TFixedString<48> Value;
        };

        struct FTitleBarStats
        {
            FTitleBarStat Entries[5];
            float         LabelGap = 0.0f;
            float         EntryGap = 0.0f;
            float         Width    = 0.0f;
        };

        FTitleBarStats BuildTitleBarStats(const FUpdateContext& UpdateContext);
        void DrawTitleBarInfoStats(const FTitleBarStats& Stats);

        // Footer drawer that slides up as a transient overlay when undocked.
        struct FFooterDrawer
        {
            FEditorTool*  Tool       = nullptr;
            const char*   Icon       = nullptr;
            const char*   Label      = nullptr;
            ImGuiKeyChord Shortcut   = 0;       // 0 = no shortcut
            bool          bDocked    = false;   // true = drawn in the dock layout, not as a drawer
            float         HeightFrac = 0.4f;    // open height as a fraction of the work area (session-only)
        };

        // Bottom status bar: drawer toggle buttons (left) + status text (right).
        void DrawStatusBar(const FUpdateContext& UpdateContext);

        // Renders the currently-open drawer as a slide-up overlay above the status bar.
        void DrawFooterDrawer(const FUpdateContext& UpdateContext);

        // Stands in for a drag source whose drawer has closed, so ImGui does not fall back to its "..." tooltip.
        void DrawOrphanedDragPreview();

        // Footer-button / shortcut action: focus when docked, else toggle the drawer.
        void ActivateDrawer(FFooterDrawer& Drawer);

        // Makes a drawer visible without ever closing it: focus when docked, else open.
        void ShowDrawer(FFooterDrawer& Drawer);

        FFooterDrawer* FindDrawerForTool(const FEditorTool* Tool);

        void RegisterFooterDrawer(FEditorTool* Tool, const char* Icon, const char* Label, ImGuiKeyChord Shortcut);
        void SetDrawerDocked(FFooterDrawer& Drawer, bool bDocked);

        // Docked drawers are saved in the layout file beside the dock nodes they sit in, so a layout reset clears both.
        static constexpr const char* DockedDrawersSettingsName = "LuminaDockedDrawers";
        void RegisterDockedDrawerSettings();
        static void* ReadDockedDrawersOpen(ImGuiContext* Context, ImGuiSettingsHandler* Handler, const char* Name);
        static void ReadDockedDrawersLine(ImGuiContext* Context, ImGuiSettingsHandler* Handler, void* Entry, const char* Line);
        static void WriteDockedDrawers(ImGuiContext* Context, ImGuiSettingsHandler* Handler, ImGuiTextBuffer* Buffer);
        void DrawFileMenu();
        void DrawProjectMenu();
        void DrawToolsMenu();
        void DrawHelpMenu();
        void OpenProjectDialog();
        void NewProjectDialog();
        void ProjectCreatedDialog(FStringView ProjectFile);

        // Queues a modal for next frame, since FEditorModalManager allows only one active modal at a time.
        void DeferShowDialog(TFunction<void()> Action) { PendingDialogAction = std::move(Action); }

        void OnProjectLoaded();

        // Opens Project.EditorStartupMap, returning false while discovery has not found it yet.
        bool TryOpenEditorStartupMap();

        // Confirms opening another world during PIE, taking a GUID since nothing roots the asset across frames.
        void PromptOpenWorldDuringPlay(const FGuid& AssetGUID, const FString& WorldName);

        // Live only until the startup map opens, retrying on each registry update instead of blocking on discovery.
        FDelegateHandle PendingStartupMapHandle;

        // Lives here rather than in the material editor because a material with a closed editor still renders stale.
        FDelegateHandle AssetDataChangedHandle;

        void HandleUserInput(const FUpdateContext& UpdateContext);

        // Launches the bundled Tracy profiler, reporting the path it looked at when missing.
        void LaunchTracyProfiler();

    private:

        FEditorModalManager                             ModalManager;
        TFunction<void()>                               PendingDialogAction;

        char                                            NewProjectName[256] = "MyProject";
        char                                            NewProjectPath[512] = "";
        FString                                         NewProjectError;

        FString                                         VisualStudio2022Exe;
        FString                                         VisualStudio2026Exe;
        FString                                         VSCodeExe;
        FString                                         RiderExe;

        ImGuiX::FApplicationTitleBar                    TitleBar;
        ImGuiWindowClass                                EditorWindowClass;

        FWorldEditorTool*                               WorldEditorTool = nullptr;

        // The stand-down flags clear once on exit, since clearing per frame would clobber the editor camera's NoMouse capture.

        struct FShortcutLayer final : IEventHandler
        {
            FEditorUI* Owner = nullptr;
            bool OnEvent(FEvent& Event) override { return Owner->OnShortcutEvent(Event); }
        };
        FShortcutLayer                                  ShortcutLayer;

        // Extra-player Game Preview tools, non-owning since each tool owns its own PIE world.
        struct FExtraGamePreview
        {
            CWorld*           World = nullptr;
            FGamePreviewTool* Tool  = nullptr;
        };
        TVector<FExtraGamePreview>                      ExtraGamePreviews;

        FConsoleLogEditorTool*                          ConsoleLogTool = nullptr;
        FContentBrowserEditorTool*                      ContentBrowser = nullptr;
        TVector<FEditorTool*>                           EditorTools;
        FMaterialCompileScheduler                       MaterialCompiles;
        FParticleCompileScheduler                       ParticleCompiles;
        FImpostorBaker                                  ImpostorBaker;
        FStandaloneLauncher                             StandaloneLauncher;

        FString                                         SceneTargetTab;

        TVector<FFooterDrawer>                          FooterDrawers;

        // Drawers the layout file lists as docked, kept for a plugin drawer that registers later or is absent this run.
        TVector<FString>                                SavedDockedDrawers;
        FEditorTool*                                    OpenDrawer = nullptr;   // tool whose drawer is open (nullptr = none)
        float                                           DrawerOpenAmount = 0.0f; // 0..1 slide animation
        bool                                            bDrawerActivatedThisFrame = false; // guards focus-loss auto-close

        // A drag begun inside the drawer closes it once it leaves, uncovering the drop target underneath.
        bool                                            bDrawerDragSeen = false;
        bool                                            bDrawerDragStartedInDrawer = false;
        ImGuiID                                         MainDockspaceID = 0;    // root editor dockspace, for "Dock in Layout"

        /**
         * The bottom strip a drawer docks into, split off the dockspace on first use.
         */
        ImGuiID                                         BottomDockID = 0;

        /**
         * Drawer awaiting a bottom dock, applied at the top of the next frame.
         */
        FEditorTool*                                    PendingBottomDockTool = nullptr;
        float                                           PendingBottomDockHeightFrac = 0.0f;

        // Splits the bottom strip off the dockspace, seeding the ratio so a docked drawer keeps its overlay height.
        ImGuiID GetOrCreateBottomDockID(float HeightFrac);

        FEditorTool*                                    LastActiveTool = nullptr;
        // The last focused tool that has a world, which keeps full rate while a worldless tool such as the Profiler has focus.
        FEditorTool*                                    LastActiveWorldTool = nullptr;
        FString                                         FocusTargetWindowName; // If this is set we need to switch focus to this window

        struct FFloatRequest
        {
            FString  WindowName;
            FVector2 Position;
            FVector2 Size;
            bool     bUndockQueued = false;
        };
        FFloatRequest                                   PendingFloat;

        THashMap<CObject*, FEditorTool*>                ActiveAssetTools;
        THashMap<FString, FEditorTool*>                 ActiveFileTools;

        // Live tool -> its persisted session key, so closing a tab can find the entry to remove.
        THashMap<FEditorTool*, FString>                 ToolSessionKeys;

        // Armed by OnProjectLoaded rather than construction, since the tab list lives in the project config.
        bool                                            bSessionRestorePending = false;

        // Set during teardown so closing every tool does not forget each tab and leave an empty list on disk.
        bool                                            bTearingDownTools = false;

        // Retry hook for tabs whose asset had not been scanned into the registry yet at restore time.
        FDelegateHandle                                 SessionRestoreRetryHandle;
        FDelegateHandle                                 AssetDeletedHandle;

        // Session keys already opened, since re-opening an open tool during the retry pass yanks focus away.
        THashSet<FString>                               RestoredSessionTabs;
        TQueue<FEditorTool*>                            ToolsPendingAdd;
        TQueue<FEditorTool*>                            ToolsPendingDestroy;

        bool                                            bShowDearImGuiDemoWindow = false;
        bool                                            bShowImGuiStyleEditor = false;
        bool                                            bShowImPlotDemoWindow = false;

        float                                           SmoothedFPS = 0.0f;
        float                                           SmoothedFrameTime = 0.0f;   // <= 0 seeds from the first sample
        float                                           SmoothedMemoryMiB = 0.0f;   // <= 0 seeds from the first sample
        float                                           SmoothedGPUMemoryMiB = 0.0f; // <= 0 seeds from the first sample
        float                                           GPUMemoryBudgetMiB = 0.0f;  // device-local budget; not smoothed, it does not move

        // Time constants in seconds to reach 63% of a step, so settling is frame rate independent.
        static constexpr float                          FrameTimeSmoothingSeconds = 0.2f;
        static constexpr float                          MemorySmoothingSeconds = 1.0f;

        // The memory readouts query the OS and the GPU driver, which cost more than the text they feed, so they sample a few times a second.
        static constexpr double                         MemorySampleSeconds = 0.25;
        double                                          LastMemorySampleSeconds = -1.0;

        // Recounted on a timer since counting walks every package and a hint need not be frame-accurate.
        uint32                                          DirtyPackageCount = 0;
        float                                           DirtyPackageScanCountdown = 0.0f;
        static constexpr float                          DirtyPackageScanInterval = 0.5f;
    };

    template <typename T, typename ... Args>
    requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
    T* FEditorUI::CreateTool(Args&&... args)
    {
        return static_cast<T*>(FinalizeNewTool(Memory::New<T>(std::forward<Args>(args)...)));
    }

    template <typename T, typename ... Args>
    requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
    T* FEditorUI::CreateDrawerTool(const char* Icon, const char* Label, ImGuiKeyChord Shortcut, Args&&... args)
    {
        T* Tool = CreateTool<T>(std::forward<Args>(args)...);
        RegisterFooterDrawer(Tool, Icon, Label, Shortcut);
        return Tool;
    }

    template <typename T>
    requires std::is_base_of_v<FEditorTool, T>
    T* FEditorUI::FindTool() const
    {
        return static_cast<T*>(FindToolByTypeID(T::s_toolTypeID));
    }

    template <typename T, typename... Args>
    requires std::is_base_of_v<FEditorTool, T> && std::is_constructible_v<T, Args...>
    void FEditorUI::ToggleTool(Args&&... args)
    {
        if (FEditorTool* Existing = FindToolByTypeID(T::s_toolTypeID))
        {
            ToolsPendingDestroy.Enqueue(Existing);
            return;
        }
        CreateTool<T>(std::forward<Args>(args)...);
    }
}
