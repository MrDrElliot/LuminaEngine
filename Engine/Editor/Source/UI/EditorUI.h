#pragma once

#include <imgui/imgui.h>
#include "Events/EventProcessor.h"
#include "nlohmann/json.hpp"
#include "Tools/EditorToolContext.h"
#include "Tools/EditorToolModal.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/ImGui/ImGuiX.h"



namespace Lumina
{
    class FGamePreviewTool;
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
    class EDITOR_API FEditorUI : public IDevelopmentToolUI, public IEditorToolContext
    {
    public:

        FEditorUI() = default;
        ~FEditorUI() override = default;
        LE_NO_COPYMOVE(FEditorUI);

        // Begin IEventHandler
        bool OnEvent(FEvent& Event) override;
        //~ End IEventHandler

        void Initialize(const FUpdateContext& UpdateContext) override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;

        void OnStartFrame(const FUpdateContext& UpdateContext) override;
        void OnUpdate(const FUpdateContext& UpdateContext) override;
        void OnEndFrame(const FUpdateContext& UpdateContext) override;

        void DestroyTool(const FUpdateContext& UpdateContext, FEditorTool* Tool);

        void PushModal(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction) override;

        void OpenScriptEditor(FStringView ScriptPath) override;

        /** Opens a script source file in the editor chosen in CScriptEditorSettings (falls back to the OS association). */
        static void OpenScriptInExternalEditor(FStringView ScriptPath);

        void OpenAssetEditor(const FGuid& AssetGUID) override;
        void OpenFileEditor(FStringView VirtualPath) override;
        void BrowseToAsset(FStringView VirtualPath) override;
        const FAssetData* GetContentBrowserSelectedAsset() const override;
        void OnDestroyAsset(CObject* InAsset) override;


        template<typename T, typename... Args>
        requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
        T* CreateTool(Args&&... args);

        // Find an active tool by its singleton-style unique type id, or nullptr if not present.
        template<typename T>
        requires eastl::is_base_of_v<FEditorTool, T>
        T* FindTool() const;

        // Returns true if a tool of type T is currently alive (not pending destroy).
        template<typename T>
        requires eastl::is_base_of_v<FEditorTool, T>
        bool IsToolActive() const { return FindTool<T>() != nullptr; }

        // Toggle a singleton-style debug tool. Creates one with the given args
        // if none exists; otherwise schedules the live one for destruction.
        template<typename T, typename... Args>
        requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
        void ToggleTool(Args&&... args);

        // Convenience for the Tools menu, draws a MenuItem whose check state
        // mirrors the tool's existence and whose click toggles it.
        template<typename T, typename... Args>
        requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
        void DrawToolMenuItem(const char* Label, Args&&... args);

        void VerifyDirtyPackages();

        // Ctrl+P quick-open: a filterable list of every registered asset, Enter to open.
        void OpenAssetSearchModal();

        // File > Save: saves the focused tool, which is what Ctrl+S already does.
        void SaveActiveTool();

        // File > Save All: every dirty package, routed through its open editor tool when there is one
        // so type-specific save work still runs.
        void SaveAllDirtyPackages();

        // Re-entry guard for the dirty-packages prompt; true while the dialog is open.
        // A member (not a function-local static) so Cancel can re-arm it.
        bool bVerifyingDirtyPackages = false;

    private:

        // Shared tail of tool creation: initializes the tool and queues it. Passes
        // through nullptr so registry-driven creation can chain without null checks.
        FEditorTool* FinalizeNewTool(FEditorTool* Tool);

        // Registers the built-in asset/file editors with FEditorToolRegistry.
        // Plugins add their own via the same registry in StartupModule.
        void RegisterBuiltinEditorTools();

        FEditorTool* FindToolByTypeID(uint32 TypeID) const;

        // --- Session tab restore (CEditorSessionSettings) ---

        // Adds Tool's tab to the persisted list and writes it out immediately. Key is "asset:<guid>" or
        // "file:<path>". A key already in the list (i.e. this open IS the restore) is not re-appended,
        // which is also what keeps replay from rewriting the file it is reading.
        void RecordSessionTab(FEditorTool* Tool, FString Key);

