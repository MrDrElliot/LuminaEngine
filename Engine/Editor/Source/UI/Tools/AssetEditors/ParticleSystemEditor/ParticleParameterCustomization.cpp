#include "ParticleParameterCustomization.h"
#include "imgui.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiXNumeric.h"

namespace Lumina
{
    static const char* ParameterTypeNames[] = { "Float", "Int", "Bool", "Vec2", "Vec3", "Vec4", "Color" };

    static void ResetValueStorage(SParticleParameter& Param)
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

    EPropertyChangeOp FParticleParameterCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bChanged = false;

        const float TotalWidth = ImGui::GetContentRegionAvail().x;
        const float NameWidth  = Math::Max(120.0f, TotalWidth * 0.30f);
        const float TypeWidth  = 80.0f;
        const float Spacing    = ImGui::GetStyle().ItemSpacing.x;
        const float ValueWidth = Math::Max(80.0f, TotalWidth - NameWidth - TypeWidth - Spacing * 2.0f);

        ImGui::PushItemWidth(NameWidth);
        const bool bNameEntered = ImGuiX::InputText("##Name", NameText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (bNameEntered || ImGui::IsItemDeactivatedAfterEdit())
        {
            Value.Name = FName(NameText);
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
            if (ImGuiX::DragFloat("##Value", &Value.Scalar, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Int:
        {
            if (ImGuiX::DragInt("##Value", &Value.Integer))
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
            if (ImGuiX::DragFloat2("##Value", &Value.Vector.x, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Vec3:
        {
            if (ImGuiX::DragFloat3("##Value", &Value.Vector.x, 0.01f))
            {
                bChanged = true;
            }
            break;
        }
        case EParticleParameterType::Vec4:
        {
            if (ImGuiX::DragFloat4("##Value", &Value.Vector.x, 0.01f))
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

        NameText = Value.Name.IsNone() ? FString() : Value.Name.ToString();
    }
}
