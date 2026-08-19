#include "ParticleParameterCustomization.h"
#include "imgui.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    static const char* ParameterTypeNames[] = { "Float", "Int", "Bool", "Vec2", "Vec3", "Vec4", "Color" };

    static void ResetValueStorage(FParticleParameter& Param)
    {
        Param.Scalar  = 0.0f;
        Param.Integer = 0;
        Param.Boolean = false;
        Param.Vector  = FVector4(0.0f);
    }

    TSharedPtr<FParticleParameterCustomization> FParticleParameterCustomization::MakeInstance()
    {
        return MakeShared<FParticleParameterCustomization>();
    }

    EPropertyChangeOp FParticleParameterCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        bool bChanged = false;

        const float TotalWidth = ImGui::GetContentRegionAvail().x;
        const float NameWidth  = Math::Max(120.0f, TotalWidth * 0.30f);
        const float TypeWidth  = 80.0f;
        const float Spacing    = ImGui::GetStyle().ItemSpacing.x;
        const float ValueWidth = Math::Max(80.0f, TotalWidth - NameWidth - TypeWidth - Spacing * 2.0f);

        ImGui::PushItemWidth(NameWidth);
        if (ImGui::InputText("##Name", NameBuffer, sizeof(NameBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            Value.Name = FName(NameBuffer);
            bChanged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Value.Name = FName(NameBuffer);
            bChanged = true;
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::PushItemWidth(TypeWidth);
        int32 CurrentType = (int32)Value.Type;
        if (ImGui::Combo("##Type", &CurrentType, ParameterTypeNames, IM_ARRAYSIZE(ParameterTypeNames)))
        {
            Value.Type = (EParticleParameterType)CurrentType;
            ResetValueStorage(Value);
            bChanged = true;
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::PushItemWidth(ValueWidth);
        switch (Value.Type)
        {
        case EParticleParameterType::Float:
        {
            if (ImGui::DragFloat("##Value", &Value.Scalar, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Int:
        {
            if (ImGui::DragInt("##Value", &Value.Integer))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Bool:
        {
            if (ImGui::Checkbox("##Value", &Value.Boolean))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Vec2:
        {
            if (ImGui::DragFloat2("##Value", &Value.Vector.x, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Vec3:
        {
            if (ImGui::DragFloat3("##Value", &Value.Vector.x, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Vec4:
        {
            if (ImGui::DragFloat4("##Value", &Value.Vector.x, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Color:
        {
            if (ImGui::ColorEdit4("##Value", &Value.Vector.x, ImGuiColorEditFlags_AlphaBar))
            {
                bChanged = true;
            }
            break;
        }
        }
        ImGui::PopItemWidth();

        return bChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FParticleParameterCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(Value);
    }

    void FParticleParameterCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&Value);

        const char* NameStr = Value.Name.IsNone() ? "" : Value.Name.c_str();
        strncpy(NameBuffer, NameStr, sizeof(NameBuffer));
        NameBuffer[sizeof(NameBuffer) - 1] = '\0';
    }
}
