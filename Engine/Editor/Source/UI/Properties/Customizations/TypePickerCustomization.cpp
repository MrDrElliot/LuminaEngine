#include "CoreTypeCustomization.h"


#include "imgui.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // Index 0 is always None, and the class analog is ImGuiX::ClassCombo, shared with the dialogue.
        template<typename TType>
        TType* DrawTypePicker(const char* StrId, TType* Base, TType* Current, bool& bOutChanged)
        {
            bOutChanged = false;

            TVector<TType*> Candidates;
            for (TObjectIterator<TType> It; It; ++It)
            {
                TType* Candidate = *It;
                if (Base == nullptr || Candidate->IsChildOf(Base))
                {
                    Candidates.push_back(Candidate);
                }
            }

            Algo::Sort(Candidates, [](TType* A, TType* B)
            {
                return strcmp(A->GetName().c_str(), B->GetName().c_str()) < 0;
            });

            const int32 Found = Algo::IndexOf(Candidates, Current);
            const int32 CurrentIndex = Found != INDEX_NONE ? Found + 1 : 0;

            const char* Preview = Current ? Current->GetName().c_str() : "None";

            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            const int32 Picked = ImGuiX::SearchableCombo(StrId, Preview, static_cast<int32>(Candidates.size()) + 1, CurrentIndex,
                [&Candidates](int32 Index) -> FFixedString
                {
                    return (Index == 0) ? FFixedString("None") : FFixedString(Candidates[Index - 1]->GetName().c_str());
                });
            ImGui::PopItemWidth();

            if (Picked != INDEX_NONE)
            {
                bOutChanged = true;
                return (Picked == 0) ? nullptr : Candidates[Picked - 1];
            }

            return Current;
        }
    }

    EPropertyChangeOp FClassPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FClassProperty* ClassProperty = static_cast<FClassProperty*>(Property->Property);

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        const bool bChanged = ImGuiX::ClassCombo("##classpicker", ClassProperty->GetMetaClass(), Value);
        ImGui::PopItemWidth();

        return bChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FClassPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (void* Ptr = Property->GetValuePtr())
        {
            *static_cast<CClass**>(Ptr) = Value;
        }
    }

    void FClassPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (const void* Ptr = Property->GetValuePtr())
        {
            Value = *static_cast<CClass* const*>(Ptr);
        }
    }

    EPropertyChangeOp FSubStructPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FSubStructProperty* SubStructProperty = static_cast<FSubStructProperty*>(Property->Property);

        bool bChanged = false;
        Value = DrawTypePicker<CStruct>("##substructpicker", SubStructProperty->GetMetaStruct(), Value, bChanged);

        return bChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FSubStructPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (void* Ptr = Property->GetValuePtr())
        {
            *static_cast<CStruct**>(Ptr) = Value;
        }
    }

    void FSubStructPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (const void* Ptr = Property->GetValuePtr())
        {
            Value = *static_cast<CStruct* const*>(Ptr);
        }
    }
}
