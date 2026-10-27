#include "MaterialInput.h"
#include "MaterialOutput.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectArray.h"
#include "MaterialCompiler.h"
#include "Nodes/MaterialGraphNode.h"
#include "Renderer/RenderManager.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    FString CMaterialOutput::GetPinTypeName() const
    {
        return FMaterialCompiler::GetHLSLTypeName(InputType);
    }

    EGraphPinShape CMaterialOutput::GetPinShape() const
    {
        return CMaterialInput::ShapeFor(InputType, Mask);
    }

    uint32 CMaterialOutput::GetPinColor() const
    {
        if (PinColor != IM_COL32_WHITE)
        {
            return PinColor;
        }

        switch (InputType)
        {
            case EMaterialInputType::Float:         return IM_COL32(150, 215, 140, 255);
            case EMaterialInputType::Float2:        return IM_COL32(110, 195, 235, 255);
            case EMaterialInputType::Float3:        return IM_COL32(240, 205, 105, 255);
            case EMaterialInputType::Float4:        return IM_COL32(225, 135, 205, 255);
            case EMaterialInputType::Texture:       return IM_COL32(185, 135, 250, 255);
            case EMaterialInputType::TextureHandle: return IM_COL32(235, 185, 70, 255);
        }
        return PinColor;
    }

    float CMaterialOutput::DrawPin()
    {
        float ReturnSize = 1.0f;
        if (ShouldDrawEditor())
        {
            CMaterialGraphNode* MaterialNode = static_cast<CMaterialGraphNode*>(OwningNode);
            void* NodeValue = MaterialNode->GetNodeDefaultValue();
            switch (InputType)
            {
            // Each writes the value in place, so each must report the edit or nothing marks a recompile.
            case EMaterialInputType::Float:
                {
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::DragFloat("##Value", (float*)NodeValue, 0.01f))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 60.0f;
                }
                break;
            case EMaterialInputType::Float2:
                {
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::DragFloat2("##Value", (float*)NodeValue, 0.01f))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 120.0f;
                }
                break;
            case EMaterialInputType::Float3:
                {
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::ColorEdit3("##Value", (float*)NodeValue))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 120.0f;
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
            case EMaterialInputType::Texture:
                {
                    FObjectHandle* TextureValue = static_cast<FObjectHandle*>(NodeValue);
                    CTexture* Texture = (CTexture*)TextureValue->Resolve();
                    
                    ImGui::SetNextItemWidth(200.0f);

                    if (Texture != nullptr && Texture->GetResourceID() >= 0)
                    {
                        ImGuiX::TextureImage(Texture, ImVec2(164.0f, 164.0f));
                    }
                    
                    ReturnSize = 200.0f;
                }
                break;

            // TextureHandle pins carry no inline editor; they keep the default pin width.
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
