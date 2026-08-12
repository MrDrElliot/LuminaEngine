#pragma once
#include "UI/Tools/EditorTool.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class CObject;

    // Dockable Asset Registry view: every known asset grouped by class with loaded state, ref-count,
    // CPU/disk size, and (for the selected asset) which loaded objects reference it.
    class FAssetRegistryEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FAssetRegistryEditorTool)

        FAssetRegistryEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Asset Registry", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_DATABASE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        // One row of resolved per-asset state, rebuilt each frame from the registry.
        struct FAssetRow
        {
            FGuid           GUID;
            FName           Name;
            FName           Class;
            FFixedString    Path;
            CObject*        Loaded      = nullptr;
            int32           RefCount    = 0;
            uint64          CpuBytes    = 0;
            uint64          DiskBytes   = 0;
        };

        // A loaded object that holds a reference to the selected asset.
        struct FReferencer
        {
            FName   Name;
            FName   Class;
            FName   Package;
        };

        // Contiguous span of VisibleRows belonging to one category, in display order.
        struct FRowGroup
        {
            FString Category;
            uint32  Start = 0;
            uint32  Count = 0;
        };

        void DrawWindow(bool bIsFocused);
        void DrawStatsBar(const TVector<FAssetRow>& Rows);
        void DrawFilterBar();
        void DrawAssetTable();
        void DrawDetailsPanel(const TVector<FAssetRow>& Rows);

        // BaseIndex maps a row's position in this slice back to its index in VisibleRows, which is what
        // shift-range selection needs -- without it a range could not span two category groups.
        void DrawAssetTableRows(const TVector<const FAssetRow*>& Rows, uint32 BaseIndex);

        // Resolves the filtered, sorted, display-ordered row list into VisibleRows/VisibleGroups. Built
        // once per frame BEFORE anything draws, so Ctrl+A has something to select against.
        void BuildVisibleRows(const TVector<FAssetRow>& Rows);

        // Ctrl+A over the whole visible set. Ignored while a text field has focus, or typing "a" into
        // the search box with Ctrl held would silently select the project.
        void HandleSelectionShortcuts();

        void ApplyRowClick(uint32 VisibleIndex);

        // Checkbox-per-class type filter; seeded from the registry each frame, preserving prior choices.
        void DrawTypeFilterMenu();
        uint32 CountHiddenTypes() const;

        // Opens the confirm -> progress -> results modal over the current selection (or, with nothing
        // selected, everything currently visible).
        void OpenResaveModal();

        // Saves a bounded number of packages and returns; called once per frame while the modal runs, so
        // a project-wide resave stays interactive instead of freezing the editor for minutes.
        void TickResave();

        // Sums approximate CPU-side bulk data held by a loaded asset (textures/meshes/materials).
        static uint64 EstimateCpuBytes(CObject* Asset);

        // Walks every live object's reflected references; records those pointing at Target.
        void RebuildReferencers(CObject* Target);

        bool PassesFilter(const FAssetRow& Row) const;

        // Focus row: drives the details pane and is the anchor for shift-range. Always a member of
        // SelectedGUIDs while a selection exists.
        FGuid           SelectedGUID;

        // Multi-selection. Keyed by GUID rather than row index because the row list is rebuilt every
        // frame from the registry and indices move whenever a filter changes.
        THashSet<FGuid> SelectedGUIDs;

        // Index into VisibleRows of the last plain/ctrl click; INDEX_NONE when there is no anchor.
        int32           RangeAnchor = INDEX_NONE;

        FString         SearchFilter;
        char            SearchBuffer[256] = {};

        // Per-class visibility. Absent == visible; only classes the user has hidden are stored false,
        // so a newly imported asset type shows up rather than being silently filtered out.
        THashMap<FName, bool> TypeVisibility;

        bool            bShowLoadedOnly   = false;
        bool            bGroupByCategory  = true;

        // Display-ordered filtered rows, rebuilt each frame. Points into DrawWindow's local row array,
        // so it is only valid for the duration of one DrawWindow call.
        TVector<const FAssetRow*> VisibleRows;
        TVector<FRowGroup>        VisibleGroups;

        // Resave job. Runs across frames in TickResave; the modal owns its lifetime.
        enum class EResavePhase : uint8 { Confirm, Running, Done };

        EResavePhase        ResavePhase = EResavePhase::Confirm;
        TVector<FGuid>      ResaveQueue;
        THashSet<CPackage*> ResavedPackages;   // one save per package, however many exports it holds
        uint32              ResaveIndex  = 0;
        uint32              ResaveSaved  = 0;
        uint32              ResaveFailed = 0;
        FName               ResaveCurrent;

        // Disk sizes hit the filesystem, so cache them; cleared by Refresh.
        THashMap<FGuid, uint64> DiskSizeCache;

        // Referencer list is computed on selection change (and Refresh), not per-frame.
        FGuid               CachedReferencerTarget;
        TVector<FReferencer> Referencers;
    };
}
