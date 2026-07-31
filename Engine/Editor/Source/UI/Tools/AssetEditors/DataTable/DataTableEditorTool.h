#pragma once

#include "Assets/AssetTypes/DataTable/DataTableCSV.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    class CStruct;
    class FProperty;
    class FPropertyTable;

    /** Editor for CDataTable: a virtualized grid of rows over the table's row struct, plus a property
     *  panel for the selected row.
     *
     *  The grid only shows text-convertible columns, because a cell is one line of text. Anything the
     *  grid cannot represent (nested structs, arrays, colors) is still fully editable in the row
     *  details panel, so no field is unreachable.
     */
    class FDataTableEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FDataTableEditorTool)

        FDataTableEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_TABLE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void DrawTableWindow(bool bFocused);
        void DrawRowDetailsWindow(bool bFocused);

        void DrawToolbar();
        void DrawGrid();
        void DrawNameCell(int32 RowIndex);
        void DrawValueCell(int32 RowIndex, int32 ColumnIndex);
        void DrawImportReportModal();
        void DrawChangeRowStructModal();

        /** Recomputes the visible column set. Cheap, but the result is stable between structural
         *  changes, so it is cached rather than rebuilt per frame. */
        void RebuildColumns();

        /** Filtered, sorted display order. Rebuilt when the filter, sort or row set changes. */
        void RebuildDisplayOrder();

        void AddRow();
        void DuplicateSelectedRow();
        void RemoveSelectedRow();

        void ImportCSV();
        void ExportCSV();

        void MarkDirty();

        /** Cancels any in-progress cell edit. Called before anything that can move rows, since the
         *  edit is anchored to an index. */
        void CancelCellEdit();

        void CommitCellEdit();

        TUniquePtr<FPropertyTable> RowPropertyTable;

        /** Text-convertible properties of the row struct, supers first. Column 0 of the grid is the
         *  row name and is not in this list. */
        TVector<FProperty*> Columns;

        /** Row struct the cached columns and property table were built for. Anything else means the
         *  table's type changed underneath us and both must be rebuilt. */
        CStruct* BoundRowStruct = nullptr;

        /** Row memory the details panel is currently pointed at; the rows vector reallocates on
         *  add/remove, so this is re-checked rather than assumed. */
        void* BoundRowMemory = nullptr;

        TVector<int32> DisplayOrder;
        bool bDisplayOrderDirty = true;

        int32 SelectedRow = INDEX_NONE;

        /** Cell currently being typed into. Column INDEX_NONE with a valid row means the name cell. */
        int32 EditingRow = INDEX_NONE;
        int32 EditingColumn = INDEX_NONE;
        bool bEditorJustOpened = false;
        char EditBuffer[256] = {};

        ImGuiTextFilter Filter;

        /** SortColumn indexes Columns, with -1 meaning the name column -- so it cannot also encode
         *  "unsorted", which is what bSortActive is for. */
        bool bSortActive = false;
        int32 SortColumn = INDEX_NONE;
        bool bSortAscending = true;

        FDataTableCSVResult LastImportResult;
        bool bShowImportReport = false;

        bool bShowChangeRowStruct = false;
        CStruct* PendingRowStruct = nullptr;
    };
}