        // Drops Tool's tab from the persisted list and writes it out. No-op for unrecorded tools.
        void ForgetSessionTab(FEditorTool* Tool);

        // Reopens the persisted tabs in order, skipping any whose asset or file no longer resolves, then
        // prunes those dead entries. Runs once, on the first update, so the asset registry is populated.
        void RestoreSessionTabs();

        void EditorToolLayoutCopy(FEditorTool* SourceTool);

        /** Returns false if the tool wants to close */
        bool SubmitToolMainWindow(const FUpdateContext& UpdateContext, FEditorTool* EditorTool, ImGuiID TopLevelDockspaceID);
        void DrawToolContents(const FUpdateContext& UpdateContext, FEditorTool* Tool);


        // Multiplayer PIE: spawn/destroy the extra-player Game Preview tools (players 2..N).
        // Driven by FWorldEditorTool's OnGamePreviewStart/StopRequested delegates.
        void CreateGameViewportTool();
        void DestroyGameViewportTool();

        void DrawTitleBarMenu(const FUpdateContext& UpdateContext);

        // Titlebar stats, composed once per frame so the right-hand region can be sized from their
        // measured width rather than a constant that silently clips when a stat is added.
        struct FTitleBarStats
        {
            TFixedString<64> Perf;
            TFixedString<64> Objects;
            TFixedString<64> Memory;
            float            Width = 0.0f;
        };

        FTitleBarStats BuildTitleBarStats(const FUpdateContext& UpdateContext);
        void DrawTitleBarInfoStats(const FTitleBarStats& Stats);

        // Footer drawer: status-bar toggle that slides up as a transient overlay
        // when undocked; "Dock in Layout" pins it back into the dockspace.
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

        // Footer-button / shortcut action: focus when docked, else toggle the drawer.
        void ActivateDrawer(FFooterDrawer& Drawer);

        // Makes a drawer visible without ever closing it: focus when docked, else open.
        void ShowDrawer(FFooterDrawer& Drawer);

        FFooterDrawer* FindDrawerForTool(const FEditorTool* Tool);
        void DrawFileMenu();
        void DrawProjectMenu();
        void DrawToolsMenu();
        void DrawHelpMenu();
        void OpenProjectDialog();
        void NewProjectDialog();
        void ProjectCreatedDialog(FStringView ProjectFile);

        // Queues a modal for next frame to chain dialogs (Open -> New), since
        // FEditorModalManager allows only one active modal at a time.
        void DeferShowDialog(TFunction<void()> Action) { PendingDialogAction = std::move(Action); }

        void OnProjectLoaded();

        // Opens Project.EditorStartupMap if asset discovery has already found it. Returns false when the
        // map is configured but not in the registry yet, which is the caller's cue to wait for discovery
        // rather than to report an error.
        bool TryOpenEditorStartupMap();

        // Live only between a project load and the startup map opening. Discovery runs asynchronously, so
        // the map usually is not in the registry yet when the project finishes loading; this retries on
        // each registry update instead of blocking the main thread until the scan drains.
        FDelegateHandle PendingStartupMapHandle;

        void HandleUserInput(const FUpdateContext& UpdateContext);

    private:

        FEditorModalManager                             ModalManager;
        TFunction<void()>                               PendingDialogAction;

        ImGuiX::ApplicationTitleBar                     TitleBar;
        ImGuiWindowClass                                EditorWindowClass;

        FGamePreviewTool*                               GamePreviewTool = nullptr;
        FWorldEditorTool*                               WorldEditorTool = nullptr;

        // Tracks the game-owns-input state so the ImGui stand-down flags are reasserted while the game owns
        // input and cleared ONCE on the way out -- never per-frame, which would clobber the editor camera's
        // own NoMouse capture during RMB-look.
        bool                                            bWasGameOwningInput = false;

        // Extra-player (players 2..N) Game Preview tools spawned for multiplayer PIE. Each tool owns its
        // own PIE world (torn down on the tool's Deinitialize), so this list is non-owning bookkeeping.
        struct FExtraGamePreview
        {
            CWorld*           World = nullptr;
            FGamePreviewTool* Tool  = nullptr;
        };
        TVector<FExtraGamePreview>                      ExtraGamePreviews;

