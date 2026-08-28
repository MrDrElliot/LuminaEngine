#pragma once

#include <string>
#include <vector>
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Threading/Atomic.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "Renderer/RHITexture.h"
#include "UI/RmlUiBridge.h"
#include "UI/ColorTextEdit/TextEditor.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

namespace Rml
{
    class Context;
}

namespace Lumina
{
    // Editor for raw .rml files. Not in the CObject asset pipeline: documents stay as
    // plain text so Lua/RmlUi can include each other without a binary package layer.
    class FRmlUiEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FRmlUiEditorTool)

        FRmlUiEditorTool(IEditorToolContext* Context, FStringView VirtualPath);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FILE_CODE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnSave() override;
        void DrawHelpMenu() override;

        bool IsUnsavedDocument() override { return bBufferDirty; }

        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        enum class EBgMode : uint8 { Checker, Solid, Transparent };
        enum class EPalette : uint8 { Dark, Light };

        void LoadFromDisk();
        // bAlwaysReport: report the parse outcome even when it matches the previous reload. Pass it for a
        // deliberate action; leave it off for the debounced auto reload.
        void ReloadDocument(bool bAlwaysReport = false);
        // Width/Height are the VISIBLE canvas size in screen pixels; the texture is padded up from it.
        void EnsurePreviewTarget(uint32 Width, uint32 Height);
        void TearDownPreview();
        void StartWatching();
        // Copy persisted CRmlUiEditorSettings values into the cached member fields. Run at construction
        // and on the OnSettingsSaved live-refresh.
        void PullSettings();
        void ApplyEditorSettings();
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
        void PersistSettings() const;

        // A read-only view of the document, since editing happens by typing in the code editor.
        struct FCompSlot
        {
            FString Id;
            FString Tag;
            ImVec2  OffsetPx{0.0f, 0.0f};   // border-box, context px
            ImVec2  SizePx{0.0f, 0.0f};
            int32   Depth = 0;
            int32   ChildCount = 0;
            FString AssignedSrc;            // src of the slotted <template>, parsed from the buffer
        };

        void RefreshCompositionSlots();     // pull live-DOM slots, then stamp buffer-parsed assignments

        void DrawHierarchyPanel();

        // Rebuilds HierarchyTree from the current source text. Wired to RebuildTreeFunction, so the widget
        // calls it whenever the tree is marked dirty rather than the panel rebuilding every frame.
        void RebuildHierarchyTree(FTreeListView& Tree);
        void DrawInspectorPanel();
        void DrawSlotOverlays(const ImVec2& CanvasMin, float ScalePx);

        const FCompSlot* FindSlot(const FString& Id) const;

        // Moves the code editor's caret to the selected element's open tag.
        void RevealSelectionInCode();

        // Surfaces what RmlUi complained about during the last reload as an editor notification. Silent
        // when the outcome is unchanged, so the debounced auto reload does not toast on a loop.
        // bAlwaysReport opts a deliberate action (save, Reload, Re-render) out of that: acting on a file
        // and hearing nothing back reads as success, whatever the reason for the silence.
        void ReportReloadDiagnostics(bool bLoaded, const TVector<RmlUi::FRmlDiagnostic>& Diagnostics,
                                     bool bAlwaysReport);

        // False for .rcss, which has no DOM; the panels explain that rather than showing an empty tree.
        bool HasElementTree() const { return !bIsStylesheet; }

        // Canvas overlay density. Drawing a filled, labeled box for every slot buries a real document in
        // overlapping rectangles, so the default keeps context slots as thin outlines.
        enum class EOverlayDetail : uint8 { All, Assigned, SelectionOnly };

        TVector<FCompSlot>   CompSlots;
        FString              SelectedSlotId;
        FString              SelectedTag;
        // Byte offset of the selection's open tag, so an element without an id is still addressable.
        size_t               SelectedOpenLt = ~size_t(0);
        FString              HoveredSlotId;          // what the overlay draws; promoted from Pending each frame
        FString              PendingHoveredSlotId;   // written by the tree/canvas producers, expires every frame
        bool                 bShowSlotOverlays = true;
        EOverlayDetail       OverlayDetail = EOverlayDetail::Assigned;
        char                 HierarchySearch[64] = {};

        // The document tree. Expansion, filtering, indentation and row hit-testing all live in the widget;
        // this tool only supplies nodes on rebuild and reacts to the callbacks.
        FTreeListView        HierarchyTree;
        FTreeListViewContext HierarchyContext;

        // Rebuilt from the source text, so any edit that changes the markup has to invalidate it.
        bool                 bHierarchyDirty = true;

        // Assignment parse is keyed on the undo index so the buffer is copied only when it actually
        // changed; slot geometry still refreshes every frame (cheap DOM walk, no text copy).
        std::string          CompAssignText;
        size_t               CompAssignUndoIndex = ~size_t(0);
        bool                 bCompAssignDirty = true;

        FString                     VirtualPath;

        // Outcome of the last reload, so a repeat of the same result stays quiet. Starts clean so the
        // first parse of an already-broken document still reports.
        FString                     LastReloadDiagnosticSignature;
        bool                        bLastReloadWasClean = true;
        FString                     ParentDir;

        // Retargets VirtualPath when this file is renamed/moved, so a save writes the new file.
        FDelegateHandle             FileRenamedHandle;
        // Live-refresh subscription: re-pull + re-apply when CRmlUiEditorSettings is saved from the
        // global Settings panel, so palette/appearance edits show up without reopening the editor.
        FDelegateHandle             SettingsSavedHandle;
        // .rcss stylesheets are edited the same as .rml, but can't render on
        // their own -- the preview wraps them in a component specimen.
        bool                        bIsStylesheet = false;

        TextEditor                  CodeEditor;
        std::string                 LastSyncedText;     // matches disk + last preview reload
        bool                        bBufferDirty = false;
        bool                        bAutoReload = true;

        // Per-frame churn guard: the status bar's byte count is recomputed only when the undo index moves.
        size_t                      CachedDocBytes = 0;             // status-bar byte count
        size_t                      CachedStatusUndoIndex = ~size_t(0);

        Rml::Context*               PreviewContext = nullptr;
        RHI::FManagedTexture        PreviewTarget;

        // The VISIBLE canvas: the Rml context's dimensions and its render viewport, in screen pixels.
        // One context pixel is one screen pixel, so overlay/drag math converts 1:1 and text rasterizes
        // at its displayed size instead of being resampled on the way to the pane.
        uint32                      PreviewWidth = 0;
        uint32                      PreviewHeight = 0;

        // The backing texture, padded up to a block so a continuous resize or zoom drag reuses one
        // allocation. Only the top-left PreviewWidth x PreviewHeight region is ever sampled.
        uint32                      PreviewRTWidth = 0;
        uint32                      PreviewRTHeight = 0;

        // Canvas resolution selected by the user (decoupled from pane size).
        // 0,0 = "fit to pane".
        uint32                      CanvasWidth = 0;
        uint32                      CanvasHeight = 0;
        int                         ResolutionPreset = 0;     // 0 = Fit
        
        // Auto DPI tracks the engine's dp convention (ratio = canvas height / 1080) so dp-authored UI
        // previews at the same relative size it will in-game, instead of a fixed ratio that overflows
        // small canvases. The slider becomes a manual override when this is off.
        bool                        bAutoDpi = true;
        float                       PreviewDpiScale = 1.5f;
        bool  bPreviewHovered = false;
        bool  bPreviewLeftDown = false;
        bool  bPreviewRightDown = false;
        float                       ViewZoom = 1.0f;          // pan/zoom over the canvas
        ImVec2                      ViewPan{0.0f, 0.0f};

        // Background.
        EBgMode                     BgMode = EBgMode::Checker;
        ImVec4                      BgColor{0.10f, 0.10f, 0.12f, 1.0f};

        // Overlays.
        bool                        bShowGrid = false;
        float                       GridSize = 32.0f;         // canvas-space px
        ImVec4                      GridColor{1.0f, 1.0f, 1.0f, 0.10f};

        bool                        bShowSafeZones = false;
        float                       SafeZoneAction = 0.95f;   // 95%, action safe
        float                       SafeZoneTitle = 0.90f;    // 90%, title safe
        ImVec4                      SafeZoneColor{1.0f, 0.85f, 0.30f, 0.65f};

        bool                        bShowRulers = false;

        float                       EditorFontScale = 1.25f;
        int                         EditorTabSize = 4;
        float                       EditorLineSpacing = 1.0f;
        bool                        bEditorShowWhitespace = false;
        bool                        bEditorShowLineNumbers = true;
        bool                        bEditorShowMiniMap = true;
        bool                        bEditorReadOnly = false;
        bool                        bAutoIndent = true;
        bool                        bShowMatchingBrackets = true;
        bool                        bCompletePairedGlyphs = true;
        bool                        bInsertSpacesOnTabs = false;
        bool                        bTrimTrailingOnSave = false;
        EPalette                    EditorPalette = EPalette::Dark;

        int                         GotoLineBuffer = 1;
        bool                        bRequestOpenGoto = false;

        FDirectoryWatcher           FileWatcher;
        TAtomic<bool>               bExternalChangePending{false};
    };
}
