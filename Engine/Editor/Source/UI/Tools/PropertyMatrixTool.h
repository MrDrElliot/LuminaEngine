#pragma once
#include "UI/Tools/EditorTool.h"
#include "UI/Properties/PropertyTable.h"

namespace Lumina
{
    class FMatrixEditCommand;

    // Assets side by side with chosen properties as columns, and a details panel whose edits land on every selected row.
    class FPropertyMatrixTool : public FEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FPropertyMatrixTool)

        FPropertyMatrixTool(IEditorToolContext* Context, TVector<FGuid> InAssets);

        const char* GetTitlebarIcon() const override { return LE_ICON_TABLE_EDIT; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;

        void OnSave() override;
        bool IsUnsavedDocument() override;

    protected:

        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void OnPostUndoRedo() override;

    private:

        struct FCell
        {
            TSharedPtr<FPropertyHandle>             Handle;
            TSharedPtr<IPropertyTypeCustomization>  Customization;
        };

        struct FRow
        {
            TStrongObjectPtr<CObject>   Object;
            TVector<FCell>              Cells;
            bool                        bSelected = true;
        };

        // The edit in flight, from its first changed frame to its commit.
        struct FBulkEdit
        {
            TVector<CObject*>       Targets;
            CObject*                Source = nullptr;
            FProperty*              Property = nullptr;
            FMatrixEditCommand*     Command = nullptr;
            TVector<TVector<uint8>> SourceBaseline;
        };

        void DrawGrid();
        void DrawDetails();
        void DrawCell(FRow& Row, int32 Column);
        void DrawColumnsMenu();

        void RebuildCells();
        void RefreshSelection();
        void PinDefaultColumns();
        void TogglePinned(FProperty* Property);
        void DropDeletedRows();

        // Positions index into Visible, so a shift-click range skips the rows the filter hides.
        void SelectRow(const TVector<int32>& Visible, int32 Position, bool bToggle, bool bRange);

        // A selected row edits every selected row, while an unselected one edits only itself.
        void BeginEdit(FName Label, TVector<CObject*> Targets, CObject* Source, FProperty* Property);
        void SpreadEdit();
        void FinishEdit();

        // The top-level property of the source that no longer matches the baseline taken when the edit began.
        FProperty* FindChangedProperty() const;

        TVector<FRow>               Rows;
        TVector<CObject*>           Selection;
        TVector<FProperty*>         Columns;
        TVector<FProperty*>         ColumnCandidates;
        CClass*                     CommonClass = nullptr;
        FPropertyTable              Details;
        TOptional<FBulkEdit>        Edit;
        ImGuiTextFilter             NameFilter;
        FGuid                       LastClickedAsset;
    };
}