        FConsoleLogEditorTool*                          ConsoleLogTool = nullptr;
        FContentBrowserEditorTool*                      ContentBrowser = nullptr;
        TVector<FEditorTool*>                           EditorTools;

        TVector<FFooterDrawer>                          FooterDrawers;
        FEditorTool*                                    OpenDrawer = nullptr;   // tool whose drawer is open (nullptr = none)
        float                                           DrawerOpenAmount = 0.0f; // 0..1 slide animation
        bool                                            bDrawerActivatedThisFrame = false; // guards focus-loss auto-close
        ImGuiID                                         MainDockspaceID = 0;    // root editor dockspace, for "Dock in Layout"
        FEditorTool*                                    LastActiveTool = nullptr;
        FString                                         FocusTargetWindowName; // If this is set we need to switch focus to this window

        THashMap<CObject*, FEditorTool*>                ActiveAssetTools;
        THashMap<FString, FEditorTool*>                 ActiveFileTools;

        // Live tool -> its persisted session key, so closing a tab can find the entry to remove.
        THashMap<FEditorTool*, FString>                 ToolSessionKeys;

        // Armed by OnProjectLoaded, consumed on the next update. NOT armed at construction: the tab list
        // lives in the project's own config, so restoring before a project is loaded would read an empty
        // list and burn the one shot.
        bool                                            bSessionRestorePending = false;

        // Set for the duration of editor teardown. Deinitialize closes every tool through DestroyTool,
        // which would otherwise "forget" each tab on the way out and leave an empty list on disk -- the
        // exact state we are trying to preserve. Only a user closing a tab may shorten the list.
        bool                                            bTearingDownTools = false;

        // Retry hook for tabs whose asset had not been scanned into the registry yet at restore time.
        FDelegateHandle                                 SessionRestoreRetryHandle;
        TQueue<FEditorTool*>                            ToolsPendingAdd;
        TQueue<FEditorTool*>                            ToolsPendingDestroy;

        bool                                            bShowDearImGuiDemoWindow = false;
        bool                                            bShowImGuiStyleEditor = false;
        bool                                            bShowImPlotDemoWindow = false;

        float                                           SmoothedFPS = 0.0f;
        float                                           SmoothedFrameTime = 0.0f;   // <= 0 seeds from the first sample
        float                                           SmoothedMemoryMiB = 0.0f;   // <= 0 seeds from the first sample

        // Time constants, not per-frame blend factors: a fixed factor makes the settling time depend on
        // the frame rate, so the readout was sluggish at 30 fps and twitchy at 300 -- worst at exactly the
        // moment you are reading it. Seconds to reach ~63% of a step change, frame rate independent.
        static constexpr float                          FrameTimeSmoothingSeconds = 0.2f;
        static constexpr float                          MemorySmoothingSeconds = 1.0f;
    };

    template <typename T, typename ... Args>
    requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
    T* FEditorUI::CreateTool(Args&&... args)
    {
        return static_cast<T*>(FinalizeNewTool(Memory::New<T>(std::forward<Args>(args)...)));
    }

    template <typename T>
    requires eastl::is_base_of_v<FEditorTool, T>
    T* FEditorUI::FindTool() const
    {
        return static_cast<T*>(FindToolByTypeID(T::s_toolTypeID));
    }

    template <typename T, typename... Args>
    requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
    void FEditorUI::ToggleTool(Args&&... args)
    {
        if (FEditorTool* Existing = FindToolByTypeID(T::s_toolTypeID))
        {
            ToolsPendingDestroy.push(Existing);
            return;
        }
        CreateTool<T>(std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    requires eastl::is_base_of_v<FEditorTool, T> && eastl::is_constructible_v<T, Args...>
    void FEditorUI::DrawToolMenuItem(const char* Label, Args&&... args)
    {
        const bool bActive = IsToolActive<T>();
        if (ImGui::MenuItem(Label, nullptr, bActive))
        {
            ToggleTool<T>(std::forward<Args>(args)...);
        }
    }
}
