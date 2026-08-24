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

        // --- Composition designer ---
        // A "slot" is any live-DOM element with an id; a "widget" is a <template>-rooted .rml in the
        // project. Assigning a widget to a slot writes <link type="text/template"> + <template src> into
        // the buffer (the single source of truth), which the normal reload re-parses. Assignment state is
        // read back from the buffer text since a <template src> directive leaves no DOM element.
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
        struct FCompWidget
        {
            FString DisplayName;            // file name without extension
            FString VirtualPath;            // full virtual path (backs the <link href>)
            FString TemplateName;           // <template name="..."> -> the value in <template src="...">
            FString ContentSlotId;          // <template content="..."> (informational)
        };

        void RefreshCompositionSlots();     // pull live-DOM slots, then stamp buffer-parsed assignments
        void RefreshWidgetLibrary();        // scan the document's folder tree for <template> widgets

        // The designer is three docked panels rather than one stacked column: selecting in the tree used to
        // reflow everything below it, because the inspector was drawn inline between the hierarchy and the
        // palette. Hierarchy and Inspector are visible together; the Palette tabs alongside the Hierarchy.
        void DrawHierarchyPanel();

        // Rebuilds HierarchyTree from the current source text. Wired to RebuildTreeFunction, so the widget
        // calls it whenever the tree is marked dirty rather than the panel rebuilding every frame.
        void RebuildHierarchyTree(FTreeListView& Tree);
        void DrawInspectorPanel();
        void DrawPalettePanel();
        void DrawSlotOverlays(const ImVec2& CanvasMin, float ScalePx);

        // Right-click menu for a tree row. Takes the element's source facts directly so it doesn't depend on
        // the .cpp-local parse node type.
        void DrawElementContextMenu(const std::string& Tag, const std::string& Id, size_t OpenLt, bool bAssigned);

        void AssignWidgetToSlot(const FString& SlotId, int WidgetIndex);
        void ClearSlotAssignment(const FString& SlotId);

        const FCompSlot* FindSlot(const FString& Id) const;

        // UMG-style authoring: insert a new building block (kElementPrimitives) as the last child of
        // TargetSlotId (empty = the document body), auto-id'd so it's immediately a manipulable slot;
        // RemoveElement deletes a slot's element (open..close) from the buffer entirely.
        void AddElement(const FString& TargetSlotId, int PrimitiveIndex);
        void RemoveElement(const FString& SlotId);

        // Surfaces what RmlUi complained about during the last reload as an editor notification. Silent
        // when the outcome is unchanged, so the debounced auto reload does not toast on a loop.
        // bAlwaysReport opts a deliberate action (save, Reload, Re-render) out of that: acting on a file
        // and hearing nothing back reads as success, whatever the reason for the silence.
        void ReportReloadDiagnostics(bool bLoaded, const TVector<RmlUi::FRmlDiagnostic>& Diagnostics,
                                     bool bAlwaysReport);

        // True when this document can host elements at all. False for .rcss, which has no DOM -- the
        // designer panels explain that instead of offering authoring controls that can only fail.
        bool SupportsElementAuthoring() const { return !bIsStylesheet; }

        // Makes sure the buffer has a `</body>` to insert before, writing the scaffold when the document
        // lacks one (new/empty file) or expanding a self-closed `<body/>`. False = can't author here.
        bool EnsureDocumentBody();
        // Reorder: swap a slot's element with its previous/next sibling. Inline text: replace a text-leaf
        // element's inner text (no child elements) from the inspector.
        void MoveElement(const FString& SlotId, bool bUp);
        void SetElementInnerText(const FString& SlotId, const std::string& NewText);
        // Returns the element's id, assigning a fresh one (written into its open tag) if it had none -- so any
        // element in the hierarchy, id'd or not, becomes addressable the moment the user acts on it.
        FString EnsureElementId(const std::string& Tag, size_t OpenLt, const std::string& ExistingId);

        // Repositioning writes a `transform: translate(...)` to the slot element's inline style -- a relative
        // nudge that never changes the element's position mode or anchoring, so it stays responsive on resize
        // (unlike position:absolute + left/top, which pins it). CommitSlotVisual sets the transform so the
        // element's rendered top-left lands at TargetVisualPx; CommitSlotMove is the drag-delta wrapper.
        // SetSlotInlineStyle merges arbitrary props into the element's style="" attribute in the buffer.
        void CommitSlotMove(const FString& SlotId, ImVec2 DeltaPx);
        void CommitSlotVisual(const FString& SlotId, ImVec2 TargetVisualPx, bool bSnapToGrid);
        void SetSlotInlineStyle(const FString& SlotId, const std::vector<std::pair<std::string, std::string>>& Sets);

        // Canvas overlay density. Drawing a filled, labeled box for every slot buries a real document in
        // overlapping rectangles, so the default keeps context slots as thin outlines.
        enum class EOverlayDetail : uint8 { All, Assigned, SelectionOnly };

        TVector<FCompSlot>   CompSlots;
        TVector<FCompWidget> CompWidgets;
        FString              SelectedSlotId;
        FString              HoveredSlotId;          // what the overlay draws; promoted from Pending each frame
        FString              PendingHoveredSlotId;   // written by the tree/canvas producers, expires every frame
        bool                 bShowSlotOverlays = true;
        EOverlayDetail       OverlayDetail = EOverlayDetail::Assigned;
        bool                 bWidgetLibraryDirty = true;
        char                 WidgetSearch[64] = {};
        char                 HierarchySearch[64] = {};

        // The document tree. Expansion, filtering, indentation and row hit-testing all live in the widget;
        // this tool only supplies nodes on rebuild and reacts to the callbacks.
        FTreeListView        HierarchyTree;
        FTreeListViewContext HierarchyContext;

        // Rebuilt from the source text, so any edit that changes the markup has to invalidate it.
        bool                 bHierarchyDirty = true;

        bool                 bDraggingSlot = false;
        FString              DraggingSlotId;
        ImVec2               DragDeltaPx{0.0f, 0.0f}; // accumulated drag, context px

        // Inspector field caches: DragFloat owns these while being scrubbed/typed; we snap them back to
        // the live DOM value whenever the field isn't active (so canvas drags + reloads flow in).
        float                InspLeft = 0.0f;
        float                InspTop = 0.0f;
        float                InspWidth = 0.0f;
        float                InspHeight = 0.0f;
        char                 InspText[256] = {}; // inline inner-text edit buffer for text-leaf slots
        float                InspFontSize = 16.0f;
        ImVec4               InspColor{1.0f, 1.0f, 1.0f, 1.0f};
        FString              InspColorSyncId;    // re-pull InspColor only when the selection changes

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
