#include "AnimGraphNode_State.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "Containers/StringFormat.h"

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
        // The In pin accepts many incoming transitions, while Out fans out to many outgoing ones.
        InPin  = CreateAnimPin("In", ENodePinDirection::Input, EAnimPinType::StateFlow);
        InPin->bAllowMultipleConnections = true;

        OutPin = CreateAnimPin("Out", ENodePinDirection::Output, EAnimPinType::StateFlow);
    }

    FString CAnimGraphNode_State::GetNodeTitleText() const
    {
        return GetStateLabel();
    }

    bool CAnimGraphNode_State::GetRenameText(FString& OutText) const
    {
        OutText = StateName.IsNone() ? FString() : StateName.ToString();
        return true;
    }

    void CAnimGraphNode_State::SetRenameText(const FString& InText)
    {
        StateName = InText.empty() ? NAME_None : FName(InText.c_str());
    }

    CAnimationGraphNodeGraph* CAnimGraphNode_State::AllocateBlendTree()
    {
        const FString GraphName = FString("StateBlendTree_") + Format("{}", GetNodeID());
        BlendTree = NewObject<CAnimationGraphNodeGraph>(GetPackage(), GraphName);
        return BlendTree.Get();
    }

    CAnimationGraphNodeGraph* CAnimGraphNode_State::GetOrCreateBlendTree()
    {
        if (!BlendTree.IsValid())
        {
            AllocateBlendTree();
        }

        // Context-free, since the compiler evaluates states never opened and Initialize is deferred.
        BlendTree->EnsureSetup();
        return BlendTree.Get();
    }

    void CAnimGraphNode_State::PostCloneFrom(const CEdGraphNode* Source)
    {
        const CAnimGraphNode_State* SourceState = Cast<CAnimGraphNode_State>(Source);
        CAnimationGraphNodeGraph* SourceTree = SourceState != nullptr ? SourceState->BlendTree.Get() : nullptr;

        BlendTree = nullptr;
        if (SourceTree == nullptr)
        {
            return;
        }

        // Cloned before EnsureSetup so the copied Output node is not joined by a second one.
        AllocateBlendTree()->CloneContentFrom(SourceTree);
    }

    CEdNodeGraph* CAnimGraphNode_State::GetOwnedSubGraph() const
    {
        return BlendTree.Get();
    }

    void CAnimGraphNode_State::ReplaceSubGraphWithCopy()
    {
        CAnimationGraphNodeGraph* Shared = BlendTree.Get();
        if (Shared != nullptr)
        {
            AllocateBlendTree()->CloneContentFrom(Shared);
        }
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
