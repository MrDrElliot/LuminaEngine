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

        DrawCreationDialogueTitle(LE_ICON_TABLE " Select Row Struct");

        ImGui::SetNextItemWidth(-1.0f);
        bool bChanged = false;
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        SelectedRowStruct = DataTableUI::DrawRowStructPicker("##RowStruct", SelectedRowStruct, bChanged);
        ImGui::PopItemWidth();

        ImGui::Spacing();
        ImGui::TextDisabled("Row structs are reflected structs deriving from SDataTableRowBase.");

        const bool bConfirm = DrawCreationDialogueButtons(SelectedRowStruct != nullptr, bShouldClose);
        if (bShouldClose)
        {
            SelectedRowStruct = nullptr;
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
