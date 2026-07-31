#include "MaterialOutput.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectArray.h"
#include "imgui-node-editor/imgui_node_editor_internal.h"
#include "Nodes/MaterialGraphNode.h"
#include "Renderer/RenderManager.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    float CMaterialOutput::DrawPin()
    {
        float ReturnSize = 1.0f;
        if (ShouldDrawEditor())
        {
            CMaterialGraphNode* MaterialNode = static_cast<CMaterialGraphNode*>(OwningNode);
            void* NodeValue = MaterialNode->GetNodeDefaultValue();
            switch (InputType)
            {
            // Every one of these writes the node's value in place, so each has to report the edit --
            // nothing else marks the graph as needing a recompile (CEdGraphNode::NotifyValueEdited).
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
                        ImGui::Image(ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()), ImVec2(164.0f, 164.0f));
                    }
                    
                    ReturnSize = 200.0f;
                }
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
