#include "MaterialInput.h"
#include "Nodes/MaterialGraphNode.h"

namespace Lumina
{
    float CMaterialInput::DrawPin()
    {
        float ReturnSize = 1.5f;
        if (ShouldDrawEditor())
        {
            CMaterialGraphNode* MaterialNode = static_cast<CMaterialGraphNode*>(OwningNode);
            void* NodeValue = MaterialNode->GetNodeDefaultValue();
            switch (InputType)
            {
            // Each writes the value in place, so each must report the edit or nothing marks a recompile.
            case EMaterialInputType::Float:
                {
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::DragFloat("##Value", (float*)NodeValue + Index, 0.01f))
                    {
                        MaterialNode->NotifyValueEdited();
                    }
                    ReturnSize = 100.0f;
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
