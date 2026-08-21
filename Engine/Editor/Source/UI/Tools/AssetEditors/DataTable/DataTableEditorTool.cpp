#include "DataTableEditorTool.h"

#include "DataTableWidgets.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Assets/AssetTypes/DataTable/DataTableCSV.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Process/PlatformProcess.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* TableWindowName = "Data Table";
    static const char* RowDetailsWindowName = "Row Details";

    namespace
    {
        // Sorting numbers as strings would put 10 before 2, which is worse than not sorting.
        bool IsNumericProperty(FProperty* Property)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double:
                return true;
            default:
                return false;
            }
        }

        void GatherColumns(CStruct* Struct, TVector<FProperty*>& Out)
        {
            if (Struct == nullptr)
            {
                return;
            }

            GatherColumns(Struct->GetSuperStruct(), Out);

            Struct->ForEachProperty<FProperty>([&Out](FProperty* Property)
            {
                if (Reflection::IsTextConvertible(Property))
                {
                    Out.push_back(Property);
                }
            });
        }
    }

    FDataTableEditorTool::FDataTableEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FDataTableEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        RowPropertyTable = MakeUnique<FPropertyTable>();
        RowPropertyTable->SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            MarkDirty();

            // A details edit can change a value the grid is sorting or filtering on.
            bDisplayOrderDirty = true;
        });

        CreateToolWindow(TableWindowName, [this](bool bFocused)
        {
            DrawTableWindow(bFocused);
        });

        CreateToolWindow(RowDetailsWindowName, [this](bool bFocused)
        {
            DrawRowDetailsWindow(bFocused);
        });
    }

    void FDataTableEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID RightID = 0;
        const ImGuiID LeftID = ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Left, 0.72f, nullptr, &RightID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(TableWindowName).c_str(), LeftID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RowDetailsWindowName).c_str(), RightID);
    }

    void FDataTableEditorTool::MarkDirty()
    {
        if (CDataTable* Table = GetAsset<CDataTable>())
        {
            Table->GetPackage()->MarkDirty();
        }
    }

    void FDataTableEditorTool::RebuildColumns()
    {
        Columns.clear();

        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || Table->GetRowStruct() == nullptr)
        {
            BoundRowStruct = nullptr;
            return;
        }

        BoundRowStruct = Table->GetRowStruct();
        GatherColumns(BoundRowStruct, Columns);
    }

    void FDataTableEditorTool::RebuildDisplayOrder()
    {
        bDisplayOrderDirty = false;
        DisplayOrder.clear();

        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr)
        {
            return;
        }

        const bool bFiltering = Filter.IsActive();

        for (int32 i = 0; i < Table->GetRowCount(); ++i)
        {
            const SDataTableRow& Row = Table->Rows[i];

            if (bFiltering)
            {
                // Matches every visible cell, so searching for a value finds the row holding it.
                bool bMatched = ImGuiX::PassSearchFilter(Filter, Row.Name.c_str());
                if (!bMatched)
                {
                    const void* RowMemory = Row.Value.GetMemory();
                    for (FProperty* Property : Columns)
                    {
                        if (RowMemory != nullptr && ImGuiX::PassSearchFilter(Filter, Reflection::ToText(Property, RowMemory).c_str()))
                        {
                            bMatched = true;
                            break;
                        }
                    }
                }

                if (!bMatched)
                {
                    continue;
                }
            }

            DisplayOrder.push_back(i);
        }

        if (!bSortActive)
        {
            return;
        }

        // SortColumn -1 is the name column; anything else indexes Columns.
        FProperty* SortProperty = (SortColumn >= 0 && SortColumn < (int32)Columns.size()) ? Columns[SortColumn] : nullptr;
        const bool bNumeric = SortProperty != nullptr && IsNumericProperty(SortProperty);
        const bool bAscending = bSortAscending;

        Algo::StableSort(DisplayOrder.begin(), DisplayOrder.end(),
            [Table, SortProperty, bNumeric, bAscending](int32 A, int32 B)
            {
                const SDataTableRow& RowA = Table->Rows[A];
                const SDataTableRow& RowB = Table->Rows[B];

                int32 Comparison = 0;
                if (SortProperty == nullptr)
                {
                    Comparison = strcmp(RowA.Name.c_str(), RowB.Name.c_str());
                }
                else
                {
                    const void* MemoryA = RowA.Value.GetMemory();
                    const void* MemoryB = RowB.Value.GetMemory();
                    const FString TextA = MemoryA ? Reflection::ToText(SortProperty, MemoryA) : FString();
                    const FString TextB = MemoryB ? Reflection::ToText(SortProperty, MemoryB) : FString();

                    if (bNumeric)
                    {
                        const double ValueA = atof(TextA.c_str());
                        const double ValueB = atof(TextB.c_str());
                        Comparison = (ValueA < ValueB) ? -1 : (ValueA > ValueB ? 1 : 0);
                    }
                    else
                    {
                        Comparison = strcmp(TextA.c_str(), TextB.c_str());
                    }
                }

                return bAscending ? Comparison < 0 : Comparison > 0;
            });
    }

    void FDataTableEditorTool::CancelCellEdit()
    {
        EditingRow = INDEX_NONE;
        EditingColumn = INDEX_NONE;
        bEditorJustOpened = false;
    }

    void FDataTableEditorTool::CommitCellEdit()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || EditingRow < 0 || EditingRow >= Table->GetRowCount())
        {
            CancelCellEdit();
            return;
        }

        SDataTableRow& Row = Table->Rows[EditingRow];
        const FStringView Text(EditBuffer);

        if (EditingColumn == INDEX_NONE)
        {
            const FString Trimmed(EditBuffer);
            const FName NewName(Trimmed);

            if (!Trimmed.empty() && NewName != Row.Name)
            {
                // Names key the table, so uniquify rather than reject and the edit is never silently lost.
                const int32 Collision = Table->FindRowIndex(NewName);
                Row.Name = (Collision == INDEX_NONE) ? NewName : Table->MakeUniqueRowName(NewName);

                MarkDirty();
                bDisplayOrderDirty = true;
            }
        }
        else if (EditingColumn >= 0 && EditingColumn < (int32)Columns.size())
        {
            void* RowMemory = Row.Value.GetMutableMemory();
            if (RowMemory != nullptr)
            {
                FProperty* Property = Columns[EditingColumn];
                const FString Previous = Reflection::ToText(Property, RowMemory);

                if (Reflection::FromText(Property, RowMemory, Text))
                {
                    if (Reflection::ToText(Property, RowMemory) != Previous)
                    {
                        MarkDirty();
                        bDisplayOrderDirty = true;
                    }
                }
                else
                {
                    ImGuiX::Notifications::NotifyWarning("'{}' is not a valid value for {}.",
                        FString(EditBuffer).c_str(), Property->GetPropertyName().c_str());
                }
            }
        }

        CancelCellEdit();
    }

    void FDataTableEditorTool::AddRow()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr)
        {
            return;
        }

        CancelCellEdit();

        const int32 Index = Table->AddRow(Table->MakeUniqueRowName("NewRow"));
        if (Index == INDEX_NONE)
        {
            ImGuiX::Notifications::NotifyWarning("Set a row struct before adding rows.");
            return;
        }

        SelectedRow = Index;
        bDisplayOrderDirty = true;
        MarkDirty();
    }

    void FDataTableEditorTool::DuplicateSelectedRow()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || SelectedRow < 0 || SelectedRow >= Table->GetRowCount())
        {
            return;
        }

        CancelCellEdit();

        // Copied by value first, since emplace_back can reallocate and dangle the source reference.
        const SDataTableRow Source = Table->Rows[SelectedRow];

        SDataTableRow& Copy = Table->Rows.emplace_back();
        Copy.Value = Source.Value;
        Copy.Name = Table->MakeUniqueRowName(Source.Name);

        SelectedRow = Table->GetRowCount() - 1;
        bDisplayOrderDirty = true;
        MarkDirty();
    }

    void FDataTableEditorTool::RemoveSelectedRow()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || SelectedRow < 0 || SelectedRow >= Table->GetRowCount())
        {
            return;
        }

        CancelCellEdit();

        Table->RemoveRow(SelectedRow);

        // Keep the selection where the deleted row was so a run of deletions doesn't need a re-click.
        SelectedRow = Math::Min(SelectedRow, Table->GetRowCount() - 1);

        BoundRowMemory = nullptr;
        bDisplayOrderDirty = true;
        MarkDirty();
    }

    void FDataTableEditorTool::ImportCSV()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || Table->GetRowStruct() == nullptr)
        {
            ImGuiX::Notifications::NotifyWarning("Set a row struct before importing.");
            return;
        }

        FFixedString SelectedFile;
        if (!Platform::OpenFileDialogue(SelectedFile, "Import CSV", "CSV Files\0*.csv\0All Files\0*.*\0"))
        {
            return;
        }

        FString Text;
        if (!FileHelper::LoadFileIntoString(Text, SelectedFile.c_str()))
        {
            ImGuiX::Notifications::NotifyError("Could not read '{}'.", SelectedFile.c_str());
            return;
        }

        CancelCellEdit();

        LastImportResult = DataTableCSV::ImportText(Table, Text);

        if (!LastImportResult.bSucceeded)
        {
            ImGuiX::Notifications::NotifyError("Import failed: {}", LastImportResult.FailureReason.c_str());
            return;
        }

        SelectedRow = Table->GetRowCount() > 0 ? 0 : INDEX_NONE;
        BoundRowMemory = nullptr;
        bDisplayOrderDirty = true;
        MarkDirty();

        // Only interrupt with the report when something needs a decision; a clean import just says so.
        const bool bHasWarnings = !LastImportResult.Errors.empty()
            || !LastImportResult.UnknownColumns.empty()
            || !LastImportResult.MissingColumns.empty();

        if (bHasWarnings)
        {
            bShowImportReport = true;
        }
        else
        {
            ImGuiX::Notifications::NotifySuccess("Imported {} rows.", LastImportResult.RowsImported);
        }
    }

    void FDataTableEditorTool::ExportCSV()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr || Table->GetRowStruct() == nullptr)
        {
            return;
        }

        // A null filter selects a folder, and the file is named after the asset anyway.
        FFixedString Folder;
        if (!Platform::OpenFileDialogue(Folder, "Choose Export Folder"))
        {
            return;
        }

        const FFixedString FilePath = Paths::Combine(Folder, Table->GetName().ToString() + ".csv");
        const FString Text = DataTableCSV::ExportText(Table);

        if (FileHelper::SaveStringToFile(Text, FilePath.c_str()))
        {
            ImGuiX::Notifications::NotifySuccess("Exported {} rows.", Table->GetRowCount());
            Platform::ShowFileInExplorer(UTF8_TO_TCHAR(FilePath.c_str()));
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Could not write '{}'.", FilePath.c_str());
        }
    }

    void FDataTableEditorTool::DrawToolbar()
    {
        CDataTable* Table = GetAsset<CDataTable>();
        CStruct* RowStruct = Table != nullptr ? Table->GetRowStruct() : nullptr;
        const bool bHasRowStruct = RowStruct != nullptr;

        ImGui::BeginDisabled(!bHasRowStruct);
        if (ImGui::Button(LE_ICON_PLUS " Add Row"))
        {
            AddRow();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedRow == INDEX_NONE);
        if (ImGui::Button(LE_ICON_CONTENT_COPY " Duplicate"))
        {
            DuplicateSelectedRow();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_DELETE " Remove"))
        {
            RemoveSelectedRow();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::BeginDisabled(!bHasRowStruct);
        if (ImGui::Button(LE_ICON_FILE_IMPORT " Import CSV"))
        {
            ImportCSV();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_FILE_EXPORT " Export CSV"))
        {
            ExportCSV();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Row Struct:");
        ImGui::SameLine();
        if (ImGui::Button(bHasRowStruct ? RowStruct->GetName().c_str() : "<none>"))
        {
            PendingRowStruct = RowStruct;
            bShowChangeRowStruct = true;
        }
        ImGuiX::TextTooltip("Change the struct every row is an instance of.");

        // Right-aligned when there is room, otherwise it simply follows the row.
        ImGui::SameLine();
        const float FilterWidth = 240.0f;
        const float Remaining = ImGui::GetContentRegionAvail().x;
        if (Remaining > FilterWidth)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (Remaining - FilterWidth));
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (Filter.Draw("##filter"))
        {
            bDisplayOrderDirty = true;
        }
        if (!Filter.IsActive())
        {
            // Filter::Draw has no hint parameter, so label the empty box in place.
            const ImVec2 Min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(Min.x + ImGui::GetStyle().FramePadding.x, Min.y + ImGui::GetStyle().FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), LE_ICON_MAGNIFY " Search rows...");
        }
    }

    void FDataTableEditorTool::DrawNameCell(int32 RowIndex)
    {
        CDataTable* Table = GetAsset<CDataTable>();
        SDataTableRow& Row = Table->Rows[RowIndex];

        const bool bEditing = EditingRow == RowIndex && EditingColumn == INDEX_NONE;

        if (bEditing)
        {
            ImGui::SetNextItemWidth(-1.0f);
            if (bEditorJustOpened)
            {
                ImGui::SetKeyboardFocusHere();
                bEditorJustOpened = false;
            }

            if (ImGui::InputText("##nameedit", EditBuffer, sizeof(EditBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                CommitCellEdit();
            }
            else if (ImGui::IsItemDeactivated())
            {
                // Escape reverts; losing focus any other way commits, which is what a grid should do.
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    CancelCellEdit();
                }
                else
                {
                    CommitCellEdit();
                }
            }
            return;
        }

        const bool bSelected = SelectedRow == RowIndex;
        // Without AllowOverlap this row-spanning selectable claims hover and the value cells see no click.
        constexpr ImGuiSelectableFlags NameFlags =
            ImGuiSelectableFlags_SpanAllColumns |
            ImGuiSelectableFlags_AllowDoubleClick |
            ImGuiSelectableFlags_AllowOverlap;

        if (ImGui::Selectable(Row.Name.c_str(), bSelected, NameFlags))
        {
            SelectedRow = RowIndex;

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                CancelCellEdit();
                EditingRow = RowIndex;
                EditingColumn = INDEX_NONE;
                bEditorJustOpened = true;

                Memory::Memzero(EditBuffer, sizeof(EditBuffer));
                const FString Current = Row.Name.ToString();
                Memory::Memcpy(EditBuffer, Current.c_str(), Math::Min(Current.size(), sizeof(EditBuffer) - 1));
            }
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && EditingRow == INDEX_NONE)
        {
            ImGui::SetTooltip(bCanReorder
                ? "Double-click to rename. Drag to reorder."
                : "Double-click to rename. Clear the search and sort to reorder by dragging.");
        }

        HandleRowDrag(RowIndex);
    }

    void FDataTableEditorTool::HandleRowDrag(int32 RowIndex)
    {
        if (!bCanReorder || !ImGui::IsItemActive() || !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            return;
        }

        DraggingRow = RowIndex;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        // Distance, not hover, since overlapping value cells would step the row on horizontal movement.
        const float RowHeight = ImGui::GetItemRectSize().y + ImGui::GetStyle().CellPadding.y * 2.0f;
        const float DragY = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y;

        if (RowHeight <= 0.0f || Math::Abs(DragY) < RowHeight)
        {
            return;
        }

        CDataTable* Table = GetAsset<CDataTable>();
        const int32 Target = RowIndex + (DragY < 0.0f ? -1 : 1);

        if (Target >= 0 && Target < Table->GetRowCount())
        {
            CancelCellEdit();

            Table->MoveRow(RowIndex, Target);
            SelectedRow = Target;
            DraggingRow = Target;
            BoundRowMemory = nullptr;
            bDisplayOrderDirty = true;
            MarkDirty();
        }

        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    }

    void FDataTableEditorTool::DrawValueCell(int32 RowIndex, int32 ColumnIndex)
    {
        CDataTable* Table = GetAsset<CDataTable>();
        SDataTableRow& Row = Table->Rows[RowIndex];
        FProperty* Property = Columns[ColumnIndex];

        void* RowMemory = Row.Value.GetMutableMemory();
        if (RowMemory == nullptr)
        {
            ImGui::TextDisabled("--");
            return;
        }

        const bool bEditing = EditingRow == RowIndex && EditingColumn == ColumnIndex;

        if (bEditing)
        {
            ImGui::SetNextItemWidth(-1.0f);
            if (bEditorJustOpened)
            {
                ImGui::SetKeyboardFocusHere();
                bEditorJustOpened = false;
            }

            if (ImGui::InputText("##celledit", EditBuffer, sizeof(EditBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                CommitCellEdit();
            }
            else if (ImGui::IsItemDeactivated())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    CancelCellEdit();
                }
                else
                {
                    CommitCellEdit();
                }
            }
            return;
        }

        // A text item is not hit-testable, and an InputText per cell is what costs at this row count.
        const FString Text = Reflection::ToText(Property, RowMemory);

        if (ImGui::Selectable(Text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
        {
            SelectedRow = RowIndex;

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                CancelCellEdit();
                EditingRow = RowIndex;
                EditingColumn = ColumnIndex;
                bEditorJustOpened = true;

                Memory::Memzero(EditBuffer, sizeof(EditBuffer));
                Memory::Memcpy(EditBuffer, Text.c_str(), Math::Min(Text.size(), sizeof(EditBuffer) - 1));
            }
        }

        HandleRowDrag(RowIndex);
    }

    void FDataTableEditorTool::DrawGrid()
    {
        CDataTable* Table = GetAsset<CDataTable>();

        constexpr ImGuiTableFlags TableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortTristate |
            ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingFixedFit;

        // Reorder writes storage order, so it needs the view to BE storage order.
        bCanReorder = !bSortActive && !Filter.IsActive();

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            DraggingRow = INDEX_NONE;
        }

        // One extra for the name column, which is always present.
        const int32 ColumnCount = (int32)Columns.size() + 1;

        if (!ImGui::BeginTable("##datatable", ColumnCount, TableFlags))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed, 180.0f);
        for (FProperty* Property : Columns)
        {
            ImGui::TableSetupColumn(Property->GetPropertyDisplayName().c_str(), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        }
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* SortSpecs = ImGui::TableGetSortSpecs())
        {
            if (SortSpecs->SpecsDirty)
            {
                bSortActive = SortSpecs->SpecsCount > 0;
                if (bSortActive)
                {
                    // Grid column 0 is the name, so it lands on -1 and a separate flag tracks sorting at all.
                    SortColumn = SortSpecs->Specs[0].ColumnIndex - 1;
                    bSortAscending = SortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                }

                bDisplayOrderDirty = true;
                SortSpecs->SpecsDirty = false;
            }
        }

        if (bDisplayOrderDirty)
        {
            RebuildDisplayOrder();
        }

        // Clipped over the display order, so thousands of rows build widgets only for what is visible.
        ImGuiListClipper Clipper;
        Clipper.Begin((int32)DisplayOrder.size());

        // Clipping these away destroys the InputText mid-edit, and drops the drag's active id.
        for (const int32 KeepAlive : {EditingRow, DraggingRow})
        {
            if (KeepAlive == INDEX_NONE)
            {
                continue;
            }

            for (int32 i = 0; i < (int32)DisplayOrder.size(); ++i)
            {
                if (DisplayOrder[i] == KeepAlive)
                {
                    Clipper.IncludeItemByIndex(i);
                    break;
                }
            }
        }

        while (Clipper.Step())
        {
            for (int32 Display = Clipper.DisplayStart; Display < Clipper.DisplayEnd; ++Display)
            {
                const int32 RowIndex = DisplayOrder[Display];
                if (RowIndex < 0 || RowIndex >= Table->GetRowCount())
                {
                    continue;
                }

                ImGui::TableNextRow();

                // Keyed by name, since an index-derived id changes under a dragged row and drops it.
                ImGui::PushID(Table->Rows[RowIndex].Name.c_str());

                ImGui::TableSetColumnIndex(0);
                DrawNameCell(RowIndex);

                for (int32 Col = 0; Col < (int32)Columns.size(); ++Col)
                {
                    if (!ImGui::TableSetColumnIndex(Col + 1))
                    {
                        continue;
                    }
                    ImGui::PushID(Col);
                    DrawValueCell(RowIndex, Col);
                    ImGui::PopID();
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    void FDataTableEditorTool::DrawTableWindow(bool /*bFocused*/)
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr)
        {
            return;
        }

        // The row struct can change from the details panel, an import or an undo, so rebuild on mismatch.
        if (Table->GetRowStruct() != BoundRowStruct)
        {
            RebuildColumns();
            CancelCellEdit();
            SelectedRow = Table->GetRowCount() > 0 ? 0 : INDEX_NONE;
            BoundRowMemory = nullptr;
            bDisplayOrderDirty = true;
        }

        // Row count changes from anywhere (import, undo) must not leave stale indices in the view.
        if ((int32)DisplayOrder.size() > Table->GetRowCount() && !Filter.IsActive())
        {
            bDisplayOrderDirty = true;
        }

        DrawToolbar();
        ImGui::Separator();

        if (Table->GetRowStruct() == nullptr)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                "This table has no row struct. Pick one to start adding rows.");
        }
        else
        {
            if (bDisplayOrderDirty)
            {
                RebuildDisplayOrder();
            }

            ImGui::TextDisabled("%d row%s%s", (int32)DisplayOrder.size(),
                DisplayOrder.size() == 1 ? "" : "s",
                Filter.IsActive() ? " (filtered)" : "");

            DrawGrid();
        }

        DrawImportReportModal();
        DrawChangeRowStructModal();
    }

    void FDataTableEditorTool::DrawRowDetailsWindow(bool /*bFocused*/)
    {
        CDataTable* Table = GetAsset<CDataTable>();
        if (Table == nullptr)
        {
            return;
        }

        if (SelectedRow < 0 || SelectedRow >= Table->GetRowCount())
        {
            ImGui::TextDisabled("Select a row to edit it.");
            BoundRowMemory = nullptr;
            return;
        }

        SDataTableRow& Row = Table->Rows[SelectedRow];
        void* RowMemory = Row.Value.GetMutableMemory();
        CStruct* RowStruct = Row.Value.GetScriptStruct();

        if (RowMemory == nullptr || RowStruct == nullptr)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "This row has no value.");
            BoundRowMemory = nullptr;
            return;
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Row:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", Row.Name.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // Rows reallocate on add/remove/import, so re-point rather than trusting the last binding.
        if (RowMemory != BoundRowMemory)
        {
            BoundRowMemory = RowMemory;
            RowPropertyTable->SetObject(RowMemory, RowStruct);
        }

        RowPropertyTable->DrawTree();
    }

    void FDataTableEditorTool::DrawImportReportModal()
    {
        if (bShowImportReport)
        {
            ImGui::OpenPopup("Import Report");
            bShowImportReport = false;
        }

        ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Import Report", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::Text("Imported %d row%s.", LastImportResult.RowsImported, LastImportResult.RowsImported == 1 ? "" : "s");
        if (LastImportResult.RowsSkipped > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Skipped %d row%s.",
                LastImportResult.RowsSkipped, LastImportResult.RowsSkipped == 1 ? "" : "s");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!LastImportResult.UnknownColumns.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Columns with no matching field:");
            for (const FString& Column : LastImportResult.UnknownColumns)
            {
                ImGui::BulletText("%s", Column.c_str());
            }
            ImGui::Spacing();
        }

        if (!LastImportResult.MissingColumns.empty())
        {
            ImGui::TextDisabled("Fields the file did not supply (left at their defaults):");
            for (const FString& Column : LastImportResult.MissingColumns)
            {
                ImGui::BulletText("%s", Column.c_str());
            }
            ImGui::Spacing();
        }

        if (!LastImportResult.Errors.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "Problems:");
            if (ImGui::BeginChild("##errors", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders))
            {
                for (const FString& Error : LastImportResult.Errors)
                {
                    ImGui::TextWrapped("%s", Error.c_str());
                }
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void FDataTableEditorTool::DrawChangeRowStructModal()
    {
        if (bShowChangeRowStruct)
        {
            ImGui::OpenPopup("Row Struct");
            bShowChangeRowStruct = false;
        }

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Row Struct", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        CDataTable* Table = GetAsset<CDataTable>();

        ImGui::TextDisabled("Every row is an instance of this struct.");
        ImGui::Spacing();

        bool bChanged = false;
        ImGui::PushItemWidth(-1.0f);
        PendingRowStruct = DataTableUI::DrawRowStructPicker("##rowstruct", PendingRowStruct, bChanged);
        ImGui::PopItemWidth();

        const bool bWouldDiscard = Table != nullptr
            && Table->GetRowCount() > 0
            && PendingRowStruct != Table->GetRowStruct();

        if (bWouldDiscard)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                "This discards all %d existing rows.", Table->GetRowCount());
            ImGui::TextDisabled("Their fields belong to the old struct and cannot be carried over.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool bIsChange = Table != nullptr && PendingRowStruct != Table->GetRowStruct();

        ImGui::BeginDisabled(!bIsChange);
        if (ImGui::Button(bWouldDiscard ? "Change and Discard Rows" : "Change", ImVec2(200.0f, 0.0f)))
        {
            CancelCellEdit();

            Table->SetRowStruct(PendingRowStruct);

            RebuildColumns();
            SelectedRow = INDEX_NONE;
            BoundRowMemory = nullptr;
            SortColumn = INDEX_NONE;
            bSortActive = false;
            bDisplayOrderDirty = true;
            MarkDirty();

            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
