#include "AnimGraphNode_CachedPose.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Core/Object/Cast.h"

namespace Lumina
{
    void CAnimGraphNode_SaveCachedPose::BuildNode()
    {
        PoseInputPin = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
    }

    void CAnimGraphNode_SaveCachedPose::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        // Publishes the register itself, so a reader aliases this evaluation instead of repeating it.
        Compiler.SetCachedPose(CacheName, ResolvePoseInput(PoseInputPin, Compiler));
    }

    FString CAnimGraphNode_SaveCachedPose::GetNodeTitleText() const
    {
        return CacheName.IsNone() ? FString("Save Cached Pose") : Format("Save {}", CacheName.c_str());
    }

    void CAnimGraphNode_UseCachedPose::BuildNode()
    {
        PosePin = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    CEdGraphNode* CAnimGraphNode_UseCachedPose::GetImplicitInputNode() const
    {
        CEdNodeGraph* Graph = GetOwningGraph();
        if (Graph == nullptr || CacheName.IsNone())
        {
            return nullptr;
        }

        for (CEdGraphNode* Node : Graph->Nodes)
        {
            CAnimGraphNode_SaveCachedPose* Save = Cast<CAnimGraphNode_SaveCachedPose>(Node);
            if (Save != nullptr && Save->CacheName == CacheName)
            {
                return Save;
            }
        }

        return nullptr;
    }

    void CAnimGraphNode_UseCachedPose::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        uint16 CachedRegister = 0;
        if (!Compiler.TryGetCachedPose(CacheName, CachedRegister))
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Cached Pose";
            NodeError.Description = "Use Cached Pose names a cache no Save Cached Pose node writes; it will read the reference pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);

            Compiler.SetPinRegister(PosePin, Compiler.EmitRefPose());
            return;
        }

        Compiler.SetPinRegister(PosePin, CachedRegister);
    }

    FString CAnimGraphNode_UseCachedPose::GetNodeTitleText() const
    {
        return CacheName.IsNone() ? FString("Use Cached Pose") : FString(CacheName.c_str());
    }
}
