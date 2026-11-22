#pragma once
#include "UI/Tools/EditorTool.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Delegates/Delegate.h"
#include "GUID/GUID.h"
#include <atomic>

namespace Lumina
{
    class CObject;

    // Dockable view of every known asset grouped by class, with the loaded objects referencing the selected one.
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

        // One asset, snapshotted when the registry changes, with its loaded state rechecked a slice at a time.
        struct FAssetRow
        {
            FGuid           GUID;
            FName           Name;
            FName           Class;
            uint64          DiskBytes   = 0;
            uint64          CpuBytes    = 0;
            int32           RefCount    = 0;
            bool            bLoaded     = false;
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
            FName   Category;
            uint32  Start    = 0;
            uint32  Count    = 0;
            uint32  Loaded   = 0;
            uint64  CpuBytes = 0;
        };

        void DrawWindow(bool bIsFocused);
        void DrawStatsBar();
        void DrawFilterBar();
        void DrawAssetTable();
        void DrawDetailsPanel();

        // BaseIndex maps a row back to VisibleRows so shift-range selection can span category groups.
        void DrawAssetTableRows(const FAssetRow* const* GroupRows, uint32 Count, uint32 BaseIndex);

        // Snapshots the registry into Rows and both sort orders, which only a registry change makes stale.
        void RebuildRows();

        // Rechecks a slice of rows each frame, so a sweep of every asset never lands on one frame.
        void StepLiveState();

        // Filters a presorted order into VisibleRows/VisibleGroups, only when a filter or the rows changed.
        void BuildVisibleRows();

        // Ctrl+A is ignored while a text field has focus, or it would select the whole project.
        void HandleSelectionShortcuts();

        void ApplyRowClick(uint32 VisibleIndex);

        // Checkbox-per-class type filter; seeded from the registry each frame, preserving prior choices.
        void DrawTypeFilterMenu();
        uint32 CountHiddenTypes() const;

        // Covers the current selection, or everything visible when nothing is selected.
        void OpenResaveModal();

        // Saves a bounded number of packages per frame so a project-wide resave stays interactive.
        void TickResave();

        // Sums approximate CPU-side bulk data held by a loaded asset (textures/meshes/materials).
        static uint64 EstimateCpuBytes(CObject* Asset);

        // Saved packages that import the asset, from the registry's reverse map, so it costs nothing per click.
        void RebuildRegistryReferencers(const FGuid& Target);

        // Serializes every live object to find unsaved references too, which is slow enough to be on demand only.
        void RebuildLiveReferencers(CObject* Target);

        bool PassesFilter(const FAssetRow& Row) const;

        // Focus row that drives the details pane and anchors shift-range, and is always in SelectedGUIDs.
        FGuid           SelectedGUID;

        // Keyed by GUID because row indices move whenever the list is rebuilt or a filter changes.
        THashSet<FGuid> SelectedGUIDs;

        // Index into VisibleRows of the last plain/ctrl click; Constants::kIndexNone when there is no anchor.
        int32           RangeAnchor = Constants::kIndexNone;

        FString         SearchFilter;
        char            SearchBuffer[256] = {};

        TVector<FAssetRow>          Rows;
        TVector<uint32>             RowsByName;
        TVector<uint32>             RowsByCategory;
        THashMap<FGuid, uint32>     RowIndexByGUID;
        TVector<FName>              AssetTypes;
        uint32                      LiveSweepCursor = 0;
        bool                        bSweepChangedLoaded = false;

        uint32                      LoadedCount     = 0;
        uint64                      TotalCpuBytes   = 0;
        uint64                      TotalDiskBytes  = 0;

        // Set from whichever thread broadcasts the registry update, and consumed on the next draw.
        std::atomic<bool>           bRowsStale { true };
        bool                        bVisibleStale = true;
        double                      NextRowsRebuildSeconds  = 0.0;
        FDelegateHandle             RegistryUpdatedHandle;

        // Absent means visible, so a newly imported asset type is not silently filtered out.
        THashMap<FName, bool> TypeVisibility;

        bool            bShowLoadedOnly   = false;
        bool            bGroupByCategory  = true;

        // Display-ordered filtered rows pointing into Rows, so a row rebuild always rebuilds these too.
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

        // Referencer list is computed on selection change (and Refresh), not per-frame.
        FGuid               CachedReferencerTarget;
        TVector<FReferencer> Referencers;
        bool                bReferencersFromLiveScan = false;
    };
}
