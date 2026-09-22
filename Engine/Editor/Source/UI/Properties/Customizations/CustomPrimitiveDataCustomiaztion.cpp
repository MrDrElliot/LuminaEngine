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
            Value.Data = ECustomPrimitiveDataUnion::FromUInt(0);
            bWasChanged = true;
        }
        ImGui::PopItemWidth();
        
        ImGui::SameLine();
    
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        switch (Value.Type)
        {
            case ECustomPrimitiveDataType::Float:
            {
                float V = Value.Data.AsFloat();
                if (ImGui::DragFloat("##Value", &V, 0.01f))
                {
                    Value.Data = ECustomPrimitiveDataUnion::FromFloat(V);
                    bWasChanged = true;
                }
                break;
            }

            case ECustomPrimitiveDataType::Int:
            {
                int32 V = Value.Data.AsInt();
                if (ImGui::DragInt("##Value", &V))
                {
                    Value.Data = ECustomPrimitiveDataUnion::FromInt(V);
                    bWasChanged = true;
                }
                break;
            }

            case ECustomPrimitiveDataType::UInt:
            {
                int32 V = (int32)Value.Data.AsUInt();
                if (ImGui::DragInt("##Value", &V, 1.0f, 0, INT_MAX))
                {
                    Value.Data = ECustomPrimitiveDataUnion::FromUInt((uint32)Math::Max(0, V));
                    bWasChanged = true;
                }
                break;
            }

            case ECustomPrimitiveDataType::Color:
            {
                const FU8Vector4 Bytes = Value.Data.AsBytes();

                float Col[4] = { Bytes.r / 255.0f, Bytes.g / 255.0f, Bytes.b / 255.0f, Bytes.a / 255.0f };
                if (ImGui::ColorEdit4("##Color", Col, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar))
                {
                    Value.Data = ECustomPrimitiveDataUnion::FromColor(
                        FU8Vector4((uint8)(Col[0] * 255), (uint8)(Col[1] * 255),
                                   (uint8)(Col[2] * 255), (uint8)(Col[3] * 255)));
                    bWasChanged = true;
                }
                break;
            }

            case ECustomPrimitiveDataType::Bool:
            {
                bool V = Value.Data.AsBool();
                if (ImGui::Checkbox("##Value", &V))
                {
                    Value.Data = ECustomPrimitiveDataUnion::FromBool(V);
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
