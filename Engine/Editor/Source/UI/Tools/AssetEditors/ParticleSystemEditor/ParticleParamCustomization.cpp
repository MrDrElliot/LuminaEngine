#include "ParticleParamCustomization.h"

#include <imgui.h>
#include <string>
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace ParticleParamContext
    {
        namespace
        {
            TVector<CParticleSystem*>& GetStack()
            {
                static TVector<CParticleSystem*> Stack;
                return Stack;
            }
        }

        void PushSystem(CParticleSystem* System)
        {
            GetStack().push_back(System);
        }

        void PopSystem()
        {
            auto& Stack = GetStack();
            if (!Stack.empty())
            {
                Stack.pop_back();
            }
        }

        CParticleSystem* GetActiveSystem()
        {
            const auto& Stack = GetStack();
            return Stack.empty() ? nullptr : Stack.back();
        }
    }

    namespace
    {
        const char* ParamTypeLabel(EParticleParameterType Type)
        {
            switch (Type)
            {
            case EParticleParameterType::Float: return "Float";
            case EParticleParameterType::Int:   return "Int";
            case EParticleParameterType::Bool:  return "Bool";
            case EParticleParameterType::Vec2:  return "Vec2";
            case EParticleParameterType::Vec3:  return "Vec3";
            case EParticleParameterType::Vec4:  return "Vec4";
            case EParticleParameterType::Color: return "Color";
            }
            return "?";
        }

        // A single scalar broadcasts, and Vec4 and Color differ only in which editor widget is shown.
        bool CanParameterDrive(EParticleParameterType ParamType, EParticleParameterType InputType)
        {
            const uint32 ParamWidth = ParticleParamComponents(ParamType);
            return ParamWidth == 1 || ParamWidth == ParticleParamComponents(InputType);
        }

        // The Color tag distinguishes a color from any other float4, as it does for a plain FVector4.
        EParticleParameterType ResolveInputType(const SParticleParam& Value, const FProperty* Property)
        {
            if (Value.Type == EParticleParameterType::Vec4 && Property != nullptr && Property->HasMetadata("Color"))
            {
                return EParticleParameterType::Color;
            }
            return Value.Type;
        }

        // Named after the property and uniquified, so binding to code is one click rather than six steps.
        FName MakeUniqueParameterName(const CParticleSystem& System, const FName& Base)
        {
            const FString BaseStr = Base.IsNone() ? FString("Param") : FString(Base.c_str());

            FName Candidate(BaseStr.c_str());
            int32 Suffix = 1;
            while (System.FindUserParameter(Candidate) != nullptr)
            {
                Candidate = FName((BaseStr + "_" + Format("{}", Suffix).c_str()).c_str());
                ++Suffix;
            }
            return Candidate;
        }
    }

    TSharedPtr<FParticleParamCustomization> FParticleParamCustomization::MakeInstance()
    {
        return MakeShared<FParticleParamCustomization>();
    }

    EPropertyChangeOp FParticleParamCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FProperty* Prop = Property->Property;
        const EParticleParameterType InputType = ResolveInputType(Value, Prop);

        float Min = 0.0f;
        float Max = 0.0f;
        if (Prop != nullptr && Prop->HasMetadata("ClampMin"))
        {
            Min = std::stof(Prop->GetMetadata("ClampMin").c_str());
        }
        if (Prop != nullptr && Prop->HasMetadata("ClampMax"))
        {
            Max = std::stof(Prop->GetMetadata("ClampMax").c_str());
        }

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        const float ButtonWidth = ImGui::GetFrameHeight();
        const float ValueWidth  = Math::Max(ImGui::GetContentRegionAvail().x - ButtonWidth, 40.0f);

        if (Value.IsBound())
        {
            // The editor is replaced rather than disabled, since a grayed number still reads as the live value.
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button((FString(LE_ICON_LINK " ") + Value.ParameterName.c_str()).c_str(), ImVec2(ValueWidth, 0)))
            {
                ImGui::OpenPopup("##ParamBind");
            }
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::WrappedTooltip("Driven by the user parameter '{}'. Set it at runtime with the matching ParticleSystemComponent Set call.",
                    Value.ParameterName.c_str());
            }
        }
        else
        {
            ImGui::PushItemWidth(ValueWidth);
            switch (InputType)
            {
            case EParticleParameterType::Bool:
            {
                bool Bool = Value.Constant.x != 0.0f;
                if (ImGui::Checkbox("##Value", &Bool))
                {
                    Value.Constant.x = Bool ? 1.0f : 0.0f;
                }
                break;
            }
            case EParticleParameterType::Int:
            {
                int32 Int = (int32)Value.Constant.x;
                if (ImGui::DragInt("##Value", &Int, 1.0f, (int32)Min, (int32)Max))
                {
                    Value.Constant.x = (float)Int;
                }
                break;
            }
            case EParticleParameterType::Vec2:
                ImGui::DragFloat2("##Value", &Value.Constant.x, 0.01f, Min, Max);
                break;
            case EParticleParameterType::Vec3:
                ImGui::DragFloat3("##Value", &Value.Constant.x, 0.01f, Min, Max);
                break;
            case EParticleParameterType::Vec4:
                ImGui::DragFloat4("##Value", &Value.Constant.x, 0.01f, Min, Max);
                break;
            case EParticleParameterType::Color:
                ImGui::ColorEdit4("##Value", &Value.Constant.x, ImGuiColorEditFlags_AlphaBar);
                break;
            default:
                ImGui::DragFloat("##Value", &Value.Constant.x, 0.01f, Min, Max);
                break;
            }
            ImGui::PopItemWidth();

            if (ImGui::IsItemEdited())
            {
                Result = EPropertyChangeOp::Updated;
            }
            if (ImGui::IsItemActivated())
            {
                Result = EPropertyChangeOp::Started;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                Result = EPropertyChangeOp::Finished;
            }
        }

        ImGui::SameLine(0, 0);
        if (ImGui::Button(LE_ICON_MENU_DOWN "##ParamBindOpen", ImVec2(ButtonWidth, 0)))
        {
            ImGui::OpenPopup("##ParamBind");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Drive this input from a named parameter");
        }

        if (ImGui::BeginPopup("##ParamBind"))
        {
            CParticleSystem* System = ParticleParamContext::GetActiveSystem();

            if (ImGui::Selectable("Use Constant", !Value.IsBound()))
            {
                // Clearing only drops the name, so the input returns to the constant it showed before binding.
                Value.ParameterName = FName();
                Result = EPropertyChangeOp::Finished;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            if (System == nullptr)
            {
                ImGui::TextDisabled("No particle system context.");
            }
            else if (System->UserParameters.empty())
            {
                ImGui::TextDisabled("This system declares no parameters.");
            }
            else
            {
                for (const FParticleParameter& Param : System->UserParameters)
                {
                    if (Param.Name.IsNone())
                    {
                        continue;
                    }

                    // Listed disabled rather than hidden, so an incompatible parameter reads as wrong type, not deleted.
                    if (!CanParameterDrive(Param.Type, InputType))
                    {
                        ImGui::TextDisabled("%s (%s)", Param.Name.c_str(), ParamTypeLabel(Param.Type));
                        continue;
                    }

                    if (ImGui::Selectable(Param.Name.c_str(), Param.Name == Value.ParameterName))
                    {
                        Value.ParameterName = Param.Name;
                        Result = EPropertyChangeOp::Finished;
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGuiX::TextTooltip_Internal(ParamTypeLabel(Param.Type));
                    }
                }
            }

            if (System != nullptr)
            {
                ImGui::Separator();
                if (ImGui::Selectable("New Parameter From Value"))
                {
                    FParticleParameter& New = System->UserParameters.emplace_back();
                    New.Name   = MakeUniqueParameterName(*System, Prop != nullptr ? Prop->GetPropertyName() : FName());
                    New.Type   = InputType;
                    New.Scalar = Value.Constant.x;
                    New.Vector = Value.Constant;

                    Value.ParameterName = New.Name;
                    Result = EPropertyChangeOp::Finished;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGuiX::WrappedTooltip_Internal("Declare a parameter on the system seeded with this value, and bind this input to it.");
                }
            }

            ImGui::EndPopup();
        }

        return Result;
    }

    void FParticleParamCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(Value);
    }

    void FParticleParamCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&Value);
    }
}
