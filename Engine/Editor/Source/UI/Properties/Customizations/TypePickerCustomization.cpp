#include "CoreTypeCustomization.h"


#include "imgui.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
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

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        const bool bChanged = ImGuiX::StructCombo("##substructpicker", SubStructProperty->GetMetaStruct(), Value, true, nullptr, true);
        ImGui::PopItemWidth();

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
