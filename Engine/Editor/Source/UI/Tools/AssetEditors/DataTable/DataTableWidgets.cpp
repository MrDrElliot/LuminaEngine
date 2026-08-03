#include "DataTableWidgets.h"
#include "Scripting/ScriptDataStruct.h"
#include "Scripting/ScriptStruct.h"

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Containers/Array.h"
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

            // The base itself carries no fields, so a table of it would be a list of names and
            // nothing else. IsChildOf also excludes CClass instances, which derive from CStruct and
            // would otherwise put every engine class in this list.
            if (Candidate == Base || !Candidate->IsChildOf(Base))
            {
                continue;
            }

            // Reaches script-declared rows too: a minted CScriptStruct is force-registered like any other
            // reflected type, and the iterator walks the object array rather than a package, so deriving
            // from the base is the whole test. Nothing extra to merge in.
            Candidates.push_back(Candidate);
        }

        // Nothing derives from the base yet. An empty combo reads as a broken picker, so say what is
        // actually missing and how to supply it.
        if (Candidates.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "No row structs are defined.");
            ImGui::TextDisabled("Declare one in your game or plugin module:");
            ImGui::TextDisabled("    REFLECT()");
            ImGui::TextDisabled("    struct SMyRow : public SDataTableRowBase");
            ImGui::TextDisabled("    { GENERATED_BODY() PROPERTY(Editable) float Health; };");
            return nullptr;
        }

        eastl::sort(Candidates.begin(), Candidates.end(), [](CStruct* A, CStruct* B)
        {
            return strcmp(A->GetName().c_str(), B->GetName().c_str()) < 0;
        });

        int32 CurrentIndex = 0;
        for (size_t i = 0; i < Candidates.size(); ++i)
        {
            if (Candidates[i] == Current)
            {
                CurrentIndex = (int32)(i + 1);
                break;
            }
        }

        const char* Preview = Current != nullptr ? Current->GetName().c_str() : "None";

        const int32 Picked = ImGuiX::SearchableCombo(StrId, Preview, (int32)Candidates.size() + 1, CurrentIndex,
            [&Candidates](int32 Index) -> FFixedString
            {
                return (Index == 0) ? FFixedString("None") : FFixedString(Candidates[Index - 1]->GetName().c_str());
            });

        if (Picked != INDEX_NONE)
        {
            bOutChanged = true;
            return (Picked == 0) ? nullptr : Candidates[Picked - 1];
        }

        return Current;
    }
}
