#include "MCPGraphTools.h"

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

namespace Lumina::MCP
{
    void CollectPins(CEdGraphNode* Node, TVector<SGraphPinInfo>& Out)
    {
        const auto Append = [&](const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins, const char* Direction)
        {
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (!Pin.IsValid())
                {
                    continue;
                }

                SGraphPinInfo Info;
                Info.Name      = Pin->GetPinName();
                Info.Direction = Direction;

                for (CEdNodeGraphPin* Other : Pin->GetConnections())
                {
                    if (Other != nullptr && Other->GetOwningNode() != nullptr)
                    {
                        Info.Connections.Add({ Other->GetOwningNode()->GetNodeID(), Other->GetPinName() });
                    }
                }

                Out.Add(Move(Info));
            }
        };

        Append(Node->GetInputPins(), "Input");
        Append(Node->GetOutputPins(), "Output");
    }

    bool RemoveGraphNode(CEdNodeGraph* Graph, int64 NodeId, FString& OutError)
    {
        CEdGraphNode* Node = NodeGraphOps::FindNode(Graph, NodeId);
        if (Node == nullptr)
        {
            OutError = Lumina::Format("No node {} is in this graph.", NodeId);
            return false;
        }

        return NodeGraphOps::RemoveNode(Graph, Node, OutError);
    }

    bool ConnectGraphPins(CEdNodeGraph* Graph, int64 FromNode, const FString& FromPin, int64 ToNode, const FString& ToPin, FString& OutError)
    {
        CEdGraphNode* From = NodeGraphOps::FindNode(Graph, FromNode);
        CEdGraphNode* To   = NodeGraphOps::FindNode(Graph, ToNode);

        if (From == nullptr || To == nullptr)
        {
            OutError = Lumina::Format("No node {} is in this graph.", From == nullptr ? FromNode : ToNode);
            return false;
        }

        CEdNodeGraphPin* OutputPin = NodeGraphOps::FindPin(From, FStringView(FromPin), ENodePinDirection::Output);
        if (OutputPin == nullptr)
        {
            OutError = Lumina::Format("Node {} has no output pin '{}'. It has {}.",
                FromNode, FromPin, NodeGraphOps::DescribePinNames(From, ENodePinDirection::Output));
            return false;
        }

        CEdNodeGraphPin* InputPin = NodeGraphOps::FindPin(To, FStringView(ToPin), ENodePinDirection::Input);
        if (InputPin == nullptr)
        {
            OutError = Lumina::Format("Node {} has no input pin '{}'. It has {}.",
                ToNode, ToPin, NodeGraphOps::DescribePinNames(To, ENodePinDirection::Input));
            return false;
        }

        return NodeGraphOps::ConnectPins(Graph, OutputPin, InputPin, OutError);
    }

    bool DisconnectGraphPin(CEdNodeGraph* Graph, int64 NodeId, const FString& PinName, FString& OutError)
    {
        CEdGraphNode* Node = NodeGraphOps::FindNode(Graph, NodeId);
        if (Node == nullptr)
        {
            OutError = Lumina::Format("No node {} is in this graph.", NodeId);
            return false;
        }

        CEdNodeGraphPin* Pin = NodeGraphOps::FindPin(Node, FStringView(PinName), ENodePinDirection::Input);
        if (Pin == nullptr)
        {
            Pin = NodeGraphOps::FindPin(Node, FStringView(PinName), ENodePinDirection::Output);
        }

        if (Pin == nullptr)
        {
            OutError = Lumina::Format("Node {} has no pin '{}'.", NodeId, PinName);
            return false;
        }

        return NodeGraphOps::DisconnectPin(Graph, Pin, OutError);
    }
}
