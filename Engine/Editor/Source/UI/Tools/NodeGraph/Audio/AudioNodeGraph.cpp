#include "EditorPCH.h"
#include "AudioNodeGraph.h"

#include "AudioGraphNode.h"
#include "Nodes/AudioNodes_Generators.h"
#include "AudioGraphSchema.h"
#include "Audio/Graph/AudioGraphOperator.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include <imgui_internal.h>

namespace Lumina
{
    void CAudioNodeGraph::Initialize()
    {
        Super::Initialize();

        EnsureRootNodes();
        ValidateGraph();
    }

    void CAudioNodeGraph::EnsureRootNodes()
    {
        if (FindOutputNode() != nullptr)
        {
            return;
        }

        CreateNode(CAudioGraphOutputNode::StaticClass());
    }

    CAudioGraphOutputNode* CAudioNodeGraph::FindOutputNode() const
    {
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid())
            {
                if (CAudioGraphOutputNode* Output = Cast<CAudioGraphOutputNode>(Node.Get()))
                {
                    return Output;
                }
            }
        }
        return nullptr;
    }

    const FEdGraphSchema& CAudioNodeGraph::GetSchema() const
    {
        return GetAudioGraphSchema();
    }

    bool CAudioNodeGraph::IsGraphRootNode(CEdGraphNode* Node) const
    {
        return Cast<CAudioGraphOutputNode>(Node) != nullptr;
    }

    void CAudioNodeGraph::DrawCanvasDropTarget()
    {
        // The node editor consumes the canvas and leaves no item, so the target covers the host rect.
        ImGuiWindow* Window = ImGui::GetCurrentWindow();
        if (Window == nullptr)
        {
            return;
        }

        if (ImGui::BeginDragDropTargetCustom(Window->Rect(), Window->ID))
        {
            if (CAudioStream* Dropped = DragDrop::AcceptAsset<CAudioStream>())
            {
                CAudioNode_WavePlayer* Node = Cast<CAudioNode_WavePlayer>(
                    CreateNode(CAudioNode_WavePlayer::StaticClass()));

                if (Node != nullptr)
                {
                    Node->Wave = Dropped;
                    Node->NotifyValueEdited();
                    QueueNodePlacement(Node, ImGui::GetMousePos());
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void CAudioNodeGraph::ValidateGraph()
    {
        Connections.clear();

        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }
}
