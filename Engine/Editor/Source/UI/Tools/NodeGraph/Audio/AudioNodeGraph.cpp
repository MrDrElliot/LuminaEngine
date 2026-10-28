#include "EditorPCH.h"
#include "AudioNodeGraph.h"

#include "AudioGraphNode.h"
#include "Nodes/AudioNodes_Generators.h"
#include "AudioGraphSchema.h"
#include "Audio/Graph/AudioGraphInstance.h"
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
        if (FindOutputNode() == nullptr)
        {
            CreateNode(CAudioGraphOutputNode::StaticClass());
        }

        if (FindOnPlayNode() == nullptr)
        {
            // Typed before AddNode, because AddNode builds the pin from the type.
            CAudioGraphInputNode* OnPlay = NewObject<CAudioGraphInputNode>(GetNodeOuter());
            OnPlay->ParameterName = FName(kAudioGraphOnPlayInput);
            OnPlay->Type          = EAudioGraphType::Trigger;
            AddNode(OnPlay);
            OnPlay->SetGridPos(-400.0f, 0.0f);
        }
    }

    CAudioGraphInputNode* CAudioNodeGraph::FindOnPlayNode() const
    {
        const FName OnPlayName = FName(kAudioGraphOnPlayInput);
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            CAudioGraphInputNode* Input = Node.IsValid() ? Cast<CAudioGraphInputNode>(Node.Get()) : nullptr;
            if (Input != nullptr && Input->ParameterName == OnPlayName)
            {
                return Input;
            }
        }
        return nullptr;
    }

    CAudioGraphOutputNode* CAudioNodeGraph::FindOutputNode() const
    {
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
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
        // The canvas is a child window, so the target covers the host rect around it.
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
                    PlaceNodeAtScreen(Node, ImGui::GetMousePos());
                    WireDroppedWavePlayer(Node);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    namespace
    {
        CEdNodeGraphPin* FindPinNamed(const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins, const char* Name)
        {
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (Pin.IsValid() && Pin->GetPinName() == Name)
                {
                    return Pin.Get();
                }
            }
            return nullptr;
        }
    }

    void CAudioNodeGraph::WireDroppedWavePlayer(CAudioNode_WavePlayer* Player)
    {
        CAudioGraphInputNode* OnPlay = FindOnPlayNode();
        CEdNodeGraphPin* Play = FindPinNamed(Player->GetInputPins(), "Play");
        if (OnPlay != nullptr && OnPlay->Type == EAudioGraphType::Trigger && !OnPlay->GetOutputPins().empty() && Play != nullptr)
        {
            ConnectPins(OnPlay->GetOutputPins()[0].Get(), Play);
        }

        // Output inputs already in use stay as the designer wired them.
        CAudioGraphOutputNode* Output = FindOutputNode();
        if (Output == nullptr)
        {
            return;
        }

        for (const char* PinName : { "Out Left", "Out Right", "On Finished" })
        {
            CEdNodeGraphPin* Destination = FindPinNamed(Output->GetInputPins(), PinName);
            CEdNodeGraphPin* Source      = FindPinNamed(Player->GetOutputPins(), PinName);
            if (Destination != nullptr && Source != nullptr && !Destination->HasConnection())
            {
                ConnectPins(Source, Destination);
            }
        }
    }

    void CAudioNodeGraph::ValidateGraph()
    {
        Connections.clear();

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }
}
