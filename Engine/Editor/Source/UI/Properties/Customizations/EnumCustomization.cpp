#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    EPropertyChangeOp FEnumPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(Property->Property);
        bool bWasChanged = false;
        bool bIsBitmask = EnumProperty->GetEnum()->IsBitmaskEnum();
    
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    
        if (bIsBitmask)
        {
            int64 EnumCount = (int64)EnumProperty->GetEnum()->Names.size();
            FFixedString PreviewString = EnumProperty->GetEnum()->GetValueOrBitFieldAsString(CachedValue);
    
            if (PreviewString.empty())
            {
                PreviewString = "None";
            }

            if (ImGui::BeginCombo("##", PreviewString.c_str(), ImGuiComboFlags_HeightLarge))
            {
                bool bNoneSelected = (CachedValue == 0);
                if (ImGui::Selectable("None", bNoneSelected))
                {
                    CachedValue = 0;
                    bWasChanged = true;
                }
                if (bNoneSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                ImGui::Separator();
    
                for (int64 i = 0; i < EnumCount; ++i)
                {
                    int64 BitValue = static_cast<int64>(EnumProperty->GetEnum()->GetValueAtIndex(i));
                    if (BitValue == 0)
                    {
                        continue;
                    }

                    const char* Label = EnumProperty->GetEnum()->GetNameAtIndex(i).c_str();
                    bool bIsSet = (CachedValue & BitValue) != 0;
    
                    if (ImGui::Checkbox(Label, &bIsSet))
                    {
                        if (bIsSet)
                        {
                            CachedValue |= BitValue;
                        }
                        else
                        {
                            CachedValue &= ~BitValue;
                        }

                        bWasChanged = true;
                    }
                }
                ImGui::EndCombo();
            }
            
            
        }
        else
        {
            CEnum* Enum = EnumProperty->GetEnum();
            const int64 EnumCount = (int64)Enum->Names.size();

            // Map the stored value to its row; values need not be contiguous with indices.
            int32 CurrentIndex = INDEX_NONE;
            for (int64 i = 0; i < EnumCount; ++i)
            {
                if ((int64)Enum->GetValueAtIndex(i) == CachedValue)
                {
                    CurrentIndex = (int32)i;
                    break;
                }
            }

            const FFixedString Preview = Enum->GetNameAtValue(CachedValue).c_str();
            const int32 Picked = ImGuiX::SearchableCombo("##enum", Preview.c_str(), (int32)EnumCount, CurrentIndex, [Enum](int32 Index)
            {
                return FFixedString(Enum->GetNameAtIndex(Index).c_str());
            }, LE_ICON_RHOMBUS_OUTLINE);

            if (Picked != INDEX_NONE)
            {
                CachedValue = (int64)Enum->GetValueAtIndex(Picked);
                bWasChanged = true;
            }
        }
    
        ImGui::PopItemWidth();
    
        return bWasChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FEnumPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(Property->Property);
        EnumProperty->GetInnerProperty()->SetIntPropertyValue(Property->GetValuePtr(), CachedValue);
    }

    void FEnumPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(Property->Property);
        CachedValue = EnumProperty->GetInnerProperty()->GetSignedIntPropertyValue(Property->GetValuePtr());
    }
}
