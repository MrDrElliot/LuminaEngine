#include "CustomPrimitiveDataCustomization.h"
#include "imgui.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    TSharedPtr<FCustomPrimDataPropertyCustomization> FCustomPrimDataPropertyCustomization::MakeInstance()
    {
        return MakeShared<FCustomPrimDataPropertyCustomization>();
    }

    EPropertyChangeOp FCustomPrimDataPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bWasChanged = false;
    
        const char* TypeNames[] = { "Float", "Int", "UInt", "Color", "Bool" };
        int32 CurrentType = (int32)Value.Type;
        
        ImGui::PushItemWidth(100);
        if (ImGui::Combo("##Type", &CurrentType, TypeNames, IM_ARRAYSIZE(TypeNames)))
        {
            Value.Type = (ECustomPrimitiveDataType)CurrentType;
            Value.Data.UInt = 0;
            bWasChanged = true;
        }
        ImGui::PopItemWidth();
        
        ImGui::SameLine();
    
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        switch (Value.Type)
        {
            case ECustomPrimitiveDataType::Float:
            {
                float V = Value.Data.Float;
                if (ImGui::DragFloat("##Value", &V, 0.01f))
                {
                    Value.Data.Float = V;
                    bWasChanged = true;
                }
                break;
            }
    
            case ECustomPrimitiveDataType::Int:
            {
                int32 V = Value.Data.Int;
                if (ImGui::DragInt("##Value", &V))
                {
                    Value.Data.Int = V;
                    bWasChanged = true;
                }
                break;
            }
    
            case ECustomPrimitiveDataType::UInt:
            {
                int32 V = (int32)Value.Data.UInt;
                if (ImGui::DragInt("##Value", &V, 1.0f, 0, INT_MAX))
                {
                    Value.Data.UInt = (uint32)Math::Max(0, V);
                    bWasChanged = true;
                }
                break;
            }
    
            case ECustomPrimitiveDataType::Color:
            {
                int32 R = Value.Data.Bytes.r;
                int32 G = Value.Data.Bytes.g;
                int32 B = Value.Data.Bytes.b;
                int32 A = Value.Data.Bytes.a;

                float Col[4] = { R / 255.0f, G / 255.0f, B / 255.0f, A / 255.0f };
                if (ImGui::ColorEdit4("##Color", Col, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar))
                {
                    Value.Data.Bytes.r = (uint8)(Col[0] * 255);
                    Value.Data.Bytes.g = (uint8)(Col[1] * 255);
                    Value.Data.Bytes.b = (uint8)(Col[2] * 255);
                    Value.Data.Bytes.a = (uint8)(Col[3] * 255);
                    bWasChanged = true;
                }
                break;
            }
    
            case ECustomPrimitiveDataType::Bool:
            {
                bool V = Value.Data.UInt != 0;
                if (ImGui::Checkbox("##Value", &V))
                {
                    Value.Data.UInt = V ? 1u : 0u;
                    bWasChanged = true;
                }
                break;
            }
        }
        
        ImGui::PopItemWidth();

        if (bWasChanged)
        {
            // Fold an ongoing edit (e.g. a multi-frame drag) into the already-open transaction.
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            // Opened now, since StartChangeCallback snapshots the old value before the new one is written.
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

    void FCustomPrimDataPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(Value);
    }

    void FCustomPrimDataPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&Value);
    }
}
