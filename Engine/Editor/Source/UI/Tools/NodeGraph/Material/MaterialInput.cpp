#include "MaterialInput.h"
#include "MaterialCompiler.h"
#include "Nodes/MaterialGraphNode.h"

namespace Lumina
{
    namespace
    {
        constexpr float kInlineFloatWidth = 64.0f;
    }

    FString CMaterialInput::GetPinTypeName() const
    {
        return FMaterialCompiler::GetHLSLTypeName(InputType);
    }

    bool CMaterialInput::HasInlineEditor() const
    {
        return ShouldDrawEditor() && InputType == EMaterialInputType::Float;
    }

    EGraphPinShape CMaterialInput::GetPinShape() const
    {
        return ShapeFor(InputType, EComponentMask::None);
    }

    EGraphPinShape CMaterialInput::ShapeFor(EMaterialInputType Type, EComponentMask Mask)
    {
        if (Type == EMaterialInputType::TextureHandle)
        {
            return EGraphPinShape::Hexagon;
        }

        // A texture sample's pins are typed Texture yet carry the float4 their mask names.
        const int32 Masked = Mask != EComponentMask::None ? FMaterialCompiler::GetComponentCount(Mask) : 0;
        if (Masked == 0 && Type == EMaterialInputType::Texture)
        {
            return EGraphPinShape::Hexagon;
        }

        switch (Masked > 0 ? Masked : FMaterialCompiler::GetComponentCount(Type))
        {
            case 2:  return EGraphPinShape::Diamond;
            case 3:  return EGraphPinShape::Triangle;
            case 4:  return EGraphPinShape::Square;
            default: return EGraphPinShape::Circle;
        }
    }

    float CMaterialInput::DrawPin()
    {
        float ReturnSize = 1.5f;
        CMaterialGraphNode* MaterialNode = static_cast<CMaterialGraphNode*>(OwningNode);
        void* NodeValue = ShouldDrawEditor() ? MaterialNode->GetNodeDefaultValue() : nullptr;
        if (NodeValue != nullptr)
        {
            switch (InputType)
            {
            // Each writes the value in place, so each must report the edit or nothing marks a recompile.
            case EMaterialInputType::Float:
                {
                    ImGui::SetNextItemWidth(kInlineFloatWidth);
                    if (ImGui::DragFloat("##Value", (float*)NodeValue + Index, 0.01f, 0.0f, 0.0f, "%.3g"))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = kInlineFloatWidth;
                }
                break;
            case EMaterialInputType::Float2:
                {
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::DragFloat2("##Value", (float*)NodeValue, 0.01f))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 150.0f;
                }
                break;
            case EMaterialInputType::Float3:
                {
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::ColorEdit3("##Value", (float*)NodeValue))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 150.0f;
                }
                break;
            case EMaterialInputType::Float4:
                {
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::ColorEdit4("##Value", (float*)NodeValue))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 200.0f;
                }
                break;

            // Texture pins carry no inline editor; they keep the default pin width.
            default:
                break;
            }
        }
        else
        {
            ImGui::Dummy(ImVec2(1.5f, 1.5f));
        }
        return ReturnSize;
    }
}
