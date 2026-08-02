#include "EditorPCH.h"
#include "DataTableFactory.h"

#include "Core/Object/Class.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/AssetEditors/DataTable/DataTableWidgets.h"
#include "imgui.h"

namespace Lumina
{
    bool CDataTableFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CDataTableFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedRowStruct = nullptr;
        }

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_TABLE " Select Row Struct");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        bool bChanged = false;
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        SelectedRowStruct = DataTableUI::DrawRowStructPicker("##RowStruct", SelectedRowStruct, bChanged);
        ImGui::PopItemWidth();

        ImGui::Spacing();
        ImGui::TextDisabled("Row structs are reflected structs deriving from SDataTableRowBase.");

        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        bool bConfirm = false;
        ImGui::BeginDisabled(SelectedRowStruct == nullptr);
        if (ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0)))
        {
            bConfirm = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            SelectedRowStruct = nullptr;
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CDataTableFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CDataTable* NewTable = NewObject<CDataTable>(Package, Name);
        NewTable->SetRowStruct(SelectedRowStruct);

        SelectedRowStruct = nullptr;
        return NewTable;
    }
}
