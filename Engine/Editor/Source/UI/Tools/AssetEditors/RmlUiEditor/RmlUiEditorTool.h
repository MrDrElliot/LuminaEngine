#pragma once

#include <string>
#include <vector>
#include "RmlBindingAnalysis.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Threading/Atomic.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "Renderer/RHITexture.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/RmlUiBridge.h"
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/Languages/RmlLanguage.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Rml
{
    class Context;
}

namespace Lumina
{
    // Edits raw .rml and .rcss text rather than a package, so documents can include each other without a binary layer.
    class FRmlUiEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FRmlUiEditorTool)

        FRmlUiEditorTool(IEditorToolContext* Context, FStringView VirtualPath);

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnSave() override;
        void OnScriptsReloaded(const Scripting::FScriptReload& Reload) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FILE_CODE; }
        bool IsUnsavedDocument() override { return bBufferDirty; }
        bool HasUnsavedFileChanges() override { return bBufferDirty; }

        // The text is bulk data, so a whole snapshot per edit would cost more than the undo is worth.
        bool WantsAutomaticAssetUndo() override { return false; }

    private:

        enum class EBgMode : uint8 { Checker, Solid, Transparent };

        // Filled boxes for every slot bury a real document, so the default draws context slots as thin outlines.
        enum class EOverlayDetail : uint8 { All, Assigned, SelectionOnly };

        struct FCompSlot
        {
            FString Id;
            FString Tag;
            ImVec2  OffsetPx{0.0f, 0.0f};
            ImVec2  SizePx{0.0f, 0.0f};
            int32   Depth = 0;
            int32   ChildCount = 0;
            FString AssignedSrc;
        };

        void LoadFromDisk();
        void WriteBufferToDisk();

        // A deliberate action passes bAlwaysReport, so acting on the file always answers; the debounced auto reload does not.
        void ReloadDocument(bool bAlwaysReport = false);
        void ReportReloadDiagnostics(bool bLoaded, const TVector<RmlUi::FRmlDiagnostic>& Diagnostics, bool bAlwaysReport);

        // Width and Height are the visible canvas in screen pixels, and the texture is padded up from them.
        void EnsurePreviewTarget(uint32 Width, uint32 Height);
        void TearDownPreview();
        void StartWatching();

        // Takes the persisted CRmlUiEditorSettings, at construction and whenever they are saved.
        void PullSettings();

        void DrawPreviewToolbar();
        void DrawPreviewCanvas();
        void DrawEditorToolbar();
        void DrawEditorStatusBar();
        void DrawSnippetsPopup();
        void DrawFormatPopup();
        void DrawHelpPopup();
        void DrawGotoLinePopup();
        void HandleEditorShortcuts();
        void InsertSnippet(const char* Snippet);
        void JumpToLine(int32 Line);

        // Pulls the live DOM's slots, then stamps the assignments parsed from the buffer onto them.
        void RefreshCompositionSlots();
        const FCompSlot* FindSlot(const FString& Id) const;
        void DrawSlotOverlays(const ImVec2& CanvasMin, float ScalePx);

        // False for .rcss, which has no DOM, so the panels explain that rather than showing an empty tree.
        bool HasElementTree() const { return !bIsStylesheet; }

        void DrawHierarchyPanel();
        void RebuildHierarchyTree(FTreeListView& Tree);
        void DrawInspectorPanel();
        void RevealSelectionInCode();

        // Rebuilds the preview's stand-in data models from the C# types the document names, before it reloads.
        void RefreshDesignModels(const std::string& Body);

        // Unbinds and drops the preview's class instances, which a model still bound to the document would keep reading.
        void ReleaseDesignObjects();

        void ApplyBindingMarkers();
        void ConfigureAutoComplete();
        void GatherBindingCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out);
        void DrawDataPanel();
        void DrawModelSection(int32 ModelIndex);
        void DrawElementBindings();
        void PollDesignActivity();

        FString                             VirtualPath;
        FString                             ParentDir;
        bool                                bIsStylesheet = false;

        FRmlLanguage                        Language;
        FCodeEditor                         CodeEditor;

        // Matches disk and the last preview reload.
        std::string                         LastSyncedText;
        bool                                bBufferDirty = false;
        bool                                bDiskChangedWhileDirty = false;
        bool                                bAutoReload = true;
        int32                               GotoLineBuffer = 1;
        bool                                bRequestOpenGoto = false;

        // A repeat of the same outcome stays quiet, and starting clean lets a document broken from the start still report.
        FString                             LastReloadDiagnosticSignature;
        bool                                bLastReloadWasClean = true;

        FDirectoryWatcher                   FileWatcher;
        TAtomic<bool>                       bExternalChangePending{false};
        FDelegateHandle                     FileRenamedHandle;
        FDelegateHandle                     SettingsSavedHandle;
        FDelegateHandle                     ScriptsWillReloadHandle;
        bool                                bScriptsReloaded = false;

        TVector<FCompSlot>                  CompSlots;
        FString                             SelectedSlotId;
        FString                             SelectedTag;

        // The selection's open tag, so an element without an id is still addressable.
        size_t                              SelectedOpenLt = ~size_t(0);

        // The producers write Pending each frame, which Update promotes, since the hierarchy publishes hover a frame early.
        FString                             HoveredSlotId;
        FString                             PendingHoveredSlotId;
        bool                                bShowSlotOverlays = true;
        EOverlayDetail                      OverlayDetail = EOverlayDetail::Assigned;
        char                                HierarchySearch[64] = {};

        FTreeListView                       HierarchyTree;
        FTreeListViewContext                HierarchyContext;
        bool                                bHierarchyDirty = true;

        // Keyed on the buffer version, so the text is copied only when it changed.
        std::string                         CompAssignText;
        uint64                              CompAssignVersion = ~0ull;
        bool                                bCompAssignDirty = true;

        RmlBinding::FDocumentBindings       Bindings;
        TVector<FUIDesignModel>             DesignModels;
        TVector<bool>                       DesignDescribed;

        // Per design model, the transient instance of its class and the object model binding it, both null when inferred.
        TVector<TStrongObjectPtr<CObject>>  DesignObjects;
        TVector<void*>                      DesignObjectModels;
        TVector<RmlBinding::FProblem>       BindingProblems;
        TVector<FUIModelInfo>               AvailableModels;

        // Values typed into the Data panel, keyed "Model.Member", which survive the reload every edit causes.
        THashMap<FString, FString>          DesignOverrides;
        THashMap<FString, int32>            DesignRowCounts;
        bool                                bDesignReloadPending = false;

        // Commands the preview fired, newest last, since an editor preview has no script to run them.
        TVector<FString>                    FiredCommands;

        Rml::Context*                       PreviewContext = nullptr;
        RHI::FManagedTexture                PreviewTarget;

        // The visible canvas in screen pixels, one context pixel to one screen pixel, so text rasterizes at its displayed size.
        uint32                              PreviewWidth = 0;
        uint32                              PreviewHeight = 0;

        // Padded up to a block so a resize or zoom drag reuses one allocation; only the visible corner is sampled.
        uint32                              PreviewRTWidth = 0;
        uint32                              PreviewRTHeight = 0;

        // The user's canvas resolution, decoupled from the pane, where 0 by 0 fits the pane.
        uint32                              CanvasWidth = 0;
        uint32                              CanvasHeight = 0;
        int32                               ResolutionPreset = 0;

        // Tracks the engine's dp convention, canvas height over 1080, so dp-authored UI previews at its in-game size.
        bool                                bAutoDpi = true;
        float                               PreviewDpiScale = 1.5f;
        float                               ViewZoom = 1.0f;
        ImVec2                              ViewPan{0.0f, 0.0f};
        bool                                bPreviewHovered = false;
        bool                                bPreviewLeftDown = false;
        bool                                bPreviewRightDown = false;

        EBgMode                             BgMode = EBgMode::Checker;
        ImVec4                              BgColor{0.10f, 0.10f, 0.12f, 1.0f};
        bool                                bShowGrid = false;
        float                               GridSize = 32.0f;
        ImVec4                              GridColor{1.0f, 1.0f, 1.0f, 0.10f};
        bool                                bShowSafeZones = false;
        float                               SafeZoneAction = 0.95f;
        float                               SafeZoneTitle = 0.90f;
        ImVec4                              SafeZoneColor{1.0f, 0.85f, 0.30f, 0.65f};
        bool                                bShowRulers = false;
    };
}
