#include "DataTableWidgets.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Scripting/ScriptStruct.h"

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina::DataTableUI
{
    CStruct* DrawRowStructPicker(const char* StrId, CStruct* Current, bool& bOutChanged)
    {
        bOutChanged = false;

        CStruct* Base = SDataTableRowBase::StaticStruct();

        TVector<CStruct*> Candidates;
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;

            // IsChildOf also excludes CClass, which would otherwise put every engine class in this list.
            if (Candidate == Base || !Candidate->IsChildOf(Base))
            {
                continue;
            }

            // A minted CScriptStruct is force-registered like any other type, so deriving is the whole test.
            Candidates.Add(Candidate);
        }

        // An empty combo reads as a broken picker, so say what is missing and how to supply it.
        if (Candidates.IsEmpty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "No row structs are defined.");
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

        Algo::Sort(Candidates, [](CStruct* A, CStruct* B)
        {
            return strcmp(*A->GetName(), *B->GetName()) < 0;
        });

        int32 CurrentIndex = 0;
        for (int32 i = 0; i < Candidates.Num(); ++i)
        {
            if (Candidates[i] == Current)
            {
                CurrentIndex = i + 1;
                break;
            }
        }

        const char* Preview = Current != nullptr ? *Current->GetName() : "None";

        const int32 Picked = ImGuiX::SearchableCombo(StrId, Preview, Candidates.Num() + 1, CurrentIndex,
            [&Candidates](int32 Index) -> FFixedString
            {
                return (Index == 0) ? FFixedString("None") : FFixedString(*Candidates[Index - 1]->GetName());
            });

        if (Picked != Constants::kIndexNone)
        {
            bOutChanged = true;
            return (Picked == 0) ? nullptr : Candidates[Picked - 1];
        }

        return Current;
    }
}
