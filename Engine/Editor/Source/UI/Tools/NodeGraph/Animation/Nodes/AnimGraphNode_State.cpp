#include "AnimGraphNode_State.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"

namespace Lumina
{
    CClass* CAnimGraphNode_State::GetSupportedGraphClass() const
    {
        return CAnimStateMachineGraph::StaticClass();
    }

    CClass* CAnimGraphNode_StateAny::GetSupportedGraphClass() const
    {
        return CAnimStateMachineGraph::StaticClass();
    }

    void CAnimGraphNode_State::BuildNode()
    {
        // StateFlow pins: the In pin accepts many incoming transitions (and the
        // entry wire); the Out pin fans out to many outgoing transitions.
        InPin  = CreateAnimPin("In", ENodePinDirection::Input, EAnimPinType::StateFlow);
        InPin->bAllowMultipleConnections = true;

        OutPin = CreateAnimPin("Out", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }

    CAnimationGraphNodeGraph* CAnimGraphNode_State::GetOrCreateBlendTree()
    {
        if (!BlendTree.IsValid())
        {
            const FString GraphName = FString("StateBlendTree_") + eastl::to_string(GetNodeID());
            BlendTree = NewObject<CAnimationGraphNodeGraph>(GetPackage(), GraphName);
        }

        // Context-free: compiler evaluates states never opened; Initialize() deferred to editor tool.
        BlendTree->EnsureSetup();
        return BlendTree.Get();
    }

    CEdNodeGraph* CAnimGraphNode_State::GetEnterableSubGraph()
    {
        return GetOrCreateBlendTree();
    }

    FString CAnimGraphNode_State::GetStateLabel() const
    {
        return StateName.IsNone() ? FString("Unnamed State") : StateName.ToString();
    }

    void CAnimGraphNode_StateEntry::BuildNode()
    {
        OutPin = CreateAnimPin("Entry", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }

    void CAnimGraphNode_StateAny::BuildNode()
    {
        OutPin = CreateAnimPin("Any", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }
}
