#include "AnimGraphNode_StateRouting.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"

namespace Lumina
{
    CClass* CAnimGraphNode_StateAlias::GetSupportedGraphClass() const
    {
        return CAnimStateMachineGraph::StaticClass();
    }

    void CAnimGraphNode_StateAlias::BuildNode()
    {
        OutPin = CreateAnimPin("Alias", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }

    FString CAnimGraphNode_StateAlias::GetNodeTitleText() const
    {
        if (SourceStates.empty())
        {
            return FString("Alias (empty)");
        }

        if (SourceStates.size() == 1)
        {
            return FString(SourceStates[0].c_str());
        }

        return Format("{} +{}", SourceStates[0].c_str(), (int32)SourceStates.size() - 1);
    }

    CClass* CAnimGraphNode_StateConduit::GetSupportedGraphClass() const
    {
        return CAnimStateMachineGraph::StaticClass();
    }

    void CAnimGraphNode_StateConduit::BuildNode()
    {
        // Many states can route through one conduit, which is the point of sharing its rule.
        InPin  = CreateAnimPin("In", ENodePinDirection::Input, EAnimPinType::StateFlow);
        InPin->bAllowMultipleConnections = true;

        OutPin = CreateAnimPin("Out", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }

    FString CAnimGraphNode_StateConduit::GetNodeTitleText() const
    {
        return ConduitName.IsNone() ? FString("Conduit") : FString(ConduitName.c_str());
    }
}
