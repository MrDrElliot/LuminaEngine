#include "DataTableWidgets.h"

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectIterator.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina::DataTableUI
{
    static bool AnyRowStructExists(CStruct* Base)
    {
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;
            if (Candidate != Base && IsInstancableStructType(Candidate) && Candidate->IsChildOf(Base))
            {
                return true;
            }
        }
        return false;
    }

    CStruct* DrawRowStructPicker(const char* StrId, CStruct* Current, bool& bOutChanged)
    {
        CStruct* Base = SDataTableRowBase::StaticStruct();

        // An empty combo reads as a broken picker, so say what is missing and how to supply it.
        if (Current == nullptr && !AnyRowStructExists(Base))
        {
            bOutChanged = false;
            ImGui::TextColored(EditorColors::Warning(), "No row structs are defined.");
            ImGui::TextDisabled("Declare one in your game or plugin module:");
            ImGui::TextDisabled("    REFLECT()");
            ImGui::TextDisabled("    struct SMyRow : public SDataTableRowBase");
            ImGui::TextDisabled("    { GENERATED_BODY() PROPERTY(Editable) float Health; };");
            ImGui::TextDisabled("Or in a C# script.");
            ImGui::TextDisabled("    [Reflect]");
            ImGui::TextDisabled("    public sealed class MyRow : SDataTableRowBase");
            ImGui::TextDisabled("    { [Property] public float Health; }");
            return nullptr;
        }

        bOutChanged = ImGuiX::StructCombo(StrId, Base, Current);
        return Current;
    }
}
