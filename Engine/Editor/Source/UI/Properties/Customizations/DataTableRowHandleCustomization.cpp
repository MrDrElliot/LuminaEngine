#include "DataTableRowHandleCustomization.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptDataStruct.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        const ImVec4 GMutedText(0.6f, 0.6f, 0.6f, 1.0f);
        const ImVec4 GEmptyText(1.0f, 0.19f, 0.19f, 1.0f);
        const ImVec4 GWarnText(1.0f, 0.55f, 0.45f, 1.0f);

        // The row type the owning field demands, from PROPERTY(Editable, RowType = "SMyRow").
        CStruct* ResolveRequiredRowStruct(const FProperty* Property)
        {
            if (Property == nullptr || !Property->HasMetadata("RowType"))
            {
                return nullptr;
            }
            return ResolveDataStructByName(FName(Property->GetMetadata("RowType")));
        }

        TVector<FAssetData*> GatherDataTables()
        {
            TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([](const FAssetData& Data)
            {
                CClass* DataClass = FindObject<CClass>(Data.AssetClass);
                return DataClass != nullptr && DataClass->IsChildOf(CDataTable::StaticClass());
            });

            std::sort(Assets.begin(), Assets.end(), [](const FAssetData* A, const FAssetData* B)
            {
                return strcmp(A->AssetName.c_str(), B->AssetName.c_str()) < 0;
            });

            return Assets;
        }
    }

    TSharedPtr<FDataTableRowHandlePropertyCustomization> FDataTableRowHandlePropertyCustomization::MakeInstance()
    {
        return MakeShared<FDataTableRowHandlePropertyCustomization>();
    }

    EPropertyChangeOp FDataTableRowHandlePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        bool bWasChanged = false;

        CStruct* RequiredRowStruct = ResolveRequiredRowStruct(Property->Property);
        CDataTable* Table = DisplayValue.DataTable.Get();

        // A row name only means anything against the table it came from.
        const auto SetTable = [this, &bWasChanged](CDataTable* NewTable)
        {
            if (NewTable == DisplayValue.DataTable.Get())
            {
                return;
            }

            DisplayValue.DataTable = NewTable;
            if (NewTable == nullptr || NewTable->FindRowIndex(DisplayValue.RowName) == INDEX_NONE)
            {
                DisplayValue.RowName = FName();
            }
            bWasChanged = true;
        };

        const ImGuiStyle& Style = ImGui::GetStyle();
        const float ControlWidth = ImGui::GetFrameHeight();
        const float FullWidth    = ImGui::GetContentRegionAvail().x;
        const float TextWidth    = ImMax(FullWidth - ControlWidth * 2.0f - Style.ItemSpacing.x, 60.0f);

        ImGui::PushID(this);
        ImGui::BeginGroup();

        ImGui::SetNextItemWidth(TextWidth);
        FFixedString TableLabel = Table != nullptr ? Table->GetName().c_str() : "None";
        ImGui::PushStyleColor(ImGuiCol_Text, Table != nullptr ? GMutedText : GEmptyText);
        ImGui::InputText("##Table", TableLabel.data(), TableLabel.max_size(),
            ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor();

        if (ImGui::BeginDragDropTarget())
        {
            if (CObject* Dropped = DragDrop::AcceptAssetOfClass(CDataTable::StaticClass()))
            {
                SetTable(static_cast<CDataTable*>(Dropped));
            }
            ImGui::EndDragDropTarget();
        }
        ImGuiX::TextTooltip("Data table to read the row from. Drop one here, or pick it from the list.");

        ImGui::SameLine(0, 0);

        const ImVec2 DropdownSize = ImMax(ImVec2(220, 240), ImVec2(TextWidth, 320.0f));
        if (ImGui::BeginCombo("##TablePick", "", ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview))
        {
            TableFilter.Draw("##Search", DropdownSize.x - 30.0f);
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere(-1);
            }

            ImGui::SetNextWindowSizeConstraints(ImVec2(200, 200), DropdownSize);
            if (ImGui::BeginChild("##TableList", DropdownSize, false, ImGuiChildFlags_NavFlattened))
            {
                for (const FAssetData* Asset : GatherDataTables())
                {
                    if (!ImGuiX::PassSearchFilter(TableFilter, Asset->AssetName.c_str()))
                    {
                        continue;
                    }

                    if (ImGui::Selectable(Asset->AssetName.c_str()))
                    {
                        SetTable(LoadObject<CDataTable>(Asset->AssetGUID));
                        ImGui::CloseCurrentPopup();
                    }
                    ImGuiX::TextTooltip("{}", Asset->Path);
                }
            }
            ImGui::EndChild();
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(Table == nullptr && DisplayValue.RowName.IsNone());
        if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", ImVec2(ControlWidth, 0)))
        {
            DisplayValue.Clear();
            bWasChanged = true;
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Clear the reference");

        // Row picker. Index 0 is "None" so a row can be unset without clearing the table too.
        ImGui::BeginDisabled(Table == nullptr);

        const int32 RowCount = Table != nullptr ? Table->GetRowCount() : 0;
        int32 CurrentIndex = 0;
        for (int32 i = 0; i < RowCount; ++i)
        {
            if (Table->Rows[i].Name == DisplayValue.RowName)
            {
                CurrentIndex = i + 1;
                break;
            }
        }

        const char* RowPreview = DisplayValue.RowName.IsNone() ? "None" : DisplayValue.RowName.c_str();

        ImGui::SetNextItemWidth(FullWidth);
        const int32 Picked = ImGuiX::SearchableCombo("##Row", RowPreview, RowCount + 1, CurrentIndex,
            [Table](int32 Index) -> FFixedString
            {
                return Index == 0 ? FFixedString("None") : FFixedString(Table->Rows[Index - 1].Name.c_str());
            }, LE_ICON_TABLE_ROW);

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            DisplayValue.RowName = Picked == 0 ? FName() : Table->Rows[Picked - 1].Name;
            bWasChanged = true;
        }

        ImGui::EndDisabled();

        if (Table != nullptr)
        {
            CStruct* RowStruct = Table->GetRowStruct();

            if (RequiredRowStruct != nullptr && RowStruct != RequiredRowStruct)
            {
                ImGui::TextColored(GWarnText, LE_ICON_ALERT_CIRCLE_OUTLINE " Needs rows of %s, table holds %s.",
                    RequiredRowStruct->GetName().c_str(), RowStruct != nullptr ? RowStruct->GetName().c_str() : "nothing");
            }
            else if (!DisplayValue.RowName.IsNone() && Table->FindRowIndex(DisplayValue.RowName) == INDEX_NONE)
            {
                ImGui::TextColored(GWarnText, LE_ICON_ALERT_CIRCLE_OUTLINE " '%s' is not a row in this table.",
                    DisplayValue.RowName.c_str());
            }
        }

        ImGui::EndGroup();
        ImGui::PopID();

        if (bWasChanged)
        {
            // A transaction from a prior change is still settling; fold this one into it.
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }

        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }

        return EPropertyChangeOp::None;
    }

    void FDataTableRowHandlePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FDataTableRowHandlePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SDataTableRowHandle ActualValue;
        Property->GetValue(&ActualValue);

        if (!(CachedValue == ActualValue))
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
